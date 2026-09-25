// wxl-forever surface lighting: every light of the light service on every surface of the world.
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
#include "../core/Matrix.hpp"
#include "../core/Media.hpp"
#include "../core/Panel.hpp"
#include "../core/Passes.hpp"
#include "../core/RenderUtil.hpp"
#include "../core/ShaderLibrary.hpp"
#include "../core/BlueNoise.hpp"
#include "../lights/Lights.hpp"
#include "../lights/Cookies.hpp"
#include "../lights/Rooms.hpp"
#include "Surface.hpp"

#include "wxl/EventScript.hpp"
#include "game/Camera.hpp"
#include "game/GBuffer.hpp"
#include "game/Lights.hpp"
#include "game/Sky.hpp"
#include "game/World.hpp"
#include "wxl/OmniShadowsApi.h"
#include "wxl/SceneLightsApi.h"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <unordered_map>
#include <vector>

// The passes, in order: copy the scene; normals at half resolution (reconstructed, blurred across
// and down); the raw light at the lighting resolution (full or half); its temporal accumulation (a
// history length per pixel, a variance clamp) and a-trous denoise; resolve to full resolution (the
// omni shadow share, a depth-aware upsample, the highlight cap) into the two published buffers; the
// raw indirect light at quarter resolution, accumulated and denoised the same way; then apply. Two
// render targets at once (MRT) carry the history length, distance and variance beside each
// accumulated light; without them the light stays raw and the engine's materials get no buffer.
// Every constant is final before the first shader is bound: a shader's own literals live in the
// registers it leaves free, and an upload after binding would overwrite them.
namespace
{
    namespace ev     = wxl::events;
    namespace sf     = wxl::forever::surface;
    namespace fl     = wxl::forever::lights;
    namespace mx     = wxl::forever::matrix;
    namespace render = wxl::forever::render;
    namespace ui     = wxl::forever::ui;
    namespace sky    = wxl::game::sky;
    namespace world  = wxl::game::world;

    constexpr UINT kRegisters = 196;  // c0..c195, see shaders/surface.hlsli
    constexpr int  kMoving = 4;
    // The atlas layout OmniShadowsApi.h documents: face f is the cell (f % 4, f / 4) of a 4 x 2 grid.
    constexpr int  kOmniColumns = 4, kOmniRows = 2;
    constexpr int  kMaxUnits = 16;
    constexpr float kAlbedoCap = 0.85f;   // no real surface reflects more
    constexpr float kEngineAddCap = 1.0f; // most light the engine's materials add per channel (linear)
    constexpr int   kViewEngineAdded = 9; // Debug view: what the engine's materials added, nothing else

    enum Span { kSpanCopy, kSpanNormals, kSpanLight, kSpanDenoise, kSpanResolve, kSpanIndirect, kSpanApply, kSpanCount };

    struct Target
    {
        IDirect3DTexture9* tex = nullptr;
        IDirect3DSurface9* rt  = nullptr;
    };

    sf::Settings            g_cfg;
    wxl::forever::GpuTimer  g_timer;

    // The scene as the world pass left it: the albedo estimate every pass reads.
    Target                  g_scene;
    UINT                    g_sceneW = 0, g_sceneH = 0;
    D3DFORMAT               g_sceneFormat = D3DFMT_UNKNOWN;
    // Full resolution: the resolved light buffer and the copy the engine's materials read.
    Target  g_light, g_engine;
    UINT    g_fullW = 0, g_fullH = 0;
    // Lighting resolution (full or half): the raw light (also the a-trous scratch), its
    // accumulation and meta (swapped every frame), the omni share and its filter's scratch, the
    // a-trous output.
    Target  g_raw, g_acc[2], g_meta[2], g_ratio, g_ratioB, g_out;
    Target  g_room;   // each lighting texel's room (A8R8G8B8, shaders/room.ps.hlsl)
    Target  g_omniVis;   // each lighting texel's share of the four omni slots (A8R8G8B8, shaders/omni.ps.hlsl)
    UINT    g_lightW = 0, g_lightH = 0;
    int     g_scale = 0;
    // Half resolution normals: reconstructed, blurred across, then down (swapped every frame).
    Target  g_normalsRaw, g_normalsAcross, g_normals, g_normalsPrev;
    UINT    g_halfW = 0, g_halfH = 0;
    // Quarter resolution indirect light: raw (also scratch), accumulated with its meta, finished.
    Target  g_giRaw, g_giAcc[2], g_giMeta[2], g_giOut;
    UINT    g_quarterW = 0, g_quarterH = 0;
    int     g_current = 0;
    IDirect3DTexture9* g_lightBuffer = nullptr;    // what the last frame published for apply and post
    IDirect3DTexture9* g_engineBuffer = nullptr;   // the same with units cleared, for the engine's materials
    uint32_t g_bufferFrame = 0;

    Target                  g_gbuffer;             // the world's normals, A8R8G8B8, supplied each pass
    UINT                    g_gbufferW = 0, g_gbufferH = 0;
    bool                    g_engineBound = false; // last frame's light buffer went to the engine's materials
    const WXL_OmniShadowsApi* g_omni = nullptr;
    bool                    g_omniLooked = false;
    int                     g_omniFaceSize = 0;     // last face size asked of the core
    int                     g_omniBudget = 0;       // last round-robin budget asked of the core
    uint32_t                g_omniCleared[WXL_OMNISHADOWS_MAX] = {};
    // Lights whose shadow map changed this frame (moved, or cleared): the history is cut near them.
    fl::Moving              g_omniChanged[WXL_OMNISHADOWS_MAX];
    int                     g_omniChangedCount = 0;
    fl::OmniSlot            g_omniSlots[WXL_OMNISHADOWS_MAX] = {};   // this frame's maps, by core slot
    int                     g_omniSlotCount = 0;
    int                     g_omniHeld = 0;         // lights of the list holding a map this frame
    bool                    g_mrtChecked = false, g_mrt = false;
    bool                    g_historyValid = false;
    int                     g_movingCount = 0;
    float                   g_prevViewProj[16] = {};
    float                   g_prevEye[3] = {};
    char                    g_status[240] = "surface lighting idle";
    char                    g_passes[240] = "passes: pending";

    void Release(Target& t)
    {
        if (t.rt)  { t.rt->Release();  t.rt = nullptr; }
        if (t.tex) { t.tex->Release(); t.tex = nullptr; }
    }

