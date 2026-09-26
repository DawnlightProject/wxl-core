// wxl-graphics-lights: light families and the blackbody colour of a temperature.
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

#include "Families.hpp"

#include "wxl/GraphicsLightsApi.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    namespace fm = wxl::gfx::lights::families;

    constexpr uint8_t kNone    = WXL_GFX_LIGHT_FLICKER_NONE;
    constexpr uint8_t kFire    = WXL_GFX_LIGHT_FLICKER_FIRE;
    constexpr uint8_t kCandle  = WXL_GFX_LIGHT_FLICKER_CANDLE;
    constexpr uint8_t kLantern = WXL_GFX_LIGHT_FLICKER_LANTERN;

    constexpr uint8_t kPlain     = WXL_GFX_LIGHT_PROFILE_NONE;
    constexpr uint8_t kCage      = WXL_GFX_LIGHT_PROFILE_CAGE;
    constexpr uint8_t kDownlight = WXL_GFX_LIGHT_PROFILE_DOWNLIGHT;
    constexpr uint8_t kFlame     = WXL_GFX_LIGHT_PROFILE_FLAME;
    constexpr uint8_t kGrille    = WXL_GFX_LIGHT_PROFILE_GRILLE;

    // Indexed by WXL_GFX_LIGHT_FAMILY_*.
    const fm::Family kFamilies[fm::kCount] = {
        { "tint",        0.0f,    {},                    4.0f,  0.10f, kNone,    kPlain,     0.0f },
        { "candle",      1850.0f, {},                    0.8f,  0.03f, kCandle,  kFlame,     0.5f },
        { "chandelier",  1950.0f, {},                    5.0f,  0.30f, kCandle,  kPlain,     0.2f },
        { "torch",       2000.0f, {},                    5.0f,  0.12f, kFire,    kFlame,     0.5f },
        { "fire",        1950.0f, {},                    5.0f,  0.30f, kFire,    kFlame,     0.5f },
        { "campfire",    1900.0f, {},                    10.0f, 0.50f, kFire,    kFlame,     0.5f },
        { "brazier",     1950.0f, {},                    8.0f,  0.35f, kFire,    kFlame,     0.5f },
        { "hearth",      1900.0f, {},                    7.0f,  0.30f, kFire,    kFlame,     0.5f },
        { "lantern",     2700.0f, {},                    3.5f,  0.04f, kLantern, kCage,      0.15f },
        { "streetlamp",  2500.0f, {},                    10.0f, 0.06f, kLantern, kDownlight, 0.15f },
        { "walllight",   2600.0f, {},                    4.0f,  0.05f, kLantern, kGrille,    0.15f },
        { "greenlamp",   0.0f,    { 0.45f, 1.0f, 0.72f }, 6.0f, 0.04f, kLantern, kCage,      0.0f },
        { "engine fire", 2000.0f, {},                    5.0f,  0.25f, kFire,    kFlame,     0.5f },
        { "engine lamp", 2700.0f, {},                    4.0f,  0.10f, kLantern, kPlain,     0.15f },
        { "given",       0.0f,    {},                    4.0f,  0.10f, kNone,    kPlain,     0.0f },
    };

    fm::Family g_live[fm::kCount];
    float      g_scale[fm::kCount];
    bool       g_ready = false;

    void EnsureReady()
    {
        if (g_ready) return;
        g_ready = true;
        for (uint32_t i = 0; i < fm::kCount; ++i)
        {
            g_live[i] = kFamilies[i];
            g_scale[i] = 1.0f;
        }
    }

    // CIE 1931 xy of the Planckian locus, Kim et al. (2002) cubic fits, 1667 K to 25000 K.
    void PlanckXy(float kelvin, double& x, double& y)
    {
        const double t = std::clamp(double(kelvin), 1667.0, 25000.0);
        const double t2 = t * t, t3 = t2 * t;
        x = t <= 4000.0 ? -0.2661239e9 / t3 - 0.2343589e6 / t2 + 0.8776956e3 / t + 0.179910
                        : -3.0258469e9 / t3 + 2.1070379e6 / t2 + 0.2226347e3 / t + 0.240390;
        const double x2 = x * x, x3 = x2 * x;
        if (t <= 2222.0)      y = -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x - 0.20219683;
        else if (t <= 4000.0) y = -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x - 0.16748867;
        else                  y = 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x - 0.37001483;
    }
}

namespace wxl::gfx::lights::families
{
    const Family& Get(uint32_t family)
    {
        EnsureReady();
        const uint32_t f = family < kCount ? family : WXL_GFX_LIGHT_FAMILY_TINT;
        g_live[f].intensity = kFamilies[f].intensity * g_scale[f];
        return g_live[f];
    }

    float& IntensityScale(uint32_t family)
    {
        EnsureReady();
        return g_scale[family < kCount ? family : WXL_GFX_LIGHT_FAMILY_TINT];
    }

    uint32_t ByName(const char* name)
    {
        if (!name) return WXL_GFX_LIGHT_FAMILY_TINT;
        // The engine and given families are not named in the table.
        for (uint32_t i = 1; i <= WXL_GFX_LIGHT_FAMILY_GREENLAMP; ++i)
            if (std::strcmp(name, kFamilies[i].name) == 0) return i;
        return WXL_GFX_LIGHT_FAMILY_TINT;
    }

    const char* Name(uint32_t family) { return family < kCount ? kFamilies[family].name : ""; }

    void Blackbody(float kelvin, float rgb[3])
    {
        double x = 0.0, y = 0.0;
        PlanckXy(kelvin, x, y);
        const double X = x / y, Y = 1.0, Z = (1.0 - x - y) / y;
        const double r = 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z;
        const double g = -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z;
        const double b = 0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z;
        rgb[0] = float(std::max(r, 0.0));
        rgb[1] = float(std::max(g, 0.0));
        rgb[2] = float(std::max(b, 0.0));
        const float l = Luma(rgb);
        for (int k = 0; k < 3; ++k) rgb[k] = l > 1e-6f ? rgb[k] / l : 1.0f;
    }

    void Adapt(float rgb[3], float share)
    {
        const float s = std::clamp(share, 0.0f, 1.0f);
        const float l = Luma(rgb);
        for (int k = 0; k < 3; ++k) rgb[k] += (l - rgb[k]) * s;
    }

    void TintOf(const float gamma[3], float rgb[3])
    {
        for (int k = 0; k < 3; ++k) rgb[k] = std::pow(std::max(gamma[k], 0.0f), 2.2f);
        const float l = Luma(rgb);
        for (int k = 0; k < 3; ++k) rgb[k] = l > 1e-6f ? rgb[k] / l : 1.0f;
    }

    bool Warm(const float c[3])
    {
        return c[0] > 0.05f && c[0] >= c[1] && c[1] >= c[2] * 0.9f && c[0] > 1.3f * c[2];
    }
}
