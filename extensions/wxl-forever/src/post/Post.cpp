// wxl-forever post: what happens to the finished image, last in the chain: bloom first.
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

#include "../core/ExtensionApi.hpp"
#include "../core/GpuTimer.hpp"
#include "../core/Panel.hpp"
#include "../core/Passes.hpp"
#include "../core/RenderUtil.hpp"
#include "../core/ShaderLibrary.hpp"
#include "../surface/Surface.hpp"
#include "Post.hpp"

#include "wxl/EventScript.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    namespace ev     = wxl::events;
    namespace pp     = wxl::forever::post;
    namespace render = wxl::forever::render;
    namespace ui     = wxl::forever::ui;

    constexpr UINT kRegisters = 11;  // c0..c10, see shaders/post.hlsli
    constexpr int  kMaxLevels = 6;

    enum Span { kSpanCopy, kSpanBright, kSpanDown, kSpanUp, kSpanLuminance, kSpanComposite, kSpanCount };

    struct Target
    {
        IDirect3DTexture9* tex = nullptr;
        IDirect3DSurface9* rt  = nullptr;
        UINT w = 0, h = 0;
    };

    pp::Settings           g_cfg;
    wxl::forever::GpuTimer g_timer;
    Target                 g_scene;            // the image as the chain left it
    D3DFORMAT              g_sceneFormat = D3DFMT_UNKNOWN;
    Target                 g_down[kMaxLevels];    // half, quarter ... resolution, bright pass first
    Target                 g_up[kMaxLevels - 1];  // the way back up, each the size of g_down of its level
    int                    g_levels = 0;          // levels the targets were made for
    Target                 g_hdrWorld;         // the world's FP16 colour target, supplied each pass
    bool                   g_hdrFailed = false;
    Target                 g_lum64, g_lum16, g_lumAvg, g_adapted[2];   // the eye's luminance
    int                    g_adaptCurrent = 0;
    bool                   g_adaptFresh = true;
    LONGLONG               g_lastCount = 0;
    int                    g_noBloom = 0, g_noTonemap = 0, g_noAdaptation = 0, g_noUnclip = 0;   // isolate switches
    int                    g_noCentreMetering = 0;

    /// The gentle curve on luminance: an extended Reinhard reaching 1 at the white point.
    float Gentle(float x, float white) { return x * (1.0f + x / (white * white)) / (1.0f + x); }

    /// The exposed light each curve takes to mid grey (0.18), by bisection for the gentle one.
    float MidGreyInput(int curve, float white)
    {
        if (curve == 0) return 0.1302f;
        float lo = 0.0f, hi = 4.0f;
        for (int i = 0; i < 40; ++i)
        {
            const float mid = 0.5f * (lo + hi);
            if (Gentle(mid, white) < 0.18f) lo = mid;
            else hi = mid;
        }
        return 0.5f * (lo + hi);
    }
    char                   g_passes[160] = "passes: pending";

    void Release(Target& t)
    {
        if (t.rt)  { t.rt->Release();  t.rt = nullptr; }
        if (t.tex) { t.tex->Release(); t.tex = nullptr; }
        t.w = t.h = 0;
    }

    void ReleaseAll()
    {
        Release(g_scene);
        g_sceneFormat = D3DFMT_UNKNOWN;
        for (Target& t : g_down) Release(t);
        for (Target& t : g_up) Release(t);
        Release(g_lum64);
        Release(g_lum16);
        Release(g_lumAvg);
        Release(g_adapted[0]);
        Release(g_adapted[1]);
        g_adaptFresh = true;
    }

    bool Make(IDirect3DDevice9* dev, Target& t, UINT w, UINT h, D3DFORMAT format)
    {
        if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &t.tex, nullptr)) || !t.tex
            || FAILED(t.tex->GetSurfaceLevel(0, &t.rt)))
        {
            Release(t);
            return false;
        }
        t.w = w;
        t.h = h;
        return true;
    }

    bool Ensure(IDirect3DDevice9* dev, const D3DSURFACE_DESC& target, int levels)
    {
        if (g_scene.tex && g_scene.w == target.Width && g_scene.h == target.Height && g_sceneFormat == target.Format
            && g_levels == levels) return true;
        ReleaseAll();
        if (!Make(dev, g_scene, target.Width, target.Height, target.Format)) return false;
        g_sceneFormat = target.Format;
        UINT w = std::max((target.Width + 1) / 2, 1u), h = std::max((target.Height + 1) / 2, 1u);
        for (int i = 0; i < levels; ++i)
        {
            if (!Make(dev, g_down[i], w, h, D3DFMT_A16B16G16R16F)
                || (i < levels - 1 && !Make(dev, g_up[i], w, h, D3DFMT_A16B16G16R16F)))
            {
                ReleaseAll();
                WLOG_WARN("post: FP16 bloom targets unavailable");
                return false;
            }
            w = std::max(w / 2, 1u);
            h = std::max(h / 2, 1u);
        }
        g_levels = levels;
        if (!Make(dev, g_lum64, 64, 36, D3DFMT_A16B16G16R16F) || !Make(dev, g_lum16, 16, 9, D3DFMT_A16B16G16R16F)
            || !Make(dev, g_lumAvg, 1, 1, D3DFMT_A16B16G16R16F) || !Make(dev, g_adapted[0], 1, 1, D3DFMT_A16B16G16R16F)
            || !Make(dev, g_adapted[1], 1, 1, D3DFMT_A16B16G16R16F))
        {
            ReleaseAll();
            WLOG_WARN("post: FP16 luminance targets unavailable");
            return false;
        }
        return true;
    }

    void Texel(IDirect3DDevice9* d, UINT srcW, UINT srcH, UINT dstW, UINT dstH)
    {
        const float c[4] = { 1.0f / float(srcW), 1.0f / float(srcH), 1.0f / float(dstW), 1.0f / float(dstH) };
        d->SetPixelShaderConstantF(0, c, 1);
    }

    bool Wants() { return (g_cfg.bloom && !g_noBloom) || (g_cfg.tonemap && !g_noTonemap) || g_cfg.hdrWorld; }

    /// Before the world pass: with "HDR world" on, an FP16 target the back buffer's size for the
    /// world to draw into; the draw below resolves it.
    void Begin(const wxl::forever::passes::BeginFrame& b)
    {
        if (!g_cfg.hdrWorld || !b.colorOverride || !b.sceneDepth || g_hdrFailed) return;
        D3DSURFACE_DESC dd{};
        static_cast<IDirect3DSurface9*>(b.sceneDepth)->GetDesc(&dd);
        if (!g_hdrWorld.tex || g_hdrWorld.w != dd.Width || g_hdrWorld.h != dd.Height)
        {
            Release(g_hdrWorld);
            if (!Make(b.device, g_hdrWorld, dd.Width, dd.Height, D3DFMT_A16B16G16R16F))
            {
                g_hdrFailed = true;
                WLOG_WARN("post: FP16 world target %ux%u unavailable; HDR world stays off", dd.Width, dd.Height);
                return;
            }
            WLOG_INFO("post: HDR world target %ux%u", dd.Width, dd.Height);
        }
        *b.colorOverride = g_hdrWorld.rt;
    }

    void Draw(const wxl::forever::passes::Frame& f)
    {
        IDirect3DDevice9* d = f.device;
        namespace sh = wxl::forever::shaders;
        IDirect3DVertexShader9* vs = sh::Vertex(d, "core.fullscreen");
        IDirect3DPixelShader9* psBright = sh::Pixel(d, "post.bright");
        IDirect3DPixelShader9* psDown = sh::Pixel(d, "post.down");
        IDirect3DPixelShader9* psUp = sh::Pixel(d, "post.up");
        IDirect3DPixelShader9* psComposite = sh::Pixel(d, "post.composite");
        IDirect3DPixelShader9* psLumLog = sh::Pixel(d, "post.lumlog");
        IDirect3DPixelShader9* psLumDown = sh::Pixel(d, "post.lumdown");
        IDirect3DPixelShader9* psLumAvg = sh::Pixel(d, "post.lumavg");
        IDirect3DPixelShader9* psAdapt = sh::Pixel(d, "post.adapt");
        if (!vs || !psBright || !psDown || !psUp || !psComposite || !psLumLog || !psLumDown || !psLumAvg || !psAdapt) return;

        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const float dt = g_lastCount ? std::min(float(double(now.QuadPart - g_lastCount) / double(frequency.QuadPart)), 0.25f) : 0.0f;
        g_lastCount = now.QuadPart;
        const bool bloom = g_cfg.bloom && !g_noBloom;
        const int levels = std::clamp(g_cfg.levels, 3, kMaxLevels);
        // Softness fades the fine levels: level i weighs (1 - s)^(levels - 1 - i), the widest 1;
        // the composite scales the sum back so the glow's energy does not change with it.
        const float soft = std::clamp(g_cfg.softness, 0.0f, 0.95f);
        float weights[kMaxLevels], weightSum = 0.0f;
        for (int i = 0; i < levels; ++i) { weights[i] = std::pow(1.0f - soft, float(levels - 1 - i)); weightSum += weights[i]; }
        // The up chain sums every level by its weight, each holding the bright pass's whole energy:
        // dividing by the sum keeps the glow at intensity times that energy, whatever the softness.
        const float norm = 1.0f / std::max(weightSum, 0.001f);
        const bool tonemap = g_cfg.tonemap && !g_noTonemap;
        const bool adapt = tonemap && g_cfg.adaptation && !g_noAdaptation;
        // With the world in HDR, the image read is the FP16 target (render target 0 now) and the
        // result goes to the back buffer.
        const bool hdrWorld = f.sceneColor != nullptr;
        const float unclip = g_noUnclip || hdrWorld ? 0.0f : std::clamp(g_cfg.unclip, 0.0f, 0.9f);

        // Surface lighting's light buffer, when it ran this frame.
        const wxl::forever::surface::LightBuffer lb = wxl::forever::surface::CurrentLightBuffer();
        const bool light = lb.texture && lb.frame == f.index;

        render::StateGuard guard(d, kRegisters);
        g_timer.Begin(d);
        IDirect3DSurface9* target = nullptr;
        d->GetRenderTarget(0, &target);
        if (!target) { g_timer.End(); return; }
        D3DSURFACE_DESC desc{};
        target->GetDesc(&desc);
        if (!Ensure(d, desc, levels) || FAILED(d->StretchRect(target, nullptr, g_scene.rt, nullptr, D3DTEXF_NONE)))
        {
            g_timer.End();
            target->Release();
            return;
        }
        g_timer.Mark(kSpanCopy);

        render::PlainState(d);
        d->SetVertexShader(vs);
        const int curve = std::clamp(g_cfg.curve, 0, 1);
        const float white = std::max(g_cfg.white, 1.0f);
        const float rows[10][4] = {
            { g_cfg.threshold, std::max(g_cfg.knee, 0.001f), g_cfg.lightShare, light ? 1.0f : 0.0f },
            { g_cfg.tint[0], g_cfg.tint[1], g_cfg.tint[2], bloom ? std::max(g_cfg.intensity, 0.0f) * norm : 0.0f },
            { 0.0f, 0.0f, float(std::clamp(g_cfg.view, 0, 2)), 0.0f },
            { std::max(g_cfg.minExposure, 0.01f), std::max(g_cfg.maxExposure, g_cfg.minExposure), std::max(g_cfg.key, 0.001f),
              tonemap ? std::clamp(g_cfg.filmic, 0.0f, 1.0f) : 0.0f },
            // Before the curve, the scale that keeps mid grey (0.18, after the inverse curve) where it
            // was at an exposure of 1: the curve takes 0.1302 to 0.18.
            { adapt ? 1.0f : 0.0f, MidGreyInput(curve, white) / (0.18f / (1.0f - 0.18f * unclip)), unclip, 0.0f },
            { g_adaptFresh ? 1.0f : 1.0f - std::exp(-dt * std::max(g_cfg.adaptUp, 0.01f)),
              g_adaptFresh ? 1.0f : 1.0f - std::exp(-dt * std::max(g_cfg.adaptDown, 0.01f)), 0.0f, 0.0f },
            { 1.0f, 0.0f, 0.0f, 0.0f },
            { g_cfg.stable ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f },
            // Stored so that zeros give the look before these existed: desaturation, 1 - strength.
            { float(curve), white, 1.0f - std::clamp(g_cfg.saturation, 0.0f, 1.5f),
              1.0f - std::clamp(g_cfg.adaptStrength, 0.0f, 1.0f) },
            { g_cfg.centreMetering && !g_noCentreMetering ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f },
        };
        d->SetPixelShaderConstantF(1, &rows[0][0], 10);

        if (bloom)
        {
        // Bright pass, at half resolution.
        d->SetRenderTarget(0, g_down[0].rt);
        d->SetPixelShader(psBright);
        render::Sampler(d, 0, g_scene.tex, true);
        render::Sampler(d, 1, light ? lb.texture : nullptr, true);
        Texel(d, desc.Width, desc.Height, g_down[0].w, g_down[0].h);
        render::Quad(d, g_down[0].w, g_down[0].h);
        g_timer.Mark(kSpanBright);

        // Down the pyramid.
        d->SetPixelShader(psDown);
        for (int i = 1; i < levels; ++i)
        {
            d->SetRenderTarget(0, g_down[i].rt);
            render::Sampler(d, 0, g_down[i - 1].tex, true);
            Texel(d, g_down[i - 1].w, g_down[i - 1].h, g_down[i].w, g_down[i].h);
            render::Quad(d, g_down[i].w, g_down[i].h);
        }
        g_timer.Mark(kSpanDown);

        // Back up, each level adding its own glow by its weight.
        d->SetPixelShader(psUp);
        for (int i = levels - 2; i >= 0; --i)
        {
            const Target& smaller = i == levels - 2 ? g_down[levels - 1] : g_up[i + 1];
            const float level[4] = { weights[i], 0.0f, 0.0f, 0.0f };
            d->SetPixelShaderConstantF(7, level, 1);
            d->SetRenderTarget(0, g_up[i].rt);
            render::Sampler(d, 0, smaller.tex, true);
            render::Sampler(d, 1, g_down[i].tex, true);
            Texel(d, smaller.w, smaller.h, g_up[i].w, g_up[i].h);
            render::Quad(d, g_up[i].w, g_up[i].h);
        }
        g_timer.Mark(kSpanUp);
        }

        // The eye: log luminance down to one texel, then eased into last frame's adaptation.
        if (adapt)
        {
            render::Sampler(d, 0, g_scene.tex, true);
            d->SetRenderTarget(0, g_lum64.rt);
            d->SetPixelShader(psLumLog);
            Texel(d, desc.Width, desc.Height, 64, 36);
            render::Quad(d, 64, 36);
            d->SetRenderTarget(0, g_lum16.rt);
            d->SetPixelShader(psLumDown);
            render::Sampler(d, 0, g_lum64.tex, false);
            Texel(d, 64, 36, 16, 9);
            render::Quad(d, 16, 9);
            d->SetRenderTarget(0, g_lumAvg.rt);
            d->SetPixelShader(psLumAvg);
            render::Sampler(d, 0, g_lum16.tex, false);
            Texel(d, 16, 9, 1, 1);
            render::Quad(d, 1, 1);
            if (g_adaptFresh)
                for (Target& t : g_adapted)
                {
                    d->SetRenderTarget(0, t.rt);
                    d->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 46, 46, 46), 1.0f, 0);
                }
            const int prev = g_adaptCurrent;
            g_adaptCurrent = 1 - g_adaptCurrent;
            d->SetRenderTarget(0, g_adapted[g_adaptCurrent].rt);
            d->SetPixelShader(psAdapt);
            render::Sampler(d, 0, g_lumAvg.tex, false);
            render::Sampler(d, 1, g_adapted[prev].tex, false);
            render::Quad(d, 1, 1);
            g_adaptFresh = false;
        }
        g_timer.Mark(kSpanLuminance);

        // Over the image, or from the HDR world into the back buffer.
        d->SetRenderTarget(0, hdrWorld && f.backBuffer ? static_cast<IDirect3DSurface9*>(f.backBuffer) : target);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        d->SetPixelShader(psComposite);
        render::Sampler(d, 0, g_scene.tex, false);
        render::Sampler(d, 1, bloom ? g_up[0].tex : nullptr, true);
        render::Sampler(d, 3, g_adapted[g_adaptCurrent].tex, false);
        Texel(d, g_up[0].w, g_up[0].h, desc.Width, desc.Height);
        render::Quad(d, desc.Width, desc.Height);
        g_timer.Mark(kSpanComposite);
        g_timer.End();
        target->Release();
        if (hdrWorld && f.backBuffer && f.colorResolved) *f.colorResolved = 1;

        std::snprintf(g_passes, sizeof g_passes,
                      "GPU copy %.3f | bright %.3f | down %.3f | up %.3f | eye %.3f | composite %.3f | total %.3f ms",
                      std::max(g_timer.Milliseconds(kSpanCopy), 0.0f), std::max(g_timer.Milliseconds(kSpanBright), 0.0f),
                      std::max(g_timer.Milliseconds(kSpanDown), 0.0f), std::max(g_timer.Milliseconds(kSpanUp), 0.0f),
                      std::max(g_timer.Milliseconds(kSpanLuminance), 0.0f),
                      std::max(g_timer.Milliseconds(kSpanComposite), 0.0f), std::max(g_timer.Milliseconds(), 0.0f));
    }

    void BloomTab()
    {
        ui::Check("Bloom", &g_cfg.bloom, "Makes light sources glow: lantern glass, fire and the brightest pools of lamp light spread a soft halo.");
        ui::Text(g_passes);
        ui::Slider("Threshold", &g_cfg.threshold, 0.0f, 2.0f,
                   "How bright a pixel must be to glow. Lower it to make more of the image glow, raise it to keep the glow to the sources.");
        ui::Slider("Knee", &g_cfg.knee, 0.0f, 1.0f,
                   "How gradually the glow starts around the threshold. Higher values fade it in gently.");
        ui::Slider("Intensity", &g_cfg.intensity, 0.0f, 2.0f, "How strong the glow is.");
        ui::Slider("Softness", &g_cfg.softness, 0.0f, 0.95f,
                   "How the glow spreads: at 0 every radius counts the same and sources wear a tight bright halo, towards 1 only the widest, faintest haze remains. The overall energy stays the same.");
        ui::Slider("Levels", &g_cfg.levels, 3, 6,
                   "How many halvings of the image the glow is built from. Each level doubles the widest radius; 6 reaches across a quarter of the screen.");
        ui::Check("Stable bright pass", &g_cfg.stable,
                  "Averages the four pixels behind each glow sample by their brightness, so a single hot pixel (a specular dot, a lantern edge) cannot flicker the glow as it moves.");
        ui::Color("Tint", g_cfg.tint, "The colour the glow is multiplied by. A warm tint suits torch and lamp light.");
        ui::Slider("Light buffer share", &g_cfg.lightShare, 0.0f, 1.0f,
                   "How much the brightest pools of surface lighting glow as well as the sources themselves.");
    }

    void ToneTab()
    {
        ui::Check("HDR world", &g_cfg.hdrWorld,
                  "The game draws the world with more light than the screen can show, and the filmic curve brings it down: brights keep their colour and detail. Test it before leaving it on.");
        ui::Check("Filmic tonemap", &g_cfg.tonemap,
                  "Takes the light to the screen through a filmic curve: brights roll off softly instead of clipping, colours stay rich.");
        ui::Slider("Filmic amount", &g_cfg.filmic, 0.0f, 1.0f, "Blends from the image as it was (0) to the fully filmic look (1).");
        static const char* const kCurves[] = { "ACES", "Gentle" };
        ui::Combo("Curve", &g_cfg.curve, kCurves, 2,
                  "ACES: contrasty with deep blacks; on the image the game already toned it tones twice and looks cooked. Gentle: mid grey stays put, the darks are left alone and only the brights roll off.");
        ui::Slider("White point", &g_cfg.white, 1.0f, 8.0f,
                   "Gentle curve: how much exposed light reaches pure white. Lower keeps the brights softer and the image flatter.");
        ui::Slider("Saturation", &g_cfg.saturation, 0.0f, 1.5f,
                   "Colour after the curve: 1 as it comes, lower mutes it towards grey. A little below 1 reads natural and worn.");
        ui::Check("Eye adaptation", &g_cfg.adaptation,
                  "The exposure follows the image's brightness: stepping into a dark building, the view slowly opens up; out in the sun it closes.");
        ui::Slider("Key", &g_cfg.key, 0.05f, 0.5f, "The average brightness the eye brings the image to. Raise it for a brighter look overall.");
        ui::Slider("Darkest exposure", &g_cfg.minExposure, 0.1f, 1.0f, "How far the eye may darken a very bright image.");
        ui::Slider("Brightest exposure", &g_cfg.maxExposure, 1.0f, 8.0f, "How far the eye may brighten a very dark image. Keep it low to keep nights dark.");
        ui::Slider("Adapt to brighter (per s)", &g_cfg.adaptUp, 0.1f, 10.0f, "How fast the eye adjusts when the image gets brighter.");
        ui::Slider("Adapt to darker (per s)", &g_cfg.adaptDown, 0.1f, 10.0f, "How fast the eye adjusts when the image gets darker. Low values open up slowly, like real eyes.");
        ui::Slider("Adaptation strength", &g_cfg.adaptStrength, 0.0f, 1.0f,
                   "How much of a change in brightness the eye makes up. 1 brings every view to the key, so turning towards a bright sky or lamp swings the whole image; 0.5 makes up half of it.");
        ui::Check("Centre-weighted metering", &g_cfg.centreMetering,
                  "The eye judges brightness mostly from the middle of the view, so a bright sky or a lamp at the edge of the screen barely moves the exposure as you turn.");
        ui::Slider("Open up the brights", &g_cfg.unclip, 0.0f, 0.9f,
                   "How much the brightest parts of the image are treated as brighter than the screen could show, before the curve. Has no effect once the world draws in HDR.");
    }

    void DebugTab()
    {
        static const char* const kViews[] = { "Image", "Bloom only", "Exposed luminance" };
        ui::Combo("View", &g_cfg.view, kViews, 3, "Shows the glow on its own, or the brightness the eye sees after its exposure.");
        ui::Separator();
        ui::Text("Isolate: each switch removes one ingredient.");
        ui::Check("No bloom", &g_noBloom, "Removes the glow.");
        ui::Check("No tonemap", &g_noTonemap, "Removes the filmic curve and the exposure.");
        ui::Check("No eye adaptation", &g_noAdaptation, "Keeps a fixed exposure of 1.");
        ui::Check("No inverse tonemap", &g_noUnclip, "Leaves the image's brights as they are before the filmic curve.");
        ui::Check("No centre-weighted metering", &g_noCentreMetering, "The eye weighs the whole view evenly.");
    }

    class PostModule final : public wxl::ext::EventScript
    {
    public:
        PostModule() { on<&PostModule::OnDeviceLost>(ev::Event::OnDeviceLost); }

        void OnDeviceLost(const ev::DeviceResetArgs&)
        {
            ReleaseAll();
            Release(g_hdrWorld);
            g_hdrFailed = false;
            g_timer.Release();
        }
    };
}