    bool Make(IDirect3DDevice9* dev, UINT w, UINT h, Target& t, D3DFORMAT format = D3DFMT_A16B16G16R16F)
    {
        return SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &t.tex, nullptr))
            && t.tex && SUCCEEDED(t.tex->GetSurfaceLevel(0, &t.rt));
    }

    // The self-check: every 8 frames a 64 x 36 probe of every stage of the lighting (the raw pass,
    // its accumulation, the denoised light, the resolved buffer, the omni share, the indirect light,
    // the distance the lighting passes reconstructed) and of what apply adds, plus a row of three
    // decoded points (shaders/probe.ps.hlsl), into one of three lockable targets behind an event
    // query, read once the query says the GPU is done with it, never waiting. The raw pass is copied
    // down to the grid as it is drawn, since the denoiser reuses its target.
    constexpr UINT kProbeGridW = 64, kProbeGridH = 36, kProbeBlocks = 3;
    constexpr UINT kProbeW = kProbeGridW * kProbeBlocks, kProbeH = kProbeGridH + 1, kProbeSlots = 3;
    struct Probe
    {
        IDirect3DSurface9* rt = nullptr;
        IDirect3DQuery9*   query = nullptr;
        bool               pending = false;
    };
    Probe  g_probes[kProbeSlots];
    Target g_probeRaw;   // the raw light pass, point-sampled down to the grid
    bool   g_probeFailed = false, g_probeRawFailed = false;
    DWORD  g_uniformLogged = 0, g_strayLogged = 0, g_summaryLogged = 0, g_offLogged = 0;

    void ReleaseAll()
    {
        for (Probe& pr : g_probes)
        {
            if (pr.rt) pr.rt->Release();
            if (pr.query) pr.query->Release();
            pr = Probe{};
        }
        Release(g_probeRaw);
        Release(g_room);
        Release(g_omniVis);
        g_probeFailed = g_probeRawFailed = false;
        Release(g_scene);
        g_sceneW = g_sceneH = 0;
        g_sceneFormat = D3DFMT_UNKNOWN;
        for (Target* t : { &g_light, &g_engine, &g_raw, &g_acc[0], &g_acc[1], &g_meta[0], &g_meta[1], &g_ratio, &g_ratioB, &g_out,
                           &g_normalsRaw, &g_normalsAcross, &g_normals, &g_normalsPrev,
                           &g_giRaw, &g_giAcc[0], &g_giAcc[1], &g_giMeta[0], &g_giMeta[1], &g_giOut })
            Release(*t);
        g_fullW = g_fullH = g_lightW = g_lightH = g_halfW = g_halfH = g_quarterW = g_quarterH = 0;
        g_scale = 0;
        g_lightBuffer = g_engineBuffer = nullptr;
        g_historyValid = false;
        Release(g_gbuffer);
        g_gbufferW = g_gbufferH = 0;
    }

    // The engine's materials add last frame's buffer, reprojected; where it cannot be reprojected
    // (outside last frame's view) apply lights the pixel itself, by a different estimate. Still, the
    // two agree; in fast motion their boundary shows as a seam and the reprojected light lags, so
    // their share eases towards 0 as the view moves (apply adds the rest).
    float g_engineWeight = 1.0f, g_motionPx = 0.0f;
    float g_weightLo = 1.0f, g_weightHi = 0.0f;   // its range since the last stability line
    float g_motionMax = 0.0f;

    /// How far the view moved on screen since last frame, full pixels: the largest shift of five
    /// points ten yards out (the centre, and half-way to each corner).
    float ScreenMotion(const float viewProjRel[16], const float eye[3])
    {
        float inv[16];
        if (!g_historyValid || !mx::Invert4(viewProjRel, inv)) return 0.0f;
        static const float kPoints[5][2] = { { 0.0f, 0.0f }, { -0.5f, -0.5f }, { 0.5f, -0.5f }, { -0.5f, 0.5f }, { 0.5f, 0.5f } };
        const float shift[3] = { eye[0] - g_prevEye[0], eye[1] - g_prevEye[1], eye[2] - g_prevEye[2] };
        float most = 0.0f;
        for (const auto& pt : kPoints)
        {
            const float clip[4] = { pt[0], pt[1], 0.5f, 1.0f };
            float h[4] = {};
            for (int j = 0; j < 4; ++j)
                for (int i = 0; i < 4; ++i) h[j] += clip[i] * inv[i * 4 + j];
            if (std::fabs(h[3]) < 1e-6f) continue;
            float dir[3] = { h[0] / h[3], h[1] / h[3], h[2] / h[3] };
            const float len = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            if (len < 1e-6f) continue;
            const float q[3] = { dir[0] / len * 10.0f + shift[0], dir[1] / len * 10.0f + shift[1], dir[2] / len * 10.0f + shift[2] };
            float p[4] = {};
            for (int j = 0; j < 4; ++j) p[j] = q[0] * g_prevViewProj[j] + q[1] * g_prevViewProj[4 + j] + q[2] * g_prevViewProj[8 + j] + g_prevViewProj[12 + j];
            if (p[3] <= 1e-3f) return 1e3f;
            const float dx = (p[0] / p[3] - pt[0]) * 0.5f * float(g_fullW), dy = (p[1] / p[3] - pt[1]) * 0.5f * float(g_fullH);
            most = std::max(most, std::sqrt(dx * dx + dy * dy));
        }
        return most;
    }

    /// Before the world pass: a target for its normals, and last frame's light buffer bound for the
    /// engine's own materials with the matrix that reads it from this frame's view space.
    void Begin(const wxl::forever::passes::BeginFrame& b)
    {
        g_engineBound = false;
        if (!g_cfg.enabled) return;
        IDirect3DDevice9* d = b.device;
        // Their share: 1 still, easing to 0.3 past about 120 pixels of motion a frame (the reprojected
        // light lags by a frame); drops quickly, returns slowly. The seam where their reprojection
        // runs out is handled per pixel instead (EngineEdge in shaders/surface.hlsli), so models keep
        // their true texture under the light while the view moves.
        g_motionPx = ScreenMotion(b.viewProjRel, b.eye);
        {
            const float t = std::clamp((g_motionPx - 40.0f) / 80.0f, 0.0f, 1.0f);
            const float target = 1.0f - 0.7f * t * t * (3.0f - 2.0f * t);
            g_engineWeight += (target - g_engineWeight) * (target < g_engineWeight ? 0.5f : 0.1f);
            g_weightLo = std::min(g_weightLo, g_engineWeight);
            g_weightHi = std::max(g_weightHi, g_engineWeight);
            g_motionMax = std::max(g_motionMax, std::min(g_motionPx, 999.0f));
        }
        if (g_cfg.gbuffer && b.normalTarget && b.sceneDepth)
        {
            D3DSURFACE_DESC dd{};
            static_cast<IDirect3DSurface9*>(b.sceneDepth)->GetDesc(&dd);
            if (!g_gbuffer.tex || g_gbufferW != dd.Width || g_gbufferH != dd.Height)
            {
                Release(g_gbuffer);
                if (Make(d, dd.Width, dd.Height, g_gbuffer, D3DFMT_A8R8G8B8)) { g_gbufferW = dd.Width; g_gbufferH = dd.Height; }
                else { Release(g_gbuffer); g_gbufferW = g_gbufferH = 0; }
            }
            if (g_gbuffer.rt) *b.normalTarget = g_gbuffer.rt;
        }
        // Only with the normal target in place: its alpha is how apply knows which pixels the engine
        // already lit, so nothing is lit twice.
        const bool normalsSupplied = b.normalTarget && g_gbuffer.rt && *b.normalTarget == g_gbuffer.rt;
        if (normalsSupplied && g_cfg.engineMaterials && g_engineBuffer && g_historyValid
            && g_bufferFrame + 1 == b.index)
        {
            // This frame's view space -> camera-relative -> last frame's camera-relative -> its clip
            // -> the light buffer's texture space (u * w, v * w, -, w).
            float toRel[16];
            if (!mx::Invert4(b.viewRel, toRel)) return;
            const float shift[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,
                                      b.eye[0] - g_prevEye[0], b.eye[1] - g_prevEye[1], b.eye[2] - g_prevEye[2], 1 };
            const float toTexture[16] = { 0.5f, 0, 0, 0,  0, -0.5f, 0, 0,  0, 0, 1, 0,  0.5f, 0.5f, 0, 1 };
            float a[16], m[16], full[16];
            mx::Mul4(toRel, shift, a);
            mx::Mul4(a, g_prevViewProj, m);
            mx::Mul4(m, toTexture, full);
            float columns[4][4];
            mx::Columns(full, columns);
            // The engine's constant cache uploads only what changed: zeros first, so a still camera
            // (the same rows as last frame) still reaches the device.
            static const float kZero[4][4] = {};
            wxl::game::gbuffer::SetEnginePixelConstants(wxl::game::gbuffer::kLightBufferRows, &kZero[0][0], 4);
            wxl::game::gbuffer::SetEnginePixelConstants(wxl::game::gbuffer::kLightBufferRows, &columns[0][0], 4);
            // Strength, the cap on what is added per channel (linear), the scene's share (0 shows
            // only what is added: the "Engine materials added light" view).
            const float params[4] = { std::max(g_cfg.strength, 0.0f) * g_engineWeight, kEngineAddCap,
                                      g_cfg.view == kViewEngineAdded ? 0.0f : 1.0f, 0.0f };
            wxl::game::gbuffer::SetEnginePixelConstants(wxl::game::gbuffer::kLightBufferParams, params, 1);
            const DWORD s = wxl::game::gbuffer::kLightBufferSampler;
            d->SetTexture(s, g_engineBuffer);
            d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
            d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
            g_engineBound = true;
        }
    }

    // The units GatherUnits saw this frame, world feet and height, and when (seconds) a unit was last
    // near each light (by id), for the omni slot choice.
    float g_unitWorld[kMaxUnits][4] = {};
    int   g_unitCount = 0;
    std::unordered_map<uint32_t, double> g_unitNear;

    /// Chooses the four lights for the core's omni shadow maps and writes what the light pass needs
    /// to read them; binds each atlas on s11..s14. False when none is ready.
    // The four omni slots, held: the core clears and redraws a slot given a different light, so a
    // light keeps its slot until a clearly better one has waited out the hold, and the slot's
    // shadow fades out before it changes hands and in after (c195): a map is a bonus on a light that
    // always shines, never a switch.
    struct OmniSlotState
    {
        uint32_t id = 0;        // the light held, 0 none
        uint32_t next = 0;      // the light it goes to once faded out, 0 none
        double   since = 0.0;   // when it was taken, seconds
        float    weight = 0.0f; // its shadow's share, 0..1
        bool     leaving = false; // still listed but no longer a candidate: fading out to nobody
    };
    OmniSlotState g_slots[WXL_OMNISHADOWS_MAX];
    int g_slotChanges = 0;      // hand-overs since the last stability line
    constexpr double kSlotHold = 1.0;     // seconds a light keeps its slot at least
    constexpr float  kSlotFade = 0.5f;    // seconds a slot's shadow takes to fade in or out
    constexpr double kNearHold = 1.5;     // seconds a light keeps its unit after it left
    constexpr int    kLeavingRank = 99;   // a leaving slot yields to any candidate

    bool OmniShadows(IDirect3DDevice9* d, const float eye[3], float c[][4])
    {
        if (!g_omniLooked)
        {
            g_omniLooked = true;
            g_omni = static_cast<const WXL_OmniShadowsApi*>(wxl_forever::g_api->GetInterface("wxl.omnishadows", WXL_OMNISHADOWS_API_VERSION));
            if (g_omni && (g_omni->structSize < sizeof(WXL_OmniShadowsApi) || !g_omni->SetLightsEx || !g_omni->GetState)) g_omni = nullptr;
            WLOG_INFO("surface: omni shadow maps %s", g_omni ? "available, with change tracking" : "not offered by the core");
        }
        if (!g_omni)
        {
            fl::PublishOmni(d, nullptr, 0, 0.0f, wxl::forever::passes::FrameIndex());
            return false;
        }
        // The face size applies when the core next creates its atlases.
        const int faceSize = std::clamp(g_cfg.omniFaceSize, 64, 1024);
        if (faceSize != g_omniFaceSize && g_omni->SetFaceSize) { g_omni->SetFaceSize(uint32_t(faceSize)); g_omniFaceSize = faceSize; }
        // Faces whose light or casters moved are redrawn at once; the rest are refreshed this many a frame.
        const int budget = std::clamp(g_cfg.omniRefresh, 1, 6);
        if (budget != g_omniBudget && g_omni->SetBudget) { g_omni->SetBudget(uint32_t(budget)); g_omniBudget = budget; }

        // The four maps go, in tiers, to: point lights with a real reach that have a unit near them
        // (a passer-by must cast a shadow), then lights without a cookie (nothing else shapes their
        // light); a still lamp whose cookie already throws its own cage takes none. Within a tier the
        // camera's own room first (indoors, the lamps around the player shape what it sees), then
        // importance (brightness over distance; the list itself is in id order). A unit counts as
        // near for a short while after it leaves, so a slot does not flap as someone walks past.
        int count = 0;
        const fl::Light* list = fl::Current(count);
        WXL_OmniLightEx chosenEx[WXL_OMNISHADOWS_MAX];
        float chosenSize[WXL_OMNISHADOWS_MAX] = {};
        float chosenWeight[WXL_OMNISHADOWS_MAX] = {};
        int chosenIndex[WXL_OMNISHADOWS_MAX] = {};
        uint32_t n = 0;
        const float origin[3] = { 0.0f, 0.0f, 0.0f };
        float lo = 0.0f, hi = 0.0f;
        const int cameraRoom = wxl::forever::lights::rooms::RoomOf(origin, lo, hi);
        const uint32_t frame = wxl::forever::passes::FrameIndex();
        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        const double now = double(counter.QuadPart) / double(frequency.QuadPart);
        static double last = 0.0;
        const float dt = last > 0.0 ? float(std::clamp(now - last, 0.0, 0.25)) : 0.0f;
        last = now;
        constexpr float kBodyReach = 1.5f;
        struct Candidate { int rank; int index; float score; };
        std::vector<Candidate> candidates;
        candidates.reserve(size_t(count));
        for (int i = 0; i < count; ++i)
        {
            const fl::Light& l = list[i];
            if (l.radius < 2.0f || l.extent[0] != 0.0f || l.extent[1] != 0.0f || l.extent[2] != 0.0f) continue;
            // A carried light's map is filled by its own carrier and item, a few inches away.
            if (l.carried && !g_cfg.carriedShadowMaps && !g_cfg.noCarried) continue;
            int rank = cameraRoom >= 0 && l.room != cameraRoom ? 1 : 0;
            if (g_cfg.omniUnitsFirst)
            {
                bool unitNear = false;
                for (int u = 0; u < g_unitCount && !unitNear; ++u)
                {
                    const float dx = g_unitWorld[u][0] - l.rest[0], dy = g_unitWorld[u][1] - l.rest[1];
                    const float dz = g_unitWorld[u][2] + 0.5f * g_unitWorld[u][3] - l.rest[2];
                    const float reach = l.radius + kBodyReach;
                    unitNear = dx * dx + dy * dy + dz * dz < reach * reach;
                }
                if (l.id)
                {
                    double& seen = g_unitNear[l.id];
                    if (unitNear) seen = now;
                    else unitNear = seen > 0.0 && now - seen <= kNearHold;
                }
                // A lamp whose cookie already throws its own cage takes a map only for a passer-by.
                if (!unitNear && l.cookieOpen >= 0.0f) continue;
                rank += unitNear ? 0 : 2;
            }
            const float dx = l.rest[0] - eye[0], dy = l.rest[1] - eye[1], dz = l.rest[2] - eye[2];
            const float luma = (l.color[0] * 0.299f + l.color[1] * 0.587f + l.color[2] * 0.114f) * l.intensity;
            candidates.push_back({ rank, i, luma / (1.0f + (dx * dx + dy * dy + dz * dz) / (l.radius * l.radius)) });
        }
        std::stable_sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
            return a.rank != b.rank ? a.rank < b.rank : a.score > b.score;
        });
        // Forgotten lights leave the hold table now and then, so it cannot grow without bound.
        if (g_unitNear.size() > 512)
            for (auto it = g_unitNear.begin(); it != g_unitNear.end();)
                it = now - it->second > kNearHold ? g_unitNear.erase(it) : std::next(it);
        // Where each candidate stands: its place and rank by id; and where a light is in the list.
        auto find = [&](uint32_t id, int& rank) -> int {
            for (size_t k = 0; k < candidates.size(); ++k)
                if (list[candidates[k].index].id == id) { rank = candidates[k].rank; return int(k); }
            return -1;
        };
        auto listed = [&](uint32_t id) -> int {
            for (int i = 0; i < count; ++i)
                if (list[i].id == id) return i;
            return -1;
        };
        // A held light gone from the list frees its slot (its light has faded out with it); one still
        // listed but no longer a candidate fades its shadow out first, like a hand-over to nobody.
        for (OmniSlotState& s : g_slots)
        {
            int rank = 0;
            if (s.id && listed(s.id) < 0) { s = OmniSlotState{}; ++g_slotChanges; continue; }
            if (s.id && find(s.id, rank) < 0) s.leaving = true;
            if (s.next && find(s.next, rank) < 0) s.next = 0;
        }
        // The best candidates not held: an empty slot takes one; otherwise one held light, past its
        // hold, out of the top four and in a worse tier, starts fading out for it.
        bool started = false;
        for (size_t k = 0; k < candidates.size() && k < WXL_OMNISHADOWS_MAX; ++k)
        {
            const uint32_t id = list[candidates[k].index].id;
            if (!id || std::any_of(std::begin(g_slots), std::end(g_slots), [&](const OmniSlotState& s) { return s.id == id || s.next == id; }))
                continue;
            OmniSlotState* empty = nullptr;
            for (OmniSlotState& s : g_slots) if (!s.id) { empty = &s; break; }
            if (empty) { *empty = OmniSlotState{ id, 0, now, 0.0f, false }; ++g_slotChanges; continue; }
            if (started) continue;
            OmniSlotState* worst = nullptr;
            int worstRank = -1;
            for (OmniSlotState& s : g_slots)
            {
                int rank = kLeavingRank;
                const int at = s.leaving ? int(WXL_OMNISHADOWS_MAX) : find(s.id, rank);
                if (s.next || (!s.leaving && now - s.since < kSlotHold) || at < int(WXL_OMNISHADOWS_MAX) || rank <= candidates[k].rank)
                    continue;
                if (rank > worstRank) { worstRank = rank; worst = &s; }
            }
            if (worst) { worst->next = id; started = true; }
        }
        // Shadows fade in and out over kSlotFade seconds, whatever the frame rate.
        const float step = std::min(dt / kSlotFade, 1.0f);
        for (OmniSlotState& s : g_slots)
        {
            if (!s.id) continue;
            if (s.next || s.leaving)
            {
                s.weight -= step;
                if (s.weight > 0.0f) continue;
                s = s.next ? OmniSlotState{ s.next, 0, now, 0.0f, false } : OmniSlotState{};
                ++g_slotChanges;
            }
            else s.weight = std::min(s.weight + step, 1.0f);
        }
        // Held lights in slot order; a freed slot in the middle closes up. The core keeps each map
        // with its light's id, so this only reorders the slots.
        for (int k = 0; k + 1 < WXL_OMNISHADOWS_MAX; ++k)
            if (!g_slots[k].id && g_slots[k + 1].id) { std::swap(g_slots[k], g_slots[k + 1]); k = -1; }
        for (const OmniSlotState& s : g_slots)
        {
            if (!s.id) break;
            const int index = listed(s.id);
            if (index < 0) break;
            const fl::Light& l = list[index];
            chosenWeight[n] = s.weight;
            // Rendered from where the light rests: the flicker's sway does not redraw the map.
            chosenEx[n] = WXL_OmniLightEx{ { l.rest[0], l.rest[1], l.rest[2] }, l.radius, s.id, 0 };
            chosenSize[n] = std::max(l.size, 0.005f);
            chosenIndex[n] = index;
            ++n;
        }
        g_omni->SetLightsEx(chosenEx, n);
        g_omniChangedCount = 0;
        const uint32_t sceneFrame = wxl::game::lights::SceneFrame();

        bool any = false;
        g_omniHeld = 0;
        const uint32_t ready = std::min(g_omni->Count(), uint32_t(WXL_OMNISHADOWS_MAX));
        g_omniSlotCount = int(ready);
        for (uint32_t i = 0; i < ready; ++i)
        {
            // Slot i here, in the constants, on sampler 11 + i and in the light service's table alike.
            fl::OmniSlot& pub = g_omniSlots[i];
            pub = fl::OmniSlot{};
            pub.lightIndex = -1;
            WXL_OmniShadow s{};
            if (!g_omni->Get(i, &s) || !s.texture || s.faceSize == 0) continue;
            WXL_OmniShadowState st{};
            const bool tracked = g_omni->GetState(i, &st) != 0;
            // The chosen light this map was rendered for, by the id the core keeps with the slot.
            int match = -1;
            for (uint32_t j = 0; j < n && match < 0; ++j)
                if (tracked && st.id && chosenEx[j].id == st.id) match = int(j);
            pub.lightIndex = match >= 0 ? chosenIndex[match] : -1;
            if (match >= 0) ++g_omniHeld;
            // A map no chosen light owns shadows nothing (surfaces read the share as omniFade).
            pub.weight = match >= 0 ? chosenWeight[match] : 0.0f;
            c[195][i] = pub.weight;
            pub.radius = s.radius;
            pub.faceSize = float(s.faceSize);
            pub.texture = static_cast<IDirect3DTexture9*>(s.texture);
            if (tracked)
            {
                // Redrawn for a move this frame, cleared since last seen, or a face not yet redrawn.
                const bool changed = (st.lastMovedFrame && st.lastMovedFrame + 2 >= sceneFrame)
                                  || st.lastClearFrame != g_omniCleared[i] || st.staleFaces != 0;
                g_omniCleared[i] = st.lastClearFrame;
                if (changed)
                    g_omniChanged[g_omniChangedCount++] = fl::Moving{ { s.position[0], s.position[1], s.position[2] }, s.radius };
            }
            for (int k = 0; k < 3; ++k) c[49 + i][k] = pub.position[k] = s.position[k] - eye[k];
            c[49 + i][3] = s.radius;
            c[129][i] = match >= 0 ? chosenSize[match] : 0.1f;
            // Three rows per face: u * w, v * w and w (the projected z is not read).
            for (int f = 0; f < 6; ++f)
                for (int j = 0; j < 3; ++j)
                {
                    float* row = c[53 + i * 18 + f * 3 + j];
                    if (!s.faceFrame[f]) { row[0] = row[1] = row[2] = row[3] = 0.0f; continue; }
                    const float* r = s.faceRows[f][j == 2 ? 3 : j];
                    // Taken to camera-relative input: the eye folds into the constant term.
                    row[0] = r[0]; row[1] = r[1]; row[2] = r[2];
                    row[3] = float(double(r[3]) + double(r[0]) * eye[0] + double(r[1]) * eye[1] + double(r[2]) * eye[2]);
                }
            std::memcpy(pub.rows, &c[53 + i * 18], sizeof pub.rows);
            c[149][0] = 1.0f / float(kOmniColumns * s.faceSize);
            c[149][1] = 1.0f / float(kOmniRows * s.faceSize);
            c[128][0] = float(s.faceSize);
            render::Sampler(d, 11 + i, static_cast<IDirect3DTexture9*>(s.texture), false);
            any = true;
        }
        c[149][2] = any ? 1.0f : 0.0f;
        c[151][0] = 1.0f / float(kOmniColumns);
        c[151][1] = 1.0f / float(kOmniRows);
        c[151][2] = float(kOmniColumns);
        c[151][3] = 0.05f;   // nearer the light along a face axis, nothing was drawn
        fl::PublishOmni(d, g_omniSlots, g_omniSlotCount, c[128][3], frame);
        return any;
    }

    bool Ensure(IDirect3DDevice9* dev, const D3DSURFACE_DESC& target, int scale)
    {
        if (g_scene.tex && g_sceneW == target.Width && g_sceneH == target.Height && g_sceneFormat == target.Format
            && g_scale == scale) return true;
        ReleaseAll();
        const UINT w = target.Width, h = target.Height;
        const UINT lw = std::max((w + UINT(scale) - 1) / UINT(scale), 1u), lh = std::max((h + UINT(scale) - 1) / UINT(scale), 1u);
        const UINT hw = std::max((w + 1) / 2, 1u), hh = std::max((h + 1) / 2, 1u);
        const UINT qw = std::max((w + 3) / 4, 1u), qh = std::max((h + 3) / 4, 1u);
        bool ok = Make(dev, w, h, g_scene, target.Format);
        for (Target* t : { &g_light, &g_engine }) ok = ok && Make(dev, w, h, *t);
        for (Target* t : { &g_raw, &g_acc[0], &g_acc[1], &g_meta[0], &g_meta[1], &g_ratio, &g_ratioB, &g_out })
            ok = ok && Make(dev, lw, lh, *t);
        ok = ok && Make(dev, lw, lh, g_room, D3DFMT_A8R8G8B8);
        ok = ok && Make(dev, lw, lh, g_omniVis, D3DFMT_A8R8G8B8);
        for (Target* t : { &g_normalsRaw, &g_normalsAcross, &g_normals, &g_normalsPrev })
            ok = ok && Make(dev, hw, hh, *t);
        for (Target* t : { &g_giRaw, &g_giAcc[0], &g_giAcc[1], &g_giMeta[0], &g_giMeta[1], &g_giOut })
            ok = ok && Make(dev, qw, qh, *t);
        if (!ok)
        {
            ReleaseAll();
            WLOG_WARN("surface: FP16 targets %ux%u unavailable", w, h);
            return false;
        }
        g_sceneW = w;
        g_sceneH = h;
        g_sceneFormat = target.Format;
        g_fullW = w; g_fullH = h;
        g_lightW = lw; g_lightH = lh;
        g_halfW = hw; g_halfH = hh;
        g_quarterW = qw; g_quarterH = qh;
        // Every target starts black: history, meta and scratch are read before some are written.
        for (Target* t : { &g_light, &g_engine, &g_raw, &g_acc[0], &g_acc[1], &g_meta[0], &g_meta[1], &g_ratio, &g_ratioB,
                           &g_out, &g_normalsRaw, &g_normalsAcross, &g_normals, &g_normalsPrev,
                           &g_giRaw, &g_giAcc[0], &g_giAcc[1], &g_giMeta[0], &g_giMeta[1], &g_giOut })
            if (t->rt) dev->ColorFill(t->rt, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
        g_scale = scale;
        // Meta starts at "no history": a distance of 0 matches nothing.
        for (Target* t : { &g_meta[0], &g_meta[1], &g_giMeta[0], &g_giMeta[1] })
        {
            dev->SetRenderTarget(0, t->rt);
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(0, 0, 0, 0), 1.0f, 0);
        }
        WLOG_INFO("surface: targets %ux%u, lighting at %ux%u (1/%d), indirect at %ux%u", w, h, lw, lh, scale, qw, qh);
        return true;
    }

    float Lin(float c) { return std::pow(std::max(c, 0.0f), 2.2f); }

    /// True when the core gives the world's receivers no point light (WXL_LIGHT_POLICY=off): this
    /// feature then lights characters with every light itself, whatever engineOnUnits says.
    bool EngineLightsOff()
    {
        static const WXL_SceneLightsApi* api = nullptr;
        static bool looked = false;
        if (!looked)
        {
            looked = true;
            api = static_cast<const WXL_SceneLightsApi*>(wxl_forever::g_api->GetInterface("wxl.scenelights", WXL_SCENELIGHTS_API_VERSION));
        }
        const bool off = api && api->Policy && api->Policy() == WXL_LIGHT_POLICY_OFF;
        static int logged = -1;
        if (int(off) != logged)
        {
            logged = int(off);
            WLOG_INFO("surface: the engine's point lights are %s", off ? "off (WXL_LIGHT_POLICY=off): every light on characters comes from here"
                                                                      : "on: characters take engineOnUnits of the model lights the engine already gives them");
        }
        return off;
    }

    /// How brightly the scene's own light (ambient plus half the sun or moon) lights a surface,
    /// linear: the scene colour divided by it estimates the albedo.
    float SceneLighting()
    {
        sky::CelestialLight light{};
        if (!sky::GetCelestialLight(light)) return 0.5f;
        float lum = 0.0f;
        const float weights[3] = { 0.299f, 0.587f, 0.114f };
        for (int i = 0; i < 3; ++i) lum += weights[i] * (Lin(light.ambient[i]) + 0.5f * Lin(light.diffuse[i]));
        return std::clamp(lum, 0.05f, 2.0f);
    }

    /// The units nearest the camera, as capsules (camera-relative feet, height): their pixels keep a
    /// short history, do not take the engine's own model lights twice, and are cleared from the
    /// buffer the engine's materials read.
    int GatherUnits(const float eye[3], float out[kMaxUnits][4])
    {
        struct Seen { float pos[3]; float height; float d2; };
        std::vector<Seen> seen;
        world::ForEachObject(world::kTypeMaskUnit, [&](unsigned long long, void* obj) {
            Seen s{};
            world::Position(obj, s.pos);
            const float dx = s.pos[0] - eye[0], dy = s.pos[1] - eye[1], dz = s.pos[2] - eye[2];
            s.d2 = dx * dx + dy * dy + dz * dz;
            if (s.d2 > 80.0f * 80.0f) return true;
            float head[3];
            world::NamePosition(obj, head);
            const float h = head[2] - s.pos[2];
            s.height = (h > 0.5f && h < 8.0f) ? h : 2.2f;
            seen.push_back(s);
            return true;
        });
        std::sort(seen.begin(), seen.end(), [](const Seen& a, const Seen& b) { return a.d2 < b.d2; });
        const int n = std::min(int(seen.size()), kMaxUnits);
        for (int i = 0; i < n; ++i)
        {
            for (int k = 0; k < 3; ++k) out[i][k] = seen[i].pos[k] - eye[k];
            out[i][3] = seen[i].height;
            // Kept in world space for the omni slot choice (OmniShadows), which runs after this.
            for (int k = 0; k < 3; ++k) g_unitWorld[i][k] = seen[i].pos[k];
            g_unitWorld[i][3] = seen[i].height;
        }
        g_unitCount = n;
        return n;
    }

    bool Wants() { return g_cfg.enabled != 0; }

    void SetTemporal(IDirect3DDevice9* d, float maxLength, float clampSigmas)
    {
        const float row[4] = { maxLength, clampSigmas, 0.015f, 0.9f };
        d->SetPixelShaderConstantF(43, row, 1);
    }

    /// The resolution of the pass being drawn and its scale in full-resolution pixels (c45).
    void SetResolution(IDirect3DDevice9* d, UINT w, UINT h, int scale)
    {
        const float row[4] = { 1.0f / float(w), 1.0f / float(h), float(scale), 0.0f };
        d->SetPixelShaderConstantF(45, row, 1);
    }

    /// Accumulates raw into acc[current] with its meta, then a-trous iterations alternating between
    /// raw (free once accumulated) and out; returns the texture holding the finished result.
    IDirect3DTexture9* Denoise(IDirect3DDevice9* d, IDirect3DPixelShader9* psTemporal, IDirect3DPixelShader9* psAtrous,
                               Target& raw, Target acc[2], Target meta[2], Target& out,
                               UINT w, UINT h, int scale, float maxLength, float clampSigmas, int iterations)
    {
        const int cur = g_current, prev = 1 - g_current;
        SetResolution(d, w, h, scale);
        SetTemporal(d, maxLength, clampSigmas);
        d->SetRenderTarget(0, acc[cur].rt);
        d->SetRenderTarget(1, meta[cur].rt);
        d->SetPixelShader(psTemporal);
        render::Sampler(d, 1, g_normals.tex, false);
        render::Sampler(d, 3, raw.tex, false);
        render::Sampler(d, 4, g_normalsPrev.tex, false);
        render::Sampler(d, 5, acc[prev].tex, true);
        render::Sampler(d, 6, meta[prev].tex, false);
        render::Quad(d, w, h);
        d->SetRenderTarget(1, nullptr);

        IDirect3DTexture9* result = acc[cur].tex;
        if (iterations <= 0) return result;
        d->SetPixelShader(psAtrous);
        render::Sampler(d, 6, meta[cur].tex, false);
        for (int i = 0; i < iterations; ++i)
        {
            Target& dst = i % 2 == 0 ? raw : out;
            const float row[4] = { float(1 << i), 0.02f, 16.0f, 4.0f };
            d->SetPixelShaderConstantF(44, row, 1);
            d->SetRenderTarget(0, dst.rt);
            render::Sampler(d, 3, result, false);
            render::Quad(d, w, h);
            result = dst.tex;
        }
        return result;
    }

    /// One line naming the lights: counts by kind, the brightest as published (its unbounded peak
    /// beside, where it sits), the omni slots.
    void DescribeLights(const wxl::forever::passes::Frame& f, char* out, size_t size)
    {
        // The lights: counts by kind, and the brightest as published (its unbounded peak beside).
        int count = 0, kinds[4] = {};
        const fl::Light* list = fl::Current(count);
        int best = -1;
        float bestPeak = 0.0f, bestRaw = 0.0f, bestRadius = 0.0f;
        for (int i = 0; i < count; ++i)
        {
            kinds[std::min(int(list[i].kind), 3)]++;
            float rgb[3], radius = 0.0f;
            const float raw = fl::PublishedColour(list[i], rgb, radius);
            const float p = std::max(std::max(rgb[0], rgb[1]), rgb[2]);
            if (p > bestPeak) { bestPeak = p; bestRaw = raw; bestRadius = radius; best = i; }
        }
        char brightest[200] = "none";
        if (best >= 0)
        {
            const fl::Light& l = list[best];
            const float r[3] = { l.position[0] - f.eye[0], l.position[1] - f.eye[1], l.position[2] - f.eye[2] };
            float clip[4] = {};
            for (int j = 0; j < 4; ++j) clip[j] = r[0] * f.viewProjRel[j] + r[1] * f.viewProjRel[4 + j] + r[2] * f.viewProjRel[8 + j] + f.viewProjRel[12 + j];
            const char* where = clip[3] <= 0.0f ? "behind the camera"
                              : (std::fabs(clip[0]) <= clip[3] && std::fabs(clip[1]) <= clip[3] ? "on screen" : "off screen");
            static const char* const kKinds[] = { "M2", "WMO", "table", "given" };
            std::snprintf(brightest, sizeof brightest, "%s #%d peak %.2f (unbounded %.2f) reach %.1f yd at %.1f yd, %s%s",
                          kKinds[std::min(int(l.kind), 3)], best, bestPeak, bestRaw, bestRadius,
                          std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), where, l.carried ? ", carried" : "");
        }
        char slots[64] = "";
        for (int i = 0; i < 4; ++i)
        {
            const int idx = i < g_omniSlotCount ? g_omniSlots[i].lightIndex : -1;
            std::snprintf(slots + std::strlen(slots), sizeof slots - std::strlen(slots), "%s%d", i ? "," : "", idx);
        }
        std::snprintf(out, size, "lights %d (M2 %d, WMO %d, table %d, given %d), longest cluster list %d | brightest: %s | "
                      "omni slots %s, %d moving | engine materials %s | scene lighting %.3f",
                      count, kinds[0], kinds[1], kinds[2], kinds[3], fl::ClusterPeak(), brightest, slots, g_movingCount,
                      g_engineBound ? "on" : "off", SceneLighting());
    }

    /// Probe light values: Enc(x) = log2(1 + 64 x) / 10 on 8 bits (shaders/probe.ps.hlsl).
    float Dec(uint8_t v) { return (std::exp2(float(v) / 255.0f * 10.0f) - 1.0f) / 64.0f; }

    /// One of the probe's three decoded points, what the lighting passes and the depth say there, the
    /// nearest published light and the light reaching it most, for the log.
    void DescribePoint(const uint8_t* where, const uint8_t* reach, const uint8_t* depth, const float eye[3], char* out, size_t size)
    {
        // A8R8G8B8 bytes: b, g, r, a.
        if (where[3] == 0) { std::snprintf(out, size, "sky"); return; }
        const float p[3] = { (float(where[2]) / 255.0f - 0.5f) * 64.0f, (float(where[1]) / 255.0f - 0.5f) * 64.0f,
                             (float(where[0]) / 255.0f - 0.5f) * 64.0f };
        const float dist = float(where[3]) / 255.0f * 256.0f;
        const float lightingDist = float(depth[2]) / 255.0f * 256.0f;
        const float raw = float(int(depth[1]) * 256 + int(depth[0])) / 65535.0f;
        const float depth01 = float(depth[3]) / 255.0f;
        int count = 0;
        const fl::Light* list = fl::Current(count);
        char nearest[160] = "none";
        float best = 1e9f;
        for (int i = 0; i < count; ++i)
        {
            const fl::Light& l = list[i];
            const float r[3] = { l.position[0] - eye[0] - p[0], l.position[1] - eye[1] - p[1], l.position[2] - eye[2] - p[2] };
            const float d = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
            if (d >= best) continue;
            best = d;
            float rgb[3], radius = 0.0f;
            fl::PublishedColour(l, rgb, radius);
            std::snprintf(nearest, sizeof nearest, "#%d at (%.1f %.1f %.1f), %.1f yd away, radius %.1f",
                          i, l.position[0] - eye[0], l.position[1] - eye[1], l.position[2] - eye[2], d, radius);
        }
        const int index = int(reach[2]) - 1;
        char light[200] = "none in reach";
        if (index >= 0 && index < count)
        {
            const fl::Light& l = list[index];
            float rgb[3], radius = 0.0f;
            fl::PublishedColour(l, rgb, radius);
            const float r[3] = { l.position[0] - eye[0] - p[0], l.position[1] - eye[1] - p[1], l.position[2] - eye[2] - p[2] };
            static const char* const kKinds[] = { "M2", "WMO", "table", "given" };
            std::snprintf(light, sizeof light, "#%d %s at %.1f yd, radius %.1f, reference %.1f, colour (%.2f %.2f %.2f), share %.3f",
                          index, kKinds[std::min(int(l.kind), 3)], std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]), radius,
                          l.innerRadius, rgb[0], rgb[1], rgb[2], Dec(reach[1]));
        }
        std::snprintf(out, size, "at (%.1f %.1f %.1f) camera-relative, %.1f yd (the lighting passes saw %.1f yd), depth %.5f "
                      "(%.3f of the world range), buffer %.3f, %d lights in reach, nearest light %s, strongest %s",
                      p[0], p[1], p[2], dist, lightingDist, raw, depth01, Dec(reach[3]),
                      int(float(reach[0]) / 255.0f * 32.0f + 0.5f), nearest, light);
    }

    /// The spread of one stage over the probe's surface samples.
    struct Spread
    {
        float lo = 1e9f, hi = 0.0f, above = 0.0f;
        double sum = 0.0;
        float Mean(int n) const { return n > 0 ? float(sum / n) : 0.0f; }
        bool Uniform(int n) const { return n >= 200 && hi > 0.01f && Mean(n) > 0.6f * hi; }
    };

    /// Reads every probe the GPU has finished (the query answers without flushing, the lock does
    /// not wait) and logs: every few seconds each stage's spread and how far the lighting passes'
    /// surface points sit from the screen's; at once, when apply adds light where the buffer is about
    /// zero, when the lighting passes reconstruct other surface points than the screen shows, or when
    /// the resolved buffer is a near-uniform field (its mean above 0.6 of its maximum), naming the
    /// first stage that was already uniform, with three decoded points and light 0 as published.
    void ReadProbes(const wxl::forever::passes::Frame& f)
    {
        enum { kRaw, kTemporal, kDenoised, kResolve, kRatio, kOmniPass, kGi, kStages };
        static const char* const kStageNames[kStages] = { "raw", "temporal", "denoised", "resolve", "omni share", "omni pass",
                                                          "indirect" };
        for (Probe& pr : g_probes)
        {
            if (!pr.pending || pr.query->GetData(nullptr, 0, 0) != S_OK) continue;
            D3DLOCKED_RECT lr{};
            if (FAILED(pr.rt->LockRect(&lr, nullptr, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT))) continue;
            pr.pending = false;
            static float values[kStages][kProbeGridH][kProbeGridW];
            static bool surface[kProbeGridH][kProbeGridW];
            Spread spread[kStages];
            double added = 0.0, dist = 0.0, reach = 0.0;
            float addedMax = 0.0f, strayMax = 0.0f, distMin = 1e9f, distMax = 0.0f;
            int stray = 0, surfaces = 0, off = 0;
            for (UINT y = 0; y < kProbeGridH; ++y)
            {
                const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
                for (UINT x = 0; x < kProbeGridW; ++x)
                {
                    // A8R8G8B8 bytes b, g, r, a. Block 0: distance, added, resolve, reach; block 1:
                    // denoised, temporal, raw; block 2: the lighting passes' distance, omni share, indirect.
                    const uint8_t* t0 = row + x * 4;
                    const uint8_t* t1 = row + (kProbeGridW + x) * 4;
                    const uint8_t* t2 = row + (2 * kProbeGridW + x) * 4;
                    surface[y][x] = t0[3] != 0;
                    if (!surface[y][x]) continue;
                    ++surfaces;
                    values[kResolve][y][x] = Dec(t0[2]);
                    values[kRaw][y][x] = Dec(t1[2]);
                    values[kTemporal][y][x] = Dec(t1[1]);
                    values[kDenoised][y][x] = Dec(t1[0]);
                    values[kRatio][y][x] = float(t2[1]) / 255.0f;
                    values[kOmniPass][y][x] = float(t2[3]) / 255.0f;
                    values[kGi][y][x] = Dec(t2[2]);
                    for (int s = 0; s < kStages; ++s)
                    {
                        const float v = values[s][y][x];
                        spread[s].sum += v;
                        spread[s].lo = std::min(spread[s].lo, v);
                        spread[s].hi = std::max(spread[s].hi, v);
                    }
                    const float a = Dec(t0[1]), dd = float(t0[0]) / 255.0f * 256.0f;
                    const float lightingDist = float(t2[0]) / 255.0f * 256.0f;
                    // 8-bit distances step a yard: more than 2 yd plus a tenth apart is another surface.
                    if (std::fabs(lightingDist - dd) > 2.0f + 0.1f * dd) ++off;
                    added += a;
                    dist += dd;
                    reach += float(t0[3]) / 255.0f * 32.0f - 1.0f;
                    addedMax = std::max(addedMax, a);
                    distMin = std::min(distMin, dd);
                    distMax = std::max(distMax, dd);
                    if (values[kResolve][y][x] < 0.0005f && a > 0.004f) { ++stray; strayMax = std::max(strayMax, a); }
                }
            }
            char points[3][600] = {};
            {
                const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + size_t(kProbeGridH) * lr.Pitch;
                for (int k = 0; k < 3; ++k)
                    DescribePoint(row + (3 * k) * 4, row + (3 * k + 1) * 4, row + (3 * k + 2) * 4, f.eye, points[k], sizeof points[k]);
            }
            pr.rt->UnlockRect();
            if (surfaces == 0) continue;
            for (UINT y = 0; y < kProbeGridH; ++y)
                for (UINT x = 0; x < kProbeGridW; ++x)
                    if (surface[y][x])
                        for (int s = 0; s < kStages; ++s)
                            if (values[s][y][x] >= 0.5f * spread[s].hi) spread[s].above += 1.0f;

            const float n = float(surfaces);
            const float meanAdded = float(added / n), offShare = float(off) / n;
            char stages[640] = "";
            for (int s = 0; s < kStages; ++s)
                std::snprintf(stages + std::strlen(stages), sizeof stages - std::strlen(stages), "%s%s %.3f/%.3f/%.3f (%.0f%%)",
                              s ? " | " : "", kStageNames[s], spread[s].lo, spread[s].Mean(surfaces), spread[s].hi,
                              spread[s].above / n * 100.0f);
            char lights[400];
            const DWORD now = GetTickCount();
            if (now - g_summaryLogged > (g_cfg.washLog ? 2000u : 10000u))
            {
                g_summaryLogged = now;
                WLOG_INFO("surface: self-check: min/mean/max (share above half the max) over %d surface samples: %s | added mean %.3f "
                          "max %.3f | distance %.1f..%.1f yd | lighting passes' distance off at %.0f%% of samples | %d omni maps, "
                          "%d held by a light of the list | longest cluster list %d, %d entries",
                          surfaces, stages, meanAdded, addedMax, distMin, distMax, offShare * 100.0f, g_omniSlotCount, g_omniHeld,
                          fl::ClusterPeak(), fl::ClusterEntries());
            }
            if (stray > 8 && now - g_strayLogged > 5000)
            {
                g_strayLogged = now;
                DescribeLights(f, lights, sizeof lights);
                WLOG_WARN("surface: self-check: apply adds light where the light buffer is about zero at %d of %d samples "
                          "(up to %.4f) | buffer mean %.4f, added mean %.4f max %.4f | %s",
                          stray, surfaces, strayMax, spread[kResolve].Mean(surfaces), meanAdded, addedMax, lights);
            }
            if (surfaces >= 200 && offShare > 0.25f && now - g_offLogged > 5000)
            {
                g_offLogged = now;
                WLOG_WARN("surface: self-check: the lighting passes light other surface points than the screen shows: their "
                          "distance is off at %.0f%% of %d samples (the pass resolution or its texel mapping, PassUv)",
                          offShare * 100.0f, surfaces);
                for (int k = 0; k < 3; ++k)
                    WLOG_INFO("surface: self-check: point %s: %s", k == 0 ? "centre" : (k == 1 ? "lower centre" : "left"), points[k]);
            }
            // A pool of light is bright near its lamp and falls off: a mean near the maximum means
            // the light is spread evenly over the view.
            if (spread[kResolve].Uniform(surfaces) && now - g_uniformLogged > 5000)
            {
                g_uniformLogged = now;
                int first = kResolve;
                for (int s = kRaw; s < kResolve; ++s)
                    if (spread[s].Uniform(surfaces)) { first = s; break; }
                DescribeLights(f, lights, sizeof lights);
                WLOG_WARN("surface: self-check: near-uniform light field, already at the %s stage | %s | lighting passes' distance "
                          "off at %.0f%% | added mean %.3f max %.3f | distance %.1f..%.1f yd (mean %.1f) | lights in reach per "
                          "sample %.1f | %s", kStageNames[first], stages, offShare * 100.0f, meanAdded, addedMax, distMin, distMax,
                          float(dist / n), float(reach / n), lights);
                for (int k = 0; k < 3; ++k)
                    WLOG_INFO("surface: self-check: point %s: %s", k == 0 ? "centre" : (k == 1 ? "lower centre" : "left"), points[k]);
                int count = 0;
                const fl::Light* list = fl::Current(count);
                char row[600];
                fl::DescribePublished(0, count > 0 ? list : nullptr, row, sizeof row);
                WLOG_INFO("surface: self-check: %s", row);
            }
        }
    }

    /// A free probe slot, its target and query made on first use; null when none is free.
    Probe* FreeProbe(IDirect3DDevice9* d)
    {
        if (g_probeFailed) return nullptr;
        Probe* slot = nullptr;
        for (Probe& pr : g_probes)
            if (!pr.pending) { slot = &pr; break; }
        if (!slot) return nullptr;
        if (!slot->rt && (FAILED(d->CreateRenderTarget(kProbeW, kProbeH, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, TRUE,
                                                        &slot->rt, nullptr))
                          || FAILED(d->CreateQuery(D3DQUERYTYPE_EVENT, &slot->query))))
        {
            g_probeFailed = true;
            WLOG_WARN("surface: self-check unavailable on this device (lockable target or event query)");
            return nullptr;
        }
        return slot;
    }

    /// Copies the raw light pass down to the probe's grid before the denoiser reuses its target.
    void SnapshotRaw(IDirect3DDevice9* d)
    {
        if (g_probeRawFailed) return;
        if (!g_probeRaw.rt && !Make(d, kProbeGridW, kProbeGridH, g_probeRaw))
        {
            Release(g_probeRaw);
            g_probeRawFailed = true;
            WLOG_WARN("surface: self-check: no target for the raw light's copy; its stage reads 0");
            return;
        }
        if (FAILED(d->StretchRect(g_raw.rt, nullptr, g_probeRaw.rt, nullptr, D3DTEXF_POINT)))
        {
            g_probeRawFailed = true;
            WLOG_WARN("surface: self-check: the raw light cannot be copied on this device; its stage reads 0");
        }
    }

    /// Draws a probe into a free slot and issues its query. The caller restores the render target.
    void IssueProbe(IDirect3DDevice9* d, IDirect3DPixelShader9* ps, IDirect3DTexture9* gi, IDirect3DTexture9* ratio,
                    IDirect3DTexture9* denoised)
    {
        if (!ps) return;
        Probe* slot = FreeProbe(d);
        if (!slot) return;
        SetResolution(d, kProbeGridW, kProbeGridH, 1);
        d->SetRenderTarget(0, slot->rt);
        d->SetPixelShader(ps);
        render::Sampler(d, 1, ratio, false);
        render::Sampler(d, 2, g_scene.tex, false);
        render::Sampler(d, 3, g_lightBuffer, false);
        render::Sampler(d, 4, gi, true);
        render::Sampler(d, 5, g_probeRawFailed ? nullptr : g_probeRaw.tex, false);
        render::Sampler(d, 6, g_acc[g_current].tex, false);
        render::Sampler(d, 7, denoised, false);
        render::Sampler(d, 12, g_meta[g_current].tex, false);
        render::Sampler(d, 13, g_omniVis.tex, false);
        render::Quad(d, kProbeW, kProbeH);
        slot->query->Issue(D3DISSUE_END);
        slot->pending = true;
    }

    void Draw(const wxl::forever::passes::Frame& f)
    {
        IDirect3DDevice9* d = f.device;
        // The engine's materials are done with last frame's light buffer: this pass writes a new one,
        // and nothing drawn after the world pass (portraits, previews) adds it.
        d->SetTexture(wxl::game::gbuffer::kLightBufferSampler, nullptr);
        static const float kOff[4] = {};
        wxl::game::gbuffer::SetEnginePixelConstants(wxl::game::gbuffer::kLightBufferParams, kOff, 1);
        if (!fl::Frame(f)) { std::snprintf(g_status, sizeof g_status, "surface: light textures unavailable"); return; }
        int count = 0;
        fl::Current(count);
        if (count <= 0)
        {
            std::snprintf(g_status, sizeof g_status, "surface: no light near the camera");
            g_historyValid = false;
            g_lightBuffer = g_engineBuffer = nullptr;
            return;
        }
        if (!g_mrtChecked)
        {
            g_mrtChecked = true;
            D3DCAPS9 caps{};
            g_mrt = SUCCEEDED(d->GetDeviceCaps(&caps)) && caps.NumSimultaneousRTs >= 2;
            if (!g_mrt) WLOG_WARN("surface: the device draws to one target at a time; the light stays raw (no denoise)");
        }

        namespace sh = wxl::forever::shaders;
        IDirect3DVertexShader9* vs = sh::Vertex(d, "core.fullscreen");
        IDirect3DPixelShader9* psLight = sh::Pixel(d, "surface.light");
        IDirect3DPixelShader9* psApply = sh::Pixel(d, "surface.apply");
        IDirect3DPixelShader9* psNormals = sh::Pixel(d, "surface.normals");
        IDirect3DPixelShader9* psBlur = sh::Pixel(d, "surface.blur");
        IDirect3DPixelShader9* psGi = sh::Pixel(d, "surface.gi");
        IDirect3DPixelShader9* psTemporal = sh::Pixel(d, "surface.temporal");
        IDirect3DPixelShader9* psAtrous = sh::Pixel(d, "surface.atrous");
        IDirect3DPixelShader9* psResolve = sh::Pixel(d, "surface.resolve");
        IDirect3DPixelShader9* psOmniFilter = sh::Pixel(d, "surface.omnifilter");
        IDirect3DPixelShader9* psProbe = sh::Pixel(d, "surface.probe");
        IDirect3DPixelShader9* psRoom = sh::Pixel(d, "surface.room");
        IDirect3DPixelShader9* psOmni = sh::Pixel(d, "surface.omni");
        if (g_cfg.selfCheck) ReadProbes(f);
        if (!vs || !psLight || !psApply || !psNormals || !psBlur || !psGi || !psTemporal || !psAtrous || !psResolve) return;

        const int scale = g_cfg.resolution >= 2 ? 2 : 1;

        render::StateGuard guard(d, kRegisters);
        g_timer.Begin(d);
        IDirect3DSurface9* target = nullptr;
        d->GetRenderTarget(0, &target);
        if (!target) { g_timer.End(); return; }
        D3DSURFACE_DESC desc{};
        target->GetDesc(&desc);
        if (!Ensure(d, desc, scale) || FAILED(d->StretchRect(target, nullptr, g_scene.rt, nullptr, D3DTEXF_NONE)))
        {
            g_timer.End();
            target->Release();
            return;
        }
        g_timer.Mark(kSpanCopy);

        // --- constants, all of them, before any shader is bound -------------------------------------
        static float c[kRegisters][4];
        std::memset(c, 0, sizeof c);
        float inv[16];
        if (!mx::Invert4(f.viewProjRel, inv)) { g_timer.End(); target->Release(); return; }
        mx::Columns(inv, &c[0]);
        const float head[2][4] = {
            { 1.0f / float(f.width), 1.0f / float(f.height), float(std::clamp(g_cfg.view, 0, kViewEngineAdded)), 0.0f },
            { f.rangeMin, 1.0f / std::max(f.rangeMax - f.rangeMin, 1e-6f), f.rangeMax, 0.0f },
        };
        std::memcpy(c[4], head, sizeof head);
        c[6][0] = float(count);
        c[6][1] = g_cfg.noShadows ? 0.0f : float(std::clamp(g_cfg.shadowSteps, 0, 6));
        c[6][2] = std::max(g_cfg.strength, 0.0f);
        c[6][3] = std::clamp(g_cfg.wrap, 0.0f, 1.0f);
        fl::ClusterConstants(c[7], c[8]);
        c[9][0] = g_cfg.noSpecular ? 0.0f : std::max(g_cfg.specular, 0.0f);
        c[9][1] = std::clamp(g_cfg.wetness, 0.0f, 1.0f);
        // The albedo estimate divides the scene by this; at night the sky gives 0.05 while the ground
        // is lit by far more (the engine's own ambient), so it never goes below the floor.
        c[9][2] = std::max(SceneLighting(), std::clamp(g_cfg.albedoFloor, 0.02f, 1.0f));
        c[9][3] = g_cfg.fogDimming ? wxl::forever::media::Extinction() : 0.0f;
        mx::Columns(f.viewProjRel, &c[10]);

        const float soft = std::clamp(g_cfg.softness, 0.0f, 1.0f);
        c[14][0] = 0.35f;                      // albedo assumed where the scene is too dark to tell
        c[14][1] = soft;
        c[14][2] = std::clamp(g_cfg.highlightKnee, 0.2f, 0.95f);
        c[14][3] = 0.5f + 1.5f * soft;         // on each light's own source size
        c[15][0] = 1.0f / float(g_halfW); c[15][1] = 1.0f / float(g_halfH); c[15][2] = float(g_halfW); c[15][3] = float(g_halfH);

        const float moved[3] = { f.eye[0] - g_prevEye[0], f.eye[1] - g_prevEye[1], f.eye[2] - g_prevEye[2] };
        const bool keep = g_historyValid && moved[0] * moved[0] + moved[1] * moved[1] + moved[2] * moved[2] < 25.0f;
        mx::Columns(keep ? g_prevViewProj : f.viewProjRel, &c[16]);
        c[20][0] = moved[0]; c[20][1] = moved[1]; c[20][2] = moved[2];
        c[20][3] = g_cfg.noChangeCut ? 0.0f : std::clamp(g_cfg.changeCut, 0.0f, 8.0f);

        const float smooth = std::clamp(g_cfg.normalSmoothing, 0.0f, 1.0f);
        const float blurTaps = std::floor(1.0f + 5.0f * smooth + 0.5f), blurCos = 0.97f + (0.77f - 0.97f) * smooth;

        // Blue noise, rotated every frame by the R4 sequence: stratified over time per pixel.
        const float step = float(f.index % 4096 + 1);
        const float alpha[4] = { 0.8566749f, 0.7338919f, 0.6287067f, 0.5385973f };
        for (int i = 0; i < 4; ++i) c[22][i] = step * alpha[i] - std::floor(step * alpha[i]);

        // Normals the engine wrote (its G-buffer) where it did, or ones set from elsewhere.
        IDirect3DTexture9* normals = f.normals && g_cfg.gbuffer && !g_cfg.noGBuffer ? g_gbuffer.tex : nullptr;
        c[23][0] = normals ? 1.0f : 0.0f;
        c[23][1] = g_cfg.noProfiles ? 1.0f : 0.0f;
        {
            float carried[4];
            fl::CarriedConstants(carried);
            c[23][2] = g_cfg.noHotCore ? 0.0f : carried[2];
        }
        {
            float viewRel[16];
            mx::CameraRelativeView(wxl::game::camera::GetView(), f.eye, viewRel);
            // World normal = view normal times the inverse rotation, the transpose for a rotation.
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j) c[46 + i][j] = viewRel[i * 4 + j];
        }
        c[24][0] = 1.0f / float(g_quarterW); c[24][1] = 1.0f / float(g_quarterH); c[24][2] = float(g_quarterW); c[24][3] = float(g_quarterH);
        const bool indirect = !g_cfg.noIndirect && g_cfg.indirect > 0.0f && g_mrt;
        c[25][0] = indirect ? g_cfg.indirect : 0.0f;
        c[25][1] = std::clamp(g_cfg.indirectRadius, 0.5f, 12.0f);
        c[25][2] = std::max(g_cfg.emissive, 0.0f);

        float unitRows[kMaxUnits][4] = {};
        const int unitCount = GatherUnits(f.eye, unitRows);
        std::memcpy(c[26], unitRows, sizeof unitRows);
        c[42][0] = float(unitCount);
        c[42][1] = 0.9f;
        c[42][2] = EngineLightsOff() ? 1.0f : std::clamp(g_cfg.engineOnUnits, 0.0f, 1.0f);

        // Shadows: omni bias in yards plus a slope term in map texels, contact reach, filter taps.
        c[125][0] = std::clamp(g_cfg.shadowBias, 0.0f, 0.5f);
        c[125][1] = g_cfg.noSlopeBias ? 0.0f : std::clamp(g_cfg.shadowSlope, 0.0f, 4.0f);
        c[125][2] = std::clamp(g_cfg.contactLength, 0.25f, 10.0f);
        c[125][3] = g_cfg.plainOmniFilter || g_cfg.omniTaps < 5 ? 0.0f : float(std::min(g_cfg.omniTaps, 12));
        c[126][0] = 1.0f / float(g_lightW); c[126][1] = 1.0f / float(g_lightH); c[126][2] = float(g_lightW); c[126][3] = float(g_lightH);
        c[127][0] = std::max(g_cfg.highlightCap, 0.0f);
        c[127][1] = kAlbedoCap;
        c[127][2] = std::clamp(g_cfg.unitMargin, 0.0f, 3.0f);
        c[128][1] = float(scale);
        c[128][2] = scale == 2 ? 1.0f : 0.0f;
        c[128][3] = std::clamp(g_cfg.omniSelf, 0.0f, 2.0f);
        // The light cookies (the light service's atlas of each lamp's own housing).
        fl::cookies::Constants(c[130], c[131], c[132]);

        {
            float carried[4];
            fl::CarriedConstants(carried);
            c[152][0] = carried[0];
            c[152][1] = std::clamp(g_cfg.carriedOnCarrier, 0.0f, 1.0f);
            c[152][2] = g_cfg.noNearCap ? 1e6f : carried[1];
            c[152][3] = g_cfg.noCarried ? 0.0f : 1.0f;
        }
        c[158][0] = std::clamp(g_cfg.indirectOnUnits, 0.0f, 1.0f);
        // The rooms the light service built with its list (fl::Frame above), smallest first.
        {
            namespace rooms = wxl::forever::lights::rooms;
            c[158][1] = psRoom ? float(rooms::Count()) : 0.0f;
            c[158][2] = fl::RoomCross();
            std::memcpy(c[159], rooms::Rows(), sizeof(float) * 4 * 3 * size_t(rooms::Count()));
        }
        c[153][2] = 0.75f;

        const bool omniWanted = g_cfg.omniShadows && !g_cfg.noOmni;
        const bool omniReady = omniWanted && OmniShadows(d, f.eye, c);
        if (!omniWanted)
        {
            g_omniChangedCount = 0;
            g_omniSlotCount = g_omniHeld = 0;
            fl::PublishOmni(d, nullptr, 0, 0.0f, f.index);
        }
        // The omni term goes to its own target and joins after the history.
        const bool omniApart = omniReady && g_mrt;
        c[153][3] = omniApart ? 1.0f : 0.0f;
        {
            // Lights that moved lately, then shadow maps that changed, nearest first, four at most.
            fl::Moving moving[kMoving];
            int m = fl::MovingLights(moving, kMoving, !g_cfg.noCarried);
            for (int i = 0; i < g_omniChangedCount && m < kMoving; ++i) moving[m++] = g_omniChanged[i];
            for (int i = 0; i < m; ++i)
            {
                for (int k = 0; k < 3; ++k) c[154 + i][k] = moving[i].position[k] - f.eye[k];
                c[154 + i][3] = moving[i].radius;
            }
            c[153][0] = float(m);
            g_movingCount = m;
        }
        // Only the engine-lit pixels of this frame's world pass were lit by the engine's buffer.
        c[150][0] = g_engineBound && f.normals ? 1.0f : 0.0f;
        c[150][1] = g_engineWeight;

        const bool temporal = g_mrt && keep && !g_cfg.noTemporal;
        const float maxLength = temporal ? float(std::clamp(g_cfg.historyLength, 1, 64)) : 1.0f;
        const int iterations = g_mrt && !g_cfg.noDenoise ? std::clamp(g_cfg.denoise, 0, 3) : 0;
        const bool probing = g_cfg.selfCheck && psProbe && f.index % 8 == 0;

        render::PlainState(d);
        d->SetVertexShader(vs);
        d->SetPixelShaderConstantF(0, &c[0][0], kRegisters);
        g_current = 1 - g_current;
        std::swap(g_normals, g_normalsPrev);
        IDirect3DTexture9* blue = wxl::forever::bluenoise::Get(d);
        render::Sampler(d, 10, blue, false, true);
        render::Sampler(d, 0, f.depth, false);

        // --- normals: reconstructed from the depth at half resolution, blurred across and down ------
        d->SetRenderTarget(0, g_normalsRaw.rt);
        d->SetPixelShader(psNormals);
        render::Quad(d, g_halfW, g_halfH);
        d->SetPixelShader(psBlur);
        const float across[4] = { 1.0f, 0.0f, blurTaps, blurCos };
        const float down[4]   = { 0.0f, 1.0f, blurTaps, blurCos };
        d->SetPixelShaderConstantF(21, across, 1);
        d->SetRenderTarget(0, g_normalsAcross.rt);
        render::Sampler(d, 1, g_normalsRaw.tex, false);
        render::Quad(d, g_halfW, g_halfH);
        d->SetPixelShaderConstantF(21, down, 1);
        d->SetRenderTarget(0, g_normals.rt);
        render::Sampler(d, 1, g_normalsAcross.tex, false);
        render::Quad(d, g_halfW, g_halfH);
        g_timer.Mark(kSpanNormals);

        // --- each lighting texel's room, while the rooms gate the lamps -------------------------------
        const bool roomGate = c[158][1] > 0.5f && c[158][2] < 1.0f;
        if (roomGate)
        {
            d->SetRenderTarget(0, g_room.rt);
            d->SetPixelShader(psRoom);
            SetResolution(d, g_lightW, g_lightH, scale);
            render::Quad(d, g_lightW, g_lightH);
        }

        // --- each lighting texel's share of the omni slots, before the light pass reads it per light --
        const bool omniPass = omniReady && psOmni;
        if (omniPass)
        {
            d->SetRenderTarget(0, g_omniVis.rt);
            d->SetPixelShader(psOmni);
            SetResolution(d, g_lightW, g_lightH, scale);
            render::Sampler(d, 1, g_normals.tex, false);
            render::Sampler(d, 4, normals, false);
            render::Quad(d, g_lightW, g_lightH);
        }

        // --- the raw light, at the lighting resolution: every lit pixel sums its cluster's lights ---
        if (omniApart)
        {
            d->SetRenderTarget(0, g_ratio.rt);
            d->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 255, 255, 255), 1.0f, 0);
        }
        d->SetRenderTarget(0, g_raw.rt);
        d->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        if (omniApart) d->SetRenderTarget(1, g_ratio.rt);
        d->SetPixelShader(psLight);
        SetResolution(d, g_lightW, g_lightH, scale);
        render::Sampler(d, 1, g_normals.tex, false);
        render::Sampler(d, 2, g_scene.tex, false);
        render::Sampler(d, 4, normals, false);
        render::Sampler(d, 5, roomGate ? g_room.tex : nullptr, false);
        render::Sampler(d, 6, omniPass ? g_omniVis.tex : nullptr, false);
        render::Sampler(d, 7, fl::OmniTexture(), false);
        render::Sampler(d, 8, fl::LightTexture(), false);
        render::Sampler(d, 9, fl::ClusterTexture(), false);
        fl::cookies::Bind(d, 15);
        render::Quad(d, g_lightW, g_lightH);
        if (omniApart) d->SetRenderTarget(1, nullptr);
        d->SetTexture(15, nullptr);
        if (probing) SnapshotRaw(d);
        g_timer.Mark(kSpanLight);

        // --- accumulated over time and denoised -----------------------------------------------------
        IDirect3DTexture9* result = g_mrt ? Denoise(d, psTemporal, psAtrous, g_raw, g_acc, g_meta, g_out,
                                                    g_lightW, g_lightH, scale, maxLength, 1.25f, iterations)
                                          : g_raw.tex;
        // The omni share joins after the history, so its PCSS noise is filtered here, in space only:
        // a-trous passes on depth and normal, alternating between the share and its scratch.
        IDirect3DTexture9* ratio = omniApart ? g_ratio.tex : nullptr;
        const int omniPasses = omniApart && psOmniFilter ? std::clamp(g_cfg.omniDenoise, 0, 3) : 0;
        if (omniPasses > 0)
        {
            SetResolution(d, g_lightW, g_lightH, scale);
            d->SetPixelShader(psOmniFilter);
            render::Sampler(d, 1, g_normals.tex, false);
            render::Sampler(d, 6, g_meta[g_current].tex, false);
            for (int i = 0; i < omniPasses; ++i)
            {
                Target& dst = i % 2 == 0 ? g_ratioB : g_ratio;
                const float row[4] = { float(1 << i), 0.02f, 8.0f, 0.0f };
                d->SetPixelShaderConstantF(44, row, 1);
                d->SetRenderTarget(0, dst.rt);
                render::Sampler(d, 7, ratio, false);
                render::Quad(d, g_lightW, g_lightH);
                ratio = dst.tex;
            }
        }
        g_timer.Mark(kSpanDenoise);

        // --- resolve to full resolution: the omni share, the cap, the two published buffers ----------
        SetResolution(d, g_fullW, g_fullH, 1);
        d->SetRenderTarget(0, g_light.rt);
        if (g_mrt) d->SetRenderTarget(1, g_engine.rt);
        d->SetPixelShader(psResolve);
        render::Sampler(d, 3, result, false);
        render::Sampler(d, 6, g_meta[g_current].tex, false);
        render::Sampler(d, 7, ratio, false);
        render::Quad(d, g_fullW, g_fullH);
        if (g_mrt) d->SetRenderTarget(1, nullptr);
        g_lightBuffer = g_light.tex;
        g_engineBuffer = g_mrt ? g_engine.tex : nullptr;
        g_bufferFrame = f.index;
        g_timer.Mark(kSpanResolve);

        // --- one bounce of indirect light at quarter resolution, accumulated and denoised the same way
        IDirect3DTexture9* gi = nullptr;
        if (indirect)
        {
            SetResolution(d, g_quarterW, g_quarterH, 4);
            d->SetRenderTarget(0, g_giRaw.rt);
            d->SetPixelShader(psGi);
            render::Sampler(d, 1, g_normals.tex, false);
            render::Sampler(d, 2, g_scene.tex, false);
            render::Sampler(d, 3, g_lightBuffer, false);
            render::Sampler(d, 6, normals, false);
            render::Quad(d, g_quarterW, g_quarterH);
            gi = Denoise(d, psTemporal, psAtrous, g_giRaw, g_giAcc, g_giMeta, g_giOut,
                         g_quarterW, g_quarterH, 4, maxLength, 2.0f, std::max(iterations, 1));
        }
        g_timer.Mark(kSpanIndirect);

        // --- apply over the scene, read from the copy while the pass writes over it -----------------
        if (!gi) { c[25][0] = 0.0f; d->SetPixelShaderConstantF(25, c[25], 1); }
        d->SetRenderTarget(0, target);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        d->SetPixelShader(psApply);
        render::Sampler(d, 2, g_scene.tex, false);
        render::Sampler(d, 3, g_lightBuffer, false);
        render::Sampler(d, 4, gi, false);
        render::Sampler(d, 5, g_giMeta[g_current].tex, false);
        render::Sampler(d, 6, normals, false);
        render::Sampler(d, 7, g_engineBuffer, false);
        render::Quad(d, desc.Width, desc.Height);
        if (probing)
        {
            d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
            IssueProbe(d, psProbe, gi, ratio, result);
            d->SetRenderTarget(0, target);
        }
        g_timer.Mark(kSpanApply);
        g_timer.End();
        target->Release();
        for (DWORD s = 11; s <= 14; ++s) d->SetTexture(s, nullptr);

        std::memcpy(g_prevViewProj, f.viewProjRel, sizeof g_prevViewProj);
        std::memcpy(g_prevEye, f.eye, sizeof g_prevEye);
        g_historyValid = true;

        const float ms = g_timer.Milliseconds();
        std::snprintf(g_passes, sizeof g_passes,
                      "GPU copy %.3f | normals %.3f | light %.3f | denoise %.3f | resolve %.3f | indirect %.3f | apply %.3f | total %.3f ms",
                      std::max(g_timer.Milliseconds(kSpanCopy), 0.0f), std::max(g_timer.Milliseconds(kSpanNormals), 0.0f),
                      std::max(g_timer.Milliseconds(kSpanLight), 0.0f), std::max(g_timer.Milliseconds(kSpanDenoise), 0.0f),
                      std::max(g_timer.Milliseconds(kSpanResolve), 0.0f), std::max(g_timer.Milliseconds(kSpanIndirect), 0.0f),
                      std::max(g_timer.Milliseconds(kSpanApply), 0.0f), std::max(ms, 0.0f));
        std::snprintf(g_status, sizeof g_status,
                      "surface: %d lights, %d units, %d moving, %d omni maps, lighting 1/%d, scene lighting %.2f, fog extinction %.4f/yd%s",
                      count, unitCount, g_movingCount, omniReady ? int(std::min(g_omni->Count(), 4u)) : 0, scale, c[9][2], c[9][3],
                      g_mrt ? "" : ", no MRT: raw light");
        // What could make the lighting alternate: omni slots changing hands, rooms entering the list,
        // the engine materials' share moving. A line every five seconds while any of it happened.
        static DWORD lastStability = 0;
        if (GetTickCount() - lastStability > 5000)
        {
            lastStability = GetTickCount();
            const int rooms = wxl::forever::lights::rooms::Changes();
            if (g_slotChanges > 0 || rooms > 0 || g_weightLo < 0.99f)
                WLOG_INFO("surface: stability over 5 s: %d omni slot hand-overs, %d rooms entered the list, engine materials "
                          "share %.2f..%.2f (screen motion up to %.0f px a frame)",
                          g_slotChanges, rooms, g_weightLo, g_weightHi, g_motionMax);
            g_slotChanges = 0;
            g_weightLo = 1.0f;
            g_weightHi = 0.0f;
            g_motionMax = 0.0f;
        }
        static DWORD lastLog = 0;
        if (ms >= 0.0f && GetTickCount() - lastLog > 10000)
        {
            lastLog = GetTickCount();
            WLOG_INFO("surface: %s | %s", g_status, g_passes);
        }
    }

    void OverviewTab()
    {
        ui::Check("Enabled", &g_cfg.enabled,
                  "Lights terrain, buildings and models with every light near the camera, not only the four the game gives each model.");
        ui::Text(g_status);
        ui::Text(g_passes);
        ui::Text(fl::Status());
    }

    void LightingTab()
    {
        static const char* const kResolutions[] = { "Full", "Half" };
        int res = g_cfg.resolution >= 2 ? 1 : 0;
        if (ui::Combo("Lighting resolution", &res, kResolutions, 2,
                      "The resolution the lights, shadows and history are computed at. Half costs about a quarter of full and is brought "
                      "back up along depth edges; soft light hides the difference, very sharp shadow edges show it a little."))
            g_cfg.resolution = res == 1 ? 2 : 1;
        ui::Slider("Softness", &g_cfg.softness, 0.0f, 1.0f,
                   "How soft the lighting reads overall: a longer, gentler fall of light away from each lamp and wider shadow edges. Lower it for a crisper, harsher look.");
        ui::Slider("Strength", &g_cfg.strength, 0.0f, 3.0f,
                   "How strongly lamps and torches light the surfaces around them. Raise it for bright pools of light, lower it for a subtle glow.");
        ui::Slider("Highlight cap", &g_cfg.highlightCap, 0.0f, 8.0f,
                   "The brightness the added light rolls off towards, so a surface touching a lamp never burns to white while the pool keeps its gradient. Lower is gentler; 0 turns the cap off.");
        ui::Slider("Highlight knee", &g_cfg.highlightKnee, 0.2f, 0.95f,
                   "The brightness where the lit result starts easing towards white instead of clipping. Lower values start the shoulder earlier and look softer.");
        ui::Slider("Albedo lighting floor", &g_cfg.albedoFloor, 0.02f, 1.0f,
                   "A surface's colour is guessed as the scene divided by the light that lit it. At night the sky gives very little, while the ground is lit by more, so this is the least light assumed. Too low and every surface reads as bright and saturated, and a faint lamp floods a whole field with its colour.");
        ui::Slider("Wrap", &g_cfg.wrap, 0.0f, 1.0f,
                   "How far light rolls around a surface past where it faces away. Higher values give softer, fuller lighting.");
        ui::Slider("Specular", &g_cfg.specular, 0.0f, 2.0f,
                   "The highlight where a light reflects towards you, physically based: rough pale plaster barely shines, dark stone a little more. Set 0 for matte surfaces.");
        ui::Slider("Wetness", &g_cfg.wetness, 0.0f, 1.0f,
                   "How wet every surface is: wet ground turns smoother and throws sharper, brighter reflections of the lamps. Meant to follow the rain later.");
        ui::Slider("Indirect light", &g_cfg.indirect, 0.0f, 2.0f,
                   "Light that bounces once off nearby surfaces: a lamp's warm pool spills onto the walls beside it and colours them. Set 0 to turn it off.");
        ui::Slider("Indirect radius", &g_cfg.indirectRadius, 0.5f, 12.0f,
                   "How far that bounce reaches, in yards. Larger radii spread the colour further and look softer.");
        ui::Slider("Glowing surfaces", &g_cfg.emissive, 0.0f, 2.0f,
                   "How much the scene's own glowing pixels (lantern glass, fire, lit windows) light the surfaces around them.");
        ui::Check("Fog dims the light", &g_cfg.fogDimming,
                  "When the fog is on, light crossing it to reach a surface is dimmed by it.");
        ui::Slider("Normal smoothing", &g_cfg.normalSmoothing, 0.0f, 1.0f,
                   "Smooths the surface directions read from the depth, so characters and rounded shapes look smooth instead of faceted. Corners of walls and floors stay sharp. Set 0 for the raw directions.");
        ui::Separator();
        ui::Text("Shadows");
        ui::Check("Omni shadow maps", &g_cfg.omniShadows,
                  "The four most important lights cast true shadows from the game's own shadow casters, near the player (needs shadow quality 1 or more). A lamp's own cage, post or bracket is among them, so it shapes its own light.");
        ui::Slider("Omni filter taps", &g_cfg.omniTaps, 0, 12,
                   "How the shadow maps are filtered. 5 to 12: a search for what casts the shadow, then a filter as wide as the penumbra it throws, so a cage bar an inch from the flame stays crisp on the wall and a post yards away softens; more taps are smoother. 0 to 4: a plain 4-tap filter.");
        ui::Check("Shadow maps to lights with units first", &g_cfg.omniUnitsFirst,
                  "The four shadow maps go first to lights with a character or creature near them (a zombie passing a lantern must cast its shadow), then to lights without a cookie; a still lamp whose baked cookie already throws its own cage takes none. Off: importance order alone.");
        ui::Slider("Own housing (yards)", &g_cfg.omniSelf, 0.0f, 1.5f,
                   "Casters this close to a light do not shadow it: the lamp's own head, glass and bracket, which its cookie shapes instead. Without it a lamp that earns a shadow map (the nearest ones) goes dark around itself, on the ground and in the fog.");
        ui::Slider("Omni shadow denoise", &g_cfg.omniDenoise, 0, 3,
                   "Smoothing passes over the shadow the maps cast, before it joins the light. Each pass averages the filter's grain over a wider patch of the same surface, never across to another one. 0 shows it raw, as a fine grain in the penumbras and on grazing ground; 3 is widest and softens the crispest shadows a little.");
        static const char* const kFaces[] = { "256", "512", "1024" };
        int face = g_cfg.omniFaceSize >= 1024 ? 2 : (g_cfg.omniFaceSize >= 512 ? 1 : 0);
        if (ui::Combo("Omni face size", &face, kFaces, 3,
                      "Texels per side of each shadow map face. Larger keeps thin struts and rails in the shadow; 1024 costs four times the memory and fill of 512. Takes effect when the maps are next created."))
            g_cfg.omniFaceSize = face == 2 ? 1024 : (face == 1 ? 512 : 256);
        ui::Slider("Omni refresh (faces per frame)", &g_cfg.omniRefresh, 1, 6,
                   "How many shadow map faces of still lights are redrawn each frame, in turn, to catch what the game moves on its own (swaying trees, doors). A face a character walks through, or a light that moves, is redrawn at once whatever this says. Lower is cheaper; 3 refreshes each face about every eighth frame with four maps.");
        ui::Slider("Shadow bias (yards)", &g_cfg.shadowBias, 0.0f, 0.2f,
                   "How far a surface is considered in front of its own shadow map depth. Too low shows speckles (acne) on lit surfaces, too high detaches shadows from the feet of what casts them.");
        ui::Slider("Slope bias (texels)", &g_cfg.shadowSlope, 0.0f, 4.0f,
                   "Extra bias on surfaces the light grazes, in shadow map texels: removes the acne bands on floors and slopes without pushing shadows away where the light falls straight on.");
        ui::Slider("Shadow steps", &g_cfg.shadowSteps, 0, 6,
                   "Checks the screen a short way from each surface towards each light, so props, door frames and the terrain (which the game's shadows never cast) shade what touches them. More steps catch thinner occluders and cost more. 0 turns it off.");
        ui::Slider("Contact shadow length (yards)", &g_cfg.contactLength, 0.25f, 10.0f,
                   "How far from the surface those screen checks reach, for every light: enough for a frame or a crate, not the whole way to the light, so what the camera sees in front of a light never switches it off.");
        ui::Separator();
        ui::Slider("History length (frames)", &g_cfg.historyLength, 1, 64,
                   "How many frames a still pixel averages. Longer histories remove the grain of soft shadows and bounced light; moving things always keep a short one.");
        ui::Slider("History cut on change (sigmas)", &g_cfg.changeCut, 0.0f, 4.0f,
                   "How far the remembered light may drift from what this frame shows, measured in the local noise, before the history is dropped. Low values react at once to a passing torch or a flare but average less; 0 turns the test off.");
        ui::Slider("Denoise passes", &g_cfg.denoise, 0, 3,
                   "Edge-aware smoothing passes over the light after the history. They respect depth, surface direction and brightness edges, and skip pixels whose history has settled. 0 turns it off.");
        ui::Check("G-buffer normals", &g_cfg.gbuffer,
                  "Uses the true surface directions the game's own shaders write for models, buildings and grass (needs shadow quality 1 or more), instead of rebuilding them from the depth.");
        ui::Check("Engine materials read the light", &g_cfg.engineMaterials,
                  "The game's own materials add the light directly as they draw, so lamps light models and buildings in their true colours. Needs shadow quality 1 or more. Characters are left out of that buffer and lit here instead, since it is last frame's and they move.");
        ui::Slider("Unit margin (yards)", &g_cfg.unitMargin, 0.0f, 3.0f,
                   "How far around each character the buffer the game's materials read is cleared. It covers the character's movement since last frame, so a running character does not read the bright floor beside its feet.");
        ui::Slider("Engine lights on characters", &g_cfg.engineOnUnits, 0.0f, 1.0f,
                   "The game already lights characters and creatures with its own model lights. At 0 those lights are not added again on them; raise it to add them on purpose.");
        ui::Slider("Carried light on carrier", &g_cfg.carriedOnCarrier, 0.0f, 1.0f,
                   "How much a light a character holds (a torch, a lantern, a glowing weapon) adds on that character itself. It sits a few inches from the body, so at full strength it burns the character out.");
        ui::Slider("Carried light core (yards)", &fl::Settings().carriedCore, 0.0f, 2.0f,
                   "How wide the bright heart of a light a character holds is. Wider cores keep a torch from flaring on whatever passes right next to it; the light further out is unchanged. Shared with the fog.");
        ui::Slider("Near-light cap", &fl::Settings().nearCap, 0.5f, 5.0f,
                   "The most any light gives right next to its source, in multiples of its own colour. Lower it if surfaces touching a lamp or torch burn white. Shared with the fog; 5 is uncapped.");
        ui::Slider("Indirect light on characters", &g_cfg.indirectOnUnits, 0.0f, 1.0f,
                   "How much of the bounced light is added on characters and creatures. The floor a torch lights bounces a lot of warm light up; the game already lights characters itself, so a share is enough.");
        ui::Check("Shadow maps for carried lights", &g_cfg.carriedShadowMaps,
                  "Lets a light a character holds take one of the four shadow maps. Its carrier and the item itself then shadow the floor around them heavily, since they sit right at the light.");
    }

    void DebugTab()
    {
        static const char* const kViews[] = { "Lit scene", "Surface lights only", "Normals", "Indirect only", "Specular only",
                                              "Omni shadow atlas (first light)", "Omni faces (first light)", "Engine light buffer",
                                              "Albedo estimate", "Engine materials added light" };
        ui::Combo("View", &g_cfg.view, kViews, 10,
                  "Shows one ingredient on its own: the light added, the surface directions, the bounced light, the highlights, "
                  "the first shadow map, which face of it each pixel reads (+X red, -X dark red, +Y green, -Y dark green, +Z blue, "
                  "-Z dark blue, white off its cell, black out of reach; darker where it shadows), the buffer the game's materials read "
                  "(characters cleared), the albedo the lights are multiplied by, or exactly what the game's own materials added "
                  "as they drew (black where they added nothing and this feature lights the pixel itself).");
        ui::Separator();
        ui::Check("Self-check", &g_cfg.selfCheck,
                  "Every eighth frame a tiny sample of each lighting stage (raw light, its history, the denoised light, the final buffer, the shadow maps' share, the bounced light) is read back a few frames later, without waiting for the graphics card. Every ten seconds a line goes to the log with each stage's range; a warning goes out at once when light is added where no light reaches, when the lights are computed at other surface points than the screen shows, or when the light is spread evenly over the whole view, naming the first stage where it already was, with three points of the screen and the lights near each.");
        ui::Check("Log light washes", &g_cfg.washLog,
                  "With the self-check on: the line with each lighting stage's range over the view (minimum, mean, maximum) goes to the log every two seconds instead of every ten.");
        ui::Separator();
        ui::Text("Isolate: each switch removes one ingredient.");
        ui::Check("No hot core", &g_cfg.noHotCore, "A flame's light keeps one colour from its source to its edge on surfaces (the light service's Hot core is ignored here).");
        ui::Check("No change-based history cut", &g_cfg.noChangeCut,
                  "The history is never shortened for a change of lighting, only clamped. Shows the trail a carried torch would leave without it.");
        ui::Check("Plain omni filter", &g_cfg.plainOmniFilter, "The shadow maps use the plain 4-tap filter with no penumbra estimate.");
        ui::Check("No slope bias", &g_cfg.noSlopeBias, "Only the constant bias is applied to the shadow maps. Shows the acne on grazing floors.");
        ui::Check("No denoise", &g_cfg.noDenoise, "Removes the edge-aware smoothing passes.");
        ui::Check("No temporal", &g_cfg.noTemporal, "Removes the history: each frame shows its own raw light.");
        ui::Check("No shadows", &g_cfg.noShadows, "Lights shine through everything the screen shows between them and a surface.");
        ui::Check("No indirect light", &g_cfg.noIndirect, "Removes the bounced light.");
        ui::Check("No specular", &g_cfg.noSpecular, "Removes the highlights.");
        ui::Check("No light profiles", &g_cfg.noProfiles, "Every light shines evenly in all directions: no cage bars, hoods or grilles.");
        ui::Check("No omni shadows", &g_cfg.noOmni, "The four main lights stop using their shadow maps; only the screen-space steps remain.");
        ui::Check("No G-buffer normals", &g_cfg.noGBuffer, "Uses only the normals rebuilt from the depth, even where the game wrote its own.");
        ui::Check("No carried-light handling", &g_cfg.noCarried,
                  "Lights held by characters are treated like any other model light: the usual core, nothing of them on characters, a shadow map if they rank, and a history cut only while they move.");
        ui::Check("No near-light cap", &g_cfg.noNearCap, "Lights are not capped next to their source.");
    }

    class SurfaceModule final : public wxl::ext::EventScript
    {
    public:
        SurfaceModule() { on<&SurfaceModule::OnDeviceLost>(ev::Event::OnDeviceLost); }

        void OnDeviceLost(const ev::DeviceResetArgs&)
        {
            ReleaseAll();
            g_timer.Release();
        }
    };
}

