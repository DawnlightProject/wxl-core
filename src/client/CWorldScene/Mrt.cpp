// Mrt: render targets 1 and 2 during the world pass, each written only by pixel shaders that output it.
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

#include "client/CWorldScene/Mrt.hpp"

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "common/Mem.hpp"
#include "offsets/engine/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <unordered_map>

// The engine only ever binds render target 0 (CGxDeviceD3d::DeviceSetRenderTarget 0x0068F770 calls
// SetRenderTarget(0, ...) and re-binds the depth cached at device +0x3B40), so render target 1 would
// stay bound through the shadow maps, glow and every other target switch inside the pass. D3D9 wants
// every bound target the same size, so render targets 1 and 2 are bound only while the world's own
// target 0 is. Shaders that do not write oCn leave undefined values in a bound target, so each write
// mask (COLORWRITEENABLE1, COLORWRITEENABLE2) is opened only for shaders that write that output, and
// never while blending.
namespace wxl::runtime::mrt
{
    namespace
    {
        namespace vt = wxl::offsets::engine::gx::vt;

        constexpr D3DRENDERSTATETYPE kWriteMask1 = static_cast<D3DRENDERSTATETYPE>(190); // COLORWRITEENABLE1
        constexpr D3DRENDERSTATETYPE kWriteMask2 = static_cast<D3DRENDERSTATETYPE>(191); // COLORWRITEENABLE2

        using SetRenderTargetFn = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
        using ClearFn           = HRESULT(__stdcall*)(IDirect3DDevice9*, DWORD, const D3DRECT*, DWORD, D3DCOLOR, float, DWORD);
        using SetRenderStateFn  = HRESULT(__stdcall*)(IDirect3DDevice9*, D3DRENDERSTATETYPE, DWORD);
        using SetPixelShaderFn  = HRESULT(__stdcall*)(IDirect3DDevice9*, IDirect3DPixelShader9*);

        SetRenderTargetFn g_origSetRenderTarget = nullptr;
        ClearFn           g_origClear           = nullptr;
        SetRenderStateFn  g_origSetRenderState  = nullptr;
        SetPixelShaderFn  g_origSetPixelShader  = nullptr;
        void**            g_hookedVtbl          = nullptr;

        bool               g_active   = false;  // inside a world pass that uses render target 1
        bool               g_bound    = false;  // render targets 1 (and 2) currently bound
        IDirect3DSurface9* g_world0   = nullptr;
        IDirect3DSurface9* g_target   = nullptr;
        IDirect3DSurface9* g_target2  = nullptr; // render target 2, null when not used this pass
        uint32_t           g_psWrites = 0;      // bit n: the bound pixel shader writes oCn
        bool               g_blend    = false;
        DWORD              g_mask     = 0xFFFFFFFF;
        DWORD              g_mask2    = 0xFFFFFFFF;
        uint32_t           g_capsState = 0;      // 0 unknown, 1 usable, 2 refused

        std::unordered_map<IDirect3DPixelShader9*, uint32_t> g_writes;

        /// The colour outputs a ps_2_0+ token stream writes, bit n for oCn.
        uint32_t WrittenOutputs(const DWORD* t, UINT bytes)
        {
            uint32_t outputs = 0;
            const UINT n = bytes / 4;
            for (UINT i = 1; i < n;)
            {
                const DWORD tok = t[i];
                const DWORD op  = tok & 0xFFFF;
                if (op == 0xFFFF) break;
                if (op == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
                const UINT len = (tok >> 24) & 0x0F;
                if (op != 0x1F && op != 0x51 && len >= 1 && i + 1 < n)   // not dcl / def: first param is the dest
                {
                    const DWORD d    = t[i + 1];
                    const DWORD type = ((d >> 28) & 0x7) | ((d >> 8) & 0x18);
                    if (type == 8 && (d & 0x7FF) < 4) outputs |= 1u << (d & 0x7FF);   // D3DSPR_COLOROUT
                }
                i += 1 + len;
            }
            return outputs;
        }

        uint32_t ShaderOutputs(IDirect3DPixelShader9* ps)
        {
            if (!ps) return 0;
            auto it = g_writes.find(ps);
            if (it != g_writes.end()) return it->second;
            UINT size = 0;
            uint32_t outputs = 0;
            if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < (1u << 20))
            {
                DWORD* buf = static_cast<DWORD*>(HeapAlloc(GetProcessHeap(), 0, size));
                if (buf)
                {
                    if (SUCCEEDED(ps->GetFunction(buf, &size))) outputs = WrittenOutputs(buf, size);
                    HeapFree(GetProcessHeap(), 0, buf);
                }
            }
            if (g_writes.size() > 20000) g_writes.clear();
            g_writes.emplace(ps, outputs);
            return outputs;
        }