namespace wxl::forever::post
{
    Settings& Config() { return g_cfg; }

    void UiOverview()
    {
        ui::Scope scope("post.overview");
        ui::Check("Bloom", &g_cfg.bloom, "Makes light sources glow: lantern glass, fire and the brightest pools of lamp light spread a soft halo.");
        ui::Check("Filmic tonemap", &g_cfg.tonemap, "Takes the light to the screen through a curve, with the eye's exposure.");
        ui::Check("Eye adaptation", &g_cfg.adaptation, "The exposure follows the image's brightness.");
        ui::Text(g_passes);
    }

    void UiSettings()
    {
        ui::Scope scope("post");
        if (!ui::BeginTabs("forever.post")) return;
        if (ui::BeginTab("Bloom")) { BloomTab(); ui::EndTab(); }
        if (ui::BeginTab("Tone and exposure")) { ToneTab(); ui::EndTab(); }
        ui::EndTabs();
    }

    void UiDebug() { ui::Scope scope("post.debug"); DebugTab(); }

    void Install()
    {
        using wxl_forever::ConfigBool;
        using wxl_forever::ConfigFloat;
        g_cfg.bloom      = ConfigBool("WXL_FOREVER_POST_BLOOM", true) ? 1 : 0;
        g_cfg.threshold  = ConfigFloat("WXL_FOREVER_POST_THRESHOLD", g_cfg.threshold, 0.0f, 4.0f);
        g_cfg.knee       = ConfigFloat("WXL_FOREVER_POST_KNEE", g_cfg.knee, 0.0f, 2.0f);
        g_cfg.intensity  = ConfigFloat("WXL_FOREVER_POST_INTENSITY", g_cfg.intensity, 0.0f, 4.0f);
        g_cfg.softness   = ConfigFloat("WXL_FOREVER_POST_SOFTNESS", g_cfg.softness, 0.0f, 0.95f);
        g_cfg.levels     = int(ConfigFloat("WXL_FOREVER_POST_LEVELS", float(g_cfg.levels), 3.0f, 6.0f));
        g_cfg.stable     = ConfigBool("WXL_FOREVER_POST_STABLE", true) ? 1 : 0;
        g_cfg.lightShare = ConfigFloat("WXL_FOREVER_POST_LIGHT_SHARE", g_cfg.lightShare, 0.0f, 2.0f);
        g_cfg.tonemap    = ConfigBool("WXL_FOREVER_POST_TONEMAP", true) ? 1 : 0;
        g_cfg.filmic     = ConfigFloat("WXL_FOREVER_POST_FILMIC", g_cfg.filmic, 0.0f, 1.0f);
        g_cfg.curve      = int(ConfigFloat("WXL_FOREVER_POST_CURVE", float(g_cfg.curve), 0.0f, 1.0f));
        g_cfg.white      = ConfigFloat("WXL_FOREVER_POST_WHITE", g_cfg.white, 1.0f, 16.0f);
        g_cfg.saturation = ConfigFloat("WXL_FOREVER_POST_SATURATION", g_cfg.saturation, 0.0f, 2.0f);
        g_cfg.adaptStrength = ConfigFloat("WXL_FOREVER_POST_ADAPT_STRENGTH", g_cfg.adaptStrength, 0.0f, 1.0f);
        g_cfg.centreMetering = ConfigBool("WXL_FOREVER_POST_CENTRE_METERING", true) ? 1 : 0;
        g_cfg.adaptation = ConfigBool("WXL_FOREVER_POST_ADAPTATION", true) ? 1 : 0;
        g_cfg.key        = ConfigFloat("WXL_FOREVER_POST_KEY", g_cfg.key, 0.01f, 1.0f);
        g_cfg.minExposure = ConfigFloat("WXL_FOREVER_POST_MIN_EXPOSURE", g_cfg.minExposure, 0.05f, 1.0f);
        g_cfg.maxExposure = ConfigFloat("WXL_FOREVER_POST_MAX_EXPOSURE", g_cfg.maxExposure, 1.0f, 16.0f);
        g_cfg.adaptUp    = ConfigFloat("WXL_FOREVER_POST_ADAPT_UP", g_cfg.adaptUp, 0.05f, 20.0f);
        g_cfg.adaptDown  = ConfigFloat("WXL_FOREVER_POST_ADAPT_DOWN", g_cfg.adaptDown, 0.05f, 20.0f);
        g_cfg.unclip     = ConfigFloat("WXL_FOREVER_POST_UNCLIP", g_cfg.unclip, 0.0f, 0.9f);
        g_cfg.hdrWorld   = ConfigBool("WXL_FOREVER_POST_HDR_WORLD", false) ? 1 : 0;

        static PostModule module;
        wxl::forever::passes::Add(300, "post", &Wants, &Draw);
        wxl::forever::passes::AddBegin(&Begin);
        WLOG_INFO("post: installed (bloom=%d threshold=%.2f intensity=%.2f)", g_cfg.bloom, g_cfg.threshold, g_cfg.intensity);
    }
}
