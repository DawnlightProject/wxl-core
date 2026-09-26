// wxl-graphics-fog: the overlay panel "Graphics Fog" -- overview and GPU timings, quality, the outdoor
// profile by theme, the atmosphere, the rivers, the indoor profile, the effects, the threat API's test buttons and the
// debug views. Every control explains itself with a "(?)".
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

#include "../Fog.hpp"
#include "../api/Threat.hpp"
#include "../core/Extension.hpp"
#include "../core/Settings.hpp"
#include "../gpu/Gpu.hpp"
#include "../gpu/Renderer.hpp"
#include "../sim/Inputs.hpp"
#include "../sim/Primitives.hpp"
#include "../terrain/Block.hpp"

#include "wxl/gfx/Ui.hpp"
#include "game/Camera.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    namespace fog = wxl::gfx::fog;
    namespace ui  = wxl::gfx::ui;

    /// A dimmed hint line (a plain line on a core too old for the dimmed style).
    void Hint(const char* text)
    {
        if (ui::HasTooltips()) fog::g_api->UiTextDisabled(text);
        else ui::Text(text);
    }

    void OutdoorKnobs(fog::ProfileTab tab)
    {
        using namespace wxl::gfx::fog;
        fog::OutdoorProfile& p = fog::Outdoor();
#define WXL_FOG_KNOB(f, key, def, lo, hi, label, knobTab, help) \
        if (knobTab == tab) ui::Slider(label, &p.f, lo, hi, help);
        WXL_FOG_OUTDOOR_FLOATS(WXL_FOG_KNOB)
#undef WXL_FOG_KNOB
    }

    void IndoorKnobs()
    {
        using namespace wxl::gfx::fog;
        fog::IndoorProfile& p = fog::Indoor();
#define WXL_FOG_KNOB(f, key, def, lo, hi, label, knobTab, help) ui::Slider(label, &p.f, lo, hi, help);
        WXL_FOG_INDOOR_FLOATS(WXL_FOG_KNOB)
#undef WXL_FOG_KNOB
    }

    void ServiceLines()
    {
        const WXL_GraphicsExtendApi* gfx = fog::Gfx();
        const WXL_GfxVulkanApi* vk = fog::Vk();
        if (!gfx) ui::Text("wxl-graphics-extend is not loaded: the fog has nothing to draw with.");
        else if (!vk || !vk->Available())
        {
            ui::Text("DXVK not found on this device: the fog stays inert.");
            ui::Text("Run the client on DXVK (WXL_D3D9_BACKEND=dxvk on Windows, or DXVK under Wine).");
        }
        else ui::Text(vk->Status());
        if (!fog::Lights()) ui::Text("wxl-graphics-lights is not loaded: no lamps light the fog and every place is outdoors.");
    }

    void OverviewTab()
    {
        fog::Settings& s = fog::Config();
        ServiceLines();
        ui::Check("Enabled", &s.enabled, "Turns the fog on or off.");
        ui::Slider("Overall density", &s.intensity, 0.0f, 3.0f,
                   "Scales the whole fog at once. The threat API multiplies this by its own intensity and pulses.");
        if (ui::Button("Start the simulation over", "Resets the fog to its resting state and lets the rivers settle again, as after a loading screen."))
            fog::gpu::Restart();
        ui::Separator();
        ui::Text(fog::gpu::Status());
        ui::Text(fog::gpu::SimStatus());
        ui::Text(fog::terrain::Status());
        ui::Text(fog::threat::Status());
        const fog::State& st = fog::GetState();
        ui::Textf("camera %s, its air %.0f%% indoor | inputs: %d bodies (+%d phantoms), %d missiles (%d tunnels), %d blasts, %d plumes, %d fires | primitives %d",
                  st.cameraIndoor ? "indoors" : "outdoors", st.indoorEased * 100.0f, fog::inputs::Bodies(), fog::inputs::Phantoms(),
                  fog::inputs::Missiles(), fog::inputs::Segments(), fog::inputs::Shocks(), fog::inputs::Plumes(), fog::inputs::Fires(),
                  fog::prims::Count());
        if (const WXL_GraphicsLightsApi* lights = fog::Lights()) ui::Text(lights->Status());
        if (const WXL_GraphicsExtendApi* gfx = fog::Gfx()) ui::Text(gfx->AssetStatus());
        ui::Separator();
        ui::Text("GPU time per pass (ms, smoothed):");
        const float total = fog::gpu::SpanMs(-1);
        for (int i = 0; i < fog::gpu::kSpanCount; ++i)
        {
            const float ms = fog::gpu::SpanMs(i);
            ui::Textf("  %-9s %s", fog::gpu::kSpanNames[i], ms < 0.0f ? "not measured" : "");
            if (ms >= 0.0f)
            {
                fog::g_api->UiSameLine();
                ui::Textf("%.3f", ms);
            }
        }
        const float applyMs = fog::gpu::SpanMs(fog::gpu::kSpanApply);
        ui::Textf("  total     %.3f ms", std::max(total, 0.0f) + std::max(applyMs, 0.0f));
        if (!fog::gpu::TimersSupported()) Hint("Vulkan timestamps are not available on this device.");
    }

    void QualityTab()
    {
        fog::Settings& s = fog::Config();
        static const char* const kQualities[] = { "Custom", "Low", "Medium", "High", "Ultra" };
        if (ui::Combo("Quality", &s.quality, kQualities, 5,
                      "Sets the steps, the lamp grid, the simulation rate and the detail distance together. Low is fastest, Ultra is richest. Custom keeps your own values."))
            fog::ApplyQuality(s.quality);
        ui::Separator();
        ui::Slider("Near steps", &s.nearSteps, 4, 128, "Samples along each ray over the near range, at half resolution. More gives finer wisps near you and costs the most.");
        ui::Slider("Far steps", &s.farSteps, 4, 128, "Samples along each ray beyond the near range, at quarter resolution, out to the horizon.");
        ui::Slider("Near range (yd)", &s.nearRange, 8.0f, 200.0f, "Where the detailed near march hands over to the far one.");
        ui::Slider("Far distance (yd)", &s.farDistance, 300.0f, 8000.0f, "How far rays towards the sky march before the haze is added in closed form.");
        ui::Slider("Start distance (yd)", &s.startDistance, 0.0f, 2.0f, "How far from the lens the fog starts. Keep it small so wisps pass right in front of the camera.");
        ui::Slider("Detail distance (yd)", &s.detailDistance, 0.0f, 200.0f, "The fine fraying of the fog's edges fades out by this distance.");
        ui::Separator();
        ui::Slider("History kept", &s.temporal, 0.0f, 0.98f, "Share of the past frames kept each frame. Higher is smoother, lower reacts faster.");
        ui::Slider("History clip", &s.clipGamma, 0.5f, 4.0f, "How far the kept history may stray from this frame's neighbourhood, in standard deviations. Lower removes ghosts, higher removes flicker.");
        ui::Slider("History reject", &s.rejectDepth, 0.01f, 1.0f, "How much the scene's distance may change before the history is dropped, as a share. Lower drops it at more edges.");
        ui::Check("Dither", &s.dither, "Adds a tiny noise that hides banding in smooth fog.");
        ui::Separator();
        ui::Slider("Simulation rate (Hz)", &s.clipRate, 5.0f, 120.0f, "Steps per second of the finest fog level. Each coarser level steps half as often.");
        ui::Slider("Coarse from fine", &s.restrict, 0.0f, 1.0f, "How much a coarse level takes from the finer one where they overlap, so holes and wakes are seen from afar.");
        ui::Slider("Lamp grid across", &s.gridX, 32, 320, "Cells across the camera's lamp and indoor grid.");
        ui::Slider("Lamp grid down", &s.gridY, 18, 180, "Cells down the lamp grid.");
        ui::Slider("Lamp grid deep", &s.gridZ, 16, 128, "Slices of the lamp grid in depth, from half a yard to its far distance.");
    }

    void FogTab()
    {
        ui::Text("Outdoor fog: where it lies and how thick it is (WXL_FOG_* keys).");
        OutdoorKnobs(fog::kTabFog);
    }

    void AirTab()
    {
        ui::Text("The atmosphere: a smooth haze in the whole air column, above and beyond the lying fog (WXL_FOG_AIR_* keys).");
        Hint("It shares one transmittance with the lying fog, lights with the sun, moon, sky and lamps, and gives way to the indoor air.");
        OutdoorKnobs(fog::kTabAir);
    }

    void CascadeTab()
    {
        fog::Settings& s = fog::Config();
        ui::Text("Cascades: fog banks lying behind crests spill over them and pour down the far side, thinning as they fall.");
        Hint("The spill points come from the baked cascade maps (5.tools/forever-bake, bake cascade); gameplay adds its own through the API.");
        ui::Check("Cascades", &s.cascades, "Banks behind crests pour over them, from the baked maps and the API. Off, only the rivers move the lying fog.");
        OutdoorKnobs(fog::kTabCascade);
    }

    void ShapeTab()
    {
        ui::Text("The fog's substance: billows, their cascade to finer ones, and the wisps that fray the edges.");
        OutdoorKnobs(fog::kTabShape);
    }

    void MotionTab()
    {
        ui::Text("How the fog moves: renewal, wind, turbulence and the rivers' pull.");
        OutdoorKnobs(fog::kTabMotion);
    }

    void RiversTab()
    {
        fog::Transport& t = fog::Config().transport;
        ui::Text(fog::terrain::Status());
        ui::Check("Fog rivers", &t.enabled, "Cold fog that forms on the ground, runs down slopes, pools in hollows, spills over cols and streams along valleys. It needs the baked terrain tiles.");
        ui::Slider("Drainage", &t.drainage, 0.0f, 40.0f, "How fast the fog runs downhill once it flows, in yards per second on a 45 degree slope.");
        ui::Slider("Friction", &t.friction, 0.05f, 10.0f, "How fast the flow loses its momentum, per second. Low values let rivers overshoot and slosh.");
        ui::Slider("Wind push", &t.windPush, 0.0f, 0.5f, "How much the wind tilts the fog's surface and drives it downwind. Relief still holds it back.");
        ui::Slider("Top speed", &t.maxSpeed, 0.5f, 60.0f, "The fastest the fog may run, in yards per second.");
        ui::Slider("Time scale", &t.timeScale, 0.0f, 20.0f, "Simulated seconds per second. Above 1 the rivers form and travel faster.");
        ui::Separator();
        ui::Slider("Formation (yd/min)", &t.formation, 0.0f, 60.0f, "Yards of fog formed each minute on open ground at night.");
        ui::Slider("Formation by day", &t.dayShare, 0.0f, 1.0f, "Share of the formation that goes on while the sun is up.");
        ui::Slider("Formation in hollows", &t.hollowBoost, 0.0f, 20.0f, "Extra fog formed in hollows and valley floors, times the formation.");
        ui::Slider("Formation over water", &t.waterBoost, 0.0f, 20.0f, "Extra fog formed over lakes and rivers, times the formation. It needs the baked water maps.");
        ui::Slider("Fading (share/min)", &t.decay, 0.0f, 60.0f, "Share of the fog that mixes away each minute.");
        ui::Slider("Sun burn-off (share/min)", &t.sunDecay, 0.0f, 60.0f, "How fast full sun burns the fog off where it reaches the ground. Shaded valleys keep theirs.");
        ui::Slider("Wind scour (share/min)", &t.windScour, 0.0f, 20.0f, "How fast the wind strips exposed ridges, for each yard per second of wind.");
        ui::Slider("Pool depth (yd)", &t.poolDepth, 1.0f, 200.0f, "How deep a basin may fill before its top mixes away quickly.");
        ui::Separator();
        ui::Slider("Steps per second", &t.rate, 2.0f, 60.0f, "How often the rivers are updated. The rate holds whatever the frame rate.");
        ui::Slider("Warm-up (s)", &t.warmup, 0.0f, 1200.0f, "Simulated seconds run quickly when new terrain arrives or after a teleport, so the rivers are there at once.");
        ui::Slider("Initial fill", &t.initialFill, 0.0f, 1.0f, "Share of each hollow's depth filled with fog when its terrain arrives.");
        if (ui::Button("Settle again", "Starts the simulation over: the rivers settle from their initial state."))
            fog::gpu::Restart();
    }

    void LightTab()
    {
        fog::Settings& s = fog::Config();
        fog::OutdoorProfile& o = fog::Outdoor();
        static const char* const kModes[] = { "Zone colour", "My colour", "Blend" };
        ui::Combo("Colour", &o.colorMode, kModes, 3, "Zone colour follows the zone's own fog colour through the day. My colour uses yours; Blend mixes them.");
        ui::Color("Fog colour", o.color, "The fog's own colour: how it tints the light it scatters.");
        ui::Separator();
        OutdoorKnobs(fog::kTabLight);
        ui::Separator();
        ui::Slider("Terrain shadow", &s.terrainShadow, 0.0f, 1.0f, "How much hills and ridges shade the fog from the sun and moon. Shafts fall between them.");
        ui::Slider("Terrain penumbra (deg)", &s.penumbra, 0.2f, 20.0f, "Degrees of the body's elevation a ridge's shadow edge ramps over. Wider is softer.");
        ui::Check("World shadows", &s.worldShadows, "Trees and buildings shade the fog near you through the engine's sun shadow maps: shafts between trunks and walls.");
        ui::Slider("World shadow strength", &s.worldShadow, 0.0f, 1.0f, "How dark the world's shadows are in the fog. The engine renders them along a steeper light than the sun, so keep it moderate at dusk.");
        ui::Slider("Scattering octaves", &s.msOctaves, 0, 3, "Extra bounces of the multiple-scattering estimate. More lets thick fog glow through more.");
    }

    void IndoorTab()
    {
        fog::Settings& s = fog::Config();
        fog::IndoorProfile& n = fog::Indoor();
        static const char* const kModes[] = { "Zone colour", "My colour", "Blend" };
        ui::Check("Indoor detection", &s.indoorDetect, "Inside buildings the air is its own medium: dusty, still, lit by lamps. Off, the outdoor fog is everywhere.");
        ui::Slider("Transition (s)", &s.indoorTransition, 0.0f, 30.0f, "Seconds the air around you takes to change when you pass a doorway. The view beyond keeps its own air at once.");
        ui::Slider("Transition reach (yd)", &s.indoorLagRadius, 1.0f, 40.0f, "How far around you the air follows your own passage through a doorway.");
        ui::Slider("Room fade (s)", &s.classifyTime, 0.05f, 5.0f, "Seconds a room that streams in or out takes to change the air it holds.");
        ui::Separator();
        ui::Text("Indoor air (WXL_FOG_INDOOR_* keys):");
        ui::Combo("Dust colour mode", &n.colorMode, kModes, 3, "Zone colour, your colour, or a blend of the two.");
        ui::Color("Dust colour", n.color, "The indoor air's own colour.");
        IndoorKnobs();
    }

    void EffectsTab()
    {
        fog::Settings& s = fog::Config();
        ui::Check("Bodies push the fog", &s.wakes, "Players and creatures clear the fog around them, push it aside and drag it as they move: their wakes are real holes the fog carries.");
        ui::Slider("Body range (yd)", &s.wakeRange, 5.0f, 150.0f, "How far from you bodies are tracked. The 16 nearest are used.");
        ui::Check("Projectiles", &s.projectiles, "Spells, arrows and bullets cut a tunnel that holds a moment, then fills back with thick rolling fog.");
        ui::Check("Smoke plumes", &s.plumes, "Chimneys, campfires and spell smoke put smoke into the fog. It rises, bends with the wind and spreads.");
        ui::Check("Heat", &s.heat, "Fires thin the fog around them and lift it.");
        ui::Check("Immersion", &s.immersion, "Standing inside thick fog closes it in around the screen and softens the world.");
        ui::Check("Lantern at the player", &s.debugLantern, "A warm light at your character, to see lamps in the fog.");
        ui::Check("Pretend torch in hand", &s.debugTorch, "A torch flame in your character's hand, with the pocket a carried torch clears in the fog, to try it without one.");
        ui::Check("Push the client's fog away", &s.nativeFog, "Moves the game's own distance fog to the far edge while this fog draws, so the two never stack.");
        ui::Slider("Client fog start", &s.nativeFogStart, 0.0f, 0.99f, "Where the game's own fog then starts, as a share of the view distance.");
        ui::Separator();
        OutdoorKnobs(fog::kTabEffects);
    }

    // --- the threat API's test buttons -----------------------------------------------------------------

    uint32_t g_testSource = 0, g_testFront = 0, g_testPulse = 0, g_testFlow = 0, g_testCascade = 0;
    float g_testIntensity = 1.0f;

    void Forward(float dir[2])
    {
        const float* v = wxl::game::camera::GetView();
        // The view's forward in world space: the third column of the rotation (row-vector convention).
        float f[2] = { v[2], v[6] };
        const float len = std::sqrt(f[0] * f[0] + f[1] * f[1]);
        dir[0] = len > 1e-4f ? f[0] / len : 1.0f;
        dir[1] = len > 1e-4f ? f[1] / len : 0.0f;
    }

    void ThreatTab()
    {
        const WXL_GraphicsFogApi* api = fog::threat::Table();
        const fog::State& st = fog::GetState();
        ui::Text(fog::threat::Status());
        ui::Textf("camera: %.3f extinction per yard, visibility %.0f yd, %.0f%% indoor", api->CameraDensity(), std::min(api->Visibility(), 99999.0f),
                  api->CameraIndoor() * 100.0f);
        ui::Separator();
        if (!st.havePlayer) Hint("Log in with a character to use the test buttons.");
        const float* me = st.player;
        if (ui::Button("A phantom horde crosses", "Sixty invisible walkers cross the fog 25 yards ahead of you, from left to right, to see a horde plough through it."))
        {
            float fwd[2];
            Forward(fwd);
            const float right[2] = { fwd[1], -fwd[0] };
            const float from[3] = { me[0] + fwd[0] * 25.0f - right[0] * 40.0f, me[1] + fwd[1] * 25.0f - right[1] * 40.0f, me[2] };
            const float to[3] = { me[0] + fwd[0] * 25.0f + right[0] * 40.0f, me[1] + fwd[1] * 25.0f + right[1] * 40.0f, me[2] };
            fog::inputs::SpawnPhantoms(from, to, 60, 3.5f, 16.0f);
        }
        if (ui::Button("One phantom runs past", "A single invisible runner crosses 8 yards ahead of you at a sprint, to see one wake up close."))
        {
            float fwd[2];
            Forward(fwd);
            const float right[2] = { fwd[1], -fwd[0] };
            const float from[3] = { me[0] + fwd[0] * 8.0f - right[0] * 25.0f, me[1] + fwd[1] * 8.0f - right[1] * 25.0f, me[2] };
            const float to[3] = { me[0] + fwd[0] * 8.0f + right[0] * 25.0f, me[1] + fwd[1] * 8.0f + right[1] * 25.0f, me[2] };
            fog::inputs::SpawnPhantoms(from, to, 1, 7.0f, 0.0f);
        }
        if (ui::Button("Clear the phantoms", "Removes the invisible walkers.")) fog::inputs::ClearPhantoms();
        if (ui::Button("A cascade pours towards me", "A bank 90 yards ahead and 20 yards up pours towards you for a minute, 30 yards wide."))
        {
            float fwd[2];
            Forward(fwd);
            WXL_GfxFogCascade c{};
            c.structSize = sizeof c;
            c.mapId = -1;
            c.position[0] = me[0] + fwd[0] * 90.0f;
            c.position[1] = me[1] + fwd[1] * 90.0f;
            c.position[2] = me[2] + 20.0f;
            c.direction[0] = -fwd[0];
            c.direction[1] = -fwd[1];
            c.width = 30.0f;
            c.depth = 14.0f;
            c.duration = 60.0f;
            c.fadeIn = 3.0f;
            c.fadeOut = 10.0f;
            if (g_testCascade) api->RemoveCascade(g_testCascade, 3.0f);
            g_testCascade = api->AddCascade(&c);
        }
        if (ui::Button("A fog wall advances on me", "A front 120 yards ahead, 30 yards wide and 12 high, rolls towards you at 4 yards per second."))
        {
            float fwd[2];
            Forward(fwd);
            WXL_GfxFogFront f{};
            f.structSize = sizeof f;
            f.kind = WXL_GFX_FOG_FRONT_LINE;
            f.origin[0] = me[0] + fwd[0] * 120.0f;
            f.origin[1] = me[1] + fwd[1] * 120.0f;
            f.origin[2] = me[2];
            f.direction[0] = -fwd[0];
            f.direction[1] = -fwd[1];
            f.speed = 4.0f;
            f.endRadius = 160.0f;
            f.halfWidth = 30.0f;
            f.height = 12.0f;
            f.depth = 40.0f;
            f.density = 0.12f;
            f.billow = 0.8f;
            f.push = 1.5f;
            f.duration = 60.0f;
            f.fadeIn = 2.0f;
            f.fadeOut = 8.0f;
            if (g_testFront) api->RemoveFront(g_testFront, 3.0f);
            g_testFront = api->AddFront(&f);
        }
        if (ui::Button("The fog closes in on me", "A ring 80 yards out closes on you at 3 yards per second and stops 6 yards away."))
        {
            WXL_GfxFogFront f{};
            f.structSize = sizeof f;
            f.kind = WXL_GFX_FOG_FRONT_CLOSING;
            f.origin[0] = me[0];
            f.origin[1] = me[1];
            f.origin[2] = me[2];
            f.speed = 3.0f;
            f.startRadius = 80.0f;
            f.endRadius = 6.0f;
            f.height = 10.0f;
            f.depth = 30.0f;
            f.density = 0.14f;
            f.billow = 0.9f;
            f.push = 1.0f;
            f.duration = 60.0f;
            f.fadeIn = 3.0f;
            f.fadeOut = 10.0f;
            if (g_testFront) api->RemoveFront(g_testFront, 3.0f);
            g_testFront = api->AddFront(&f);
        }
        if (ui::Button("A bank here", "Adds a lumpy bank of fog 10 yards around you for 30 seconds."))
        {
            WXL_GfxFogSource s{};
            s.structSize = sizeof s;
            s.shape = WXL_GFX_FOG_SHAPE_SPHERE;
            s.center[0] = me[0];
            s.center[1] = me[1];
            s.center[2] = me[2];
            s.extent[0] = 10.0f;
            s.op = WXL_GFX_FOG_OP_HOLD;
            s.strength = 0.12f;
            s.softness = 0.5f;
            s.billow = 0.8f;
            s.duration = 30.0f;
            s.fadeIn = 2.0f;
            s.fadeOut = 6.0f;
            if (g_testSource) api->RemoveSource(g_testSource, 2.0f);
            g_testSource = api->AddSource(&s);
        }
        if (ui::Button("Clear around me", "Carves the fog away 8 yards around you for 20 seconds, pushing it outwards."))
        {
            WXL_GfxFogSource s{};
            s.structSize = sizeof s;
            s.shape = WXL_GFX_FOG_SHAPE_CYLINDER;
            s.center[0] = me[0];
            s.center[1] = me[1];
            s.center[2] = me[2] - 2.0f;
            s.extent[0] = 8.0f;
            s.extent[2] = 12.0f;
            s.op = WXL_GFX_FOG_OP_CARVE;
            s.strength = 4.0f;
            s.softness = 0.5f;
            s.billow = 0.5f;
            s.radialPush = 1.5f;
            s.duration = 20.0f;
            s.fadeIn = 1.0f;
            s.fadeOut = 4.0f;
            if (g_testSource) api->RemoveSource(g_testSource, 2.0f);
            g_testSource = api->AddSource(&s);
        }
        if (ui::Button("A gust along my view", "Blows the fog along your view at 6 yards per second, 40 yards around you, for 10 seconds."))
        {
            float fwd[2];
            Forward(fwd);
            WXL_GfxFogFlow f{};
            f.structSize = sizeof f;
            f.shape = WXL_GFX_FOG_SHAPE_SPHERE;
            f.center[0] = me[0];
            f.center[1] = me[1];
            f.center[2] = me[2];
            f.extent[0] = 40.0f;
            f.velocity[0] = fwd[0] * 6.0f;
            f.velocity[1] = fwd[1] * 6.0f;
            f.softness = 0.6f;
            f.duration = 10.0f;
            f.fadeIn = 1.0f;
            f.fadeOut = 3.0f;
            if (g_testFlow) api->RemoveFlow(g_testFlow, 1.0f);
            g_testFlow = api->AddFlow(&f);
        }
        if (ui::Button("Breathe", "The whole fog swells and ebbs by half, every 6 seconds, for 30 seconds."))
        {
            WXL_GfxFogPulse p{};
            p.structSize = sizeof p;
            p.kind = WXL_GFX_FOG_PULSE_BREATH;
            p.amplitude = 0.5f;
            p.period = 6.0f;
            p.duration = 30.0f;
            p.attack = 2.0f;
            p.release = 3.0f;
            if (g_testPulse) api->StopPulse(g_testPulse, 1.0f);
            g_testPulse = api->Pulse(&p);
        }
        if (ui::Button("Surge", "The whole fog doubles over 3 seconds, then falls back over 10."))
        {
            WXL_GfxFogPulse p{};
            p.structSize = sizeof p;
            p.kind = WXL_GFX_FOG_PULSE_SURGE;
            p.amplitude = 1.0f;
            p.attack = 3.0f;
            p.release = 10.0f;
            g_testPulse = api->Pulse(&p);
        }
        ui::Slider("Test intensity", &g_testIntensity, 0.0f, 3.0f, "The API's global intensity. Apply sets it over 4 seconds.");
        fog::g_api->UiSameLine();
        if (ui::Button("Apply", "Fades the fog's overall density to the test intensity over 4 seconds.")) api->SetIntensity(g_testIntensity, 4.0f);
        if (ui::Button("Clear everything", "Removes every source, front, flow and pulse, fading over 3 seconds."))
        {
            api->ClearAll(3.0f);
            g_testSource = g_testFront = g_testPulse = g_testFlow = 0;
        }
    }

    void DebugTab()
    {
        fog::Settings& s = fog::Config();
        static const char* const kViews[] = {
            "Fog", "Fog light only (over black)", "Transmittance", "Slice from above (map)", "Slice along the view (map)",
            "Rivers: depth (map)", "Rivers: flow (map)", "Rivers: formation and fading (map)", "Indoor (red) and outdoor (blue)",
            "Lamp light in the fog", "Sun and sky light in the fog", "Terrain: floor, hollows, sky share (map)",
            "Distance and history rejection", "Empty-space skipping", "Wakes: fog cleared (red), thickened (blue), air flow (map)",
            "Cascades: spill points (red), banks (blue), cascade fog (green) (map)",
        };
        ui::Combo("View", &s.view, kViews, FOG_VIEW_COUNT,
                  "Shows one ingredient of the fog. Map views draw in the lower right corner, north up, you ringed; the others cover the screen.");
        ui::Slider("Level", &s.debugLevel, 0, FOG_LEVELS - 1, "The clipmap level the slice views show: 0 finest (half a yard) to 3 coarsest (32 yards).");
        ui::Slider("Slice height (yd)", &s.debugHeight, -2.0f, 100.0f, "Height over the ground of the slice seen from above.");
        ui::Slider("Distance scale (yd)", &s.debugFar, 10.0f, 3000.0f, "The distance shown as full brightness in the distance view.");
        ui::Separator();
        ui::Text("Isolate: each switch removes one ingredient, to find which one causes an artefact.");
        struct Iso { uint32_t bit; const char* label; const char* help; };
        static const Iso kIso[] = {
            { FOG_ISO_NO_DETAIL,     "No fraying",            "Removes the fine wisps that fray the edges." },
            { FOG_ISO_NO_SHADOW,     "No self-shadow",        "The fog no longer shades itself from the sun and moon." },
            { FOG_ISO_NO_TERRAIN_SH, "No terrain shadow",     "Hills no longer shade the fog." },
            { FOG_ISO_NO_WORLD_SH,   "No world shadows",      "Trees and buildings no longer shade the fog." },
            { FOG_ISO_NO_MS,         "No multiple scattering","Only the first bounce of the sun and moon." },
            { FOG_ISO_NO_AMBIENT,    "No sky light",          "Removes the sky's light from the fog." },
            { FOG_ISO_NO_LAMPS,      "No lamps",              "Removes lamps and torches from the fog." },
            { FOG_ISO_NO_INDOOR,     "No indoor air",         "Treats everything as outdoors." },
            { FOG_ISO_NO_HAZE,       "No haze",               "Removes the haze towards the horizon." },
            { FOG_ISO_NO_FAR,        "No far march",          "Draws only the near fog, to the near range." },
            { FOG_ISO_NO_NEAR,       "No near march",         "Draws only the far fog, past the near range." },
            { FOG_ISO_NO_HISTORY,    "No history",            "Shows each frame on its own, without smoothing over time." },
            { FOG_ISO_NO_CLIP,       "No history clip",       "Keeps the past frames without limiting them to this one's neighbourhood." },
            { FOG_ISO_NO_SKIP,       "No empty-space skipping","Samples every step fully, even where the fog is empty." },
            { FOG_ISO_FLAT,          "Flat fog",              "Only an even ground mist: no layer, no banks, no billows." },
            { FOG_ISO_NO_LAYER,      "No rivers",             "Removes the terrain layer from the fog." },
            { FOG_ISO_NO_AIR,        "No atmosphere",         "Removes the atmosphere: only the lying fog and the haze remain." },
            { FOG_ISO_ONLY_AIR,      "Only the atmosphere",   "Removes the lying fog: only the atmosphere and the haze remain." },
        };
        for (const Iso& iso : kIso)
        {
            int on = (s.isolate & iso.bit) ? 1 : 0;
            if (ui::Check(iso.label, &on, iso.help)) s.isolate = on ? (s.isolate | iso.bit) : (s.isolate & ~iso.bit);
        }
    }
}

