// wxl-graphics-shadow: the debug view drawn over the finished frame (D3D9, WXL_GFX_ORDER_OVERLAY): the
// compute pass copies its debug image into an A16B16G16R16F texture made here, and this blends it on.
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

#include "Overlay.hpp"
#include "Spirv.hpp"
#include "../core/Extension.hpp"

#include "wxl/gfx/RenderState.hpp"

#include <windows.h>
#include <d3d9.h>

namespace
{
    namespace render = wxl::gfx::render;
    using wxl::gfx::shadow::Gfx;

    void*                  g_device = nullptr;
    IDirect3DTexture9*     g_tex = nullptr;
    uint32_t               g_w = 0, g_h = 0;
    IDirect3DPixelShader9* g_ps = nullptr;
    bool                   g_psFailed = false;
    bool                   g_warned = false;

    template <class T>
    void Free(T*& p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    void Follow(void* device)
    {
        if (g_device == device) return;
        Free(g_tex);
        Free(g_ps);
        g_psFailed = false;
        g_w = g_h = 0;
        g_device = device;
    }
}

namespace wxl::gfx::shadow::overlay
{
    void* Prepare(void* device, uint32_t width, uint32_t height)
    {
        IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(device);
        if (!d || !width || !height) return nullptr;
        Follow(device);
        if (g_tex && g_w == width && g_h == height) return g_tex;
        Free(g_tex);
        if (FAILED(d->CreateTexture(width, height, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_tex, nullptr)) || !g_tex)
        {
            g_tex = nullptr;
            if (!g_warned)
            {
                g_warned = true;
                SHADOW_LOG_WARN("overlay: the %ux%u debug texture was refused; no debug view", width, height);
            }
            return nullptr;
        }
        g_w = width;
        g_h = height;
        return g_tex;
    }

    void Draw(const WXL_GfxFrame& frame)
    {
        IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(frame.device);
        IDirect3DSurface9* target = static_cast<IDirect3DSurface9*>(frame.target);
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (!d || !target || !gfx || !g_tex || g_device != frame.device) return;
        if (!g_ps && !g_psFailed)
        {
            size_t size = 0;
            WXL_GfxShaderDesc desc{};
            desc.structSize = sizeof desc;
            desc.name = "wxl-graphics-shadow/shaders/d3d9/overlay.ps.hlsl";
            desc.source = spirv::D3d9Source("overlay.ps", &size);
            desc.sourceSize = size;
            desc.target = "ps_3_0";
            g_ps = desc.source ? static_cast<IDirect3DPixelShader9*>(gfx->CreatePixelShader(d, &desc)) : nullptr;
            g_psFailed = g_ps == nullptr;
            if (g_psFailed) SHADOW_LOG_WARN("overlay: the debug view's shader did not compile; no debug view");
        }
        if (!g_ps) return;
        D3DSURFACE_DESC td{};
        if (FAILED(target->GetDesc(&td)) || !td.Width || !td.Height) return;
        render::StateGuard guard(d, 1, 1);
        render::PlainState(d);
        d->SetRenderTarget(0, target);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        render::Sampler(d, 0, g_tex, false);
        const float screen[4] = { float(td.Width), float(td.Height), 1.0f / float(td.Width), 1.0f / float(td.Height) };
        d->SetPixelShaderConstantF(0, screen, 1);
        d->SetPixelShader(g_ps);
        gfx->DrawFullscreen(d, td.Width, td.Height);
    }

    void Release()
    {
        Free(g_tex);
        g_w = g_h = 0;
    }
}
