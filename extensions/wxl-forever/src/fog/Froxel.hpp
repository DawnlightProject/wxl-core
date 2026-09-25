// wxl-forever fog: the froxel volume -- injection, temporal resolve, integration and full-resolution apply.
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
#include "FogProfile.hpp"
#include "../lights/Lights.hpp"

struct IDirect3DDevice9;
struct IDirect3DTexture9;

namespace wxl::forever::fog::froxel
{
    /// Everything one frame of the volume needs from the caller.
    struct FrameInput
    {
        IDirect3DDevice9*    device;
        IDirect3DTexture9*   depth;          // the world's INTZ depth
        unsigned             depthW, depthH;
        float                viewProjRel[16]; // camera-relative view * projection
        float                eye[3];
        float                seconds;
        float                rangeMin, rangeMax;
        ResolvedProfile      outdoor, indoor;
        bool                 cameraIndoor;   // whether the camera stands inside an interior box now
        float                indoorness;     // the same, eased over the indoor/outdoor transition (0..1)
        ResolvedProfile      camera;         // outdoor and indoor mixed by indoorness: exposure, far distance, near effects
        const float          (*boxRows)[4];  // 3 rows per interior box
        int                  boxCount;
        const lights::Light* lights;         // world space, at most lights::kMaxLights
        int                  lightCount;
        const inputs::Volume* volumes; // world space, at most inputs::kMaxVolumes
        int                  volumeCount;
        float                farDistance;    // last slice distance, yards
        IDirect3DTexture9*   ground;         // terrain heightfield (terrain.hpp); null keeps the absolute base
        float                groundFallback; // ground height where no cell is measured
        bool                 trails;         // projectile trails published this frame (Projectiles.hpp)
    };

    /// Draws the fog over the bound target; false when the volume is unavailable (yet) on this device.
    bool Render(const FrameInput& in);

    /// Frees the DEFAULT-pool targets and queries; recreated on next use.
    void ReleaseResources();

    /// One line for the panel: grid, format, GPU time.
    const char* Status();

    /// One line for the panel: each pass's GPU time in milliseconds.
    const char* PassTimes();

    /// Counts, on the next frame, how many froxels leave injection by each path; the result reaches
    /// Census() and the log a few frames later.
    void RequestCensus();
    const char* Census();

    /// One line for the panel: the celestial inputs as fed, and their instability over the last second.
    const char* Diagnostics();
}
