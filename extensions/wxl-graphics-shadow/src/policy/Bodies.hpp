// wxl-graphics-shadow: the body capsules -- the player, NPCs and creatures nearest the camera, as
// vertical capsules from the feet to the head, for shadows of lamps without a map.
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

#include "wxl/GraphicsShadowApi.h"

#include <cstdint>

namespace wxl::gfx::shadow::bodies
{
    struct Capsule
    {
        float a[3];                 ///< world, the lower sphere's centre
        float b[3];                 ///< world, the upper sphere's centre
        float radius;
        unsigned long long guid;
        bool  player;               ///< the active player
    };

    /// Walks the units once a frame (render thread): the nearest WXL_GFX_SHADOW_CAPSULES to the eye
    /// within range.
    void Update(const float eye[3], float range, float radiusPerYard);

    int Count();
    const Capsule* List();

    /// The capsule of a unit by GUID, -1 when it is not listed.
    int Find(unsigned long long guid);

    /// The capsule whose axis passes nearest a point, within reach yards of its surface; -1 none.
    int Nearest(const float p[3], float reach);

    /// Capsules within reach of a light (radius plus a body), as a bit mask.
    uint32_t Mask(const float light[3], float radius);
}
