// Engine allocations by source: Storm's allocator and the CRT heap under it, counted per call site.
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

// SMemAlloc is malloc or calloc on the CRT heap, the size rounded up to 8, with no header of its own.
// Each tracked block is asked 8 bytes larger and ends with a tag naming its site; free and realloc find
// the tag again through the heap's own block size (HeapSize is exact on the system heap). The CRT
// hooks count what bypasses Storm, and settle a Storm block the CRT frees directly (or the reverse).
// Counters are lock-free atomics; the tables are allocated at install, filled on first use, never freed.

#include "engine/diag/AllocTrack.hpp"

#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "offsets/engine/Crt.hpp"
#include "offsets/engine/Mem.hpp"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstring>
#include <new>

#pragma intrinsic(_AddressOfReturnAddress)

namespace
{
    namespace mem = wxl::offsets::engine::mem;
    namespace crt = wxl::offsets::engine::crt;

    constexpr uint32_t kSiteCap     = wxl::diag::alloc::kSiteSlots - 1;   // power of two
    constexpr uint32_t kOverflow    = kSiteCap;      // shared site once the table is full
    constexpr uint32_t kNoSite      = 0xFFFFFFFFu;
    constexpr uint32_t kFileCap     = wxl::diag::alloc::kFileSlots;       // power of two; slot 0 collects the overflow
    constexpr uint32_t kMaxProbe    = 64;
    constexpr uint32_t kTagBytes    = 8;
    constexpr uint32_t kTagKey      = 0x5EB1A7E5u;
    constexpr uint32_t kTagMix      = 0x9E3779B1u;
    constexpr uint32_t kMaxTracked  = 0x7FFF0000u;   // larger requests fail anyway: passed on untagged
    constexpr uint32_t kReAllocNoop = 0xB00BEEE5u;   // SMemReAlloc returns null for this flags value
    constexpr uint32_t kInPlaceOnly = 0x10;          // SMemReAlloc flag: never move a live block
    constexpr uint32_t kNameNull    = 0;
    constexpr uint32_t kNameHeap    = 1;             // a name outside every image: one shared key

    enum : uint32_t { kEmpty = 0, kFilling = 1, kReady = 2 };

    struct Site
    {
        std::atomic<uint32_t> state;
        uint32_t              file, line, owner;
        std::atomic<uint32_t> bytes, count, peak, allocs;
    };
    static_assert(sizeof(Site) == 32, "two sites per cache line");

    struct SiteMeta
    {
        uint32_t ret;
        uint16_t file;
        uint8_t  depth;     // frames walked for the owner; 0 when the name identifies the caller
        uint8_t  generic;
    };

    struct FileEntry
    {
        std::atomic<uint32_t> state;
        uint32_t              ptr;
        uint8_t               depth;
        char                  raw[55];
    };
    static_assert(sizeof(FileEntry) == 64, "one cache line per name");

    // Allocated by Install only: with the diagnostic off they cost nothing, not even address space.
    Site*      g_sites = nullptr;    // kSiteCap + 1
    SiteMeta*  g_meta = nullptr;     // kSiteCap + 1
    FileEntry* g_files = nullptr;    // kFileCap
    std::atomic<uint32_t> g_siteCount{ 0 };
    std::atomic<uint32_t> g_fileCount{ 0 };

    std::atomic<uint32_t> g_stormBytes{ 0 }, g_stormPeak{ 0 }, g_windowPeak{ 0 };
    std::atomic<uint32_t> g_directBytes{ 0 }, g_directBlocks{ 0 }, g_directPeak{ 0 }, g_directAllocs{ 0 };
    std::atomic<uint32_t> g_mixedFrees{ 0 };
    std::atomic<uint32_t> g_untracked{ 0 };   // late start: frees of blocks older than the hooks
    bool g_active = false;
    bool g_late = false;                      // installed after the client's CRT started (unpatched Wow.exe)

    thread_local uint32_t t_depth = 0;   // inside one of our hooks' originals: the CRT hooks stand aside

    // --- Wow.exe's layout, from its own headers ------------------------------------------------------

    uint32_t g_imageLo = 0, g_imageSize = 0, g_textLo = 0, g_textSize = 0;

