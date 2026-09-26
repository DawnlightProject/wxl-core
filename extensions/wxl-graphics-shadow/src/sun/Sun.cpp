// wxl-graphics-shadow: the sun, the moon and the engine's cascades.
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

#include "Sun.hpp"
#include "../core/Extension.hpp"
#include "../core/Settings.hpp"

#include "wxl/ShadowLightApi.h"
#include "wxl/gfx/Matrix.hpp"
#include "game/Shadows.hpp"
#include "game/Sky.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    namespace sky = wxl::game::sky;
    namespace sh  = wxl::game::shadows;
    namespace mx  = wxl::gfx::matrix;
    namespace su  = wxl::gfx::shadow::sun;
    namespace ws  = wxl::gfx::shadow;

    su::Bodies                g_bodies;
    const WXL_ShadowLightApi* g_light = nullptr;
    uint32_t                  g_lightMisses = 0;
    const int                 g_owner = 0;
    bool                      g_claimed = false;
    bool                      g_adjusting = false;
    bool                      g_failed = false;
    int                       g_renderedBody = -1;   // what the maps were last rendered along
    char                      g_status[224] = "sun: idle";

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

    /// The engine's direction in, ours out: the body that shines (the sun by day, the moon by night),
    /// its height lifted by a factor and floored at the minimum elevation. A body under the horizon
    /// leaves the engine's own.
    void __cdecl Adjust(float dir[3], const float[3], void*)
    {
        const ws::Settings& s = ws::Config();
        const su::Bodies& b = g_bodies;
        // Left alone, the engine renders along its own (lifted) sun: meaningful only by day.
        g_renderedBody = b.night ? -1 : 0;
        if (!b.valid || !s.enabled || !s.sun || !s.lowSun) return;
        const bool night = b.night != 0;
        if (night && !s.moon) return;
        const float* body = night ? b.toMoon : b.toSun;
        const float horizontal = std::sqrt(body[0] * body[0] + body[1] * body[1]);
        if (horizontal < 1e-4f) return;
        const float elevation = std::atan2(body[2], horizontal);
        if (elevation <= 0.01f) return;
        const float lifted = std::atan(std::tan(std::min(elevation, 1.55f)) * std::max(s.sunLift, 0.1f));
        const float floorRad = std::clamp(s.sunMinElevation, 1.0f, 89.0f) * 3.14159265f / 180.0f;
        const float e = std::min(std::max(lifted, floorRad), 89.0f * 3.14159265f / 180.0f);
        // The direction the light travels: away from the body, downwards.
        dir[0] = -body[0] / horizontal * std::cos(e);
        dir[1] = -body[1] / horizontal * std::cos(e);
        dir[2] = -std::sin(e);
        g_renderedBody = night ? 1 : 0;
    }

    void KeepAdjuster()
    {
        const ws::Settings& s = ws::Config();
        const bool want = s.enabled && s.sun && s.lowSun;
        if (g_failed) return;
        if (!g_light)
        {
            if (!want || (g_lightMisses++ & 63u) != 0) return;
            g_light = static_cast<const WXL_ShadowLightApi*>(ws::g_api->GetInterface("wxl.shadowlight", WXL_SHADOWLIGHT_API_VERSION));
            if (g_light && (g_light->structSize < sizeof(WXL_ShadowLightApi) || !g_light->Claim)) g_light = nullptr;
            if (!g_light) return;
        }
        if (want && !g_claimed)
        {
            g_claimed = g_light->Claim(&g_owner) != 0;
            if (!g_claimed)
            {
                g_failed = true;
                SHADOW_LOG_WARN("sun: another extension holds the shadow light; the cascades keep its direction");
                return;
            }
        }
        if (want == g_adjusting) return;
        if (want && !g_light->SetAdjustOwned(&g_owner, &Adjust, nullptr))
        {
            g_failed = true;
            SHADOW_LOG_WARN("sun: the core could not hook the shadow light; the cascades keep the engine's direction");
            return;
        }
        if (!want)
        {
            g_light->Release(&g_owner);
            g_claimed = false;
        }
        g_adjusting = want;
        SHADOW_LOG_INFO("sun: the cascades %s", want ? "follow a lower, truer sun (and the moon at night)" : "follow the engine's light again");
    }
}

