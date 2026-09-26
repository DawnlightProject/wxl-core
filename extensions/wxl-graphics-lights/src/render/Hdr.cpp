// wxl-graphics-lights: the HDR chain's D3D9 side -- composite, interim resolve, resolve contract.
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

#include "Hdr.hpp"
#include "../core/Extension.hpp"
#include "../gpu/Shaders.hpp"

#include "wxl/gfx/GpuTimer.hpp"
#include "wxl/gfx/RenderState.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    namespace hd     = wxl::gfx::lights::hdr;
    namespace gl     = wxl::gfx::lights;
    namespace render = wxl::gfx::render;

    constexpr uint32_t kRetryFrames = 300;

    hd::Settings g_cfg;

    void* g_device = nullptr;
    IDirect3DTexture9* g_scene = nullptr;       // the copy of render target 0 the composite reads
    IDirect3DSurface9* g_sceneRt = nullptr;
    UINT               g_sceneW = 0, g_sceneH = 0;
    D3DFORMAT          g_sceneFormat = D3DFMT_UNKNOWN;

    struct Program
    {
        const char*            file;
        IDirect3DPixelShader9* ps = nullptr;
        bool                   failed = false;
        uint32_t               tried = 0;
    };
    Program g_composite{ "composite.ps.hlsl" };
    Program g_resolve{ "resolve.ps.hlsl" };

    wxl::gfx::GpuTimer g_compositeTimer, g_resolveTimer;

    // The frame's state for the contract.
    WXL_GfxHdrScene g_scene_state{};
    uint32_t    g_expandedFrame = ~0u;
    bool        g_claimed = false;
    std::string g_owner;
    char        g_status[256] = "hdr: not run yet";

    template <class T>
    void Free(T*& p)
    {
        if (p) { p->Release(); p = nullptr; }
    }

    void FreeScene()
    {
        Free(g_sceneRt);
        Free(g_scene);
        g_sceneW = g_sceneH = 0;
        g_sceneFormat = D3DFMT_UNKNOWN;
    }

    void FollowDevice(void* device)
    {
        if (g_device == device) return;
        if (g_device)
        {
            FreeScene();
            Free(g_composite.ps);
            Free(g_resolve.ps);
            g_composite.failed = g_resolve.failed = false;
            g_compositeTimer.Release();
            g_resolveTimer.Release();
        }
        g_device = device;
    }

    /// The includes the D3D9 shaders read, from the embedded table ("../wxl/lights/hdr.hlsli").
    const char* __cdecl Include(void*, const char* name, size_t* size)
    {
        if (!name) return nullptr;
        const char* at = std::strstr(name, "wxl/lights/");
        return at ? gl::shaders::Text(at, size) : nullptr;
    }

    IDirect3DPixelShader9* Shader(IDirect3DDevice9* d, Program& p, uint32_t frameIndex)
    {
        if (p.ps) return p.ps;
        if (p.failed && frameIndex - p.tried < kRetryFrames) return nullptr;
        const WXL_GraphicsExtendApi* gfx = gl::Gfx();
        if (!gfx) return nullptr;
        p.tried = frameIndex;
        size_t size = 0;
        const char* source = gl::shaders::Text(p.file, &size);
        if (!source) return nullptr;
        char name[96];
        std::snprintf(name, sizeof name, "wxl-graphics-lights/shaders/d3d9/%s", p.file);
        WXL_GfxShaderDesc desc{};
        desc.structSize = sizeof desc;
        desc.name = name;
        desc.source = source;
        desc.sourceSize = size;
        desc.entry = "main";
        desc.target = "ps_3_0";
        desc.include = &Include;
        p.ps = static_cast<IDirect3DPixelShader9*>(gfx->CreatePixelShader(d, &desc));
        if (!p.ps)
        {
            if (!p.failed) LIGHTS_LOG_WARN("hdr: %s failed to compile (see the graphics-extend log); retried later", p.file);
            p.failed = true;
            return nullptr;
        }
        p.failed = false;
        LIGHTS_LOG_INFO("hdr: %s compiled", p.file);
        return p.ps;
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

    float Frac(double x) { return float(x - std::floor(x)); }

    /// The blue-noise tile's offset this frame (an R2 sequence), so the dither never stands still in a pattern.
    void NoiseOffset(uint32_t frameIndex, float& x, float& y)
    {
        const uint32_t n = frameIndex % 1024u;
        x = std::floor(Frac(0.5 + double(n) * 0.7548776662) * 128.0f);
        y = std::floor(Frac(0.5 + double(n) * 0.5698402910) * 128.0f);
    }

    // --- the C ABI ---------------------------------------------------------------------------------------

    int __cdecl ApiScene(WXL_GfxHdrScene* out)
    {
        if (!out || !g_scene_state.structSize) return 0;
        const uint32_t size = std::min<uint32_t>(out->structSize, sizeof(WXL_GfxHdrScene));
        if (size < sizeof(uint32_t)) return 0;
        WXL_GfxHdrScene copy = g_scene_state;
        copy.structSize = size;
        std::memcpy(out, &copy, size);
        return 1;
    }

    int __cdecl ApiClaimResolve(const char* owner)
    {
        g_owner = owner && owner[0] ? owner : "another pass";
        if (!g_claimed) LIGHTS_LOG_INFO("hdr: %s claimed the resolve; the interim resolve stands down", g_owner.c_str());
        g_claimed = true;
        return 1;
    }

    const char* __cdecl ApiStatus(void) { return g_status; }

    const WXL_GraphicsLightsResolveApi kApi = {
        sizeof(WXL_GraphicsLightsResolveApi),
        WXL_GRAPHICS_LIGHTS_RESOLVE_API_VERSION,
        &ApiScene,
        &ApiClaimResolve,
        &ApiStatus,
    };

    void UpdateStatus(bool hdr, bool expanded)
    {
        const char* who = g_scene_state.resolvedBy == WXL_GFX_RESOLVED_LIGHTS ? "the interim resolve"
                        : g_scene_state.resolvedBy == WXL_GFX_RESOLVED_OTHER ? (g_claimed ? g_owner.c_str() : "an earlier pass")
                        : "nobody yet";
        std::snprintf(g_status, sizeof g_status, "hdr: world %s, %s, knee %.2f, exposure %.2f, resolved by %s%s",
                      hdr ? "in FP16" : "in 8-bit", expanded ? "expanded" : "not expanded", g_cfg.knee, g_cfg.exposure, who,
                      g_claimed ? " (claimed)" : "");
    }
}

