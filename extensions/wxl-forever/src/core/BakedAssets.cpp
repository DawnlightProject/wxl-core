// wxl-forever: baked textures (5.tools/forever-bake) found through their manifests and streamed
// from the client's files within a memory budget.
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

#include "ExtensionApi.hpp"
#include "BakedAssets.hpp"
#include "Dds.hpp"

#include "game/Io.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace
{
    namespace io = wxl::game::io;
    namespace bk = wxl::forever::baked;

    constexpr const char* kRoot = "Textures\\Forever\\";
    constexpr const char* kLooseRoot = "Data\\Patch-4.MPQ\\";
    constexpr int kCreatesPerFrame = 2;

    /// A client file, through its file system first, then loose from the patch folder.
    bool ReadClientFile(const std::string& path, std::string& out)
    {
        void* handle = nullptr;
        if (io::FileOpen(path.c_str(), io::kOpenWholeFile, &handle) && handle)
        {
            uint32_t high = 0;
            const uint32_t size = io::FileSize(handle, &high);
            bool ok = false;
            if (size && !high)
            {
                out.resize(size);
                uint32_t got = 0;
                ok = io::FileRead(handle, out.data(), size, &got) != 0 && got == size;
            }
            io::FileClose(handle);
            if (ok) return true;
        }
        std::ifstream loose(kLooseRoot + path, std::ios::binary);
        if (!loose) return false;
        out.assign(std::istreambuf_iterator<char>(loose), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    std::string Backslashes(std::string s)
    {
        for (char& c : s) if (c == '/') c = '\\';
        return s;
    }

    std::string Lower(std::string s)
    {
        for (char& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        return s;
    }

    struct Asset
    {
        std::string path;         // client path, "Textures\Forever\..."
        uint32_t    want = bk::kWantTexture;
        int         priority = 0;
        bk::State   state = bk::State::None;
        std::string bytes;        // read by the worker; kept for kWantBytes, dropped after creation otherwise
        IDirect3DBaseTexture9* texture = nullptr;
        size_t      size = 0;     // bytes counted against the budget
        uint32_t    touched = 0;  // frame of the last Request/Texture/Bytes
        bool        held = true;  // a consumer still wants it
        bool        readDone = false;
    };

    std::vector<Asset>                         g_assets;      // index = id - 1
    std::unordered_map<std::string, bk::AssetId> g_byPath;
    std::mutex                                 g_mutex;
    std::condition_variable                    g_wake;
    std::deque<bk::AssetId>                    g_queue;       // ids to read, ordered by priority at insert
    std::thread                                g_worker;
    std::atomic<bool>                          g_stop{ false };
    bool                                       g_async = true;
    size_t                                     g_budget = 64u << 20;
    uint32_t                                   g_frame = 0;
    char                                       g_status[160] = "baked: nothing requested";
    bk::Stats                                  g_stats{};
    bk::Stats                                  g_logged{};
    ULONGLONG                                  g_lastLog = 0;

    IDirect3DTexture9*       g_white2D = nullptr;
    IDirect3DCubeTexture9*   g_whiteCube = nullptr;
    IDirect3DVolumeTexture9* g_whiteVolume = nullptr;

    void Worker()
    {
        for (;;)
        {
            bk::AssetId id = 0;
            std::string path;
            {
                std::unique_lock<std::mutex> lock(g_mutex);
                g_wake.wait(lock, [] { return g_stop.load() || !g_queue.empty(); });
                if (g_stop.load()) return;
                id = g_queue.front();
                g_queue.pop_front();
                path = g_assets[id - 1].path;
            }
            std::string bytes;
            const bool ok = ReadClientFile(path, bytes);
            std::lock_guard<std::mutex> lock(g_mutex);
            Asset& a = g_assets[id - 1];
            a.bytes = ok ? std::move(bytes) : std::string();
            a.readDone = true;
            if (!ok) a.state = bk::State::Failed;
        }
    }

    void StartWorker()
    {
        static bool started = false;
        if (started) return;
        started = true;
        g_async = wxl_forever::ConfigBool("WXL_FOREVER_BAKED_ASYNC", true);
        g_budget = size_t(wxl_forever::ConfigFloat("WXL_FOREVER_BAKED_BUDGET_MB", 64.0f, 4.0f, 1024.0f) * 1048576.0f);
        if (g_async) g_worker = std::thread(Worker);
        WLOG_INFO("baked: streaming %s, budget %zu MB", g_async ? "on a worker thread" : "on the render thread",
                  g_budget >> 20);
    }

    void Enqueue(bk::AssetId id)
    {
        // Higher priority first; equal priority keeps arrival order.
        const int p = g_assets[id - 1].priority;
        auto it = g_queue.begin();
        while (it != g_queue.end() && g_assets[*it - 1].priority >= p) ++it;
        g_queue.insert(it, id);
    }

    void UpdateStatus()
    {
        std::snprintf(g_status, sizeof g_status, "baked: %u resident (%.1f MB of %zu), %u pending, %u failed",
                      g_stats.resident, double(g_stats.bytes) / 1048576.0, g_budget >> 20, g_stats.pending, g_stats.failed);
        const ULONGLONG now = GetTickCount64();
        if ((g_stats.resident != g_logged.resident || g_stats.bytes != g_logged.bytes || g_stats.failed != g_logged.failed)
            && now - g_lastLog >= 1000)
        {
            g_lastLog = now;
            g_logged = g_stats;
            WLOG_INFO("%s", g_status);
        }
    }

    void Drop(Asset& a)
    {
        if (a.texture) { a.texture->Release(); a.texture = nullptr; }
        a.bytes.clear();
        a.bytes.shrink_to_fit();
        a.size = 0;
        a.readDone = false;
        a.state = bk::State::Evicted;
    }

    /// Creates the texture of a finished read, or keeps its bytes; false when it failed.
    void Finish(IDirect3DDevice9* dev, Asset& a)
    {
        if (a.state == bk::State::Failed || a.bytes.empty())
        {
            a.state = bk::State::Failed;
            a.bytes.clear();
            WLOG_WARN("baked: %s could not be read; the neutral texture stands in", a.path.c_str());
            return;
        }
        if (a.want == bk::kWantBytes)
        {
            a.size = a.bytes.size();
            a.state = bk::State::Ready;
            return;
        }
        wxl::forever::dds::Info info{};
        if (!wxl::forever::dds::Parse(a.bytes, info))
        {
            a.state = bk::State::Failed;
            a.bytes.clear();
            WLOG_WARN("baked: %s is not a DDS file this loader takes", a.path.c_str());
            return;
        }
        a.texture = wxl::forever::dds::Create(dev, a.bytes, info, D3DPOOL_MANAGED, a.path.c_str());
        a.size = info.dataSize;
        a.bytes.clear();
        a.bytes.shrink_to_fit();
        a.state = a.texture ? bk::State::Ready : bk::State::Failed;
    }

    void Evict()
    {
        // Over budget: drop the least recently touched ready assets nobody holds, then held ones.
        size_t total = 0;
        for (const Asset& a : g_assets) total += a.size;
        for (int pass = 0; pass < 2 && total > g_budget; ++pass)
        {
            std::vector<Asset*> ready;
            for (Asset& a : g_assets)
                if (a.state == bk::State::Ready && (pass == 1 || !a.held) && a.touched != g_frame) ready.push_back(&a);
            std::sort(ready.begin(), ready.end(), [](const Asset* x, const Asset* y) { return x->touched < y->touched; });
            for (Asset* a : ready)
            {
                if (total <= g_budget) break;
                total -= a->size;
                Drop(*a);
            }
        }
    }
}

namespace wxl::forever::baked
{
    // --- Manifest ---------------------------------------------------------------------------------

    bool Manifest::Load(const char* type, const char* file)
    {
        type_ = type;
        folder_ = std::string(kRoot) + type + "\\";
        columns_.clear();
        rows_.clear();
        index_.clear();
        version_ = 0;
        std::string text;
        if (!ReadClientFile(folder_ + file, text))
        {
            WLOG_WARN("baked: %s%s missing; %s data stays empty", folder_.c_str(), file, type);
            return false;
        }
        size_t at = 0;
        bool header = false;
        while (at < text.size())
        {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(at, end - at);
            at = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (line[0] == '#')
            {
                const size_t f = line.find("format ");
                if (f != std::string::npos) version_ = std::atoi(line.c_str() + f + 7);
                continue;
            }
            std::vector<std::string> fields;
            size_t s = 0;
            for (;;)
            {
                const size_t c = line.find(',', s);
                fields.push_back(line.substr(s, c == std::string::npos ? std::string::npos : c - s));
                if (c == std::string::npos) break;
                s = c + 1;
            }
            if (!header)
            {
                columns_ = std::move(fields);
                header = true;
                continue;
            }
            if (fields.empty() || fields[0].empty()) continue;
            index_.emplace_back(Lower(fields[0]), int(rows_.size()));
            rows_.push_back(std::move(fields));
        }
        std::sort(index_.begin(), index_.end());
        WLOG_INFO("baked: %s manifest format %d, %zu rows, %zu columns", type, version_, rows_.size(), columns_.size());
        return true;
    }

    int Manifest::Column(const char* name) const
    {
        for (size_t i = 0; i < columns_.size(); ++i)
            if (columns_[i] == name) return int(i);
        return -1;
    }

    int Manifest::Find(const char* key) const
    {
        const std::string k = Lower(key);
        auto it = std::lower_bound(index_.begin(), index_.end(), std::make_pair(k, -1));
        return it != index_.end() && it->first == k ? it->second : -1;
    }

    const char* Manifest::Field(int row, int column) const
    {
        if (row < 0 || size_t(row) >= rows_.size() || column < 0 || size_t(column) >= rows_[row].size()) return "";
        return rows_[row][column].c_str();
    }

    float Manifest::Number(int row, int column, float fallback) const
    {
        const char* f = Field(row, column);
        char* end = nullptr;
        const float v = std::strtof(f, &end);
        return end == f ? fallback : v;
    }

    // --- streaming ---------------------------------------------------------------------------------

    AssetId Request(const std::string& rel, int priority, uint32_t want)
    {
        StartWorker();
        std::string path = Backslashes(rel);
        if (_strnicmp(path.c_str(), kRoot, std::strlen(kRoot)) != 0) path = kRoot + path;
        const std::string key = Lower(path);
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_byPath.find(key);
        AssetId id;
        if (it == g_byPath.end())
        {
            Asset a;
            a.path = path;
            a.want = want;
            g_assets.push_back(std::move(a));
            id = AssetId(g_assets.size());
            g_byPath.emplace(key, id);
        }
        else id = it->second;
        Asset& a = g_assets[id - 1];
        a.held = true;
        a.touched = g_frame;
        a.priority = priority;
        if (a.state == State::None || a.state == State::Evicted)
        {
            a.state = State::Pending;
            a.readDone = false;
            if (g_async)
            {
                Enqueue(id);
                g_wake.notify_one();
            }
            else
            {
                std::string bytes;
                const bool ok = ReadClientFile(a.path, bytes);
                a.bytes = ok ? std::move(bytes) : std::string();
                a.readDone = true;
                if (!ok) a.state = State::Failed;
            }
        }
        return id;
    }

    IDirect3DBaseTexture9* Texture(AssetId id)
    {
        if (!id || id > g_assets.size()) return nullptr;
        std::lock_guard<std::mutex> lock(g_mutex);
        Asset& a = g_assets[id - 1];
        a.touched = g_frame;
        return a.state == State::Ready ? a.texture : nullptr;
    }

    const std::string* Bytes(AssetId id)
    {
        if (!id || id > g_assets.size()) return nullptr;
        std::lock_guard<std::mutex> lock(g_mutex);
        Asset& a = g_assets[id - 1];
        a.touched = g_frame;
        return a.state == State::Ready && a.want == kWantBytes ? &a.bytes : nullptr;
    }

    State StateOf(AssetId id)
    {
        if (!id || id > g_assets.size()) return State::None;
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_assets[id - 1].state;
    }

    void Release(AssetId id)
    {
        if (!id || id > g_assets.size()) return;
        std::lock_guard<std::mutex> lock(g_mutex);
        g_assets[id - 1].held = false;
    }

    void Pump(IDirect3DDevice9* dev, uint32_t frame)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        // Once per frame, whichever consumer asks first.
        static uint32_t pumped = ~0u;
        if (frame == pumped) return;
        pumped = frame;
        g_frame = frame;
        int created = 0;
        for (Asset& a : g_assets)
        {
            if (a.state != State::Pending && !(a.state == State::Failed && a.readDone)) continue;
            if (!a.readDone) continue;
            if (created >= kCreatesPerFrame && a.state == State::Pending && a.want == kWantTexture) break;
            a.readDone = false;
            Finish(dev, a);
            ++created;
        }
        Evict();
        g_stats = Stats{};
        for (const Asset& a : g_assets)
        {
            if (a.state == State::Ready) { ++g_stats.resident; g_stats.bytes += a.size; }
            else if (a.state == State::Pending) ++g_stats.pending;
            else if (a.state == State::Failed) ++g_stats.failed;
        }
        UpdateStatus();
    }

    void SetBudget(size_t bytes) { g_budget = std::max(bytes, size_t(1) << 20); }
    size_t Budget() { return g_budget; }
    Stats GetStats() { return g_stats; }
    const char* Status() { return g_status; }

    IDirect3DTexture9* White2D(IDirect3DDevice9* dev)
    {
        if (!g_white2D && dev && SUCCEEDED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &g_white2D, nullptr)))
        {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(g_white2D->LockRect(0, &lr, nullptr, 0))) { *static_cast<uint8_t*>(lr.pBits) = 255; g_white2D->UnlockRect(0); }
        }
        return g_white2D;
    }

    IDirect3DCubeTexture9* WhiteCube(IDirect3DDevice9* dev)
    {
        if (!g_whiteCube && dev && SUCCEEDED(dev->CreateCubeTexture(1, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &g_whiteCube, nullptr)))
            for (int f = 0; f < 6; ++f)
            {
                D3DLOCKED_RECT lr{};
                if (SUCCEEDED(g_whiteCube->LockRect(D3DCUBEMAP_FACES(f), 0, &lr, nullptr, 0)))
                { *static_cast<uint8_t*>(lr.pBits) = 255; g_whiteCube->UnlockRect(D3DCUBEMAP_FACES(f), 0); }
            }
        return g_whiteCube;
    }

    IDirect3DVolumeTexture9* WhiteVolume(IDirect3DDevice9* dev)
    {
        if (!g_whiteVolume && dev && SUCCEEDED(dev->CreateVolumeTexture(1, 1, 1, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &g_whiteVolume, nullptr)))
        {
            D3DLOCKED_BOX box{};
            if (SUCCEEDED(g_whiteVolume->LockBox(0, &box, nullptr, 0))) { *static_cast<uint8_t*>(box.pBits) = 255; g_whiteVolume->UnlockBox(0); }
        }
        return g_whiteVolume;
    }

    void ReleaseAll()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (Asset& a : g_assets)
            if (a.state == State::Ready) Drop(a);
        if (g_white2D) { g_white2D->Release(); g_white2D = nullptr; }
        if (g_whiteCube) { g_whiteCube->Release(); g_whiteCube = nullptr; }
        if (g_whiteVolume) { g_whiteVolume->Release(); g_whiteVolume = nullptr; }
    }

    void Shutdown()
    {
        g_stop.store(true);
        g_wake.notify_all();
        if (g_worker.joinable()) g_worker.join();
    }

    // --- per-tile helpers ------------------------------------------------------------------------

    void TileOf(const float world[3], int& cx, int& cy)
    {
        cx = int(std::floor(32.0f - world[1] / kTileSize));
        cy = int(std::floor(32.0f - world[0] / kTileSize));
    }

    std::string TileKey(const char* map, int cx, int cy)
    {
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s_%d_%d", map, cx, cy);
        return buf;
    }

    int TilesAround(const float eye[3], int ring, int* cxOut, int* cyOut, int cap)
    {
        int cx, cy;
        TileOf(eye, cx, cy);
        struct T { int dx, dy; };
        std::vector<T> tiles;
        for (int dy = -ring; dy <= ring; ++dy)
            for (int dx = -ring; dx <= ring; ++dx) tiles.push_back({ dx, dy });
        // Nearest first by the distance from the eye to each tile's centre.
        const float fx = 32.0f - eye[0] / kTileSize - float(cy) - 0.5f;   // eye offset from the tile centre, tile units
        const float fy = 32.0f - eye[1] / kTileSize - float(cx) - 0.5f;
        std::sort(tiles.begin(), tiles.end(), [&](const T& a, const T& b) {
            const float da = (a.dy - fx) * (a.dy - fx) + (a.dx - fy) * (a.dx - fy);
            const float db = (b.dy - fx) * (b.dy - fx) + (b.dx - fy) * (b.dx - fy);
            return da < db;
        });
        int n = 0;
        for (const T& t : tiles)
        {
            if (n >= cap) break;
            const int x = cx + t.dx, y = cy + t.dy;
            if (x < 0 || y < 0 || x > 63 || y > 63) continue;
            cxOut[n] = x;
            cyOut[n] = y;
            ++n;
        }
        return n;
    }
}
