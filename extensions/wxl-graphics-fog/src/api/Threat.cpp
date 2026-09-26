// wxl-graphics-fog: the threat API.
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

#include "Threat.hpp"
#include "../core/Extension.hpp"
#include "../gpu/Gpu.hpp"
#include "../sim/Primitives.hpp"
#include "../Fog.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    namespace pr = wxl::gfx::fog::prims;

    /// A fade in, a life and a fade out; removal starts the fade out from wherever it stands.
    struct Life
    {
        bool     live = false;
        uint32_t handle = 0;
        float    age = 0.0f, duration = 0.0f, fadeIn = 0.0f, fadeOut = 0.0f;
        float    removedAt = -1.0f, removeFade = 0.0f;

        float Ease(float x) const { x = std::clamp(x, 0.0f, 1.0f); return x * x * (3.0f - 2.0f * x); }

        float End() const { return removedAt >= 0.0f ? removedAt : (duration > 0.0f ? duration : 1e30f); }
        float Fade() const { return removedAt >= 0.0f ? removeFade : fadeOut; }

        float Weight() const
        {
            const float in = fadeIn > 0.0f ? Ease(age / fadeIn) : 1.0f;
            const float end = End(), fade = Fade();
            const float out = age < end ? 1.0f : (fade > 0.0f ? Ease(1.0f - (age - end) / fade) : 0.0f);
            return in * out;
        }

        bool Dead() const { return age > End() + Fade(); }
    };

    struct Source { Life life; WXL_GfxFogSource d; };
    struct Front  { Life life; WXL_GfxFogFront d; };
    struct Flow   { Life life; WXL_GfxFogFlow d; };
    struct Pulse  { Life life; WXL_GfxFogPulse d; };
    struct Pour   { Life life; WXL_GfxFogCascade d; };

    Source g_sources[WXL_GFX_FOG_MAX_SOURCES];
    Front  g_fronts[WXL_GFX_FOG_MAX_FRONTS];
    Flow   g_flows[WXL_GFX_FOG_MAX_FLOWS];
    Pulse  g_pulses[WXL_GFX_FOG_MAX_PULSES];
    Pour   g_pours[WXL_GFX_FOG_MAX_CASCADES];
    uint32_t g_nextHandle = 1;

    WXL_GfxFogVolume g_volumes[WXL_GFX_FOG_MAX_VOLUMES];
    int              g_volumeCount = 0;
    WXL_GfxFogLight  g_lights[WXL_GFX_FOG_MAX_LIGHTS];
    int              g_lightCount = 0;

    // The intensity eases from `from` to `to` over `span` seconds.
    float g_intensity = 1.0f, g_intFrom = 1.0f, g_intTo = 1.0f, g_intT = 1.0f, g_intSpan = 0.0f;
    float g_pulse = 0.0f;

    // The wind override and its share.
    float g_windDir[2] = { 1.0f, 0.0f };
    float g_windSpeed = 0.0f;
    float g_windShare = 0.0f, g_windTarget = 0.0f, g_windRate = 1.0f;

    char g_status[192] = "threat: nothing fed";

    uint32_t NewHandle() { uint32_t h = g_nextHandle++; if (!g_nextHandle) g_nextHandle = 1; return h; }

    template <class T, size_t N>
    T* Find(T (&pool)[N], uint32_t handle)
    {
        if (!handle) return nullptr;
        for (T& e : pool)
            if (e.life.live && e.life.handle == handle) return &e;
        return nullptr;
    }

    template <class T, size_t N>
    T* Free(T (&pool)[N])
    {
        for (T& e : pool)
            if (!e.life.live) return &e;
        return nullptr;
    }

    Life Start(float duration, float fadeIn, float fadeOut)
    {
        Life l;
        l.live = true;
        l.handle = NewHandle();
        l.duration = std::max(duration, 0.0f);
        l.fadeIn = std::max(fadeIn, 0.0f);
        l.fadeOut = std::max(fadeOut, 0.0f);
        return l;
    }

    void Remove(Life& l, float fadeOut)
    {
        if (l.removedAt >= 0.0f && l.removedAt <= l.age) return;
        l.removedAt = std::min(l.age, l.End());
        l.removeFade = fadeOut >= 0.0f ? fadeOut : l.fadeOut;
    }

    template <class T, size_t N>
    void Age(T (&pool)[N], float dt)
    {
        for (T& e : pool)
        {
            if (!e.life.live) continue;
            e.life.age += dt;
            if (e.life.Dead()) e.life = Life{};
        }
    }

    pr::Prim Shape(int shape, const float c[3], const float e[3], float yaw)
    {
        switch (shape)
        {
        case WXL_GFX_FOG_SHAPE_BOX: return pr::Box(c, e, yaw);
        case WXL_GFX_FOG_SHAPE_CAPSULE:
        {
            const float r = std::max(e[0], 0.1f);
            const float a[3] = { c[0], c[1], c[2] + r };
            const float b[3] = { c[0], c[1], c[2] + std::max(e[2] - r, r) };
            return pr::Capsule(a, b, r);
        }
        case WXL_GFX_FOG_SHAPE_CYLINDER: return pr::Cylinder(c, std::max(e[0], 0.1f), std::max(e[2], 0.1f));
        default: return pr::Sphere(c, std::max(e[0], 0.1f));
        }
    }

    // --- the C ABI (plain copies, nothing that throws) --------------------------------------------

    void __cdecl ApiSetLights(const WXL_GfxFogLight* lights, int count)
    {
        g_lightCount = lights ? std::clamp(count, 0, WXL_GFX_FOG_MAX_LIGHTS) : 0;
        if (g_lightCount) std::memcpy(g_lights, lights, sizeof(WXL_GfxFogLight) * size_t(g_lightCount));
    }

    void __cdecl ApiSetVolumes(const WXL_GfxFogVolume* volumes, int count)
    {
        g_volumeCount = volumes ? std::clamp(count, 0, WXL_GFX_FOG_MAX_VOLUMES) : 0;
        if (g_volumeCount) std::memcpy(g_volumes, volumes, sizeof(WXL_GfxFogVolume) * size_t(g_volumeCount));
    }

    int __cdecl ApiActive(void) { return wxl::gfx::fog::Active() ? 1 : 0; }

    uint32_t __cdecl ApiAddSource(const WXL_GfxFogSource* s)
    {
        if (!s || s->structSize < sizeof(WXL_GfxFogSource)) return 0;
        Source* e = Free(g_sources);
        if (!e) return 0;
        e->d = *s;
        e->life = Start(s->duration, s->fadeIn, s->fadeOut);
        return e->life.handle;
    }

    int __cdecl ApiUpdateSource(uint32_t handle, const WXL_GfxFogSource* s)
    {
        Source* e = Find(g_sources, handle);
        if (!e || !s || s->structSize < sizeof(WXL_GfxFogSource)) return 0;
        e->d = *s;
        e->life.duration = std::max(s->duration, 0.0f);
        e->life.fadeOut = std::max(s->fadeOut, 0.0f);
        return 1;
    }

    void __cdecl ApiRemoveSource(uint32_t handle, float fadeOut)
    {
        if (Source* e = Find(g_sources, handle)) Remove(e->life, fadeOut);
    }

    uint32_t __cdecl ApiAddFront(const WXL_GfxFogFront* f)
    {
        if (!f || f->structSize < sizeof(WXL_GfxFogFront)) return 0;
        Front* e = Free(g_fronts);
        if (!e) return 0;
        e->d = *f;
        const float len = std::sqrt(f->direction[0] * f->direction[0] + f->direction[1] * f->direction[1]);
        e->d.direction[0] = len > 1e-4f ? f->direction[0] / len : 1.0f;
        e->d.direction[1] = len > 1e-4f ? f->direction[1] / len : 0.0f;
        e->life = Start(f->duration, f->fadeIn, f->fadeOut);
        return e->life.handle;
    }

    void __cdecl ApiRemoveFront(uint32_t handle, float fadeOut)
    {
        if (Front* e = Find(g_fronts, handle)) Remove(e->life, fadeOut);
    }

    uint32_t __cdecl ApiAddFlow(const WXL_GfxFogFlow* f)
    {
        if (!f || f->structSize < sizeof(WXL_GfxFogFlow)) return 0;
        Flow* e = Free(g_flows);
        if (!e) return 0;
        e->d = *f;
        e->life = Start(f->duration, f->fadeIn, f->fadeOut);
        return e->life.handle;
    }

    int __cdecl ApiUpdateFlow(uint32_t handle, const WXL_GfxFogFlow* f)
    {
        Flow* e = Find(g_flows, handle);
        if (!e || !f || f->structSize < sizeof(WXL_GfxFogFlow)) return 0;
        e->d = *f;
        e->life.duration = std::max(f->duration, 0.0f);
        e->life.fadeOut = std::max(f->fadeOut, 0.0f);
        return 1;
    }

    void __cdecl ApiRemoveFlow(uint32_t handle, float fadeOut)
    {
        if (Flow* e = Find(g_flows, handle)) Remove(e->life, fadeOut);
    }

    void __cdecl ApiSetIntensity(float intensity, float seconds)
    {
        g_intFrom = g_intensity;
        g_intTo = std::clamp(intensity, 0.0f, 8.0f);
        g_intSpan = std::max(seconds, 0.0f);
        g_intT = g_intSpan > 0.0f ? 0.0f : 1.0f;
        if (g_intSpan <= 0.0f) g_intensity = g_intTo;
    }

    uint32_t __cdecl ApiPulse(const WXL_GfxFogPulse* p)
    {
        if (!p || p->structSize < sizeof(WXL_GfxFogPulse)) return 0;
        Pulse* e = Free(g_pulses);
        if (!e) return 0;
        e->d = *p;
        const bool breath = p->kind == WXL_GFX_FOG_PULSE_BREATH;
        e->life = Start(breath ? p->duration : std::max(p->attack, 0.0f) + std::max(p->release, 0.0f),
                        breath ? std::max(p->attack, 0.5f) : 0.0f, breath ? std::max(p->release, 0.5f) : 0.0f);
        return e->life.handle;
    }

    void __cdecl ApiStopPulse(uint32_t handle, float fadeOut)
    {
        if (Pulse* e = Find(g_pulses, handle)) Remove(e->life, fadeOut);
    }

    void __cdecl ApiSetWind(const float direction[2], float speed, float seconds)
    {
        g_windRate = seconds > 0.0f ? 1.0f / seconds : 1e6f;
        if (speed < 0.0f || !direction)
        {
            g_windTarget = 0.0f;
            return;
        }
        const float len = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1]);
        if (len > 1e-4f)
        {
            g_windDir[0] = direction[0] / len;
            g_windDir[1] = direction[1] / len;
        }
        g_windSpeed = speed;
        g_windTarget = 1.0f;
    }

    void Normalise(WXL_GfxFogCascade& d)
    {
        const float len = std::sqrt(d.direction[0] * d.direction[0] + d.direction[1] * d.direction[1]);
        d.direction[0] = len > 1e-4f ? d.direction[0] / len : 1.0f;
        d.direction[1] = len > 1e-4f ? d.direction[1] / len : 0.0f;
        d.width = std::max(d.width, 2.0f);
        d.depth = std::clamp(d.depth, 0.0f, 80.0f);
    }

    uint32_t __cdecl ApiAddCascade(const WXL_GfxFogCascade* c)
    {
        if (!c || c->structSize < sizeof(WXL_GfxFogCascade)) return 0;
        Pour* e = Free(g_pours);
        if (!e) return 0;
        e->d = *c;
        Normalise(e->d);
        e->life = Start(c->duration, c->fadeIn, c->fadeOut);
        return e->life.handle;
    }

    int __cdecl ApiUpdateCascade(uint32_t handle, const WXL_GfxFogCascade* c)
    {
        Pour* e = Find(g_pours, handle);
        if (!e || !c || c->structSize < sizeof(WXL_GfxFogCascade)) return 0;
        e->d = *c;
        Normalise(e->d);
        e->life.duration = std::max(c->duration, 0.0f);
        e->life.fadeOut = std::max(c->fadeOut, 0.0f);
        return 1;
    }

    void __cdecl ApiRemoveCascade(uint32_t handle, float fadeOut)
    {
        if (Pour* e = Find(g_pours, handle)) Remove(e->life, fadeOut);
    }

    float __cdecl ApiCameraDensity(void) { return wxl::gfx::fog::gpu::Probe().extinction; }
    float __cdecl ApiVisibility(void) { return wxl::gfx::fog::gpu::Probe().visibility; }
    float __cdecl ApiCameraIndoor(void) { return wxl::gfx::fog::gpu::Probe().indoor; }

    void __cdecl ApiClearAll(float fadeOut)
    {
        for (Source& e : g_sources) if (e.life.live) Remove(e.life, fadeOut);
        for (Front& e : g_fronts) if (e.life.live) Remove(e.life, fadeOut);
        for (Flow& e : g_flows) if (e.life.live) Remove(e.life, fadeOut);
        for (Pulse& e : g_pulses) if (e.life.live) Remove(e.life, fadeOut);
        for (Pour& e : g_pours) if (e.life.live) Remove(e.life, fadeOut);
        g_volumeCount = 0;
        g_lightCount = 0;
        ApiSetIntensity(1.0f, fadeOut);
        g_windTarget = 0.0f;
        g_windRate = fadeOut > 0.0f ? 1.0f / fadeOut : 1e6f;
    }

    const WXL_GraphicsFogApi kApi = {
        sizeof(WXL_GraphicsFogApi),
        WXL_GRAPHICS_FOG_API_VERSION,
        &ApiSetLights,
        &ApiSetVolumes,
        &ApiActive,
        &ApiAddSource,
        &ApiUpdateSource,
        &ApiRemoveSource,
        &ApiAddFront,
        &ApiRemoveFront,
        &ApiAddFlow,
        &ApiUpdateFlow,
        &ApiRemoveFlow,
        &ApiSetIntensity,
        &ApiPulse,
        &ApiStopPulse,
        &ApiSetWind,
        &ApiCameraDensity,
        &ApiVisibility,
        &ApiCameraIndoor,
        &ApiClearAll,
        &ApiAddCascade,
        &ApiUpdateCascade,
        &ApiRemoveCascade,
    };
}

