// wxl-forever fog: hands each frame to the froxel volume, with the panel, the config and the per-tick
// bookkeeping around it. The world's depth comes from core/SceneDepth.
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
#include "../core/SceneDepth.hpp"
#include "../core/Panel.hpp"
#include "../core/Passes.hpp"
#include "../core/Media.hpp"
#include "Fog.hpp"
#include "FogProfile.hpp"
#include "FogInputs.hpp"
#include "Froxel.hpp"
#include "../lights/Lights.hpp"
#include "../lights/Rooms.hpp"
#include "Terrain.hpp"
#include "Wakes.hpp"
#include "Projectiles.hpp"
#include "Plumes.hpp"
#include "../core/Matrix.hpp"
#include "NoiseVolume.hpp"
#include "../core/BlueNoise.hpp"
#include "../core/ShaderLibrary.hpp"

#include "wxl/EventScript.hpp"
#include "game/Camera.hpp"
#include "game/Gx.hpp"
#include "game/Lights.hpp"
#include "game/Sky.hpp"
#include "game/World.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <cstring>

namespace
{
    namespace ev    = wxl::events;
    namespace fog   = wxl::forever::fog;
    namespace cam   = wxl::game::camera;
    namespace world = wxl::game::world;
    namespace sky   = wxl::game::sky;
    namespace gx    = wxl::game::gx;

    fog::Settings g_cfg;

    bool                    g_haveBase[2] = {};    // a profile's base height came from config or was seeded
    fog::ResolvedProfile    g_resolved[2] = {};
    bool                    g_resolvedReady = false;
    bool                    g_cameraIndoor  = false;
    float                   g_indoorness    = 0.0f;

    /// The camera's indoor state eased over the indoor/outdoor transition (smootherstep: no visible
    /// start or stop); a flip mid-transition starts from the value on screen.
    float EaseIndoor(bool indoor)
    {
        static bool started = false, target = false;
        static float from = 0.0f, t = 1.0f;
        static LONGLONG last = 0;
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const float dt = last ? std::min(float(double(now.QuadPart - last) / double(frequency.QuadPart)), 0.25f) : 0.0f;
        last = now.QuadPart;
        if (!started || g_cfg.indoorTransition <= 0.0f)
        {
            started = true;
            target = indoor;
            t = 1.0f;
            g_indoorness = indoor ? 1.0f : 0.0f;
            return g_indoorness;
        }
        if (indoor != target)
        {
            from = g_indoorness;
            target = indoor;
            t = 0.0f;
        }
        t = std::min(t + dt / g_cfg.indoorTransition, 1.0f);
        const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        g_indoorness = from + ((target ? 1.0f : 0.0f) - from) * s;
        return g_indoorness;
    }
    int                     g_boxCount      = 0;
    bool                    g_havePlayer = false;
    float                   g_player[3] = {};

    /// The fog's clock: the performance counter's steps, each capped at a fifteenth of a second, so a
    /// frame hitch does not make the fog leap to catch up; it never wraps (a wrap snaps every drift).
    /// Held still by the freeze switch.
    float Seconds()
    {
        static LARGE_INTEGER frequency{};
        static LONGLONG last = 0;
        static double clock = 0.0;
        if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        const double step = last ? double(now.QuadPart - last) / double(frequency.QuadPart) : 0.0;
        last = now.QuadPart;
        if (!g_cfg.isolate.freezeTime) clock += std::clamp(step, 0.0, 1.0 / 15.0);
        return float(clock);
    }

    /// The lights the fog gives the light service: the debug lantern and lights other extensions fed
    /// through the fog's interface. Sent on the tick; the service gathers the client's own.
    void GiveLights()
    {
        namespace in = fog::inputs;
        namespace fl = wxl::forever::lights;
        fl::Light given[in::kMaxLights + 1];
        int count = 0;

        if (g_cfg.enabled && g_cfg.debugLantern && g_havePlayer)
        {
            fl::Light& l = given[count++];
            l = fl::Light{};
            l.kind = fl::Kind::Given;
            l.position[0] = g_player[0];
            l.position[1] = g_player[1];
            l.position[2] = g_player[2] + 1.8f;
            l.radius    = 14.0f;
            l.color[0]  = 1.0f;
            l.color[1]  = 0.62f;
            l.color[2]  = 0.28f;
            l.intensity = 1.5f;
            l.cosCone   = -2.0f;
        }
        for (int i = 0; i < in::LightCount() && count < in::kMaxLights + 1; ++i)
        {
            const in::Light& src = in::Lights()[i];
            fl::Light& l = given[count++];
            l = fl::Light{};
            for (int k = 0; k < 3; ++k)
            {
                l.position[k]  = src.position[k];
                l.color[k]     = src.color[k];
                l.direction[k] = src.direction[k];
            }
            l.radius      = src.radius;
            l.intensity   = src.intensity;
            l.cosCone     = src.cosCone;
            l.innerRadius = src.innerRadius;
            l.kind        = fl::Kind::Given;
        }
        fl::SetGiven(given, count);
    }

