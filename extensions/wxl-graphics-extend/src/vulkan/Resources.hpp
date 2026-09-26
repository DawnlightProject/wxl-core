// wxl-graphics-extend: the objects the Vulkan service keeps for consumers -- images, pipelines,
// samplers and the shared textures as service images.
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

// Every object lives on the current Vulkan device and goes with it (Destroy). A service image is
// always in GENERAL: its first transition is recorded at once when a block is open, otherwise at
// the start of the next one (RecordPending), before any pass records.
namespace wxl::gfx::vulkan::resources
{
    int  CreateImage(const WXL_GfxVkImageDesc* desc, WXL_GfxVkImage* out);
    void DestroyImage(const WXL_GfxVkImage* image);
    int  UploadImage(const WXL_GfxVkImage* image, const void* data, size_t bytes);

    int  CreateComputePipeline(const WXL_GfxVkPipelineDesc* desc, WXL_GfxVkPipeline* out);
    void DestroyPipeline(const WXL_GfxVkPipeline* pipeline);

    VkSampler Sampler(uint32_t which);

    int  SharedTexture(uint32_t which, WXL_GfxVkImage* out);

    /// Whether the handle is a live service image (so it is in GENERAL and has TRANSFER_SRC).
    bool IsServiceImage(VkImage image);

    /// At the start of a block: the first transitions of new images, the shared textures' uploads.
    void RecordPending(VkCommandBuffer cmd);

    /// The device goes. With destroyObjects false the handles are only forgotten (the GPU may still
    /// be using them; see frames::Destroy).
    void Destroy(bool destroyObjects);

    void Stats(uint32_t& images, uint64_t& imageBytes, uint32_t& pipelines);
}
