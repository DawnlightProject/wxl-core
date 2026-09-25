// wxl-forever fog: bodies moving through the fog push it away and leave a short fading wake.
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

#include "FogInputs.hpp"

// Units near the player (players and creatures) are sampled on the game tick. Each becomes a
// carving capsule from its feet to its head, and a moving one drops a few past positions that fade
// and shrink over the trail time. Both reach the volume as carving density volumes.
namespace wxl::forever::fog::wakes
{
    constexpr int kMaxBodies = 16;

    /// Samples the units within range of the player; call on the tick.
    void Update(float dt, const float player[3], bool havePlayer, float range, float trailSeconds);

    /**
     * @brief Writes the carving volumes, bodies first and nearest the camera first.
     * @param radius  capsule radius, yards.
     * @return how many were written, at most cap.
     */
    int Emit(const float eye[3], float radius, float trailSeconds, inputs::Volume* out, int cap);

    /// Bodies tracked on the last tick.
    int Tracked();
}
