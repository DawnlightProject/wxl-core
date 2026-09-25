// wxl-forever fog: the froxel volume -- injection, temporal resolve, integration and full-resolution apply.
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
#include "Froxel.hpp"
#include "Fog.hpp"
#include "../core/BlueNoise.hpp"
#include "../core/GpuTimer.hpp"
#include "../core/RenderUtil.hpp"
#include "../core/Matrix.hpp"
#include "../core/Passes.hpp"
#include "NoiseVolume.hpp"
#include "Projectiles.hpp"
#include "../lights/Lights.hpp"
#include "../core/ShaderLibrary.hpp"
#include "../lights/Cookies.hpp"
#include "../terrain/Horizon.hpp"

#include "game/Sky.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    namespace fog = wxl::forever::fog;
    namespace fx  = wxl::forever::fog::froxel;
    namespace in  = wxl::forever::fog::inputs;
    namespace fl  = wxl::forever::lights;
    namespace render = wxl::forever::render;
    using render::Sampler;
    using render::Quad;
    namespace mx  = wxl::forever::matrix;
    namespace sky = wxl::game::sky;

    constexpr UINT kRegisters   = 224;   // c0..c215, see shaders/common.hlsli; c216..c220 the resolve's own, c221 omni, c222..c223 cookies
    constexpr UINT kCookieReg   = 222;
    constexpr UINT kMovingReg   = 216;
    constexpr UINT kOmniReg     = 221;
    constexpr UINT kOmniTableW  = UINT(wxl::forever::lights::kMaxLights);   // the light service's omni table width
    constexpr UINT kProfileRows = 12;
    constexpr UINT kOutdoorReg  = 20;
    constexpr UINT kIndoorReg   = 32;
    constexpr UINT kBoxReg      = 44;
    constexpr UINT kLightReg    = 92;
    constexpr UINT kVolumeReg   = 116;
    constexpr UINT kHorizonReg  = 176;
    constexpr UINT kHorizonMapReg = 178;   // c178..c179, terrain/shaders/horizon.hlsli
    constexpr float kFogCookieFloor = 0.04f;  // least a cookie lets through in the fog
    constexpr float kUnshapedHalo   = 0.6f;   // a light no cookie shapes glows this much in the fog (a softer round halo)
    constexpr UINT kTerrainReg  = 182;
    constexpr UINT kNearReg     = 183;
    constexpr UINT kContactReg  = 185;
    constexpr UINT kRealismReg  = 186;
    constexpr UINT kFrameReg    = 191;
    constexpr UINT kSliceReg    = 192;
    constexpr UINT kViewProjReg = 193;
    constexpr UINT kShadowReg   = 197;
    constexpr UINT kOutdoorQReg = 199;
    constexpr UINT kIndoorQReg  = 204;
    constexpr UINT kQRows       = 5;
    constexpr UINT kBlueReg     = 209;
    constexpr UINT kImmersionReg = 211;
    constexpr UINT kTrailReg    = 214;
    constexpr int  kBlockSlices = 8;

    // GPU timer spans, one per pass.
    enum Span { kSpanVisibility, kSpanLamps, kSpanInject, kSpanResolve, kSpanIntegrate, kSpanCopy, kSpanApply, kSpanCount };
    const char* const kSpanNames[kSpanCount] = { "visibility", "lamps", "inject", "resolve", "integrate", "copy", "apply" };

    // Views as the shaders number them.
    constexpr float kViewCodes[] = { 0.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 11.0f, 13.0f, 14.0f, 15.0f, 16.0f,
                                     17.0f, 18.0f, 19.0f, 20.0f, 21.0f };
    constexpr int   kLastView    = int(sizeof kViewCodes / sizeof kViewCodes[0]) - 1;

    struct Grid
    {
        int w = 0, h = 0, z = 0, tiles = 0, rows = 0;
        UINT atlasW() const { return UINT(w * tiles); }
        UINT atlasH() const { return UINT(h * rows); }
        bool operator==(const Grid& o) const { return w == o.w && h == o.h && z == o.z; }
    };

    Grid                    g_grid;
    D3DFORMAT               g_format    = D3DFMT_UNKNOWN;
    bool                    g_formatChecked = false;
    bool                    g_broken    = false;   // shaders or targets failed; the caller falls back
    IDirect3DTexture9*      g_vis        = nullptr; // per froxel: sun visibility, sky visibility, indoor
    IDirect3DSurface9*      g_visRt      = nullptr;
    IDirect3DTexture9*      g_raw        = nullptr; // this frame's injection
    IDirect3DSurface9*      g_rawRt      = nullptr;
    // Before the injection, per froxel: each omni slot's visibility, then the lamps' single scattering
    // (and its mean distance) and their halo and heat, each pass with the whole register file for its loop.
    IDirect3DTexture9*      g_omniVis    = nullptr;
    IDirect3DSurface9*      g_omniVisRt  = nullptr;
    IDirect3DTexture9*      g_lamps      = nullptr;
    IDirect3DSurface9*      g_lampsRt    = nullptr;
    IDirect3DTexture9*      g_lampsHeat  = nullptr;
    IDirect3DSurface9*      g_lampsHeatRt = nullptr;
    IDirect3DTexture9*      g_history[2] = {};      // resolved volumes, this frame's and last frame's
    IDirect3DSurface9*      g_historyRt[2] = {};
    IDirect3DTexture9*      g_integrated   = nullptr;
    IDirect3DSurface9*      g_integratedRt = nullptr;
    IDirect3DTexture9*      g_blocks       = nullptr;   // eight-slice partial integrals, per column
    IDirect3DSurface9*      g_blocksRt     = nullptr;
    bool                    g_groundFilter = false;     // the device filters G32R32F: one ground fetch
    int                     g_current   = 0;
    IDirect3DVertexShader9* g_vs        = nullptr;
    IDirect3DPixelShader9*  g_psVis     = nullptr;
    IDirect3DPixelShader9*  g_psInject  = nullptr;
    IDirect3DPixelShader9*  g_psOmni    = nullptr;
    IDirect3DPixelShader9*  g_psLamps   = nullptr;
    IDirect3DPixelShader9*  g_psResolve = nullptr;
    IDirect3DPixelShader9*  g_psIntegrate = nullptr;
    IDirect3DPixelShader9*  g_psApply   = nullptr;
    IDirect3DPixelShader9*  g_psBlocks    = nullptr;
    IDirect3DPixelShader9*  g_psImmersion = nullptr;

    // How deep the camera stands in fog, one texel eased from frame to frame.
    IDirect3DTexture9*      g_immersion[2] = {};
    IDirect3DSurface9*      g_immersionRt[2] = {};
    float                   g_frameDt = 0.0f;

    // The scene as the world pass left it, read by the apply pass so the composite can run in
    // linear light instead of through the fixed blend.
    IDirect3DTexture9*      g_sceneCopy   = nullptr;
    IDirect3DSurface9*      g_sceneCopyRt = nullptr;
    UINT                    g_sceneW = 0, g_sceneH = 0;
    D3DFORMAT               g_sceneFormat = D3DFMT_UNKNOWN;

    int      g_lightCount = 0;   // lights handed to the light service this frame
    int      g_omniBound = 0;    // atlases bound on s11.. this frame
    float    g_volumeReach = 0.0f;   // yards from the camera past which no density volume reaches

    // Census: how many froxels leave injection by each path, counted once on request with occlusion
    // queries over variants of the injection shader.
    constexpr int     kCensusPaths = 5;
    IDirect3DQuery9*  g_census[kCensusPaths] = {};
    bool              g_censusRequested = false;
    bool              g_censusPending = false;
    char              g_censusLine[256] = "injection paths: not counted yet";

    void ReleaseCensus()
    {
        for (IDirect3DQuery9*& q : g_census)
            if (q) { q->Release(); q = nullptr; }
        g_censusPending = false;
    }

    /// Picks up the census counts once the GPU has them; never waits.
    void PollCensus(UINT froxels)
    {
        if (!g_censusPending) return;
        DWORD counts[kCensusPaths] = {};
        for (int i = 0; i < kCensusPaths; ++i)
            if (!g_census[i] || g_census[i]->GetData(&counts[i], sizeof counts[i], 0) != S_OK) return;
        const float total = float(std::max(froxels, 1u));
        std::snprintf(g_censusLine, sizeof g_censusLine,
                      "injection paths of %u froxels: nothing possible %.1f%%, between banks %.1f%%, empty after noise %.1f%%, "
                      "thin (no shadow steps) %.1f%%, full %.1f%%",
                      froxels, 100.0f * counts[0] / total, 100.0f * counts[1] / total, 100.0f * counts[2] / total,
                      100.0f * counts[3] / total, 100.0f * counts[4] / total);
        WLOG_INFO("fog: %s", g_censusLine);
        ReleaseCensus();
    }
    bool     g_fresh = false;   // targets just created, contents undefined until cleared
    bool     g_historyValid = false;
    float    g_prevViewProj[16] = {};
    float    g_prevEye[3] = {};
    float    g_prevFar = 0.0f;
    unsigned g_frame = 0;
    LONGLONG g_lastCount = 0;

    // The celestial inputs as fed to the shader: glare dimming undone, then eased over time.
    struct Celestial
    {
        bool  valid = false;
        float toSun[3] = { 0, 0, 1 }, toMoon[3] = { 0, 0, 1 };
        float rgb[3] = {};
        float sunWeight = 0.0f, moonWeight = 0.0f;
    };
    Celestial g_celestial;

    // What the diagnostics report: the raw inputs' spread over the last second.
    struct Spread
    {
        float glareLo = 1e9f, glareHi = -1e9f, glareStep = 0.0f, lastGlare = -1.0f;
        float readLo = 1e9f, readHi = -1e9f, fedLo = 1e9f, fedHi = -1e9f;
        void Reset() { *this = Spread{}; }
    };
    Spread g_spread;
    DWORD  g_spreadStart = 0;
    char   g_diag[256] = "diagnostics off";

    wxl::forever::GpuTimer g_timer;
    char          g_status[200] = "froxel volume idle";
    char          g_passes[256] = "passes: pending";

    const char* FormatName(D3DFORMAT f)
    {
        return f == D3DFMT_A16B16G16R16F ? "RGBA16F" : f == D3DFMT_A32B32G32R32F ? "RGBA32F" : "none";
    }

    /// A filterable float target format, half first.
    D3DFORMAT PickFormat(IDirect3DDevice9* dev)
    {
        IDirect3D9* d3d = nullptr;
        if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return D3DFMT_UNKNOWN;
        D3DDEVICE_CREATION_PARAMETERS cp{};
        dev->GetCreationParameters(&cp);
        D3DDISPLAYMODE mode{};
        d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode);

        g_groundFilter = SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                          D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_G32R32F));

        D3DFORMAT chosen = D3DFMT_UNKNOWN;
        for (D3DFORMAT f : { D3DFMT_A16B16G16R16F, D3DFMT_A32B32G32R32F })
        {
            if (SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                                 D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_FILTER,
                                                 D3DRTYPE_TEXTURE, f)))
            {
                chosen = f;
                break;
            }
        }
        d3d->Release();
        return chosen;
    }

    Grid WantedGrid()
    {
        const fog::Settings& s = fog::Config();
        Grid g;
        g.w = std::clamp(s.froxelX, 16, 320);
        g.h = std::clamp(s.froxelY, 9, 180);
        g.z = std::clamp(s.froxelZ, 8, 128);
        g.tiles = int(std::ceil(std::sqrt(double(g.z))));
        g.rows  = (g.z + g.tiles - 1) / g.tiles;
        return g;
    }

    void ReleaseTarget(IDirect3DTexture9*& tex, IDirect3DSurface9*& rt)
    {
        if (rt)  { rt->Release();  rt  = nullptr; }
        if (tex) { tex->Release(); tex = nullptr; }
    }

    void ReleaseSceneCopy()
    {
        ReleaseTarget(g_sceneCopy, g_sceneCopyRt);
        g_sceneW = g_sceneH = 0;
        g_sceneFormat = D3DFMT_UNKNOWN;
    }

    void ReleaseTargets()
    {
        ReleaseCensus();
        ReleaseSceneCopy();
        ReleaseTarget(g_blocks, g_blocksRt);
        ReleaseTarget(g_raw, g_rawRt);
        ReleaseTarget(g_omniVis, g_omniVisRt);
        ReleaseTarget(g_lamps, g_lampsRt);
        ReleaseTarget(g_lampsHeat, g_lampsHeatRt);
        ReleaseTarget(g_vis, g_visRt);
        ReleaseTarget(g_history[0], g_historyRt[0]);
        ReleaseTarget(g_history[1], g_historyRt[1]);
        ReleaseTarget(g_integrated, g_integratedRt);
        ReleaseTarget(g_immersion[0], g_immersionRt[0]);
        ReleaseTarget(g_immersion[1], g_immersionRt[1]);
        g_grid = Grid{};
        g_historyValid = false;
    }

    bool CreateSized(IDirect3DDevice9* dev, UINT w, UINT h, IDirect3DTexture9*& tex, IDirect3DSurface9*& rt)
    {
        if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, g_format, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex)
        {
            tex = nullptr;
            return false;
        }
        return SUCCEEDED(tex->GetSurfaceLevel(0, &rt)) && rt;
    }

    bool CreateTarget(IDirect3DDevice9* dev, const Grid& g, IDirect3DTexture9*& tex, IDirect3DSurface9*& rt)
    {
        return CreateSized(dev, g.atlasW(), g.atlasH(), tex, rt);
    }

    int BlockCount(const Grid& g) { return (g.z + kBlockSlices - 1) / kBlockSlices; }

    bool EnsureResources(IDirect3DDevice9* dev)
    {
        if (!g_formatChecked)
        {
            g_formatChecked = true;
            g_format = PickFormat(dev);
            WLOG_INFO("fog: froxel target format %s", FormatName(g_format));
            if (g_format == D3DFMT_UNKNOWN)
            {
                WLOG_WARN("fog: no filterable float target; froxel volume unavailable");
                g_broken = true;
            }
        }
        if (g_broken) return false;

        // Programs come from the shader library each frame, so a reload takes effect at once. A
        // missing one leaves this frame without fog rather than the volume broken for good.
        namespace sh = wxl::forever::shaders;
        g_vs             = sh::Vertex(dev, "core.fullscreen");
        g_psVis          = sh::Pixel(dev, "fog.visibility");
        g_psInject       = sh::Pixel(dev, "fog.inject");
        g_psOmni         = sh::Pixel(dev, "fog.omni");
        g_psLamps        = sh::Pixel(dev, "fog.lamps");
        g_psResolve      = sh::Pixel(dev, "fog.resolve");
        g_psIntegrate    = sh::Pixel(dev, "fog.integrate");
        g_psApply        = sh::Pixel(dev, "fog.apply");
        g_psBlocks       = sh::Pixel(dev, "fog.blocks");
        g_psImmersion    = sh::Pixel(dev, "fog.immersion");
        if (!g_vs || !g_psVis || !g_psInject || !g_psOmni || !g_psLamps || !g_psResolve || !g_psIntegrate || !g_psApply
            || !g_psBlocks || !g_psImmersion)
            return false;

        const Grid want = WantedGrid();
        if (g_integrated && g_grid == want) return true;

        ReleaseTargets();
        if (!CreateTarget(dev, want, g_raw, g_rawRt)
            || !CreateTarget(dev, want, g_omniVis, g_omniVisRt)
            || !CreateTarget(dev, want, g_lamps, g_lampsRt)
            || !CreateTarget(dev, want, g_lampsHeat, g_lampsHeatRt)
            || !CreateTarget(dev, want, g_vis, g_visRt)
            || !CreateTarget(dev, want, g_history[0], g_historyRt[0])
            || !CreateTarget(dev, want, g_history[1], g_historyRt[1])
            || !CreateTarget(dev, want, g_integrated, g_integratedRt)
            || !CreateSized(dev, UINT(want.w * BlockCount(want)), UINT(want.h), g_blocks, g_blocksRt)
            || !CreateSized(dev, 1, 1, g_immersion[0], g_immersionRt[0])
            || !CreateSized(dev, 1, 1, g_immersion[1], g_immersionRt[1]))
        {
            WLOG_WARN("fog: froxel atlas %ux%u create failed; froxel volume unavailable", want.atlasW(), want.atlasH());
            ReleaseTargets();
            g_broken = true;
            return false;
        }
        g_grid = want;
        g_fresh = true;
        WLOG_INFO("fog: froxel volume %dx%dx%d, atlas %ux%u %s", want.w, want.h, want.z,
                  want.atlasW(), want.atlasH(), FormatName(g_format));
        return true;
    }

    bool EnsureSceneCopy(IDirect3DDevice9* dev, const D3DSURFACE_DESC& target)
    {
        if (g_sceneCopy && g_sceneW == target.Width && g_sceneH == target.Height && g_sceneFormat == target.Format)
            return true;
        ReleaseSceneCopy();
        if (FAILED(dev->CreateTexture(target.Width, target.Height, 1, D3DUSAGE_RENDERTARGET, target.Format,
                                      D3DPOOL_DEFAULT, &g_sceneCopy, nullptr)) || !g_sceneCopy
            || FAILED(g_sceneCopy->GetSurfaceLevel(0, &g_sceneCopyRt)))
        {
            ReleaseSceneCopy();
            return false;
        }
        g_sceneW = target.Width;
        g_sceneH = target.Height;
        g_sceneFormat = target.Format;
        return true;
    }

    float Frac(float x) { return x - std::floor(x); }

    float Smoothstep(float a, float b, float x)
    {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    float Luma(const float c[3]) { return c[0] * 0.299f + c[1] * 0.587f + c[2] * 0.114f; }

    void Normalize(float v[3])
    {
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 1e-6f) for (int i = 0; i < 3; ++i) v[i] /= len;
    }

    /// Reads the sun and moon, undoes the engine's glare dimming and eases everything over ~0.3 s.
    void UpdateCelestial(float dt)
    {
        sky::CelestialLight light{};
        if (!sky::GetCelestialLight(light)) return;

        // The engine dims its lighting colours by the sun-glare occlusion query; the fog does not
        // want that per-frame dip, so it is divided back out before anything else.
        const float glare = std::clamp(light.glare, 0.0f, 1.0f);
        const float undo = 1.0f / std::max(1.0f - light.glareDimming * glare, 0.05f);
        float rgb[3];
        for (int i = 0; i < 3; ++i) rgb[i] = light.diffuse[i] * undo;

        const float sunW  = Smoothstep(-0.05f, 0.12f, light.toSun[2]);
        const float moonW = Smoothstep(-0.05f, 0.12f, light.toMoon[2]) * (1.0f - sunW);

        Celestial& c = g_celestial;
        const float a = c.valid ? 1.0f - std::exp(-std::max(dt, 0.0f) / 0.3f) : 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            c.toSun[i]  += (light.toSun[i]  - c.toSun[i])  * a;
            c.toMoon[i] += (light.toMoon[i] - c.toMoon[i]) * a;
            c.rgb[i]    += (rgb[i] - c.rgb[i]) * a;
        }
        Normalize(c.toSun);
        Normalize(c.toMoon);
        c.sunWeight  += (sunW - c.sunWeight) * a;
        c.moonWeight += (moonW - c.moonWeight) * a;

        if (!c.valid)
        {
            WLOG_INFO("fog: sun dir (%.3f %.3f %.3f), moon dir (%.3f %.3f %.3f), lighting dir (%.3f %.3f %.3f), "
                      "diffuse (%.2f %.2f %.2f), glare %.3f",
                      light.toSun[0], light.toSun[1], light.toSun[2], light.toMoon[0], light.toMoon[1], light.toMoon[2],
                      light.direction[0], light.direction[1], light.direction[2],
                      light.diffuse[0], light.diffuse[1], light.diffuse[2], light.glare);
        }
        c.valid = true;

        Spread& s = g_spread;
        s.glareLo = std::min(s.glareLo, light.glare);
        s.glareHi = std::max(s.glareHi, light.glare);
        if (s.lastGlare >= 0.0f) s.glareStep = std::max(s.glareStep, std::fabs(light.glare - s.lastGlare));
        s.lastGlare = light.glare;
        s.readLo = std::min(s.readLo, Luma(light.diffuse));
        s.readHi = std::max(s.readHi, Luma(light.diffuse));
        s.fedLo  = std::min(s.fedLo, Luma(c.rgb));
        s.fedHi  = std::max(s.fedHi, Luma(c.rgb));
    }

    /// Gamma to linear: the engine's colours are gamma-encoded, the fog works in linear light.
    float Lin(float c)
    {
        return std::pow(std::max(c, 0.0f), 2.2f);
    }

    void FillProfile(float (*dst)[4], const fog::ResolvedProfile& p, float t)
    {
        const Celestial& c = g_celestial;
        const fog::Settings::Isolate& iso = fog::Config().isolate;
        // Wind blows along a fixed heading; the slow vertical term keeps the noise evolving in place.
        const float wx = 0.944f, wy = 0.330f;
        const float freq = p.noiseScale * 0.25f;   // one noise tile holds four base cycles
        const float drift = p.windSpeed * t * freq;
        // The second layer runs slower and 30 degrees off the wind, and rolls upwards.
        const float drift2 = drift * p.flowSpeed;
        const bool flow = !iso.noFlow;
        const float rows[kProfileRows][4] = {
            { p.density, p.heightFalloff, p.baseZ, freq },
            { -wx * drift, -wy * drift, t * 0.005f, iso.flatDensity ? 0.0f : p.noiseStrength },
            { Lin(p.rgb[0]), Lin(p.rgb[1]), Lin(p.rgb[2]), p.ambient },
            { p.coverage, p.erosion, p.billow, p.warp * 0.5f },
            { iso.noCelestial ? 0.0f : p.sunScatter * c.sunWeight, iso.noCelestial ? 0.0f : p.moonScatter * c.moonWeight,
              p.lightScatter, p.maxScatter },
            { std::clamp(p.anisotropy, -0.95f, 0.95f), std::clamp(p.backAnisotropy, -0.95f, 0.95f),
              std::clamp(p.lobeBlend, 0.0f, 1.0f), p.detailScale * 0.25f },
            { iso.noShadow ? 0.0f : p.shadowStrength, p.shadowDistance, p.powder, p.wakeStrength },
            { p.ambientLow, p.ambientHigh, iso.noTerrain ? 0.0f : p.groundFollow, p.valleyPooling },
            { flow ? -0.652f * drift2 : 0.0f, flow ? -0.758f * drift2 : 0.0f, flow ? t * p.roll * freq : 0.0f,
              flow ? p.flowLayer : 0.0f },
            { p.contact, p.contactThickness, p.wakeSwirl, p.nearStrength },
            { p.msStrength, p.msExtinction, p.msPhase, std::floor(std::clamp(p.msOctaves, 0.0f, 3.0f) + 0.5f) },
            { p.skyAmbient, iso.noHeat ? 0.0f : std::max(p.heat, 0.0f),
              iso.noCloudLight ? 0.0f : std::clamp(p.cloudLight, 0.0f, 1.0f), 0.25f / std::max(p.cloudSize, 1.0f) },
        };
        std::memcpy(dst, rows, sizeof rows);
    }

    /// The profile's second block of rows (see shaders/common.hlsli, Q0..Q4).
    void FillProfileExtra(float (*dst)[4], const fog::ResolvedProfile& p, float t)
    {
        const fog::Settings::Isolate& iso = fog::Config().isolate;
        const float wx = 0.944f, wy = 0.330f;
        // One noise tile holds four base cycles, so a feature of s yards needs 0.25 / s tiles per yard.
        // Offsets are taken modulo the tile, which the noise repeats on, to keep their precision.
        const float macroFreq = 0.25f / std::max(p.macroScale, 1.0f);
        const float macroDrift = p.windSpeed * p.macroDrift * t * macroFreq;
        const float hazeFreq = p.noiseScale * 0.25f * 0.4f;
        const float hazeDrift = p.windSpeed * p.hazeSpeed * t * hazeFreq;
        const float rows[kQRows][4] = {
            { std::clamp(p.worldShadow, 0.0f, 1.0f), std::clamp(p.worldShadowSoftness, 0.0f, 1.0f),
              iso.noSkyOcclusion ? 0.0f : std::clamp(p.skyOcclusion, 0.0f, 1.0f),
              iso.noBankShading ? 0.0f : std::clamp(p.bankShading, 0.0f, 1.0f) },
            { macroFreq, std::max(p.macroContrast, 0.1f), 1.0f - std::clamp(p.macroCoverage, 0.0f, 1.0f),
              iso.noBanks ? 0.0f : std::clamp(p.macroStrength, 0.0f, 1.0f) },
            { Frac(-wx * macroDrift), Frac(-wy * macroDrift), std::clamp(p.edgeErosion, 0.0f, 1.0f),
              iso.noBankShading ? 0.0f : std::max(p.bankReach, 0.0f) },
            { iso.noHighHaze ? 0.0f : std::max(p.hazeDensity, 0.0f), std::max(p.hazeFalloff, 0.0f), p.hazeHeight, hazeFreq },
            { Frac(-0.87f * hazeDrift), Frac(-0.49f * hazeDrift), Frac(t * 0.01f * hazeFreq), std::clamp(p.hazeNoise, 0.0f, 1.0f) },
        };
        std::memcpy(dst, rows, sizeof rows);
    }

    /// Fills c0..c131; false when the matrices cannot be inverted.
    bool BuildConstants(const fx::FrameInput& f, float c[kRegisters][4])
    {
        const fog::Settings& s = fog::Config();
        const fog::Settings::Isolate& iso = s.isolate;
        const fog::ResolvedProfile& camProfile = f.camera;

        float inv[16];
        if (!mx::Invert4(f.viewProjRel, inv)) return false;
        mx::Columns(inv, &c[0]);

        const float view = kViewCodes[std::clamp(s.froxelView, 0, kLastView)];
        const float head[5][4] = {
            { 1.0f / float(f.depthW), 1.0f / float(f.depthH), f.seconds, view },
            { float(g_grid.w), float(g_grid.h), float(g_grid.z), float(g_grid.tiles) },
            { 1.0f / float(g_grid.atlasW()), 1.0f / float(g_grid.atlasH()), s.froxelNear,
              std::log(std::max(f.farDistance, s.froxelNear * 2.0f) / s.froxelNear) },
            { f.rangeMin, 1.0f / (f.rangeMax - f.rangeMin), f.rangeMax, 0.0f },
            // R3 low-discrepancy rotation; frozen, each froxel keeps its own fixed offset.
            { iso.freezeJitter ? 0.0f : Frac(float(g_frame % 4096 + 1) * 0.8191725f),
              iso.freezeJitter ? 0.0f : Frac(float(g_frame % 4096 + 1) * 0.6710436f),
              iso.freezeJitter ? 0.0f : Frac(float(g_frame % 4096 + 1) * 0.5497005f), camProfile.tonemapWhite },
        };
        std::memcpy(c[4], head, sizeof head);

        // The slice distribution (shaders/common.hlsli, SliceToDistL): bend b thins the near slices.
        const float bend = std::clamp(s.sliceBend, 0.0f, 0.9f);
        c[kSliceReg][0] = bend;
        c[kSliceReg][1] = 1.0f - bend;
        c[kSliceReg][2] = (1.0f - bend) * (1.0f - bend);
        c[kSliceReg][3] = bend > 0.0f ? 0.5f / bend : 0.0f;
        // A change of the distribution moves every slice: the history starts over.
        static float prevBend = -1.0f, prevNear = -1.0f;
        if (bend != prevBend || s.froxelNear != prevNear) g_historyValid = false;
        prevBend = bend;
        prevNear = s.froxelNear;

        // History is kept unless something invalidated it: new targets, a jump, a new range.
        const float moved[3] = { f.eye[0] - g_prevEye[0], f.eye[1] - g_prevEye[1], f.eye[2] - g_prevEye[2] };
        const float jump = std::sqrt(moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2]);
        // A new far distance reprojects the history into the new slices (the resolve reads last
        // frame's slice spacing).
        const bool keep = g_historyValid && jump < 30.0f;
        c[7][3] = keep && !iso.noHistory ? std::clamp(s.temporal, 0.0f, 0.98f) : 0.0f;

        mx::Columns(keep ? g_prevViewProj : f.viewProjRel, &c[9]);
        c[13][0] = moved[0]; c[13][1] = moved[1]; c[13][2] = moved[2]; c[13][3] = camProfile.exposure;
        c[14][0] = f.eye[0]; c[14][1] = f.eye[1]; c[14][2] = f.eye[2]; c[14][3] = iso.noBoxes ? 0.0f : float(f.boxCount);

        const Celestial& cel = g_celestial;
        std::memcpy(c[15], cel.toSun, sizeof cel.toSun);
        std::memcpy(c[16], cel.toMoon, sizeof cel.toMoon);
        {
            float carried[4];
            fl::CarriedConstants(carried);
            c[15][3] = carried[0];
            c[16][3] = carried[1];
        }
        c[17][0] = Lin(cel.rgb[0]); c[17][1] = Lin(cel.rgb[1]); c[17][2] = Lin(cel.rgb[2]);

        // Self-shadow marches towards whichever body lights the scene.
        float shadow[3];
        for (int i = 0; i < 3; ++i) shadow[i] = cel.toSun[i] * cel.sunWeight + cel.toMoon[i] * cel.moonWeight;
        if (shadow[0] * shadow[0] + shadow[1] * shadow[1] + shadow[2] * shadow[2] < 1e-6f) { shadow[0] = 0; shadow[1] = 0; shadow[2] = 1; }
        Normalize(shadow);
        std::memcpy(c[18], shadow, sizeof shadow);

        FillProfile(&c[kOutdoorReg], f.outdoor, f.seconds);
        FillProfile(&c[kIndoorReg], f.indoor, f.seconds);
        FillProfileExtra(&c[kOutdoorQReg], f.outdoor, f.seconds);
        FillProfileExtra(&c[kIndoorQReg], f.indoor, f.seconds);

        // World shadows march towards the same body; the tangents widen the march into a cone.
        mx::Columns(f.viewProjRel, &c[kViewProjReg]);
        {
            const float up[3] = { std::fabs(shadow[2]) > 0.95f ? 1.0f : 0.0f, 0.0f, std::fabs(shadow[2]) > 0.95f ? 0.0f : 1.0f };
            float t1[3] = { shadow[1] * up[2] - shadow[2] * up[1], shadow[2] * up[0] - shadow[0] * up[2],
                            shadow[0] * up[1] - shadow[1] * up[0] };
            Normalize(t1);
            const float t2[3] = { shadow[1] * t1[2] - shadow[2] * t1[1], shadow[2] * t1[0] - shadow[0] * t1[2],
                                  shadow[0] * t1[1] - shadow[1] * t1[0] };
            const bool lit = std::max(cel.sunWeight, cel.moonWeight) > 0.01f && !iso.noCelestial;
            const bool wanted = s.worldShadows && !iso.noWorldShadow && lit
                             && (f.outdoor.worldShadow > 0.0f || f.indoor.worldShadow > 0.0f);
            const float rows[2][4] = {
                { t1[0], t1[1], t1[2], wanted ? float(std::clamp(s.shadowSteps, 4, 12)) : 0.0f },
                { t2[0], t2[1], t2[2], s.skyOcclusion && !iso.noSkyOcclusion ? 1.0f : 0.0f },
            };
            std::memcpy(c[kShadowReg], rows, sizeof rows);
        }

        if (f.boxCount > 0 && f.boxRows)
            std::memcpy(c[kBoxReg], f.boxRows, sizeof(float) * 4 * 3 * size_t(f.boxCount));

        const int lights = f.lights && !iso.noLights ? std::clamp(f.lightCount, 0, fl::kMaxLights) : 0;
        g_lightCount = lights;
        // w: how far the camera's eased indoorness lags its actual state; cells near the camera ease by it.
        const float lightRow[4] = { float(lights), 0.0f, std::max(s.lightMargin, 0.1f),
                                    (f.cameraIndoor ? 1.0f : 0.0f) - f.indoorness };
        std::memcpy(c[kLightReg], lightRow, sizeof lightRow);
        fl::ClusterConstants(c[kLightReg + 1], c[kLightReg + 2]);

        const int volumes = f.volumes ? std::clamp(f.volumeCount, 0, in::kMaxVolumes) : 0;
        for (int i = 0; i < volumes; ++i)
        {
            const in::Volume& v = f.volumes[i];
            float (*r)[4] = &c[kVolumeReg + i * 3];
            r[0][0] = v.center[0] - f.eye[0]; r[0][1] = v.center[1] - f.eye[1];
            r[0][2] = v.center[2] - f.eye[2]; r[0][3] = float(v.shape);
            r[1][0] = std::max(v.extent[0], 0.01f); r[1][1] = std::max(v.extent[1], 0.01f);
            r[1][2] = std::max(v.extent[2], 0.01f); r[1][3] = v.density;
            r[2][0] = v.falloff;
            r[2][1] = v.carve ? 1.0f : 0.0f;
            r[2][2] = v.swirl;
            r[2][3] = v.radius;
        }
        c[19][0] = float(volumes);

        // How far from the camera any volume reaches: froxels beyond it skip the volume loop.
        float reach = 0.0f;
        for (int i = 0; i < volumes; ++i)
        {
            const in::Volume& v = f.volumes[i];
            const float dx = v.center[0] - f.eye[0], dy = v.center[1] - f.eye[1], dz = v.center[2] - f.eye[2];
            const float extent = std::sqrt(v.extent[0] * v.extent[0] + v.extent[1] * v.extent[1] + v.extent[2] * v.extent[2]);
            reach = std::max(reach, std::sqrt(dx * dx + dy * dy + dz * dz) + extent + std::max(v.radius, v.falloff) + 1.0f);
        }
        g_volumeReach = reach;

        // A froxel's width per yard of distance: the chord between the screen's left and right rays.
        auto ray = [&inv](float ndcX) {
            const float clip[4] = { ndcX, 0.0f, 0.5f, 1.0f };
            float h[4] = {};
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) h[j] += clip[i] * inv[i * 4 + j];
            float d[3] = { h[0] / h[3], h[1] / h[3], h[2] / h[3] };
            Normalize(d);
            return std::array<float, 3>{ d[0], d[1], d[2] };
        };
        const auto left = ray(-1.0f), right = ray(1.0f);
        const float chord = std::sqrt((left[0] - right[0]) * (left[0] - right[0]) + (left[1] - right[1]) * (left[1] - right[1])
                                    + (left[2] - right[2]) * (left[2] - right[2]));
        c[19][1] = chord / float(g_grid.w);
        c[19][2] = iso.noClamp ? 1.0f : 0.0f;
        c[19][3] = fl::RoomCross();

        // Past the far distance the volume goes on from its last slice (shaders/apply.ps.hlsl): the
        // outdoor profile's horizon strength, over at most so many yards towards the sky.
        c[kHorizonReg][0] = iso.noHaze ? 0.0f : std::clamp(f.outdoor.horizonHaze, 0.0f, 1.0f) * (1.0f - f.indoorness);
        c[kHorizonReg][1] = 3000.0f;

        const sky::SkyColors skyColors = sky::GetSkyColors();
        auto channel = [](uint32_t argb, int shift) { return float((argb >> shift) & 0xFF) / 255.0f; };
        float top[3], horizon[3], groundBounce[3];
        for (int i = 0; i < 3; ++i)
        {
            const int shift = 16 - 8 * i;
            top[i]     = Lin(channel(skyColors.top, shift));
            horizon[i] = Lin(channel(skyColors.horizon, shift));
            // Light the ground throws back up: dimmer than the horizon, tinted by the fog it lights.
            groundBounce[i] = (horizon[i] + Lin(f.outdoor.rgb[i])) * 0.5f * 0.3f;
        }
        static bool skyLogged = false;
        if (!skyLogged)
        {
            skyLogged = true;
            WLOG_INFO("fog: sky colours top %08X middle %08X horizon %08X",
                      skyColors.top, skyColors.middle, skyColors.horizon);
        }
        // Chromatic extinction ratios, neutral at strength 0; phase model; dither; sky gradient.
        const float chroma = std::clamp(camProfile.chroma, 0.0f, 1.0f);
        const float realism[5][4] = {
            { 1.0f + chroma * (camProfile.chromaR - 1.0f), 1.0f + chroma * (camProfile.chromaG - 1.0f),
              1.0f + chroma * (camProfile.chromaB - 1.0f), s.debugFar },
            { s.phaseModel ? 1.0f : 0.0f, s.dither ? 1.0f : 0.0f, Frac(float(g_frame % 4096) * 0.6180340f),
              std::clamp(camProfile.desaturation, 0.0f, 1.0f) },
            { top[0], top[1], top[2], 0.0f },
            { horizon[0], horizon[1], horizon[2], 0.0f },
            { groundBounce[0], groundBounce[1], groundBounce[2], 0.0f },
        };
        std::memcpy(c[kRealismReg], realism, sizeof realism);

        // Per-frame values the shaders would otherwise recompute per pixel.
        const int blocks = BlockCount(g_grid);
        const float frame[4] = { std::max(f.farDistance, s.froxelNear * 2.0f), float(blocks),
                                 1.0f / float(g_grid.w * blocks), 1.0f / float(g_grid.h) };
        std::memcpy(c[kFrameReg], frame, sizeof frame);

        const bool terrain = f.ground && !iso.noTerrain;
        c[kTerrainReg][0] = 1.0f / 4.0f;       // terrain::kCellSize
        c[kTerrainReg][1] = f.groundFallback;
        c[kTerrainReg][2] = 32.0f * 4.0f - 8.0f; // half the measured window, less a cell of margin
        c[kTerrainReg][3] = terrain ? (g_groundFilter ? 2.0f : 1.0f) : 0.0f;

        // Near wisps (in the injection's density) and contact follow the profile the camera stands in.
        const bool nearOn = s.nearField && !iso.noNear && camProfile.nearStrength > 0.0f;
        const float turbulence = f.seconds * camProfile.nearTurbulence;
        const float nearFreq = camProfile.nearScale * 0.25f;
        const float nearRows[2][4] = {
            { std::max(camProfile.nearDistance, 1.0f), std::max(camProfile.nearStrength, 0.0f), nearOn ? 1.0f : 0.0f, 0.0f },
            { -0.944f * turbulence * nearFreq, -0.330f * turbulence * nearFreq, turbulence * 0.35f * nearFreq, nearFreq },
        };
        std::memcpy(c[kNearReg], nearRows, sizeof nearRows);
        c[kContactReg][0] = camProfile.contact;
        c[kContactReg][1] = camProfile.contactThickness;
        c[kContactReg][2] = !iso.noContact && camProfile.contact > 0.0f ? 1.0f : 0.0f;
        // Across a doorway the outdoor and indoor shapes are evaluated apart and their densities
        // cross-faded, so the pattern never re-shapes; off, the shape rows blend as before.
        c[kContactReg][3] = iso.noCrossfade ? 0.0f : 1.0f;

        // Blue-noise rotation, R4 sequence: each channel walks its own low-discrepancy sequence.
        const float blueStep = float(g_frame % 4096 + 1);
        const float alpha[4] = { 0.8566749f, 0.7338919f, 0.6287067f, 0.5385973f };
        for (int i = 0; i < 4; ++i) c[kBlueReg][i] = iso.freezeJitter ? 0.0f : Frac(blueStep * alpha[i]);

        // Immersion eases in and out over about half a second; c212 stays free.
        const bool immersionOn = s.immersion && !iso.noImmersion;
        const float immersionRows[3][4] = {
            { 1.0f - std::exp(-std::max(g_frameDt, 0.0f) / 0.5f), std::max(camProfile.immersionDensity, 0.001f),
              immersionOn ? std::clamp(camProfile.immersion, 0.0f, 1.0f) : 0.0f, std::clamp(camProfile.immersionBloom, 0.0f, 1.0f) },
            { 0.0f, 0.0f, 0.0f, 0.0f },
            // Smoke's tint: a darker grey-brown, as far as the profile asks.
            { 1.0f + (0.50f - 1.0f) * camProfile.smokeDarkness, 1.0f + (0.44f - 1.0f) * camProfile.smokeDarkness,
              1.0f + (0.38f - 1.0f) * camProfile.smokeDarkness, g_volumeReach },
        };
        std::memcpy(c[kImmersionReg], immersionRows, sizeof immersionRows);

        // Projectile trails: the pool and its screen tiles, from Projectiles.cpp.
        namespace pj = fog::projectiles;
        const bool trails = f.trails && pj::Live() > 0 && pj::SegmentTexture() && pj::TileTexture();
        const float trailRows[2][4] = {
            { trails ? float(pj::Live()) : 0.0f, pj::Reach(), float(pj::kTilesX), float(pj::kTilesY) },
            { 1.0f / float(pj::kTilesX * pj::kTileSlots / 4), 1.0f / float(pj::kTilesY), 1.0f / float(pj::kMaxSegments), 0.0f },
        };
        std::memcpy(c[kTrailReg], trailRows, sizeof trailRows);

        // Lights that moved lately: the resolve clamps the history without slack around them, and
        // weighs it down where it had to be clamped past the neighbourhood's spread (the change cut).
        {
            fl::Moving moving[4];
            const int m = fl::MovingLights(moving, 4, false);
            c[kMovingReg][0] = float(m);
            c[kMovingReg][1] = iso.noChangeCut ? 0.5f : 0.3f;
            c[kMovingReg][3] = iso.noChangeCut ? 0.0f : 1.0f;
            // Last frame's slice spacing, log(far / near), for the reprojection.
            c[kMovingReg][2] = g_prevFar > 0.0f ? std::log(std::max(g_prevFar, s.froxelNear * 2.0f) / s.froxelNear) : 0.0f;
            for (int i = 0; i < m; ++i)
            {
                for (int k = 0; k < 3; ++k) c[kMovingReg + 1 + i][k] = moving[i].position[k] - f.eye[k];
                c[kMovingReg + 1 + i][3] = moving[i].radius;
            }
        }

        // Lamps in the fog: the omni shadow maps (bound in Render), bias, the hot core, the table's width.
        {
            float carried[4];
            fl::CarriedConstants(carried);
            c[kOmniReg][0] = 0.0f;   // set once the maps are bound
            c[kOmniReg][1] = std::clamp(s.omniBias, 0.0f, 2.0f);
            c[kOmniReg][2] = s.isolate.noHotCore ? 0.0f : carried[2];
            c[kOmniReg][3] = 1.0f / float(kOmniTableW);
        }
        float cookieE[4];
        fl::cookies::Constants(c[kCookieReg], c[kCookieReg + 1], cookieE);
        // In the fog a cookie keeps its full contrast: the surfaces' floor (light leaking through a
        // housing, bounced) would leave a round glow a quarter as bright all round the lamp, a ball
        // over the beams its panes throw. The fog takes at most kFogCookieFloor.
        c[kCookieReg][3] = std::min(c[kCookieReg][3], kFogCookieFloor);
        // The third cookie row (ps_3_0 ends at c223) and the softer round glow a light no cookie
        // shapes keeps (shaders/lamps.ps.hlsl).
        c[kHorizonReg + 1][0] = cookieE[0];
        c[kHorizonReg + 1][1] = kUnshapedHalo;
        return true;
    }

    /// Binds this frame's omni maps on s11.. and the light service's omni table on s4; false (nothing
    /// bound) when the maps are off or not of this frame.
    bool BindOmni(IDirect3DDevice9* d, uint32_t frame)
    {
        g_omniBound = 0;
        const fog::Settings& s = fog::Config();
        if (!s.omniShadows || s.isolate.noOmniShadows || s.isolate.noLights) return false;
        int count = 0;
        uint32_t published = 0;
        const fl::OmniSlot* slots = fl::CurrentOmni(count, published);
        if (published != frame || count <= 0 || !fl::OmniTexture()) return false;
        int bound = 0;
        for (int i = 0; i < count && i < 4; ++i)
        {
            if (!slots[i].texture || slots[i].lightIndex < 0 || slots[i].weight <= 0.0f) continue;
            Sampler(d, DWORD(11 + i), slots[i].texture, false);
            bound = i + 1;
        }
        if (!bound) return false;
        Sampler(d, 4, fl::OmniTexture(), false);
        g_omniBound = bound;
        return true;
    }

    /// Updates the once-a-second diagnostics line from the spread gathered since the last one.
    void Diagnose(const fx::FrameInput& f, float historyWeight)
    {
        const DWORD now = GetTickCount();
        if (!fog::Config().diagnostics)
        {
            g_spread.Reset();
            g_spreadStart = now;
            std::snprintf(g_diag, sizeof g_diag, "diagnostics off");
            return;
        }
        if (now - g_spreadStart < 1000) return;

        // How squarely the camera faces the sun: the centre ray against the sun direction.
        float inv[16], centre[4] = {};
        if (mx::Invert4(f.viewProjRel, inv))
        {
            const float clip[4] = { 0.0f, 0.0f, 0.5f, 1.0f };
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) centre[j] += clip[i] * inv[i * 4 + j];
        }
        float dir[3] = { centre[0], centre[1], centre[2] };
        Normalize(dir);
        const Celestial& c = g_celestial;
        const float facing = dir[0] * c.toSun[0] + dir[1] * c.toSun[1] + dir[2] * c.toSun[2];

        const Spread& s = g_spread;
        std::snprintf(g_diag, sizeof g_diag,
                      "glare %.3f..%.3f (largest step %.3f) | diffuse as read %.3f..%.3f, as fed %.3f..%.3f | "
                      "sun weight %.2f, facing sun %.3f | history %.2f",
                      s.glareLo, s.glareHi, s.glareStep, s.readLo, s.readHi, s.fedLo, s.fedHi,
                      c.sunWeight, facing, historyWeight);
        WLOG_INFO("fog: diag %s", g_diag);
        g_spread.Reset();
        g_spreadStart = now;
    }

}

