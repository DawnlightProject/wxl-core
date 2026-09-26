// wxl-graphics-extend: baked assets -- manifests and files streamed from the client within a memory budget.
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

#include "Assets.hpp"
#include "ManifestCsv.hpp"
#include "../core/Extension.hpp"
#include "../io/ClientFile.hpp"
#include "../textures/Dds.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <new>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// The opaque handle of GraphicsExtendApi.h: immutable once built, alive for the process, so the
/// accessors need no lock and the pointers ManifestField hands out stay valid.
struct WXL_GfxManifest
{
    std::string folder;                    // "Textures\Forever\Cookies\" (or "" for the client root)
    wxl::gfx::assets::csv::Table table;
};

namespace
{
    namespace csv = wxl::gfx::assets::csv;
    namespace dds = wxl::gfx::dds;
    namespace io  = wxl::gfx::io;

    // Per-frame work on the render thread: texture creation stops after this many, or once the
    // source bytes uploaded reach kCreateBytesPerFrame (a single larger file still goes); reads
    // through the client's own file system, which only the render thread may use, stop after two.
    constexpr int      kCreatesPerFrame     = 4;
    constexpr size_t   kCreateBytesPerFrame = size_t(8) << 20;
    constexpr int      kRenderReadsPerFrame = 2;
    constexpr uint32_t kLogIntervalMs       = 3000;
    constexpr uint32_t kWants               = WXL_GFX_ASSET_TEXTURE_GPU + 1;
    constexpr uint32_t kNone                = 0;   // list terminator (ids start at 1)

    /// Where an asset's read is; the WXL_GFX_ASSET_* state is what the consumer sees.
    enum Phase : uint8_t
    {
        kIdle,      // nothing in flight: None, Ready, Failed or Evicted
        kQueued,    // in the worker's queue
        kReading,   // a thread is reading it, with the mutex released
        kRead,      // bytes arrived: on the finished list, Pump turns them into the asset
        kRetry,     // the worker found nothing: on the retry list, Pump reads it on the render thread
    };

    struct Asset
    {
        std::string            path;          // client path, backslashes, as first requested (case kept)
        uint32_t               want = WXL_GFX_ASSET_TEXTURE;
        int                    priority = 0;
        uint32_t               state = WXL_GFX_ASSET_NONE;
        Phase                  phase = kIdle;
        bool                   held = true;   // a consumer still wants it
        uint32_t               queueSerial = 0;   // matches the queue entry that is current for it
        uint32_t               next = kNone;      // the finished or retry list
        uint32_t               touched = 0;       // the Pump count when last requested or read
        std::string            bytes;         // read by a thread; kept for BYTES, dropped once a texture exists
        IDirect3DBaseTexture9* texture = nullptr;
        size_t                 size = 0;      // bytes counted against the budget
    };

    /// Higher priority first; equal priority in request order. A re-prioritised request pushes a
    /// fresh entry, and the stale one is skipped when it surfaces (its serial no longer matches).
    struct QueueEntry
    {
        int      priority;
        uint64_t sequence;
        uint32_t id;
        uint32_t serial;
    };

    struct QueueOrder
    {
        bool operator()(const QueueEntry& a, const QueueEntry& b) const noexcept
        {
            if (a.priority != b.priority) return a.priority < b.priority;
            return a.sequence > b.sequence;
        }
    };

    /// An intrusive FIFO of asset ids: linking never allocates, so the worker cannot fail to hand a
    /// read back.
    struct IdList
    {
        uint32_t head = kNone, tail = kNone;
    };

    struct Stats
    {
        uint32_t resident = 0, pending = 0, failed = 0;
        size_t   bytes = 0;

        bool operator!=(const Stats& o) const noexcept
        {
            return resident != o.resident || pending != o.pending || failed != o.failed || bytes != o.bytes;
        }
    };

    /// Everything the service holds. One mutex guards all of it; the worker waits on the condition
    /// variable. Heap-allocated and never freed: the worker is detached (the process ends with the
    /// client), and a static destructor tearing the mutex down under it would be a race for nothing.
    struct Service
    {
        std::mutex              mutex;
        std::condition_variable wake;

