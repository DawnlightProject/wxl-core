// Proxy-side D3D9 resource tracking: every texture, buffer and surface the device creates, by creator.
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

#include "engine/gpu/Resources.hpp"
#include "engine/gpu/Managed.hpp"

#include "common/Log.hpp"
#include "common/Mem.hpp"

#include <windows.h>
#include <intrin.h>

#include <atomic>
#include <cstring>
#include <unordered_map>

#pragma intrinsic(_ReturnAddress)

namespace wxl::gpu::resources
{
    namespace
    {
        // IDirect3DDevice9 slots.
        constexpr unsigned kSlotCreateTexture       = 23;
        constexpr unsigned kSlotCreateVolumeTexture = 24;
        constexpr unsigned kSlotCreateCubeTexture   = 25;
        constexpr unsigned kSlotCreateVertexBuffer  = 26;
        constexpr unsigned kSlotCreateIndexBuffer   = 27;
        constexpr unsigned kSlotCreateRenderTarget  = 28;
        constexpr unsigned kSlotCreateDepthStencil  = 29;
        constexpr unsigned kSlotCreateOffscreen     = 36;
        constexpr unsigned kSlotRelease             = 2;

        using CreateTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT,
                                                            D3DPOOL, IDirect3DTexture9**, HANDLE*);
        using CreateVolumeTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, UINT, DWORD,
                                                                  D3DFORMAT, D3DPOOL, IDirect3DVolumeTexture9**,
                                                                  HANDLE*);
        using CreateCubeTextureFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, DWORD, D3DFORMAT,
                                                                D3DPOOL, IDirect3DCubeTexture9**, HANDLE*);
        using CreateVertexBufferFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, DWORD, D3DPOOL,
                                                                 IDirect3DVertexBuffer9**, HANDLE*);
        using CreateIndexBufferFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                                                IDirect3DIndexBuffer9**, HANDLE*);
        using CreateRenderTargetFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT,
                                                                 D3DMULTISAMPLE_TYPE, DWORD, BOOL,
                                                                 IDirect3DSurface9**, HANDLE*);
        using CreateDepthStencilFn = CreateRenderTargetFn;
        using CreateOffscreenFn = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DPOOL,
                                                              IDirect3DSurface9**, HANDLE*);
        using ReleaseFn = ULONG(STDMETHODCALLTYPE*)(IUnknown*);

        CreateTextureFn       g_origCreateTexture       = nullptr;
        CreateVolumeTextureFn g_origCreateVolumeTexture = nullptr;
        CreateCubeTextureFn   g_origCreateCubeTexture   = nullptr;
        CreateVertexBufferFn  g_origCreateVertexBuffer  = nullptr;
        CreateIndexBufferFn   g_origCreateIndexBuffer   = nullptr;
        CreateRenderTargetFn  g_origCreateRenderTarget  = nullptr;
        CreateDepthStencilFn  g_origCreateDepthStencil  = nullptr;
        CreateOffscreenFn     g_origCreateOffscreen     = nullptr;

        bool g_ex = false;

        // --- creators --------------------------------------------------------------------------------

        struct Creator
        {
            uintptr_t base;
            uintptr_t end;
            char      name[kCreatorNameLen];
        };

        SRWLOCK  g_lock = SRWLOCK_INIT;
        Creator  g_creators[kMaxCreators] = {};
        uint32_t g_creatorCount = 0;

        /// Returns the creator slot for a code address, registering its module on first sight.
        uint32_t CreatorOf(void* address)
        {
            const uintptr_t a = reinterpret_cast<uintptr_t>(address);
            for (uint32_t i = 0; i < g_creatorCount; ++i)
                if (a >= g_creators[i].base && a < g_creators[i].end) return i;

            HMODULE module = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                    reinterpret_cast<LPCWSTR>(address), &module) || !module)
                return kMaxCreators;
            if (g_creatorCount >= kMaxCreators) return kMaxCreators;

            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
            Creator& c = g_creators[g_creatorCount];
            c.base = reinterpret_cast<uintptr_t>(module);
            c.end = c.base + nt->OptionalHeader.SizeOfImage;

            char path[MAX_PATH] = {};
            GetModuleFileNameA(module, path, MAX_PATH);
            const char* slash = std::strrchr(path, '\\');
            strncpy_s(c.name, slash ? slash + 1 : path, _TRUNCATE);
            return g_creatorCount++;
        }

        // --- live objects ----------------------------------------------------------------------------

        struct Entry
        {
            uint32_t creator;
            ResType  type;
            ResPool  pool;
            uint64_t bytes;
        };

        std::unordered_map<void*, Entry>& Live()
        {
            static std::unordered_map<void*, Entry> live;
            return live;
        }

        GpuCell  g_cells[kMaxCreators][kTypeCount][kPoolCount] = {};
        uint64_t g_untracked = 0;
        GpuEmulation g_emulation = {};

        ResPool PoolOf(D3DPOOL pool)
        {
            switch (pool)
            {
            case D3DPOOL_MANAGED:   return ResPool::Managed;
            case D3DPOOL_SYSTEMMEM: return ResPool::SystemMem;
            case D3DPOOL_SCRATCH:   return ResPool::Scratch;
            default:                return ResPool::Default;
            }
        }

        GpuCell& Cell(const Entry& e)
        {
            return g_cells[e.creator][static_cast<uint32_t>(e.type)][static_cast<uint32_t>(e.pool)];
        }

        /// Removes an object from the live set. Runs when its last reference goes.
        void Forget(void* object)
        {
            AcquireSRWLockExclusive(&g_lock);
            auto& live = Live();
            const auto it = live.find(object);
            if (it != live.end())
            {
                GpuCell& cell = Cell(it->second);
                if (cell.count) --cell.count;
                cell.bytes = cell.bytes >= it->second.bytes ? cell.bytes - it->second.bytes : 0;
                live.erase(it);
            }
            ReleaseSRWLockExclusive(&g_lock);
        }

        // --- Release, per resource vtable ------------------------------------------------------------
        // The same hook is written into every resource vtable met; the original is found by vtable.

        struct VtRelease
        {
            void**    vtbl;
            ReleaseFn orig;
        };
        constexpr uint32_t kMaxVtables = 32;
        VtRelease g_vtables[kMaxVtables] = {};
        std::atomic<uint32_t> g_vtableCount{ 0 };

        ULONG STDMETHODCALLTYPE hkRelease(IUnknown* self)
        {
            void** vtbl = *reinterpret_cast<void***>(self);
            ReleaseFn orig = nullptr;
            const uint32_t n = g_vtableCount.load(std::memory_order_acquire);
            for (uint32_t i = 0; i < n; ++i)
                if (g_vtables[i].vtbl == vtbl) { orig = g_vtables[i].orig; break; }
            if (!orig) return 0; // unreachable: the hook is only written into recorded vtables

            const ULONG left = orig(self);
            if (left == 0)
            {
                managed::OnDestroyed(self);
                Forget(self);
            }
            return left;
        }

        /// Writes hkRelease into an object's vtable once. Called with g_lock held.
        void HookReleaseLocked(void* object)
        {
            void** vtbl = *reinterpret_cast<void***>(object);
            const uint32_t n = g_vtableCount.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < n; ++i)
                if (g_vtables[i].vtbl == vtbl) return;
            if (n >= kMaxVtables) return;
            if (vtbl[kSlotRelease] == reinterpret_cast<void*>(&hkRelease)) return;

            void* previous = nullptr;
            g_vtables[n].vtbl = vtbl;
            g_vtables[n].orig = reinterpret_cast<ReleaseFn>(vtbl[kSlotRelease]);
            g_vtableCount.store(n + 1, std::memory_order_release);
            if (!::wxl::mem::SwapPointer(&vtbl[kSlotRelease], reinterpret_cast<void*>(&hkRelease), &previous))
                g_vtableCount.store(n, std::memory_order_release);
        }

        // --- sizes -----------------------------------------------------------------------------------

        /// Bytes per 4x4 block for block-compressed formats, 0 otherwise.
        uint32_t BlockBytes(D3DFORMAT f)
        {
            switch (static_cast<uint32_t>(f))
            {
            case MAKEFOURCC('D', 'X', 'T', '1'):
            case MAKEFOURCC('A', 'T', 'I', '1'):
                return 8;
            case MAKEFOURCC('D', 'X', 'T', '2'):
            case MAKEFOURCC('D', 'X', 'T', '3'):
            case MAKEFOURCC('D', 'X', 'T', '4'):
            case MAKEFOURCC('D', 'X', 'T', '5'):
            case MAKEFOURCC('A', 'T', 'I', '2'):
                return 16;
            default:
                return 0;
            }
        }

        uint32_t PixelBytes(D3DFORMAT f)
        {
            switch (static_cast<uint32_t>(f))
            {
            case D3DFMT_L8: case D3DFMT_A8: case D3DFMT_P8: case D3DFMT_A4L4: case D3DFMT_R3G3B2:
                return 1;
            case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
            case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_D16:
            case D3DFMT_D15S1: case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_A8R3G3B2: case D3DFMT_A8P8:
            case D3DFMT_D16_LOCKABLE:
                return 2;
            case D3DFMT_R8G8B8:
                return 3;
            case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16:
                return 8;
            case D3DFMT_A32B32G32R32F:
                return 16;
            case MAKEFOURCC('N', 'U', 'L', 'L'):
                return 0;
            case MAKEFOURCC('D', 'F', '1', '6'):
                return 2;
            default:
                return 4; // 32-bit colour, depth (D24S8, INTZ, DF24, RAWZ), R32F, G16R16F and unknowns
            }
        }

        uint32_t LevelCount(uint32_t levels, uint32_t w, uint32_t h, uint32_t d = 1)
        {
            if (levels) return levels;
            uint32_t n = 1;
            while (w > 1 || h > 1 || d > 1) { w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1; d = d > 1 ? d >> 1 : 1; ++n; }
            return n;
        }

        uint64_t ChainBytes(D3DFORMAT f, uint32_t w, uint32_t h, uint32_t levels)
        {
            uint64_t total = 0;
            for (uint32_t l = 0; l < levels; ++l)
            {
                total += SurfaceBytes(f, w, h);
                w = w > 1 ? w >> 1 : 1;
                h = h > 1 ? h >> 1 : 1;
            }
            return total;
        }

        uint64_t VolumeChainBytes(D3DFORMAT f, uint32_t w, uint32_t h, uint32_t d, uint32_t levels)
        {
            uint64_t total = 0;
            for (uint32_t l = 0; l < levels; ++l)
            {
                total += SurfaceBytes(f, w, h) * d;
                w = w > 1 ? w >> 1 : 1;
                h = h > 1 ? h >> 1 : 1;
                d = d > 1 ? d >> 1 : 1;
            }
            return total;
        }

        // --- creation hooks --------------------------------------------------------------------------

        HRESULT STDMETHODCALLTYPE hkCreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT levels, DWORD usage,
                                                  D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out, HANDLE* shared)
        {
            void* caller = _ReturnAddress();
            const managed::Plan plan = managed::PlanTexture(caller, usage, fmt, pool);
            const HRESULT hr = g_origCreateTexture(d, w, h, levels, plan.usage, fmt, plan.pool, out, shared);
            if (SUCCEEDED(hr) && out && *out)
            {
                const uint32_t n = (*out)->GetLevelCount();
                const uint64_t bytes = ChainBytes(fmt, w, h, n);
                Track(*out, ResType::Texture, plan.pool, bytes, caller);
                if (plan.emulate) managed::AdoptTexture(d, *out, caller, bytes);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCreateVolumeTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT depth, UINT levels,
                                                        DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                                        IDirect3DVolumeTexture9** out, HANDLE* shared)
        {
            void* caller = _ReturnAddress();
            const managed::Plan plan = managed::PlanTexture(caller, usage, fmt, pool);
            const HRESULT hr = g_origCreateVolumeTexture(d, w, h, depth, levels, plan.usage, fmt, plan.pool, out, shared);
            if (SUCCEEDED(hr) && out && *out)
            {
                const uint64_t bytes = VolumeChainBytes(fmt, w, h, depth, (*out)->GetLevelCount());
                Track(*out, ResType::Volume, plan.pool, bytes, caller);
                if (plan.emulate) managed::AdoptVolume(d, *out, caller, bytes);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCreateCubeTexture(IDirect3DDevice9* d, UINT edge, UINT levels, DWORD usage,
                                                      D3DFORMAT fmt, D3DPOOL pool, IDirect3DCubeTexture9** out,
                                                      HANDLE* shared)
        {
            void* caller = _ReturnAddress();
            const managed::Plan plan = managed::PlanTexture(caller, usage, fmt, pool);
            const HRESULT hr = g_origCreateCubeTexture(d, edge, levels, plan.usage, fmt, plan.pool, out, shared);
            if (SUCCEEDED(hr) && out && *out)
            {
                const uint64_t bytes = ChainBytes(fmt, edge, edge, (*out)->GetLevelCount()) * 6;
                Track(*out, ResType::Cube, plan.pool, bytes, caller);
                if (plan.emulate) managed::AdoptCube(d, *out, caller, bytes);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCreateVertexBuffer(IDirect3DDevice9* d, UINT length, DWORD usage, DWORD fvf,
                                                       D3DPOOL pool, IDirect3DVertexBuffer9** out, HANDLE* shared)
        {
            void* caller = _ReturnAddress();
            const managed::Plan plan = managed::PlanBuffer(caller, usage, pool);
            const HRESULT hr = g_origCreateVertexBuffer(d, length, plan.usage, fvf, plan.pool, out, shared);
            if (SUCCEEDED(hr) && out && *out)
            {
                Track(*out, ResType::VertexBuffer, plan.pool, length, caller);
                if (plan.emulate) managed::AdoptVertexBuffer(d, *out, caller, length, usage);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCreateIndexBuffer(IDirect3DDevice9* d, UINT length, DWORD usage, D3DFORMAT fmt,
                                                      D3DPOOL pool, IDirect3DIndexBuffer9** out, HANDLE* shared)
        {
            void* caller = _ReturnAddress();
            const managed::Plan plan = managed::PlanBuffer(caller, usage, pool);
            const HRESULT hr = g_origCreateIndexBuffer(d, length, plan.usage, fmt, plan.pool, out, shared);
            if (SUCCEEDED(hr) && out && *out)
            {
                Track(*out, ResType::IndexBuffer, plan.pool, length, caller);
                if (plan.emulate) managed::AdoptIndexBuffer(d, *out, caller, length, usage);
            }
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCreateRenderTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt,
                                                       D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL lockable,
                                                       IDirect3DSurface9** out, HANDLE* shared)
        {
            void* caller = _ReturnAddress();
            const HRESULT hr = g_origCreateRenderTarget(d, w, h, fmt, ms, quality, lockable, out, shared);
            if (SUCCEEDED(hr) && out && *out)
                Track(*out, ResType::Surface, D3DPOOL_DEFAULT, SurfaceBytes(fmt, w, h) * (ms > 1 ? ms : 1), caller);
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCreateDepthStencil(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt,
                                                       D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL discard,
                                                       IDirect3DSurface9** out, HANDLE* shared)
        {
            void* caller = _ReturnAddress();
            const HRESULT hr = g_origCreateDepthStencil(d, w, h, fmt, ms, quality, discard, out, shared);
            if (SUCCEEDED(hr) && out && *out)
                Track(*out, ResType::Surface, D3DPOOL_DEFAULT, SurfaceBytes(fmt, w, h) * (ms > 1 ? ms : 1), caller);
            return hr;
        }

        HRESULT STDMETHODCALLTYPE hkCreateOffscreen(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool,
                                                    IDirect3DSurface9** out, HANDLE* shared)
        {
            void* caller = _ReturnAddress();
            const HRESULT hr = g_origCreateOffscreen(d, w, h, fmt, pool, out, shared);
            if (SUCCEEDED(hr) && out && *out)
                Track(*out, ResType::Surface, pool, SurfaceBytes(fmt, w, h), caller);
            return hr;
        }

        template <class Fn>
        void Swap(void** vtbl, unsigned slot, Fn hook, Fn* orig)
        {
            if (vtbl[slot] == reinterpret_cast<void*>(hook)) return;
            void* previous = nullptr;
            if (::wxl::mem::SwapPointer(&vtbl[slot], reinterpret_cast<void*>(hook), &previous))
                *orig = reinterpret_cast<Fn>(previous);
        }
    }

    uint64_t SurfaceBytes(D3DFORMAT format, uint32_t width, uint32_t height)
    {
        if (const uint32_t block = BlockBytes(format))
            return uint64_t((width + 3) / 4) * ((height + 3) / 4) * block;
        return uint64_t(width) * height * PixelBytes(format);
    }

    void Track(void* object, ResType type, D3DPOOL pool, uint64_t bytes, void* returnAddress)
    {
        managed::OnCreated(object);   // outside g_lock: the managed layer's lock is always taken first
        AcquireSRWLockExclusive(&g_lock);
        const uint32_t creator = CreatorOf(returnAddress);
        if (creator >= kMaxCreators)
        {
            ++g_untracked;
            ReleaseSRWLockExclusive(&g_lock);
            return;
        }

        // A new object at an address still recorded means the old one died unseen: drop the stale entry.
        auto& live = Live();
        const auto stale = live.find(object);
        if (stale != live.end())
        {
            GpuCell& old = Cell(stale->second);
            if (old.count) --old.count;
            old.bytes = old.bytes >= stale->second.bytes ? old.bytes - stale->second.bytes : 0;
            live.erase(stale);
        }

        const Entry e{ creator, type, PoolOf(pool), bytes };
        live.emplace(object, e);
        GpuCell& cell = Cell(e);
        ++cell.count;
        cell.bytes += bytes;
        HookReleaseLocked(object);
        ReleaseSRWLockExclusive(&g_lock);
    }

    void Attach(IDirect3DDevice9* device, bool ex)
    {
        if (!device) return;
        g_ex = ex;
        void** vtbl = *reinterpret_cast<void***>(device);
        Swap(vtbl, kSlotCreateTexture,       &hkCreateTexture,       &g_origCreateTexture);
        Swap(vtbl, kSlotCreateVolumeTexture, &hkCreateVolumeTexture, &g_origCreateVolumeTexture);
        Swap(vtbl, kSlotCreateCubeTexture,   &hkCreateCubeTexture,   &g_origCreateCubeTexture);
        Swap(vtbl, kSlotCreateVertexBuffer,  &hkCreateVertexBuffer,  &g_origCreateVertexBuffer);
        Swap(vtbl, kSlotCreateIndexBuffer,   &hkCreateIndexBuffer,   &g_origCreateIndexBuffer);
        Swap(vtbl, kSlotCreateRenderTarget,  &hkCreateRenderTarget,  &g_origCreateRenderTarget);
        Swap(vtbl, kSlotCreateDepthStencil,  &hkCreateDepthStencil,  &g_origCreateDepthStencil);
        Swap(vtbl, kSlotCreateOffscreen,     &hkCreateOffscreen,     &g_origCreateOffscreen);
        managed::AttachDevice(device, ex);
    }

    GpuEmulation& Emulation()
    {
        return g_emulation;
    }

    int Snapshot(GpuStats* out, uint32_t outSize)
    {
        if (!out || outSize < sizeof(GpuStats)) return 0;
        std::memset(out, 0, sizeof(GpuStats));
        out->structSize = sizeof(GpuStats);
        out->version = kGpuStatsVersion;
        out->d3d9ex = g_ex ? 1u : 0u;

        AcquireSRWLockShared(&g_lock);
        out->creatorCount = g_creatorCount;
        for (uint32_t i = 0; i < g_creatorCount; ++i)
            std::memcpy(out->creators[i], g_creators[i].name, kCreatorNameLen);
        std::memcpy(out->cells, g_cells, sizeof(g_cells));
        out->untracked = g_untracked;
        ReleaseSRWLockShared(&g_lock);

        managed::FillEmulation(out->emulation);
        return 1;
    }
}