namespace wxl::forever::fog::froxel
{
    bool Render(const FrameInput& f)
    {
        IDirect3DDevice9* d = f.device;
        if (!d || !f.depth || !EnsureResources(d)) return false;
        IDirect3DVolumeTexture9* noise = fog::noise::Get(d);
        IDirect3DTexture9* blue = wxl::forever::bluenoise::Get(d);
        if (!noise || !blue) return true;   // still baking (a fraction of a second after load): no fog yet

        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        const float dt = g_lastCount ? float(double(counter.QuadPart - g_lastCount) / double(frequency.QuadPart)) : 0.0f;
        g_lastCount = counter.QuadPart;
        // After a gap (another path drew, a loading screen) last frame's matrices are not last frame's.
        if (dt > 0.25f) g_historyValid = false;
        // A frame that took over 50 ms reads as the fog freezing: one line, at most every 2 s.
        {
            static DWORD lastHitch = 0;
            if (dt > 0.05f && dt <= 0.25f && GetTickCount() - lastHitch > 2000)
            {
                lastHitch = GetTickCount();
                WLOG_WARN("fog: frame hitch %.0f ms | %s | %s", dt * 1000.0f, fl::cookies::Status(), fl::Status());
            }
        }
        g_frameDt = dt;
        const DWORD now = GetTickCount();
        UpdateCelestial(dt);

        static float c[kRegisters][4];
        std::memset(c, 0, sizeof c);
        if (!BuildConstants(f, c)) return false;
        Diagnose(f, c[7][3]);

        render::StateGuard guard(d, kRegisters);
        g_timer.Begin(d);

        IDirect3DSurface9* target = nullptr;
        d->GetRenderTarget(0, &target);
        D3DSURFACE_DESC targetDesc{};
        if (target) target->GetDesc(&targetDesc);

        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);

