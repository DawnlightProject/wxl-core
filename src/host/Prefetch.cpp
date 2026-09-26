// wxl-host: reads ahead of the client what it is likely to need next.
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Tile names and indices follow the client's streaming: CMap::PreUpdateAreas (0x007B5950) loads the tiles
// around the focus, and the per-tile loader (0x007D9A20) formats <folder>\<map>_<first>_<second>.adt, where
// first = (32 * 533.33 - y) / 533.33 and second = (32 * 533.33 - x) / 533.33 in world yards.

#include "host/Prefetch.hpp"

#include "host/Assets.hpp"
#include "common/Log.hpp"
#include "ipc/Profile.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

namespace wxl::hostd::prefetch
{
    namespace
    {
        constexpr float kTileYards = 533.33333f;
        constexpr float kOriginYards = 32.0f * kTileYards;
        constexpr size_t kQueueCap = 8192;
        constexpr size_t kSeenCap = 65536;
        constexpr ULONGLONG kSeenTtlMs = 180000;   // a file queued or read this recently is not queued again
        constexpr uint32_t kUrgent = 0;            // what a file the client just read references
        const float kLookaheadSeconds[] = { 0.0f, 2.0f, 5.0f, 10.0f };

        enum class Kind : uint8_t { Tile, Texture, Model, WmoRoot, File };

        struct Item
        {
            uint32_t    priority = 0;   // lower first
            uint64_t    seq = 0;
            std::string name;
            Kind        kind = Kind::File;

            bool operator<(const Item& o) const
            {
                return priority != o.priority ? priority < o.priority : seq < o.seq;
            }
        };

        std::mutex                                 g_mutex;
        std::condition_variable                    g_wake;
        std::set<Item>                             g_queue;
        std::unordered_map<std::string, ULONGLONG> g_seen;   // lowercase name -> when queued or read
        uint64_t                                   g_seq = 0;
        std::string                                g_mapFolder;

        std::atomic<bool>     g_run{ false };
        std::atomic<uint32_t> g_inFlight{ 0 };
        std::atomic<uint32_t> g_maxEdge{ 0 };
        HANDLE                g_thread = nullptr;
        uint32_t              g_slot = 0;
        uint32_t              g_workers = 1;
        bool                  g_enabled = false;
        ipc::HostCounters*    g_counters = nullptr;

