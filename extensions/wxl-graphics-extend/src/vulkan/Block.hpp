// wxl-graphics-extend: the compute pass registry and the per-frame compute block.
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

#include "wxl/GraphicsVulkanApi.h"

#include <cstddef>
#include <cstdint>

// Passes join for the process lifetime and are polled with the D3D9 passes; the block records every
// wanting one, in order, into the frame slot's command buffer and submits once. D3D9 textures are
// imported per block: moved to GENERAL when first asked for, handed back to DXVK's layout at the end.
namespace wxl::gfx::vulkan::block
{
    uint32_t AddComputePass(const WXL_GfxVkPassDesc* desc);
    uint32_t PollWants();
    void     Run(const WXL_GfxFrame& frame);

    int  ImportTexture(void* d3dResource, WXL_GfxVkImage* out);
    int  CopyToTexture(const WXL_GfxVkImage* source, void* d3dTexture);
    void CmdBarrier(VkCommandBuffer cmd);

    /// For the panel: one registered pass.
    struct PassInfo
    {
        const char* name;
        int32_t     order;
        uint32_t    lastWants;
        bool        ran;
        float       gpuMs;
    };
    size_t PassCount();
    bool   GetPassInfo(size_t index, PassInfo& out);

    /// For the panel: the last block's imports, and whether a block was submitted on the last frame.
    uint32_t LastImports();
    bool     LastBlockRan();
}