        d->SetVertexShader(g_vs);
        d->SetFVF(D3DFVF_XYZW);
        // The omni maps go in before the upload: no constant is written once a shader is bound.
        if (BindOmni(d, wxl::forever::passes::FrameIndex())) c[kOmniReg][0] = float(g_omniBound);
        d->SetPixelShaderConstantF(0, &c[0][0], kRegisters);
        Sampler(d, 10, blue, false, true);

        if (g_fresh)
        {
            g_fresh = false;
            for (IDirect3DSurface9* rt : { g_immersionRt[0], g_immersionRt[1], g_visRt, g_rawRt, g_historyRt[0], g_historyRt[1], g_integratedRt, g_blocksRt })
            {
                d->SetRenderTarget(0, rt);
                d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
            }
        }

        const int cur  = g_current;
        const int prev = 1 - g_current;

        // Visibility: world shadows, sky occlusion and the indoor test, at the points injection samples.
        d->SetRenderTarget(0, g_visRt);
        Sampler(d, 0, f.depth, false);
        Sampler(d, 7, f.ground, g_groundFilter, true);
        // The baked horizon maps (neutral textures and strength 0 when off or not resident).
        {
            namespace hz = wxl::forever::terrain::horizon;
            hz::Bind(d, 1, 2, 3);
            float hc[2][4];
            hz::Constants(hc[0], hc[1], true);
            d->SetPixelShaderConstantF(kHorizonMapReg, &hc[0][0], 2);
        }
        d->SetPixelShader(g_psVis);
        Quad(d, g_grid.atlasW(), g_grid.atlasH());
        g_timer.Mark(kSpanVisibility);

