// wxl-graphics-extend: the Vulkan functions the service calls, loaded per device from the loader.
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

#include "wxl/GraphicsVulkanApi.h"   // VK_NO_PROTOTYPES, then vulkan_core.h

// Nothing links vulkan-1: every function comes from vkGetInstanceProcAddr (found in the loader DXVK
// already loaded) and vkGetDeviceProcAddr, for the device DXVK created. Core 1.0 and 1.1 only, and
// DXVK needs 1.3, so any device it runs on has them all; a missing one still fails the load rather
// than crash later.
namespace wxl::gfx::vulkan
{
#define WXL_VK_INSTANCE_FUNCTIONS(X)              \
    X(vkGetPhysicalDeviceProperties)              \
    X(vkGetPhysicalDeviceProperties2)             \
    X(vkGetPhysicalDeviceFeatures2)               \
    X(vkGetPhysicalDeviceMemoryProperties)        \
    X(vkGetPhysicalDeviceQueueFamilyProperties)   \
    X(vkGetPhysicalDeviceFormatProperties)        \
    X(vkGetPhysicalDeviceFormatProperties2)       \
    X(vkEnumerateDeviceExtensionProperties)

#define WXL_VK_DEVICE_FUNCTIONS(X)   \
    X(vkCreateCommandPool)           \
    X(vkDestroyCommandPool)          \
    X(vkResetCommandPool)            \
    X(vkAllocateCommandBuffers)      \
    X(vkBeginCommandBuffer)          \
    X(vkEndCommandBuffer)            \
    X(vkCreateFence)                 \
    X(vkDestroyFence)                \
    X(vkResetFences)                 \
    X(vkWaitForFences)               \
    X(vkGetFenceStatus)              \
    X(vkQueueSubmit)                 \
    X(vkCreateDescriptorPool)        \
    X(vkDestroyDescriptorPool)       \
    X(vkResetDescriptorPool)         \
    X(vkAllocateDescriptorSets)      \
    X(vkCreateBuffer)                \
    X(vkDestroyBuffer)               \
    X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory)            \
    X(vkAllocateMemory)              \
    X(vkFreeMemory)                  \
    X(vkMapMemory)                   \
    X(vkUnmapMemory)                 \
    X(vkCreateImage)                 \
    X(vkDestroyImage)                \
    X(vkGetImageMemoryRequirements)  \
    X(vkBindImageMemory)             \
    X(vkCreateImageView)             \
    X(vkDestroyImageView)            \
    X(vkCreateQueryPool)             \
    X(vkDestroyQueryPool)            \
    X(vkGetQueryPoolResults)         \
    X(vkCmdResetQueryPool)           \
    X(vkCmdWriteTimestamp)           \
    X(vkCmdPipelineBarrier)          \
    X(vkCmdCopyImage)                \
    X(vkCmdCopyBufferToImage)        \
    X(vkCreateShaderModule)          \
    X(vkDestroyShaderModule)         \
    X(vkCreateDescriptorSetLayout)   \
    X(vkDestroyDescriptorSetLayout)  \
    X(vkCreatePipelineLayout)        \
    X(vkDestroyPipelineLayout)       \
    X(vkCreateComputePipelines)      \
    X(vkDestroyPipeline)             \
    X(vkCreateSampler)               \
    X(vkDestroySampler)

    struct Functions
    {
#define WXL_VK_DECLARE(name) PFN_##name name = nullptr;
        WXL_VK_INSTANCE_FUNCTIONS(WXL_VK_DECLARE)
        WXL_VK_DEVICE_FUNCTIONS(WXL_VK_DECLARE)
#undef WXL_VK_DECLARE
    };

    /// Loads every function above; returns the name of the first one missing, or null when complete.
    inline const char* LoadFunctions(Functions& fn, VkInstance instance, VkDevice device,
                                     PFN_vkGetInstanceProcAddr getInstanceProcAddr, PFN_vkGetDeviceProcAddr getDeviceProcAddr)
    {
#define WXL_VK_LOAD_INSTANCE(name)                                                           \
        fn.name = reinterpret_cast<PFN_##name>(getInstanceProcAddr(instance, #name));        \
        if (!fn.name) return #name;
#define WXL_VK_LOAD_DEVICE(name)                                                             \
        fn.name = reinterpret_cast<PFN_##name>(getDeviceProcAddr(device, #name));            \
        if (!fn.name) return #name;
        WXL_VK_INSTANCE_FUNCTIONS(WXL_VK_LOAD_INSTANCE)
        WXL_VK_DEVICE_FUNCTIONS(WXL_VK_LOAD_DEVICE)
#undef WXL_VK_LOAD_INSTANCE
#undef WXL_VK_LOAD_DEVICE
        return nullptr;
    }

    /// The VkResult names that reach the log.
    inline const char* ResultName(VkResult r)
    {
        switch (r)
        {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY: return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_INVALID_SHADER_NV: return "VK_ERROR_INVALID_SHADER_NV";
        case VK_ERROR_UNKNOWN: return "VK_ERROR_UNKNOWN";
        default: return "VkResult(?)";
        }
    }
}