    constexpr uint32_t kRangeCap = 16;
    struct Range { uint32_t lo, size; };
    Range g_ranges[kRangeCap];                 // other images whose strings were passed as names
    std::atomic<uint32_t> g_rangeCount{ 0 };
    std::atomic<bool>     g_rangeLock{ false };

    bool ImageBounds(uintptr_t base, uint32_t& size, uint32_t* textLo, uint32_t* textSize)
    {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
        size = nt->OptionalHeader.SizeOfImage;
        if (textLo)
        {
            *textLo = uint32_t(base + nt->OptionalHeader.BaseOfCode);
            *textSize = nt->OptionalHeader.SizeOfCode;
        }
        return true;
    }

    inline bool InRange(uint32_t p, uint32_t lo, uint32_t size) { return p - lo < size; }

    /// Keys a name pointer: Wow.exe's strings and other images' are kept, anything else is one key.
    uint32_t Canonical(const char* file)
    {
        const uint32_t p = uint32_t(reinterpret_cast<uintptr_t>(file));
        if (InRange(p, g_imageLo, g_imageSize)) return p;
        if (p <= kNameHeap) return p;
        const uint32_t n = g_rangeCount.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n; ++i)
            if (InRange(p, g_ranges[i].lo, g_ranges[i].size)) return p;

