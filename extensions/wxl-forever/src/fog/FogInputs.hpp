// wxl-forever fog: what the froxel volume takes from the outside: point lights and local density volumes.
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

#include <cstdint>

// Both lists are filled per frame by whoever owns the data (a light-gathering API, zone scripts),
// then read once by the injection pass. Anything past the capacity is dropped, nearest kept.
namespace wxl::forever::fog::inputs
{
    constexpr int kMaxLights  = 8;     // lights another extension may feed
    constexpr int kMaxVolumes = 20;

    /// A point light scattering into the fog; a spot is a point with a cone.
    struct Light
    {
        float position[3];     // world space
        float radius;          // influence radius, yards; nothing past it
        float color[3];        // rgb 0..1
        float intensity;       // multiplies color
        float direction[3];    // spot axis, world space; ignored for a point
        float cosCone;         // cosine of the spot half-angle; <= -1 makes it a point light
        float innerRadius;     // full colour up to here, fading to nothing at radius
        int   interior;        // 1 lights only indoor fog, 0 only outdoor (set from the interior boxes)
    };

    enum class VolumeShape : int { Sphere = 0, Box = 1, Capsule = 2, Segment = 3, Plume = 4 };

    /// Adds density inside a shape, or carves the fog away: fog banks, graveyards, swamp pockets,
    /// bodies walking through.
    struct DensityVolume
    {
        int   shape;           // VolumeShape
        float center[3];       // world space; a capsule's is its bottom (feet)
        float extent[3];       // sphere: x = radius; box: half-sizes; capsule: x = radius, z = height
        float density;         // added: extinction per yard at the core; carved: fraction removed 0..1
        float falloff;         // 0 hard edge .. 1 fades from the centre
        int   carve;           // 0 adds density, 1 removes that fraction of whatever fog is there
        float swirl;           // 0..1 local churn of the noise inside it (a wake's turbulence)
    };

    /// A volume as the fog itself builds them (not part of the published interface): the published
    /// shapes, plus the segment projectiles carve with. A segment's center is one end, its extent the
    /// vector to the other end, radius its thickness; its density is the share carved at the center
    /// end, less towards the other, and is not scaled by the profile's wake strength.
    struct Volume : DensityVolume
    {
        float radius = 0.0f;
    };

    /// Replaces this frame's lights. Called on the game thread before the world draws.
    void SetLights(const Light* lights, int count);

    /// Replaces this frame's density volumes.
    void SetDensityVolumes(const DensityVolume* volumes, int count);

    int LightCount();
    const Light* Lights();
    int VolumeCount();
    const DensityVolume* Volumes();

    /// C table published as "wxl.forever.fog.inputs" v2 so another extension can feed the lists.
    struct InputsApi
    {
        void(__cdecl* SetLights)(const Light* lights, int count);
        void(__cdecl* SetDensityVolumes)(const DensityVolume* volumes, int count);
    };

    /// Publishes InputsApi through the core.
    void Publish();
}
