// wxl-graphics-lights: the sun, the moon and the sky as light definitions.
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

#include "Sky.hpp"
#include "Families.hpp"

#include "game/Sky.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace
{
    namespace sk = wxl::game::sky;
    namespace fm = wxl::gfx::lights::families;

    WXL_GfxSkyLight g_sky{};
    float g_engineDiffuse[3] = {}, g_engineAmbient[3] = {}, g_engineToLight[3] = { 0.0f, 0.0f, 1.0f };
    char  g_status[200] = "sky: no world frame yet";

    float Lin(float c) { return std::pow(std::max(c, 0.0f), 2.2f); }

    void LinArgb(uint32_t argb, float out[3])
    {
        out[0] = Lin(float((argb >> 16) & 0xFF) / 255.0f);
        out[1] = Lin(float((argb >> 8) & 0xFF) / 255.0f);
        out[2] = Lin(float(argb & 0xFF) / 255.0f);
    }

    float Smoothstep(float a, float b, float x)
    {
        const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }

    /// The correlated colour temperature of a linear sRGB colour (McCamy's approximation).
    float Cct(const float rgb[3])
    {
        const double X = 0.4124 * rgb[0] + 0.3576 * rgb[1] + 0.1805 * rgb[2];
        const double Y = 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2];
        const double Z = 0.0193 * rgb[0] + 0.1192 * rgb[1] + 0.9505 * rgb[2];
        const double sum = X + Y + Z;
        if (sum <= 1e-6) return 0.0f;
        const double x = X / sum, y = Y / sum;
        const double n = (x - 0.3320) / (0.1858 - y);
        return float(std::clamp(449.0 * n * n * n + 3525.0 * n * n + 6823.3 * n + 5520.33, 1000.0, 40000.0));
    }
}

namespace wxl::gfx::lights::sky
{
    void Update(uint32_t frameIndex, float dt)
    {
        sk::CelestialLight in{};
        if (!sk::GetCelestialLight(in)) return;
        WXL_GfxSkyLight& s = g_sky;
        const bool first = !s.valid;
        const float a = first ? 1.0f : 1.0f - std::exp(-std::max(dt, 0.0f) / 0.3f);

        const float sunW = Smoothstep(-0.05f, 0.12f, in.toSun[2]);
        const float moonW = Smoothstep(-0.05f, 0.12f, in.toMoon[2]) * (1.0f - sunW);
        s.sunWeight += (sunW - s.sunWeight) * a;
        s.moonWeight += (moonW - s.moonWeight) * a;
        s.dayFactor = s.sunWeight;

        for (int k = 0; k < 3; ++k)
        {
            s.toSun[k] = in.toSun[k];
            s.toMoon[k] = in.toMoon[k];
            s.toLight[k] = -in.direction[k];
            s.diffuse[k] = Lin(in.diffuse[k]);
            s.ambient[k] = Lin(in.ambient[k]);
            g_engineDiffuse[k] = in.diffuse[k];
            g_engineAmbient[k] = in.ambient[k];
        }
        const float len = std::sqrt(s.toLight[0] * s.toLight[0] + s.toLight[1] * s.toLight[1] + s.toLight[2] * s.toLight[2]);
        for (int k = 0; k < 3; ++k)
        {
            if (len > 1e-6f) s.toLight[k] /= len;
            g_engineToLight[k] = s.toLight[k];
        }

        // One directional light shines at a time in the engine: its colour is the sun's by day, the moon's
        // by night, split by the bodies' weights through twilight.
        const float lum = fm::Luma(s.diffuse);
        float chroma[3] = { 1.0f, 1.0f, 1.0f };
        for (int k = 0; k < 3; ++k) chroma[k] = lum > 1e-5f ? s.diffuse[k] / lum : 1.0f;
        const float total = std::max(s.sunWeight + s.moonWeight, 1e-3f);
        for (int k = 0; k < 3; ++k)
        {
            s.sunColor[k] = chroma[k];
            s.moonColor[k] = chroma[k];
        }
        s.sunIlluminance = lum * s.sunWeight / total;
        s.moonIlluminance = lum * s.moonWeight / total;
        s.kelvinSun = Cct(s.diffuse);

        const sk::SkyColors sky = sk::GetSkyColors();
        LinArgb(sky.top, s.skyZenith);
        LinArgb(sky.horizon, s.skyHorizon);
        LinArgb(sk::FogColor(), s.fogColor);
        s.weather = 0.0f;   // no core weather binding yet (docs/design.md, section 12)

        s.structSize = sizeof s;
        s.frameIndex = frameIndex;
        s.valid = 1;
        std::snprintf(g_status, sizeof g_status, "sky: sun %.2f moon %.2f, light (%.2f %.2f %.2f) %.3f, ambient %.3f, about %.0f K",
                      s.sunWeight, s.moonWeight, s.diffuse[0], s.diffuse[1], s.diffuse[2], lum, fm::Luma(s.ambient), s.kelvinSun);
    }

    const WXL_GfxSkyLight& Current() { return g_sky; }

    void EngineColours(float diffuse[3], float ambient[3], float towardsLight[3])
    {
        for (int k = 0; k < 3; ++k)
        {
            diffuse[k] = g_engineDiffuse[k];
            ambient[k] = g_engineAmbient[k];
            towardsLight[k] = g_engineToLight[k];
        }
    }

    const char* Status() { return g_status; }
}
