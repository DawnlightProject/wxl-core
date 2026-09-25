// sky bindings: the day-night state the engine computed for this frame.
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

#pragma once

#include <cmath>
#include <cstdint>

#include "game/Binding.hpp"
#include "offsets/engine/Sky.hpp"

/// Read-only views of the day-night state, valid while a world is up.
namespace wxl::game::sky
{
    namespace off = wxl::offsets::engine::sky;

    /**
     * @brief Reads the fog colour the day-night update computed for this frame.
     * @return the colour as 0xAARRGGBB.
     */
    inline uint32_t FogColor()
    {
        return *reinterpret_cast<const volatile uint32_t*>(off::kFogColor);
    }

    /// The sky's gradient this frame, each as 0xAARRGGBB: zenith, middle, horizon.
    struct SkyColors
    {
        uint32_t top;
        uint32_t middle;
        uint32_t horizon;
    };

    /// Reads the sky gradient the day-night update resolved for this frame.
    inline SkyColors GetSkyColors()
    {
        const auto* colors = reinterpret_cast<const volatile uint32_t*>(off::kSkyColors);
        return { colors[off::kSkyColorTop], colors[off::kSkyColorMiddle], colors[off::kSkyColorHorizon] };
    }

    /// Non-zero while the sky may draw; an override that only moves the fog passes it back unchanged.
    inline uint32_t SkyDrawGate()
    {
        return *reinterpret_cast<const volatile uint32_t*>(off::kSkyDrawGate);
    }

    /// True while some caller has the fog overridden.
    inline bool OverrideFogActive()
    {
        return *reinterpret_cast<const volatile uint32_t*>(off::kOverrideFogActive) != 0;
    }

    /**
     * @brief Replaces the zone's distance fog until ClearOverrideFog.
     * @param startFraction  fog starts at endDistance * startFraction.
     * @param endDistance    fog end, capped by the engine's own distance ceiling.
     * @param colorArgb      fog colour.
     * @param skyGate        written to the sky-draw gate; pass SkyDrawGate() to leave the sky drawing.
     *
     * The live fog is saved on the first call only, so calling it again every frame is safe.
     */
    inline void SetOverrideFog(float startFraction, float endDistance, uint32_t colorArgb, uint32_t skyGate)
    {
        Native<off::SetOverrideFogFn>(off::kSetOverrideFog)(startFraction, endDistance, colorArgb, skyGate);
    }

    /// Restores the fog the first SetOverrideFog saved.
    inline void ClearOverrideFog()
    {
        Native<off::ClearOverrideFogFn>(off::kClearOverrideFog)();
    }

    /// The active celestial light (sun or moon) as the engine lights its models with it this frame.
    struct CelestialLight
    {
        float direction[3];  // lighting direction as the engine stores it (points down)
        float diffuse[3];    // rgb 0..1, as the engine lights with it (glare-dimmed)
        float ambient[3];    // rgb 0..1, glare-dimmed likewise
        float toSun[3];      // unit vector towards the drawn sun
        float toMoon[3];     // unit vector towards the drawn moon
        float glare;         // 0..1, the sun glare the colours above were dimmed by
        float glareDimming;  // the factor: colours carry (1 - glareDimming * glare)
    };

    /**
     * @brief Reads the celestial light from the day-night info block.
     * @param out  receives the light.
     * @return false when no info block is available.
     */
    inline bool GetCelestialLight(CelestialLight& out)
    {
        const auto* info = static_cast<const uint8_t*>(Native<off::DayNightGetInfoFn>(off::kDayNightGetInfo)());
        if (!info) return false;
        const float* dir = reinterpret_cast<const float*>(info + off::kInfoLightDir);
        for (int i = 0; i < 3; ++i) out.direction[i] = dir[i];
        // Bytes are B, G, R.
        for (int i = 0; i < 3; ++i)
        {
            out.diffuse[i] = float(info[off::kInfoDiffuseColor + 2 - i]) / 255.0f;
            out.ambient[i] = float(info[off::kInfoAmbientColor + 2 - i]) / 255.0f;
        }

        const float* origin = reinterpret_cast<const float*>(info + off::kInfoSkyOrigin);
        auto towards = [origin](const float* pos, float o[3]) {
            float v[3] = { pos[0] - origin[0], pos[1] - origin[1], pos[2] - origin[2] };
            float len = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
            len = len > 1e-12f ? 1.0f / std::sqrt(len) : 0.0f;
            for (int i = 0; i < 3; ++i) o[i] = v[i] * len;
        };
        towards(reinterpret_cast<const float*>(info + off::kInfoSunPosition), out.toSun);
        towards(reinterpret_cast<const float*>(info + off::kInfoMoonPosition), out.toMoon);

        out.glare        = *reinterpret_cast<const volatile float*>(off::kSunGlare);
        out.glareDimming = off::kGlareDimming;
        return true;
    }
}