        // The omni slots' visibility (only while a map is bound: the lamps read it then alone), then
        // the lamps (every light's cookie sampled per light), both at the points injection samples.
        if (g_omniBound > 0)
        {
            d->SetRenderTarget(0, g_omniVisRt);
            d->SetPixelShader(g_psOmni);
            Quad(d, g_grid.atlasW(), g_grid.atlasH());
        }
        d->SetRenderTarget(0, g_lampsRt);
        d->SetRenderTarget(1, g_lampsHeatRt);
        Sampler(d, 1, g_omniVis, false);
        Sampler(d, 5, g_vis, false);
        Sampler(d, 8, fl::LightTexture(), false);
        Sampler(d, 9, fl::ClusterTexture(), false);
        fl::cookies::Bind(d, 15);
        d->SetPixelShader(g_psLamps);
        Quad(d, g_grid.atlasW(), g_grid.atlasH());
        d->SetRenderTarget(1, nullptr);
        d->SetTexture(15, nullptr);
        g_timer.Mark(kSpanLamps);

        // Injection: one pixel per froxel, this frame only; the lamps' light joins by the froxel's
        // extinction.
        d->SetRenderTarget(0, g_rawRt);
        Sampler(d, 1, g_lamps, false);
        Sampler(d, 3, noise, true, true, true);
        Sampler(d, 5, g_vis, false);
        Sampler(d, 8, g_lampsHeat, false);
        Sampler(d, 2, fog::projectiles::TileTexture(), false);
        Sampler(d, 6, fog::projectiles::SegmentTexture(), false);
        d->SetPixelShader(g_psInject);
        Quad(d, g_grid.atlasW(), g_grid.atlasH());
        g_timer.Mark(kSpanInject);

