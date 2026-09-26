// wxl-graphics-fog: the orchestration. The game tick reads the player, the bodies, the ground under
// the camera and feeds the light service; the compute pass ("fog", WXL_GFX_ORDER_ATMOSPHERE) gathers
// the frame's inputs and records the fog on DXVK's device; the D3D9 pass ("fog.apply", same order)
// composites it.
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

#include "Fog.hpp"
#include "api/Threat.hpp"
#include "core/Extension.hpp"
#include "core/Settings.hpp"
#include "gpu/Renderer.hpp"
#include "sim/Inputs.hpp"
#include "sim/Primitives.hpp"
#include "terrain/Block.hpp"

#include "wxl/EventScript.hpp"
#include "game/Camera.hpp"
#include "game/Pick.hpp"
#include "game/Sky.hpp"
#include "game/World.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <string>

namespace
{
    namespace ev    = wxl::events;
    namespace fog   = wxl::gfx::fog;
    namespace cam   = wxl::game::camera;
    namespace world = wxl::game::world;
    namespace sky   = wxl::game::sky;

    fog::State g_state;
    double     g_lastDrawn = -1e9;
    bool       g_ownOverride = false;
    const int  kGivenKey = 0;   // this extension's identity for wxl-graphics-lights SetGiven

    double Now()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    /// The fog's clock: real time in steps capped at a fifteenth of a second, so a hitch never makes
    /// the fog leap. Advanced once per compute frame.
    struct Clock
    {
        double last = -1.0, t = 0.0;
        float  dt = 0.0f;
        void Advance()
        {
            const double now = Now();
            dt = last >= 0.0 ? float(std::clamp(now - last, 0.0, 1.0 / 15.0)) : 0.0f;
            last = now;
            t += dt;
        }
    } g_clock;

    // --- the sun and the moon ------------------------------------------------------------------------

    struct Celestial
    {
        bool  valid = false;
        float toSun[3] = { 0.0f, 0.0f, 1.0f }, toMoon[3] = { 0.0f, 0.0f, 1.0f };
        float rgb[3] = {};
        float sunWeight = 0.0f, moonWeight = 0.0f;
    } g_celestial;

    float Smoothstep(float a, float b, float x)
    {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    void Normalize(float v[3])
    {
        const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len > 1e-6f) for (int i = 0; i < 3; ++i) v[i] /= len;
    }

    float Lin(float c) { return std::pow(std::max(c, 0.0f), 2.2f); }

    /// The engine's light, its glare dimming undone (the fog should not dip when the sun is hidden
    /// from the camera), eased over a third of a second.
    void UpdateCelestial(float dt)
    {
        sky::CelestialLight light{};
        if (!sky::GetCelestialLight(light)) return;
        const float glare = std::clamp(light.glare, 0.0f, 1.0f);
        const float undo = 1.0f / std::max(1.0f - light.glareDimming * glare, 0.05f);
        const float sunW = Smoothstep(-0.05f, 0.12f, light.toSun[2]);
        const float moonW = Smoothstep(-0.05f, 0.12f, light.toMoon[2]) * (1.0f - sunW);
        Celestial& c = g_celestial;
        const float a = c.valid ? 1.0f - std::exp(-std::max(dt, 0.0f) / 0.3f) : 1.0f;
        for (int i = 0; i < 3; ++i)
        {
            c.toSun[i] += (light.toSun[i] - c.toSun[i]) * a;
            c.toMoon[i] += (light.toMoon[i] - c.toMoon[i]) * a;
            c.rgb[i] += (Lin(light.diffuse[i] * undo) - c.rgb[i]) * a;
        }
        Normalize(c.toSun);
        Normalize(c.toMoon);
        c.sunWeight += (sunW - c.sunWeight) * a;
        c.moonWeight += (moonW - c.moonWeight) * a;
        if (!c.valid)
            FOG_LOG_INFO("sun (%.2f %.2f %.2f) weight %.2f, moon (%.2f %.2f %.2f) weight %.2f, light (%.2f %.2f %.2f)",
                         light.toSun[0], light.toSun[1], light.toSun[2], sunW, light.toMoon[0], light.toMoon[1], light.toMoon[2], moonW,
                         light.diffuse[0], light.diffuse[1], light.diffuse[2]);
        c.valid = true;
    }