        bool EndsWith(const std::string& s, const char* suffix)
        {
            const size_t n = std::strlen(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        }

        uint32_t U32(const uint8_t* p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        Kind KindOf(const std::string& lower)
        {
            if (EndsWith(lower, ".blp")) return Kind::Texture;
            if (EndsWith(lower, ".m2")) return Kind::Model;
            if (EndsWith(lower, ".wmo")) return Kind::WmoRoot;
            if (EndsWith(lower, ".adt")) return Kind::Tile;
            return Kind::File;
        }

        /// Forgets old entries of the seen map. Called with g_mutex held.
        void PruneSeen(ULONGLONG now)
        {
            for (auto it = g_seen.begin(); it != g_seen.end();)
                it = now - it->second > kSeenTtlMs ? g_seen.erase(it) : std::next(it);
            if (g_seen.size() > kSeenCap) g_seen.clear();
        }

        /// Queues a file unless it was queued or read recently. Called with g_mutex held.
        void QueueLocked(const std::string& name, uint32_t priority)
        {
            if (name.empty() || name.size() > 300) return;
            std::string key = cache::Normalize(name);
            const ULONGLONG now = GetTickCount64();
            const auto seen = g_seen.find(key);
            if (seen != g_seen.end() && now - seen->second < kSeenTtlMs) return;
            if (g_seen.size() >= kSeenCap) PruneSeen(now);
            const Kind kind = KindOf(key);
            g_seen[std::move(key)] = now;
            if (g_queue.size() >= kQueueCap)
            {
                auto worst = std::prev(g_queue.end());
                if (!(Item{ priority, g_seq + 1 } < *worst)) return;
                g_queue.erase(worst);
            }
            g_queue.insert(Item{ priority, ++g_seq, name, kind });
            if (g_counters) g_counters->prefetchQueued.fetch_add(1, std::memory_order_relaxed);
        }

        // --- what files reference --------------------------------------------------------------------

        /// Null-separated names of a chunk.
        void Names(const uint8_t* p, size_t n, std::vector<std::string>& out)
        {
            size_t i = 0;
            while (i < n)
            {
                const size_t start = i;
                while (i < n && p[i]) ++i;
                if (i > start) out.emplace_back(reinterpret_cast<const char*>(p + start), i - start);
                ++i;
            }
        }

        /// Placed models are named .mdx in terrain and map objects; the files are .m2.
        std::string ModelFile(std::string name)
        {
            const std::string lower = cache::Normalize(name);
            if (EndsWith(lower, ".mdx") || EndsWith(lower, ".mdl")) name = name.substr(0, name.size() - 4) + ".m2";
            return name;
        }

        /// Walks top-level chunks; tags are stored reversed ('MTEX' reads "XETM").
        template <class Fn>
        void Chunks(const uint8_t* p, size_t n, Fn&& fn)
        {
            size_t at = 0;
            while (at + 8 <= n)
            {
                const uint32_t size = U32(p + at + 4);
                if (size > n - at - 8) break;
                fn(p + at, p + at + 8, size);
                at += 8 + size_t(size);
            }
        }

        bool Tag(const uint8_t* at, const char* reversed)
        {
            return std::memcmp(at, reversed, 4) == 0;
        }

        void Cascade(const std::string& name, Kind kind, const cache::Blob& bytes, uint32_t priority)
        {
            if (!bytes || bytes->size() < 16) return;
            const uint8_t* p = bytes->data();
            const size_t n = bytes->size();
            std::vector<std::string> files;

            if (kind == Kind::Tile)
            {
                Chunks(p, n, [&](const uint8_t* tag, const uint8_t* data, uint32_t size) {
                    if (Tag(tag, "XETM") || Tag(tag, "OMWM")) Names(data, size, files);
                    else if (Tag(tag, "XDMM"))
                    {
                        std::vector<std::string> models;
                        Names(data, size, models);
                        for (std::string& m : models) files.push_back(ModelFile(m));
                    }
                });
            }
            else if (kind == Kind::Model)
            {
                if (n < 0x58 || std::memcmp(p, "MD20", 4) != 0) return;
                const uint32_t count = U32(p + 0x50), offset = U32(p + 0x54);
                for (uint32_t i = 0; i < count && i < 64 && uint64_t(offset) + (i + 1) * 16 <= n; ++i)
                {
                    const uint8_t* t = p + offset + i * 16;
                    const uint32_t type = U32(t), len = U32(t + 8), at = U32(t + 12);
                    if (type != 0 || len < 2 || len > 260 || uint64_t(at) + len > n) continue;
                    files.emplace_back(reinterpret_cast<const char*>(p + at), strnlen(reinterpret_cast<const char*>(p + at), len));
                }
                if (U32(p + 0x44) && EndsWith(cache::Normalize(name), ".m2"))
                    files.push_back(name.substr(0, name.size() - 3) + "00.skin");
            }
            else if (kind == Kind::WmoRoot)
            {
                uint32_t groups = 0;
                bool root = false;
                Chunks(p, n, [&](const uint8_t* tag, const uint8_t* data, uint32_t size) {
                    if (Tag(tag, "DHOM") && size >= 8) { root = true; groups = U32(data + 4); }
                    else if (Tag(tag, "XTOM")) Names(data, size, files);
                    else if (Tag(tag, "NDOM"))
                    {
                        std::vector<std::string> models;
                        Names(data, size, models);
                        for (std::string& m : models) files.push_back(ModelFile(m));
                    }
                });
                if (!root) return;   // a group file: its textures are the root's
                const std::string base = name.substr(0, name.size() - 4);
                char suffix[16];
                for (uint32_t g = 0; g < groups && g < 512; ++g)
                {
                    std::snprintf(suffix, sizeof suffix, "_%03u.wmo", g);
                    files.push_back(base + suffix);
                }
            }
            else
            {
                return;
            }

            if (files.empty()) return;
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const std::string& f : files) QueueLocked(f, priority);
            g_wake.notify_one();
        }

        // --- the thread ------------------------------------------------------------------------------

        DWORD WINAPI Run(LPVOID)
        {
            WXL_THREAD_NAME("wxl-host prefetch");
            // Lower CPU, I/O and memory priority: the client's own reads always go first.
            SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
            while (g_run.load())
            {
                Item item;
                {
                    std::unique_lock<std::mutex> lock(g_mutex);
                    g_wake.wait_for(lock, std::chrono::milliseconds(500), [] { return !g_run.load() || !g_queue.empty(); });
                    if (!g_run.load()) break;
                    if (g_queue.empty()) continue;
                    item = *g_queue.begin();
                    g_queue.erase(g_queue.begin());
                }
                while (g_run.load() && g_inFlight.load(std::memory_order_relaxed) >= g_workers) Sleep(1);

                bool read = false;
                const cache::Blob bytes = assets::Prefetch(g_slot, item.name, item.kind == Kind::Texture,
                                                           g_maxEdge.load(std::memory_order_relaxed), &read);
                if (read && g_counters)
                {
                    g_counters->prefetchDone.fetch_add(1, std::memory_order_relaxed);
                    g_counters->prefetchBytes.fetch_add(bytes ? bytes->size() : 0, std::memory_order_relaxed);
                }
                if (read && bytes) Cascade(item.name, item.kind, bytes, item.priority + 1);
            }
            return 0;
        }
    }