        void UpdateMask(IDirect3DDevice9* d)
        {
            if (!g_active || !g_bound) return;
            const DWORD want = ((g_psWrites & 2u) && !g_blend) ? 0xF : 0;
            if (want != g_mask)
            {
                g_mask = want;
                g_origSetRenderState(d, kWriteMask1, want);
            }
            if (!g_target2) return;
            const DWORD want2 = ((g_psWrites & 4u) && !g_blend) ? 0xF : 0;
            if (want2 != g_mask2)
            {
                g_mask2 = want2;
                g_origSetRenderState(d, kWriteMask2, want2);
            }
        }

        void Bind(IDirect3DDevice9* d, bool on)
        {
            if (on == g_bound) return;
            // Render target 2 goes on after 1 and comes off before it: the bound targets stay contiguous.
            if (!on && g_target2) g_origSetRenderTarget(d, 2, nullptr);
            g_origSetRenderTarget(d, 1, on ? g_target : nullptr);
            if (on && g_target2) g_origSetRenderTarget(d, 2, g_target2);
            g_bound = on;
            g_mask  = 0xFFFFFFFF;
            g_mask2 = 0xFFFFFFFF;
            UpdateMask(d);
        }

        HRESULT __stdcall hkSetRenderTarget(IDirect3DDevice9* d, DWORD index, IDirect3DSurface9* surface)
        {
            if (g_active && index == 0 && g_bound && surface != g_world0) Bind(d, false); // before the size changes
            const HRESULT hr = g_origSetRenderTarget(d, index, surface);
            if (g_active && index == 0 && surface == g_world0) Bind(d, true);
            return hr;
        }

        HRESULT __stdcall hkClear(IDirect3DDevice9* d, DWORD count, const D3DRECT* rects, DWORD flags, D3DCOLOR color,
                                  float z, DWORD stencil)
        {
            // The engine's colour clears are meant for its own target only.
            if (g_active && g_bound && (flags & D3DCLEAR_TARGET))
            {
                if (g_target2) g_origSetRenderTarget(d, 2, nullptr);
                g_origSetRenderTarget(d, 1, nullptr);
                const HRESULT hr = g_origClear(d, count, rects, flags, color, z, stencil);
                g_origSetRenderTarget(d, 1, g_target);
                if (g_target2) g_origSetRenderTarget(d, 2, g_target2);
                return hr;
            }
            return g_origClear(d, count, rects, flags, color, z, stencil);
        }

        HRESULT __stdcall hkSetRenderState(IDirect3DDevice9* d, D3DRENDERSTATETYPE state, DWORD value)
        {
            const HRESULT hr = g_origSetRenderState(d, state, value);
            if (state == D3DRS_ALPHABLENDENABLE)
            {
                g_blend = value != 0;
                UpdateMask(d);
            }
            return hr;
        }

        HRESULT __stdcall hkSetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* ps)
        {
            const HRESULT hr = g_origSetPixelShader(d, ps);
            if (g_active)
            {
                g_psWrites = ShaderOutputs(ps);
                UpdateMask(d);
            }
            return hr;
        }

        template <class Fn>
        void Swap(void** vtbl, unsigned idx, Fn hook, Fn* orig)
        {
            void* previous = nullptr;
            if (wxl::mem::SwapPointer(&vtbl[idx], reinterpret_cast<void*>(hook), &previous))
                *orig = reinterpret_cast<Fn>(previous);
        }