    /// This frame's density volumes: those other extensions fed, then the bodies and their wakes.
    int GatherVolumes(const float eye[3], const fog::ResolvedProfile& p, fog::inputs::Volume out[fog::inputs::kMaxVolumes])
    {
        namespace in = fog::inputs;
        int count = 0;
        for (int i = 0; i < in::VolumeCount() && count < in::kMaxVolumes; ++i)
        {
            out[count] = in::Volume{};
            static_cast<in::DensityVolume&>(out[count++]) = in::Volumes()[i];
        }
        if (g_cfg.plumes && !g_cfg.isolate.noPlumes && (p.smoke > 0.0f || p.fireSmoke > 0.0f))
        {
            fog::plumes::Tuning t;
            t.smoke     = p.smoke;
            t.fireSmoke = p.fireSmoke;
            count += fog::plumes::Emit(eye, t, out + count, in::kMaxVolumes - count);
        }
        const float wakeRadius = p.wakeRadius;
        if (g_cfg.wakes && !g_cfg.isolate.noWakes)
            count += fog::wakes::Emit(eye, wakeRadius, g_cfg.wakeTrail, out + count, in::kMaxVolumes - count);
        return count;
    }

    bool     g_froxelDrawn   = false;   // the volume drew this frame; the native fog may step aside

    bool DrawFroxel(const wxl::forever::passes::Frame& frame)
    {
        namespace fx = fog::froxel;
        IDirect3DDevice9* dev = frame.device;
        fx::FrameInput f{};
        f.device  = dev;
        f.depth   = frame.depth;
        f.depthW  = frame.width;
        f.depthH  = frame.height;
        std::copy(frame.eye, frame.eye + 3, f.eye);
        std::copy(frame.viewProjRel, frame.viewProjRel + 16, f.viewProjRel);
        f.seconds  = Seconds();
        f.rangeMin = frame.rangeMin;
        f.rangeMax = frame.rangeMax;

        if (!g_resolvedReady)
            fog::ResolveProfiles(0.0f, g_cfg.transitionTime, sky::FogColor(), g_resolved);
        f.outdoor = g_resolved[int(fog::Slot::Outdoor)];
        f.indoor  = g_resolved[int(fog::Slot::Indoor)];

        // The light service builds this frame's rooms with its list; the interior boxes are those rooms.
        namespace rooms = wxl::forever::lights::rooms;
        const bool lightsReady = wxl::forever::lights::Frame(frame);
        const int boxes = g_cfg.indoorDetect ? rooms::Count() : 0;
        const bool cameraIndoor = boxes > 0 && rooms::CameraIndoor();
        g_cameraIndoor = cameraIndoor;
        g_boxCount     = boxes;
        f.boxRows  = rooms::Rows();
        f.boxCount = boxes;
        f.cameraIndoor = cameraIndoor;
        f.indoorness = EaseIndoor(cameraIndoor);
        f.camera = fog::MixProfiles(f.outdoor, f.indoor, f.indoorness);
        if (!g_cfg.isolate.noCrossfade)
        {
            // Near wisps cross-fade rather than re-shape: their scale, churn and reach follow the
            // profile with the larger share, and their strength dips to zero at the switch, so the
            // outdoor wisps fade out and the indoor ones fade in.
            const fog::ResolvedProfile& lead = f.indoorness >= 0.5f ? f.indoor : f.outdoor;
            f.camera.nearScale = lead.nearScale;
            f.camera.nearTurbulence = lead.nearTurbulence;
            f.camera.nearDistance = lead.nearDistance;
            const float s = std::fabs(2.0f * f.indoorness - 1.0f);
            f.camera.nearStrength *= s * s * (3.0f - 2.0f * s);
        }
        f.farDistance = f.camera.maxDistance;
        f.ground = g_cfg.terrain ? fog::terrain::Texture(dev) : nullptr;
        f.groundFallback = g_havePlayer ? fog::terrain::Fallback() : f.eye[2];

        // The light service's list for this frame, the same the surfaces light with.
        namespace fl = wxl::forever::lights;
        int lightCount = 0;
        const fl::Light* lights = fl::Current(lightCount);
        f.lightCount = lightsReady ? lightCount : 0;
        f.lights = lights;

        // How much the fog around the camera dims light per yard, for surface lighting.
        wxl::forever::media::SetExtinction(f.camera.density);

        // Diagnostics: which light is nearest, of what kind, and whether the interior gate keeps it
        // out of the outdoor fog.
        static DWORD lastLightLog = 0;
        if (g_cfg.diagnostics && GetTickCount() - lastLightLog > 2000)
        {
            lastLightLog = GetTickCount();
            WLOG_INFO("fog: %s", wxl::forever::lights::Nearest());
            int best = -1;
            float bestSq = 1e30f;
            for (int i = 0; i < f.lightCount; ++i)
            {
                const float dx = lights[i].position[0] - f.eye[0], dy = lights[i].position[1] - f.eye[1],
                            dz = lights[i].position[2] - f.eye[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 < bestSq) { bestSq = d2; best = i; }
            }
            if (best >= 0)
                WLOG_INFO("fog: nearest used light %.1f yd, radius %.1f, colour (%.2f %.2f %.2f) x %.2f, %s",
                          std::sqrt(bestSq), lights[best].radius, lights[best].color[0], lights[best].color[1],
                          lights[best].color[2], lights[best].intensity,
                          lights[best].interior ? "inside an interior box: lights indoor fog only" : "outdoor");
        }

        // Projectile trails: sampled, aged and published for this frame's camera.
        const fog::ResolvedProfile& cp = f.camera;
        f.trails = false;
        if (g_cfg.projectiles && !g_cfg.isolate.noProjectiles)
        {
            fog::projectiles::Tuning t;
            t.carve      = cp.projectileCarve;
            t.radius     = cp.projectileRadius;
            t.hold       = cp.projectileHold;
            t.refill     = cp.projectileRefill;
            t.heat       = cp.projectileHeat;
            t.billow     = cp.trailBillow;
            t.billowSize = cp.trailBillowSize;
            t.roll       = cp.trailBillowRoll;
            t.smoke      = cp.trailSmoke;
            t.crater     = cp.projectileCrater;
            fog::projectiles::Update(f.eye, f.seconds, t);
            f.trails = fog::projectiles::Publish(dev, f.eye, f.viewProjRel);
        }

        static fog::inputs::Volume volumes[fog::inputs::kMaxVolumes];
        f.volumeCount = GatherVolumes(f.eye, f.camera, volumes);
        f.volumes = volumes;

        const bool drawn = fx::Render(f);
        g_froxelDrawn = drawn;
        static DWORD lastLog = 0;
        const DWORD tick = GetTickCount();
        if (drawn && tick - lastLog > 10000)
        {
            lastLog = tick;
            WLOG_INFO("fog: %s | wakes: %d bodies, %d volumes", wxl::forever::lights::Status(), fog::wakes::Tracked(), f.volumeCount);
        }
        return drawn;
    }

