// wxl-graphics-fog: the Vulkan side's internals -- the device functions, every image the fog keeps,
// the pipelines, one dispatch with its descriptor set, the per-pass GPU timers and the probe's
// read-back. Private to src/gpu and src/terrain.
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

#include "Constants.hpp"
#include "../core/Extension.hpp"

#include "wxl/gfx/Vk.hpp"

#include <cstdint>

namespace wxl::gfx::fog::gpu
{
    /// Vk.hpp's functions plus the ones the fog loads itself (timestamps, memory types); the extras
    /// are optional and what needs them stands down without them.
    struct Device : wxl::gfx::vk::Device
    {
        PFN_vkCreateQueryPool                        vkCreateQueryPool = nullptr;
        PFN_vkDestroyQueryPool                       vkDestroyQueryPool = nullptr;
        PFN_vkCmdResetQueryPool                      vkCmdResetQueryPool = nullptr;
        PFN_vkCmdWriteTimestamp                      vkCmdWriteTimestamp = nullptr;
        PFN_vkGetQueryPoolResults                    vkGetQueryPoolResults = nullptr;
        PFN_vkInvalidateMappedMemoryRanges           vkInvalidateMappedMemoryRanges = nullptr;
        PFN_vkGetPhysicalDeviceMemoryProperties      vkGetPhysicalDeviceMemoryProperties = nullptr;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
        VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
        uint32_t         queueFamily = 0;
        float            timestampPeriod = 0.0f;

        bool LoadAll(const WXL_GfxVkContext& ctx);
    };

    Device& Dev();

    /// Follows the service's Vulkan device: a new generation drops every handle (the service destroyed
    /// what it made; the rest died with the device). False while no device can be used.
    bool FollowDevice(const WXL_GfxVulkanApi* api);

    // --- images -------------------------------------------------------------------------------------------

    struct Images
    {
        // Noise, baked once per device.
        WXL_GfxVkImage shape{}, detail{}, curl{};
        bool           noiseBaked = false;

        // The terrain block.
        WXL_GfxVkImage height{}, sky{}, water{}, floor{}, sunVis{};
        VkImageView    floorMip[FOG_BLOCK_MIPS] = {};

        // The transport layer, and the share of its depth that came from cascades (ping-pong), with the
        // baked cascade map (spill points and reservoirs) on the block.
        WXL_GfxVkImage flux{}, layer{}, tracer[2]{}, cascadeMap{};

        // The clipmaps.
        WXL_GfxVkImage state{}, hat{}, vel{}, ground{}, light{}, occ{};

        // The wake fluid: state (vx, vy, fog change, height) ping-pong, vorticity, pressure, divergence.
        WXL_GfxVkImage wake[2]{}, wakeCurl{}, wakePressure[2]{}, wakeDiv{};

        // The lamp and indoor grid, last frame's and this one's.
        WXL_GfxVkImage lamp[2]{};
        uint32_t       lampX = 0, lampY = 0, lampZ = 0;

        // Screen images at half and quarter resolution.
        WXL_GfxVkImage nearFog{}, nearAux{}, farFog{}, farAux{}, combFog{}, combAux{}, histFog[2]{}, histAux[2]{}, debug{};
        // The front layers (a footprint's nearest scene distance); the images above hold the back ones.
        WXL_GfxVkImage nearFront{}, farFront{}, combFront{}, histFront[2]{};
        uint32_t       halfW = 0, halfH = 0, quarterW = 0, quarterH = 0;

        // Stand-ins for an input that is missing this frame.
        WXL_GfxVkImage neutral2D{}, neutral3D{};

        bool persistent = false;   ///< everything but the screen and lamp images exists
    };

    Images& Img();

    /// The images that do not depend on the screen; false when one was refused (logged once).
    bool EnsurePersistent(const WXL_GfxVulkanApi* api);

    /// The screen images for a world size; true when they match it.
    bool EnsureScreen(const WXL_GfxVulkanApi* api, uint32_t width, uint32_t height);

    /// The lamp grid at a size; true when it exists at that size.
    bool EnsureLampGrid(const WXL_GfxVulkanApi* api, uint32_t x, uint32_t y, uint32_t z);

    /// Records the clears of images made since the last block (to zero), then a barrier.
    void ClearFresh(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd);

    // --- pipelines ------------------------------------------------------------------------------------------

    /// One pipeline per FOG_PIPE_*, all on one descriptor set layout; false (logged once per device)
    /// when a module is missing or refused.
    bool EnsurePipelines(const WXL_GfxVulkanApi* api);

    // --- one dispatch -------------------------------------------------------------------------------------

    /// What a dispatch binds besides the frame's constants, primitives and samplers.
    struct Bind
    {
        const WXL_GfxVkImage* tex[FOG_TEX_SLOTS] = {};
        VkImageView           texView[FOG_TEX_SLOTS] = {};   ///< overrides the image's own view
        const WXL_GfxVkImage* out[FOG_OUT_SLOTS] = {};
        VkImageView           outView[FOG_OUT_SLOTS] = {};
        VkBuffer              rw = VK_NULL_HANDLE;
        VkDeviceSize          rwOffset = 0, rwSize = 0;
    };

    /// The frame's shared bindings, set once per record.
    struct FrameBindings
    {
        const WXL_GfxVulkanApi* api = nullptr;
        VkCommandBuffer         cmd = VK_NULL_HANDLE;
        WXL_GfxVkAlloc          constants{}, prims{}, bins{};
        VkSampler               samplers[4] = {};
    };

    FrameBindings& Frame();

    /// Binds, dispatches, and puts a barrier after it (the next dispatch may read what it wrote).
    bool Dispatch(uint32_t pipe, const Bind& bind, const PushData& push, uint32_t gx, uint32_t gy, uint32_t gz);

    // --- timers -------------------------------------------------------------------------------------------

    enum Span
    {
        kSpanTerrain, kSpanTransport, kSpanWake, kSpanLevel0, kSpanLevel1, kSpanLevel2, kSpanLevel3, kSpanLight, kSpanLamps,
        kSpanNear, kSpanFar, kSpanTemporal, kSpanCopy, kSpanApply, kSpanCount
    };
    extern const char* const kSpanNames[kSpanCount];

    void ForgetTimers();
    bool BeginTimers(VkCommandBuffer cmd, uint32_t slot);
    void MarkTimer(VkCommandBuffer cmd, Span span);
    void EndTimers();
    /// Smoothed milliseconds of a span; -1 until measured. span < 0: all Vulkan spans together.
    float SpanMs(int span);
    void  SetApplyMs(float ms);
    bool  TimersSupported();

    // --- the camera probe's read-back ---------------------------------------------------------------------

    void     ForgetProbe();
    /// The buffer range the probe writes this frame (32 bytes); VK_NULL_HANDLE without one.
    VkBuffer ProbeBuffer(uint32_t slot, VkDeviceSize& offset);
    /// Picks up what the slot's frame wrote (its fence has signalled once the slot comes round again).
    void     PollProbe(uint32_t slot);

    /// The last probe read back: extinction, outdoor fog, indoor share, smoke; visibility, sun, lamps.
    struct ProbeResult
    {
        bool  valid = false;
        float extinction = 0.0f, fog = 0.0f, indoor = 0.0f, smoke = 0.0f;
        float visibility = 1e5f, sun = 0.0f, lamps = 0.0f;
    };
    const ProbeResult& Probe();
}
