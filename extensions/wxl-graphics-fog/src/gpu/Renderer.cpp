// wxl-graphics-fog: one frame of the fog -- the plan (which clipmap levels step, how many transport
// steps run), the constants, every pass recorded in order, the copies for the composite.
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

#include "Renderer.hpp"
#include "../api/Threat.hpp"
#include "Apply.hpp"
#include "Gpu.hpp"
#include "../core/Settings.hpp"
#include "../sim/Primitives.hpp"
#include "../terrain/Block.hpp"

#include "wxl/gfx/Matrix.hpp"
#include "game/Shadows.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    using namespace wxl::gfx::fog;
    using namespace wxl::gfx::fog::gpu;
    namespace mx = wxl::gfx::matrix;
    namespace vkh = wxl::gfx::vk;

    // The four levels (docs/design.md, section 1).
    constexpr float kCellXY[FOG_LEVELS] = { 0.5f, 2.0f, 8.0f, 32.0f };
    constexpr float kCellZ[FOG_LEVELS]  = { 0.5f, 1.0f, 2.0f, 4.0f };
    constexpr float kHMin[FOG_LEVELS]   = { -1.0f, -2.0f, -4.0f, -8.0f };
    constexpr float kSettleDt = 0.4f;     // simulated seconds per warm-up step of the rivers
    constexpr int   kSettleSteps = 16;    // warm-up steps per frame at most

    struct Level
    {
        int    ox = 0, oy = 0, prevOx = 0, prevOy = 0;
        double acc = 0.0;
        bool   valid = false, step = false, reset = false;
        float  dt = 0.0f;
    };

    Level    g_level[FOG_LEVELS];
    double   g_transAcc = 0.0;
    double   g_warmupLeft = 0.0;
    bool     g_transAll = true;        // the next transport step initialises every tile
    uint64_t g_initMask = 0;           // tiles waiting for their first transport step
    int      g_transSteps = 0, g_settleSteps = 0;
    int      g_tracer = 0;                 // the cascade share image holding the last step's

    // The wake fluid: two levels around the player, stepped at a fixed rate.
    constexpr float  kWakeCell[FOG_WAKE_LEVELS] = { 0.25f, 1.0f };
    constexpr double kWakeDt = 1.0 / 30.0;
    constexpr int    kWakeJacobi = 12;
    struct WakeLevel { int ox = 0, oy = 0, prevOx = 0, prevOy = 0; };
    WakeLevel g_wakeLevel[FOG_WAKE_LEVELS];
    double    g_wakeAcc = 0.0;
    int       g_wakeSteps = 0, g_wakeCur = 0;
    bool      g_wakeValid = false;
    double    g_wakeLastSplat = -1e9;      // the fluid is read only while something stirred it lately
    uint32_t g_generation = 0;         // the device the state belongs to
    bool     g_restart = true;
    int      g_hist = 0, g_lamp = 0;
    bool     g_historyValid = false, g_lampHistory = false;
    double   g_sunVisClock = -100.0;
    double   g_gridUntil = -1.0;       // the lamp grid keeps running until then (rooms fade out)
    uint32_t g_wantW = 0, g_wantH = 0;
    double   g_bankDrift[2] = {}, g_detailDrift[3] = {}, g_curlDrift[3] = {}, g_airDrift[3] = {};
    float    g_hazeGround = 0.0f;
    bool     g_hazeGroundSet = false;
    float    g_immersion = 0.0f;

    bool          g_recorded = false;
    apply::Input  g_apply;
    char g_status[256] = "fog: idle";
    char g_times[512] = "passes: pending";
    char g_sim[256] = "simulation: idle";

    float Frac(double x) { return float(x - std::floor(x)); }
    float Lin(float c) { return std::pow(std::max(c, 0.0f), 2.2f); }
    float Luma(const float c[3]) { return c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f; }

    bool Views(int v, int a, int b) { return v >= a && v <= b; }
    bool MapView(int v)
    {
        return v == FOG_VIEW_SLICE_TOP || v == FOG_VIEW_SLICE_SIDE || v == FOG_VIEW_LAYER || v == FOG_VIEW_FLOW
            || v == FOG_VIEW_SOURCES || v == FOG_VIEW_TERRAIN || v == FOG_VIEW_WAKE || v == FOG_VIEW_CASCADE;
    }

    /// The fog's albedo: fog droplets scatter nearly all the light they meet, so its colour is a hue
    /// (the zone's, the custom colour's, or a blend) carried at an albedo of 0.85 times brightness.
    void Albedo(const float custom[4], int mode, float blend, float brightness, const float zone[3], float out[3])
    {
        const float c[3] = { Lin(custom[0]), Lin(custom[1]), Lin(custom[2]) };
        const float t = mode == int(ColorMode::Native) ? 0.0f : mode == int(ColorMode::Custom) ? 1.0f : std::clamp(blend, 0.0f, 1.0f);
        const float cl = std::max(Luma(c), 1e-3f), zl = std::max(Luma(zone), 1e-3f);
        for (int k = 0; k < 3; ++k)
        {
            const float hue = zone[k] / zl + (c[k] / cl - zone[k] / zl) * t;
            out[k] = std::clamp(hue * 0.85f * brightness, 0.0f, 1.0f);
        }
    }

    void Plan(const FrameInput& in)
    {
        const Settings& s = Config();
        const bool restart = g_restart || in.jumped;
        for (int L = 0; L < FOG_LEVELS; ++L)
        {
            Level& lv = g_level[L];
            const double period = double(1u << L) / std::max(double(s.clipRate), 1.0);
            lv.step = false;
            lv.reset = false;
            lv.acc += in.dt;
            if (!lv.valid || restart)
            {
                lv.step = lv.reset = true;
                lv.acc = 0.0;
            }
            else if (lv.acc >= period)
            {
                lv.step = true;
                lv.acc -= period;
                if (lv.acc > 2.0 * period) lv.acc = 0.0;
            }
            lv.dt = float(period);
            if (!lv.step) continue;
            const int nx = int(std::floor(in.eye[0] / kCellXY[L])) - FOG_LEVEL_N / 2;
            const int ny = int(std::floor(in.eye[1] / kCellXY[L])) - FOG_LEVEL_N / 2;
            lv.prevOx = lv.reset ? nx : lv.ox;
            lv.prevOy = lv.reset ? ny : lv.oy;
            lv.ox = nx;
            lv.oy = ny;
            lv.valid = true;
        }

        // The wake fluid: whole-cell moves of its windows, fixed-rate steps (two at most a frame).
        g_wakeSteps = 0;
        if (s.wakes)
        {
            g_wakeAcc += in.dt;
            g_wakeSteps = int(std::min(g_wakeAcc / kWakeDt, 2.0));
            g_wakeAcc -= g_wakeSteps * kWakeDt;
            if (g_wakeAcc > 2.0 * kWakeDt) g_wakeAcc = 0.0;
            if (restart || !g_wakeValid) g_wakeSteps = std::max(g_wakeSteps, 1);
            // The windows move only with a step: the state always matches the window it was made in.
            for (int k = 0; k < FOG_WAKE_LEVELS && g_wakeSteps > 0; ++k)
            {
                WakeLevel& w = g_wakeLevel[k];
                const int nx = int(std::floor(in.wakeCentre[0] / kWakeCell[k])) - FOG_WAKE_N / 2;
                const int ny = int(std::floor(in.wakeCentre[1] / kWakeCell[k])) - FOG_WAKE_N / 2;
                // After a restart nothing of the old window is kept: its origin is put far away.
                w.prevOx = g_wakeValid && !restart ? w.ox : nx + 100000;
                w.prevOy = g_wakeValid && !restart ? w.oy : ny + 100000;
                w.ox = nx;
                w.oy = ny;
            }
            if (g_wakeSteps > 0) g_wakeValid = true;
        }
        else
        {
            g_wakeValid = false;
        }

        // The rivers: a fixed rate of simulated time, and warm-up steps after new tiles or a restart.
        const Transport& t = s.transport;
        g_transSteps = g_settleSteps = 0;
        if (restart)
        {
            g_transAll = true;
            g_initMask = 0;
            g_warmupLeft = t.warmup;
        }
        const uint64_t arrived = terrain::TakeInitMask();
        if (arrived)
        {
            g_initMask |= arrived;
            g_warmupLeft = std::max(g_warmupLeft, double(t.warmup) * 0.5);
        }
        if (!t.enabled || terrain::ResidentMask() == 0)
        {
            g_transAcc = 0.0;
            return;
        }
        const double stepDt = 1.0 / std::max(double(t.rate), 1.0);
        g_transAcc += in.dt * t.timeScale;
        g_transSteps = int(std::min(g_transAcc / stepDt, 4.0));
        g_transAcc -= g_transSteps * stepDt;
        if (g_transAcc > 4.0 * stepDt) g_transAcc = 0.0;
        if (g_warmupLeft > 0.0)
        {
            g_settleSteps = std::min(kSettleSteps, int(std::ceil(g_warmupLeft / kSettleDt)));
            g_warmupLeft -= g_settleSteps * double(kSettleDt);
        }
        if ((g_transAll || g_initMask) && g_transSteps + g_settleSteps == 0) g_transSteps = 1;
    }

    /// The engine's sun cascades, imported for this block, and their rows for camera-relative points.
    int BindCascades(const WXL_GfxVulkanApi* api, const FrameInput& in, FogConstants& c, WXL_GfxVkImage out[3])
    {
        namespace sh = wxl::game::shadows;
        const Settings& s = Config();
        if (!s.worldShadows || s.worldShadow <= 0.0f) return 0;
        sh::Snapshot snap;
        if (!sh::Get(snap) || snap.mode < 3) return 0;
        // p_view = (r + eye - cameraPos, 1) * view: the translation row absorbs the eye's offset.
        float view[16];
        mx::Copy(snap.view, view);
        const float d[3] = { in.eye[0] - snap.cameraPos[0], in.eye[1] - snap.cameraPos[1], in.eye[2] - snap.cameraPos[2] };
        for (int j = 0; j < 4; ++j) view[12 + j] += d[0] * snap.view[j] + d[1] * snap.view[4 + j] + d[2] * snap.view[8 + j];
        int bands = 0;
        for (int b = 0; b < 3; ++b)
        {
            const sh::Map& m = snap.maps[size_t(sh::Slot::Band0) + size_t(b)];
            if (!m.present || !m.texture) continue;
            if (!api->ImportTexture(m.texture, &out[bands]) || out[bands].image == VK_NULL_HANDLE) continue;
            for (int k = 0; k < 3; ++k)
            {
                float* row = c.cascade[bands * 3 + k];
                for (int i = 0; i < 4; ++i)
                {
                    float v = 0.0f;
                    for (int j = 0; j < 3; ++j) v += view[i * 4 + j] * m.rows[k][j];
                    row[i] = v;
                }
                row[3] += m.rows[k][3];
            }
            ++bands;
        }
        Set(c.cascadeInfo, float(bands), snap.hwPcf ? 1.0f : 0.0f, 0.0f, 0.0f);
        return bands;
    }

    struct LightInputs
    {
        WXL_GfxVkImage light{}, cluster{}, omniTable{}, cookie{}, omni[4]{};
        int  count = 0;
        int  omniBound = 0;
        bool mono = false;
        bool ready = false;
        int  rooms = 0;
    };

    void GatherLights(const WXL_GfxVulkanApi* api, const FrameInput& in, FogConstants& c, LightInputs& li)
    {
        const Settings& s = Config();
        const WXL_GraphicsLightsApi* lights = Lights();
        if (!lights) return;
        int count = 0;
        uint32_t published = 0;
        lights->Current(&count, &published);
        const bool current = published == in.frame->frameIndex;
        void* lt = lights->LightTexture();
        void* ct = lights->ClusterTexture();
        void* ot = lights->OmniTexture();
        void* ck = lights->CookieAtlas();
        li.ready = s.lamps && current && count > 0 && lt && ct && ot && ck && api->ImportTexture(lt, &li.light)
                && api->ImportTexture(ct, &li.cluster) && api->ImportTexture(ot, &li.omniTable) && api->ImportTexture(ck, &li.cookie)
                && li.light.image && li.cluster.image && li.omniTable.image && li.cookie.image;
        if (li.ready)
        {
            li.count = count;
            li.mono = li.cookie.format == VK_FORMAT_R8_UNORM;
            if (s.omniShadows)
            {
                WXL_GfxOmniSlot slots[4] = {};
                const int n = std::clamp(lights->OmniSlots(slots, 4), 0, 4);
                for (int i = 0; i < n; ++i)
                {
                    if (!slots[i].texture || slots[i].lightIndex < 0) continue;
                    if (api->ImportTexture(slots[i].texture, &li.omni[i]) && li.omni[i].image) li.omniBound = i + 1;
                    else li.omni[i] = WXL_GfxVkImage{};
                }
            }
            lights->ClusterConstants(c.clusterC, c.clusterD);
            lights->CookieConstants(c.cookieC, c.cookieD, c.cookieE);
            lights->CarriedConstants(c.carried);
        }
        const float* rows = nullptr;
        const int rooms = s.indoorDetect && !(s.isolate & FOG_ISO_NO_INDOOR) ? lights->Rooms(&rows) : 0;
        li.rooms = rows ? std::clamp(rooms, 0, FOG_MAX_ROOMS) : 0;
        if (li.rooms > 0) std::memcpy(c.rooms, rows, sizeof(float) * 12 * size_t(li.rooms));
        Set(c.roomInfo, float(li.rooms), lights->RoomCross(), 0.0f, 0.0f);
    }

    void BuildConstants(const FrameInput& in, FogConstants& c, const LightInputs& li, bool gridOn, int cascades)
    {
        const Settings& s = Config();
        const OutdoorProfile& o = Outdoor();
        const IndoorProfile& n = Indoor();
        const WXL_GfxFrame& f = *in.frame;
        const Images& img = Img();
        const uint32_t frameNo = f.frameIndex;

        Set(c.eye, in.eye[0], in.eye[1], in.eye[2], Frac(in.clock / 4096.0) * 4096.0f);
        const float r2x = Frac(0.5 + double(frameNo % 1024u) * 0.7548776662), r2y = Frac(0.5 + double(frameNo % 1024u) * 0.5698402910);
        Set(c.frame, float(frameNo % 1024u), in.dt, std::floor(r2x * 128.0f), std::floor(r2y * 128.0f));
        Set(c.prevEye, f.view.prevEye[0], f.view.prevEye[1], f.view.prevEye[2], 0.0f);
        mx::Columns(f.view.viewProjRel, c.viewProj);
        mx::Columns(f.view.invViewProjRel, c.invViewProj);
        mx::Columns(f.view.reprojectRel, c.reproject);
        Set(c.depthRange, f.depthRange[0], 1.0f / std::max(f.depthRange[1] - f.depthRange[0], 1e-6f), f.depthRange[1], 0.0f);
        std::memcpy(c.depthLin, f.view.depthLinearize, sizeof c.depthLin);
        Set(c.screenFull, float(f.width), float(f.height), 1.0f / float(std::max(f.width, 1u)), 1.0f / float(std::max(f.height, 1u)));
        Set(c.screenHalf, float(img.halfW), float(img.halfH), 1.0f / float(std::max(img.halfW, 1u)), 1.0f / float(std::max(img.halfH, 1u)));
        Set(c.screenQuarter, float(img.quarterW), float(img.quarterH), 1.0f / float(std::max(img.quarterW, 1u)),
            1.0f / float(std::max(img.quarterH, 1u)));
        Set(c.march, s.nearRange, std::max(s.farDistance, s.nearRange * 2.0f), float(s.nearSteps), float(s.farSteps));
        // The fraying lives in the near march: it fades out by the near range at the latest.
        Set(c.march2, std::min(s.detailDistance, s.nearRange), 1e-4f, s.startDistance, std::max(in.intensity, 0.0f));
        {
            const float move = std::sqrt((in.eye[0] - f.view.prevEye[0]) * (in.eye[0] - f.view.prevEye[0])
                                       + (in.eye[1] - f.view.prevEye[1]) * (in.eye[1] - f.view.prevEye[1])
                                       + (in.eye[2] - f.view.prevEye[2]) * (in.eye[2] - f.view.prevEye[2]));
            const float speed = in.dt > 0.0f ? move / in.dt : 0.0f;
            const float fresh = 1.0f - std::clamp(s.temporal, 0.0f, 0.98f);
            Set(c.temporal, fresh + (0.6f - fresh) * std::clamp(speed / 60.0f, 0.0f, 1.0f), s.clipGamma,
                g_historyValid ? 1.0f : 0.0f, s.rejectDepth);
        }

        for (int L = 0; L < FOG_LEVELS; ++L)
        {
            const Level& lv = g_level[L];
            Set(c.levelA[L], kCellXY[L], kCellZ[L], kHMin[L], 1.0f / kCellXY[L]);
            Set(c.levelB[L], float(lv.ox), float(lv.oy), float(lv.prevOx), float(lv.prevOy));
            Set(c.levelC[L], lv.dt, lv.reset ? 1.0f : 0.0f, 3.0f, L > 0 ? s.restrict : 0.0f);
        }

        const uint64_t resident = terrain::ResidentMask();
        Set(c.block, float(terrain::OriginX()), float(terrain::OriginY()), float(FOG_TEXEL_YARDS), in.groundFallback);
        Set(c.blockMask, Bits(uint32_t(resident)), Bits(uint32_t(resident >> 32)), float(terrain::Resident()), resident ? 1.0f : 0.0f);
        Set(c.blockInit, Bits(uint32_t(g_initMask)), Bits(uint32_t(g_initMask >> 32)), g_transAll ? 1.0f : 0.0f, 0.0f);

        const Transport& t = s.transport;
        Set(c.transA, t.drainage, t.friction, t.windPush, 0.0f);
        Set(c.transB, t.formation / 60.0f, t.dayShare, t.hollowBoost, t.waterBoost);
        Set(c.transC, t.decay / 60.0f, t.sunDecay / 60.0f, t.windScour / 60.0f, t.poolDepth);
        Set(c.transD, 0.0f, t.maxSpeed, t.initialFill, in.windSpeed);

        // The sun and the moon: the active body's direction and colour.
        const float sw = in.sunWeight, mw = in.moonWeight;
        Set(c.sunDir, in.toSun[0], in.toSun[1], in.toSun[2], sw);
        Set(c.moonDir, in.toMoon[0], in.toMoon[1], in.toMoon[2], mw);
        float dir[3] = { in.toSun[0] * sw + in.toMoon[0] * mw, in.toSun[1] * sw + in.toMoon[1] * mw, in.toSun[2] * sw + in.toMoon[2] * mw };
        float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        if (len < 1e-4f) { dir[0] = 0.0f; dir[1] = 0.0f; dir[2] = 1.0f; len = 1.0f; }
        const float bodies = std::clamp(sw + mw, 0.0f, 1.0f);
        Set(c.lightDir, dir[0] / len, dir[1] / len, dir[2] / len, bodies);
        const float scatter = (o.sunScatter * sw + o.moonScatter * mw) / std::max(sw + mw, 1e-3f);
        Set(c.lightColor, in.lightRgb[0] * scatter, in.lightRgb[1] * scatter, in.lightRgb[2] * scatter, 0.0f);
        // The sky light: the sky's gradient tinted towards the zone's fog colour.
        float amb[3];
        for (int k = 0; k < 3; ++k)
        {
            const float sky = 0.5f * (in.skyTop[k] + in.skyHorizon[k]);
            amb[k] = (sky + (in.zoneFog[k] - sky) * std::clamp(o.zoneTint, 0.0f, 1.0f)) * o.ambient;
        }
        Set(c.skyZenith, amb[0], amb[1], amb[2], 0.0f);
        Set(c.skyHorizon, in.skyHorizon[0], in.skyHorizon[1], in.skyHorizon[2], 0.0f);
        Set(c.zoneFog, in.zoneFog[0], in.zoneFog[1], in.zoneFog[2], 0.0f);
        Set(c.phase, std::clamp(o.anisotropy, 0.0f, 0.95f), std::clamp(o.backAnisotropy, -0.95f, 0.0f), std::clamp(o.lobeBlend, 0.0f, 1.0f),
            std::clamp(o.powder, 0.0f, 1.0f));
        Set(c.scatterMs, o.msStrength, float(std::clamp(s.msOctaves, 0, 3)), o.skyShadow, o.softWhite);
        Set(c.shadow, o.selfShadow, std::clamp(s.terrainShadow, 0.0f, 1.0f), s.penumbra * 3.14159265f / 180.0f,
            cascades > 0 ? std::clamp(s.worldShadow, 0.0f, 1.0f) : 0.0f);

        // The outdoor profile.
        if (!g_hazeGroundSet || in.jumped)
        {
            g_hazeGround = in.groundFallback;
            g_hazeGroundSet = true;
        }
        g_hazeGround += (in.groundFallback - g_hazeGround) * std::min(in.dt / 30.0f, 1.0f);
        Set(c.outA, o.density, o.groundDensity, o.groundHeight, o.hazeDensity);
        Set(c.outB, g_hazeGround + o.hazeHeight, o.hazeFalloff, o.bankCoverage, o.bankStrength);
        float albedo[3];
        Albedo(o.color, o.colorMode, o.colorBlend, o.brightness, in.zoneFog, albedo);
        Set(c.outC, albedo[0], albedo[1], albedo[2], 0.0f);
        Set(c.outD, o.ambient, o.ambientFloor, o.valleyDarkening, o.lampScatter);
        Set(c.outE, o.sunScatter, o.moonScatter, o.exposure, 0.0f);
        Set(c.outF, o.shapeContrast, o.denseContrast, o.topSoftness, o.topBillow);
        Set(c.outG, in.windDir[0], in.windDir[1], in.windSpeed, o.turbulence);
        const float dark = std::clamp(o.smokeDarkness, 0.0f, 1.0f);
        Set(c.outH, albedo[0] * (1.0f - dark * 0.50f), albedo[1] * (1.0f - dark * 0.56f), albedo[2] * (1.0f - dark * 0.62f),
            std::max(o.smokeDensity, 0.0f));
        Set(c.outI, 1.0f / std::max(o.renewal, 0.5f), s.heat ? o.heat : 0.0f, o.erosion, o.curl);
        Set(c.outJ, o.shapeScale, o.detailScale, o.bankScale, o.layerFlow);
        Set(c.outK, std::max(o.climb, 0.0f), std::max(o.wakePush, 0.0f) / 1.5f, 0.0f, 0.0f);
        for (int k = 0; k < FOG_WAKE_LEVELS; ++k)
        {
            const WakeLevel& w = g_wakeLevel[k];
            Set(c.wakeA[k], float(w.ox), float(w.oy), float(w.prevOx), float(w.prevOy));
            Set(c.wakeB[k], kWakeCell[k], 1.0f / kWakeCell[k], 0.0f, 0.0f);
        }
        // Bodies shadowing lamps in the fog.
        {
            const int n = in.occluders ? std::clamp(in.occluderCount, 0, FOG_MAX_OCCLUDERS) : 0;
            if (n > 0) std::memcpy(c.occl, in.occluders, sizeof(float) * 4 * size_t(n));
            const bool masks = in.occluderMasks && in.maskCount > 0;
            if (masks) std::memcpy(c.occMask, in.occluderMasks, sizeof(uint32_t) * size_t(std::min(in.maskCount, 128)));
            Set(c.occInfo, o.bodyShadow > 0.0f ? float(n) : 0.0f, std::clamp(o.bodyShadow, 0.0f, 1.0f), masks ? 1.0f : 0.0f, 0.0f);
        }

        // Cascades: the baked banks, and the API's pours on this map.
        {
            const bool on = s.cascades && t.enabled;
            Set(c.pourA, on ? std::max(o.cascadeStrength, 0.0f) : 0.0f, std::max(o.cascadeDepth, 0.0f), std::clamp(o.cascadeThreshold, 0.0f, 0.95f),
                std::clamp(o.cascadeWind, 0.0f, 1.0f));
            float rows[FOG_MAX_POURS * 8] = {};
            const int pours = on ? threat::Cascades(in.mapId, rows, FOG_MAX_POURS) : 0;
            std::memcpy(c.pour, rows, sizeof(float) * 8 * size_t(pours));
            Set(c.pourB, on ? 1.0f / std::max(o.cascadeEvaporation, 1.0f) : 0.0f, std::max(o.cascadeStreaks, 0.0f), float(pours),
                std::max(o.minDepth, 0.0f));
        }
        Set(c.torchA, std::max(o.torchFloor, 0.0f), 0.35f, 1.0f / std::max(o.torchRecovery, 0.5f), 0.0f);
        Set(c.wakeC, float(kWakeDt), std::max(o.wakeDamping, 0.0f), 1.0f / std::max(o.wakeRefill, 0.5f), std::max(o.wakeVorticity, 0.0f));
        if (in.splatCount > 0) g_wakeLastSplat = in.clock;
        const bool stirred = in.clock - g_wakeLastSplat < 3.0 * std::max(o.wakeRefill, 1.0f);
        Set(c.wakeD, float(std::clamp(in.splatCount, 0, FOG_MAX_BODIES)), std::max(o.wakeHeight, 0.0f), std::clamp(o.wakeStrength, 0.0f, 1.0f),
            s.wakes && g_wakeValid && stirred ? 1.0f : 0.0f);
        // Radians a half-resolution pixel spans (the projection's y scale is cot(fov / 2)).
        const float pixelAngle = 2.0f / (std::max(std::fabs(f.view.projection[5]), 0.1f) * float(std::max(img.halfH, 1u)));
        Set(c.detailA, 0.4f, pixelAngle, 0.0f, 0.0f);

        // The atmosphere: its own body light, sky light and albedo; the ground it rests on.
        {
            const float airBody = (o.airSun * sw + o.airMoon * mw) / std::max(sw + mw, 1e-3f);
            float airAlbedo[3], airSky[3];
            for (int k = 0; k < 3; ++k)
            {
                const float sky = 0.5f * (in.skyTop[k] + in.skyHorizon[k]);
                airSky[k] = (sky + (in.zoneFog[k] - sky) * std::clamp(o.zoneTint, 0.0f, 1.0f)) * o.airSky;
                airAlbedo[k] = 0.85f + (albedo[k] - 0.85f) * std::clamp(o.airColorShare, 0.0f, 1.0f);
            }
            const bool airOn = o.airDensity > 0.0f && !(s.isolate & FOG_ISO_NO_AIR);
            Set(c.airA, airOn ? o.airDensity : 0.0f, 1.0f / std::max(o.airHeight, 1.0f), g_hazeGround, std::clamp(o.airFollow, 0.0f, 1.0f));
            Set(c.airB, std::clamp(o.airNoise, 0.0f, 1.0f), 1.0f / std::max(o.airNoiseScale, 10.0f), std::clamp(o.airAnisotropy, 0.0f, 0.95f),
                o.airBase);
            Set(c.airC, airAlbedo[0], airAlbedo[1], airAlbedo[2], 0.5f * o.valleyDarkening);
            Set(c.airD, in.lightRgb[0] * airBody, in.lightRgb[1] * airBody, in.lightRgb[2] * airBody, o.airLamp);
            Set(c.airE, airSky[0], airSky[1], airSky[2], 0.0f);
            const double scale = std::max(double(o.airNoiseScale), 10.0);
            for (int k = 0; k < 2; ++k) g_airDrift[k] -= double(in.windDir[k]) * in.windSpeed * in.dt / scale;
            g_airDrift[2] += 0.004 * in.dt;
            Set(c.airF, Frac(g_airDrift[0]), Frac(g_airDrift[1]), Frac(g_airDrift[2]), 0.0f);
        }

        // The indoor profile.
        Set(c.inA, n.density, n.floorHaze, n.floorHazeHeight, n.noiseStrength);
        float inAlbedo[3];
        Albedo(n.color, n.colorMode, n.colorBlend, n.brightness, in.zoneFog, inAlbedo);
        Set(c.inB, inAlbedo[0], inAlbedo[1], inAlbedo[2], 0.0f);
        Set(c.inC, n.ambient, n.lampScatter, n.noiseScale, n.drift);
        Set(c.inD, std::clamp(n.anisotropy, 0.0f, 0.95f), std::clamp(n.backAnisotropy, -0.95f, 0.0f), std::clamp(n.lobeBlend, 0.0f, 1.0f),
            n.seep);
        const float lag = std::clamp(std::fabs(in.indoorEased - in.indoorActual) * 20.0f, 0.0f, 1.0f);
        Set(c.indoorState, in.indoorEased, in.indoorActual, s.indoorLagRadius, s.indoorDetect ? lag : 0.0f);

        // Noise: boiling offsets (the pattern evolves in place), drifts integrated frame by frame.
        const double clock = in.clock;
        const double d0[3] = { 0.71, 0.43, 0.25 }, d1[3] = { -0.38, 0.81, 0.18 };
        // A billow of the fine tile (period 16 billows) is replaced in about 5 s, one of the coarse (8 x) in 40 s.
        Set(c.boil[0], Frac(clock * 0.0125 * d0[0]), Frac(clock * 0.0125 * d0[1]), Frac(clock * 0.0125 * d0[2]), std::max(o.shapeScale, 0.25f) * 16.0f);
        Set(c.boil[1], Frac(clock * 0.0016 * d1[0]), Frac(clock * 0.0016 * d1[1]), Frac(clock * 0.0016 * d1[2]), std::max(o.shapeScale, 0.25f) * 128.0f);
        const double dust = clock * double(n.drift) / std::max(double(n.noiseScale), 0.5);
        Set(c.boil[2], Frac(-dust * 0.8), Frac(-dust * 0.6), Frac(dust * 0.1), n.noiseScale);
        const double bank = std::max(double(o.bankScale), 10.0);
        const double detail = std::max(double(o.detailScale), 0.05) * 4.0;
        for (int k = 0; k < 2; ++k)
        {
            g_bankDrift[k] -= double(in.windDir[k]) * in.windSpeed * in.dt / bank;
            g_detailDrift[k] -= double(in.windDir[k]) * in.windSpeed * 0.5 * in.dt / detail;
        }
        g_detailDrift[2] -= 0.05 * in.dt / detail;
        const double curlStep[3] = { 0.011, 0.017, 0.013 };
        for (int k = 0; k < 3; ++k) g_curlDrift[k] += curlStep[k] * in.dt;
        Set(c.drift, Frac(g_bankDrift[0]), Frac(g_bankDrift[1]), Frac(g_detailDrift[0]), Frac(g_detailDrift[1]));
        Set(c.drift2, Frac(g_detailDrift[2]), Frac(g_curlDrift[0]), Frac(g_curlDrift[1]), Frac(g_curlDrift[2]));

        // Lamps and the grid.
        Set(c.lights, li.ready && !(s.isolate & FOG_ISO_NO_LAMPS) ? float(li.count) : 0.0f, li.mono ? 1.0f : 0.0f, float(li.omniBound),
            gridOn ? 1.0f : 0.0f);
        const float gridNear = 0.5f, gridFar = std::max(s.gridFar, 10.0f);
        const float keep = g_lampHistory && in.dt > 0.0f ? std::exp(-in.dt / std::max(s.classifyTime, 0.05f)) : 0.0f;
        Set(c.lampGrid, float(img.lampX), float(img.lampY), float(img.lampZ), keep);
        Set(c.lampGrid2, gridNear, 1.0f / std::log(gridFar / gridNear), gridFar, s.omniBias);

        Set(c.prims, float(prims::Count()), 0.0f, 0.0f, 0.0f);
        Set(c.debug, float(s.view), float(std::clamp(s.debugLevel, 0, FOG_LEVELS - 1)), s.debugHeight, s.debugFar);
        Set(c.debug2, Bits(s.isolate), 0.0f, 0.0f, 0.0f);
    }

    /// The primitives' rows and a bin mask per level cell block of the new windows.
    void BuildPrims(float* rows, uint32_t (*bins)[4])
    {
        const int count = prims::Count();
        const prims::Prim* list = prims::List();
        for (int i = 0; i < count; ++i) std::memcpy(rows + size_t(i) * 20, list[i].rows, sizeof list[i].rows);
        std::memset(bins, 0, sizeof(uint32_t) * 4 * FOG_LEVELS * FOG_BINS * FOG_BINS);
        const int span = FOG_LEVEL_N / FOG_BINS;
        for (int L = 0; L < FOG_LEVELS; ++L)
        {
            const Level& lv = g_level[L];
            const float s = kCellXY[L];
            for (int i = 0; i < count; ++i)
            {
                const prims::Prim& p = list[i];
                // Too small for this level's cells: the finer levels carry it.
                if (p.size < 0.35f * s && L > 0) continue;
                const int x0 = std::clamp(int(std::floor((p.lo[0] / s - float(lv.ox)) / float(span))), 0, FOG_BINS - 1);
                const int x1 = std::clamp(int(std::floor((p.hi[0] / s - float(lv.ox)) / float(span))), 0, FOG_BINS - 1);
                const int y0 = std::clamp(int(std::floor((p.lo[1] / s - float(lv.oy)) / float(span))), 0, FOG_BINS - 1);
                const int y1 = std::clamp(int(std::floor((p.hi[1] / s - float(lv.oy)) / float(span))), 0, FOG_BINS - 1);
                if (p.hi[0] / s < float(lv.ox) || p.lo[0] / s > float(lv.ox + FOG_LEVEL_N)
                    || p.hi[1] / s < float(lv.oy) || p.lo[1] / s > float(lv.oy + FOG_LEVEL_N))
                    continue;
                for (int y = y0; y <= y1; ++y)
                    for (int x = x0; x <= x1; ++x)
                        bins[L * FOG_BINS * FOG_BINS + y * FOG_BINS + x][i >> 5] |= 1u << (i & 31);
            }
        }
    }

    const WXL_GfxVkImage* Or(const WXL_GfxVkImage& img, const WXL_GfxVkImage& neutral)
    {
        return img.image != VK_NULL_HANDLE ? &img : &neutral;
    }

    PushData Push(uint32_t a0, uint32_t a1 = 0, float b0 = 0.0f)
    {
        PushData p{};
        p.a[0] = a0;
        p.a[1] = a1;
        p.b[0] = b0;
        return p;
    }

    bool RecordImpl(const WXL_GfxVkFrame& vk, const FrameInput& in)
    {
        const WXL_GfxVulkanApi* api = Vk();
        const Settings& s = Config();
        if (!api || !vk.cmd || !in.frame) return false;
        const WXL_GfxFrame& frame = *in.frame;
        if (!FollowDevice(api) || !EnsurePipelines(api) || !Img().persistent)
        {
            std::snprintf(g_status, sizeof g_status, "fog: waiting for the device, the pipelines or the images");
            return false;
        }
        if (Dev().generation != g_generation)
        {
            g_generation = Dev().generation;
            terrain::Forget();
            g_restart = true;
            g_historyValid = false;
            g_lampHistory = false;
        }
        if (vk.depth.image == VK_NULL_HANDLE)
        {
            std::snprintf(g_status, sizeof g_status, "fog: no depth this frame (multisampling on, or INTZ refused)");
            return false;
        }
        Images& img = Img();
        g_wantW = frame.width;
        g_wantH = frame.height;
        if (img.halfW != std::max((frame.width + 1) / 2, 1u) || img.halfH != std::max((frame.height + 1) / 2, 1u))
        {
            std::snprintf(g_status, sizeof g_status, "fog: march targets being made for %ux%u", frame.width, frame.height);
            g_historyValid = false;
            return false;
        }

        FrameBindings& fb = Frame();
        fb.api = api;
        fb.cmd = vk.cmd;
        fb.samplers[0] = api->Sampler(WXL_GFX_VK_SAMPLER_POINT_CLAMP);
        fb.samplers[1] = api->Sampler(WXL_GFX_VK_SAMPLER_LINEAR_CLAMP);
        fb.samplers[2] = api->Sampler(WXL_GFX_VK_SAMPLER_LINEAR_WRAP);
        fb.samplers[3] = api->Sampler(WXL_GFX_VK_SAMPLER_POINT_WRAP);
        for (VkSampler sm : fb.samplers)
            if (sm == VK_NULL_HANDLE) return false;

        PollProbe(vk.slot);
        BeginTimers(vk.cmd, vk.slot);
        ClearFresh(api, vk.cmd);

        // The terrain block follows the camera; arrived tiles are uploaded before anything reads them.
        terrain::Update(in.eye);
        terrain::RecordUploads(api, vk.cmd, 4);
        const bool floorDirty = terrain::TakeFloorDirty();
        Plan(in);

        // Lights, rooms and cascades, then the constants every pass reads.
        static FogConstants c;
        std::memset(&c, 0, sizeof c);
        LightInputs li;
        GatherLights(api, in, c, li);
        const bool gridWanted = (li.ready || li.rooms > 0) && (s.lamps || s.indoorDetect);
        if (gridWanted) g_gridUntil = in.clock + 2.0;
        const bool gridOn = in.clock < g_gridUntil && img.lampX > 0;
        if (gridWanted && (img.lampX != uint32_t(s.gridX) || img.lampY != uint32_t(s.gridY) || img.lampZ != uint32_t(s.gridZ)))
            g_lampHistory = false;   // Prepare makes the grid at its size for the next frame
        WXL_GfxVkImage cascade[3] = {};
        const int cascades = BindCascades(api, in, c, cascade);
        BuildConstants(in, c, li, gridOn, cascades);

        if (!api->AllocUniform(sizeof c, &fb.constants) || !fb.constants.mapped) return false;
        std::memcpy(fb.constants.mapped, &c, sizeof c);
        const VkDeviceSize primBytes = sizeof(float) * 20 * FOG_MAX_PRIMS;
        const VkDeviceSize binBytes = sizeof(uint32_t) * 4 * FOG_LEVELS * FOG_BINS * FOG_BINS;
        if (!api->AllocUniform(primBytes, &fb.prims) || !fb.prims.mapped || !api->AllocUniform(binBytes, &fb.bins) || !fb.bins.mapped)
            return false;
        BuildPrims(static_cast<float*>(fb.prims.mapped), static_cast<uint32_t(*)[4]>(fb.bins.mapped));

        WXL_GfxVkImage blue{};
        api->SharedTexture(WXL_GFX_TEX_BLUE_NOISE, &blue);
        const WXL_GfxVkImage& n2 = img.neutral2D;
        const WXL_GfxVkImage& n3 = img.neutral3D;

        // Noise, once per device.
        if (!img.noiseBaked)
        {
            const WXL_GfxVkImage* outs[3] = { &img.shape, &img.detail, &img.curl };
            for (uint32_t k = 0; k < 3; ++k)
            {
                Bind b;
                b.out[0] = outs[k];
                const uint32_t g = vkh::Groups(k == 0 ? 128u : 32u, 4u);
                Dispatch(FOG_PIPE_NOISE, b, Push(k), g, g, g);
            }
            img.noiseBaked = true;
            FOG_LOG_INFO("gpu: noise textures baked on the GPU");
        }

        // The floor and its mips, and the ground's sun visibility.
        {
            Bind b;
            b.tex[0] = &img.height;
            b.tex[1] = &img.water;
            b.tex[2] = &img.floor;
            b.out[1] = &img.sunVis;
            if (floorDirty)
            {
                b.outView[0] = img.floorMip[0];
                Dispatch(FOG_PIPE_TERRAIN, b, Push(0), FOG_BLOCK_N / 8, FOG_BLOCK_N / 8, 1);
                for (uint32_t m = 1; m < FOG_BLOCK_MIPS; ++m)
                {
                    b.outView[0] = img.floorMip[m];
                    const uint32_t size = std::max(uint32_t(FOG_BLOCK_N) >> m, 1u);
                    Dispatch(FOG_PIPE_TERRAIN, b, Push(1, m), vkh::Groups(size, 8), vkh::Groups(size, 8), 1);
                }
            }
            if (floorDirty || in.clock - g_sunVisClock > 1.0 || in.clock < g_sunVisClock)
            {
                b.outView[0] = img.floorMip[0];
                Dispatch(FOG_PIPE_TERRAIN, b, Push(2), FOG_SUNVIS_N / 8, FOG_SUNVIS_N / 8, 1);
                g_sunVisClock = in.clock;
            }
        }
        MarkTimer(vk.cmd, kSpanTerrain);

        // The rivers: warm-up steps first (longer), then the fixed-rate ones.
        {
            Bind b;
            b.tex[0] = &img.floor;
            b.tex[1] = &img.sky;
            b.tex[2] = &img.water;
            b.tex[3] = &img.sunVis;
            b.tex[4] = &img.cascadeMap;
            b.out[0] = &img.flux;
            b.out[1] = &img.layer;
            const uint32_t g = FOG_BLOCK_N / 8;
            const float stepDt = 1.0f / std::max(s.transport.rate, 1.0f);
            const int total = g_settleSteps + g_transSteps;
            for (int i = 0; i < total; ++i)
            {
                const float dt = i < g_settleSteps ? kSettleDt : stepDt;
                const uint32_t init = i == 0 && (g_transAll || g_initMask) ? 1u : 0u;
                Dispatch(FOG_PIPE_TRANSPORT, b, Push(0, init, dt), g, g, 1);
                b.tex[5] = &img.tracer[g_tracer];
                b.out[2] = &img.tracer[1 - g_tracer];
                Dispatch(FOG_PIPE_TRANSPORT, b, Push(1, init, dt), g, g, 1);
                b.tex[5] = nullptr;
                b.out[2] = nullptr;
                g_tracer = 1 - g_tracer;
            }
            if (total > 0)
            {
                g_transAll = false;
                g_initMask = 0;
            }
        }
        MarkTimer(vk.cmd, kSpanTransport);

        // The wake fluid: advect, splat the bodies, project (divergence, Jacobi, gradient), vorticity.
        if (g_wakeSteps > 0)
        {
            WXL_GfxVkAlloc splats{};
            const int count = std::clamp(in.splatCount, 0, FOG_MAX_BODIES);
            const VkDeviceSize bytes = sizeof(float) * 8 * std::max(count, 1);
            if (api->AllocUniform(bytes, &splats) && splats.mapped)
            {
                if (count > 0) std::memcpy(splats.mapped, in.splats, sizeof(float) * 8 * size_t(count));
                const uint32_t g = FOG_WAKE_N / 8;
                for (int step = 0; step < g_wakeSteps; ++step)
                {
                    const float first = step == 0 ? 1.0f : 0.0f;
                    const int prev = g_wakeCur, cur = 1 - g_wakeCur;
                    {
                        Bind b;
                        b.tex[0] = &img.wake[prev];
                        b.tex[1] = &img.wakeCurl;
                        b.out[0] = &img.wake[cur];
                        Dispatch(FOG_PIPE_WAKE, b, Push(FOG_WAKE_ADVECT, 0, first), g, g, FOG_WAKE_LEVELS);
                    }
                    if (count > 0)
                    {
                        Bind b;
                        b.out[0] = &img.wake[cur];
                        b.rw = splats.buffer;
                        b.rwOffset = splats.offset;
                        b.rwSize = bytes;
                        Dispatch(FOG_PIPE_WAKE, b, Push(FOG_WAKE_SPLAT, 0, first), uint32_t(count), 1, FOG_WAKE_LEVELS);
                    }
                    {
                        Bind b;
                        b.out[0] = &img.wake[cur];
                        b.out[3] = &img.wakeDiv;
                        Dispatch(FOG_PIPE_WAKE, b, Push(FOG_WAKE_DIVERGE, 0, first), g, g, FOG_WAKE_LEVELS);
                    }
                    for (int i = 0; i < kWakeJacobi; ++i)
                    {
                        Bind b;
                        b.tex[2] = &img.wakePressure[i & 1];
                        b.tex[3] = &img.wakeDiv;
                        b.out[2] = &img.wakePressure[(i + 1) & 1];
                        Dispatch(FOG_PIPE_WAKE, b, Push(FOG_WAKE_JACOBI, 0, first), g, g, FOG_WAKE_LEVELS);
                    }
                    {
                        Bind b;
                        b.tex[2] = &img.wakePressure[kWakeJacobi & 1];
                        b.out[0] = &img.wake[cur];
                        Dispatch(FOG_PIPE_WAKE, b, Push(FOG_WAKE_PROJECT, 0, first), g, g, FOG_WAKE_LEVELS);
                    }
                    {
                        Bind b;
                        b.out[0] = &img.wake[cur];
                        b.out[1] = &img.wakeCurl;
                        Dispatch(FOG_PIPE_WAKE, b, Push(FOG_WAKE_CURL, 0, first), g, g, FOG_WAKE_LEVELS);
                    }
                    g_wakeCur = cur;
                }
            }
        }
        MarkTimer(vk.cmd, kSpanWake);
        const WXL_GfxVkImage& wakeNow = img.wake[g_wakeCur];

        // The clipmaps, coarse to fine: a fine level scrolling in reads the coarse one just stepped.
        for (int L = FOG_LEVELS - 1; L >= 0; --L)
        {
            if (!g_level[L].step) continue;
            const uint32_t lv = uint32_t(L);
            {
                Bind b;
                b.tex[0] = &img.floor;
                b.out[0] = &img.ground;
                Dispatch(FOG_PIPE_GROUND, b, Push(lv), FOG_LEVEL_N / 8, FOG_LEVEL_N / 8, 1);
            }
            {
                Bind b;
                b.tex[0] = &img.floor;
                b.tex[1] = &img.layer;
                b.tex[2] = &img.ground;
                b.tex[3] = &img.curl;
                b.tex[4] = &wakeNow;
                b.out[0] = &img.vel;
                Dispatch(FOG_PIPE_VELOCITY, b, Push(lv), FOG_VEL_N / 8, FOG_VEL_N / 8, FOG_VEL_NZ / 2);
            }
            {
                Bind b;
                b.tex[0] = &img.state;
                b.tex[1] = &img.ground;
                b.tex[2] = &img.vel;
                b.tex[3] = &img.floor;
                b.out[0] = &img.hat;
                Dispatch(FOG_PIPE_ADVECT, b, Push(lv), FOG_LEVEL_N / 8, FOG_LEVEL_N / 8, FOG_LEVEL_NZ / 4);
            }
            {
                Bind b;
                b.tex[0] = &img.hat;
                b.tex[1] = &img.ground;
                b.tex[2] = &img.vel;
                b.tex[3] = &img.layer;
                b.tex[4] = &img.shape;
                b.tex[5] = &img.state;
                b.tex[6] = &img.floor;
                b.tex[7] = &img.tracer[g_tracer];
                b.out[0] = &img.state;
                Dispatch(FOG_PIPE_SOURCE, b, Push(lv), FOG_LEVEL_N / 8, FOG_LEVEL_N / 8, FOG_LEVEL_NZ / 4);
            }
            MarkTimer(vk.cmd, Span(kSpanLevel0 + L));
        }
        bool anyStep = false;
        for (int L = 0; L < FOG_LEVELS; ++L)
        {
            if (!g_level[L].step) continue;
            anyStep = true;
            Bind b;
            b.tex[0] = &img.state;
            b.out[0] = &img.occ;
            Dispatch(FOG_PIPE_OCCUPANCY, b, Push(uint32_t(L)), FOG_OCC_N, FOG_OCC_N, FOG_OCC_NZ);
        }
        for (int L = 0; L < FOG_LEVELS; ++L)
        {
            if (!g_level[L].step) continue;
            Bind b;
            b.tex[0] = &img.state;
            b.tex[1] = &img.ground;
            b.tex[2] = &img.floor;
            b.out[0] = &img.light;
            Dispatch(FOG_PIPE_LIGHT, b, Push(uint32_t(L)), FOG_VEL_N / 8, FOG_VEL_N / 8, FOG_VEL_NZ / 2);
        }
        if (anyStep) MarkTimer(vk.cmd, kSpanLight);

        // The lamp and indoor grid.
        const int lampPrev = g_lamp, lampCur = 1 - g_lamp;
        if (gridOn)
        {
            Bind b;
            b.tex[0] = &img.lamp[lampPrev];
            b.tex[1] = &img.state;
            b.tex[2] = &img.ground;
            b.tex[4] = Or(li.light, n2);
            b.tex[5] = Or(li.cluster, n2);
            b.tex[6] = Or(li.omniTable, n2);
            b.tex[7] = Or(li.cookie, n2);
            for (int k = 0; k < 4; ++k) b.tex[8 + k] = Or(li.omni[k], n2);
            b.out[0] = &img.lamp[lampCur];
            Dispatch(FOG_PIPE_LAMPS, b, Push(0), vkh::Groups(img.lampX, 8), vkh::Groups(img.lampY, 8), vkh::Groups(img.lampZ, 4));
            MarkTimer(vk.cmd, kSpanLamps);
            g_lamp = lampCur;
            g_lampHistory = true;
        }
        else g_lampHistory = false;
        const WXL_GfxVkImage& lampNow = gridOn ? img.lamp[lampCur] : n3;

        // The marches.
        {
            Bind b;
            b.tex[0] = &vk.depth;
            b.tex[1] = &img.state;
            b.tex[2] = &img.ground;
            b.tex[3] = &img.light;
            b.tex[4] = &img.occ;
            b.tex[5] = &img.detail;
            b.tex[6] = &img.curl;
            b.tex[7] = lampNow.image ? &lampNow : &n3;
            b.tex[8] = Or(blue, n2);
            b.tex[9] = &img.sky;
            for (int k = 0; k < 3; ++k) b.tex[10 + k] = Or(cascade[k], n2);
            b.tex[13] = &img.shape;
            b.tex[14] = &wakeNow;
            b.out[0] = &img.nearFog;
            b.out[1] = &img.nearAux;
            b.out[2] = &img.debug;
            b.out[3] = &img.nearFront;
            Dispatch(FOG_PIPE_MARCH, b, Push(0), vkh::Groups(img.halfW, 8), vkh::Groups(img.halfH, 8), 1);
            MarkTimer(vk.cmd, kSpanNear);
            b.out[0] = &img.farFog;
            b.out[1] = &img.farAux;
            b.out[3] = &img.farFront;
            Dispatch(FOG_PIPE_MARCH, b, Push(1), vkh::Groups(img.quarterW, 8), vkh::Groups(img.quarterH, 8), 1);
            MarkTimer(vk.cmd, kSpanFar);
        }

        // One image, then time.
        const int histPrev = g_hist, histCur = 1 - g_hist;
        {
            Bind b;
            b.tex[0] = &img.nearFog;
            b.tex[1] = &img.nearAux;
            b.tex[2] = &img.farFog;
            b.tex[3] = &img.farAux;
            b.tex[4] = &img.nearFront;
            b.tex[5] = &img.farFront;
            b.out[0] = &img.combFog;
            b.out[1] = &img.combAux;
            b.out[2] = &img.combFront;
            Dispatch(FOG_PIPE_COMBINE, b, Push(0), vkh::Groups(img.halfW, 8), vkh::Groups(img.halfH, 8), 1);
        }
        {
            Bind b;
            b.tex[0] = &img.combFog;
            b.tex[1] = &img.combAux;
            b.tex[2] = &img.histFog[histPrev];
            b.tex[3] = &img.histAux[histPrev];
            b.tex[4] = &img.combFront;
            b.tex[5] = &img.histFront[histPrev];
            b.out[0] = &img.histFog[histCur];
            b.out[1] = &img.histAux[histCur];
            b.out[2] = &img.debug;
            b.out[3] = &img.histFront[histCur];
            Dispatch(FOG_PIPE_TEMPORAL, b, Push(0), vkh::Groups(img.halfW, 8), vkh::Groups(img.halfH, 8), 1);
        }
        if (MapView(s.view))
        {
            Bind b;
            b.tex[0] = &img.state;
            b.tex[1] = &img.ground;
            b.tex[2] = &img.layer;
            b.tex[3] = &img.floor;
            b.tex[4] = &img.sky;
            b.tex[5] = &wakeNow;
            b.tex[6] = &img.cascadeMap;
            b.tex[7] = &img.tracer[g_tracer];
            b.out[0] = &img.debug;
            Dispatch(FOG_PIPE_DEBUG, b, Push(0), vkh::Groups(img.halfW, 8), vkh::Groups(img.halfH, 8), 1);
        }
        {
            VkDeviceSize offset = 0;
            const VkBuffer probe = ProbeBuffer(vk.slot, offset);
            if (probe != VK_NULL_HANDLE)
            {
                Bind b;
                b.tex[1] = &img.state;
                b.tex[2] = &img.ground;
                b.tex[3] = &img.light;
                b.tex[4] = &img.occ;
                b.tex[5] = &img.detail;
                b.tex[6] = &img.curl;
                b.tex[7] = lampNow.image ? &lampNow : &n3;
                b.tex[9] = &img.sky;
                for (int k = 0; k < 3; ++k) b.tex[10 + k] = Or(cascade[k], n2);
                b.tex[13] = &img.shape;
                b.tex[14] = &wakeNow;
                b.rw = probe;
                b.rwOffset = offset;
                b.rwSize = 32;
                Dispatch(FOG_PIPE_PROBE, b, Push(0), 1, 1, 1);
            }
        }
        MarkTimer(vk.cmd, kSpanTemporal);

        // The composite's inputs, into D3D9 textures.
        bool copied = apply::EnsureTargets(frame.device, img.halfW, img.halfH);
        copied = copied && api->CopyToTexture(&img.histFog[histCur], apply::FogTexture())
                        && api->CopyToTexture(&img.histFront[histCur], apply::FrontTexture())
                        && api->CopyToTexture(&img.histAux[histCur], apply::AuxTexture());
        if (copied && s.view >= 3) api->CopyToTexture(&img.debug, apply::DebugTexture());
        MarkTimer(vk.cmd, kSpanCopy);
        EndTimers();
        if (!copied)
        {
            std::snprintf(g_status, sizeof g_status, "fog: the copies into the D3D9 textures were refused");
            return false;
        }

        g_hist = histCur;
        g_historyValid = true;
        g_restart = false;

        // The composite's constants.
        apply::Input& a = g_apply;
        a.frameIndex = frame.frameIndex;
        std::memcpy(a.c[0], c.screenFull, sizeof c.screenFull);
        std::memcpy(a.c[1], c.screenHalf, sizeof c.screenHalf);
        Set(a.c[2], frame.depthRange[0], c.depthRange[1], frame.depthRange[1], c.march[1]);
        std::memcpy(a.c[3], c.invViewProj, sizeof c.invViewProj);
        const OutdoorProfile& o = Outdoor();
        const ProbeResult& pr = Probe();
        // The lying fog and smoke close in around you; the atmosphere alone does not.
        const float target = s.immersion && pr.valid ? std::clamp((pr.fog + pr.smoke) / 0.08f, 0.0f, 1.0f) : 0.0f;
        g_immersion += (target - g_immersion) * std::min(in.dt / 0.4f, 1.0f);
        Set(a.c[7], float(s.view), s.immersion ? o.immersionBlur : 0.0f, s.immersion ? o.immersion : 0.0f, o.softWhite);
        Set(a.c[8], c.frame[2], c.frame[3], s.dither ? 1.0f : 0.0f, g_immersion);

        const float block = SpanMs(-1), applyMs = SpanMs(kSpanApply);
        char time[48];
        if (!TimersSupported()) std::snprintf(time, sizeof time, "GPU time n/a");
        else if (block < 0.0f) std::snprintf(time, sizeof time, "GPU time pending");
        else std::snprintf(time, sizeof time, "GPU %.2f ms", block + std::max(applyMs, 0.0f));
        std::snprintf(g_status, sizeof g_status, "fog: %ux%u march, %d+%d steps, lights %d, rooms %d, prims %d, cascades %d | %s",
                      img.halfW, img.halfH, s.nearSteps, s.farSteps, li.ready ? li.count : 0, li.rooms, prims::Count(), cascades, time);
        int w = 0;
        for (int i = 0; i < kSpanCount && w >= 0 && w < int(sizeof g_times); ++i)
            w += std::snprintf(g_times + w, sizeof g_times - size_t(w), "%s%s %.3f", i ? " | " : "", kSpanNames[i], std::max(SpanMs(i), 0.0f));
        std::snprintf(g_sim, sizeof g_sim, "simulation: rivers %d steps (+%d warm-up), levels %s%s%s%s, camera extinction %.3f/yd, visibility %.0f yd",
                      g_transSteps, g_settleSteps, g_level[0].step ? "0" : "-", g_level[1].step ? "1" : "-", g_level[2].step ? "2" : "-",
                      g_level[3].step ? "3" : "-", pr.extinction, std::min(pr.visibility, 99999.0f));
        return true;
    }
}