    namespace ui = wxl::forever::ui;

    int g_editSlot = 0;   // the profile the Volume, Lighting, Effects and Profiles tabs edit

    /// Grid, near wisps and world shadow steps for each quality level: Low, Medium, High, Ultra.
    void ApplyQuality(int quality)
    {
        struct Level { int x, y, z, wisps, shadowSteps; };
        static const Level kLevels[] = {
            { 96, 54, 32, 0, 6 },
            { 128, 72, 48, 1, 8 },
            { 160, 90, 64, 1, 10 },
            { 192, 108, 96, 1, 12 },
        };
        if (quality < 1 || quality > 4) return;
        const Level& l = kLevels[quality - 1];
        g_cfg.froxelX   = l.x;
        g_cfg.froxelY   = l.y;
        g_cfg.froxelZ   = l.z;
        g_cfg.nearField = l.wisps;
        g_cfg.shadowSteps = l.shadowSteps;
    }

    void ProfilePicker()
    {
        static const char* const kSlots[] = { "Outdoor", "Indoor" };
        ui::Combo("Profile being edited", &g_editSlot, kSlots, 2,
                  "The settings below belong to this profile. Outdoor applies in the open, indoor inside buildings.");
        ui::Separator();
    }

    /// The profile's knobs that belong on this tab.
    void ProfileKnobs(fog::ProfileTab tab)
    {
        const fog::Slot slot = fog::Slot(g_editSlot);
        fog::FogProfile& p = fog::Builtin(slot);
        const char* tag = g_editSlot == 0 ? "outdoor" : "indoor";
        char label[96];
        auto id = [&](const char* name) { std::snprintf(label, sizeof label, "%s##%s", name, tag); return label; };
        using fog::kTabVolume;
        using fog::kTabLighting;
        using fog::kTabEffects;

#define WXL_FOG_SLIDER(name, key, out, in, lo, hi, text, knobTab, help)             \
        if (knobTab == tab)                                                           \
        {                                                                             \
            ui::Slider(id(text), &p.name, lo, hi, help);                              \
            if (&p.name == &p.baseZ)                                                  \
            {                                                                         \
                wxl_forever::g_api->UiSameLine();                                     \
                if (wxl_forever::g_api->UiButton(id("Set to my feet")) && g_havePlayer) \
                {                                                                     \
                    p.baseZ = g_player[2];                                            \
                    g_haveBase[int(slot)] = true;                                     \
                }                                                                     \
                ui::Help("Puts the base height where you stand now.");                \
            }                                                                         \
        }
        WXL_FOG_PROFILE_FLOATS(WXL_FOG_SLIDER)
#undef WXL_FOG_SLIDER
    }