namespace wxl::gfx::fog::threat
{
    void Publish()
    {
        g_api->PublishInterface(WXL_GRAPHICS_FOG_API_NAME, WXL_GRAPHICS_FOG_API_VERSION, const_cast<WXL_GraphicsFogApi*>(&kApi));
        // Earlier versions' functions keep their places: an older caller finds the same table.
        g_api->PublishInterface(WXL_GRAPHICS_FOG_API_NAME, 2, const_cast<WXL_GraphicsFogApi*>(&kApi));
        g_api->PublishInterface(WXL_GRAPHICS_FOG_API_NAME, 1, const_cast<WXL_GraphicsFogApi*>(&kApi));
    }

    const WXL_GraphicsFogApi* Table() { return &kApi; }

    void Tick(double clock, float dt)
    {
        (void)clock;
        Age(g_sources, dt);
        Age(g_fronts, dt);
        Age(g_flows, dt);
        Age(g_pulses, dt);
        Age(g_pours, dt);

        if (g_intT < 1.0f)
        {
            g_intT = std::min(g_intT + dt / std::max(g_intSpan, 1e-3f), 1.0f);
            const float s = g_intT * g_intT * (3.0f - 2.0f * g_intT);
            g_intensity = g_intFrom + (g_intTo - g_intFrom) * s;
        }

        float pulse = 0.0f;
        for (const Pulse& p : g_pulses)
        {
            if (!p.life.live) continue;
            if (p.d.kind == WXL_GFX_FOG_PULSE_BREATH)
            {
                const float period = std::max(p.d.period, 0.5f);
                pulse += p.d.amplitude * std::sin(6.2831853f * p.life.age / period) * p.life.Weight();
            }
            else
            {
                const float a = std::max(p.d.attack, 0.01f), r = std::max(p.d.release, 0.01f);
                const float t = p.life.age;
                float env = t < a ? t / a : 1.0f - (t - a) / r;
                env = std::clamp(env, 0.0f, 1.0f);
                pulse += p.d.amplitude * env * env * (3.0f - 2.0f * env);
            }
        }
        g_pulse = pulse;

        const float step = g_windRate * dt;
        g_windShare += std::clamp(g_windTarget - g_windShare, -step, step);

        int s = 0, f = 0, w = 0, p = 0, c = 0;
        for (const Pour& e : g_pours) c += e.life.live ? 1 : 0;
        for (const Source& e : g_sources) s += e.life.live ? 1 : 0;
        for (const Front& e : g_fronts) f += e.life.live ? 1 : 0;
        for (const Flow& e : g_flows) w += e.life.live ? 1 : 0;
        for (const Pulse& e : g_pulses) p += e.life.live ? 1 : 0;
        std::snprintf(g_status, sizeof g_status,
                      "threat: %d sources, %d fronts, %d flows, %d pulses, %d cascades, %d volumes, %d lights | intensity %.2f x %.2f | wind override %.0f%%",
                      s, f, w, p, c, g_volumeCount, g_lightCount, g_intensity, 1.0f + g_pulse, g_windShare * 100.0f);
    }