        // Assets are addressed by id - 1 and never erased; a deque keeps every reference stable when
        // one is appended, so the worker may read a path, and a consumer its bytes, without a copy.
        std::deque<Asset>                          assets;
        std::unordered_map<std::string, uint32_t>  byKey;   // lower-cased path + want
        std::priority_queue<QueueEntry, std::vector<QueueEntry>, QueueOrder> queue;
        uint64_t                                   sequence = 0;
        IdList                                     finished, retry;

        std::vector<std::unique_ptr<WXL_GfxManifest>>              manifests;
        std::unordered_map<std::string, const WXL_GfxManifest*>    manifestByKey;   // null: known absent

        bool     async = true;
        bool     workerStarted = false;   // the first request starts it (or decides not to)
        bool     workerRunning = false;
        size_t   budget = size_t(64) << 20;

        IDirect3DDevice9* device = nullptr;   // the device every texture belongs to
        uint32_t          pumped = ~0u;       // the frame Pump last ran for
        uint32_t          pumps = 0;          // Pumps so far; a touch records this (the frame in progress)
        bool              dirty = false;      // residency may have changed since the last accounting

        Stats     stats, logged;
        bool      logPending = false;
        ULONGLONG lastLog = 0;
        char      status[192] = "assets: nothing requested";
    };

    Service& g = *new Service;

    // --- helpers ------------------------------------------------------------------------------

    std::string Normalised(const char* path)
    {
        std::string s(path);
        for (char& c : s) if (c == '/') c = '\\';
        return s;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return s;
    }

    Asset* Find(uint32_t id) noexcept
    {
        return id && id <= g.assets.size() ? &g.assets[id - 1] : nullptr;
    }

    void Push(IdList& list, uint32_t id) noexcept
    {
        Asset& a = g.assets[id - 1];
        a.next = kNone;
        if (list.tail) g.assets[list.tail - 1].next = id;
        else list.head = id;
        list.tail = id;
    }

    uint32_t Pop(IdList& list) noexcept
    {
        const uint32_t id = list.head;
        if (!id) return kNone;
        Asset& a = g.assets[id - 1];
        list.head = a.next;
        if (!list.head) list.tail = kNone;
        a.next = kNone;
        return id;
    }

    void DropBytes(Asset& a) noexcept
    {
        a.bytes.clear();
        a.bytes.shrink_to_fit();
    }

    /// Back to Evicted: the next Request reads it again.
    void Drop(Asset& a) noexcept
    {
        if (a.texture) { a.texture->Release(); a.texture = nullptr; }
        DropBytes(a);
        a.size = 0;
        a.phase = kIdle;
        a.state = WXL_GFX_ASSET_EVICTED;
    }

    // --- the worker -----------------------------------------------------------------------------

    /// One queue entry: the read runs with the mutex released. Nothing here allocates after the read,
    /// so a result is always handed back. `current` names the asset being read for the caller's
    /// exception handler.
    void WorkerStep(std::unique_lock<std::mutex>& lock, uint32_t& current)
    {
        g.wake.wait(lock, [] { return !g.queue.empty(); });
        const QueueEntry e = g.queue.top();
        g.queue.pop();
        Asset& a = g.assets[e.id - 1];
        if (a.phase != kQueued || a.queueSerial != e.serial) return;   // superseded or stale
        a.phase = kReading;
        current = e.id;

        std::string bytes;
        bool ok = false;
        lock.unlock();
        try
        {
            // Off the render thread this reads the loose sources only: a file that lives in the
            // client's archives comes back "not found" and is retried on the render thread.
            ok = io::Read(a.path.c_str(), bytes);
        }
        catch (...)
        {
            ok = false;
        }
        lock.lock();
        current = kNone;
        if (a.phase != kReading) return;
        if (ok)
        {
            a.bytes = std::move(bytes);
            a.phase = kRead;
            Push(g.finished, e.id);
        }
        else
        {
            a.phase = kRetry;
            Push(g.retry, e.id);
        }
    }