    void OverviewTab()
    {
        static const char* const kQualities[] = { "Custom", "Low", "Medium", "High", "Ultra" };
        ui::Check("Enabled", &g_cfg.enabled, "Turns the fog on or off.");
        if (ui::Combo("Quality", &g_cfg.quality, kQualities, 5,
                      "Sets the fog grid, the near wisps, the light shafts and the world shadow steps together. Low is fastest, Ultra is sharpest. Custom keeps your own values."))
            ApplyQuality(g_cfg.quality);
        ui::Separator();

        ui::Text(wxl::forever::depth::Status());
        ui::Text(fog::froxel::Status());
        ui::Text(fog::froxel::PassTimes());
        char line[160];
        std::snprintf(line, sizeof line, "interior groups drawn %d, boxes used %d | camera %s, fog %.0f%% indoor",
                      wxl::forever::lights::rooms::Collected(), g_boxCount, g_cameraIndoor ? "indoors" : "outdoors", g_indoorness * 100.0f);
        ui::Text(line);
        ui::Text(wxl::forever::lights::Status());
        std::snprintf(line, sizeof line, "bodies tracked %d | missiles %d, trail segments %d | ground cells measured %d of %d",
                      fog::wakes::Tracked(), fog::projectiles::Tracked(), fog::projectiles::Live(), fog::terrain::Filled(),
                      fog::terrain::kCells * fog::terrain::kCells);
        ui::Text(line);
        ui::Text(fog::plumes::Status());
    }

    void VolumeTab()
    {
        ProfilePicker();
        ui::Slider("Froxels across", &g_cfg.froxelX, 16, 320,
                   "Horizontal resolution of the fog grid. Raise it for crisper fog edges, lower it to save time.");
        ui::Slider("Froxels down", &g_cfg.froxelY, 9, 180,
                   "Vertical resolution of the fog grid.");
        ui::Slider("Slices", &g_cfg.froxelZ, 8, 128,
                   "Depth resolution of the fog grid. More slices give finer detail at distance and cost more.");
        ui::Slider("Near slice density", &g_cfg.sliceBend, 0.0f, 0.9f,
                   "Bends the slices towards you: higher values spend more slices on the first yards, where the wisps live, and fewer far away. 0 spaces them evenly in depth ratio. Changing it restarts the fog's history.");
        ui::Slider("First slice (yards)", &g_cfg.froxelNear, 0.05f, 10.0f,
                   "How far the first slice reaches from the camera. The slices beyond it grow from there; the fog inside it is still counted.");
        ui::Slider("Temporal (history kept)", &g_cfg.temporal, 0.0f, 0.98f,
                   "How much of the last frames the fog keeps. Higher values are smoother and steadier, lower values react faster.");
        ui::Check("Indoor detection", &g_cfg.indoorDetect,
                  "Uses the indoor profile inside buildings. Turn it off to use the outdoor profile everywhere.");
        ui::Check("Fog follows the ground", &g_cfg.terrain,
                  "Measures the terrain around you so the fog can hug slopes and pool in hollows.");
        ui::Slider("Ground traces per tick", &g_cfg.terrainBudget, 4, 256,
                   "How many ground heights are measured each frame. Raise it if the fog lags behind when you travel fast.");
        ui::Separator();
        ProfileKnobs(fog::kTabVolume);
    }

    void LightingTab()
    {
        static const char* const kPhases[] = { "Dual Henyey-Greenstein", "Cornette-Shanks" };
        ProfilePicker();
        ui::Text("Light sources are gathered by the light service: tab Lights, Gather.");
        ui::Slider("Indoor light margin", &g_cfg.lightMargin, 0.5f, 10.0f,
                   "How many yards a light from inside a building fades over at the building's edge, instead of stopping at a hard line.");
        ui::Check("Lamp shadow maps in the fog", &g_cfg.omniShadows,
                  "The shadow maps surface lighting renders for its four main lights also shade their glow in the fog, so a lantern's cage, a lamp post and passers-by throw beams and shadows through the mist. Needs surface lighting and its omni shadow maps on.");
        ui::Slider("Fog shadow bias (yards)", &g_cfg.omniBias, 0.0f, 1.0f,
                   "How far a bit of fog is pulled towards the light before its shadow map is read. Too low darkens the fog right against lit walls, too high lets beams leak through thin casters.");
        ui::Check("World shadows", &g_cfg.worldShadows,
                  "Trees, buildings and hills shade the fog from the sun and moon. Their strength is set per profile below.");
        ui::Slider("World shadow steps", &g_cfg.shadowSteps, 4, 12,
                   "Samples per froxel for the world shadows. More steps catch thinner occluders and cost more.");
        ui::Check("Sky occlusion", &g_cfg.skyOcclusion,
                  "Less sky light reaches fog under trees, between walls and in hollows. Its strength is set per profile below.");
        ui::Combo("Phase function", &g_cfg.phaseModel, kPhases, 2,
                  "How light scatters in the fog. Cornette-Shanks gives a brighter halo with a wider shoulder.");
        ui::Check("Dither", &g_cfg.dither,
                  "Adds a tiny amount of noise that hides banding in smooth fog gradients.");
        ui::Separator();
        ProfileKnobs(fog::kTabLighting);
    }

