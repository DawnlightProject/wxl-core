// BLP textures served by wxl-host: the loader reads a header stand-in, its levels live in shared memory.
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

// Why the stand-in is exact: the loader (0x004B7BD0) copies the header with CBLPFile::Source and reaches a
// level only as buffer + offset (in place for DXT and ARGB, through CBLPFile::Lock2 for palettized files),
// and the completion (0x004B7E80) closes the file only after the upload. A texture image from the host is
// either the file itself, so the loader does exactly what it always did from other memory, or a decoded
// image (compression 3, ARGB8888 levels) that equals what the loader's own decode writes; verify mode
// (WXL_HOST_TEXTURES_VERIFY=1) runs the loader's decoder on the file and compares, level by level.

#include "engine/storage/HostTextures.hpp"

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "engine/storage/StorageHook.hpp"
#include "ipc/Ring.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Texture.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace wxl::runtime::storage::textures
{
    namespace
    {
        namespace h = wxl::host;
        namespace gx = wxl::offsets::engine::gx;
        namespace tex = wxl::offsets::engine::texture;
        using wxl::ipc::Status;

        constexpr uint32_t kOffsets = 0x14;      // BLP2 header: level offsets
        constexpr uint32_t kSizes = 0x54;        // level sizes
        constexpr uint32_t kMaxLevels = 16;
        constexpr uint32_t kSettleMs = 6000;
        constexpr uint32_t kThrottleMs = 500;    // longest wait for images to be released
        constexpr size_t kLatencySamples = 2048;

        bool Enabled()
        {
            static const bool on = wxl::config::Env("WXL_HOST_TEXTURES", true);
            return on;
        }

        bool Verify()
        {
            static const bool on = wxl::config::Env("WXL_HOST_TEXTURES_VERIFY", false);
            return on;
        }

        uint64_t InFlightCap()
        {
            static const uint64_t bytes = wxl::config::U64("WXL_HOST_TEXTURES_INFLIGHT_MB", 12, 2, 256) << 20;
            return bytes;
        }

        // --- counters --------------------------------------------------------------------------------

        struct Counters
        {
            std::atomic<uint64_t> served{ 0 }, decoded{ 0 }, imageBytes{ 0 };
            std::atomic<uint64_t> fallbacks{ 0 }, declined{ 0 };
            std::atomic<uint64_t> verified{ 0 }, mismatches{ 0 };
            std::atomic<uint64_t> latencyTotal{ 0 }, latencyCount{ 0 };
            std::atomic<uint64_t> heapSaved{ 0 }, heapInFlight{ 0 }, heapPeak{ 0 };
            std::atomic<uint64_t> imagesInFlight{ 0 }, throttled{ 0 };
            std::atomic<uint64_t> fromCache{ 0 }, prefetched{ 0 };
        } g_c;

        std::atomic<uint32_t> g_latency[kLatencySamples];
        std::atomic<uint32_t> g_latencyAt{ 0 };

        void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
        {
            uint64_t cur = a.load(std::memory_order_relaxed);
            while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
        }

        void RecordLatency(uint64_t us)
        {
            const uint32_t v = uint32_t(std::min<uint64_t>(us, 0xFFFFFFFFull));
            g_latency[g_latencyAt.fetch_add(1, std::memory_order_relaxed) % kLatencySamples].store(v, std::memory_order_relaxed);
            g_c.latencyTotal.fetch_add(us, std::memory_order_relaxed);
            g_c.latencyCount.fetch_add(1, std::memory_order_relaxed);
        }

        // --- the loader's entry ----------------------------------------------------------------------

        // Armed for the duration of a TextureCreate call, disarmed by the first .blp open it makes: that
        // open is CreateBlpTexture's, whose reader copies the whole file into one buffer (0x004B8A50).
        thread_local int t_armed = 0;
        thread_local int t_creating = 0;   // inside TextureCreate: this thread is the one that uploads

        gx::TextureCreateFn g_nextCreate = nullptr;

        void* __cdecl hkTextureCreate(const char* name, uint32_t flags, int* status, uint32_t flags2)
        {
            const int previous = t_armed;
            t_armed = 1;
            ++t_creating;
            void* handle = g_nextCreate(name, flags, status, flags2);
            --t_creating;
            t_armed = previous;
            return handle;
        }

        bool InstallFeature()
        {
            if (!Enabled())
            {
                WLOG_INFO("textures: WXL_HOST_TEXTURES=0; the client decodes its own textures");
                return true;
            }
            wxl::hook::Install("HostTextures.TextureCreate", gx::kTextureCreate, &hkTextureCreate, &g_nextCreate);
            WLOG_INFO("textures: BLP loads read from wxl-host (verify %s, in flight up to %llu MB)", Verify() ? "on" : "off",
                      static_cast<unsigned long long>(InFlightCap() >> 20));
            return true;
        }

        // --- helpers ---------------------------------------------------------------------------------

        uint32_t U32(const uint8_t* p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        bool EndsWithBlp(const char* name)
        {
            const size_t n = std::strlen(name);
            return n >= 4 && _stricmp(name + n - 4, ".blp") == 0;
        }

        uintptr_t Device()
        {
            return *reinterpret_cast<const uintptr_t*>(gx::kGxDevicePtr);
        }

        /// The largest texture edge the loader lets through without skipping levels (0 when no device yet).
        uint32_t MaxEdge()
        {
            const uintptr_t device = Device();
            return device ? *reinterpret_cast<const uint32_t*>(device + tex::kDeviceMaxTextureEdge) : 0;
        }

        bool IsBlp2(const uint8_t* p, uint32_t size)
        {
            return p && size >= kStubBytes && U32(p) == 0x32504C42 && U32(p + 4) == 1;
        }

        /// True while the main thread blocks on one read (it then uploads nothing until that read is done).
        bool MainThreadWaiting()
        {
            return *reinterpret_cast<const volatile uint32_t*>(tex::kAsyncReadWaiting) != 0;
        }

        /// Waits while too many texture images sit in the window, for the main thread to upload some.
        void Throttle(uint32_t incoming)
        {
            auto over = [incoming] { return g_c.imagesInFlight.load(std::memory_order_relaxed) + incoming > InFlightCap(); };
            // A read made inside TextureCreate (the client's synchronous create) is the uploader itself.
            if (!over() || t_creating || MainThreadWaiting()) return;
            g_c.throttled.fetch_add(1, std::memory_order_relaxed);
            const ULONGLONG until = GetTickCount64() + kThrottleMs;
            while (over() && !MainThreadWaiting() && GetTickCount64() < until) Sleep(1);
        }

        // --- verify mode -----------------------------------------------------------------------------

        /// Runs the loader's own palettized decode (CBLPFile::Source + LockChain2) and compares each level.
        bool SameAsLoader(const char* name, const uint8_t* file, uint32_t fileSize, const uint8_t* image, uint32_t imageSize)
        {
            if (!IsBlp2(file, fileSize) || !IsBlp2(image, imageSize)) return false;
            // Everything but the compression, the offsets and the sizes is the file's header.
            if (std::memcmp(file + 9, image + 9, kOffsets - 9) != 0 || std::memcmp(file + 0x94, image + 0x94, kStubBytes - 0x94) != 0)
            {
                WLOG_WARN("textures: verify: '%s' header differs from the file", name);
                return false;
            }
            const uint32_t width = U32(file + 0x0C), height = U32(file + 0x10);
            uint32_t levels = 0;
            uint64_t chain = 0;
            while (levels < kMaxLevels && U32(image + kSizes + levels * 4))
            {
                chain += U32(image + kSizes + levels * 4);
                ++levels;
            }
            std::vector<uint8_t> table(size_t(levels) * 4 + size_t(chain) + 64, 0);
            alignas(16) uint8_t blp[0x500] = {};
            reinterpret_cast<tex::BlpFileInitFn>(tex::kBlpFileInit)(blp);
            bool ok = reinterpret_cast<tex::BlpFileSourceFn>(tex::kBlpFileSource)(blp, file) != 0;
            void* chainTable = table.data();
            ok = ok && reinterpret_cast<tex::BlpFileLockChain2Fn>(tex::kBlpFileLockChain2)(
                           blp, name, tex::kPixelArgb8888, &chainTable, 0, 0) != 0;
            reinterpret_cast<tex::BlpFileCloseFn>(tex::kBlpFileClose)(blp);
            if (!ok)
            {
                WLOG_WARN("textures: verify: the loader refused '%s' (%ux%u) which the host decoded", name, width, height);
                return false;
            }
            const auto* levelPtrs = reinterpret_cast<const uint8_t* const*>(table.data());
            for (uint32_t i = 0; i < levels; ++i)
            {
                const uint32_t offset = U32(image + kOffsets + i * 4), bytes = U32(image + kSizes + i * 4);
                const uint8_t* mine = image + offset;
                const uint8_t* theirs = levelPtrs[i];
                if (uint64_t(offset) + bytes > imageSize || !theirs) return false;
                if (std::memcmp(mine, theirs, bytes) != 0)
                {
                    uint32_t at = 0;
                    while (at < bytes && mine[at] == theirs[at]) ++at;
                    WLOG_WARN("textures: verify: '%s' level %u (%ux%u) differs at byte %u: host %02X, loader %02X", name, i,
                              std::max(1u, width >> i), std::max(1u, height >> i), at, mine[at], theirs[at]);
                    return false;
                }
            }
            return true;
        }

        /// Checks a texture image against the file it came from; false on any difference.
        bool Check(const char* name, uint32_t archive, const h::FileData& image)
        {
            h::FileData file;
            if (h::ReadFile(name, archive, file) != Status::Ok) return true;   // nothing to compare against
            bool same;
            if (image.image == uint32_t(wxl::ipc::TextureImage::Decoded))
                same = SameAsLoader(name, file.data, uint32_t(file.size), image.data, uint32_t(image.size));
            else
                same = file.size == image.size && std::memcmp(file.data, image.data, size_t(file.size)) == 0;
            h::ReleaseFile(file);
            g_c.verified.fetch_add(1, std::memory_order_relaxed);
            if (!same)
            {
                g_c.mismatches.fetch_add(1, std::memory_order_relaxed);
                WLOG_WARN("textures: verify: '%s' does not match what the loader would build; read natively", name);
            }
            return same;
        }

        /// The loader's buffer for this texture is the stand-in, not the file: what it would have allocated.
        uint64_t HeapSaved(const Image& image)
        {
            return !image.owned && image.fileSize > kStubBytes ? image.fileSize - kStubBytes : 0;
        }

        void Account(Image& out)
        {
            g_c.imagesInFlight.fetch_add(out.size, std::memory_order_relaxed);
            if (const uint64_t saved = HeapSaved(out))
            {
                g_c.heapSaved.fetch_add(saved, std::memory_order_relaxed);
                AtomicMax(g_c.heapPeak, g_c.heapInFlight.fetch_add(saved, std::memory_order_relaxed) + saved);
            }
        }
    }

    bool Claim(const char* name, uint64_t size)
    {
        if (!t_armed || !name || !EndsWithBlp(name)) return false;
        t_armed = 0;
        if (size < kStubBytes || size > 0x7FFFFFFFull || HasClientTransform(name))
        {
            g_c.declined.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    bool Fetch(const char* name, uint32_t archive, uint32_t fileSize, Image& out)
    {
        out = Image{};
        out.fileSize = fileSize;
        Throttle(fileSize);
        const uint64_t t0 = wxl::ipc::NowUs();

        h::ReadOptions options;
        options.flags = wxl::ipc::kFlagTexture;
        options.a4 = MaxEdge();
        options.sizeHint = fileSize;
        h::FileData file;
        Status s = Status::Failed;
        for (int attempt = 0; attempt < 2; ++attempt)
        {
            s = h::ReadFile(name, archive, file, 0, &options);
            if (s == Status::Ok || s == Status::NotFound || !h::WaitSettled(kSettleMs)) break;
        }
        if (s != Status::Ok)
        {
            // The texture path failed on its own: the file itself, as any read.
            s = h::ReadFile(name, archive, file);
            if (s == Status::Ok) file.image = uint32_t(wxl::ipc::TextureImage::File);
        }
        if (s != Status::Ok || file.size < kStubBytes || file.size > 0x7FFFFFFFull)
        {
            h::ReleaseFile(file);
            g_c.fallbacks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (Verify() && !Check(name, archive, file))
        {
            h::ReleaseFile(file);
            if (h::ReadFile(name, archive, file) != Status::Ok)
            {
                g_c.fallbacks.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            file.image = uint32_t(wxl::ipc::TextureImage::File);
        }

        out.file = file;
        out.data = file.data;
        out.size = uint32_t(file.size);
        out.decoded = file.image == uint32_t(wxl::ipc::TextureImage::Decoded);
        g_c.served.fetch_add(1, std::memory_order_relaxed);
        if (out.decoded) g_c.decoded.fetch_add(1, std::memory_order_relaxed);
        if (file.served & wxl::ipc::kServedFromCache) g_c.fromCache.fetch_add(1, std::memory_order_relaxed);
        if (file.served & wxl::ipc::kServedPrefetched) g_c.prefetched.fetch_add(1, std::memory_order_relaxed);
        g_c.imageBytes.fetch_add(out.size, std::memory_order_relaxed);
        Account(out);
        RecordLatency(wxl::ipc::NowUs() - t0);
        return true;
    }

    void Adopt(uint8_t* bytes, uint32_t size, uint32_t fileSize, Image& out)
    {
        out = Image{};
        out.owned = bytes;
        out.data = bytes;
        out.size = size;
        out.fileSize = fileSize;
        g_c.fallbacks.fetch_add(1, std::memory_order_relaxed);
        Account(out);
    }

    void WriteStub(const Image& image, uintptr_t base, uint32_t pos, uint8_t* dst, uint32_t len)
    {
        WriteStubBytes(image.data, image.size, base, pos, dst, len);
    }

    void Release(Image& image)
    {
        if (image.data)
        {
            g_c.imagesInFlight.fetch_sub(std::min<uint64_t>(g_c.imagesInFlight.load(), image.size), std::memory_order_relaxed);
            if (const uint64_t saved = HeapSaved(image))
                g_c.heapInFlight.fetch_sub(std::min<uint64_t>(g_c.heapInFlight.load(), saved), std::memory_order_relaxed);
        }
        h::ReleaseFile(image.file);
        free(image.owned);
        image = Image{};
    }

    void LogCounters(const char* reason)
    {
        constexpr double mb = 1024.0 * 1024.0;
        uint32_t samples[kLatencySamples];
        const uint64_t count = std::min<uint64_t>(g_c.latencyCount.load(), kLatencySamples);
        for (uint64_t i = 0; i < count; ++i) samples[i] = g_latency[i].load(std::memory_order_relaxed);
        uint32_t p99 = 0;
        if (count)
        {
            const size_t at = size_t((count * 99) / 100);
            std::nth_element(samples, samples + at, samples + count);
            p99 = samples[at];
        }
        const uint64_t n = g_c.latencyCount.load();
        WLOG_INFO("memory[%s]: textures: %llu from wxl-host (%llu decoded there, %llu from its cache, %llu prefetched, "
                  "%.1f MB of images), %llu read natively (%llu fallbacks, %llu declined) | verified %llu, mismatches %llu | "
                  "fetch mean %llu us, p99 %u us (last %llu) | Wow.exe heap not allocated %.1f MB (%.1f MB now, peak %.1f MB), "
                  "images in window %.1f MB, throttled %llu",
                  reason, static_cast<unsigned long long>(g_c.served.load()), static_cast<unsigned long long>(g_c.decoded.load()),
                  static_cast<unsigned long long>(g_c.fromCache.load()), static_cast<unsigned long long>(g_c.prefetched.load()),
                  g_c.imageBytes.load() / mb,
                  static_cast<unsigned long long>(g_c.fallbacks.load() + g_c.declined.load()),
                  static_cast<unsigned long long>(g_c.fallbacks.load()), static_cast<unsigned long long>(g_c.declined.load()),
                  static_cast<unsigned long long>(g_c.verified.load()), static_cast<unsigned long long>(g_c.mismatches.load()),
                  static_cast<unsigned long long>(n ? g_c.latencyTotal.load() / n : 0), p99,
                  static_cast<unsigned long long>(count), g_c.heapSaved.load() / mb, g_c.heapInFlight.load() / mb,
                  g_c.heapPeak.load() / mb, g_c.imagesInFlight.load() / mb,
                  static_cast<unsigned long long>(g_c.throttled.load()));

        const wxl::ipc::HostCounters* hc = h::HostSideCounters();
        if (!hc || !h::Available()) return;
        WLOG_INFO("memory[%s]: host cache %.1f of %.1f MB in %llu entries, hits %llu, misses %llu, evictions %llu | "
                  "prefetch: %llu hints, %llu queued, %llu read (%.1f MB), %llu used, %llu evicted unused | "
                  "host textures %llu (%llu decoded in %.1f ms) | backing copies kept as references %llu (%.1f MB), "
                  "rebuilt %llu, failed %llu",
                  reason, hc->cacheBytes.load() / mb, hc->cacheLimit.load() / mb,
                  static_cast<unsigned long long>(hc->cacheEntries.load()), static_cast<unsigned long long>(hc->cacheHits.load()),
                  static_cast<unsigned long long>(hc->cacheMisses.load()), static_cast<unsigned long long>(hc->cacheEvictions.load()),
                  static_cast<unsigned long long>(hc->prefetchHints.load()), static_cast<unsigned long long>(hc->prefetchQueued.load()),
                  static_cast<unsigned long long>(hc->prefetchDone.load()), hc->prefetchBytes.load() / mb,
                  static_cast<unsigned long long>(hc->prefetchHits.load()), static_cast<unsigned long long>(hc->prefetchWasted.load()),
                  static_cast<unsigned long long>(hc->textureReads.load()), static_cast<unsigned long long>(hc->textureDecoded.load()),
                  hc->textureDecodeNs.load() / 1e6, static_cast<unsigned long long>(hc->dedupeValues.load()),
                  hc->dedupeBytes.load() / mb, static_cast<unsigned long long>(hc->dedupeRegenerated.load()),
                  static_cast<unsigned long long>(hc->dedupeFailed.load()));
    }
}

WXL_REGISTER_FEATURE("host textures", true, wxl::runtime::storage::textures::InstallFeature)