    void Worker() noexcept
    {
        try
        {
            std::unique_lock<std::mutex> lock(g.mutex);
            uint32_t current = kNone;
            for (;;)
            {
                try
                {
                    WorkerStep(lock, current);
                }
                catch (...)
                {
                    // Out of memory (or the mutex refused): whatever was being read is lost, and the
                    // consumer sees it Failed rather than Pending forever.
                    if (!lock.owns_lock()) lock.lock();
                    if (Asset* a = Find(current); a && a->phase == kReading)
                    {
                        a->phase = kIdle;
                        a->state = WXL_GFX_ASSET_FAILED;
                        g.dirty = true;
                    }
                    current = kNone;
                }
            }
        }
        catch (...)
        {
            // The mutex itself failed: the worker ends, and the render thread reads from now on.
            g.workerRunning = false;
        }
    }

    /// The first request decides how reads run. Detached: the process ends with the client.
    void StartWorker()
    {
        if (g.workerStarted) return;
        g.workerStarted = true;
        if (g.async)
        {
            try
            {
                std::thread(&Worker).detach();
                g.workerRunning = true;
            }
            catch (...)
            {
                GFX_LOG_WARN("assets: no worker thread could be started; reading on the render thread");
            }
        }
        GFX_LOG_INFO("assets: streaming %s, budget %u MB", g.workerRunning ? "on a worker thread" : "on the render thread",
                     unsigned(g.budget >> 20));
    }

    /// Queues a read at the asset's priority, for the worker or, without one, for Pump. The asset
    /// changes only once the push succeeded: when it throws (out of memory) the asset is as it was,
    /// and the next Request tries again.
    void Enqueue(uint32_t id, Asset& a)
    {
        const uint32_t serial = a.queueSerial + 1;
        g.queue.push(QueueEntry{ a.priority, g.sequence + 1, id, serial });
        ++g.sequence;
        a.queueSerial = serial;
        a.phase = kQueued;
        if (g.workerRunning) g.wake.notify_one();
    }

    // --- the render thread ----------------------------------------------------------------------

    /// Turns finished bytes into the asset: kept as they are, or a device texture. Allocates nothing.
    void Finish(IDirect3DDevice9* dev, Asset& a) noexcept
    {
        if (a.bytes.empty())
        {
            a.state = WXL_GFX_ASSET_FAILED;
            GFX_LOG_WARN("assets: %s could not be read", a.path.c_str());
            return;
        }
        if (a.want == WXL_GFX_ASSET_BYTES)
        {
            a.size = a.bytes.size();
            a.state = WXL_GFX_ASSET_READY;
            return;
        }
        WXL_GfxDdsInfo info{};
        if (!dds::Parse(a.bytes.data(), a.bytes.size(), &info))
        {
            DropBytes(a);
            a.state = WXL_GFX_ASSET_FAILED;
            GFX_LOG_WARN("assets: %s is not a DDS file this reader takes", a.path.c_str());
            return;
        }
        const uint32_t pool = a.want == WXL_GFX_ASSET_TEXTURE_GPU ? uint32_t(D3DPOOL_DEFAULT) : uint32_t(D3DPOOL_MANAGED);
        a.texture = static_cast<IDirect3DBaseTexture9*>(dds::CreateTexture(dev, a.bytes.data(), a.bytes.size(), pool, a.path.c_str()));
        DropBytes(a);
        if (a.texture)
        {
            // The budget counts what the texture holds (every surface as stored), not the file.
            a.size = info.dataSize;
            a.state = WXL_GFX_ASSET_READY;
        }
        else if (dev->TestCooperativeLevel() != D3D_OK)
        {
            a.state = WXL_GFX_ASSET_EVICTED;   // refused by a lost device (the reason is logged): reloaded on the next Request
        }
        else
        {
            a.state = WXL_GFX_ASSET_FAILED;    // dds logged the reason; not retried
        }
    }

    /// The next asset the render thread reads: what the worker could not find first, then, when no
    /// worker runs (none was asked for, or none could be started), the queue itself in its priority
    /// order. kNone when there is nothing.
    uint32_t NextRenderRead()
    {
        while (g.retry.head)
        {
            const uint32_t id = Pop(g.retry);
            if (g.assets[id - 1].phase == kRetry) return id;
        }
        while (!g.workerRunning && !g.queue.empty())
        {
            const QueueEntry e = g.queue.top();
            g.queue.pop();
            const Asset& a = g.assets[e.id - 1];
            if (a.phase == kQueued && a.queueSerial == e.serial) return e.id;
        }
        return kNone;
    }