    void Argb(uint32_t argb, float out[3])
    {
        out[0] = Lin(float((argb >> 16) & 0xFF) / 255.0f);
        out[1] = Lin(float((argb >> 8) & 0xFF) / 255.0f);
        out[2] = Lin(float(argb & 0xFF) / 255.0f);
    }

    /// The camera's indoor state eased over the transition time (smootherstep); a flip halfway
    /// starts from the value on screen.
    float EaseIndoor(bool indoor, float dt)
    {
        static bool started = false, target = false;
        static float from = 0.0f, t = 1.0f;
        const float seconds = fog::Config().indoorTransition;
        if (!started || seconds <= 0.0f)
        {
            started = true;
            target = indoor;
            t = 1.0f;
            return g_state.indoorEased = indoor ? 1.0f : 0.0f;
        }
        if (indoor != target)
        {
            from = g_state.indoorEased;
            target = indoor;
            t = 0.0f;
        }
        t = std::min(t + dt / seconds, 1.0f);
        const float s = t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        return g_state.indoorEased = from + ((target ? 1.0f : 0.0f) - from) * s;
    }

    bool CanDraw()
    {
        const WXL_GfxVulkanApi* vk = fog::Vk();
        return vk && vk->Available() != 0;
    }

    bool DrewRecently() { return Now() - g_lastDrawn < 1.0; }

    void NoteNoDxvk()
    {
        static bool said = false;
        if (said) return;
        said = true;
        FOG_LOG_WARN("DXVK not found on this device: the fog stays inert. Run the client on DXVK "
                     "(WXL_D3D9_BACKEND=dxvk on Windows, or DXVK under Wine).");
    }

    /// The lights the fog hands the light service: the test lantern and those fed through the API.
    void GiveLights()
    {
        const WXL_GraphicsLightsApi* lights = fog::Lights();
        if (!lights) return;
        WXL_GfxLight given[WXL_GFX_FOG_MAX_LIGHTS + 1];
        int count = 0;
        if (fog::Config().enabled && fog::Config().debugLantern && g_state.havePlayer)
        {
            WXL_GfxLight& l = given[count++];
            l = WXL_GfxLight{};
            l.kind = WXL_GFX_LIGHT_KIND_GIVEN;
            l.position[0] = g_state.player[0];
            l.position[1] = g_state.player[1];
            l.position[2] = g_state.player[2] + 1.8f;
            l.radius = 14.0f;
            l.color[0] = 1.0f;
            l.color[1] = 0.62f;
            l.color[2] = 0.28f;
            l.intensity = 1.5f;
            l.cosCone = -2.0f;
            l.flicker = WXL_GFX_LIGHT_FLICKER_LANTERN;
        }
        if (fog::Config().enabled && fog::Config().debugTorch && g_state.havePlayer && count < WXL_GFX_FOG_MAX_LIGHTS + 1)
        {
            // The pretend torch's flame, in the right hand.
            WXL_GfxLight& l = given[count++];
            l = WXL_GfxLight{};
            l.kind = WXL_GFX_LIGHT_KIND_GIVEN;
            l.position[0] = g_state.player[0];
            l.position[1] = g_state.player[1] - 0.45f;
            l.position[2] = g_state.player[2] + 1.3f;
            l.radius = 12.0f;
            l.color[0] = 1.0f;
            l.color[1] = 0.55f;
            l.color[2] = 0.22f;
            l.intensity = 1.8f;
            l.cosCone = -2.0f;
            l.flicker = WXL_GFX_LIGHT_FLICKER_FIRE;
        }
        const WXL_GfxFogLight* fed = fog::threat::Lights();
        for (int i = 0; i < fog::threat::LightCount() && count < WXL_GFX_FOG_MAX_LIGHTS + 1; ++i)
        {
            const WXL_GfxFogLight& src = fed[i];
            WXL_GfxLight& l = given[count++];
            l = WXL_GfxLight{};
            for (int k = 0; k < 3; ++k)
            {
                l.position[k] = src.position[k];
                l.color[k] = src.color[k];
                l.direction[k] = src.direction[k];
            }
            l.radius = src.radius;
            l.intensity = src.intensity;
            l.cosCone = src.cosCone;
            l.innerRadius = src.innerRadius;
            l.interior = src.interior;
            l.kind = WXL_GFX_LIGHT_KIND_GIVEN;
        }
        lights->SetGiven(&kGivenKey, given, count);
    }

