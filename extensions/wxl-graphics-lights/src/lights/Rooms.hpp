// wxl-graphics-lights: the interior rooms near the camera, for indoor fog and for keeping a lamp's light in its room.
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

#include "wxl/GraphicsLightsApi.h"

#include <cstdint>

// Every placed map object within reach of the camera (core lights::CollectWmoPlacements), drawn or
// not, gives all of its interior groups (its root's group table), each an oriented box (the group's
// model-space bounds under the inverse of its placement): what the camera looks at never changes
// which rooms exist. A working set of at most kMaxRooms is kept by stable identity (placement and
// group), with hysteresis: a room joins within 45 yd and leaves past 65 yd (or after a second
// uncollected), a newcomer displaces a kept room only when 15 yd nearer, and every room fades in and
// out over 0.3 s (Weights). It is published smallest first, so the first box holding a point is its
// most specific room: an upstairs bedroom before the double-height hall around it.
//
// A room's height range at a point comes from its box's vertical row: WMO instances stand upright,
// so model z is the world's.
namespace wxl::gfx::lights::rooms
{
    constexpr int   kMaxRooms = WXL_GFX_LIGHTS_ROOMS_MAX;
    constexpr float kPad      = 0.3f;   // yards added around each box so wall surfaces test inside

    /// Announces the source of the rooms. Called once at load.
    void Install();

    /**
     * @brief Builds this frame's rooms from the placed map objects around the camera; once per frame.
     * @param eye  camera world position.
     * @return the number of rooms, at most kMaxRooms.
     */
    int Build(const float eye[3]);

    /// 3 float4 rows per room, smallest room first: q = dot(float4(r, 1), row), inside when every
    /// |q| <= 1, r a point relative to the camera. Padded by kPad.
    const float (*Rows())[4];

    /// Each published room's weight, 0..1: a room fades in over 0.3 s on joining the working set and out
    /// on leaving it, so a gate built on it blends rather than flips.
    const float* Weights();
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
     * @param inset  yards the point must lie inside a box horizontally, from its padded faces: 0 takes
     *               the padding as part of the room (a wall's surface tests inside), kPad asks for the
     *               group's own bounds (a lamp just outside a wall is not in the room behind it).
     * @return the room's index, or -1 outside every room.
     */
    int RoomOf(const float r[3], float& lo, float& hi, float inset = 0.0f);

    /**
     * @brief A settled room holding a camera-relative point: the smallest one fully faded in, else the
     *        one most faded in. What a lamp is classified by: a group only joining the working set
     *        never takes a lamp over while its weight is still low, and one leaving gives the lamp up
     *        to another holding it. Same parameters and result as RoomOf.
     */
    int SettledRoomOf(const float r[3], float& lo, float& hi, float inset);

    /// Whether room b holds the camera-relative point r, `inset` yards inside its padded faces
    /// horizontally and within its floor and ceiling (as RoomOf tests), whatever its weight.
    bool Contains(int b, const float r[3], float inset);

    /// The rooms a light of room b at the camera-relative point r may light (bit o for room o; 0 when
    /// b is no room): its own, and those whose height span holds the light (a quarter yard of slack)
    /// and whose floor lies within its own room's height (a yard of slack). An upstairs bedroom holds
    /// no downstairs lamp; the hall under a bedroom's lamp has its floor far below the bedroom's; the
    /// hall's own gallery, a vestibule and rooms side by side pass both.
    uint32_t Mask(int b, const float r[3]);

    /// Room b's unpadded floor and ceiling at the camera-relative point r, wherever r lies; false
    /// for a box that does not stand upright.
    bool Height(int b, const float r[3], float& lo, float& hi);

    /// Room b's stable identity: its placement and group. False when b is out of range.
    bool Identity(int b, const void*& owner, uint32_t& group);

    /// The index this frame of the room with that identity; -1 when it is not published.
    int IndexOf(const void* owner, uint32_t group);
}