    void EffectsTab()
    {
        ProfilePicker();
        ui::Check("Near wisps", &g_cfg.nearField,
                  "Finer curls in the fog over the first yards in front of the camera. They are part of the fog volume itself (lit and shadowed with it), tuned per profile below.");
        ui::Check("Projectiles carve the fog", &g_cfg.projectiles,
                  "Spells, arrows and bullets punch a hole through the fog that stays open a moment, then refills slowly in rolling billows. Tuned per profile below.");
        ui::Check("Smoke plumes", &g_cfg.plumes,
                  "Chimneys, campfires and spell smoke put real smoke into the fog, which you can walk through. Their density is set per profile below.");
        ui::Check("Immersion", &g_cfg.immersion,
                  "Shows on screen when you stand inside dense fog or smoke: a veil of the fog's own light closing in from the edges, and softened lights. It reads the fog volume at the camera; its strength is set per profile below.");
        ui::Check("Bodies push the fog away", &g_cfg.wakes,
                  "Players and creatures clear the fog around them and leave a wake when they move.");
        ui::Slider("Body range", &g_cfg.wakeRange, 5.0f, 100.0f,
                   "How far from you bodies are tracked, in yards. The 16 nearest are used.");
        ui::Slider("Wake length (s)", &g_cfg.wakeTrail, 0.0f, 4.0f,
                   "How long a wake takes to close behind a moving body.");
        ui::Check("Push the client's own fog out", &g_cfg.nativeFog,
                  "Moves the game's distance fog to the far edge so it does not stack with this fog.");
        ui::Slider("Client fog start", &g_cfg.nativeFogStart, 0.0f, 0.99f,
                   "Where the game's own fog starts, as a share of the view distance. Keep it high.");
        ui::Separator();
        ProfileKnobs(fog::kTabEffects);
    }

    void ProfilesTab()
    {
        static const char* const kModes[] = { "Native fog colour", "Custom colour", "Blend" };
        ProfilePicker();
        fog::FogProfile& p = fog::Builtin(fog::Slot(g_editSlot));
        const char* tag = g_editSlot == 0 ? "outdoor" : "indoor";
        char label[96];
        auto id = [&](const char* name) { std::snprintf(label, sizeof label, "%s##%s", name, tag); return label; };

        ui::Combo(id("Colour"), &p.colorMode, kModes, 3,
                  "Native follows the zone's own fog colour through the day. Custom uses your colour, Blend mixes the two.");
        if (p.colorMode != int(fog::ColorMode::Native))
            ui::Color(id("Custom colour"), p.color, "The colour the fog takes in Custom and Blend.");
        if (p.colorMode == int(fog::ColorMode::Blend))
            ui::Slider(id("Blend (native to custom)"), &p.colorBlend, 0.0f, 1.0f,
                       "0 keeps the zone's colour, 1 uses yours.");
        ui::Slider(id("Colour brightness"), &p.brightness, 0.0f, 4.0f,
                   "Multiplies the fog colour. Lower it for darker fog without changing its density.");
        ui::Separator();
        ui::Slider("Profile transition (s)", &g_cfg.transitionTime, 0.0f, 20.0f,
                   "How long the fog takes to change when a different profile takes over.");
        ui::Slider("Indoor/outdoor transition (s)", &g_cfg.indoorTransition, 0.0f, 20.0f,
                   "How long the fog around you takes to change when you walk through a doorway, easing in and out. The fog you see through the doorway keeps its own kind. 0 switches at once.");
    }