        // Rare: a name from one of our own DLLs, or from memory that may not outlive the call.
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(file, &mbi, sizeof mbi) || mbi.Type != MEM_IMAGE || mbi.State != MEM_COMMIT) return kNameHeap;
        const uintptr_t base = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
        uint32_t size = 0;
        if (!ImageBounds(base, size, nullptr, nullptr)) return kNameHeap;
        while (g_rangeLock.exchange(true, std::memory_order_acquire)) _mm_pause();
        const uint32_t count = g_rangeCount.load(std::memory_order_relaxed);
        bool known = false;
        for (uint32_t i = 0; i < count; ++i) known |= g_ranges[i].lo == uint32_t(base);
        if (!known && count < kRangeCap)
        {
            g_ranges[count] = { uint32_t(base), size };
            g_rangeCount.store(count + 1, std::memory_order_release);
        }
        g_rangeLock.store(false, std::memory_order_release);
        return p;
    }

    // --- names ---------------------------------------------------------------------------------------

    size_t SafeRead(char* out, size_t cap, const char* s)
    {
        size_t n = 0;
        __try
        {
            while (n + 1 < cap && s[n]) { out[n] = s[n]; ++n; }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
        }
        out[n] = 0;
        return n;
    }

    /// Copies a name: a path keeps its base name, a type name its head.
    void CopyName(char* dst, size_t cap, uint32_t ptr)
    {
        if (ptr == kNameNull) { strcpy_s(dst, cap, "(null)"); return; }
        if (ptr == kNameHeap) { strcpy_s(dst, cap, "(heap name)"); return; }
        char tmp[256];
        const size_t n = SafeRead(tmp, sizeof tmp, reinterpret_cast<const char*>(uintptr_t(ptr)));
        const char* base = tmp;
        for (size_t i = 0; i < n; ++i)
            if (tmp[i] == '\\' || tmp[i] == '/') base = tmp + i + 1;
        strncpy_s(dst, cap, *base ? base : tmp, _TRUNCATE);
    }

    constexpr uint8_t kOwnCallSite = 0xFF;   // "depth" of a name inlined into its owner: the call site is the owner

    /// Frames to walk for the owner: 0 when the name identifies the caller.
    uint8_t DepthOf(const char* raw, uint32_t ptr)
    {
        if (ptr == kNameNull || ptr == kNameHeap) return 1;
        // A built-in type's name (".E" = unsigned char): a container template sits between.
        if (raw[0] == '.' && !std::strchr(raw, '@')) return 2;
        static const char* const kHelpers[] = { "new", "MemoryStorm.cpp", "DynamicString.cpp", "cmemblock.cpp",
                                                "RCString.cpp", "CDataStore.h" };
        for (const char* h : kHelpers)
            if (_stricmp(raw, h) == 0) return 1;
        // Template headers compiled into each user: every user passes the same name and line.
        static const char* const kInlined[] = { "stpl.h", "HashMap.h", "NewZerofill.h" };
        for (const char* h : kInlined)
            if (_stricmp(raw, h) == 0) return kOwnCallSite;
        return 0;
    }

    uint16_t FileFor(uint32_t ptr)
    {
        uint32_t h = ((ptr * 0x9E3779B1u) >> 16) & (kFileCap - 1);
        for (uint32_t probe = 0; probe < kMaxProbe; ++probe, h = (h + 1) & (kFileCap - 1))
        {
            if (h == 0) continue;
            FileEntry& e = g_files[h];
            uint32_t st = e.state.load(std::memory_order_acquire);
            if (st == kEmpty)
            {
                uint32_t expected = kEmpty;
                if (e.state.compare_exchange_strong(expected, kFilling, std::memory_order_acquire))
                {
                    e.ptr = ptr;
                    CopyName(e.raw, sizeof e.raw, ptr);
                    e.depth = DepthOf(e.raw, ptr);
                    e.state.store(kReady, std::memory_order_release);
                    g_fileCount.fetch_add(1, std::memory_order_relaxed);
                    return uint16_t(h);
                }
                st = expected;
            }
            while (st == kFilling)
            {
                _mm_pause();
                st = e.state.load(std::memory_order_acquire);
            }
            if (e.ptr == ptr) return uint16_t(h);
        }
        return 0;
    }

    // --- sites ---------------------------------------------------------------------------------------

    inline uint32_t Hash(uint32_t a, uint32_t b, uint32_t c)
    {
        const uint32_t h = a * 0x9E3779B1u + b * 0x85EBCA77u + c * 0xC2B2AE3Du;
        return h ^ (h >> 13);
    }

    uint32_t Intern(uint32_t file, uint32_t line, uint32_t owner, uint32_t ret)
    {
        uint32_t h = Hash(file, line, owner) & (kSiteCap - 1);
        for (uint32_t probe = 0; probe < kMaxProbe; ++probe, h = (h + 1) & (kSiteCap - 1))
        {
            Site& s = g_sites[h];
            uint32_t st = s.state.load(std::memory_order_acquire);
            if (st == kEmpty)
            {
                uint32_t expected = kEmpty;
                if (s.state.compare_exchange_strong(expected, kFilling, std::memory_order_acquire))
                {
                    s.file = file;
                    s.line = line;
                    s.owner = owner;
                    const uint16_t f = FileFor(file);
                    const uint8_t depth = g_files[f].depth;
                    g_meta[h] = { ret, f, uint8_t(owner ? 0 : depth), uint8_t(depth ? 1 : 0) };
                    s.state.store(kReady, std::memory_order_release);
                    g_siteCount.fetch_add(1, std::memory_order_relaxed);
                    return h;
                }
                st = expected;
            }
            while (st == kFilling)
            {
                _mm_pause();
                st = s.state.load(std::memory_order_acquire);
            }
            if (s.file == file && s.line == line && s.owner == owner) return h;
        }
        return kOverflow;
    }

    /// The return address `depth` frames above the hook, through the engine's EBP chain.
    uint32_t OwnerOf(const uintptr_t* retSlot, uint32_t depth)
    {
        const uintptr_t stackHi = __readfsdword(4);   // NT_TIB.StackBase
        uintptr_t below = reinterpret_cast<uintptr_t>(retSlot);
        uintptr_t fp = retSlot[-1];                   // the caller's EBP, saved by the hook's own prologue
        uint32_t owner = 0;
        for (uint32_t i = 0; i < depth; ++i)
        {
            if (fp <= below || fp + 8 > stackHi || (fp & 3)) break;
            const uint32_t ret = uint32_t(reinterpret_cast<const uintptr_t*>(fp)[1]);
            if (!InRange(ret, g_textLo, g_textSize)) break;
            owner = ret;
            below = fp;
            fp = reinterpret_cast<const uintptr_t*>(fp)[0];
        }
        return owner;
    }

    uint32_t SiteFor(const char* file, int line, const uintptr_t* retSlot)
    {
        const uint32_t f = Canonical(file);
        const uint32_t ret = uint32_t(retSlot[0]);
        const uint32_t key = Intern(f, uint32_t(line), 0, ret);
        const uint32_t depth = g_meta[key].depth;
        if (!depth || key == kOverflow) return key;
        const uint32_t owner = depth == kOwnCallSite ? ret : OwnerOf(retSlot, depth);
        return owner ? Intern(f, uint32_t(line), owner, ret) : key;
    }

    // --- the block tag -------------------------------------------------------------------------------

    HANDLE g_crtHeap = nullptr;
    int    g_heapMode = 0;

    inline void ReadHeapMode()
    {
        // _heap_init has run by the first allocation; both globals are then fixed for the session.
        g_crtHeap = *reinterpret_cast<HANDLE*>(crt::kHeap);
        g_heapMode = *reinterpret_cast<int*>(crt::kActiveHeap);
    }

    /// The heap's own size for a block: exact on the system heap, the CRT's _msize otherwise.
    inline uint32_t BlockSize(const void* p)
    {
        if (!g_heapMode) ReadHeapMode();
        const SIZE_T n = g_heapMode == 1 ? HeapSize(g_crtHeap, 0, p)
                                         : reinterpret_cast<crt::MsizeFn>(crt::kMsize)(const_cast<void*>(p));
        return n == SIZE_T(-1) ? 0 : uint32_t(n);
    }

    inline uint32_t TagCheck(const void* p, uint32_t block, uint32_t site)
    {
        return (uint32_t(reinterpret_cast<uintptr_t>(p)) ^ block ^ (site << 13)) * kTagMix;
    }

    inline void WriteTag(uint8_t* p, uint32_t block, uint32_t site)
    {
        const uint32_t tag[2] = { site ^ kTagKey, TagCheck(p, block, site) };
        std::memcpy(p + block - kTagBytes, tag, sizeof tag);
    }

    inline uint32_t ReadTag(const uint8_t* p, uint32_t block)
    {
        if (block < kTagBytes) return kNoSite;
        uint32_t tag[2];
        std::memcpy(tag, p + block - kTagBytes, sizeof tag);
        const uint32_t site = tag[0] ^ kTagKey;
        return site <= kOverflow && tag[1] == TagCheck(p, block, site) ? site : kNoSite;
    }

    /// Wipes a tag before its block is freed or moved, so a later block at the same place cannot match it.
    inline void ClearTag(uint8_t* p, uint32_t block)
    {
        std::memset(p + block - kTagBytes, 0, kTagBytes);
    }

    inline uint32_t Round8(uint32_t n) { return (n + 7) & ~7u; }

    // --- counters ------------------------------------------------------------------------------------

    constexpr auto kRelaxed = std::memory_order_relaxed;

    /// A racy maximum: two writers may lose one update, which a diagnostic can afford.
    inline void RaiseTo(std::atomic<uint32_t>& peak, uint32_t value)
    {
        if (value > peak.load(kRelaxed)) peak.store(value, kRelaxed);
    }

    void Charge(uint32_t site, uint32_t bytes)
    {
        Site& s = g_sites[site];
        RaiseTo(s.peak, s.bytes.fetch_add(bytes, kRelaxed) + bytes);
        s.count.fetch_add(1, kRelaxed);
        s.allocs.fetch_add(1, kRelaxed);
        const uint32_t total = g_stormBytes.fetch_add(bytes, kRelaxed) + bytes;
        RaiseTo(g_stormPeak, total);
        RaiseTo(g_windowPeak, total);
    }

    void Discharge(uint32_t site, uint32_t bytes)
    {
        Site& s = g_sites[site];
        s.bytes.fetch_sub(bytes, kRelaxed);
        s.count.fetch_sub(1, kRelaxed);
        g_stormBytes.fetch_sub(bytes, kRelaxed);
    }

    void ChargeDirect(uint32_t bytes)
    {
        RaiseTo(g_directPeak, g_directBytes.fetch_add(bytes, kRelaxed) + bytes);
        g_directBlocks.fetch_add(1, kRelaxed);
        g_directAllocs.fetch_add(1, kRelaxed);
    }

    void DischargeDirect(uint32_t bytes)
    {
        g_directBytes.fetch_sub(bytes, kRelaxed);
        g_directBlocks.fetch_sub(1, kRelaxed);
    }

    /// Settles a block about to be freed: its site when tagged, the CRT-direct total otherwise.
    void Settle(void* ptr, bool fromStorm)
    {
        uint8_t* p = static_cast<uint8_t*>(ptr);
        const uint32_t block = BlockSize(p);
        if (!block) return;   // the heap does not know this pointer: the free itself will say so
        const uint32_t site = ReadTag(p, block);
        if (site != kNoSite)
        {
            ClearTag(p, block);
            Discharge(site, block - kTagBytes);
            return;
        }
        if (g_late)
        {
            g_untracked.fetch_add(1, kRelaxed);   // may predate the hooks: never subtracted
            return;
        }
        DischargeDirect(block);
        if (fromStorm) g_mixedFrees.fetch_add(1, kRelaxed);
    }

    /// Tags a fresh Storm block and charges its site.
    void Adopt(uint8_t* p, uint32_t asked, uint32_t site)
    {
        if (!g_heapMode) ReadHeapMode();
        const uint32_t block = g_heapMode == 1 ? asked : BlockSize(p);   // HeapAlloc keeps the exact size
        WriteTag(p, block, site);
        Charge(site, block - kTagBytes);
    }

    // --- Storm hooks ---------------------------------------------------------------------------------

    // SMemFree returns 1 in EAX; the hook passes it on.
    using StormFreeFn = int(__stdcall*)(void* ptr, const char* file, int line, uint32_t flags);

    mem::Mem_AllocFn   g_origAlloc = nullptr;
    StormFreeFn        g_origFree = nullptr;
    mem::Mem_ReAllocFn g_origReAlloc = nullptr;

    // The Storm hooks keep an EBP frame: OwnerOf reads the caller's EBP just below the return address.
