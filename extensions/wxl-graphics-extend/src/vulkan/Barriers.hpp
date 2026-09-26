// wxl-graphics-extend: the barriers the Vulkan service records, spelled out once.
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

#include "Functions.hpp"

// Every pass and copy in a block runs on the compute and transfer stages only, so "everything a
// block may do" is these two stages with their reads and writes.
namespace wxl::gfx::vulkan::barriers
{
    constexpr VkPipelineStageFlags kBlockStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    constexpr VkAccessFlags        kBlockWrites = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    constexpr VkAccessFlags        kBlockAccess = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                                | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;

    /// Every earlier compute / transfer write made visible to every later compute / transfer access.
    inline void Global(const Functions& fn, VkCommandBuffer cmd)
    {
        VkMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        b.srcAccessMask = kBlockWrites;
        b.dstAccessMask = kBlockAccess;
        fn.vkCmdPipelineBarrier(cmd, kBlockStages, kBlockStages, 0, 1, &b, 0, nullptr, 0, nullptr);
    }

    /// A whole-image layout transition, every mip and layer of the given aspects.
    inline VkImageMemoryBarrier Image(VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                                      VkAccessFlags srcAccess, VkAccessFlags dstAccess)
    {
        VkImageMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        b.srcAccessMask = srcAccess;
        b.dstAccessMask = dstAccess;
        b.oldLayout = from;
        b.newLayout = to;
        b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange.aspectMask = aspect;
        b.subresourceRange.baseMipLevel = 0;
        b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        b.subresourceRange.baseArrayLayer = 0;
        b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        return b;
    }

    /// A D3D9 image taken into the block: whatever DXVK last did to it, at any stage, is complete
    /// and visible before the block's compute and transfer commands touch it.
    inline VkImageMemoryBarrier Acquire(VkImage image, VkImageAspectFlags aspect, VkImageLayout reported)
    {
        return Image(image, aspect, reported, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_MEMORY_WRITE_BIT, kBlockAccess);
    }

    /// Handed back: the block's writes are complete and visible to anything DXVK records after it.
    inline VkImageMemoryBarrier Release(VkImage image, VkImageAspectFlags aspect, VkImageLayout restore)
    {
        return Image(image, aspect, VK_IMAGE_LAYOUT_GENERAL, restore, kBlockWrites, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
    }

    /// A service image's first and only transition: nothing to wait for, GENERAL from here on.
    inline VkImageMemoryBarrier Initial(VkImage image)
    {
        return Image(image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, kBlockAccess);
    }
}
