// wxl-forever lights: the interior rooms near the camera, for indoor fog and for keeping a lamp's light in its room.
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

// Every placed map object within reach of the camera (core lights::CollectWmoPlacements), drawn or
// not, gives all of its interior groups (its root's group table), each an oriented box (the group's
// model-space bounds under the inverse of its placement): what the camera looks at never changes
// which rooms exist. The nearest kMaxRooms are kept (last frame's counted a little nearer), smallest
// first, so the first box holding a point is its most specific room: an upstairs bedroom before the
// double-height hall around it.
//
// A room's height range at a point comes from its box's vertical row: WMO instances stand upright,
// so model z is the world's.
namespace wxl::forever::lights::rooms
{
    constexpr int   kMaxRooms = 12;
    constexpr float kPad      = 0.3f;   // yards added around each box so wall surfaces test inside

    /// Announces the source of the rooms. Called once at load.
    bool Install();

    /**
     * @brief Builds this frame's rooms from the placed map objects around the camera; once per frame.
     * @param eye  camera world position.
     * @return the number of rooms, at most kMaxRooms.
     */
    int Build(const float eye[3]);

    /// 3 float4 rows per room, smallest room first: q = dot(float4(r, 1), row), inside when every
    /// |q| <= 1, r a point relative to the camera. Padded by kPad.
    const float (*Rows())[4];
    int  Count();
    bool CameraIndoor();

    /// Interior groups within reach in the last Build, before the kMaxRooms cut.
    int Collected();

    /// Rooms that entered the list since the last call (a diagnostic of how stable the list is).
    int Changes();

    /**
     * @brief The smallest room holding a camera-relative point and its height range there.
     * @param r      the point, relative to the camera.
     * @param lo,hi  receive the room's unpadded floor and ceiling heights at r, camera-relative.
     * @return the room's index, or -1 outside every room.
     */
    int RoomOf(const float r[3], float& lo, float& hi);

    /// Room b's unpadded floor and ceiling at the camera-relative point r, wherever r lies; false
    /// for a box that does not stand upright.
    bool Height(int b, const float r[3], float& lo, float& hi);
}
