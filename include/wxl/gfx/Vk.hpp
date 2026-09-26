// wxl::gfx::vk: the Vulkan device functions a compute pass records with, loaded from the service's
// context, and a small descriptor-set writer.
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

// Header-only; compiles into the consumer. Nothing links vulkan-1: every function comes from the
// context's getDeviceProcAddr, so a consumer that never finds DXVK never touches Vulkan at all.
//
//     wxl::gfx::vk::Device vk;
//     WXL_GfxVkContext ctx{ sizeof ctx };
//     if (vkApi->GetContext(&ctx) && vk.generation != ctx.generation && !vk.Load(ctx)) return;
namespace wxl::gfx::vk
{
#define WXL_GFX_VK_DEVICE_FUNCTIONS(X)   \
    X(vkCmdBindPipeline)                 \
    X(vkCmdBindDescriptorSets)           \
    X(vkCmdPushConstants)                \
    X(vkCmdDispatch)                     \
    X(vkCmdDispatchIndirect)             \
    X(vkCmdPipelineBarrier)              \
    X(vkCmdCopyImage)                    \
    X(vkCmdCopyBufferToImage)            \
    X(vkCmdCopyImageToBuffer)            \
    X(vkCmdCopyBuffer)                   \
    X(vkCmdClearColorImage)              \
    X(vkCmdFillBuffer)                   \
    X(vkCmdUpdateBuffer)                 \
    X(vkUpdateDescriptorSets)            \
    X(vkCreateBuffer)                    \
    X(vkDestroyBuffer)                   \
    X(vkGetBufferMemoryRequirements)     \
    X(vkAllocateMemory)                  \
    X(vkFreeMemory)                      \
    X(vkBindBufferMemory)                \
    X(vkMapMemory)                       \
    X(vkUnmapMemory)                     \
    X(vkCreateImageView)                 \
    X(vkDestroyImageView)

    /// The device functions above, loaded for one context generation.
    struct Device
    {
        uint32_t generation = 0;
        VkDevice device = VK_NULL_HANDLE;
#define WXL_GFX_VK_DECLARE(name) PFN_##name name = nullptr;
        WXL_GFX_VK_DEVICE_FUNCTIONS(WXL_GFX_VK_DECLARE)
#undef WXL_GFX_VK_DECLARE

        /// Loads every function; false (and generation 0) when one is missing.
        bool Load(const WXL_GfxVkContext& ctx)
        {
            generation = 0;
            device = ctx.device;
            if (!ctx.getDeviceProcAddr || !device) return false;
            bool ok = true;
#define WXL_GFX_VK_LOAD(name)                                                                     \
            name = reinterpret_cast<PFN_##name>(ctx.getDeviceProcAddr(device, #name));            \
            ok = ok && name != nullptr;
            WXL_GFX_VK_DEVICE_FUNCTIONS(WXL_GFX_VK_LOAD)
#undef WXL_GFX_VK_LOAD
            if (ok) generation = ctx.generation;
            return ok;
        }
    };

    /// Collects descriptor writes for one set, then applies them in one vkUpdateDescriptorSets.
    template <uint32_t Capacity = 16>
    class DescriptorWriter
    {
    public:
        explicit DescriptorWriter(VkDescriptorSet set) : set_(set) {}

        DescriptorWriter& Image(uint32_t binding, VkDescriptorType type, VkImageView view, VkImageLayout layout,
                                VkSampler sampler = VK_NULL_HANDLE)
        {
            if (count_ >= Capacity) { overflow_ = true; return *this; }
            images_[count_] = VkDescriptorImageInfo{ sampler, view, layout };
            VkWriteDescriptorSet& w = writes_[count_];
            w = VkWriteDescriptorSet{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set_;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pImageInfo = &images_[count_];
            ++count_;
            return *this;
        }

        /// A sampled image (VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) in the service's layout.
        DescriptorWriter& Sampled(uint32_t binding, const WXL_GfxVkImage& image)
        {
            return Image(binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, image.view, image.layout);
        }

        /// A storage image (VK_DESCRIPTOR_TYPE_STORAGE_IMAGE), always GENERAL.
        DescriptorWriter& Storage(uint32_t binding, const WXL_GfxVkImage& image)
        {
            return Image(binding, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, image.view, VK_IMAGE_LAYOUT_GENERAL);
        }

        DescriptorWriter& SamplerOnly(uint32_t binding, VkSampler sampler)
        {
            return Image(binding, VK_DESCRIPTOR_TYPE_SAMPLER, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, sampler);
        }

        DescriptorWriter& Buffer(uint32_t binding, VkDescriptorType type, VkBuffer buffer, VkDeviceSize offset,
                                 VkDeviceSize range)
        {
            if (count_ >= Capacity) { overflow_ = true; return *this; }
            buffers_[count_] = VkDescriptorBufferInfo{ buffer, offset, range };
            VkWriteDescriptorSet& w = writes_[count_];
            w = VkWriteDescriptorSet{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = set_;
            w.dstBinding = binding;
            w.descriptorCount = 1;
            w.descriptorType = type;
            w.pBufferInfo = &buffers_[count_];
            ++count_;
            return *this;
        }

        /// A uniform buffer range from WXL_GfxVulkanApi::AllocUniform.
        DescriptorWriter& Uniform(uint32_t binding, const WXL_GfxVkAlloc& alloc)
        {
            return Buffer(binding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, alloc.buffer, alloc.offset, alloc.size);
        }

        void Apply(const Device& vk) const
        {
            if (count_ && vk.vkUpdateDescriptorSets) vk.vkUpdateDescriptorSets(vk.device, count_, writes_, 0, nullptr);
        }

        /// False when more writes were asked for than Capacity (the extra ones were dropped).
        bool Complete() const { return !overflow_; }

    private:
        VkDescriptorSet        set_;
        uint32_t               count_ = 0;
        bool                   overflow_ = false;
        VkWriteDescriptorSet   writes_[Capacity] = {};
        VkDescriptorImageInfo  images_[Capacity] = {};
        VkDescriptorBufferInfo buffers_[Capacity] = {};
    };

    /// Workgroups needed to cover `size` items with `group`-sized groups.
    constexpr uint32_t Groups(uint32_t size, uint32_t group) { return (size + group - 1) / group; }

    /// The device's capabilities; false when not Available or when the service predates GetCaps.
    inline bool GetCaps(const WXL_GfxVulkanApi* api, WXL_GfxVkCaps& out)
    {
        out = WXL_GfxVkCaps{};
        out.structSize = sizeof out;
        if (!api || api->structSize < offsetof(WXL_GfxVulkanApi, GetCaps) + sizeof api->GetCaps || !api->GetCaps) return false;
        return api->GetCaps(&out) != 0;
    }

    /// True when every WXL_GFX_VK_CAP_* bit of `caps` is enabled on the device.
    inline bool Has(const WXL_GfxVkCaps& caps, uint64_t bits) { return (caps.enabled & bits) == bits; }
}
