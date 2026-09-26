// wxl-graphics-fog: the frame's simulation primitives -- everything that acts on the fog at a place:
// the threat API's sources, fronts and flows, the bodies' wakes, the missiles' tunnels, the plumes of
// smoke and the heat of fires. Rebuilt every frame on the CPU, uploaded as rows (shaders/sim.hlsli,
// Prim) with a bin mask per level cell block, and applied by the clipmap steps.
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

#include "../../shaders/shared.h"

#include <cstdint>

namespace wxl::gfx::fog::prims
{
    struct Prim
    {
        float rows[5][4] = {};
        float lo[3] = {}, hi[3] = {};   ///< world bounds, for the bins
        float size = 0.0f;              ///< its smallest feature, yards: coarse levels skip small ones
    };

    /// Starts the frame's list.
    void Begin();

    /// Adds one primitive; false when the list is full (FOG_MAX_PRIMS).
    bool Add(const Prim& p);

    int         Count();
    const Prim* List();

    // Builders. Positions are world space; the rows' meaning is sim.hlsli's.

    /// A sphere, box (yawed), capsule (a to b) or vertical cylinder of fog added (rate > 0, extinction
    /// per yard per second), carved (rate < 0, share per second) or held at a density (rate 0, hold).
    Prim Sphere(const float c[3], float radius);
    Prim Box(const float c[3], const float half[3], float yaw);
    Prim Capsule(const float a[3], const float b[3], float radius);
    Prim Cylinder(const float base[3], float radius, float height);
    /// A front: a wall of fog behind a line moving along dir (horizontal), `height` over the ground,
    /// `depth` deep, halfWidth wide (0 = unbounded).
    Prim Front(const float point[3], const float dir[2], float halfWidth, float height, float depth);
    /// A closing ring: fog outside a circle of radius around centre, `depth` thick, `height` high.
    Prim Ring(const float centre[3], float radius, float height, float depth);

    void SetDensity(Prim& p, float rate, float smoke, float hold, float soft, float billow);
    void SetFlow(Prim& p, const float push[3], float radial, float swirl);
}