namespace wxl::gfx::fog::gpu
{
    bool Prepare(const WXL_GfxVulkanApi* api)
    {
        if (!FollowDevice(api)) return false;
        if (!EnsurePipelines(api) || !EnsurePersistent(api)) return false;
        const Settings& s = Config();
        if (g_wantW && g_wantH) EnsureScreen(api, g_wantW, g_wantH);
        EnsureLampGrid(api, uint32_t(std::clamp(s.gridX, 16, 320)), uint32_t(std::clamp(s.gridY, 9, 180)),
                       uint32_t(std::clamp(s.gridZ, 8, 128)));
        return true;
    }

    bool Record(const WXL_GfxVkFrame& vk, const FrameInput& in)
    {
        g_recorded = false;
        bool ok = false;
        try
        {
            ok = RecordImpl(vk, in);
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; FOG_LOG_ERROR("gpu: exception while recording; no fog this frame"); }
            ok = false;
        }
        if (!ok)
        {
            g_historyValid = false;
            EndTimers();
        }
        g_recorded = ok;
        return ok;
    }

    bool Apply(const WXL_GfxFrame& frame)
    {
        if (!g_recorded || g_apply.frameIndex != frame.frameIndex) return false;
        try
        {
            return apply::Draw(frame, g_apply);
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; FOG_LOG_ERROR("apply: exception while compositing; no fog this frame"); }
            return false;
        }
    }

    void OnDeviceLost()
    {
        apply::Release();
        g_recorded = false;
        g_historyValid = false;
    }

    const char* Status() { return g_status; }
    const char* PassTimes() { return g_times; }
    const char* SimStatus() { return g_sim; }

    void Restart() { g_restart = true; }
}
