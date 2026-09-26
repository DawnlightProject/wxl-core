// wxl-graphics-extend: the frames in flight -- per-slot command buffer, fence, pools, host rings,
// timestamps and deferred destruction; the submission on DXVK's queue.
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

#include <windows.h>
#include <unknwn.h>

#include <cstdint>

// WXL_GFX_VK_FRAMES_IN_FLIGHT slots, used round robin. A slot is reused only once its fence was
// waited (bounded: a wait that times out skips the block rather than hang the client), and slots
// are always waited in the order they were submitted, so "block N is done" implies every earlier
// block is too. Objects handed to the deferred lists are tagged with the current block number and
// destroyed once a block with at least that number is known done.
namespace wxl::gfx::vulkan::frames
{
    constexpr uint32_t kSlots = WXL_GFX_VK_FRAMES_IN_FLIGHT;

    /// Creates the slots on the current device; false (logged) when a Vulkan object was refused.
    bool Create();

    /**
     * @brief The device goes: waits every in-flight slot (bounded), then destroys the slots and every
     *        deferred object. False when a wait timed out: nothing was destroyed (the GPU may still
     *        be using it) and every handle was forgotten instead.
     */
    bool Destroy();

    /// Before a D3D9 reset: retires every in-flight slot, releasing the D3D9 resources the blocks held.
    void WaitAll();

    /// Every frame: retires the in-flight slots whose fence already signaled, without waiting.
    void Poll();

    /// Waits the next slot's fence, resets its pools and rings, begins its command buffer. False when
    /// the wait timed out (the block is skipped this frame) or the device is unusable.
    bool Begin();

    /// Ends the command buffer and submits it behind the flushed D3D9 work, on DXVK's queue.
    bool Submit();

    /// Drops the block being recorded (an exception mid-record): nothing is submitted.
    void Abandon();

    bool            Recording();
    VkCommandBuffer Cmd();
    uint32_t        SlotIndex();

    // --- inside a block ------------------------------------------------------------------------

    bool            AllocUniform(VkDeviceSize size, WXL_GfxVkAlloc& out);
    /// 16-byte aligned for the API; an upload asks for a multiple of its texel size as well.
    bool            AllocStaging(VkDeviceSize size, WXL_GfxVkAlloc& out, VkDeviceSize alignment = 16);
    VkDescriptorSet AllocDescriptorSet(VkDescriptorSetLayout layout);

    /// Timestamps around the whole block and around each compute pass (pass id for the readback).
    void TimestampBlockBegin();
    void TimestampBlockEnd();
    int  TimerBegin(uint32_t passId);   ///< -1 when unsupported or out of pairs
    void TimerEnd(int timer);

    /// A D3D9 resource a block imported: kept alive (the reference is taken here) until that
    /// block's GPU work is done, so its VkImage cannot go away under the commands that use it.
    void HoldResource(IUnknown* resource);

    // --- any time --------------------------------------------------------------------------------

    void DeferImageView(VkImageView view);
    void DeferImage(VkImage image, VkImageView view, VkDeviceMemory memory);
    void DeferPipeline(VkPipeline pipeline, VkPipelineLayout layout, VkDescriptorSetLayout setLayout);

    /// Smoothed GPU milliseconds; -1 until measured (or without timestamps on this queue).
    float PassGpuMs(uint32_t passId);
    float BlockGpuMs();
    bool  TimestampsSupported();

    struct Stats
    {
        uint64_t uniformPeak, uniformCapacity;
        uint64_t stagingPeak, stagingCapacity;
        uint32_t setsPeak, setsCapacity;
        uint32_t deferred;      // objects waiting for the GPU
        uint32_t inFlight;      // slots submitted and not yet known done
    };
    void GetStats(Stats& out);
}
