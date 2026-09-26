// wxl-graphics-lights: the sun, the moon and the sky as light definitions, from the engine's day-night
// state this frame (docs/design.md, section 7).
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

#include "wxl/GraphicsLightsSourcesApi.h"

#include <cstdint>

namespace wxl::gfx::lights::sky
{
    /// Once per frame, from the light service's frame: reads the engine's celestial light, sky and fog
    /// colours, eases the bodies' weights over a third of a second.
    void Update(uint32_t frameIndex, float dt);

    /// This frame's definition; valid is 0 before the first world frame.
    const WXL_GfxSkyLight& Current();

    /// The engine's own lighting colours this frame, gamma, as its shaders use them (for the sun factor).
    void EngineColours(float diffuse[3], float ambient[3], float towardsLight[3]);

    const char* Status();
}
