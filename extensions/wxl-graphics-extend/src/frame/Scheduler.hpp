// wxl-graphics-extend: the post-world pass scheduler and the world pass's shared targets.
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

#include <cstddef>
#include <cstdint>

// Before each world pass the scheduler polls every pass's wants. With none, the world pass is left
// untouched (no redirect, no target, no callback). Otherwise it supplies the union -- INTZ depth,
// G-buffer normals, HDR colour -- runs the wanting passes' begins, and once the world is drawn runs
// their draws in order on one shared WXL_GfxFrame (see GraphicsExtendApi.h for every guarantee).
namespace wxl::gfx::frame
{
    /// Subscribes to the world pass events. Called once from WXL_Load.
    void Install();

    /// Releases every DEFAULT-pool resource (INTZ depth, normals, HDR colour, GPU queries) before a
    /// device reset; the device is probed again on the next world pass.
    void OnDeviceLost();

    uint32_t AddPass(const WXL_GfxPassDesc* desc);
    int      CurrentFrame(WXL_GfxFrame* out);
    float    PassGpuMs(uint32_t passId);
    int      SetEngineLightBuffer(void* texture, const float rows[16], const float params[4]);

    /// Fills caps, frameIndex, lastRequested, lastAvailable, passCount and depthStatus.
    void FillStatus(WXL_GfxStatus* out);

    /// The last world render target size seen; false before the first world pass.
    bool WorldSize(uint32_t& width, uint32_t& height);

    /// For the panel: one registered pass.
    struct PassInfo
    {
        const char* name;       // owned by the scheduler, valid for the process
        int32_t     order;
        uint32_t    lastWants;  // what it asked for on the last polled frame
        bool        ran;        // whether its draw ran on the last frame
        float       gpuMs;      // smoothed, -1 when unknown
    };
    size_t PassCount();
    bool   GetPassInfo(size_t index, PassInfo& out);

    /// For the panel: whether GPU timing is on (WXL_GFX_PROFILE, default 1) and the frame total.
    bool  ProfilingEnabled();
    float TotalGpuMs();
}
