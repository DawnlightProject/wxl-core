// Scene lights: injects point lights into the world M2 scene and replaces the per-receiver selection
// with an influence ranking, or selects none (policy off, for an extension that shades point lights
// itself). Published as "wxl.scenelights" (include/wxl/SceneLightsApi.h).
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

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "game/World.hpp"
#include "offsets/game/Lights.hpp"
#include "offsets/game/SceneLights.hpp"
#include "runtime/Extensions.hpp"
#include "wxl/SceneLightsApi.h"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace
{
    namespace ev  = wxl::events;
    namespace off = wxl::offsets::game::scenelights;
    namespace rec = wxl::offsets::game::lights;

    constexpr uint32_t kPoolSize     = 256;
    constexpr float    kMaxWiden     = 32.0f;  // yards the cell scan grows by, at most
    constexpr float    kInfluenceCut = 0.02f;  // contribution below which a light no longer counts

    // The CM2Light image comes first: the engine only ever touches bytes 0..0x6B of it.
    struct Slot
    {
        alignas(16) uint8_t light[off::kLightSize];
        WXL_SceneLight params;
        uint32_t handle;       // 0 = free
        uintptr_t scene;       // scene it was initialised in, 0 = not in any
        uintptr_t grid;        // that scene's grid when it was linked
        uint32_t frame;        // last frame it was stamped with
    };

    Slot     g_slots[kPoolSize];
    uint32_t g_nextHandle = 1;
    uint32_t g_count      = 0;

    int   g_policy    = WXL_LIGHT_POLICY_INFLUENCE;
    bool  g_worldPass = false;  // between OnWorldSceneBegin and OnWorldSceneEnd
    float g_widen     = 0.0f;  // this frame's scan growth, from the largest light radius
    float g_scanWiden = 0.0f;  // growth applied to the receiver currently being selected for

    off::SceneAnimateFn      g_origAnimate      = nullptr;
    off::SceneSelectLightsFn g_origSelectLights = nullptr;
    off::AddLightFn          g_origAddLight     = nullptr;

    // Diagnostic orb and pick log (WXL_DIAG_LIGHTS).
    bool               g_diag       = false;
    uint32_t           g_diagHandle = 0;
    int                g_diagLogs   = 0;
    unsigned long long g_diagWindow = 0;

    template <class T>
    inline T& At(uintptr_t base, size_t offset) { return *reinterpret_cast<T*>(base + offset); }

    inline uintptr_t WorldScene() { return *reinterpret_cast<const uintptr_t*>(rec::kWorldM2Scene); }

    inline float Luma(const float c[3]) { return 0.212671f * c[0] + 0.71516f * c[1] + 0.072169f * c[2]; }

    /// Distance at which luma * falloff drops under kInfluenceCut.
    float InfluenceRadius(const float color[3], const float f[3])
    {
        const float l = Luma(color);
        if (l <= 0.0f) return 0.0f;
        const float c = f[0] - l / kInfluenceCut;
        float d;
        if (f[2] > 1e-6f)      d = (-f[1] + std::sqrt(std::fmax(0.0f, f[1] * f[1] - 4.0f * f[2] * c))) / (2.0f * f[2]);
        else if (f[1] > 1e-6f) d = -c / f[1];
        else                   d = kMaxWiden;
        return std::fmin(std::fmax(d, 0.0f), kMaxWiden);
    }

    // --- engine calls on one of our records ---------------------------------------------------

    void* Light(Slot& s) { return s.light; }

    /// True when s is linked into the live world scene's grid, so unlinking it is safe.
    bool LinkedInLiveScene(const Slot& s)
    {
        const uintptr_t scene = WorldScene();
        if (!s.scene || s.scene != scene) return false;
        if (At<uintptr_t>(scene, rec::kOffScenePointGrid) != s.grid) return false;
        if (At<uint32_t>(scene, rec::kOffSceneFrame) + 1 < s.frame) return false; // a newer scene reused the address
        return At<uint32_t>(uintptr_t(s.light), rec::kOffLightVisible) != 0;
    }

    void Unlink(Slot& s)
    {
        if (LinkedInLiveScene(s))
            reinterpret_cast<off::LightSetVisibleFn>(off::kLightSetVisible)(Light(s), nullptr, 0);
        s.scene = 0;
        s.grid  = 0;
    }

    void Apply(Slot& s, uintptr_t scene, uint32_t frame)
    {
        const uintptr_t l = uintptr_t(s.light);
        if (s.scene != scene || At<uintptr_t>(scene, rec::kOffScenePointGrid) != s.grid)
        {
            // Never linked here: start from a clean record (an old scene's links are not ours to fix).
            if (s.scene && s.scene == scene) Unlink(s);
            std::memset(s.light, 0, sizeof s.light);
            reinterpret_cast<off::LightConstructFn>(off::kLightConstruct)(Light(s));
            reinterpret_cast<off::LightInitializeFn>(off::kLightInitialize)(Light(s), nullptr, reinterpret_cast<void*>(scene));
            reinterpret_cast<off::LightSetTypeFn>(off::kLightSetType)(Light(s), nullptr, int(rec::kLightTypePoint));
            s.scene = scene;
        }

        for (int k = 0; k < 3; ++k)
        {
            At<float>(l, rec::kOffLightAmbient + k * 4) = 0.0f;
            At<float>(l, rec::kOffLightDiffuse + k * 4) = s.params.color[k];
            At<float>(l, rec::kOffLightAtten + k * 4)   = s.params.falloff[k];
        }
        At<uint32_t>(l, rec::kOffLightStamp) = frame;
        s.frame = frame;

        if (!At<uint32_t>(l, rec::kOffLightVisible))
        {
            for (int k = 0; k < 3; ++k) At<float>(l, rec::kOffLightPosition + k * 4) = s.params.position[k];
            reinterpret_cast<off::LightSetVisibleFn>(off::kLightSetVisible)(Light(s), nullptr, 1);
        }
        else
        {
            reinterpret_cast<off::LightSetPositionFn>(off::kLightSetPosition)(Light(s), nullptr, s.params.position);
        }
        s.grid = At<uintptr_t>(scene, rec::kOffScenePointGrid);
    }

    Slot* Find(uint32_t handle)
    {
        if (!handle) return nullptr;
        for (Slot& s : g_slots) if (s.handle == handle) return &s;
        return nullptr;
    }

    /// Largest influence radius among the lights in the grid, for the scan growth.
    float LargestRadius(uintptr_t scene)
    {
        const uintptr_t grid = At<uintptr_t>(scene, rec::kOffScenePointGrid);
        if (!grid) return 0.0f;
        float best = 0.0f;
        uint32_t guard = 0;
        for (uint32_t cell = 0; cell < off::kGridCells * off::kGridCells; ++cell)
            for (uintptr_t l = At<uintptr_t>(grid, cell * 4); l && guard < 100000;
                 l = At<uintptr_t>(l, rec::kOffLightNext), ++guard)
                best = std::fmax(best, InfluenceRadius(&At<float>(l, rec::kOffLightDiffuse),
                                                       &At<float>(l, rec::kOffLightAtten)));
        return best;
    }

    // --- detours ------------------------------------------------------------------------------

    /// Runs before the scene frame is bumped: stamps and links our lights with the frame the engine
    /// is about to select against.
    void __fastcall hkAnimate(void* scene, void* edx, const float* cameraPos)
    {
        const uintptr_t sc = uintptr_t(scene);
        if (sc && sc == WorldScene())
        {
            __try
            {
                const uint32_t next = At<uint32_t>(sc, rec::kOffSceneFrame) + 1;
                for (Slot& s : g_slots) if (s.handle) Apply(s, sc, next);
                g_widen = g_policy == WXL_LIGHT_POLICY_INFLUENCE ? LargestRadius(sc) : 0.0f;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) { g_widen = 0.0f; }
        }
        g_origAnimate(scene, edx, cameraPos);
    }

    void LogPicks(const void* lighting);

    void __fastcall hkSelectLights(void* scene, void* edx, void* lighting)
    {
        if (g_policy != WXL_LIGHT_POLICY_INFLUENCE || !lighting || uintptr_t(scene) != WorldScene())
        {
            g_origSelectLights(scene, edx, lighting);
            return;
        }
        float& radius = At<float>(uintptr_t(lighting), off::kOffLightingSphere + 12);
        const float saved = radius;
        radius      = saved + g_widen;
        g_scanWiden = g_widen;
        g_origSelectLights(scene, edx, lighting);
        radius      = saved;
        g_scanWiden = 0.0f;
        if (g_diag) LogPicks(lighting);
    }

    /// Influence policy: key = -(luma x falloff at the receiver's sphere), kept ascending like the
    /// stock squared distances so every reader of the array (terrain takes the first 3) sees best first.
    void __fastcall hkAddLight(void* lighting, void* edx, void* light)
    {
        const uintptr_t l = uintptr_t(light), g = uintptr_t(lighting);
        // Off: every receiver of the world (M2, map-object groups, terrain chunks and detail doodads
        // all select through here) takes no point light; the world scene's own lights are dropped
        // wherever they are added, the rest only during the world pass, so UI models keep theirs.
        if (g_policy == WXL_LIGHT_POLICY_OFF && At<uint32_t>(l, rec::kOffLightType) == rec::kLightTypePoint &&
            (g_worldPass || At<uintptr_t>(l, 0) == WorldScene()))
            return;
        if (g_policy != WXL_LIGHT_POLICY_INFLUENCE || !At<uint32_t>(l, rec::kOffLightVisible) ||
            At<uint32_t>(l, rec::kOffLightType) != rec::kLightTypePoint)
        {
            g_origAddLight(lighting, edx, light);
            return;
        }

        const float* c  = &At<float>(g, off::kOffLightingSphere);
        const float* p  = &At<float>(l, rec::kOffLightPosition);
        const float* f  = &At<float>(l, rec::kOffLightAtten);
        const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
        const float receiverRadius = std::fmax(0.0f, c[3] - g_scanWiden);
        const float d   = std::fmax(0.0f, std::sqrt(dx * dx + dy * dy + dz * dz) - receiverRadius);
        const float den = std::fmax(f[0] + f[1] * d + f[2] * d * d, 0.25f);
        const float key = -Luma(&At<float>(l, rec::kOffLightDiffuse)) / den;

        uintptr_t* lights = &At<uintptr_t>(g, off::kOffLightingLights);
        float*     keys   = &At<float>(g, off::kOffLightingKeys);
        uint32_t&  count  = At<uint32_t>(g, off::kOffLightingCount);
        uint32_t   n      = count;
        if (n >= off::kLightingSlots)
        {
            if (keys[off::kLightingSlots - 1] <= key) return;
            n = off::kLightingSlots - 1;
        }
        uint32_t i = n;
        for (; i > 0 && keys[i - 1] > key; --i)
        {
            lights[i] = lights[i - 1];
            keys[i]   = keys[i - 1];
        }
        lights[i] = l;
        keys[i]   = key;
        if (count < off::kLightingSlots) ++count;
    }

    // --- published interface --------------------------------------------------------------------

    uint32_t __cdecl ApiAdd(const WXL_SceneLight* light)
    {
        if (!light) return 0;
        for (Slot& s : g_slots)
        {
            if (s.handle) continue;
            std::memset(&s, 0, sizeof s);
            s.params = *light;
            s.handle = g_nextHandle++;
            if (!g_nextHandle) g_nextHandle = 1;
            ++g_count;
            return s.handle;
        }
        return 0;
    }

    int __cdecl ApiUpdate(uint32_t handle, const WXL_SceneLight* light)
    {
        Slot* s = Find(handle);
        if (!s || !light) return 0;
        s->params = *light;
        return 1;
    }

    void __cdecl ApiRemove(uint32_t handle)
    {
        Slot* s = Find(handle);
        if (!s) return;
        __try { Unlink(*s); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        s->handle = 0;
        --g_count;
    }

    uint32_t __cdecl ApiCount() { return g_count; }

    void __cdecl ApiFalloffForRadius(float radius, float f[3])
    {
        const float r = std::fmax(radius, 0.1f);
        f[0] = 1.0f;
        f[1] = 0.0f;
        f[2] = 99.0f / (r * r); // 1 / (1 + 99) = 1% at r
    }

    void __cdecl ApiSetPolicy(int policy)
    {
        g_policy = policy == WXL_LIGHT_POLICY_STOCK || policy == WXL_LIGHT_POLICY_OFF ? policy : WXL_LIGHT_POLICY_INFLUENCE;
    }

    const char* PolicyName(int policy)
    {
        return policy == WXL_LIGHT_POLICY_STOCK ? "stock (nearest centre)"
             : policy == WXL_LIGHT_POLICY_OFF   ? "off (the world takes no point light; an extension shades them)"
                                                : "influence (brightest at the receiver)";
    }

    int __cdecl ApiPolicy() { return g_policy; }

    int __cdecl ApiSelected(const void* lighting, WXL_SelectedLight* out, int cap)
    {
        if (!lighting || !out || cap <= 0) return 0;
        const uintptr_t g = uintptr_t(lighting);
        const uint32_t count = At<uint32_t>(g, off::kOffLightingCount);
        int n = 0;
        for (uint32_t i = 0; i < count && i < off::kLightingSlots && n < cap; ++i)
        {
            const uintptr_t l = At<uintptr_t>(g, off::kOffLightingLights + i * 4);
            if (!l) continue;
            WXL_SelectedLight& o = out[n++];
            for (int k = 0; k < 3; ++k)
            {
                o.position[k] = At<float>(l, rec::kOffLightPosition + k * 4);
                o.color[k]    = At<float>(l, rec::kOffLightDiffuse + k * 4);
                o.falloff[k]  = At<float>(l, rec::kOffLightAtten + k * 4);
            }
            o.key      = At<float>(g, off::kOffLightingKeys + i * 4);
            o.injected = l >= uintptr_t(g_slots) && l < uintptr_t(g_slots + kPoolSize);
        }
        return n;
    }

    const WXL_SceneLightsApi g_api = {
        sizeof(WXL_SceneLightsApi), WXL_SCENELIGHTS_API_VERSION,
        &ApiAdd, &ApiUpdate, &ApiRemove, &ApiCount, &ApiFalloffForRadius, &ApiSetPolicy, &ApiPolicy, &ApiSelected,
    };

    // --- lifetime --------------------------------------------------------------------------------

    void __cdecl OnWorldPassBegin(void*, const void*) { g_worldPass = true; }
    void __cdecl OnWorldPassEnd(void*, const void*) { g_worldPass = false; }

    void __cdecl OnWorldLeave(void*, const void*)
    {
        for (Slot& s : g_slots)
        {
            if (!s.handle) continue;
            __try { Unlink(s); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            s.handle = 0;
        }
        g_count      = 0;
        g_diagHandle = 0;
    }

    // --- diagnostic ------------------------------------------------------------------------------

    void __cdecl OnUpdateDiag(void*, const void*)
    {
        namespace world = wxl::game::world;
        const unsigned long long guid = world::ActivePlayerGuid();
        void* unit = guid ? world::ResolveObject(guid, world::kTypeMaskPlayer) : nullptr;
        if (!unit) return;
        WXL_SceneLight orb{};
        world::UnitPosition(unit, orb.position);
        orb.position[2] += 2.5f;
        orb.color[0] = 1.6f; orb.color[1] = 1.0f; orb.color[2] = 0.45f;
        ApiFalloffForRadius(12.0f, orb.falloff);
        if (!g_diagHandle || !ApiUpdate(g_diagHandle, &orb))
        {
            g_diagHandle = ApiAdd(&orb);
            WLOG_INFO("scene-lights: diag orb %u at (%.1f, %.1f, %.1f), radius 12 yd", g_diagHandle,
                      orb.position[0], orb.position[1], orb.position[2]);
        }
    }

    void LogPicks(const void* lighting)
    {
        const unsigned long long now = GetTickCount64();
        if (now >= g_diagWindow) { g_diagWindow = now + 5000; g_diagLogs = 0; }
        if (g_diagLogs >= 6) return;

        WXL_SelectedLight picks[4];
        const int n = ApiSelected(lighting, picks, 4);
        bool orb = false;
        for (int i = 0; i < n; ++i) orb |= picks[i].injected != 0;
        if (!orb) return;
        ++g_diagLogs;

        const float* c = reinterpret_cast<const float*>(uintptr_t(lighting) + off::kOffLightingSphere);
        WLOG_INFO("scene-lights: receiver (%.1f, %.1f, %.1f) r %.1f, widen %.1f, %d picks:", c[0], c[1], c[2], c[3], g_widen, n);
        for (int i = 0; i < n; ++i)
            WLOG_INFO("scene-lights:   %s at (%.1f, %.1f, %.1f) key %.4f", picks[i].injected ? "injected" : "model   ",
                      picks[i].position[0], picks[i].position[1], picks[i].position[2], picks[i].key);
    }

    bool InstallSceneLights()
    {
        char policy[32] = {};
        if (wxl::config::Raw("WXL_LIGHT_POLICY", policy, sizeof policy))
        {
            if (_stricmp(policy, "stock") == 0) g_policy = WXL_LIGHT_POLICY_STOCK;
            else if (_stricmp(policy, "off") == 0) g_policy = WXL_LIGHT_POLICY_OFF;
        }

        bool ok = wxl::hook::Install("M2.SceneAnimate", off::kSceneAnimate, &hkAnimate, &g_origAnimate);
        ok = wxl::hook::Install("M2.SceneSelectLights", off::kSceneSelectLights, &hkSelectLights, &g_origSelectLights) && ok;
        ok = wxl::hook::Install("M2.AddLight", off::kAddLight, &hkAddLight, &g_origAddLight) && ok;
        if (!ok) return false;

        ev::Subscribe(ev::Event::OnWorldLeave, &OnWorldLeave, nullptr);
        ev::Subscribe(ev::Event::OnWorldSceneBegin, &OnWorldPassBegin, nullptr);
        ev::Subscribe(ev::Event::OnWorldSceneEnd, &OnWorldPassEnd, nullptr);
        wxl::runtime::extensions::PublishInterface("wxl.scenelights", WXL_SCENELIGHTS_API_VERSION,
                                                   const_cast<WXL_SceneLightsApi*>(&g_api));
        WLOG_INFO("scene-lights: selection policy %s (WXL_LIGHT_POLICY)", PolicyName(g_policy));

        g_diag = wxl::config::Env("WXL_DIAG_LIGHTS", false);
        if (g_diag)
        {
            ev::Subscribe(ev::Event::OnUpdate, &OnUpdateDiag, nullptr);
            WLOG_INFO("scene-lights: diag on (WXL_DIAG_LIGHTS): a warm orb follows the player");
        }
        return true;
    }
}

WXL_REGISTER_FEATURE("scene-lights", true, InstallSceneLights)