        // Resolve: last frame's volume, reprojected and clamped to this frame's neighbourhood.
        d->SetRenderTarget(0, g_historyRt[cur]);
        Sampler(d, 1, g_history[prev], true);
        Sampler(d, 4, g_raw, false);
        d->SetPixelShader(g_psResolve);
        Quad(d, g_grid.atlasW(), g_grid.atlasH());

        // Census, on request: the injection variants into the raw target (read by nothing after the
        // resolve this frame), each counted by an occlusion query.
        PollCensus(UINT(g_grid.w * g_grid.h * g_grid.z));
        if (g_censusRequested && !g_censusPending)
        {
            g_censusRequested = false;
            d->SetTexture(4, nullptr);   // the resolve read the raw target from here
            d->SetRenderTarget(0, g_rawRt);
            bool issued = true;
            for (int i = 0; i < kCensusPaths && issued; ++i)
            {
                char path[4];
                std::snprintf(path, sizeof path, "%d", i + 1);
                IDirect3DPixelShader9* variant = wxl::forever::shaders::PixelVariant(d, "fog.inject", "WXL_CENSUS", path);
                issued = variant && SUCCEEDED(d->CreateQuery(D3DQUERYTYPE_OCCLUSION, &g_census[i])) && g_census[i];
                if (!issued) break;
                d->SetPixelShader(variant);
                g_census[i]->Issue(D3DISSUE_BEGIN);
                Quad(d, g_grid.atlasW(), g_grid.atlasH());
                g_census[i]->Issue(D3DISSUE_END);
            }
            if (issued) g_censusPending = true;
            else
            {
                ReleaseCensus();
                std::snprintf(g_censusLine, sizeof g_censusLine, "injection paths: the census could not run on this device");
            }
            d->SetPixelShaderConstantF(0, &c[0][0], kRegisters);
        }