    void DebugTab()
    {
        static const char* const kViews[] = { "Fog", "Slices (resolved atlas)", "Density (1 - T)",
                                              "Scatter only", "Transmittance", "History clamp (red)",
                                              "Lights only",
                                              "Terrain (r valleys, g height above ground)",
                                              "Linear depth", "Raw depth (r 0.9-1, g 0.99-1, b 0-1)",
                                              "Indoor (red) / outdoor (blue)", "Shadow visibility",
                                              "Sky visibility", "Fog banks (macro map)",
                                              "Noise pattern (the three blue-noise channels)",
                                              "Projectile trails (r carved, g billows, b fire smoke)" };
        ui::Combo("View", &g_cfg.froxelView, kViews, 16,
                  "Shows one ingredient of the fog on its own instead of the finished fog.");
        ui::Slider("Debug far", &g_cfg.debugFar, 10.0f, 1000.0f,
                   "The distance shown as white in the depth and indoor views, in yards.");
        if (ui::Button("Count injection paths", "Counts once how many cells of the fog volume take each path through injection (nothing possible, between banks, empty, thin, full) and writes it to the log. The full path is the costly one."))
            fog::froxel::RequestCensus();
        ui::Text(fog::froxel::Census());
        if (ui::Button("Reload shaders", "Reads every shader again from the client's files and rebuilds them live. With WXL_FOREVER_SHADER_DEV=1 the sources under Data/Patch-4.MPQ/Shaders/Forever/src are compiled first, so an edit shows at once."))
            wxl::forever::shaders::Reload();
        ui::Text(wxl::forever::shaders::Status());
        ui::Check("Lantern at the player", &g_cfg.debugLantern,
                  "A warm light at your character, to check that lights reach the fog.");
        ui::Check("Diagnostics", &g_cfg.diagnostics,
                  "Logs the sun, moon and glare inputs once a second.");
        if (g_cfg.diagnostics) ui::Text(fog::froxel::Diagnostics());
        ui::Separator();

        ui::Text("Isolate: each switch removes one ingredient, to find which one causes an artefact.");
        fog::Settings::Isolate& iso = g_cfg.isolate;
        ui::Check("Freeze jitter", &iso.freezeJitter,
                  "Stops the per-frame rotation of every sample offset, froxel and per-pixel. With it on, the Noise pattern view holds still.");
        ui::Check("Freeze noise time", &iso.freezeTime, "Stops the fog from drifting.");
        ui::Check("No history", &iso.noHistory, "Shows each frame on its own, without smoothing over time.");
        ui::Check("No history clamp", &iso.noClamp, "Keeps the past frames without limiting them to the current one.");
        ui::Check("No change-based history cut", &iso.noChangeCut,
                  "Where the fog changed (a torch's halo passed) the history keeps its full weight instead of weighing down. Shows the trail a halo would leave without it.");
        ui::Check("No lamp shadow maps in the fog", &iso.noOmniShadows, "The lamps' glow in the fog ignores the shadow maps: no beams, no cage shadows in the mist.");
        ui::Check("No heat", &iso.noHeat, "Flames no longer thin the fog around them.");
        ui::Check("No hot core", &iso.noHotCore, "A flame's glow in the fog keeps one colour from its source outwards (the light service's Hot core is ignored here).");
        ui::Check("No cloud light", &iso.noCloudLight, "The sun and moon light the fog evenly, with no drifting patches of cloud shade.");
        ui::Check("No doorway cross-fade", &iso.noCrossfade,
                  "Each bit of fog takes the outdoor or the indoor fog whole, switching at the midpoint of the transition, instead of cross-fading the two; the near wisps re-shape instead of fading. Shows what the cross-fade hides.");
        ui::Check("Flat density", &iso.flatDensity, "Removes the noise: an even height fog.");
        ui::Check("No self-shadow", &iso.noShadow, "Removes the fog's shading of itself.");
        ui::Check("No sun or moon", &iso.noCelestial, "Removes the sun and moon light from the fog.");
        ui::Check("No lights", &iso.noLights, "Removes lamps and torches from the fog.");
        ui::Check("No indoor boxes", &iso.noBoxes, "Treats everything as outdoors for the fog volume.");
        ui::Check("No wakes", &iso.noWakes, "Bodies no longer push the fog away.");
        ui::Check("No projectile trails", &iso.noProjectiles, "Missiles in flight no longer punch holes in the fog.");
        ui::Check("No smoke plumes", &iso.noPlumes, "Emitters no longer put smoke into the fog.");
        ui::Check("No immersion", &iso.noImmersion, "No veil or softened lights when inside the fog.");
        ui::Check("No horizon continuation", &iso.noHaze, "The fog stops at the volume's far distance instead of carrying on to the horizon: shows the seam the continuation hides.");
        ui::Check("No near wisps", &iso.noNear, "Removes the finer curls near the camera from the density.");
        ui::Check("No ground following", &iso.noTerrain, "Uses the flat base height instead of the terrain.");
        ui::Check("No surface contact", &iso.noContact, "Removes the fog layer on surfaces.");
        ui::Check("No flow", &iso.noFlow, "One noise layer and a still warp.");
        ui::Check("No world shadows", &iso.noWorldShadow, "Trees, buildings and hills no longer shade the fog.");
        ui::Check("No fog banks", &iso.noBanks, "Removes the large-scale banks and gaps: fog everywhere.");
        ui::Check("No high haze", &iso.noHighHaze, "Removes the upper haze layer, leaving the ground mist alone.");
        ui::Check("No sky occlusion", &iso.noSkyOcclusion, "The sky light reaches every bit of fog evenly.");
        ui::Check("No bank shading", &iso.noBankShading, "Removes the long bank shadows and the darkening under fog above.");
    }