namespace wxl::gfx::lights::hdr
{
    Settings& Config() { return g_cfg; }

    void Install()
    {
        g_cfg.hdr = ConfigBool("WXL_GFX_LIGHTS_HDR", g_cfg.hdr != 0) ? 1 : 0;
        g_cfg.knee = ConfigFloat("WXL_GFX_LIGHTS_HDR_KNEE", g_cfg.knee, 0.2f, 0.9f);
        g_cfg.exposure = ConfigFloat("WXL_GFX_LIGHTS_HDR_EXPOSURE", g_cfg.exposure, 0.1f, 8.0f);
        g_cfg.dither = ConfigBool("WXL_GFX_LIGHTS_HDR_DITHER", g_cfg.dither != 0) ? 1 : 0;
        g_api->PublishInterface(WXL_GRAPHICS_LIGHTS_RESOLVE_API_NAME, WXL_GRAPHICS_LIGHTS_RESOLVE_API_VERSION,
                                const_cast<WXL_GraphicsLightsResolveApi*>(&kApi));
        LIGHTS_LOG_INFO("hdr: %s, knee %.2f, exposure %.2f, dither %d", g_cfg.hdr ? "FP16 world, resolved at the end" : "off (8-bit, tonemapped in the composite)",
                        g_cfg.knee, g_cfg.exposure, g_cfg.dither);
    }