        // Immersion: the fog at the camera, eased into last frame's value.
        d->SetRenderTarget(0, g_immersionRt[cur]);
        Sampler(d, 1, g_history[cur], true);
        Sampler(d, 2, g_immersion[prev], false);
        d->SetPixelShader(g_psImmersion);
        Quad(d, 1, 1);
        g_timer.Mark(kSpanResolve);

        // Integration: eight-slice blocks per column, then front to back through them.
        d->SetRenderTarget(0, g_blocksRt);
        Sampler(d, 1, g_history[cur], false);
        d->SetPixelShader(g_psBlocks);
        Quad(d, UINT(g_grid.w * BlockCount(g_grid)), UINT(g_grid.h));

        d->SetRenderTarget(0, g_integratedRt);
        Sampler(d, 2, g_blocks, false);
        d->SetPixelShader(g_psIntegrate);
        Quad(d, g_grid.atlasW(), g_grid.atlasH());
        g_timer.Mark(kSpanIntegrate);

        // Apply over the scene: the shader composites scene * transmittance + in-scatter itself, from a
        // copy of the scene, so no fixed blend is involved.
        if (!target || !EnsureSceneCopy(d, targetDesc)
            || FAILED(d->StretchRect(target, nullptr, g_sceneCopyRt, nullptr, D3DTEXF_NONE)))
        {
            g_timer.End();
            if (target) target->Release();
            return false;
        }
        g_timer.Mark(kSpanCopy);
        d->SetRenderTarget(0, target);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        // Destination alpha is left alone; the engine's own post passes may read it.
        d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        Sampler(d, 0, f.depth, false);
        Sampler(d, 1, g_history[cur], true);
        Sampler(d, 2, g_integrated, true);
        Sampler(d, 7, f.ground, g_groundFilter, true);
        Sampler(d, 8, g_sceneCopy, false);
        Sampler(d, 4, g_immersion[cur], false);
        d->SetPixelShader(g_psApply);
        Quad(d, targetDesc.Width ? targetDesc.Width : f.depthW, targetDesc.Height ? targetDesc.Height : f.depthH);
        g_timer.Mark(kSpanApply);