namespace wxl::gfx::fog
{
    void Panel()
    {
        ui::Scope scope("gfxfog");
        if (!ui::BeginTabs("wxl-graphics-fog")) return;
        if (ui::BeginTab("Overview")) { ui::Scope s("gfxfog.overview"); OverviewTab(); ui::EndTab(); }
        if (ui::BeginTab("Quality"))  { ui::Scope s("gfxfog.quality");  QualityTab();  ui::EndTab(); }
        if (ui::BeginTab("Fog"))      { ui::Scope s("gfxfog.fog");      FogTab();      ui::EndTab(); }
        if (ui::BeginTab("Air"))      { ui::Scope s("gfxfog.air");      AirTab();      ui::EndTab(); }
        if (ui::BeginTab("Shape"))    { ui::Scope s("gfxfog.shape");    ShapeTab();    ui::EndTab(); }
        if (ui::BeginTab("Motion"))   { ui::Scope s("gfxfog.motion");   MotionTab();   ui::EndTab(); }
        if (ui::BeginTab("Rivers"))   { ui::Scope s("gfxfog.rivers");   RiversTab();   ui::EndTab(); }
        if (ui::BeginTab("Cascades")) { ui::Scope s("gfxfog.cascades"); CascadeTab();  ui::EndTab(); }
        if (ui::BeginTab("Light"))    { ui::Scope s("gfxfog.light");    LightTab();    ui::EndTab(); }
        if (ui::BeginTab("Indoor"))   { ui::Scope s("gfxfog.indoor");   IndoorTab();   ui::EndTab(); }
        if (ui::BeginTab("Effects"))  { ui::Scope s("gfxfog.effects");  EffectsTab();  ui::EndTab(); }
        if (ui::BeginTab("Threat"))   { ui::Scope s("gfxfog.threat");   ThreatTab();   ui::EndTab(); }
        if (ui::BeginTab("Debug"))    { ui::Scope s("gfxfog.debug");    DebugTab();    ui::EndTab(); }
        ui::EndTabs();
    }
}