#pragma optimize("y", off)

    void* __stdcall hkAlloc(uint32_t size, const char* file, int line, uint32_t flags)
    {
        if (size > kMaxTracked) return g_origAlloc(size, file, line, flags);
        const uint32_t site = SiteFor(file, line, static_cast<const uintptr_t*>(_AddressOfReturnAddress()));
        const uint32_t asked = Round8(size) + kTagBytes;
        ++t_depth;
        uint8_t* p = static_cast<uint8_t*>(g_origAlloc(asked, file, line, flags));
        --t_depth;
        if (p) Adopt(p, asked, site);
        return p;
    }

    void* __stdcall hkReAlloc(void* ptr, uint32_t size, const char* file, int line, uint32_t flags)
    {
        if (flags == kReAllocNoop || (ptr && (flags & kInPlaceOnly)) || size > kMaxTracked)
            return g_origReAlloc(ptr, size, file, line, flags);

        if (size == 0 && ptr)
        {
            // Size 0 frees the block and returns null.
            Settle(ptr, true);
            ++t_depth;
            void* r = g_origReAlloc(ptr, size, file, line, flags);
            --t_depth;
            return r;
        }

        const uint32_t site = SiteFor(file, line, static_cast<const uintptr_t*>(_AddressOfReturnAddress()));
        const uint32_t asked = Round8(size) + kTagBytes;
        if (!ptr)
        {
            // A null block is a plain allocation, which SMemReAlloc forwards to SMemAlloc.
            ++t_depth;
            uint8_t* p = static_cast<uint8_t*>(g_origAlloc(asked, file, line, flags));
            --t_depth;
            if (p) Adopt(p, asked, site);
            return p;
        }

        uint8_t* old = static_cast<uint8_t*>(ptr);
        const uint32_t oldBlock = BlockSize(old);
        const uint32_t oldSite = ReadTag(old, oldBlock);
        if (oldSite != kNoSite) ClearTag(old, oldBlock);
        ++t_depth;
        uint8_t* p = static_cast<uint8_t*>(g_origReAlloc(ptr, asked, file, line, flags));
        --t_depth;
        if (!p)
        {
            if (oldSite != kNoSite) WriteTag(old, oldBlock, oldSite);   // the old block is still live
            return p;
        }
        if (oldSite != kNoSite)
            Discharge(oldSite, oldBlock - kTagBytes);
        else if (g_late)
            g_untracked.fetch_add(1, kRelaxed);
        else
        {
            DischargeDirect(oldBlock);
            g_mixedFrees.fetch_add(1, kRelaxed);
        }
        Adopt(p, asked, site);
        return p;
    }