    class FogModule final : public wxl::ext::EventScript
    {
    public:
        FogModule()
        {
            on<&FogModule::OnUpdate>(ev::Event::OnUpdate);
            on<&FogModule::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

        /**
         * @brief Keeps the client's own distance fog out of the volume's way while the volume draws.
         *
         * The override is installed again every tick with the live fog colour and sky gate, so only
         * the distance moves. An override somebody else installed is left alone.
         */
        void UpdateNativeFog()
        {
            const bool want = g_cfg.enabled && g_cfg.nativeFog && g_froxelDrawn;
            if (want && (g_ownOverride || !sky::OverrideFogActive()))
            {
                sky::SetOverrideFog(std::clamp(g_cfg.nativeFogStart, 0.0f, 0.99f), 20000.0f, sky::FogColor(), sky::SkyDrawGate());
                if (!g_ownOverride) WLOG_INFO("fog: client distance fog pushed to the far clip");
                g_ownOverride = true;
            }
            else if (!want && g_ownOverride)
            {
                sky::ClearOverrideFog();
                g_ownOverride = false;
                WLOG_INFO("fog: client distance fog restored");
            }
        }

        bool g_ownOverride = false;

        void OnUpdate(const ev::UpdateArgs& a)
        {
            fog::ResolveProfiles(a.dt, g_cfg.transitionTime, sky::FogColor(), g_resolved);
            g_resolvedReady = true;

            g_havePlayer = false;
            const unsigned long long guid = world::ActivePlayerGuid();
            if (!guid) return;
            void* unit = world::ResolveObject(guid, world::kTypeMaskPlayer);
            if (!unit) return;
            world::UnitPosition(unit, g_player);
            g_havePlayer = true;

            if (g_cfg.wakes) fog::wakes::Update(a.dt, g_player, true, g_cfg.wakeRange, g_cfg.wakeTrail);
            if (g_cfg.enabled && g_cfg.terrain)
            {
                float eye[3];
                cam::GetPosition(eye);
                fog::terrain::Update(eye, std::clamp(g_cfg.terrainBudget, 1, 512));
            }
            UpdateNativeFog();
            GiveLights();
            // A light in a room lights indoor fog, while the fog tells indoor from outdoor.
            wxl::forever::lights::SetInteriorGate(g_cfg.enabled && g_cfg.indoorDetect);

            // Without a configured base, the first known feet height becomes the fixed base.
            for (int i = 0; i < int(fog::Slot::Count); ++i)
            {
                if (g_haveBase[i]) continue;
                fog::Builtin(fog::Slot(i)).baseZ = g_player[2];
                g_haveBase[i] = true;
                WLOG_INFO("fog: %s base height seeded from the player's feet: %.2f",
                          i == int(fog::Slot::Outdoor) ? "outdoor" : "indoor", g_player[2]);
            }
        }

        /// The texture is DEFAULT pool; it goes before the reset and the device is probed again after.
        void OnDeviceLost(const ev::DeviceResetArgs&) { fog::froxel::ReleaseResources(); }
    };
}

namespace wxl::forever::fog
{
    Settings& Config() { return g_cfg; }
}

namespace
{
    bool WantsPass() { return g_cfg.enabled != 0; }

    void DrawPass(const wxl::forever::passes::Frame& frame)
    {
        if (!DrawFroxel(frame))
        {
            static bool logged = false;
            if (!logged) { logged = true; WLOG_WARN("fog: froxel volume unavailable on this device; fog inert"); }
        }
    }
}

namespace wxl::forever::fog
{
    void UiOverview() { ui::Scope scope("fog.overview"); OverviewTab(); }

    void UiSettings()
    {
        ui::Scope scope("fog");
        if (!ui::BeginTabs("forever.fog")) return;
        if (ui::BeginTab("Volume"))     { VolumeTab();   ui::EndTab(); }
        if (ui::BeginTab("Scattering")) { LightingTab(); ui::EndTab(); }
        if (ui::BeginTab("Effects"))    { EffectsTab();  ui::EndTab(); }
        if (ui::BeginTab("Profiles"))   { ProfilesTab(); ui::EndTab(); }
        ui::EndTabs();
    }

    void UiDebug() { ui::Scope scope("fog.debug"); DebugTab(); }