    /// Reads on the render thread, through the client's file system this time (its archives and
    /// mounted patch folders). At most kRenderReadsPerFrame: this is the one place the render thread
    /// waits on a file.
    void RenderReads(std::unique_lock<std::mutex>& lock)
    {
        for (int n = 0; n < kRenderReadsPerFrame; ++n)
        {
            const uint32_t id = NextRenderRead();
            if (!id) return;
            Asset& a = g.assets[id - 1];
            a.phase = kReading;
            std::string bytes;
            bool ok = false;
            lock.unlock();
            try
            {
                ok = io::Read(a.path.c_str(), bytes);
            }
            catch (...)
            {
                ok = false;
            }
            lock.lock();
            if (a.phase != kReading) continue;
            if (ok)
            {
                a.bytes = std::move(bytes);
                a.phase = kRead;
                Push(g.finished, id);
            }
            else
            {
                a.phase = kIdle;
                a.state = WXL_GFX_ASSET_FAILED;
                g.dirty = true;
                GFX_LOG_WARN("assets: %s not found", a.path.c_str());
            }
        }
    }

    /// Finished reads become textures (or resident bytes), a few per frame, in the order the reads
    /// completed: the worker pops by priority, so that order is the priority order. A lost device
    /// refuses every creation: the bytes wait for the reset instead of failing.
    void Creates(IDirect3DDevice9* dev)
    {
        if (!g.finished.head || dev->TestCooperativeLevel() != D3D_OK) return;
        int created = 0;
        size_t bytes = 0;
        while (g.finished.head && created < kCreatesPerFrame && bytes < kCreateBytesPerFrame)
        {
            const uint32_t id = Pop(g.finished);
            Asset& a = g.assets[id - 1];
            if (a.phase != kRead) continue;
            a.phase = kIdle;
            bytes += a.bytes.size();
            ++created;
            Finish(dev, a);
            g.dirty = true;
        }
    }

    /// Over budget: drop the least recently touched Ready assets nobody holds, then held ones. An
    /// asset touched since the last Pump is in use this frame and stays.
    void Evict()
    {
        size_t total = 0;
        for (const Asset& a : g.assets) total += a.size;
        if (total <= g.budget) return;
        std::vector<Asset*> ready;
        for (int pass = 0; pass < 2 && total > g.budget; ++pass)
        {
            ready.clear();
            for (Asset& a : g.assets)
                if (a.state == WXL_GFX_ASSET_READY && (pass == 1 || !a.held) && a.touched != g.pumps) ready.push_back(&a);
            std::sort(ready.begin(), ready.end(), [](const Asset* x, const Asset* y) { return x->touched < y->touched; });
            for (Asset* a : ready)
            {
                if (total <= g.budget) break;
                total -= a->size;
                Drop(*a);
            }
        }
    }

    void UpdateStatus() noexcept
    {
        Stats s;
        for (const Asset& a : g.assets)
        {
            if (a.state == WXL_GFX_ASSET_READY) { ++s.resident; s.bytes += a.size; }
            else if (a.state == WXL_GFX_ASSET_PENDING) ++s.pending;
            else if (a.state == WXL_GFX_ASSET_FAILED) ++s.failed;
        }
        g.stats = s;
        std::snprintf(g.status, sizeof g.status, "assets: %u resident (%.1f MB of %u), %u pending, %u failed", s.resident,
                      double(s.bytes) / 1048576.0, unsigned(g.budget >> 20), s.pending, s.failed);
        g.logPending = g.stats != g.logged;
    }

    /// One line whenever residency changed, at most every kLogIntervalMs: a burst of reads logs its
    /// start and its end, not every file.
    void LogResidency() noexcept
    {
        if (!g.logPending) return;
        const ULONGLONG now = GetTickCount64();
        if (now - g.lastLog < kLogIntervalMs) return;
        g.lastLog = now;
        g.logged = g.stats;
        g.logPending = false;
        GFX_LOG_INFO("%s", g.status);
    }