    /// The client's own distance fog, pushed to the far clip while this fog draws so the two never
    /// stack; handed back a second after this one last drew.
    void UpdateNativeFog()
    {
        const fog::Settings& s = fog::Config();
        const bool want = s.enabled && s.nativeFog && DrewRecently();
        if (want && (g_ownOverride || !sky::OverrideFogActive()))
        {
            sky::SetOverrideFog(std::clamp(s.nativeFogStart, 0.0f, 0.99f), 20000.0f, sky::FogColor(), sky::SkyDrawGate());
            if (!g_ownOverride) FOG_LOG_INFO("client distance fog pushed to the far clip");
            g_ownOverride = true;
        }
        else if (!want && g_ownOverride)
        {
            sky::ClearOverrideFog();
            g_ownOverride = false;
            FOG_LOG_INFO("client distance fog restored");
        }
    }

    // --- the passes (C ABI callbacks: nothing may throw out of them) --------------------------------

    uint32_t __cdecl WantsCompute(void*)
    {
        try
        {
            if (!fog::Config().enabled) return 0;
            if (!CanDraw())
            {
                NoteNoDxvk();
                return 0;
            }
            if (const WXL_GraphicsLightsApi* lights = fog::Lights()) lights->Want();
            if (!fog::gpu::Prepare(fog::Vk())) return 0;
            return WXL_GFX_NEED_DEPTH;
        }
        catch (...)
        {
            return 0;
        }
    }

    uint32_t __cdecl WantsApply(void*) { return fog::Config().enabled && CanDraw() ? WXL_GFX_NEED_DEPTH : 0; }

