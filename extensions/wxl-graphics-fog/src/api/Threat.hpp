// wxl-graphics-fog: the threat API (include/wxl/GraphicsFogApi.h, version 2 and 1): what gameplay
// feeds the fog, kept with its fades, and turned into simulation primitives once per frame.
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

#include "wxl/GraphicsFogApi.h"

namespace wxl::gfx::fog::threat
{
    /// Publishes the table under versions 2 and 1. Once, from WXL_Load.
    void Publish();

    /// The table itself (the panel's test buttons call it like any caller).
    const WXL_GraphicsFogApi* Table();

    /// Ages everything by the fog clock's step: fades, fronts' travel, pulses, the intensity's ease.
    void Tick(double clock, float dt);

    /// The global density multiplier: intensity times the pulses.
    float Intensity();

    /// The wind in effect: the profile's, blended towards the override while one is set.
    void Wind(const float profileDir[2], float profileSpeed, float dir[2], float& speed);

    /// Adds this frame's primitives for every live source, front, flow and volume.
    void EmitPrims(float renewal);

    int LightCount();
    const WXL_GfxFogLight* Lights();

    /// The live cascades on a map: two rows of four floats each (spill point xyz and width; fall xy,
    /// bank depth, weight), at most cap. Returns how many were written.
    int Cascades(int mapId, float* rows, int cap);

    /// One line for the panel: live counts, intensity, wind override.
    const char* Status();
}
