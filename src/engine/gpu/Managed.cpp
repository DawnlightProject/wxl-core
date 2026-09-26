// D3DPOOL_MANAGED emulation for a D3D9Ex device: managed resources live in DEFAULT, locks go through staging.
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

// D3D9Ex refuses D3DPOOL_MANAGED. Every managed request is created in DEFAULT and its locks are emulated by
// per-class vtable hooks (docs/host.md, "GPU memory"):
//  - engine textures (created from Wow.exe): a lock hands out a pooled SYSTEMMEM staging texture, the unlock
//    uploads the rectangle with UpdateSurface. No system-memory copy stays; the staged bytes go to wxl-host
//    as the backing copy that answers a read lock or refills the texture after a device removal;
//  - engine buffers: a lock hands out pooled heap memory, the unlock writes it through and backs it up;
//  - everything else (core, extensions, volume textures): a SYSTEMMEM mirror, uploaded with UpdateTexture,
//    which is what the runtime did for MANAGED. Their lock patterns are unknown, so they keep the copy;
//  - a texture whose surface reaches an API that reads it on the CPU (the hardware cursor) is promoted to a
//    mirror, filled from what the GPU holds, and the API is handed the mirror's surface.

#include "engine/gpu/Managed.hpp"
#include "engine/gpu/HostLink.hpp"
#include "engine/gpu/Resources.hpp"

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "common/Mem.hpp"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <tuple>
#include <unordered_map>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace wxl::gpu::managed
{
    namespace
    {
        // --- configuration ---------------------------------------------------------------------------

        bool BackingEnabled()
        {
            static const bool on = ::wxl::config::Env("WXL_D3D9EX_BACKING", true);
            return on;
        }

        uint64_t StagingCap()
        {
            static const uint64_t bytes = ::wxl::config::U64("WXL_D3D9EX_STAGING_MB", 32, 4, 1024) << 20;
            return bytes;
        }

        // --- counters --------------------------------------------------------------------------------

        struct Counters
        {
            std::atomic<uint64_t> objects{ 0 }, bytes{ 0 }, mirrorBytes{ 0 }, stagingBytes{ 0 };
            std::atomic<uint64_t> uploads{ 0 }, uploadBytes{ 0 }, readbacks{ 0 };
            std::atomic<uint64_t> backingSent{ 0 }, backingBytes{ 0 }, backingDropped{ 0 }, restores{ 0 };
        } g_c;

        // --- the engine image, to tell its resources apart --------------------------------------------

        uintptr_t g_engineBase = 0;
        uintptr_t g_engineEnd = 0;

        bool IsEngine(void* caller)
        {
            const auto a = reinterpret_cast<uintptr_t>(caller);
            return a >= g_engineBase && a < g_engineEnd;
        }

        // The system d3d9 validates its arguments through the same vtables (UpdateSurface reads the
        // destination's GetDesc): it must see the real DEFAULT pool, everyone else the MANAGED they asked for.
        uintptr_t g_runtimeBase = 0;
        uintptr_t g_runtimeEnd = 0;

        bool ReportManagedTo(void* caller)
        {
            const auto a = reinterpret_cast<uintptr_t>(caller);
            return !(a >= g_runtimeBase && a < g_runtimeEnd);
        }

        // --- per-class slot hooks: one hook, the original found by the object's vtable ---------------

        template <class Fn>
        struct Slot
        {
            unsigned index;
            struct Entry { void** vtbl; Fn orig; };
            Entry entries[16] = {};
            std::atomic<uint32_t> count{ 0 };

            explicit Slot(unsigned i) : index(i) {}

            Fn Orig(const void* object) const
            {
                void** vtbl = *reinterpret_cast<void** const*>(object);
                const uint32_t n = count.load(std::memory_order_acquire);
                for (uint32_t i = 0; i < n; ++i)
                    if (entries[i].vtbl == vtbl) return entries[i].orig;
                return nullptr;
            }

            /// Writes hook into the object's vtable once. Called under g_mutex.
            void Hook(const void* object, Fn hook)
            {
                void** vtbl = *reinterpret_cast<void** const*>(object);
                const uint32_t n = count.load(std::memory_order_relaxed);
                for (uint32_t i = 0; i < n; ++i)
                    if (entries[i].vtbl == vtbl) return;
                if (n >= 16 || vtbl[index] == reinterpret_cast<void*>(hook)) return;
                entries[n] = Entry{ vtbl, reinterpret_cast<Fn>(vtbl[index]) };
                count.store(n + 1, std::memory_order_release);
                if (!::wxl::mem::SwapPointer(&vtbl[index], reinterpret_cast<void*>(hook), nullptr))
                    count.store(n, std::memory_order_release);
            }
        };

        using PreLoadFn = void(STDMETHODCALLTYPE*)(IDirect3DResource9*);
        using TexGetLevelDescFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT, D3DSURFACE_DESC*);
        using TexGetSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT, IDirect3DSurface9**);
        using TexLockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
        using TexUnlockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, UINT);
        using TexDirtyFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DTexture9*, const RECT*);
        using CubeGetLevelDescFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DCubeTexture9*, UINT, D3DSURFACE_DESC*);
        using CubeGetSurfaceFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT,
                                                             IDirect3DSurface9**);
        using CubeLockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT*,
                                                       const RECT*, DWORD);
        using CubeUnlockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, UINT);
        using CubeDirtyFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DCubeTexture9*, D3DCUBEMAP_FACES, const RECT*);
        using VolGetLevelDescFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVolumeTexture9*, UINT, D3DVOLUME_DESC*);
        using VolGetLevelFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVolumeTexture9*, UINT, IDirect3DVolume9**);
        using VolLockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVolumeTexture9*, UINT, D3DLOCKED_BOX*, const D3DBOX*,
                                                      DWORD);
        using VolUnlockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVolumeTexture9*, UINT);
        using VolDirtyFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVolumeTexture9*, const D3DBOX*);
        using SurfGetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*, D3DSURFACE_DESC*);
        using SurfLockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*, D3DLOCKED_RECT*, const RECT*, DWORD);
        using SurfUnlockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DSurface9*);
        using VolumeGetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVolume9*, D3DVOLUME_DESC*);
        using VolumeLockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVolume9*, D3DLOCKED_BOX*, const D3DBOX*, DWORD);
        using VolumeUnlockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVolume9*);
        using VbLockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVertexBuffer9*, UINT, UINT, void**, DWORD);
        using VbUnlockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVertexBuffer9*);
        using VbGetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DVertexBuffer9*, D3DVERTEXBUFFER_DESC*);
        using IbLockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DIndexBuffer9*, UINT, UINT, void**, DWORD);
        using IbUnlockFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DIndexBuffer9*);
        using IbGetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DIndexBuffer9*, D3DINDEXBUFFER_DESC*);
        using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
        using SetCursorPropsFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, IDirect3DSurface9*);
        using ShowCursorFn = BOOL(STDMETHODCALLTYPE*)(IDirect3DDevice9*, BOOL);
        using TestCoopFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
        using ResetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

        // IDirect3DResource9 / IDirect3DBaseTexture9 / IDirect3DTexture9 slots.
        Slot<PreLoadFn>          s_texPreLoad{ 9 };
        Slot<TexGetLevelDescFn>  s_texDesc{ 17 };
        Slot<TexGetSurfaceFn>    s_texSurface{ 18 };
        Slot<TexLockFn>          s_texLock{ 19 };
        Slot<TexUnlockFn>        s_texUnlock{ 20 };
        Slot<TexDirtyFn>         s_texDirty{ 21 };
        Slot<PreLoadFn>          s_cubePreLoad{ 9 };
        Slot<CubeGetLevelDescFn> s_cubeDesc{ 17 };
        Slot<CubeGetSurfaceFn>   s_cubeSurface{ 18 };
        Slot<CubeLockFn>         s_cubeLock{ 19 };
        Slot<CubeUnlockFn>       s_cubeUnlock{ 20 };
        Slot<CubeDirtyFn>        s_cubeDirty{ 21 };
        Slot<VolGetLevelDescFn>  s_volDesc{ 17 };
        Slot<VolGetLevelFn>      s_volLevel{ 18 };
        Slot<VolLockFn>          s_volLock{ 19 };
        Slot<VolUnlockFn>        s_volUnlock{ 20 };
        Slot<VolDirtyFn>         s_volDirty{ 21 };
        // IDirect3DSurface9 and IDirect3DVolume9.
        Slot<SurfGetDescFn>      s_surfDesc{ 12 };
        Slot<SurfLockFn>         s_surfLock{ 13 };
        Slot<SurfUnlockFn>       s_surfUnlock{ 14 };
        Slot<VolumeGetDescFn>    s_volumeDesc{ 8 };
        Slot<VolumeLockFn>       s_volumeLock{ 9 };
        Slot<VolumeUnlockFn>     s_volumeUnlock{ 10 };
        // Vertex and index buffers.
        Slot<VbLockFn>           s_vbLock{ 11 };
        Slot<VbUnlockFn>         s_vbUnlock{ 12 };
        Slot<VbGetDescFn>        s_vbDesc{ 13 };
        Slot<IbLockFn>           s_ibLock{ 11 };
        Slot<IbUnlockFn>         s_ibUnlock{ 12 };
        Slot<IbGetDescFn>        s_ibDesc{ 13 };
        // The device.
        Slot<TestCoopFn>         s_devTestCoop{ 3 };
        Slot<ResetFn>            s_devReset{ 16 };
        Slot<PresentFn>          s_devPresent{ 17 };
        Slot<SetCursorPropsFn>   s_devCursor{ 10 };
        Slot<ShowCursorFn>       s_devShowCursor{ 12 };

        // --- emulated resources ----------------------------------------------------------------------

        enum class Kind : uint8_t { Tex2D, Cube, Volume, Vertex, Index };

        struct OpenLock
        {
            uint32_t sub = 0;
            bool     readOnly = false;
            RECT     rect{};
            D3DLOCKED_RECT locked{};
            IDirect3DBaseTexture9* staging = nullptr;   // engine textures
            UINT     offset = 0, size = 0;              // buffers
            uint8_t* memory = nullptr;                  // engine buffers: pooled staging
        };

        struct Emu
        {
            Kind      kind = Kind::Tex2D;
            IDirect3DDevice9* dev = nullptr;
            void*     obj = nullptr;
            D3DFORMAT fmt = D3DFMT_UNKNOWN;
            UINT      w = 0, h = 0, d = 1, levels = 1;
            DWORD     usage = 0;
            UINT      length = 0;
            bool      engine = false;
            uint64_t  serial = 0;
            uint64_t  bytes = 0;
            IDirect3DBaseTexture9* mirror = nullptr;    // SYSTEMMEM twin
            std::vector<uint8_t>   mirrorBytes;         // heap twin of a non-engine buffer
            std::vector<OpenLock>  locks;
            uint32_t               bufferUnlocks = 0;   // unlocks seen while nested buffer locks are open
            std::vector<void*>     subObjects;          // surfaces and volumes handed out

            UINT Faces() const { return kind == Kind::Cube ? 6u : 1u; }
            uint32_t SubCount() const { return kind == Kind::Vertex || kind == Kind::Index ? 1u : Faces() * levels; }
            uint64_t Key(uint32_t sub) const { return (1ull << 63) | (serial << 16) | sub; }
        };

        struct SubRef
        {
            Emu*     emu;
            uint32_t face;
            uint32_t level;
        };

        std::recursive_mutex                 g_mutex;
        std::unordered_map<void*, Emu*>      g_emu;
        std::unordered_map<void*, SubRef>    g_subs;   // surfaces and volumes of emulated textures
        std::atomic<bool>                    g_ex{ false };
        std::atomic<bool>                    g_lost{ false };
        std::atomic<uint64_t>                g_serial{ 0 };
        std::atomic<uint32_t>                g_failures{ 0 };

        Emu* Find(void* obj)
        {
            if (!obj) return nullptr;
            const auto it = g_emu.find(obj);
            return it != g_emu.end() ? it->second : nullptr;
        }

        void LogFailure(const char* what, HRESULT hr)
        {
            if (g_failures.fetch_add(1) < 24) WLOG_WARN("d3d9ex: %s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
        }

        // --- format geometry -------------------------------------------------------------------------

        uint32_t BlockBytes(D3DFORMAT f)
        {
            switch (static_cast<uint32_t>(f))
            {
            case MAKEFOURCC('D', 'X', 'T', '1'): case MAKEFOURCC('A', 'T', 'I', '1'): return 8;
            case MAKEFOURCC('D', 'X', 'T', '2'): case MAKEFOURCC('D', 'X', 'T', '3'):
            case MAKEFOURCC('D', 'X', 'T', '4'): case MAKEFOURCC('D', 'X', 'T', '5'):
            case MAKEFOURCC('A', 'T', 'I', '2'): return 16;
            default: return 0;
            }
        }

        /// Bytes of one row (one block row for compressed formats) of a span of width pixels.
        uint64_t RowBytes(D3DFORMAT f, uint32_t width)
        {
            if (const uint32_t b = BlockBytes(f)) return uint64_t((width + 3) / 4) * b;
            const uint64_t levelBytes = ::wxl::gpu::resources::SurfaceBytes(f, width, 1);
            return levelBytes;
        }

        /// Rows (block rows for compressed formats) of a span of height pixels.
        uint32_t Rows(D3DFORMAT f, uint32_t height)
        {
            return BlockBytes(f) ? (height + 3) / 4 : height;
        }

        UINT LevelDim(UINT full, UINT level) { return std::max<UINT>(1u, full >> level); }

        // --- staging textures for engine uploads ------------------------------------------------------

        using StagingKey = std::tuple<uint32_t, UINT, UINT, UINT, uint8_t>;   // format, w, h, levels, kind

        /// Released staging, oldest first: the most recently written is the likeliest still read by the GPU.
        struct PooledStaging
        {
            StagingKey             key;
            IDirect3DBaseTexture9* texture;
            uint64_t               bytes;
        };
        std::deque<PooledStaging> g_staging;
        uint64_t                  g_stagingBytes = 0;

        uint64_t ChainBytes(const Emu& e)
        {
            uint64_t total = 0;
            for (UINT l = 0; l < e.levels; ++l)
                total += ::wxl::gpu::resources::SurfaceBytes(e.fmt, LevelDim(e.w, l), LevelDim(e.h, l));
            return total * e.Faces();
        }

        StagingKey KeyOf(const Emu& e)
        {
            return StagingKey(static_cast<uint32_t>(e.fmt), e.w, e.h, e.levels, static_cast<uint8_t>(e.kind));
        }

        IDirect3DBaseTexture9* CreateStaging(const Emu& e)
        {
            HRESULT hr = E_FAIL;
            if (e.kind == Kind::Cube)
            {
                IDirect3DCubeTexture9* t = nullptr;
                hr = e.dev->CreateCubeTexture(e.w, e.levels, 0, e.fmt, D3DPOOL_SYSTEMMEM, &t, nullptr);
                if (SUCCEEDED(hr)) return t;
            }
            else
            {
                IDirect3DTexture9* t = nullptr;
                hr = e.dev->CreateTexture(e.w, e.h, e.levels, 0, e.fmt, D3DPOOL_SYSTEMMEM, &t, nullptr);
                if (SUCCEEDED(hr)) return t;
            }
            LogFailure("creating a staging texture", hr);
            return nullptr;
        }

        IDirect3DBaseTexture9* AcquireStaging(const Emu& e)
        {
            const StagingKey key = KeyOf(e);
            for (auto it = g_staging.begin(); it != g_staging.end(); ++it)
            {
                if (it->key != key) continue;
                IDirect3DBaseTexture9* t = it->texture;
                g_stagingBytes -= std::min(g_stagingBytes, it->bytes);
                g_staging.erase(it);
                g_c.stagingBytes.store(g_stagingBytes, std::memory_order_relaxed);
                return t;
            }
            return CreateStaging(e);
        }

        void ReleaseStaging(const Emu& e, IDirect3DBaseTexture9* t)
        {
            if (!t) return;
            const uint64_t bytes = ChainBytes(e);
            g_staging.push_back(PooledStaging{ KeyOf(e), t, bytes });
            g_stagingBytes += bytes;
            while (g_stagingBytes > StagingCap() && !g_staging.empty())
            {
                const PooledStaging oldest = g_staging.front();
                g_staging.pop_front();
                g_stagingBytes -= std::min(g_stagingBytes, oldest.bytes);
                oldest.texture->Release();
            }
            g_c.stagingBytes.store(g_stagingBytes, std::memory_order_relaxed);
        }

        /// The staging an engine texture holds between the levels of one upload.
        struct Held
        {
            Emu* emu = nullptr;
            IDirect3DBaseTexture9* staging = nullptr;
        } g_held;

        void ReleaseHeld()
        {
            if (g_held.emu && g_held.staging) ReleaseStaging(*g_held.emu, g_held.staging);
            g_held = Held{};
        }

        // --- buffer staging -------------------------------------------------------------------------

        std::map<uint32_t, std::vector<std::vector<uint8_t>>> g_bufferPool;   // by power-of-two size class
        uint64_t g_bufferPoolBytes = 0;

        uint32_t SizeClass(uint32_t size)
        {
            uint32_t c = 256;
            while (c < size && c < 0x80000000u) c <<= 1;
            return c;
        }

        std::vector<uint8_t> AcquireBuffer(uint32_t size)
        {
            const uint32_t cls = SizeClass(size);
            auto& list = g_bufferPool[cls];
            if (!list.empty())
            {
                std::vector<uint8_t> v = std::move(list.back());
                list.pop_back();
                g_bufferPoolBytes -= cls;
                return v;
            }
            return std::vector<uint8_t>(cls);
        }

        void ReleaseBuffer(std::vector<uint8_t>&& v)
        {
            const uint32_t cls = uint32_t(v.size());
            if (g_bufferPoolBytes + cls > StagingCap() / 2) return;   // drop it: freed on return
            g_bufferPool[cls].push_back(std::move(v));
            g_bufferPoolBytes += cls;
        }

        std::unordered_map<uint8_t*, std::vector<uint8_t>> g_bufferOut;   // staging handed out, by data pointer

        // --- backing copies --------------------------------------------------------------------------

        /// Hands rows of an upload to the host. Never waits: a full window drops the copy (counted).
        void BackRows(const Emu& e, uint32_t sub, uint64_t dstOffset, uint64_t dstPitch, const void* src,
                      uint64_t rowBytes, uint64_t rows, uint64_t srcPitch)
        {
            if (!BackingEnabled()) return;
            const WXL_HostApi* host = ::wxl::gpu::hostlink::Live();
            if (host && host->StorePatchRows(e.Key(sub), dstOffset, dstPitch, src, rowBytes, rows, srcPitch) == WXL_HOST_OK)
            {
                g_c.backingSent.fetch_add(1, std::memory_order_relaxed);
                g_c.backingBytes.fetch_add(rowBytes * rows, std::memory_order_relaxed);
            }
            else
            {
                g_c.backingDropped.fetch_add(1, std::memory_order_relaxed);
            }
        }

        /// Copies a subresource's backing copy into dst; false when there is none. A copy shorter than size
        /// (a level only ever uploaded in part) is completed with zeroes.
        bool FetchBacking(const Emu& e, uint32_t sub, void* dst, uint64_t size)
        {
            const WXL_HostApi* host = ::wxl::gpu::hostlink::Live();
            if (!host) return false;
            uint64_t full = 0;
            if (host->StoreGet(e.Key(sub), dst, size, &full, 5000) != WXL_HOST_OK || !full) return false;
            if (full < size) std::memset(static_cast<uint8_t*>(dst) + full, 0, size_t(size - full));
            return true;
        }

        // --- level surfaces --------------------------------------------------------------------------

        IDirect3DSurface9* LevelSurface(IDirect3DBaseTexture9* t, Kind kind, UINT face, UINT level)
        {
            IDirect3DSurface9* s = nullptr;
            if (kind == Kind::Cube)
                static_cast<IDirect3DCubeTexture9*>(t)->GetCubeMapSurface(static_cast<D3DCUBEMAP_FACES>(face), level, &s);
            else
                static_cast<IDirect3DTexture9*>(t)->GetSurfaceLevel(level, &s);
            return s;
        }

        HRESULT LockLevel(IDirect3DBaseTexture9* t, Kind kind, UINT face, UINT level, D3DLOCKED_RECT* out,
                          const RECT* rect, DWORD flags)
        {
            if (kind == Kind::Cube)
                return static_cast<IDirect3DCubeTexture9*>(t)->LockRect(static_cast<D3DCUBEMAP_FACES>(face), level, out,
                                                                        rect, flags);
            return static_cast<IDirect3DTexture9*>(t)->LockRect(level, out, rect, flags);
        }

        HRESULT UnlockLevel(IDirect3DBaseTexture9* t, Kind kind, UINT face, UINT level)
        {
            if (kind == Kind::Cube)
                return static_cast<IDirect3DCubeTexture9*>(t)->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face), level);
            return static_cast<IDirect3DTexture9*>(t)->UnlockRect(level);
        }

        // --- texture locks ---------------------------------------------------------------------------

        OpenLock* FindLock(Emu& e, uint32_t sub)
        {
            for (OpenLock& l : e.locks)
                if (l.sub == sub) return &l;
            return nullptr;
        }

        void EraseLock(Emu& e, uint32_t sub)
        {
            for (auto it = e.locks.begin(); it != e.locks.end(); ++it)
                if (it->sub == sub) { e.locks.erase(it); return; }
        }

        HRESULT LockTexture(Emu& e, UINT face, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags)
        {
            if (!out || level >= e.levels || face >= e.Faces()) return D3DERR_INVALIDCALL;
            const uint32_t sub = face * e.levels + level;
            if (FindLock(e, sub)) return D3DERR_INVALIDCALL;   // already locked, as the runtime answers
            const bool readOnly = (flags & D3DLOCK_READONLY) != 0;
            const DWORD passFlags = flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE | D3DLOCK_NOSYSLOCK);

            OpenLock lock;
            lock.sub = sub;
            lock.readOnly = readOnly;

            if (e.mirror)
            {
                const HRESULT hr = LockLevel(e.mirror, e.kind, face, level, out, rect, passFlags);
                if (FAILED(hr)) return hr;
                lock.locked = *out;
                e.locks.push_back(lock);
                if (readOnly) g_c.readbacks.fetch_add(1, std::memory_order_relaxed);
                return D3D_OK;
            }

            // Engine texture: the rectangle is staged, aligned to whole blocks as the runtime requires.
            const UINT lw = LevelDim(e.w, level), lh = LevelDim(e.h, level);
            RECT r = rect ? *rect : RECT{ 0, 0, LONG(lw), LONG(lh) };
            r.left = std::max<LONG>(0, r.left);
            r.top = std::max<LONG>(0, r.top);
            r.right = std::min<LONG>(LONG(lw), r.right);
            r.bottom = std::min<LONG>(LONG(lh), r.bottom);
            if (BlockBytes(e.fmt))
            {
                r.left &= ~3;
                r.top &= ~3;
                r.right = std::min<LONG>(LONG(lw), (r.right + 3) & ~3);
                r.bottom = std::min<LONG>(LONG(lh), (r.bottom + 3) & ~3);
            }
            if (r.right <= r.left || r.bottom <= r.top) return D3DERR_INVALIDCALL;

            if (g_held.emu != &e) ReleaseHeld();
            if (!g_held.staging)
            {
                g_held.staging = AcquireStaging(e);
                g_held.emu = g_held.staging ? &e : nullptr;
            }
            if (!g_held.staging) return D3DERR_OUTOFVIDEOMEMORY;

            const HRESULT hr = LockLevel(g_held.staging, e.kind, face, level, out, &r, D3DLOCK_NOSYSLOCK);
            if (FAILED(hr)) { LogFailure("locking a staging texture", hr); return hr; }

            lock.rect = r;
            lock.locked = *out;
            lock.staging = g_held.staging;

            if (readOnly)
            {
                // A read: the level's backing copy, or zeroes when there is none.
                const uint64_t rowBytes = RowBytes(e.fmt, lw);
                const uint32_t rows = Rows(e.fmt, lh);
                std::vector<uint8_t> backing(size_t(rowBytes * rows));
                if (!FetchBacking(e, sub, backing.data(), backing.size())) std::fill(backing.begin(), backing.end(), uint8_t(0));
                const uint64_t x = RowBytes(e.fmt, UINT(r.left)) * (r.left ? 1 : 0);
                const uint32_t y = Rows(e.fmt, UINT(r.top)) * (r.top ? 1 : 0);
                const uint64_t spanBytes = RowBytes(e.fmt, UINT(r.right - r.left));
                const uint32_t spanRows = Rows(e.fmt, UINT(r.bottom - r.top));
                for (uint32_t row = 0; row < spanRows; ++row)
                    std::memcpy(static_cast<uint8_t*>(out->pBits) + size_t(row) * out->Pitch,
                                backing.data() + (y + row) * rowBytes + x, size_t(spanBytes));
                g_c.readbacks.fetch_add(1, std::memory_order_relaxed);
            }
            e.locks.push_back(lock);
            return D3D_OK;
        }

        HRESULT UnlockTexture(Emu& e, UINT face, UINT level)
        {
            const uint32_t sub = face * e.levels + level;
            OpenLock* found = FindLock(e, sub);
            if (!found) return D3DERR_INVALIDCALL;
            const OpenLock lock = *found;
            EraseLock(e, sub);

            if (e.mirror)
            {
                const HRESULT hr = UnlockLevel(e.mirror, e.kind, face, level);
                if (!lock.readOnly)
                {
                    const HRESULT up = e.dev->UpdateTexture(e.mirror, static_cast<IDirect3DBaseTexture9*>(e.obj));
                    if (FAILED(up)) LogFailure("UpdateTexture from a mirror", up);
                    g_c.uploads.fetch_add(1, std::memory_order_relaxed);
                }
                return hr;
            }

            const UINT lw = LevelDim(e.w, level), lh = LevelDim(e.h, level);
            const RECT& r = lock.rect;
            const uint64_t spanBytes = RowBytes(e.fmt, UINT(r.right - r.left));
            const uint32_t spanRows = Rows(e.fmt, UINT(r.bottom - r.top));
            if (!lock.readOnly)
            {
                const uint64_t levelRow = RowBytes(e.fmt, lw);
                const uint64_t x = r.left ? RowBytes(e.fmt, UINT(r.left)) : 0;
                const uint64_t y = r.top ? Rows(e.fmt, UINT(r.top)) : 0;
                BackRows(e, sub, y * levelRow + x, levelRow, lock.locked.pBits, spanBytes, spanRows, lock.locked.Pitch);
            }
            UnlockLevel(lock.staging, e.kind, face, level);
            if (lock.readOnly) return D3D_OK;

            IDirect3DSurface9* src = LevelSurface(lock.staging, e.kind, face, level);
            IDirect3DSurface9* dst = LevelSurface(static_cast<IDirect3DBaseTexture9*>(e.obj), e.kind, face, level);
            HRESULT hr = D3DERR_INVALIDCALL;
            if (src && dst)
            {
                const bool full = r.left == 0 && r.top == 0 && UINT(r.right) == lw && UINT(r.bottom) == lh;
                const POINT at{ r.left, r.top };
                hr = e.dev->UpdateSurface(src, full ? nullptr : &r, dst, full ? nullptr : &at);
                if (FAILED(hr))
                {
                    LogFailure("UpdateSurface from staging", hr);
                    D3DSURFACE_DESC sd{}, dd{};
                    src->GetDesc(&sd);
                    dst->GetDesc(&dd);
                    if (g_failures.load() < 24)
                        WLOG_WARN("d3d9ex:   src pool %u usage 0x%lX fmt 0x%X %ux%u | dst usage 0x%lX fmt 0x%X %ux%u | rect "
                                  "%ld,%ld-%ld,%ld full %d level %u", unsigned(sd.Pool), sd.Usage, unsigned(sd.Format),
                                  sd.Width, sd.Height, dd.Usage, unsigned(dd.Format), dd.Width, dd.Height, r.left, r.top,
                                  r.right, r.bottom, int(full), level);
                }
                else if (e.usage & D3DUSAGE_AUTOGENMIPMAP) static_cast<IDirect3DBaseTexture9*>(e.obj)->GenerateMipSubLevels();
            }
            if (src) src->Release();
            if (dst) dst->Release();
            g_c.uploads.fetch_add(1, std::memory_order_relaxed);
            g_c.uploadBytes.fetch_add(spanBytes * spanRows, std::memory_order_relaxed);
            return hr;
        }

        // --- volume locks (always mirrored) ------------------------------------------------------------

        HRESULT LockVolume(Emu& e, UINT level, D3DLOCKED_BOX* out, const D3DBOX* box, DWORD flags)
        {
            if (!e.mirror || level >= e.levels || FindLock(e, level)) return D3DERR_INVALIDCALL;
            const DWORD passFlags = flags & (D3DLOCK_READONLY | D3DLOCK_NO_DIRTY_UPDATE | D3DLOCK_NOSYSLOCK);
            const HRESULT hr = static_cast<IDirect3DVolumeTexture9*>(e.mirror)->LockBox(level, out, box, passFlags);
            if (FAILED(hr)) return hr;
            OpenLock lock;
            lock.sub = level;
            lock.readOnly = (flags & D3DLOCK_READONLY) != 0;
            e.locks.push_back(lock);
            return D3D_OK;
        }

        HRESULT UnlockVolume(Emu& e, UINT level)
        {
            OpenLock* found = FindLock(e, level);
            if (!found) return D3DERR_INVALIDCALL;
            const bool readOnly = found->readOnly;
            EraseLock(e, level);
            const HRESULT hr = static_cast<IDirect3DVolumeTexture9*>(e.mirror)->UnlockBox(level);
            if (!readOnly)
            {
                const HRESULT up = e.dev->UpdateTexture(e.mirror, static_cast<IDirect3DBaseTexture9*>(e.obj));
                if (FAILED(up)) LogFailure("UpdateTexture from a volume mirror", up);
                g_c.uploads.fetch_add(1, std::memory_order_relaxed);
            }
            return hr;
        }

        // --- buffer locks ----------------------------------------------------------------------------

        template <class Buffer>
        HRESULT LockBuffer(Emu& e, Buffer* b, UINT offset, UINT size, void** data, DWORD flags)
        {
            if (!data) return D3DERR_INVALIDCALL;
            if (size == 0) size = offset < e.length ? e.length - offset : 0;
            if (offset > e.length || size > e.length - offset) return D3DERR_INVALIDCALL;
            OpenLock lock;
            lock.offset = offset;
            lock.size = size;
            lock.readOnly = (flags & D3DLOCK_READONLY) != 0;
            if (!e.mirrorBytes.empty())
            {
                *data = e.mirrorBytes.data() + offset;
            }
            else
            {
                std::vector<uint8_t> staging = AcquireBuffer(std::max<UINT>(size, 1));
                lock.memory = staging.data();
                if (lock.readOnly)
                {
                    std::vector<uint8_t> whole(e.length);
                    if (FetchBacking(e, 0, whole.data(), whole.size())) std::memcpy(lock.memory, whole.data() + offset, size);
                    else std::memset(lock.memory, 0, size);
                    g_c.readbacks.fetch_add(1, std::memory_order_relaxed);
                }
                g_bufferOut.emplace(lock.memory, std::move(staging));
                *data = lock.memory;
            }
            (void)b;
            e.locks.push_back(lock);
            return D3D_OK;
        }

        /// Writes every open lock through once the last one is released: an unlock names no range, and
        /// nested locks may be released in any order.
        template <class Buffer, class LockFnT, class UnlockFnT>
        HRESULT UnlockBuffer(Emu& e, Buffer* b, LockFnT realLock, UnlockFnT realUnlock)
        {
            if (e.locks.empty()) return D3DERR_INVALIDCALL;
            if (++e.bufferUnlocks < e.locks.size()) return D3D_OK;
            e.bufferUnlocks = 0;
            const std::vector<OpenLock> locks = std::move(e.locks);
            e.locks.clear();

            HRESULT hr = D3D_OK;
            for (const OpenLock& lock : locks)
            {
                const uint8_t* src = lock.memory ? lock.memory : e.mirrorBytes.data() + lock.offset;
                if (!lock.readOnly && lock.size)
                {
                    void* p = nullptr;
                    const HRESULT r = realLock(b, lock.offset, lock.size, &p, 0);
                    if (SUCCEEDED(r) && p)
                    {
                        std::memcpy(p, src, lock.size);
                        realUnlock(b);
                    }
                    else
                    {
                        hr = r;
                        LogFailure("writing through a buffer", r);
                    }
                    if (lock.memory) BackRows(e, 0, lock.offset, lock.size, src, lock.size, 1, lock.size);
                    g_c.uploads.fetch_add(1, std::memory_order_relaxed);
                    g_c.uploadBytes.fetch_add(lock.size, std::memory_order_relaxed);
                }
                if (lock.memory)
                {
                    const auto it = g_bufferOut.find(lock.memory);
                    if (it != g_bufferOut.end())
                    {
                        ReleaseBuffer(std::move(it->second));
                        g_bufferOut.erase(it);
                    }
                }
            }
            return hr;
        }

        // --- hooks: textures -------------------------------------------------------------------------

        void RecordSub(Emu* e, void* sub, uint32_t face, uint32_t level)
        {
            if (!sub) return;
            g_subs[sub] = SubRef{ e, face, level };
            if (std::find(e->subObjects.begin(), e->subObjects.end(), sub) == e->subObjects.end())
                e->subObjects.push_back(sub);
        }

        void STDMETHODCALLTYPE hkTexPreLoad(IDirect3DResource9* self)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (g_held.emu && g_held.emu->obj == self) ReleaseHeld();   // the engine's upload is over
            }
            PreLoadFn orig = s_texPreLoad.Orig(self);
            if (!orig) orig = s_cubePreLoad.Orig(self);   // one hook serves both texture classes
            if (orig) orig(self);
        }

        HRESULT STDMETHODCALLTYPE hkTexGetLevelDesc(IDirect3DTexture9* self, UINT level, D3DSURFACE_DESC* desc)
        {
            const bool managedCaller = ReportManagedTo(_ReturnAddress());
            const HRESULT hr = s_texDesc.Orig(self)(self, level, desc);
            if (SUCCEEDED(hr) && desc)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (managedCaller && Find(self)) desc->Pool = D3DPOOL_MANAGED;
            }
            return hr;
        }

        void HookSurfaceClass(IDirect3DSurface9* s);

        HRESULT STDMETHODCALLTYPE hkTexGetSurface(IDirect3DTexture9* self, UINT level, IDirect3DSurface9** out)
        {
            const HRESULT hr = s_texSurface.Orig(self)(self, level, out);
            if (SUCCEEDED(hr) && out && *out)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self))
                {
                    HookSurfaceClass(*out);
                    RecordSub(e, *out, 0, level);
                }
                else
                {
                    g_subs.erase(*out);   // a surface reusing a dead one's address
                }
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkTexLock(IDirect3DTexture9* self, UINT level, D3DLOCKED_RECT* out, const RECT* rect,
                                            DWORD flags)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self)) return LockTexture(*e, 0, level, out, rect, flags);
            }
            return s_texLock.Orig(self)(self, level, out, rect, flags);
        }

        HRESULT STDMETHODCALLTYPE hkTexUnlock(IDirect3DTexture9* self, UINT level)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self)) return UnlockTexture(*e, 0, level);
            }
            return s_texUnlock.Orig(self)(self, level);
        }

        HRESULT STDMETHODCALLTYPE hkTexDirty(IDirect3DTexture9* self, const RECT* rect)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self))
                {
                    if (!e->mirror) return D3D_OK;
                    static_cast<IDirect3DTexture9*>(e->mirror)->AddDirtyRect(rect);
                    return e->dev->UpdateTexture(e->mirror, static_cast<IDirect3DBaseTexture9*>(e->obj));
                }
            }
            return s_texDirty.Orig(self)(self, rect);
        }

        HRESULT STDMETHODCALLTYPE hkCubeGetLevelDesc(IDirect3DCubeTexture9* self, UINT level, D3DSURFACE_DESC* desc)
        {
            const bool managedCaller = ReportManagedTo(_ReturnAddress());
            const HRESULT hr = s_cubeDesc.Orig(self)(self, level, desc);
            if (SUCCEEDED(hr) && desc)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (managedCaller && Find(self)) desc->Pool = D3DPOOL_MANAGED;
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCubeGetSurface(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face, UINT level,
                                                   IDirect3DSurface9** out)
        {
            const HRESULT hr = s_cubeSurface.Orig(self)(self, face, level, out);
            if (SUCCEEDED(hr) && out && *out)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self))
                {
                    HookSurfaceClass(*out);
                    RecordSub(e, *out, uint32_t(face), level);
                }
                else
                {
                    g_subs.erase(*out);
                }
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCubeLock(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face, UINT level,
                                             D3DLOCKED_RECT* out, const RECT* rect, DWORD flags)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self)) return LockTexture(*e, UINT(face), level, out, rect, flags);
            }
            return s_cubeLock.Orig(self)(self, face, level, out, rect, flags);
        }

        HRESULT STDMETHODCALLTYPE hkCubeUnlock(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face, UINT level)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self)) return UnlockTexture(*e, UINT(face), level);
            }
            return s_cubeUnlock.Orig(self)(self, face, level);
        }

        HRESULT STDMETHODCALLTYPE hkCubeDirty(IDirect3DCubeTexture9* self, D3DCUBEMAP_FACES face, const RECT* rect)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self))
                {
                    if (!e->mirror) return D3D_OK;
                    static_cast<IDirect3DCubeTexture9*>(e->mirror)->AddDirtyRect(face, rect);
                    return e->dev->UpdateTexture(e->mirror, static_cast<IDirect3DBaseTexture9*>(e->obj));
                }
            }
            return s_cubeDirty.Orig(self)(self, face, rect);
        }

        HRESULT STDMETHODCALLTYPE hkVolGetLevelDesc(IDirect3DVolumeTexture9* self, UINT level, D3DVOLUME_DESC* desc)
        {
            const bool managedCaller = ReportManagedTo(_ReturnAddress());
            const HRESULT hr = s_volDesc.Orig(self)(self, level, desc);
            if (SUCCEEDED(hr) && desc)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (managedCaller && Find(self)) desc->Pool = D3DPOOL_MANAGED;
            }
            return hr;
        }

        void HookVolumeClass(IDirect3DVolume9* v);

        HRESULT STDMETHODCALLTYPE hkVolGetLevel(IDirect3DVolumeTexture9* self, UINT level, IDirect3DVolume9** out)
        {
            const HRESULT hr = s_volLevel.Orig(self)(self, level, out);
            if (SUCCEEDED(hr) && out && *out)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self))
                {
                    HookVolumeClass(*out);
                    RecordSub(e, *out, 0, level);
                }
                else
                {
                    g_subs.erase(*out);
                }
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkVolLock(IDirect3DVolumeTexture9* self, UINT level, D3DLOCKED_BOX* out,
                                            const D3DBOX* box, DWORD flags)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self)) return LockVolume(*e, level, out, box, flags);
            }
            return s_volLock.Orig(self)(self, level, out, box, flags);
        }

        HRESULT STDMETHODCALLTYPE hkVolUnlock(IDirect3DVolumeTexture9* self, UINT level)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self)) return UnlockVolume(*e, level);
            }
            return s_volUnlock.Orig(self)(self, level);
        }

        HRESULT STDMETHODCALLTYPE hkVolDirty(IDirect3DVolumeTexture9* self, const D3DBOX* box)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self))
                {
                    static_cast<IDirect3DVolumeTexture9*>(e->mirror)->AddDirtyBox(box);
                    return e->dev->UpdateTexture(e->mirror, static_cast<IDirect3DBaseTexture9*>(e->obj));
                }
            }
            return s_volDirty.Orig(self)(self, box);
        }

        // --- hooks: surfaces and volumes of emulated textures ----------------------------------------

        HRESULT STDMETHODCALLTYPE hkSurfGetDesc(IDirect3DSurface9* self, D3DSURFACE_DESC* desc)
        {
            const bool managedCaller = ReportManagedTo(_ReturnAddress());
            const HRESULT hr = s_surfDesc.Orig(self)(self, desc);
            if (SUCCEEDED(hr) && desc)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (managedCaller && g_subs.count(self)) desc->Pool = D3DPOOL_MANAGED;
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkSurfLock(IDirect3DSurface9* self, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                const auto it = g_subs.find(self);
                if (it != g_subs.end())
                    return LockTexture(*it->second.emu, it->second.face, it->second.level, out, rect, flags);
            }
            return s_surfLock.Orig(self)(self, out, rect, flags);
        }

        HRESULT STDMETHODCALLTYPE hkSurfUnlock(IDirect3DSurface9* self)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                const auto it = g_subs.find(self);
                if (it != g_subs.end()) return UnlockTexture(*it->second.emu, it->second.face, it->second.level);
            }
            return s_surfUnlock.Orig(self)(self);
        }

        HRESULT STDMETHODCALLTYPE hkVolumeGetDesc(IDirect3DVolume9* self, D3DVOLUME_DESC* desc)
        {
            const bool managedCaller = ReportManagedTo(_ReturnAddress());
            const HRESULT hr = s_volumeDesc.Orig(self)(self, desc);
            if (SUCCEEDED(hr) && desc)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (managedCaller && g_subs.count(self)) desc->Pool = D3DPOOL_MANAGED;
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkVolumeLock(IDirect3DVolume9* self, D3DLOCKED_BOX* out, const D3DBOX* box, DWORD flags)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                const auto it = g_subs.find(self);
                if (it != g_subs.end()) return LockVolume(*it->second.emu, it->second.level, out, box, flags);
            }
            return s_volumeLock.Orig(self)(self, out, box, flags);
        }

        HRESULT STDMETHODCALLTYPE hkVolumeUnlock(IDirect3DVolume9* self)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                const auto it = g_subs.find(self);
                if (it != g_subs.end()) return UnlockVolume(*it->second.emu, it->second.level);
            }
            return s_volumeUnlock.Orig(self)(self);
        }

        void HookSurfaceClass(IDirect3DSurface9* s)
        {
            s_surfDesc.Hook(s, &hkSurfGetDesc);
            s_surfLock.Hook(s, &hkSurfLock);
            s_surfUnlock.Hook(s, &hkSurfUnlock);
        }

        void HookVolumeClass(IDirect3DVolume9* v)
        {
            s_volumeDesc.Hook(v, &hkVolumeGetDesc);
            s_volumeLock.Hook(v, &hkVolumeLock);
            s_volumeUnlock.Hook(v, &hkVolumeUnlock);
        }

        // --- hooks: buffers --------------------------------------------------------------------------

        HRESULT STDMETHODCALLTYPE hkVbLock(IDirect3DVertexBuffer9* self, UINT offset, UINT size, void** data, DWORD flags)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self)) return LockBuffer(*e, self, offset, size, data, flags);
            }
            return s_vbLock.Orig(self)(self, offset, size, data, flags);
        }

        HRESULT STDMETHODCALLTYPE hkVbUnlock(IDirect3DVertexBuffer9* self)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self))
                    return UnlockBuffer(*e, self, s_vbLock.Orig(self), s_vbUnlock.Orig(self));
            }
            return s_vbUnlock.Orig(self)(self);
        }

        HRESULT STDMETHODCALLTYPE hkVbGetDesc(IDirect3DVertexBuffer9* self, D3DVERTEXBUFFER_DESC* desc)
        {
            const bool managedCaller = ReportManagedTo(_ReturnAddress());
            const HRESULT hr = s_vbDesc.Orig(self)(self, desc);
            if (SUCCEEDED(hr) && desc)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (managedCaller && Find(self)) desc->Pool = D3DPOOL_MANAGED;
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkIbLock(IDirect3DIndexBuffer9* self, UINT offset, UINT size, void** data, DWORD flags)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self)) return LockBuffer(*e, self, offset, size, data, flags);
            }
            return s_ibLock.Orig(self)(self, offset, size, data, flags);
        }

        HRESULT STDMETHODCALLTYPE hkIbUnlock(IDirect3DIndexBuffer9* self)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (Emu* e = Find(self))
                    return UnlockBuffer(*e, self, s_ibLock.Orig(self), s_ibUnlock.Orig(self));
            }
            return s_ibUnlock.Orig(self)(self);
        }

        HRESULT STDMETHODCALLTYPE hkIbGetDesc(IDirect3DIndexBuffer9* self, D3DINDEXBUFFER_DESC* desc)
        {
            const bool managedCaller = ReportManagedTo(_ReturnAddress());
            const HRESULT hr = s_ibDesc.Orig(self)(self, desc);
            if (SUCCEEDED(hr) && desc)
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                if (managedCaller && Find(self)) desc->Pool = D3DPOOL_MANAGED;
            }
            return hr;
        }

        // --- promotion to a mirror --------------------------------------------------------------------

        bool CreateMirror(Emu& e);

        /// Reads one level of an emulated texture back from the GPU into dst (a SYSTEMMEM level surface).
        bool ReadBackLevel(Emu& e, UINT face, UINT level, IDirect3DSurface9* dst)
        {
            if (BlockBytes(e.fmt)) return false;   // StretchRect cannot read a compressed surface
            const UINT lw = LevelDim(e.w, level), lh = LevelDim(e.h, level);
            IDirect3DSurface9* src = LevelSurface(static_cast<IDirect3DBaseTexture9*>(e.obj), e.kind, face, level);
            IDirect3DSurface9* rt = nullptr;
            bool ok = src && SUCCEEDED(e.dev->CreateRenderTarget(lw, lh, e.fmt, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr))
                   && SUCCEEDED(e.dev->StretchRect(src, nullptr, rt, nullptr, D3DTEXF_NONE))
                   && SUCCEEDED(e.dev->GetRenderTargetData(rt, dst));
            if (rt) rt->Release();
            if (src) src->Release();
            return ok;
        }

        /// Fills one mirror level from the backing copy in the host.
        bool FillLevelFromBacking(Emu& e, UINT face, UINT level, IDirect3DSurface9* dst)
        {
            const UINT lw = LevelDim(e.w, level), lh = LevelDim(e.h, level);
            const uint64_t rowBytes = RowBytes(e.fmt, lw);
            const uint32_t rows = Rows(e.fmt, lh);
            std::vector<uint8_t> bytes(size_t(rowBytes * rows));
            if (!FetchBacking(e, face * e.levels + level, bytes.data(), bytes.size())) return false;
            D3DLOCKED_RECT lr{};
            if (FAILED(dst->LockRect(&lr, nullptr, D3DLOCK_NO_DIRTY_UPDATE))) return false;
            for (uint32_t r = 0; r < rows; ++r)
                std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(r) * lr.Pitch, bytes.data() + r * rowBytes,
                            size_t(rowBytes));
            dst->UnlockRect();
            return true;
        }

        /**
         * @brief Gives an engine texture a SYSTEMMEM mirror holding its current contents, for an API that reads
         *        the texture on the CPU. From then on its locks take the mirror path.
         * @return false when the mirror could not be created.
         */
        bool Promote(Emu& e, const char* why)
        {
            if (e.mirror) return true;
            if (e.kind == Kind::Vertex || e.kind == Kind::Index || !e.locks.empty()) return false;
            if (g_held.emu == &e) ReleaseHeld();
            if (!CreateMirror(e)) return false;
            uint32_t fromGpu = 0, fromBacking = 0, empty = 0;
            for (UINT f = 0; f < e.Faces(); ++f)
                for (UINT l = 0; l < e.levels; ++l)
                {
                    IDirect3DSurface9* dst = LevelSurface(e.mirror, e.kind, f, l);
                    if (!dst) { ++empty; continue; }
                    if (ReadBackLevel(e, f, l, dst)) ++fromGpu;
                    else if (FillLevelFromBacking(e, f, l, dst)) ++fromBacking;
                    else ++empty;
                    dst->Release();
                }
            WLOG_INFO("d3d9ex: a %ux%u %s texture (format 0x%X, %u level(s)) %s: it keeps a system-memory copy from now "
                      "on (%u level(s) read back from the GPU, %u from its backing copy, %u empty)",
                      e.w, e.h, e.engine ? "engine" : "extension", unsigned(e.fmt), e.levels, why, fromGpu, fromBacking,
                      empty);
            ::wxl::log::Flush();
            return true;
        }

        // --- hooks: the hardware cursor ---------------------------------------------------------------
        // CGxDeviceD3d::ICursorCreate makes a 32x32 A8R8G8B8 MANAGED texture; ICursorDraw writes its level-0
        // surface and passes it to SetCursorProperties, which copies it on the CPU and refuses a DEFAULT surface.

        HRESULT STDMETHODCALLTYPE hkSetCursorProperties(IDirect3DDevice9* self, UINT x, UINT y, IDirect3DSurface9* surface)
        {
            void* caller = _ReturnAddress();
            IDirect3DSurface9* substitute = nullptr;
            bool emulated = false;
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                const auto it = surface ? g_subs.find(surface) : g_subs.end();
                if (it != g_subs.end())
                {
                    emulated = true;
                    Emu& e = *it->second.emu;
                    if (Promote(e, "is the hardware cursor") && e.mirror)
                        substitute = LevelSurface(e.mirror, e.kind, it->second.face, it->second.level);
                }
            }
            const HRESULT hr = s_devCursor.Orig(self)(self, x, y, substitute ? substitute : surface);
            if (substitute) substitute->Release();

            static std::atomic<uint32_t> logged{ 0 };
            if (FAILED(hr) || logged.fetch_add(1) < 2)
            {
                if (FAILED(hr)) LogFailure("SetCursorProperties", hr);
                WLOG_INFO("d3d9ex: cursor: SetCursorProperties from %p (hot spot %u,%u) with %s -> 0x%08lX", caller, x, y,
                          !emulated ? "a native surface"
                                    : substitute ? "the system-memory copy of an emulated texture"
                                                 : "an emulated texture without a copy",
                          static_cast<unsigned long>(hr));
                ::wxl::log::Flush();
            }
            return hr;
        }

        BOOL STDMETHODCALLTYPE hkShowCursor(IDirect3DDevice9* self, BOOL show)
        {
            const BOOL previous = s_devShowCursor.Orig(self)(self, show);
            static std::atomic<uint32_t> logged{ 0 };
            if (logged.fetch_add(1) < 2)
                WLOG_INFO("d3d9ex: cursor: ShowCursor(%d) from %p, was %d", show, _ReturnAddress(), previous);
            return previous;
        }

        // --- device loss -----------------------------------------------------------------------------

        /// Refills every emulated resource after a reset that followed a device removal.
        void RestoreAll()
        {
            std::lock_guard<std::recursive_mutex> lock(g_mutex);
            ReleaseHeld();
            size_t restored = 0, missing = 0;
            for (auto& [obj, e] : g_emu)
            {
                if (e->kind == Kind::Vertex || e->kind == Kind::Index)
                {
                    std::vector<uint8_t> whole;
                    const uint8_t* src = nullptr;
                    if (!e->mirrorBytes.empty()) src = e->mirrorBytes.data();
                    else
                    {
                        whole.resize(e->length);
                        if (FetchBacking(*e, 0, whole.data(), whole.size())) src = whole.data();
                    }
                    if (!src) { ++missing; continue; }
                    void* p = nullptr;
                    HRESULT hr = e->kind == Kind::Vertex
                        ? s_vbLock.Orig(obj)(static_cast<IDirect3DVertexBuffer9*>(obj), 0, e->length, &p, 0)
                        : s_ibLock.Orig(obj)(static_cast<IDirect3DIndexBuffer9*>(obj), 0, e->length, &p, 0);
                    if (SUCCEEDED(hr) && p)
                    {
                        std::memcpy(p, src, e->length);
                        if (e->kind == Kind::Vertex) s_vbUnlock.Orig(obj)(static_cast<IDirect3DVertexBuffer9*>(obj));
                        else s_ibUnlock.Orig(obj)(static_cast<IDirect3DIndexBuffer9*>(obj));
                        ++restored;
                    }
                    continue;
                }
                if (e->mirror)
                {
                    if (e->kind == Kind::Volume) static_cast<IDirect3DVolumeTexture9*>(e->mirror)->AddDirtyBox(nullptr);
                    else if (e->kind == Kind::Cube)
                        for (UINT f = 0; f < 6; ++f)
                            static_cast<IDirect3DCubeTexture9*>(e->mirror)->AddDirtyRect(D3DCUBEMAP_FACES(f), nullptr);
                    else static_cast<IDirect3DTexture9*>(e->mirror)->AddDirtyRect(nullptr);
                    if (SUCCEEDED(e->dev->UpdateTexture(e->mirror, static_cast<IDirect3DBaseTexture9*>(obj)))) ++restored;
                    continue;
                }
                // Engine texture: each level from its backing copy, through staging.
                IDirect3DBaseTexture9* staging = AcquireStaging(*e);
                if (!staging) { ++missing; continue; }
                bool all = true;
                for (UINT f = 0; f < e->Faces(); ++f)
                    for (UINT l = 0; l < e->levels; ++l)
                    {
                        const UINT lw = LevelDim(e->w, l), lh = LevelDim(e->h, l);
                        const uint64_t rowBytes = RowBytes(e->fmt, lw);
                        const uint32_t rows = Rows(e->fmt, lh);
                        std::vector<uint8_t> level(size_t(rowBytes * rows));
                        if (!FetchBacking(*e, f * e->levels + l, level.data(), level.size())) { all = false; continue; }
                        D3DLOCKED_RECT lr{};
                        if (FAILED(LockLevel(staging, e->kind, f, l, &lr, nullptr, 0))) { all = false; continue; }
                        for (uint32_t r = 0; r < rows; ++r)
                            std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(r) * lr.Pitch, level.data() + r * rowBytes,
                                        size_t(rowBytes));
                        UnlockLevel(staging, e->kind, f, l);
                        IDirect3DSurface9* src = LevelSurface(staging, e->kind, f, l);
                        IDirect3DSurface9* dst = LevelSurface(static_cast<IDirect3DBaseTexture9*>(obj), e->kind, f, l);
                        if (src && dst) e->dev->UpdateSurface(src, nullptr, dst, nullptr);
                        if (src) src->Release();
                        if (dst) dst->Release();
                    }
                ReleaseStaging(*e, staging);
                if (all) ++restored;
                else ++missing;
            }
            g_c.restores.fetch_add(restored, std::memory_order_relaxed);
            WLOG_WARN("d3d9ex: after the device loss, %zu resources refilled, %zu with no backing copy (they stay "
                      "blank until the engine reloads them)", restored, missing);
        }

        HRESULT STDMETHODCALLTYPE hkPresent(IDirect3DDevice9* self, const RECT* src, const RECT* dst, HWND wnd,
                                            const RGNDATA* dirty)
        {
            const HRESULT hr = s_devPresent.Orig(self)(self, src, dst, wnd, dirty);
            if (hr == D3DERR_DEVICEHUNG || hr == D3DERR_DEVICEREMOVED)
            {
                if (!g_lost.exchange(true))
                    WLOG_ERROR("d3d9ex: the device was %s (0x%08lX); the engine is told the device is lost",
                               hr == D3DERR_DEVICEHUNG ? "hung" : "removed", static_cast<unsigned long>(hr));
                return D3DERR_DEVICELOST;
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkTestCoop(IDirect3DDevice9* self)
        {
            // D3D9Ex no longer reports a lost device here; the engine resets only on this answer.
            if (g_lost.load()) return D3DERR_DEVICENOTRESET;
            return s_devTestCoop.Orig(self)(self);
        }

        HRESULT STDMETHODCALLTYPE hkReset(IDirect3DDevice9* self, D3DPRESENT_PARAMETERS* params)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(g_mutex);
                ReleaseHeld();
            }
            const HRESULT hr = s_devReset.Orig(self)(self, params);
            if (SUCCEEDED(hr) && g_lost.exchange(false))
            {
                WLOG_WARN("d3d9ex: the device reset after a loss; refilling emulated resources");
                RestoreAll();
            }
            else if (FAILED(hr) && g_lost.load())
            {
                LogFailure("Reset after a device loss", hr);
            }
            return hr;
        }

        // --- adoption --------------------------------------------------------------------------------

        Emu* NewEmu(Kind kind, IDirect3DDevice9* dev, void* obj, void* caller, uint64_t bytes)
        {
            auto* e = new Emu();
            e->kind = kind;
            e->dev = dev;
            e->obj = obj;
            e->engine = IsEngine(caller);
            e->serial = g_serial.fetch_add(1) + 1;
            e->bytes = bytes;
            return e;
        }

        void Insert(Emu* e)
        {
            const auto it = g_emu.find(e->obj);
            if (it != g_emu.end()) delete it->second;   // a dead object's entry at a reused address
            g_emu[e->obj] = e;
            g_c.objects.fetch_add(1, std::memory_order_relaxed);
            g_c.bytes.fetch_add(e->bytes, std::memory_order_relaxed);
        }

        /// Creates the SYSTEMMEM twin of a texture that keeps one.
        bool CreateMirror(Emu& e)
        {
            HRESULT hr = E_FAIL;
            if (e.kind == Kind::Tex2D)
            {
                IDirect3DTexture9* t = nullptr;
                hr = e.dev->CreateTexture(e.w, e.h, e.levels, 0, e.fmt, D3DPOOL_SYSTEMMEM, &t, nullptr);
                e.mirror = t;
            }
            else if (e.kind == Kind::Cube)
            {
                IDirect3DCubeTexture9* t = nullptr;
                hr = e.dev->CreateCubeTexture(e.w, e.levels, 0, e.fmt, D3DPOOL_SYSTEMMEM, &t, nullptr);
                e.mirror = t;
            }
            else
            {
                IDirect3DVolumeTexture9* t = nullptr;
                hr = e.dev->CreateVolumeTexture(e.w, e.h, e.d, e.levels, 0, e.fmt, D3DPOOL_SYSTEMMEM, &t, nullptr);
                e.mirror = t;
            }
            if (FAILED(hr) || !e.mirror)
            {
                LogFailure("creating a mirror texture", hr);
                e.mirror = nullptr;
                return false;
            }
            g_c.mirrorBytes.fetch_add(e.bytes, std::memory_order_relaxed);
            return true;
        }
    }

    // ==============================================================================================

    Plan PlanTexture(void*, DWORD usage, D3DFORMAT, D3DPOOL pool)
    {
        if (!g_ex.load() || pool != D3DPOOL_MANAGED) return Plan{ pool, usage, false };
        return Plan{ D3DPOOL_DEFAULT, usage, true };
    }

    Plan PlanBuffer(void*, DWORD usage, D3DPOOL pool)
    {
        if (!g_ex.load() || pool != D3DPOOL_MANAGED) return Plan{ pool, usage, false };
        return Plan{ D3DPOOL_DEFAULT, usage, true };
    }

    void AdoptTexture(IDirect3DDevice9* device, IDirect3DTexture9* texture, void* caller, uint64_t bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        Emu* e = NewEmu(Kind::Tex2D, device, texture, caller, bytes);
        D3DSURFACE_DESC desc{};
        s_texDesc.Hook(texture, &hkTexGetLevelDesc);
        texture->GetLevelDesc(0, &desc);
        e->fmt = desc.Format;
        e->w = desc.Width;
        e->h = desc.Height;
        e->usage = desc.Usage;
        e->levels = texture->GetLevelCount();
        s_texPreLoad.Hook(texture, &hkTexPreLoad);
        s_texSurface.Hook(texture, &hkTexGetSurface);
        s_texLock.Hook(texture, &hkTexLock);
        s_texUnlock.Hook(texture, &hkTexUnlock);
        s_texDirty.Hook(texture, &hkTexDirty);
        if (!e->engine) CreateMirror(*e);
        Insert(e);
    }

    void AdoptCube(IDirect3DDevice9* device, IDirect3DCubeTexture9* texture, void* caller, uint64_t bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        Emu* e = NewEmu(Kind::Cube, device, texture, caller, bytes);
        D3DSURFACE_DESC desc{};
        s_cubeDesc.Hook(texture, &hkCubeGetLevelDesc);
        texture->GetLevelDesc(0, &desc);
        e->fmt = desc.Format;
        e->w = desc.Width;
        e->h = desc.Height;
        e->usage = desc.Usage;
        e->levels = texture->GetLevelCount();
        s_cubePreLoad.Hook(texture, &hkTexPreLoad);
        s_cubeSurface.Hook(texture, &hkCubeGetSurface);
        s_cubeLock.Hook(texture, &hkCubeLock);
        s_cubeUnlock.Hook(texture, &hkCubeUnlock);
        s_cubeDirty.Hook(texture, &hkCubeDirty);
        if (!e->engine) CreateMirror(*e);
        Insert(e);
    }

    void AdoptVolume(IDirect3DDevice9* device, IDirect3DVolumeTexture9* texture, void* caller, uint64_t bytes)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        Emu* e = NewEmu(Kind::Volume, device, texture, caller, bytes);
        D3DVOLUME_DESC desc{};
        s_volDesc.Hook(texture, &hkVolGetLevelDesc);
        texture->GetLevelDesc(0, &desc);
        e->fmt = desc.Format;
        e->w = desc.Width;
        e->h = desc.Height;
        e->d = desc.Depth;
        e->usage = desc.Usage;
        e->levels = texture->GetLevelCount();
        s_volLevel.Hook(texture, &hkVolGetLevel);
        s_volLock.Hook(texture, &hkVolLock);
        s_volUnlock.Hook(texture, &hkVolUnlock);
        s_volDirty.Hook(texture, &hkVolDirty);
        CreateMirror(*e);   // volumes always keep one: no UpdateSurface for a volume level
        Insert(e);
    }

    void AdoptVertexBuffer(IDirect3DDevice9* device, IDirect3DVertexBuffer9* buffer, void* caller, uint32_t length,
                           DWORD usage)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        Emu* e = NewEmu(Kind::Vertex, device, buffer, caller, length);
        e->length = length;
        e->usage = usage;
        s_vbLock.Hook(buffer, &hkVbLock);
        s_vbUnlock.Hook(buffer, &hkVbUnlock);
        s_vbDesc.Hook(buffer, &hkVbGetDesc);
        if (!e->engine)
        {
            e->mirrorBytes.assign(length, 0);
            g_c.mirrorBytes.fetch_add(length, std::memory_order_relaxed);
        }
        Insert(e);
    }

    void AdoptIndexBuffer(IDirect3DDevice9* device, IDirect3DIndexBuffer9* buffer, void* caller, uint32_t length,
                          DWORD usage)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        Emu* e = NewEmu(Kind::Index, device, buffer, caller, length);
        e->length = length;
        e->usage = usage;
        s_ibLock.Hook(buffer, &hkIbLock);
        s_ibUnlock.Hook(buffer, &hkIbUnlock);
        s_ibDesc.Hook(buffer, &hkIbGetDesc);
        if (!e->engine)
        {
            e->mirrorBytes.assign(length, 0);
            g_c.mirrorBytes.fetch_add(length, std::memory_order_relaxed);
        }
        Insert(e);
    }

    void OnCreated(void* object)
    {
        // Whatever is still recorded at this address belongs to an object that died unseen.
        OnDestroyed(object);
    }

    void OnDestroyed(void* object)
    {
        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        g_subs.erase(object);
        const auto it = g_emu.find(object);
        if (it == g_emu.end()) return;
        Emu* e = it->second;
        g_emu.erase(it);
        if (g_held.emu == e) ReleaseHeld();
        for (void* sub : e->subObjects) g_subs.erase(sub);
        for (const OpenLock& l : e->locks)
        {
            if (!l.memory) continue;
            const auto out = g_bufferOut.find(l.memory);
            if (out != g_bufferOut.end()) g_bufferOut.erase(out);
        }
        if (e->mirror)
        {
            e->mirror->Release();
            g_c.mirrorBytes.fetch_sub(std::min(g_c.mirrorBytes.load(), e->bytes), std::memory_order_relaxed);
        }
        if (!e->mirrorBytes.empty())
            g_c.mirrorBytes.fetch_sub(std::min<uint64_t>(g_c.mirrorBytes.load(), e->mirrorBytes.size()),
                                      std::memory_order_relaxed);
        if (e->engine && BackingEnabled())
            if (const WXL_HostApi* host = ::wxl::gpu::hostlink::Live()) host->StoreDropRange(e->Key(0), e->SubCount());
        g_c.objects.fetch_sub(1, std::memory_order_relaxed);
        g_c.bytes.fetch_sub(std::min(g_c.bytes.load(), e->bytes), std::memory_order_relaxed);
        delete e;
    }

    void AttachDevice(IDirect3DDevice9* device, bool ex)
    {
        if (!g_engineBase)
        {
            const auto* image = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(image);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(image + dos->e_lfanew);
            g_engineBase = reinterpret_cast<uintptr_t>(image);
            g_engineEnd = g_engineBase + nt->OptionalHeader.SizeOfImage;
        }
        g_ex.store(ex);
        if (!ex || !device) return;
        if (!g_runtimeBase)
        {
            // The device's own methods live in the system d3d9: its image is the runtime's range.
            HMODULE runtime = nullptr;
            void* method = (*reinterpret_cast<void***>(device))[0];
            if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   static_cast<LPCWSTR>(method), &runtime) && runtime)
            {
                const auto* image = reinterpret_cast<const uint8_t*>(runtime);
                const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(
                    image + reinterpret_cast<const IMAGE_DOS_HEADER*>(image)->e_lfanew);
                g_runtimeBase = reinterpret_cast<uintptr_t>(runtime);
                g_runtimeEnd = g_runtimeBase + nt->OptionalHeader.SizeOfImage;
            }
        }

        std::lock_guard<std::recursive_mutex> lock(g_mutex);
        s_devTestCoop.Hook(device, &hkTestCoop);
        s_devReset.Hook(device, &hkReset);
        s_devPresent.Hook(device, &hkPresent);
        s_devCursor.Hook(device, &hkSetCursorProperties);
        s_devShowCursor.Hook(device, &hkShowCursor);
        WLOG_INFO("d3d9ex: managed pool emulated (engine textures and buffers without a system-memory copy, backing "
                  "copies %s, staging up to %llu MB)", BackingEnabled() ? "in wxl-host" : "off",
                  static_cast<unsigned long long>(StagingCap() >> 20));
        ::wxl::log::Flush();
    }

    void FillEmulation(GpuEmulation& out)
    {
        out.emulatedObjects = g_c.objects.load();
        out.emulatedBytes = g_c.bytes.load();
        out.mirrorBytes = g_c.mirrorBytes.load();
        out.stagingBytes = g_c.stagingBytes.load();
        out.uploads = g_c.uploads.load();
        out.uploadBytes = g_c.uploadBytes.load();
        out.readbacks = g_c.readbacks.load();
        out.backingSent = g_c.backingSent.load();
        out.backingBytes = g_c.backingBytes.load();
        out.backingDropped = g_c.backingDropped.load();
        out.restores = g_c.restores.load();
    }
}
