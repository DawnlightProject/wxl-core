// wxl-gpu-probe: drives the d3d9 proxy's D3D9Ex managed-pool emulation and reads back what the GPU holds.
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

// A correctness check, not a benchmark. Run it from a folder holding the proxy d3d9.dll and no WarcraftXL.dll:
// this executable then stands where Wow.exe does, so its resources take the engine path (staging and
// UpdateSurface). Every check reads the GPU's copy back through a render target.

#include "engine/gpu/GpuStats.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    int g_failures = 0;

    void Check(bool ok, const char* what)
    {
        std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
        if (!ok) ++g_failures;
    }

    /// Copies level 0 of a texture to a render target and reads it back as ARGB.
    bool ReadBack(IDirect3DDevice9* dev, IDirect3DTexture9* tex, UINT w, UINT h, std::vector<uint32_t>& out)
    {
        IDirect3DSurface9* rt = nullptr;
        IDirect3DSurface9* sys = nullptr;
        IDirect3DSurface9* src = nullptr;
        bool ok = SUCCEEDED(dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr))
               && SUCCEEDED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr))
               && SUCCEEDED(tex->GetSurfaceLevel(0, &src))
               && SUCCEEDED(dev->StretchRect(src, nullptr, rt, nullptr, D3DTEXF_POINT))
               && SUCCEEDED(dev->GetRenderTargetData(rt, sys));
        if (ok)
        {
            D3DLOCKED_RECT lr{};
            ok = SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
            if (ok)
            {
                out.resize(size_t(w) * h);
                for (UINT y = 0; y < h; ++y)
                    std::memcpy(&out[size_t(y) * w], static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, w * 4);
                sys->UnlockRect();
            }
        }
        if (src) src->Release();
        if (sys) sys->Release();
        if (rt) rt->Release();
        return ok;
    }

    /// Draws a texture's level 0 over a render target with point sampling and reads it back as ARGB.
    /// StretchRect cannot read a compressed format, so this is the path for DXT.
    bool DrawBack(IDirect3DDevice9* dev, IDirect3DTexture9* tex, UINT w, UINT h, std::vector<uint32_t>& out)
    {
        struct V { float x, y, z, rhw, u, v; };
        const float fw = float(w) - 0.5f, fh = float(h) - 0.5f;
        const V quad[4] = { { -0.5f, -0.5f, 0, 1, 0, 0 }, { fw, -0.5f, 0, 1, 1, 0 }, { -0.5f, fh, 0, 1, 0, 1 }, { fw, fh, 0, 1, 1, 1 } };
        IDirect3DSurface9* rt = nullptr;
        IDirect3DSurface9* sys = nullptr;
        IDirect3DSurface9* previous = nullptr;
        bool ok = SUCCEEDED(dev->CreateRenderTarget(w, h, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr))
               && SUCCEEDED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr));
        if (ok)
        {
            dev->GetRenderTarget(0, &previous);
            dev->SetRenderTarget(0, rt);
            dev->BeginScene();
            dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
            dev->SetTexture(0, tex);
            dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetRenderState(D3DRS_LIGHTING, FALSE);
            dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            ok = SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V)));
            dev->EndScene();
            dev->SetTexture(0, nullptr);
            ok = ok && SUCCEEDED(dev->GetRenderTargetData(rt, sys));
            if (previous) { dev->SetRenderTarget(0, previous); previous->Release(); }
            D3DLOCKED_RECT lr{};
            if (ok && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
            {
                out.resize(size_t(w) * h);
                for (UINT y = 0; y < h; ++y)
                    std::memcpy(&out[size_t(y) * w], static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, w * 4);
                sys->UnlockRect();
            }
        }
        if (sys) sys->Release();
        if (rt) rt->Release();
        return ok && !out.empty();
    }

    /// Calls a nine-argument __stdcall method from executable heap memory, so the call site is outside this
    /// executable: the proxy then treats the resource as created by an extension (the mirror path).
    using Call9 = HRESULT(__stdcall*)(void* fn, void*, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR, void*, void*);
    Call9 ForeignCaller()
    {
        static const uint8_t code[] = {
            0x55, 0x8B, 0xEC,                                     // push ebp; mov ebp, esp
            0xFF, 0x75, 0x2C, 0xFF, 0x75, 0x28, 0xFF, 0x75, 0x24, // push the nine arguments, last first
            0xFF, 0x75, 0x20, 0xFF, 0x75, 0x1C, 0xFF, 0x75, 0x18,
            0xFF, 0x75, 0x14, 0xFF, 0x75, 0x10, 0xFF, 0x75, 0x0C,
            0xFF, 0x55, 0x08,                                     // call [ebp+8]
            0x5D, 0xC2, 0x28, 0x00 };                             // pop ebp; ret 40
        static void* page = nullptr;
        if (!page)
        {
            page = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (page) std::memcpy(page, code, sizeof code);
        }
        return reinterpret_cast<Call9>(page);
    }

    uint32_t Pattern(UINT x, UINT y, UINT level) { return 0xFF000000u | (level << 16) | ((y & 0xFF) << 8) | (x & 0xFF); }
}