#pragma optimize("", on)

    int __stdcall hkFree(void* ptr, const char* file, int line, uint32_t flags)
    {
        if (ptr) Settle(ptr, true);
        ++t_depth;
        const int r = g_origFree(ptr, file, line, flags);
        --t_depth;
        return r;
    }

    // --- CRT hooks: what bypasses Storm --------------------------------------------------------------

    crt::MallocFn     g_origMalloc = nullptr;
    crt::CallocImplFn g_origCallocImpl = nullptr;
    crt::ReallocFn    g_origRealloc = nullptr;
    crt::FreeFn       g_origCrtFree = nullptr;

    void* __cdecl hkMalloc(size_t size)
    {
        if (t_depth) return g_origMalloc(size);
        ++t_depth;
        void* p = g_origMalloc(size);
        --t_depth;
        if (p) ChargeDirect(BlockSize(p));
        return p;
    }

    void* __cdecl hkCallocImpl(size_t count, size_t size, int* errnoOut)
    {
        if (t_depth) return g_origCallocImpl(count, size, errnoOut);
        ++t_depth;
        void* p = g_origCallocImpl(count, size, errnoOut);
        --t_depth;
        if (p) ChargeDirect(BlockSize(p));
        return p;
    }

    void* __cdecl hkRealloc(void* ptr, size_t size)
    {
        if (t_depth) return g_origRealloc(ptr, size);
        if (!ptr)
        {
            ++t_depth;
            void* p = g_origRealloc(ptr, size);
            --t_depth;
            if (p) ChargeDirect(BlockSize(p));
            return p;
        }

        uint8_t* old = static_cast<uint8_t*>(ptr);
        const uint32_t oldBlock = BlockSize(old);
        const uint32_t oldSite = ReadTag(old, oldBlock);   // a Storm block resized by the CRT leaves Storm
        if (oldSite != kNoSite) ClearTag(old, oldBlock);
        ++t_depth;
        void* p = g_origRealloc(ptr, size);
        --t_depth;
        if (!p && size)
        {
            if (oldSite != kNoSite) WriteTag(old, oldBlock, oldSite);
            return p;
        }
        if (oldSite != kNoSite) Discharge(oldSite, oldBlock - kTagBytes);
        else DischargeDirect(oldBlock);
        if (p)
        {
            g_directAllocs.fetch_sub(1, kRelaxed);   // a resize, not a new allocation
            ChargeDirect(BlockSize(p));
        }
        return p;
    }

    void __cdecl hkCrtFree(void* ptr)
    {
        if (t_depth || !ptr)
        {
            g_origCrtFree(ptr);
            return;
        }
        Settle(ptr, false);
        ++t_depth;
        g_origCrtFree(ptr);
        --t_depth;
    }
}

