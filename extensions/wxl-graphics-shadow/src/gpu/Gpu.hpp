// wxl-graphics-shadow: the Vulkan side -- device functions, images, pipelines, one dispatch, the frame's
// public block and the binding set consumers write into their own descriptor sets, and the timers.
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

#include "../../shaders/shared.h"

#include "wxl/GraphicsVulkanApi.h"
#include "wxl/gfx/Vk.hpp"

#include <cstdint>

namespace wxl::gfx::shadow::gpu
{
    /// Vk.hpp's functions plus the timestamp ones (optional: without them the timers stand down).
    struct Device : wxl::gfx::vk::Device
    {
        PFN_vkCreateQueryPool                        vkCreateQueryPool = nullptr;
        PFN_vkDestroyQueryPool                       vkDestroyQueryPool = nullptr;
        PFN_vkCmdResetQueryPool                      vkCmdResetQueryPool = nullptr;
        PFN_vkCmdWriteTimestamp                      vkCmdWriteTimestamp = nullptr;
        PFN_vkGetQueryPoolResults                    vkGetQueryPoolResults = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        uint32_t         queueFamily = 0;
        float            timestampPeriod = 0.0f;

        bool LoadAll(const WXL_GfxVkContext& ctx);
    };

    Device& Dev();

    /// Follows the service's Vulkan device: a new generation drops every handle. False while none.
    bool FollowDevice(const WXL_GfxVulkanApi* api);

    // --- images -------------------------------------------------------------------------------------------

    struct Images
    {
        // The filtered cube maps: RGBA32F EVSM moments, WXL_SHADOW_MIPS levels, with a view per level.
        WXL_GfxVkImage atlas{};
        VkImageView    atlasLevel[WXL_SHADOW_MIPS] = {};
        uint32_t       atlasFace = 0;
        bool           atlasFresh = false;      ///< to be cleared to "nothing casts" in the next record

        // The masks at full resolution, the half-resolution trace and its depth, the debug view.
        WXL_GfxVkImage masks{}, halfMasks{}, halfDepth{}, debug{};
        uint32_t       width = 0, height = 0, halfW = 0, halfH = 0;

        // Stand-ins for an input that is missing, filled once per device.
        WXL_GfxVkImage oneR32{};       ///< 2D R32F 1 x 1 = 1 (a cascade that casts nothing)
        WXL_GfxVkImage farMoments{};   ///< 2D RGBA32F 1 x 1: the moments of "nothing casts"
        WXL_GfxVkImage noHorizon{};    ///< 3D R8 1 x 1 x 1 = 0
        WXL_GfxVkImage noHeights{};    ///< 2D R16F 1 x 1 = -10000
        WXL_GfxVkImage noNormals{};    ///< 2D RGBA8 1 x 1 = 0 (no normal written)
        WXL_GfxVkImage noMasks{};      ///< 3D RGBA8 1 x 1 x 5 = 1
        bool           neutralFilled = false;
        bool           neutralMade = false;
    };

    Images& Img();

    /// The stand-ins; before the block (a wants callback). False when refused.
    bool EnsureNeutral(const WXL_GfxVulkanApi* api);

    /// The filtered atlas at a face size (128 or 256); before the block.
    bool EnsureAtlas(const WXL_GfxVulkanApi* api, uint32_t face);

    /// The masks for a world size, the half-resolution trace when asked; before the block.
    bool EnsureScreen(const WXL_GfxVulkanApi* api, uint32_t width, uint32_t height, bool half, bool debug);

    /// Inside a record: fills the stand-ins once per device, clears a fresh atlas (only with a cmd).
    void Prime(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd);

    /// The moments a texel holds where nothing casts (depth 1), for the clears.
    void FarMoments(float out[4]);

    // --- pipelines and one dispatch ----------------------------------------------------------------------

    bool EnsurePipelines(const WXL_GfxVulkanApi* api);

    struct Push
    {
        uint32_t a[4];
        float    b[4];
    };
    static_assert(sizeof(Push) == SH_PUSH_BYTES, "push constants match the shaders");

    /// What one dispatch binds besides the shadow set.
    struct Bind
    {
        WXL_GfxVkAlloc        pass{};
        const WXL_GfxVkImage* depth = nullptr;
        const WXL_GfxVkImage* normals = nullptr;
        const WXL_GfxVkImage* src0 = nullptr;
        const WXL_GfxVkImage* src1 = nullptr;
        VkImageView           out0 = VK_NULL_HANDLE;
        VkImageView           out1 = VK_NULL_HANDLE;
        const WXL_GfxVkImage* in0 = nullptr;
        const WXL_GfxVkImage* in1 = nullptr;
    };

    /// Binds, dispatches, and puts a barrier after it.
    bool Dispatch(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, uint32_t pipe, const Bind& bind, const Push& push,
                  uint32_t gx, uint32_t gy, uint32_t gz);

    // --- the frame's public block and the binding set ---------------------------------------------------

    /// What the shadow set binds this frame. `valid` from the service's record to the next wants poll.
    struct FrameSet
    {
        bool           valid = false;
        WXL_GfxVkAlloc block{};
        WXL_GfxVkImage cascades[WXL_SHADOW_MAX_CASCADES]{};
        WXL_GfxVkImage horizon{}, heights{};
        bool           mapsBound = false;
    };

    FrameSet& Current();

    /// The frame is over for consumers (the next wants poll).
    void EndFrame();

    /// Writes the shadow set from `first` on: this frame's when the service recorded, else stand-ins
    /// and an "off" block (every lookup lit). Inside a record callback. False without Vulkan.
    bool WriteBindings(VkDescriptorSet set, uint32_t first);

    // --- timers ------------------------------------------------------------------------------------------

    enum Span { kSpanUpload, kSpanConvert, kSpanMips, kSpanMask, kSpanUpsample, kSpanDebug, kSpanCount };
    extern const char* const kSpanNames[kSpanCount];

    void  ForgetTimers();
    bool  BeginTimers(VkCommandBuffer cmd, uint32_t slot);
    void  MarkTimer(VkCommandBuffer cmd, Span span);
    void  EndTimers();
    float SpanMs(int span);
    bool  TimersSupported();

    /// Smoothed GPU milliseconds of every span together; -1 until measured.
    float TotalMs();

    /// Everything of the device is gone: handles forgotten (the service destroyed what it made).
    void ForgetResources();
}