    void RecordFrame(const WXL_GfxVkFrame& vk)
    {
        const fog::Settings& s = fog::Config();
        const fog::OutdoorProfile& o = fog::Outdoor();
        const WXL_GfxFrame* frame = vk.frame;
        if (!frame) return;

        g_clock.Advance();
        g_state.clock = g_clock.t;
        fog::gpu::FrameInput in;
        in.frame = frame;
        for (int k = 0; k < 3; ++k) in.eye[k] = frame->view.eye[k];
        in.clock = g_clock.t;
        in.dt = g_clock.dt;
        {
            static float lastEye[3] = {};
            static bool haveEye = false;
            static std::string lastMap;
            const std::string mapNow = fog::terrain::LiveMapName();
            const float dx = in.eye[0] - lastEye[0], dy = in.eye[1] - lastEye[1], dz = in.eye[2] - lastEye[2];
            in.jumped = !haveEye || dx * dx + dy * dy + dz * dz > 80.0f * 80.0f || mapNow != lastMap;
            if (in.jumped && haveEye) FOG_LOG_INFO("camera jumped (%s): the fog starts over", mapNow != lastMap ? "new map" : "teleport");
            std::copy(in.eye, in.eye + 3, lastEye);
            lastMap = mapNow;
            haveEye = true;
        }
        in.groundFallback = g_state.ground;
        in.mapId = world::CurrentMapId();

        UpdateCelestial(in.dt);
        const Celestial& c = g_celestial;
        std::copy(c.toSun, c.toSun + 3, in.toSun);
        std::copy(c.toMoon, c.toMoon + 3, in.toMoon);
        in.sunWeight = c.sunWeight;
        in.moonWeight = c.moonWeight;
        std::copy(c.rgb, c.rgb + 3, in.lightRgb);
        const sky::SkyColors colours = sky::GetSkyColors();
        Argb(colours.top, in.skyTop);
        Argb(colours.horizon, in.skyHorizon);
        Argb(sky::FogColor(), in.zoneFog);

        const WXL_GraphicsLightsApi* lights = fog::Lights();
        const bool indoor = lights && s.indoorDetect && lights->CameraIndoor() != 0;
        g_state.cameraIndoor = indoor;
        in.indoorActual = indoor ? 1.0f : 0.0f;
        in.indoorEased = EaseIndoor(indoor, in.dt);

        // The wind: the profile's heading (clockwise from north), or the threat API's override.
        const float heading = o.windDirection * 3.14159265f / 180.0f;
        const float profileDir[2] = { std::cos(heading), -std::sin(heading) };
        fog::threat::Tick(in.clock, in.dt);
        fog::threat::Wind(profileDir, o.windSpeed, in.windDir, in.windSpeed);
        in.intensity = s.intensity * fog::threat::Intensity();

        // The frame's primitives: the API's first, then the built-in inputs.
        fog::prims::Begin();
        fog::threat::EmitPrims(o.renewal);
        fog::inputs::UpdateMissiles(in.eye, in.clock);
        int lightCount = 0;
        uint32_t listFrame = 0;
        const WXL_GfxLight* list = lights ? lights->Current(&lightCount, &listFrame) : nullptr;
        fog::inputs::Emit(in.eye, in.clock, list, list ? lightCount : 0);

        // The wake fluid centres on the player (the camera without one) and takes this frame's splats.
        static float splats[FOG_MAX_BODIES * 8];
        const float* centre = g_state.havePlayer ? g_state.player : in.eye;
        std::copy(centre, centre + 3, in.wakeCentre);
        in.splatCount = fog::inputs::WakeSplats(centre, in.clock, list, list ? lightCount : 0, splats, FOG_MAX_BODIES);
        in.splats = splats;
        static float occluders[FOG_MAX_OCCLUDERS * 4];
        in.occluderCount = fog::inputs::Occluders(in.eye, occluders, FOG_MAX_OCCLUDERS);
        in.occluders = occluders;
        // Per lamp, the bodies within its reach: only they are tested for its shadow.
        static uint32_t masks[128];
        const int lamps = list ? std::min(lightCount, 128) : 0;
        for (int i = 0; i < lamps; ++i)
        {
            const WXL_GfxLight& l = list[i];
            uint32_t m = 0;
            for (int k = 0; k < in.occluderCount; ++k)
            {
                const float* o = occluders + k * 4;
                const float dx = o[0] - l.position[0], dy = o[1] - l.position[1];
                const float dz = std::max(std::max(o[2] - l.position[2], l.position[2] - (o[2] + o[3])), 0.0f);
                const float reach = std::fabs(l.radius) + 1.5f;
                if (dx * dx + dy * dy + dz * dz < reach * reach) m |= 1u << k;
            }
            masks[i] = m;
        }
        in.occluderMasks = masks;
        in.maskCount = lamps;

        if (fog::gpu::Record(vk, in)) g_lastDrawn = Now();
    }