namespace wxl::diag::alloc
{
    bool Install()
    {
        const uintptr_t exe = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!exe || !ImageBounds(exe, g_imageSize, &g_textLo, &g_textSize))
        {
            WLOG_WARN("memory: Wow.exe headers unreadable, allocation tracking off");
            return false;
        }
        g_imageLo = uint32_t(exe);

        // Zeroed pages: every slot starts empty.
        const size_t bytes = sizeof(Site) * (kSiteCap + 1) + sizeof(SiteMeta) * (kSiteCap + 1) + sizeof(FileEntry) * kFileCap;
        void* tables = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!tables)
        {
            WLOG_WARN("memory: no room for the allocation tables, allocation tracking off");
            return false;
        }
        g_sites = new (tables) Site[kSiteCap + 1]{};
        g_meta = new (g_sites + kSiteCap + 1) SiteMeta[kSiteCap + 1]{};
        g_files = new (g_meta + kSiteCap + 1) FileEntry[kFileCap]{};

        FileEntry& other = g_files[0];
        other.ptr = 0xFFFFFFFFu;
        strcpy_s(other.raw, "(table full)");
        other.state.store(kReady, std::memory_order_release);
        Site& overflow = g_sites[kOverflow];
        overflow.file = 0xFFFFFFFFu;
        g_meta[kOverflow] = { 0, 0, 0, 0 };
        overflow.state.store(kReady, std::memory_order_release);

        // Loaded through the import the patcher adds, this runs before the client's CRT exists. Loaded
        // later (by the d3d9 proxy, on an unpatched Wow.exe), older blocks are untagged: Storm is still
        // exact for what follows, the CRT-direct total is not, so its hooks stay off.
        g_late = *reinterpret_cast<HANDLE*>(crt::kHeap) != nullptr;

        // Head of each chain (-100): the owner walk needs the engine as the direct caller.
        bool ok = true;
        ok &= wxl::hook::Install("Diag.SMemAlloc", mem::kAlloc, &hkAlloc, &g_origAlloc, -100);
        ok &= wxl::hook::Install("Diag.SMemReAlloc", mem::kReAlloc, &hkReAlloc, &g_origReAlloc, -100);
        ok &= wxl::hook::Install("Diag.SMemFree", mem::kFree, &hkFree, &g_origFree, -100);
        if (!g_late)
        {
            ok &= wxl::hook::Install("Diag.CrtMalloc", crt::kMalloc, &hkMalloc, &g_origMalloc, -100);
            ok &= wxl::hook::Install("Diag.CrtCallocImpl", crt::kCallocImpl, &hkCallocImpl, &g_origCallocImpl, -100);
            ok &= wxl::hook::Install("Diag.CrtRealloc", crt::kRealloc, &hkRealloc, &g_origRealloc, -100);
            ok &= wxl::hook::Install("Diag.CrtFree", crt::kFree, &hkCrtFree, &g_origCrtFree, -100);
        }
        else
        {
            WLOG_WARN("memory: allocation tracking started after the client's CRT (Wow.exe not patched?): "
                      "Storm counts only what follows, CRT-direct is not counted");
        }
        g_active = ok;
        if (!ok) WLOG_WARN("memory: an allocator hook failed to register, allocation tracking incomplete");
        return ok;
    }

    bool Active()
    {
        return g_active;
    }

    Totals ReadTotals()
    {
        Totals t;
        if (!g_active) return t;
        for (uint32_t i = 0; i <= kSiteCap; ++i)
        {
            const Site& s = g_sites[i];
            if (s.state.load(std::memory_order_acquire) != kReady) continue;
            t.stormBlocks += s.count.load(kRelaxed);
            t.stormAllocs += s.allocs.load(kRelaxed);
        }
        t.stormBytes = g_stormBytes.load(kRelaxed);
        t.stormPeak = g_stormPeak.load(kRelaxed);
        t.tagBytes = t.stormBlocks * kTagBytes;
        t.directBytes = g_directBytes.load(kRelaxed);
        t.directPeak = g_directPeak.load(kRelaxed);
        t.directBlocks = g_directBlocks.load(kRelaxed);
        t.directAllocs = g_directAllocs.load(kRelaxed);
        t.mixedFrees = g_mixedFrees.load(kRelaxed);
        t.untrackedFrees = g_untracked.load(kRelaxed);
        t.late = g_late;
        t.sites = g_siteCount.load(kRelaxed);
        t.files = g_fileCount.load(kRelaxed);
        t.overflow = g_sites[kOverflow].allocs.load(kRelaxed);
        t.heapMode = g_heapMode;
        return t;
    }

    uint32_t SiteSlots()
    {
        return kSiteCap + 1;
    }

    bool ReadSite(uint32_t index, SiteView& out)
    {
        if (index > kSiteCap || !g_sites) return false;
        const Site& s = g_sites[index];
        if (s.state.load(std::memory_order_acquire) != kReady) return false;
        const SiteMeta& m = g_meta[index];
        // A generic name's key record holds only what no owner could be found for; the overflow site
        // only what the full table turned away.
        if ((m.depth || index == kOverflow) && !s.allocs.load(kRelaxed)) return false;
        out.raw = g_files[m.file].raw;   // the overflow site uses the overflow name, "(table full)"
        out.line = s.line;
        out.owner = s.owner;
        out.ret = m.ret;
        out.file = m.file;
        out.generic = m.generic != 0;
        out.bytes = s.bytes.load(kRelaxed);
        out.count = s.count.load(kRelaxed);
        out.peak = s.peak.load(kRelaxed);
        out.allocs = s.allocs.load(kRelaxed);
        return true;
    }

    uint64_t StormBytes()
    {
        return g_stormBytes.load(kRelaxed);
    }

    uint64_t WindowPeak()
    {
        return g_windowPeak.load(kRelaxed);
    }

    void ResetWindowPeak()
    {
        g_windowPeak.store(g_stormBytes.load(kRelaxed), kRelaxed);
    }

    bool InEngineCode(uint32_t address)
    {
        return InRange(address, g_textLo, g_textSize);
    }
}