int main()
{
    HMODULE proxy = LoadLibraryA(".\\d3d9.dll");
    if (!proxy) { std::printf("d3d9.dll (the proxy) must sit next to this executable\n"); return 2; }
    using CreateFn = IDirect3D9*(WINAPI*)(UINT);
    const auto create = reinterpret_cast<CreateFn>(GetProcAddress(proxy, "Direct3DCreate9"));
    const auto stats = reinterpret_cast<wxl::gpu::ProxyStatsFn>(GetProcAddress(proxy, "WXL_ProxyStats"));
    Check(create && stats, "the proxy exports Direct3DCreate9 and WXL_ProxyStats");
    if (!create || !stats) return 1;

    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "wxl-gpu-probe";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("wxl-gpu-probe", "wxl-gpu-probe", WS_OVERLAPPEDWINDOW, 0, 0, 256, 256, nullptr, nullptr,
                             wc.hInstance, nullptr);

    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    IDirect3D9Ex* d3dEx = nullptr;
    Check(d3d && SUCCEEDED(d3d->QueryInterface(__uuidof(IDirect3D9Ex), reinterpret_cast<void**>(&d3dEx))),
          "Direct3DCreate9 returns a D3D9Ex factory");
    if (d3dEx) d3dEx->Release();
    if (!d3d) return 1;

    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 256;
    pp.BackBufferHeight = 256;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = wnd;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;   // as the engine asks when it does not multisample
    IDirect3DDevice9* dev = nullptr;
    const HRESULT created = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd,
                                              D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_PUREDEVICE | D3DCREATE_FPU_PRESERVE,
                                              &pp, &dev);
    Check(SUCCEEDED(created) && dev, "CreateDevice (the engine's flags, pure included)");
    if (!dev) return 1;
    IDirect3DDevice9Ex* devEx = nullptr;
    Check(SUCCEEDED(dev->QueryInterface(__uuidof(IDirect3DDevice9Ex), reinterpret_cast<void**>(&devEx))),
          "the device is a D3D9Ex device");
    if (devEx) devEx->Release();

    std::printf("ARGB texture, full chain, surface and texture locks\n");
    {
        const UINT w = 64, h = 32;
        IDirect3DTexture9* tex = nullptr;
        Check(SUCCEEDED(dev->CreateTexture(w, h, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) && tex,
              "a MANAGED texture is created on the D3D9Ex device");
        D3DSURFACE_DESC desc{};
        tex->GetLevelDesc(0, &desc);
        Check(desc.Pool == D3DPOOL_MANAGED, "GetLevelDesc still reports MANAGED");
        const UINT levels = tex->GetLevelCount();
        bool locks = true;
        for (UINT l = 0; l < levels; ++l)
        {
            // The engine's way: the level surface, locked on a rect, written whole.
            IDirect3DSurface9* s = nullptr;
            D3DLOCKED_RECT lr{};
            const UINT lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
            locks = locks && SUCCEEDED(tex->GetSurfaceLevel(l, &s)) && SUCCEEDED(s->LockRect(&lr, nullptr, 0));
            if (!locks) break;
            for (UINT y = 0; y < lh; ++y)
                for (UINT x = 0; x < lw; ++x)
                    reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = Pattern(x, y, l);
            locks = SUCCEEDED(s->UnlockRect());
            s->Release();
        }
        Check(locks, "every level locks and unlocks through its surface");
        tex->PreLoad();

        // A dirty rectangle through the texture itself.
        const RECT dirty{ 8, 4, 24, 12 };
        D3DLOCKED_RECT lr{};
        const bool partial = SUCCEEDED(tex->LockRect(0, &lr, &dirty, 0));
        if (partial)
        {
            for (LONG y = 0; y < dirty.bottom - dirty.top; ++y)
                for (LONG x = 0; x < dirty.right - dirty.left; ++x)
                    reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xFFABCDEFu;
            tex->UnlockRect(0);
        }
        Check(partial, "a partial LockRect on level 0");

        std::vector<uint32_t> px;
        const bool read = ReadBack(dev, tex, w, h, px);
        bool same = read;
        for (UINT y = 0; same && y < h; ++y)
            for (UINT x = 0; same && x < w; ++x)
            {
                const bool inDirty = LONG(x) >= dirty.left && LONG(x) < dirty.right && LONG(y) >= dirty.top && LONG(y) < dirty.bottom;
                same = px[size_t(y) * w + x] == (inDirty ? 0xFFABCDEFu : Pattern(x, y, 0));
            }
        Check(same, "the GPU holds the full upload with the dirty rectangle patched in");

        D3DLOCKED_RECT ro{};
        const bool readLock = SUCCEEDED(tex->LockRect(0, &ro, nullptr, D3DLOCK_READONLY));
        if (readLock) tex->UnlockRect(0);
        Check(readLock, "a read lock is answered (from the backing copy, or zeroes without a host)");
        tex->Release();
    }

    std::printf("DXT1 texture, mips down to 1x1\n");
    {
        const UINT w = 16, h = 16;
        IDirect3DTexture9* tex = nullptr;
        const bool made = SUCCEEDED(dev->CreateTexture(w, h, 0, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &tex, nullptr)) && tex;
        Check(made, "a MANAGED DXT1 texture is created");
        if (made)
        {
            bool locks = true;
            const UINT levels = tex->GetLevelCount();
            for (UINT l = 0; l < levels && locks; ++l)
            {
                D3DLOCKED_RECT lr{};
                const UINT lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1;
                locks = SUCCEEDED(tex->LockRect(l, &lr, nullptr, 0));
                if (!locks) break;
                const UINT bw = (lw + 3) / 4, bh = (lh + 3) / 4;
                for (UINT by = 0; by < bh; ++by)
                    for (UINT bx = 0; bx < bw; ++bx)
                    {
                        // Solid red: both endpoints 0xF800, every index 0.
                        uint8_t* block = static_cast<uint8_t*>(lr.pBits) + by * lr.Pitch + bx * 8;
                        const uint16_t red = 0xF800;
                        std::memcpy(block, &red, 2);
                        std::memcpy(block + 2, &red, 2);
                        std::memset(block + 4, 0, 4);
                    }
                locks = SUCCEEDED(tex->UnlockRect(l));
            }
            Check(locks, "every DXT1 level, 2x2 and 1x1 included, locks and unlocks");
            std::vector<uint32_t> px;
            bool red = DrawBack(dev, tex, w, h, px);
            for (uint32_t p : px) red = red && (p & 0x00FFFFFFu) == 0x00FF0000u;
            Check(red, "the GPU decodes the uploaded DXT1 blocks to red");
            tex->Release();
        }
    }

    std::printf("extension texture (mirror path): partial writes, read back through a read lock\n");
    {
        const UINT w = 32, h = 32;
        IDirect3DTexture9* tex = nullptr;
        void* createTexture = (*reinterpret_cast<void***>(dev))[23];
        const HRESULT hr = ForeignCaller()(createTexture, dev, w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr);
        Check(SUCCEEDED(hr) && tex, "a MANAGED texture created from outside the executable");
        if (tex)
        {
            const RECT a{ 0, 0, 16, 16 }, b{ 16, 16, 32, 32 };
            D3DLOCKED_RECT lr{};
            bool locks = SUCCEEDED(tex->LockRect(0, &lr, &a, 0));
            if (locks)
            {
                for (UINT y = 0; y < 16; ++y)
                    for (UINT x = 0; x < 16; ++x) reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xFF112233u;
                tex->UnlockRect(0);
            }
            locks = locks && SUCCEEDED(tex->LockRect(0, &lr, &b, 0));
            if (locks)
            {
                for (UINT y = 0; y < 16; ++y)
                    for (UINT x = 0; x < 16; ++x) reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xFF445566u;
                tex->UnlockRect(0);
            }
            Check(locks, "two partial locks on different quarters");

            std::vector<uint32_t> px;
            bool gpu = ReadBack(dev, tex, w, h, px);
            for (UINT y = 0; gpu && y < h; ++y)
                for (UINT x = 0; gpu && x < w; ++x)
                {
                    const uint32_t want = (x < 16 && y < 16) ? 0xFF112233u : (x >= 16 && y >= 16) ? 0xFF445566u : px[size_t(y) * w + x];
                    gpu = px[size_t(y) * w + x] == want;
                }
            Check(gpu, "the GPU holds both quarters: the second lock kept the first");

            bool cpu = SUCCEEDED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY));
            if (cpu)
            {
                cpu = reinterpret_cast<uint32_t*>(lr.pBits)[0] == 0xFF112233u
                   && reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + 20 * lr.Pitch)[20] == 0xFF445566u;
                tex->UnlockRect(0);
            }
            Check(cpu, "a read lock returns what was written");
            tex->Release();
        }
        IDirect3DVolumeTexture9* vol = nullptr;
        const bool volMade = SUCCEEDED(dev->CreateVolumeTexture(8, 8, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vol, nullptr)) && vol;
        bool volLock = false;
        if (volMade)
        {
            D3DLOCKED_BOX lb{};
            volLock = SUCCEEDED(vol->LockBox(0, &lb, nullptr, 0));
            if (volLock)
            {
                std::memset(lb.pBits, 0x7F, size_t(lb.SlicePitch) * 4);
                volLock = SUCCEEDED(vol->UnlockBox(0));
            }
            vol->Release();
        }
        Check(volMade && volLock, "a MANAGED volume texture locks and uploads through its mirror");
    }

    std::printf("MANAGED vertex buffer, nested locks\n");
    {
        struct V { float x, y, z, rhw; DWORD color; };
        IDirect3DVertexBuffer9* vb = nullptr;
        const bool made = SUCCEEDED(dev->CreateVertexBuffer(6 * sizeof(V), D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW | D3DFVF_DIFFUSE,
                                                            D3DPOOL_MANAGED, &vb, nullptr)) && vb;
        Check(made, "a MANAGED WRITEONLY vertex buffer is created");
        if (made)
        {
            const V quad[6] = { { -0.5f, -0.5f, 0, 1, 0xFF00FF00 }, { 63.5f, -0.5f, 0, 1, 0xFF00FF00 }, { -0.5f, 63.5f, 0, 1, 0xFF00FF00 },
                                { 63.5f, -0.5f, 0, 1, 0xFF00FF00 }, { 63.5f, 63.5f, 0, 1, 0xFF00FF00 }, { -0.5f, 63.5f, 0, 1, 0xFF00FF00 } };
            void* a = nullptr;
            void* b = nullptr;
            // Two ranges locked at once, released in the order they were taken.
            const bool locked = SUCCEEDED(vb->Lock(0, 3 * sizeof(V), &a, 0)) && SUCCEEDED(vb->Lock(3 * sizeof(V), 3 * sizeof(V), &b, 0));
            if (locked)
            {
                std::memcpy(a, quad, 3 * sizeof(V));
                std::memcpy(b, quad + 3, 3 * sizeof(V));
                vb->Unlock();
                vb->Unlock();
            }
            Check(locked, "two nested locks");

            IDirect3DSurface9* rt = nullptr;
            IDirect3DSurface9* sys = nullptr;
            bool drawn = SUCCEEDED(dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr))
                      && SUCCEEDED(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr));
            if (drawn)
            {
                dev->SetRenderTarget(0, rt);
                dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFF000000, 1.0f, 0);
                dev->BeginScene();
                dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
                dev->SetStreamSource(0, vb, 0, sizeof(V));
                dev->SetRenderState(D3DRS_LIGHTING, FALSE);
                dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                dev->SetRenderState(D3DRS_ZENABLE, FALSE);
                dev->DrawPrimitive(D3DPT_TRIANGLELIST, 0, 2);
                dev->EndScene();
                drawn = SUCCEEDED(dev->GetRenderTargetData(rt, sys));
                D3DLOCKED_RECT lr{};
                if (drawn && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
                {
                    for (UINT y = 0; y < 64 && drawn; ++y)
                        for (UINT x = 0; x < 64 && drawn; ++x)
                            drawn = (reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] & 0x00FFFFFF) == 0x0000FF00;
                    sys->UnlockRect();
                }
            }
            Check(drawn, "the GPU draws the quad written through both locks");
            if (sys) sys->Release();
            if (rt) rt->Release();
            vb->Release();
        }
    }

    std::printf("hardware cursor, as CGxDeviceD3d::ICursorCreate and ICursorDraw do it\n");
    {
        IDirect3DTexture9* cursor = nullptr;
        IDirect3DSurface9* level = nullptr;
        const bool made = SUCCEEDED(dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cursor, nullptr))
                       && cursor && SUCCEEDED(cursor->GetSurfaceLevel(0, &level));
        Check(made, "the 32x32 A8R8G8B8 MANAGED cursor texture and its level surface");
        bool ok = made;
        for (int round = 0; ok && round < 3; ++round)
        {
            D3DLOCKED_RECT lr{};
            ok = SUCCEEDED(level->LockRect(&lr, nullptr, 0));
            if (!ok) break;
            for (UINT y = 0; y < 32; ++y)
                for (UINT x = 0; x < 32; ++x)
                    reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch)[x] = 0xFF000000u | (round << 8) | x;
            level->UnlockRect();
            const HRESULT hr = dev->SetCursorProperties(1, 2, level);
            std::printf("    round %d: SetCursorProperties 0x%08lX\n", round, static_cast<unsigned long>(hr));
            ok = SUCCEEDED(hr);
        }
        Check(ok, "SetCursorProperties accepts the cursor surface, first time and after later updates");
        dev->ShowCursor(TRUE);

        std::vector<uint32_t> px;
        bool gpu = ReadBack(dev, cursor, 32, 32, px);
        for (UINT i = 0; gpu && i < 32 * 32; ++i) gpu = px[i] == (0xFF000000u | (2u << 8) | (i % 32));
        Check(gpu, "the GPU copy of the cursor texture holds the last update");
        if (level) level->Release();
        if (cursor) cursor->Release();
    }

    std::printf("screenshots, as CGxDeviceD3d::DeviceReadPixels does it\n");
    {
        IDirect3DSurface9* back = nullptr;
        D3DLOCKED_RECT lr{};
        const bool backOk = SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back))
                         && SUCCEEDED(back->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        if (backOk) back->UnlockRect();
        Check(backOk, "the lockable back buffer reads (single-sampled path)");
        IDirect3DSurface9* copy = nullptr;
        const RECT r{ 0, 0, 64, 64 };
        const bool rtOk = back && SUCCEEDED(dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE, &copy, nullptr))
                       && SUCCEEDED(dev->StretchRect(back, &r, copy, &r, D3DTEXF_NONE))
                       && SUCCEEDED(copy->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        if (rtOk) copy->UnlockRect();
        Check(rtOk, "a lockable render target copied from the back buffer reads (multisampled path)");
        if (copy) copy->Release();
        if (back) back->Release();
    }

    std::printf("device\n");
    {
        Check(dev->TestCooperativeLevel() == D3D_OK, "TestCooperativeLevel answers D3D_OK");
        IDirect3DTexture9* kept = nullptr;
        dev->CreateTexture(32, 32, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &kept, nullptr);
        Check(SUCCEEDED(dev->Reset(&pp)), "Reset with an emulated managed texture alive");
        if (kept) kept->Release();
    }

    static wxl::gpu::GpuStats s;
    Check(stats(&s, sizeof s) == 1 && s.d3d9ex == 1, "WXL_ProxyStats reports a D3D9Ex device");
    std::printf("  emulated now %llu, uploads %llu (%.1f KB), readbacks %llu, staging %.1f KB, backing dropped %llu\n",
                static_cast<unsigned long long>(s.emulation.emulatedObjects),
                static_cast<unsigned long long>(s.emulation.uploads), s.emulation.uploadBytes / 1024.0,
                static_cast<unsigned long long>(s.emulation.readbacks), s.emulation.stagingBytes / 1024.0,
                static_cast<unsigned long long>(s.emulation.backingDropped));
    Check(s.emulation.emulatedObjects == 0, "every emulated resource was forgotten on release");

    dev->Release();
    d3d->Release();
    std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