namespace wxl::gfx::shadow::sun
{
    void Update(float dt)
    {
        KeepAdjuster();
        sky::CelestialLight in{};
        if (!sky::GetCelestialLight(in)) return;
        Bodies& b = g_bodies;
        const float a = b.valid ? 1.0f - std::exp(-std::max(dt, 0.0f) / 0.3f) : 1.0f;
        const float sunW = Smoothstep(-0.05f, 0.12f, in.toSun[2]);
        const float moonW = Smoothstep(-0.05f, 0.12f, in.toMoon[2]) * (1.0f - sunW);
        for (int i = 0; i < 3; ++i)
        {
            b.toSun[i] += (in.toSun[i] - b.toSun[i]) * a;
            b.toMoon[i] += (in.toMoon[i] - b.toMoon[i]) * a;
        }
        Normalize(b.toSun);
        Normalize(b.toMoon);
        b.sunWeight += (sunW - b.sunWeight) * a;
        b.moonWeight += (moonW - b.moonWeight) * a;
        // Night: the moon is up and higher than a sun under the horizon, with a small hysteresis.
        if (b.night) b.night = b.toSun[2] < 0.02f ? 1 : 0;
        else b.night = b.toSun[2] < -0.02f && b.toMoon[2] > 0.0f ? 1 : 0;
        b.valid = true;
    }

    const Bodies& Current() { return g_bodies; }

    bool ReadCascades(const float eye[3], Cascades& out)
    {
        out = Cascades{};
        sh::Snapshot snap;
        const bool valid = sh::Get(snap);
        out.mode = snap.mode;
        out.hwPcf = snap.hwPcf;
        out.size = snap.size;
        if (!valid || snap.mode < 1 || snap.hwPcf || snap.size == 0)
        {
            std::snprintf(g_status, sizeof g_status, "sun maps: %s (extShadowQuality %d)",
                          snap.hwPcf ? "hardware PCF maps, not readable" : (snap.mode < 1 ? "off" : "not rendered this frame"), snap.mode);
            return false;
        }
        for (int k = 0; k < 3; ++k) out.towardsLight[k] = -snap.lightDir[k];
        Normalize(out.towardsLight);
        // Which body they follow: the adjuster's choice, else the engine's (the sun).
        out.body = g_adjusting ? g_renderedBody : (g_bodies.night ? -1 : 0);
        // p_view = (r + eye - cameraPos, 1) * view: the translation row absorbs the eye's offset.
        float view[16];
        mx::Copy(snap.view, view);
        const float d[3] = { eye[0] - snap.cameraPos[0], eye[1] - snap.cameraPos[1], eye[2] - snap.cameraPos[2] };
        for (int j = 0; j < 4; ++j) view[12 + j] += d[0] * snap.view[j] + d[1] * snap.view[4 + j] + d[2] * snap.view[8 + j];
        const sh::Slot order[4] = { sh::Slot::Main, sh::Slot::Band0, sh::Slot::Band1, sh::Slot::Band2 };
        for (sh::Slot slot : order)
        {
            const sh::Map& m = snap.maps[size_t(slot)];
            if (!m.present || !m.texture || out.count >= 4) continue;
            const int n = out.count;
            out.textures[n] = m.texture;
            out.extent[n] = m.halfExtent;
            for (int k = 0; k < 3; ++k)
            {
                float* row = out.rows[n][k];
                for (int i = 0; i < 4; ++i)
                {
                    float v = 0.0f;
                    for (int j = 0; j < 3; ++j) v += view[i * 4 + j] * m.rows[k][j];
                    row[i] = v;
                }
                row[3] += m.rows[k][3];
            }
            ++out.count;
        }
        std::snprintf(g_status, sizeof g_status, "sun maps: mode %d, %d maps of %u, %s", snap.mode, out.count, snap.size,
                      out.body == 1 ? "rendered along the moon" : (out.body == 0 ? (g_adjusting ? "rendered along the adjusted sun" : "the engine's sun")
                                                                                : "not along a body (night, moon off)"));
        return out.count > 0;
    }

    void Shutdown()
    {
        if (g_light && g_claimed) g_light->Release(&g_owner);
        g_claimed = false;
        g_adjusting = false;
    }

    const char* Status() { return g_status; }
}
