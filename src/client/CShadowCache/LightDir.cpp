// Shadow light: lets one subscriber adjust the direction CShadowCache renders its sun maps along,
// through a detour on CShadowCache::PreUpdate (0x00875C10). "wxl.shadowlight" v2 (also published as v1).
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

#include "common/Log.hpp"
#include "engine/hook/Hook.hpp"
#include "engine/hook/Registry.hpp"
#include "game/Sky.hpp"
#include "offsets/engine/Shadows.hpp"
#include "runtime/Extensions.hpp"
#include "wxl/ShadowLightApi.h"

#include <cmath>
#include <cstring>

namespace
{
    namespace off = wxl::offsets::engine::shadows;

    WXL_ShadowLightAdjustFn g_adjust = nullptr;
    void*                   g_user   = nullptr;
    off::PreUpdateFn        g_original = nullptr;
    bool                    g_hooked = false;
    bool                    g_hookFailed = false;
    float                   g_last[3] = {};
    bool                    g_haveLast = false;
    const void*             g_owner = nullptr;   // the claim's holder; null while free

    void __cdecl PreUpdateDetour(const float* lightDir, const float* cameraPos)
    {
        float dir[3] = { lightDir[0], lightDir[1], lightDir[2] };
        if (g_adjust)
        {
            float sun[3] = { 0.0f, 0.0f, 0.0f };
            wxl::game::sky::CelestialLight light{};
            if (wxl::game::sky::GetCelestialLight(light)) std::memcpy(sun, light.direction, sizeof sun);
            g_adjust(dir, sun, g_user);
            const float l = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
            // A degenerate or upward direction falls back to the engine's own.
            if (!(l > 1e-6f) || dir[2] / l > -0.02f) std::memcpy(dir, lightDir, sizeof dir);
            else for (float& d : dir) d /= l;
        }
        std::memcpy(g_last, dir, sizeof g_last);
        g_haveLast = true;
        g_original(dir, cameraPos);
    }

    bool EnsureHook()
    {
        if (g_hooked) return true;
        if (g_hookFailed) return false;
        if (!wxl::hook::Install("CShadowCache::PreUpdate", off::kPreUpdate, &PreUpdateDetour, &g_original) ||
            !wxl::hook::Enable(off::kPreUpdate) || !g_original)
        {
            g_hookFailed = true;
            WLOG_WARN("shadow-light: CShadowCache::PreUpdate could not be hooked; the engine keeps its direction");
            return false;
        }
        g_hooked = true;
        WLOG_INFO("shadow-light: PreUpdate hooked, an adjuster may change the shadow light direction");
        return true;
    }

    int Apply(WXL_ShadowLightAdjustFn fn, void* user)
    {
        if (!fn) { g_adjust = nullptr; g_user = nullptr; return 1; }
        if (!EnsureHook()) return 0;
        g_user   = user;
        g_adjust = fn;
        return 1;
    }

    int __cdecl ApiSetAdjust(WXL_ShadowLightAdjustFn fn, void* user)
    {
        if (g_owner) return 1;   // claimed: accepted, ignored
        return Apply(fn, user);
    }

    int __cdecl ApiClaim(const void* owner)
    {
        if (!owner || (g_owner && g_owner != owner)) return 0;
        if (!g_owner)
        {
            g_owner = owner;
            g_adjust = nullptr;   // the previous adjuster goes; the owner installs its own
            g_user = nullptr;
            WLOG_INFO("shadow-light: claimed; other adjusters are ignored until it is released");
        }
        return 1;
    }

    void __cdecl ApiRelease(const void* owner)
    {
        if (!owner || g_owner != owner) return;
        g_owner = nullptr;
        g_adjust = nullptr;
        g_user = nullptr;
    }

    int __cdecl ApiSetAdjustOwned(const void* owner, WXL_ShadowLightAdjustFn fn, void* user)
    {
        if (g_owner && g_owner != owner) return 0;
        return Apply(fn, user);
    }

    int __cdecl ApiGetDirection(float dir[3])
    {
        if (!g_haveLast || !dir) return 0;
        std::memcpy(dir, g_last, sizeof g_last);
        return 1;
    }

    const WXL_ShadowLightApi g_api = {
        sizeof(WXL_ShadowLightApi), WXL_SHADOWLIGHT_API_VERSION, &ApiSetAdjust, &ApiGetDirection,
        &ApiClaim, &ApiRelease, &ApiSetAdjustOwned,
    };

    bool InstallShadowLight()
    {
        // The same table under v1 as well: a v1 caller reads only the fields it knows.
        wxl::runtime::extensions::PublishInterface("wxl.shadowlight", WXL_SHADOWLIGHT_API_VERSION,
                                                   const_cast<WXL_ShadowLightApi*>(&g_api));
        wxl::runtime::extensions::PublishInterface("wxl.shadowlight", 1, const_cast<WXL_ShadowLightApi*>(&g_api));
        return true;
    }
}

WXL_REGISTER_FEATURE("shadow-light", true, InstallShadowLight)