namespace wxl::forever::surface
{
    Settings& Config() { return g_cfg; }

    LightBuffer CurrentLightBuffer()
    {
        return LightBuffer{ g_lightBuffer, uint32_t(D3DFMT_A16B16G16R16F), g_fullW, g_fullH, g_bufferFrame };
    }

    void UiOverview() { ui::Scope scope("surface.overview"); OverviewTab(); }

    void UiSettings() { ui::Scope scope("surface"); LightingTab(); }

    void UiDebug() { ui::Scope scope("surface.debug"); DebugTab(); }

    void Install()
    {
        using wxl_forever::ConfigBool;
        using wxl_forever::ConfigFloat;
        g_cfg.enabled       = ConfigBool("WXL_FOREVER_SURFACE_ENABLED", true) ? 1 : 0;
        g_cfg.resolution    = int(ConfigFloat("WXL_FOREVER_SURFACE_RESOLUTION", float(g_cfg.resolution), 1.0f, 2.0f));
        g_cfg.strength      = ConfigFloat("WXL_FOREVER_SURFACE_STRENGTH", g_cfg.strength, 0.0f, 10.0f);
        g_cfg.wrap          = ConfigFloat("WXL_FOREVER_SURFACE_WRAP", g_cfg.wrap, 0.0f, 1.0f);
        g_cfg.softness      = ConfigFloat("WXL_FOREVER_SURFACE_SOFTNESS", g_cfg.softness, 0.0f, 1.0f);
        g_cfg.specular      = ConfigFloat("WXL_FOREVER_SURFACE_SPECULAR", g_cfg.specular, 0.0f, 4.0f);
        g_cfg.wetness       = ConfigFloat("WXL_FOREVER_SURFACE_WETNESS", g_cfg.wetness, 0.0f, 1.0f);
        g_cfg.highlightCap  = ConfigFloat("WXL_FOREVER_SURFACE_HIGHLIGHT_CAP", g_cfg.highlightCap, 0.0f, 16.0f);
        g_cfg.highlightKnee = ConfigFloat("WXL_FOREVER_SURFACE_HIGHLIGHT_KNEE", g_cfg.highlightKnee, 0.2f, 0.95f);
        g_cfg.indirect      = ConfigFloat("WXL_FOREVER_SURFACE_INDIRECT", g_cfg.indirect, 0.0f, 4.0f);
        g_cfg.indirectRadius = ConfigFloat("WXL_FOREVER_SURFACE_INDIRECT_RADIUS", g_cfg.indirectRadius, 0.5f, 12.0f);
        g_cfg.emissive      = ConfigFloat("WXL_FOREVER_SURFACE_EMISSIVE", g_cfg.emissive, 0.0f, 4.0f);
        g_cfg.shadowSteps   = int(ConfigFloat("WXL_FOREVER_SURFACE_SHADOW_STEPS", float(g_cfg.shadowSteps), 0.0f, 6.0f));
        g_cfg.shadowBias    = ConfigFloat("WXL_FOREVER_SURFACE_SHADOW_BIAS", g_cfg.shadowBias, 0.0f, 0.5f);
        g_cfg.shadowSlope   = ConfigFloat("WXL_FOREVER_SURFACE_SHADOW_SLOPE", g_cfg.shadowSlope, 0.0f, 4.0f);
        g_cfg.contactLength = ConfigFloat("WXL_FOREVER_SURFACE_CONTACT_LENGTH", g_cfg.contactLength, 0.25f, 10.0f);
        g_cfg.omniTaps      = int(ConfigFloat("WXL_FOREVER_SURFACE_OMNI_TAPS", float(g_cfg.omniTaps), 0.0f, 12.0f));
        g_cfg.omniDenoise   = int(ConfigFloat("WXL_FOREVER_SURFACE_OMNI_DENOISE", float(g_cfg.omniDenoise), 0.0f, 3.0f));
        g_cfg.omniSelf      = ConfigFloat("WXL_FOREVER_SURFACE_OMNI_SELF", g_cfg.omniSelf, 0.0f, 2.0f);
        g_cfg.omniUnitsFirst = ConfigBool("WXL_FOREVER_SURFACE_OMNI_UNITS_FIRST", g_cfg.omniUnitsFirst != 0) ? 1 : 0;
        g_cfg.omniFaceSize  = int(ConfigFloat("WXL_FOREVER_SURFACE_OMNI_FACE_SIZE", float(g_cfg.omniFaceSize), 64.0f, 1024.0f));
        g_cfg.omniRefresh   = int(ConfigFloat("WXL_FOREVER_SURFACE_OMNI_REFRESH", float(g_cfg.omniRefresh), 1.0f, 6.0f));
        g_cfg.unitMargin    = ConfigFloat("WXL_FOREVER_SURFACE_UNIT_MARGIN", g_cfg.unitMargin, 0.0f, 3.0f);
        g_cfg.fogDimming    = ConfigBool("WXL_FOREVER_SURFACE_FOG_DIMMING", true) ? 1 : 0;
        g_cfg.washLog       = ConfigBool("WXL_FOREVER_SURFACE_WASH_LOG", false) ? 1 : 0;
        g_cfg.selfCheck     = ConfigBool("WXL_FOREVER_SURFACE_SELF_CHECK", true) ? 1 : 0;
        g_cfg.albedoFloor   = ConfigFloat("WXL_FOREVER_SURFACE_ALBEDO_FLOOR", g_cfg.albedoFloor, 0.02f, 1.0f);
        g_cfg.normalSmoothing = ConfigFloat("WXL_FOREVER_SURFACE_NORMAL_SMOOTHING", g_cfg.normalSmoothing, 0.0f, 1.0f);
        g_cfg.historyLength = int(ConfigFloat("WXL_FOREVER_SURFACE_HISTORY", float(g_cfg.historyLength), 1.0f, 64.0f));
        g_cfg.denoise       = int(ConfigFloat("WXL_FOREVER_SURFACE_DENOISE", float(g_cfg.denoise), 0.0f, 3.0f));
        g_cfg.engineOnUnits = ConfigFloat("WXL_FOREVER_SURFACE_ENGINE_ON_UNITS", g_cfg.engineOnUnits, 0.0f, 1.0f);
        g_cfg.carriedOnCarrier = ConfigFloat("WXL_FOREVER_SURFACE_CARRIED_ON_CARRIER", g_cfg.carriedOnCarrier, 0.0f, 1.0f);
        g_cfg.indirectOnUnits = ConfigFloat("WXL_FOREVER_SURFACE_INDIRECT_ON_UNITS", g_cfg.indirectOnUnits, 0.0f, 1.0f);
        g_cfg.carriedShadowMaps = ConfigBool("WXL_FOREVER_SURFACE_CARRIED_SHADOW_MAPS", false) ? 1 : 0;
        g_cfg.changeCut     = ConfigFloat("WXL_FOREVER_SURFACE_CHANGE_CUT", g_cfg.changeCut, 0.0f, 8.0f);
        g_cfg.gbuffer       = ConfigBool("WXL_FOREVER_SURFACE_GBUFFER", true) ? 1 : 0;
        g_cfg.engineMaterials = ConfigBool("WXL_FOREVER_SURFACE_ENGINE_MATERIALS", true) ? 1 : 0;
        g_cfg.omniShadows   = ConfigBool("WXL_FOREVER_SURFACE_OMNI_SHADOWS", true) ? 1 : 0;

        static SurfaceModule module;
        wxl::forever::passes::Add(100, "surface", &Wants, &Draw);
        wxl::forever::passes::AddBegin(&Begin);
        WLOG_INFO("surface: installed (enabled=%d resolution=1/%d strength=%.2f cap=%.2f shadow steps=%d omni taps=%d)",
                  g_cfg.enabled, g_cfg.resolution, g_cfg.strength, g_cfg.highlightCap, g_cfg.shadowSteps, g_cfg.omniTaps);
    }
}