    void Install()
    {
        using wxl_forever::ConfigBool;
        using wxl_forever::ConfigFloat;
        using wxl_forever::ConfigRaw;

        {
            char quality[32];
            if (ConfigRaw("WXL_FOG_QUALITY", quality, sizeof quality))
            {
                const char q = char(quality[0] | 0x20);
                g_cfg.quality = q == 'l' ? 1 : q == 'm' ? 2 : q == 'h' ? 3 : q == 'u' ? 4 : 0;
                ApplyQuality(g_cfg.quality);
            }
        }
        g_cfg.enabled        = ConfigBool("WXL_FOG_ENABLED", true) ? 1 : 0;
        g_cfg.indoorDetect   = ConfigBool("WXL_FOG_INDOOR_DETECT", true) ? 1 : 0;
        g_cfg.debugFar       = ConfigFloat("WXL_FOG_DEBUG_FAR", g_cfg.debugFar, 1.0f, 5000.0f);
        g_cfg.transitionTime = ConfigFloat("WXL_FOG_TRANSITION", g_cfg.transitionTime, 0.0f, 60.0f);
        // Unset, the doorway takes the profile transition's time.
        g_cfg.indoorTransition = ConfigFloat("WXL_FOG_INDOOR_TRANSITION", g_cfg.transitionTime, 0.0f, 60.0f);
        g_cfg.froxelX        = int(ConfigFloat("WXL_FOG_FROXEL_X", float(g_cfg.froxelX), 16.0f, 320.0f));
        g_cfg.froxelY        = int(ConfigFloat("WXL_FOG_FROXEL_Y", float(g_cfg.froxelY), 9.0f, 180.0f));
        g_cfg.froxelZ        = int(ConfigFloat("WXL_FOG_FROXEL_Z", float(g_cfg.froxelZ), 8.0f, 128.0f));
        g_cfg.froxelNear     = ConfigFloat("WXL_FOG_FROXEL_NEAR", g_cfg.froxelNear, 0.05f, 10.0f);
        g_cfg.temporal       = ConfigFloat("WXL_FOG_TEMPORAL", g_cfg.temporal, 0.0f, 0.98f);
        g_cfg.lightMargin    = ConfigFloat("WXL_FOG_LIGHT_MARGIN", g_cfg.lightMargin, 0.1f, 20.0f);
        g_cfg.omniShadows    = ConfigBool("WXL_FOG_OMNI_SHADOWS", true) ? 1 : 0;
        g_cfg.omniBias       = ConfigFloat("WXL_FOG_OMNI_BIAS", g_cfg.omniBias, 0.0f, 2.0f);
        g_cfg.diagnostics    = ConfigBool("WXL_FOG_DIAGNOSTICS", false) ? 1 : 0;
        g_cfg.wakes          = ConfigBool("WXL_FOG_WAKES", true) ? 1 : 0;
        g_cfg.projectiles    = ConfigBool("WXL_FOG_PROJECTILES", true) ? 1 : 0;
        g_cfg.plumes         = ConfigBool("WXL_FOG_PLUMES", true) ? 1 : 0;
        g_cfg.immersion      = ConfigBool("WXL_FOG_IMMERSION_ENABLED", true) ? 1 : 0;
        g_cfg.wakeRange      = ConfigFloat("WXL_FOG_WAKE_RANGE", g_cfg.wakeRange, 5.0f, 200.0f);
        g_cfg.wakeTrail      = ConfigFloat("WXL_FOG_WAKE_TRAIL", g_cfg.wakeTrail, 0.0f, 10.0f);
        g_cfg.nearField      = ConfigBool("WXL_FOG_NEAR_FIELD", g_cfg.nearField != 0) ? 1 : 0;
        g_cfg.sliceBend      = ConfigFloat("WXL_FOG_SLICE_BEND", g_cfg.sliceBend, 0.0f, 0.9f);
        g_cfg.terrain        = ConfigBool("WXL_FOG_TERRAIN", true) ? 1 : 0;
        g_cfg.terrainBudget  = int(ConfigFloat("WXL_FOG_TERRAIN_BUDGET", float(g_cfg.terrainBudget), 1.0f, 512.0f));
        g_cfg.dither         = ConfigBool("WXL_FOG_DITHER", true) ? 1 : 0;
        {
            char phase[32];
            if (ConfigRaw("WXL_FOG_PHASE", phase, sizeof phase))
                g_cfg.phaseModel = (phase[0] == 'c' || phase[0] == 'C' || phase[0] == '1') ? 1 : 0;
        }
        g_cfg.worldShadows   = ConfigBool("WXL_FOG_WORLD_SHADOWS", true) ? 1 : 0;
        g_cfg.shadowSteps    = int(ConfigFloat("WXL_FOG_SHADOW_STEPS", float(g_cfg.shadowSteps), 4.0f, 12.0f));
        g_cfg.skyOcclusion   = ConfigBool("WXL_FOG_SKY_OCCLUSION_ENABLED", true) ? 1 : 0;
        g_cfg.nativeFog      = ConfigBool("WXL_FOG_NATIVE_FOG", true) ? 1 : 0;
        g_cfg.nativeFogStart = ConfigFloat("WXL_FOG_NATIVE_FOG_START", g_cfg.nativeFogStart, 0.0f, 0.99f);
        g_cfg.debugLantern   = ConfigBool("WXL_FOG_DEBUG_LANTERN", false) ? 1 : 0;

        g_haveBase[int(fog::Slot::Outdoor)] = fog::LoadProfile("WXL_FOG_", fog::Builtin(fog::Slot::Outdoor));
        g_haveBase[int(fog::Slot::Indoor)]  = fog::LoadProfile("WXL_FOG_INDOOR_", fog::Builtin(fog::Slot::Indoor));

        fog::inputs::Publish();
        fog::noise::StartBake();
        wxl::forever::bluenoise::StartBake();

        static FogModule module;
        wxl::forever::passes::Add(200, "fog", &WantsPass, &DrawPass);
        wxl::forever::lights::SetInteriorGate(g_cfg.enabled && g_cfg.indoorDetect);

        const fog::FogProfile& o = fog::Builtin(fog::Slot::Outdoor);
        const fog::FogProfile& n = fog::Builtin(fog::Slot::Indoor);
        WLOG_INFO("fog: installed (enabled=%d indoorDetect=%d outdoor density=%.4f colour mode=%d, indoor density=%.4f colour mode=%d)",
                  g_cfg.enabled, g_cfg.indoorDetect, o.density, o.colorMode, n.density, n.colorMode);
    }
}