        void EnsureHooks(IDirect3DDevice9* d)
        {
            void** vtbl = *reinterpret_cast<void***>(d);
            if (vtbl == g_hookedVtbl && vtbl[vt::kSetPixelShader] == reinterpret_cast<void*>(&hkSetPixelShader)) return;
            if (vtbl[vt::kSetPixelShader] != reinterpret_cast<void*>(&hkSetPixelShader))
            {
                Swap(vtbl, vt::kSetRenderTarget, &hkSetRenderTarget, &g_origSetRenderTarget);
                Swap(vtbl, vt::kClear,           &hkClear,           &g_origClear);
                Swap(vtbl, vt::kSetRenderState,  &hkSetRenderState,  &g_origSetRenderState);
                Swap(vtbl, vt::kSetPixelShader,  &hkSetPixelShader,  &g_origSetPixelShader);
                g_writes.clear();
            }
            g_hookedVtbl = vtbl;
        }

        bool CheckCaps(IDirect3DDevice9* d)
        {
            if (g_capsState) return g_capsState == 1;
            D3DCAPS9 caps{};
            d->GetDeviceCaps(&caps);
            const bool rts   = caps.NumSimultaneousRTs >= 2;
            const bool masks = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_INDEPENDENTWRITEMASKS) != 0;
            const bool blend = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTPOSTPIXELSHADERBLENDING) != 0;
            const bool depth = (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS) != 0;
            g_capsState = (rts && masks && blend) ? 1 : 2;
            WLOG_INFO("mrt: caps NumSimultaneousRTs %lu, independent write masks %d, MRT blending %d, "
                      "independent bit depths %d, PS %lu.%lu -> %s",
                      caps.NumSimultaneousRTs, masks, blend, depth, (caps.PixelShaderVersion >> 8) & 0xFF,
                      caps.PixelShaderVersion & 0xFF, g_capsState == 1 ? "render target 1 available" : "refused");
            return g_capsState == 1;
        }

        // --- diagnostic -----------------------------------------------------------------------------
        IDirect3DTexture9* g_diagTex = nullptr;
        UINT               g_diagW = 0, g_diagH = 0;
        int                g_diagOn = -1;
    }

    bool Begin(void* device, void* target, void* target2, bool* albedoBound)
    {
        if (albedoBound) *albedoBound = false;
        auto* d = static_cast<IDirect3DDevice9*>(device);
        auto* t = static_cast<IDirect3DSurface9*>(target);
        if (!d || !t || !CheckCaps(d)) return false;

        IDirect3DSurface9* rt0 = nullptr;
        if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return false;
        D3DSURFACE_DESC a{}, b{};
        rt0->GetDesc(&a);
        t->GetDesc(&b);
        rt0->Release(); // the device keeps it alive; only its identity is needed
        D3DCAPS9 caps{};
        d->GetDeviceCaps(&caps);
        const bool depthsOk = a.Format == b.Format || (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS);
        if (a.Width != b.Width || a.Height != b.Height || a.MultiSampleType != D3DMULTISAMPLE_NONE ||
            b.MultiSampleType != D3DMULTISAMPLE_NONE || !depthsOk)
        {
            static bool logged = false;
            if (!logged) { logged = true; WLOG_WARN("mrt: target %ux%u ms%d does not match the world target %ux%u ms%d",
                                                    b.Width, b.Height, b.MultiSampleType, a.Width, a.Height, a.MultiSampleType); }
            return false;
        }

        // Render target 2 under the same rules: the world target's size, no multisampling, and three
        // simultaneous targets. Refused, the pass runs with render target 1 alone.
        auto* t2 = static_cast<IDirect3DSurface9*>(target2);
        if (t2)
        {
            D3DSURFACE_DESC c{};
            t2->GetDesc(&c);
            const bool ok2 = caps.NumSimultaneousRTs >= 3 && c.Width == a.Width && c.Height == a.Height
                          && c.MultiSampleType == D3DMULTISAMPLE_NONE
                          && (a.Format == c.Format || (caps.PrimitiveMiscCaps & D3DPMISCCAPS_MRTINDEPENDENTBITDEPTHS));
            if (!ok2)
            {
                static bool logged2 = false;
                if (!logged2) { logged2 = true; WLOG_WARN("mrt: render target 2 %ux%u ms%d refused (world target %ux%u, %lu targets)",
                                                          c.Width, c.Height, c.MultiSampleType, a.Width, a.Height, caps.NumSimultaneousRTs); }
                t2 = nullptr;
            }
        }

        EnsureHooks(d);
        d->ColorFill(t, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
        if (t2) d->ColorFill(t2, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));

        g_world0  = rt0;
        g_target  = t;
        g_target2 = t2;
        g_active  = true;
        g_bound   = false;
        DWORD blend = 0;
        g_blend = SUCCEEDED(d->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) && blend;
        IDirect3DPixelShader9* ps = nullptr;
        if (SUCCEEDED(d->GetPixelShader(&ps)) && ps) { g_psWrites = ShaderOutputs(ps); ps->Release(); }
        else g_psWrites = 0;
        Bind(d, true);
        if (albedoBound) *albedoBound = t2 != nullptr;

        static bool announced = false;
        if (!announced) { announced = true; WLOG_INFO("mrt: render target 1 bound for the world pass (%ux%u)", b.Width, b.Height); }
        static bool announced2 = false;
        if (t2 && !announced2) { announced2 = true; WLOG_INFO("mrt: render target 2 bound for the world pass (%ux%u)", b.Width, b.Height); }
        return true;
    }

    void End(void* device)
    {
        auto* d = static_cast<IDirect3DDevice9*>(device);
        if (!g_active || !d) return;
        Bind(d, false);
        g_origSetRenderState(d, kWriteMask1, 0xF);
        if (g_target2) g_origSetRenderState(d, kWriteMask2, 0xF);
        g_mask    = 0xFFFFFFFF;
        g_mask2   = 0xFFFFFFFF;
        g_active  = false;
        g_world0  = nullptr;
        g_target  = nullptr;
        g_target2 = nullptr;
    }

    void* DiagTarget(void* device)
    {
        if (g_diagOn < 0) g_diagOn = wxl::config::Env("WXL_DIAG_NORMALS", false) ? 1 : 0;
        auto* d = static_cast<IDirect3DDevice9*>(device);
        if (!g_diagOn || !d) return nullptr;

        IDirect3DSurface9* rt0 = nullptr;
        if (FAILED(d->GetRenderTarget(0, &rt0)) || !rt0) return nullptr;
        D3DSURFACE_DESC a{};
        rt0->GetDesc(&a);
        rt0->Release();
        if (g_diagTex && (g_diagW != a.Width || g_diagH != a.Height)) { g_diagTex->Release(); g_diagTex = nullptr; }
        if (!g_diagTex)
        {
            if (FAILED(d->CreateTexture(a.Width, a.Height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                        D3DPOOL_DEFAULT, &g_diagTex, nullptr)))
                return nullptr;
            g_diagW = a.Width;
            g_diagH = a.Height;
            WLOG_INFO("mrt: diag normal target %ux%u (WXL_DIAG_NORMALS)", a.Width, a.Height);
        }
        IDirect3DSurface9* s = nullptr;
        g_diagTex->GetSurfaceLevel(0, &s);
        if (s) s->Release(); // the texture keeps its level alive
        return s;
    }

    void OnDeviceLost()
    {
        if (g_diagTex) { g_diagTex->Release(); g_diagTex = nullptr; }
        g_writes.clear();
    }

    void DiagShow(void* device, void* target)
    {
        auto* d = static_cast<IDirect3DDevice9*>(device);
        if (g_diagOn != 1 || !d || !g_diagTex) return;
        IDirect3DSurface9* level = nullptr;
        g_diagTex->GetSurfaceLevel(0, &level);
        if (level) level->Release();
        if (level != target) return; // a subscriber's target: it shows its own

        IDirect3DStateBlock9* saved = nullptr;
        if (FAILED(d->CreateStateBlock(D3DSBT_ALL, &saved)) || !saved) return;
        struct V { float x, y, z, w, u, v; };
        const float x0 = g_diagW * 0.5f - 0.5f, y0 = g_diagH * 0.5f - 0.5f;
        const float x1 = g_diagW - 0.5f,        y1 = g_diagH - 0.5f;
        const V quad[4] = { { x0, y0, 0, 1, 0, 0 }, { x1, y0, 0, 1, 1, 0 }, { x0, y1, 0, 1, 0, 1 }, { x1, y1, 0, 1, 1, 1 } };
        d->SetVertexShader(nullptr);
        d->SetPixelShader(nullptr);
        d->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        d->SetTexture(0, g_diagTex);
        d->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        d->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        d->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        d->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        d->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_LIGHTING, FALSE);
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(V));
        saved->Apply();
        saved->Release();
    }
}