    bool Composite(const WXL_GfxFrame& f, void* light, bool view)
    {
        IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(f.device);
        IDirect3DSurface9* target = static_cast<IDirect3DSurface9*>(f.target);
        const WXL_GraphicsExtendApi* gfx = Gfx();
        const bool hdr = (f.available & WXL_GFX_NEED_HDR) && f.sceneColorTexture && f.target != f.backBuffer;
        g_scene_state = WXL_GfxHdrScene{};
        g_scene_state.structSize = sizeof g_scene_state;
        g_scene_state.frameIndex = f.frameIndex;
        g_scene_state.hdr = hdr ? 1 : 0;
        g_scene_state.gamma = kGamma;
        g_scene_state.knee = g_cfg.knee;
        g_scene_state.exposure = g_cfg.exposure;
        g_scene_state.claimed = g_claimed ? 1 : 0;
        if (!d || !target || !gfx) return false;
        FollowDevice(f.device);
        IDirect3DPixelShader9* ps = Shader(d, g_composite, f.frameIndex);
        if (!ps) return false;
        D3DSURFACE_DESC td{};
        if (FAILED(target->GetDesc(&td)) || !td.Width || !td.Height) return false;

        render::StateGuard guard(d, 4, 3);
        g_compositeTimer.Begin(d);
        if (!EnsureScene(d, td) || FAILED(d->StretchRect(target, nullptr, g_sceneRt, nullptr, D3DTEXF_NONE)))
        {
            g_compositeTimer.End();
            return false;
        }
        render::PlainState(d);
        d->SetRenderTarget(0, target);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        auto* noise = static_cast<IDirect3DBaseTexture9*>(gfx->Texture(d, WXL_GFX_TEX_BLUE_NOISE));
        render::Sampler(d, 0, g_scene, false);
        render::Sampler(d, 1, static_cast<IDirect3DBaseTexture9*>(light), false);
        render::Sampler(d, 2, noise, false, true);
        float nx = 0.0f, ny = 0.0f;
        NoiseOffset(f.frameIndex, nx, ny);
        const float c[4][4] = {
            { float(td.Width), float(td.Height), 1.0f / float(td.Width), 1.0f / float(td.Height) },
            { g_cfg.knee, kGamma, hdr ? 1.0f : 0.0f, g_cfg.exposure },
            { nx, ny, g_cfg.dither && noise ? 1.0f : 0.0f, view ? 1.0f : 0.0f },
            { light ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f },
        };
        d->SetPixelShaderConstantF(0, &c[0][0], 4);
        d->SetPixelShader(ps);
        gfx->DrawFullscreen(d, td.Width, td.Height);
        g_compositeTimer.Mark(0);
        g_compositeTimer.End();
        if (hdr) g_expandedFrame = f.frameIndex;
        g_scene_state.expanded = hdr ? 1 : 0;
        UpdateStatus(hdr, hdr);
        return true;
    }

    void Resolve(const WXL_GfxFrame& f, bool view)
    {
        if (g_expandedFrame != f.frameIndex || !f.sceneColorTexture || !f.backBuffer) return;
        if (f.colorResolved && *f.colorResolved)
        {
            g_scene_state.resolvedBy = WXL_GFX_RESOLVED_OTHER;
            UpdateStatus(true, true);
            return;
        }
        if (g_claimed) return;
        IDirect3DDevice9* d = static_cast<IDirect3DDevice9*>(f.device);
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (!d || !gfx) return;
        FollowDevice(f.device);
        IDirect3DPixelShader9* ps = Shader(d, g_resolve, f.frameIndex);
        if (!ps) return;
        D3DSURFACE_DESC bd{};
        auto* back = static_cast<IDirect3DSurface9*>(f.backBuffer);
        if (FAILED(back->GetDesc(&bd)) || !bd.Width || !bd.Height) return;

        render::StateGuard guard(d, 3, 2);
        g_resolveTimer.Begin(d);
        render::PlainState(d);
        d->SetRenderTarget(0, back);
        auto* noise = static_cast<IDirect3DBaseTexture9*>(gfx->Texture(d, WXL_GFX_TEX_BLUE_NOISE));
        render::Sampler(d, 0, static_cast<IDirect3DBaseTexture9*>(f.sceneColorTexture), false);
        render::Sampler(d, 1, noise, false, true);
        float nx = 0.0f, ny = 0.0f;
        NoiseOffset(f.frameIndex, nx, ny);
        const float c[3][4] = {
            { float(bd.Width), float(bd.Height), 1.0f / float(bd.Width), 1.0f / float(bd.Height) },
            { g_cfg.knee, kGamma, g_cfg.exposure, view ? 1.0f : 0.0f },
            { nx, ny, g_cfg.dither && noise && !view ? 1.0f : 0.0f, 0.0f },
        };
        d->SetPixelShaderConstantF(0, &c[0][0], 3);
        d->SetPixelShader(ps);
        gfx->DrawFullscreen(d, bd.Width, bd.Height);
        g_resolveTimer.Mark(0);
        g_resolveTimer.End();
        if (f.colorResolved) *f.colorResolved = 1;
        g_scene_state.resolvedBy = WXL_GFX_RESOLVED_LIGHTS;
        UpdateStatus(true, true);
    }

    bool Claimed() { return g_claimed; }

    float CompositeMs() { return g_compositeTimer.Supported() ? g_compositeTimer.Milliseconds(0) : -1.0f; }
    float ResolveMs() { return g_resolveTimer.Supported() ? g_resolveTimer.Milliseconds(0) : -1.0f; }

    const char* Status() { return g_status; }

    void OnDeviceLost()
    {
        FreeScene();
        g_compositeTimer.Release();
        g_resolveTimer.Release();
        g_expandedFrame = ~0u;
    }
}