        g_timer.End();
        if (target) target->Release();
        for (int i = 0; i < g_omniBound; ++i) d->SetTexture(DWORD(11 + i), nullptr);

        std::memcpy(g_prevViewProj, f.viewProjRel, sizeof g_prevViewProj);
        std::memcpy(g_prevEye, f.eye, sizeof g_prevEye);
        g_prevFar = f.farDistance;
        g_historyValid = true;
        g_current = prev;
        ++g_frame;

        const float ms = g_timer.Milliseconds();
        char time[32];
        if (!g_timer.Supported()) std::snprintf(time, sizeof time, "GPU time n/a");
        else if (ms < 0.0f)       std::snprintf(time, sizeof time, "GPU time pending");
        else                      std::snprintf(time, sizeof time, "GPU %.3f ms", ms);
        std::snprintf(g_status, sizeof g_status, "froxel %dx%dx%d %s, far %.0f yd, lights %d, volumes %d | %s",
                      g_grid.w, g_grid.h, g_grid.z, FormatName(g_format), f.farDistance,
                      g_lightCount, int(c[19][0]), time);

        int written = 0;
        for (int i = 0; i < kSpanCount && written >= 0 && written < int(sizeof g_passes); ++i)
            written += std::snprintf(g_passes + written, sizeof g_passes - size_t(written), "%s%s %.3f",
                                     i ? " | " : "", kSpanNames[i], std::max(g_timer.Milliseconds(i), 0.0f));

        static DWORD lastLog = 0;
        if (ms >= 0.0f && now - lastLog > 10000)
        {
            lastLog = now;
            WLOG_INFO("fog: %s", g_status);
            WLOG_INFO("fog: passes (ms) %s", g_passes);
        }
        return true;
    }

    void ReleaseResources()
    {
        ReleaseTargets();
        fog::projectiles::ReleaseTextures();
        g_timer.Release();
        g_formatChecked = false;   // the format is checked again on the next device
        g_broken = false;
    }

    const char* Status() { return g_status; }

    const char* Diagnostics() { return g_diag; }

    const char* PassTimes() { return g_passes; }

    void RequestCensus() { g_censusRequested = true; }

    const char* Census() { return g_censusLine; }
}