    float Intensity() { return std::max(g_intensity * (1.0f + g_pulse), 0.0f); }

    int Cascades(int mapId, float* rows, int cap)
    {
        int n = 0;
        for (const Pour& e : g_pours)
        {
            if (!e.life.live || n >= cap) continue;
            if (e.d.mapId >= 0 && e.d.mapId != mapId) continue;
            float* r = rows + n * 8;
            r[0] = e.d.position[0]; r[1] = e.d.position[1]; r[2] = e.d.position[2]; r[3] = e.d.width;
            r[4] = e.d.direction[0]; r[5] = e.d.direction[1]; r[6] = e.d.depth; r[7] = e.life.Weight();
            ++n;
        }
        return n;
    }

    void Wind(const float profileDir[2], float profileSpeed, float dir[2], float& speed)
    {
        const float t = std::clamp(g_windShare, 0.0f, 1.0f);
        // Blend the wind vectors, so a turn passes through the calm in between rather than jumping.
        const float a[2] = { profileDir[0] * profileSpeed, profileDir[1] * profileSpeed };
        const float b[2] = { g_windDir[0] * g_windSpeed, g_windDir[1] * g_windSpeed };
        const float v[2] = { a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t };
        speed = std::sqrt(v[0] * v[0] + v[1] * v[1]);
        if (speed > 1e-4f)
        {
            dir[0] = v[0] / speed;
            dir[1] = v[1] / speed;
        }
        else
        {
            dir[0] = profileDir[0];
            dir[1] = profileDir[1];
        }
    }

