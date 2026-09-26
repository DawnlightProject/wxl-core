// wxl-graphics-fog: one frame of the fog on DXVK's Vulkan device. The orchestration (Fog.cpp) hands
// what it read from the game in a FrameInput; Record plans the simulation steps, builds the constants
// and records every pass into the frame's command buffer, then the copies the D3D9 composite reads.
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

#include "wxl/GraphicsExtendApi.h"
#include "wxl/GraphicsVulkanApi.h"

#include <cstdint>

namespace wxl::gfx::fog::gpu
{
    /// What the game side read this frame.
    struct FrameInput
    {
        const WXL_GfxFrame* frame = nullptr;
        float  eye[3] = {};
        double clock = 0.0;            ///< the fog's clock, seconds (monotonic, capped steps)
        float  dt = 0.0f;              ///< that clock's step this frame
        bool   jumped = false;         ///< the camera teleported: the simulation starts over
        float  groundFallback = 0.0f;  ///< the ground under the camera, where no tile is baked

        // Sun, moon and sky: directions towards them, weights 0..1, linear colours.
        float  toSun[3] = { 0, 0, 1 }, toMoon[3] = { 0, 0, 1 };
        float  sunWeight = 0.0f, moonWeight = 0.0f;
        float  lightRgb[3] = {};
        float  skyTop[3] = {}, skyHorizon[3] = {}, zoneFog[3] = {};

        // The camera's own medium.
        float  indoorEased = 0.0f, indoorActual = 0.0f;

        // The wind in effect (the profile's, or the threat API's override), and the global density.
        float  windDir[2] = { 1, 0 };
        float  windSpeed = 0.0f;
        float  intensity = 1.0f;

        // The wake fluid: the point it centres on (the player, else the camera) and what is splatted
        // into it this frame, two rows each (shaders/wake.cs.hlsl), nearest first.
        float        wakeCentre[3] = {};
        const float* splats = nullptr;
        int          splatCount = 0;

        // The map loaded (for the API's cascades).
        int          mapId = -1;

        // Bodies near the camera that shadow lamps in the fog: world feet xyz and height each.
        const float* occluders = nullptr;
        int          occluderCount = 0;
        const uint32_t* occluderMasks = nullptr;   ///< per lamp of this frame's list, the bodies near it
        int          maskCount = 0;
    };

    /// Before the block (the compute pass's wants): device, pipelines and images, so everything made
    /// here is transitioned by the service at the block's start. False while the fog cannot run.
    bool Prepare(const WXL_GfxVulkanApi* api);

    /// Records the frame; false when nothing could be composited this frame.
    bool Record(const WXL_GfxVkFrame& vk, const FrameInput& in);

    /// Composites over render target 0 when Record ran for this frame (the D3D9 pass).
    bool Apply(const WXL_GfxFrame& frame);

    /// Before a device reset: the D3D9 side's DEFAULT-pool textures go.
    void OnDeviceLost();

    /// Panel lines.
    const char* Status();
    const char* PassTimes();
    const char* SimStatus();

    /// Starts the simulation over at the next frame (levels to their targets, the rivers re-settled).
    void Restart();
}