    void Init(uint32_t archiveSlot, uint32_t workers, ipc::HostCounters* counters, bool enabled)
    {
        g_slot = archiveSlot;
        g_workers = std::max(1u, workers);
        g_counters = counters;
        g_enabled = enabled && cache::Enabled();
    }

    void Start()
    {
        if (!g_enabled || g_thread) return;
        g_run.store(true);
        g_thread = CreateThread(nullptr, 0, &Run, nullptr, 0, nullptr);
        if (!g_thread)
        {
            g_run.store(false);
            WLOG_WARN("prefetch: could not start its thread (win32 %lu)", GetLastError());
        }
    }

    void Stop()
    {
        if (!g_thread) return;
        g_run.store(false);
        g_wake.notify_all();
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }

    void Hint(const char* mapFolder, float x, float y, float vx, float vy, float viewDistance)
    {
        if (!g_enabled || !mapFolder || !*mapFolder) return;
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(vx) || !std::isfinite(vy)) return;
        if (g_counters) g_counters->prefetchHints.fetch_add(1, std::memory_order_relaxed);

        const std::string folder(mapFolder);
        const size_t slash = folder.find_last_of('\\');
        const std::string map = slash == std::string::npos ? folder : folder.substr(slash + 1);
        const float radius = std::clamp(std::isfinite(viewDistance) ? viewDistance : 500.0f, 100.0f, 2000.0f) + kTileYards;
        const float speed = std::sqrt(vx * vx + vy * vy);

        std::lock_guard<std::mutex> lock(g_mutex);
        if (folder != g_mapFolder)
        {
            g_mapFolder = folder;
            g_queue.clear();   // queued for the map left behind
        }
        char tile[64];
        for (const float t : kLookaheadSeconds)
        {
            if (t > 0.0f && speed < 1.0f) break;
            const float px = x + vx * std::min(t * speed, 4000.0f) / std::max(speed, 1e-3f);
            const float py = y + vy * std::min(t * speed, 4000.0f) / std::max(speed, 1e-3f);
            const int f0 = std::max(0, int(std::floor((kOriginYards - (py + radius)) / kTileYards)));
            const int f1 = std::min(63, int(std::floor((kOriginYards - (py - radius)) / kTileYards)));
            const int s0 = std::max(0, int(std::floor((kOriginYards - (px + radius)) / kTileYards)));
            const int s1 = std::min(63, int(std::floor((kOriginYards - (px - radius)) / kTileYards)));
            for (int first = f0; first <= f1; ++first)
                for (int second = s0; second <= s1; ++second)
                {
                    const float cy = kOriginYards - (float(first) + 0.5f) * kTileYards;
                    const float cx = kOriginYards - (float(second) + 0.5f) * kTileYards;
                    const float d = std::sqrt((cx - px) * (cx - px) + (cy - py) * (cy - py));
                    if (d > radius + kTileYards * 0.71f) continue;
                    std::snprintf(tile, sizeof tile, "_%d_%d.adt", first, second);
                    QueueLocked(folder + "\\" + map + tile, uint32_t(t * 1000.0f) + uint32_t(d));
                }
        }
        g_wake.notify_one();
    }

    void Served(const char* name, const cache::Blob& bytes)
    {
        if (!g_enabled || !name) return;
        std::string key = cache::Normalize(name);
        const Kind kind = KindOf(key);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_seen[std::move(key)] = GetTickCount64();   // the client has it: no hint queues it again
        }
        if (kind == Kind::Tile || kind == Kind::Model || kind == Kind::WmoRoot) Cascade(name, kind, bytes, kUrgent);
    }

    void SetMaxEdge(uint32_t edge)
    {
        if (edge) g_maxEdge.store(edge, std::memory_order_relaxed);
    }

    void OnDemandBegin()
    {
        g_inFlight.fetch_add(1, std::memory_order_relaxed);
    }

    void OnDemandEnd()
    {
        g_inFlight.fetch_sub(1, std::memory_order_relaxed);
    }
}