    void EmitPrims(float renewal)
    {
        for (const Source& e : g_sources)
        {
            if (!e.life.live) continue;
            const float w = e.life.Weight();
            if (w <= 0.0f) continue;
            const WXL_GfxFogSource& d = e.d;
            pr::Prim p = Shape(d.shape, d.center, d.extent, d.yaw);
            const float soft = std::clamp(d.softness, 0.0f, 1.0f);
            switch (d.op)
            {
            case WXL_GFX_FOG_OP_CARVE: pr::SetDensity(p, -std::max(d.strength, 0.0f) * w, 0.0f, 0.0f, soft, d.billow); break;
            case WXL_GFX_FOG_OP_HOLD:  pr::SetDensity(p, 0.0f, 0.0f, std::max(d.strength, 0.0f) * w, soft, d.billow); break;
            case WXL_GFX_FOG_OP_SMOKE: pr::SetDensity(p, 0.0f, std::max(d.strength, 0.0f) * w, 0.0f, soft, d.billow); break;
            default:                   pr::SetDensity(p, std::max(d.strength, 0.0f) * w, 0.0f, 0.0f, soft, d.billow); break;
            }
            const float push[3] = { d.push[0] * w, d.push[1] * w, d.push[2] * w };
            pr::SetFlow(p, push, d.radialPush * w, d.swirl * w);
            pr::Add(p);
        }

        for (const Front& e : g_fronts)
        {
            if (!e.life.live) continue;
            const float w = e.life.Weight();
            if (w <= 0.0f) continue;
            const WXL_GfxFogFront& d = e.d;
            const float travelled = std::max(d.speed, 0.0f) * e.life.age;
            pr::Prim p;
            const float zero[3] = { 0.0f, 0.0f, 0.0f };
            if (d.kind == WXL_GFX_FOG_FRONT_CLOSING)
            {
                const float r = std::max(d.startRadius - travelled, std::max(d.endRadius, 0.0f));
                p = pr::Ring(d.origin, r, std::max(d.height, 1.0f), std::max(d.depth, 1.0f));
                pr::SetFlow(p, zero, -d.push * w, 0.0f);
            }
            else
            {
                const float along = d.endRadius > 0.0f ? std::min(travelled, d.endRadius) : travelled;
                const float at[3] = { d.origin[0] + d.direction[0] * along, d.origin[1] + d.direction[1] * along, d.origin[2] };
                p = pr::Front(at, d.direction, std::max(d.halfWidth, 0.0f), std::max(d.height, 1.0f), std::max(d.depth, 1.0f));
                const float push[3] = { d.direction[0] * d.push * w, d.direction[1] * d.push * w, 0.0f };
                pr::SetFlow(p, push, 0.0f, 0.0f);
            }
            pr::SetDensity(p, 0.0f, 0.0f, std::max(d.density, 0.0f) * w, 0.35f, std::clamp(d.billow, 0.0f, 1.0f));
            pr::Add(p);
        }

        for (const Flow& e : g_flows)
        {
            if (!e.life.live) continue;
            const float w = e.life.Weight();
            if (w <= 0.0f) continue;
            const WXL_GfxFogFlow& d = e.d;
            pr::Prim p = Shape(d.shape, d.center, d.extent, d.yaw);
            pr::SetDensity(p, 0.0f, 0.0f, 0.0f, std::clamp(d.softness, 0.0f, 1.0f), 0.0f);
            const float push[3] = { d.velocity[0] * w, d.velocity[1] * w, d.velocity[2] * w };
            pr::SetFlow(p, push, d.radial * w, d.swirl * w);
            pr::Add(p);
        }

        // Version 1's volumes: held density, or a carved share balanced against the renewal.
        for (int i = 0; i < g_volumeCount; ++i)
        {
            const WXL_GfxFogVolume& v = g_volumes[i];
            pr::Prim p = Shape(v.shape, v.center, v.extent, 0.0f);
            const float soft = std::clamp(v.falloff, 0.0f, 1.0f);
            if (v.carve)
            {
                const float share = std::clamp(v.density, 0.0f, 0.98f);
                pr::SetDensity(p, -share / ((1.0f - share) * std::max(renewal, 0.5f)), 0.0f, 0.0f, soft, 0.3f);
            }
            else pr::SetDensity(p, 0.0f, 0.0f, std::max(v.density, 0.0f), soft, 0.4f);
            const float zero[3] = { 0.0f, 0.0f, 0.0f };
            pr::SetFlow(p, zero, 0.0f, std::clamp(v.swirl, 0.0f, 1.0f) * 1.5f);
            pr::Add(p);
        }
    }

    int LightCount() { return g_lightCount; }
    const WXL_GfxFogLight* Lights() { return g_lights; }
    const char* Status() { return g_status; }
}