    /// Every texture goes: the device they were created on is not the one drawing any more.
    void ReleaseTextures(bool gpuOnly) noexcept
    {
        uint32_t n = 0;
        for (Asset& a : g.assets)
        {
            if (!a.texture || (gpuOnly && a.want != WXL_GFX_ASSET_TEXTURE_GPU)) continue;
            Drop(a);
            ++n;
        }
        if (!n) return;
        g.dirty = true;
        GFX_LOG_DEBUG("assets: %u %stextures released", n, gpuOnly ? "DEFAULT-pool " : "");
    }

    void PumpLocked(IDirect3DDevice9* dev, uint32_t frame, std::unique_lock<std::mutex>& lock)
    {
        if (frame == g.pumped) return;   // once per frame
        g.pumped = frame;
        if (dev != g.device)
        {
            // A new device object (not a reset): every texture belongs to the old one.
            if (g.device) ReleaseTextures(false);
            g.device = dev;
        }
        if (dev)
        {
            RenderReads(lock);
            Creates(dev);
        }
        if (g.dirty)
        {
            g.dirty = false;
            Evict();
            UpdateStatus();
        }
        LogResidency();
        ++g.pumps;
    }

    // --- manifests ------------------------------------------------------------------------------

    /// Reads and parses one manifest outside the lock (a rare, whole-file read on the calling
    /// thread), then registers it: whoever registered the key first wins.
    const WXL_GfxManifest* LoadManifest(const std::string& folder, const std::string& path, const std::string& key)
    {
        std::string text;
        const bool ok = io::Read(path.c_str(), text);
        std::unique_ptr<WXL_GfxManifest> m;
        if (ok)
        {
            m = std::make_unique<WXL_GfxManifest>();
            m->folder = folder;
            csv::Parse(text.data(), text.size(), m->table);
        }

        std::lock_guard<std::mutex> lock(g.mutex);
        auto [it, inserted] = g.manifestByKey.emplace(key, nullptr);
        if (!inserted) return it->second;
        if (!ok)
        {
            GFX_LOG_WARN("assets: manifest %s missing", path.c_str());
            return nullptr;
        }
        const WXL_GfxManifest* result = m.get();
        try
        {
            g.manifests.push_back(std::move(m));
        }
        catch (...)
        {
            g.manifestByKey.erase(it);   // not "known absent": the next call tries again
            throw;
        }
        it->second = result;
        GFX_LOG_INFO("assets: manifest %s format %d, %u rows, %u columns", path.c_str(), result->table.version,
                     unsigned(result->table.rows.size()), unsigned(result->table.columns.size()));
        return result;
    }
}

namespace wxl::gfx::assets
{
    void Install()
    {
        try
        {
            g.async = ConfigBool("WXL_GFX_ASSET_ASYNC", true);
            g.budget = size_t(ConfigInt("WXL_GFX_ASSET_BUDGET_MB", 64, 4, 1024)) << 20;
        }
        catch (...)
        {
            // The config reader could not allocate: the defaults stand.
        }
    }

    void Pump(void* device, uint32_t frame)
    {
        // Unguarded by Module.cpp's trampolines: nothing may leave (an eviction pass allocates).
        try
        {
            std::unique_lock<std::mutex> lock(g.mutex);
            PumpLocked(static_cast<IDirect3DDevice9*>(device), frame, lock);
        }
        catch (const std::bad_alloc&)
        {
            GFX_LOG_ERROR("assets: out of memory in Pump");
        }
        catch (...)
        {
            GFX_LOG_ERROR("assets: unexpected exception in Pump");
        }
    }

