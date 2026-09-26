// wxl-graphics-fog: the D3D9 composite.
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

#include "Apply.hpp"
#include "Gpu.hpp"
#include "Spirv.hpp"

#include "wxl/gfx/GpuTimer.hpp"
#include "wxl/gfx/RenderState.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>

namespace
{
    using namespace wxl::gfx::fog;
    namespace render = wxl::gfx::render;

    constexpr uint32_t kRetryFrames = 300;

    void* g_device = nullptr;

    IDirect3DTexture9* g_fog = nullptr;
    IDirect3DTexture9* g_front = nullptr;
    IDirect3DTexture9* g_aux = nullptr;
    IDirect3DTexture9* g_debug = nullptr;
    uint32_t           g_w = 0, g_h = 0;
    bool               g_targetWarned = false;

    IDirect3DTexture9* g_scene = nullptr;
    IDirect3DSurface9* g_sceneRt = nullptr;
    UINT               g_sceneW = 0, g_sceneH = 0;
    D3DFORMAT          g_sceneFormat = D3DFMT_UNKNOWN;

    IDirect3DPixelShader9* g_ps = nullptr;
    bool                   g_psFailed = false;
    uint32_t               g_psTried = 0;

    wxl::gfx::GpuTimer g_timer;

    template <class T>
    void Free(T*& p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    void FreeTargets()
    {
        Free(g_fog);
        Free(g_front);
        Free(g_aux);
        Free(g_debug);
        g_w = g_h = 0;
    }

    void FreeScene()
    {
        Free(g_sceneRt);
        Free(g_scene);
        g_sceneW = g_sceneH = 0;
        g_sceneFormat = D3DFMT_UNKNOWN;
    }

    void FollowD3dDevice(void* device)
    {
        if (g_device == device) return;
        if (g_device)
        {
            FreeTargets();
            FreeScene();
            Free(g_ps);
            g_psFailed = false;
            g_timer.Release();
        }
        g_device = device;
    }

    bool EnsureShader(IDirect3DDevice9* d, uint32_t frameIndex)
    {
        if (g_ps) return true;
        if (g_psFailed && frameIndex - g_psTried < kRetryFrames) return false;
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (!gfx) return false;
        g_psTried = frameIndex;
        size_t size = 0;
        WXL_GfxShaderDesc desc{};
        desc.structSize = sizeof desc;
        desc.name = "wxl-graphics-fog/shaders/apply.ps.hlsl";
        desc.source = spirv::ApplySource(&size);
        desc.sourceSize = size;
        desc.entry = "main";
        desc.target = "ps_3_0";
        g_ps = static_cast<IDirect3DPixelShader9*>(gfx->CreatePixelShader(d, &desc));
        if (!g_ps)
        {
            if (!g_psFailed) FOG_LOG_WARN("apply: the composite shader failed to compile (see the graphics-extend log); retried later");
            g_psFailed = true;
            return false;
        }
        g_psFailed = false;
        FOG_LOG_INFO("apply: composite shader compiled");
        return true;
    }

    bool EnsureScene(IDirect3DDevice9* d, const D3DSURFACE_DESC& target)
    {
        if (g_scene && g_sceneW == target.Width && g_sceneH == target.Height && g_sceneFormat == target.Format) return true;
        FreeScene();
        if (FAILED(d->CreateTexture(target.Width, target.Height, 1, D3DUSAGE_RENDERTARGET, target.Format, D3DPOOL_DEFAULT,
                                    &g_scene, nullptr)) || !g_scene
            || FAILED(g_scene->GetSurfaceLevel(0, &g_sceneRt)) || !g_sceneRt)
        {
            FreeScene();
            return false;
        }
        g_sceneW = target.Width;
        g_sceneH = target.Height;
        g_sceneFormat = target.Format;
        return true;
    }
}

namespace wxl::gfx::fog::gpu::apply
{
    bool EnsureTargets(void* device, uint32_t halfW, uint32_t halfH)
    {
        IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(device);
        if (!d) return false;
        FollowD3dDevice(device);
        if (g_fog && g_front && g_aux && g_debug && g_w == halfW && g_h == halfH) return true;
        FreeTargets();
        if (FAILED(d->CreateTexture(halfW, halfH, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_fog, nullptr)) || !g_fog
            || FAILED(d->CreateTexture(halfW, halfH, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_front, nullptr)) || !g_front
            || FAILED(d->CreateTexture(halfW, halfH, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &g_aux, nullptr)) || !g_aux
            || FAILED(d->CreateTexture(halfW, halfH, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_debug, nullptr)) || !g_debug)
        {
            FreeTargets();
            if (!g_targetWarned)
            {
                g_targetWarned = true;
                FOG_LOG_WARN("apply: the D3D9 fog textures (%ux%u) were refused; nothing is composited", halfW, halfH);
            }
            return false;
        }
        g_w = halfW;
        g_h = halfH;
        g_targetWarned = false;
        return true;
    }

    void* FogTexture() { return g_fog; }
    void* FrontTexture() { return g_front; }
    void* AuxTexture() { return g_aux; }
    void* DebugTexture() { return g_debug; }

    bool Draw(const WXL_GfxFrame& frame, const Input& in)
    {
        IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(frame.device);
        IDirect3DSurface9* target = static_cast<IDirect3DSurface9*>(frame.target);
        IDirect3DTexture9* depth = static_cast<IDirect3DTexture9*>(frame.depthTexture);
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (!d || !target || !depth || !gfx) return false;
        FollowD3dDevice(frame.device);
        if (!g_fog || !g_front || !g_aux || !g_debug) return false;
        if (!EnsureShader(d, frame.frameIndex)) return false;

        D3DSURFACE_DESC td{};
        if (FAILED(target->GetDesc(&td)) || !td.Width || !td.Height) return false;

        render::StateGuard guard(d, kRegisters, 8);
        g_timer.Begin(d);
        // The composite reads the scene and writes it back in linear light, so no fixed blend is used.
        if (!EnsureScene(d, td) || FAILED(d->StretchRect(target, nullptr, g_sceneRt, nullptr, D3DTEXF_NONE)))
        {
            g_timer.End();
            return false;
        }
        render::PlainState(d);
        d->SetRenderTarget(0, target);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        render::Sampler(d, 0, depth, false);
        render::Sampler(d, 1, g_fog, false);
        render::Sampler(d, 2, g_aux, false);
        render::Sampler(d, 3, g_scene, true);
        render::Sampler(d, 4, g_debug, false);
        render::Sampler(d, 5, static_cast<IDirect3DBaseTexture9*>(gfx->Texture(d, WXL_GFX_TEX_BLUE_NOISE)), false, true);
        render::Sampler(d, 6, g_front, false);
        d->SetPixelShaderConstantF(0, &in.c[0][0], kRegisters);
        d->SetPixelShader(g_ps);
        gfx->DrawFullscreen(d, td.Width, td.Height);
        g_timer.Mark(0);
        g_timer.End();
        SetApplyMs(g_timer.Supported() ? std::max(g_timer.Milliseconds(0), 0.0f) : -1.0f);
        return true;
    }

    void Release()
    {
        FreeTargets();
        FreeScene();
        g_timer.Release();
    }
}