    void __cdecl RecordCompute(void*, const WXL_GfxVkFrame* vk)
    {
        try
        {
            if (vk) RecordFrame(*vk);
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; FOG_LOG_ERROR("exception while recording the fog; the frame is skipped"); }
        }
    }

    void __cdecl DrawApply(void*, const WXL_GfxFrame* frame)
    {
        try
        {
            if (frame) fog::gpu::Apply(*frame);
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; FOG_LOG_ERROR("exception while compositing the fog; the frame is skipped"); }
        }
    }

    void Register()
    {
        if (g_state.registered) return;
        const WXL_GraphicsExtendApi* gfx = fog::Gfx();
        const WXL_GfxVulkanApi* vk = fog::Vk();
        if (!gfx || !vk) return;

        WXL_GfxVkPassDesc compute{};
        compute.structSize = sizeof compute;
        compute.name = "fog";
        compute.order = WXL_GFX_ORDER_ATMOSPHERE;
        compute.wants = &WantsCompute;
        compute.record = &RecordCompute;
        g_state.computePass = vk->AddComputePass(&compute);

        WXL_GfxPassDesc apply{};
        apply.structSize = sizeof apply;
        apply.name = "fog.apply";
        apply.order = WXL_GFX_ORDER_ATMOSPHERE;
        apply.wants = &WantsApply;
        apply.draw = &DrawApply;
        g_state.applyPass = gfx->AddPass(&apply);

        g_state.registered = true;
        if (!g_state.computePass || !g_state.applyPass)
            FOG_LOG_ERROR("wxl-graphics-extend refused the fog's passes (compute %u, apply %u); the fog stays inert",
                          g_state.computePass, g_state.applyPass);
        else
            FOG_LOG_INFO("passes registered at order %d: compute \"fog\" (%u), D3D9 \"fog.apply\" (%u)%s", WXL_GFX_ORDER_ATMOSPHERE,
                         g_state.computePass, g_state.applyPass,
                         fog::Lights() ? "" : "; wxl-graphics-lights not found yet (no lamps, no rooms until it is)");
    }

    void Tick(float dt)
    {
        Register();
        UpdateNativeFog();

        g_state.havePlayer = false;
        const unsigned long long guid = world::ActivePlayerGuid();
        void* unit = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
        if (unit)
        {
            world::UnitPosition(unit, g_state.player);
            g_state.havePlayer = true;
        }

        // The ground under the camera: the floor where no baked tile is resident.
        float eye[3];
        cam::GetPosition(eye);
        // Traced far down, so a flying camera still finds the ground; a miss keeps the last ground (never
        // the camera's or the player's height, which would lift the fog with them).
        static bool haveGround = false;
        const float from[3] = { eye[0], eye[1], eye[2] + 5.0f };
        const float to[3] = { eye[0], eye[1], eye[2] - 3000.0f };
        world::WorldHit hit;
        if (world::TraceLine(from, to, hit))
        {
            g_state.ground = hit.pos.z;
            haveGround = true;
        }
        else if (!haveGround && g_state.havePlayer)
        {
            g_state.ground = g_state.player[2];
        }

        const fog::Settings& s = fog::Config();
        const bool live = s.enabled && CanDraw();
        if (live && s.wakes) fog::inputs::UpdateBodies(dt, g_state.player, g_state.havePlayer, s.wakeRange);
        GiveLights();
        // A light in a room lights indoor air only, while the fog tells indoor from outdoor.
        if (const WXL_GraphicsLightsApi* lights = fog::Lights()) lights->SetInteriorGate(live && s.indoorDetect ? 1 : 0);
    }

    class FogModule final : public wxl::ext::EventScript
    {
    public:
        FogModule()
        {
            on<&FogModule::OnUpdate>(ev::Event::OnUpdate);
            on<&FogModule::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

        void OnUpdate(const ev::UpdateArgs& a)
        {
            try
            {
                Tick(a.dt);
            }
            catch (...)
            {
                static bool said = false;
                if (!said) { said = true; FOG_LOG_ERROR("exception on the fog's tick; the tick is skipped"); }
            }
        }

        void OnDeviceLost(const ev::DeviceResetArgs&)
        {
            try { fog::gpu::OnDeviceLost(); } catch (...) {}
        }
    };

    void __cdecl PanelBody(void*)
    {
        try
        {
            fog::Panel();
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; FOG_LOG_ERROR("exception while drawing the fog's panel"); }
        }
    }
}

namespace wxl::gfx::fog
{
    bool Active() { return Config().enabled && CanDraw() && DrewRecently(); }

    const State& GetState() { return g_state; }

    void Install()
    {
        LoadSettings();
        threat::Publish();
        static FogModule module;
        g_api->UiAddPanel("Graphics Fog", &PanelBody, nullptr);
        Register();
        const Settings& s = Config();
        FOG_LOG_INFO("installed (enabled %d, quality %d, near %d / far %d steps, layer density %.3f, indoor dust %.3f); %s", s.enabled,
                     s.quality, s.nearSteps, s.farSteps, Outdoor().density, Indoor().density,
                     GetState().registered ? "passes registered" : "waiting for wxl-graphics-extend");
    }
}