    void OnDeviceLost()
    {
        try
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            ReleaseTextures(true);
        }
        catch (...)
        {
        }
    }

    // --- manifests ------------------------------------------------------------------------------

    const WXL_GfxManifest* ManifestLoad(const char* folder, const char* file)
    {
        if (!folder) return nullptr;
        std::string dir = Normalised(folder);
        while (!dir.empty() && dir.back() == '\\') dir.pop_back();
        if (!dir.empty()) dir += '\\';
        const std::string path = dir + Normalised(file && *file ? file : "manifest.csv");
        const std::string key = Lower(path);
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            const auto it = g.manifestByKey.find(key);
            if (it != g.manifestByKey.end()) return it->second;
        }
        return LoadManifest(dir, path, key);
    }

    int ManifestVersion(const WXL_GfxManifest* m)
    {
        return m ? m->table.version : 0;
    }

    int ManifestCount(const WXL_GfxManifest* m)
    {
        return m ? int(m->table.rows.size()) : 0;
    }

    int ManifestColumn(const WXL_GfxManifest* m, const char* name)
    {
        return m ? m->table.Column(name) : -1;
    }

    int ManifestFind(const WXL_GfxManifest* m, const char* key)
    {
        return m ? m->table.Find(key) : -1;
    }

    const char* ManifestField(const WXL_GfxManifest* m, int row, int column)
    {
        return m ? m->table.Field(row, column) : "";
    }

    float ManifestNumber(const WXL_GfxManifest* m, int row, int column, float fallback)
    {
        return m ? m->table.Number(row, column, fallback) : fallback;
    }

    const char* ManifestFolder(const WXL_GfxManifest* m)
    {
        return m ? m->folder.c_str() : "";
    }

    // --- streaming ------------------------------------------------------------------------------

    uint32_t Request(const char* path, int priority, uint32_t want)
    {
        if (!path || !*path || want >= kWants) return 0;
        std::string norm = Normalised(path);
        std::string key = Lower(norm);
        key += '|';
        key += char('0' + want);

        std::lock_guard<std::mutex> lock(g.mutex);
        StartWorker();
        uint32_t id;
        const auto it = g.byKey.find(key);
        if (it != g.byKey.end()) id = it->second;
        else
        {
            if (g.assets.size() >= 0xFFFFFFFEu) return 0;
            g.assets.emplace_back();
            try
            {
                g.assets.back().path = std::move(norm);
                g.assets.back().want = want;
                id = uint32_t(g.assets.size());
                g.byKey.emplace(std::move(key), id);
            }
            catch (...)
            {
                g.assets.pop_back();
                throw;
            }
        }
        Asset& a = g.assets[id - 1];
        a.held = true;
        a.touched = g.pumps;
        if (a.state == WXL_GFX_ASSET_NONE || a.state == WXL_GFX_ASSET_EVICTED)
        {
            a.priority = priority;
            Enqueue(id, a);
            a.state = WXL_GFX_ASSET_PENDING;
        }
        else if (a.priority != priority)
        {
            a.priority = priority;
            // Still waiting for the worker: a fresh entry at the new priority supersedes the old one.
            if (a.phase == kQueued) Enqueue(id, a);
        }
        return id;
    }

    // The accessors below are not guarded by Module.cpp: they allocate nothing, and the one thing that
    // could throw, the lock, is caught so a caller across the C ABI never sees an exception.

    void* Texture(uint32_t id)
    {
        try
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            Asset* a = Find(id);
            if (!a) return nullptr;
            a->touched = g.pumps;
            return a->state == WXL_GFX_ASSET_READY ? a->texture : nullptr;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    const void* Bytes(uint32_t id, size_t* size)
    {
        if (size) *size = 0;
        try
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            Asset* a = Find(id);
            if (!a) return nullptr;
            a->touched = g.pumps;
            if (a->state != WXL_GFX_ASSET_READY || a->want != WXL_GFX_ASSET_BYTES) return nullptr;
            if (size) *size = a->bytes.size();
            return a->bytes.data();
        }
        catch (...)
        {
            return nullptr;
        }
    }

    uint32_t State(uint32_t id)
    {
        try
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            const Asset* a = Find(id);
            return a ? a->state : WXL_GFX_ASSET_NONE;
        }
        catch (...)
        {
            return WXL_GFX_ASSET_NONE;
        }
    }

    void Release(uint32_t id)
    {
        try
        {
            std::lock_guard<std::mutex> lock(g.mutex);
            if (Asset* a = Find(id)) a->held = false;
        }
        catch (...)
        {
        }
    }

    /// Written by Pump and read by the panel, both on the render thread.
    const char* Status()
    {
        return g.status;
    }
}
