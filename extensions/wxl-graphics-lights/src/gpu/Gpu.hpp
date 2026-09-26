// wxl-graphics-lights: the Vulkan side's internals -- the device functions, the images, the pipelines
// (one layout, plus wxl-graphics-shadow's bindings for the point-shadow variant), one dispatch with its
// descriptor set, and the per-span GPU timers.
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

#include "../../shaders/compute/shared.h"

#include "wxl/GraphicsVulkanApi.h"
#include "wxl/gfx/Vk.hpp"

#include <cstdint>
#include <cstring>

namespace wxl::gfx::lights::gpu
{
    /// Row for row the shaders' cbuffer: every member a float4 or an array of them.
    struct Constants
    {
#define LIGHTS_HOST1(name) float name[4];
#define LIGHTS_HOSTN(name, n) float name[n][4];
        LIGHTS_CONSTANTS(LIGHTS_HOST1, LIGHTS_HOSTN)
#undef LIGHTS_HOST1
#undef LIGHTS_HOSTN
    };
    static_assert(sizeof(Constants) % 16 == 0, "the constant buffer is rows of 16 bytes");

    inline void Set(float row[4], float x, float y, float z, float w)
    {
        row[0] = x;
        row[1] = y;
        row[2] = z;
        row[3] = w;
    }

    /// A 32-bit pattern in a float slot (the shaders read it back with asuint).
    inline float Bits(uint32_t v)
    {
        float f;
        std::memcpy(&f, &v, sizeof f);
        return f;
    }

    struct PushData
    {
        uint32_t a[4];
        float    b[4];
    };
    static_assert(sizeof(PushData) == LIGHTS_PUSH_BYTES, "push constants match the shaders");

    /// Vk.hpp's functions plus the timestamp ones (optional: without them there are no timings).
    struct Device : wxl::gfx::vk::Device
    {
        PFN_vkCreateQueryPool                        vkCreateQueryPool = nullptr;
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

    /// Follows the service's Vulkan device: a new generation drops every handle (the service destroyed
    /// what it made). False while no device can be used.
    bool FollowDevice(const WXL_GfxVulkanApi* api);

    /// A service image kept across frames: made on demand, cleared to zero before its first use.
    bool MakeImage(const WXL_GfxVulkanApi* api, WXL_GfxVkImage& out, const char* name, VkImageType type, VkFormat format,
                   uint32_t w, uint32_t h, uint32_t d);
    void DropImage(const WXL_GfxVulkanApi* api, WXL_GfxVkImage& img);

    /// The neutral 2D and 3D stand-ins bound where an input is absent; false when refused.
    bool EnsureNeutral(const WXL_GfxVulkanApi* api);
    const WXL_GfxVkImage& Neutral2D();
    const WXL_GfxVkImage& Neutral3D();

    /// Records the clears of images made since the last block (to zero), then a barrier.
    void ClearFresh(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd);

    /// The pipelines of LIGHTS_PIPE_*. A module not compiled (Shaders.gen.cpp) or refused is skipped;
    /// the point-shadow variant is made only while wxl-graphics-shadow describes its bindings.
    void EnsurePipelines(const WXL_GfxVulkanApi* api);
    bool HasPipeline(uint32_t pipe);

    struct Bind
    {
        const WXL_GfxVkImage* tex[LIGHTS_TEX_SLOTS] = {};
        const WXL_GfxVkImage* out[LIGHTS_OUT_SLOTS] = {};
    };

    /// The frame's shared bindings, set once per record.
    struct FrameBindings
    {
        const WXL_GfxVulkanApi* api = nullptr;
        VkCommandBuffer         cmd = VK_NULL_HANDLE;
        WXL_GfxVkAlloc          constants{};
        VkSampler               samplers[2] = {};
        const WXL_GfxVkImage*   noise = nullptr;
    };

    FrameBindings& Frame();

    /// Binds (a missing texture takes the neutral stand-in of its kind), dispatches, and puts a barrier
    /// after it. volumes marks the slots that are 3D images.
    bool Dispatch(uint32_t pipe, const Bind& bind, const PushData& push, uint32_t gx, uint32_t gy, uint32_t gz,
                  uint32_t volumes = 0);

    /// Destroys every pipeline (a settings change that alters their layout); made again on demand.
    void DropPipelines(const WXL_GfxVulkanApi* api);

    // --- timers --------------------------------------------------------------------------------------

    enum Span { kSpanField, kSpanHalo, kSpanSurface, kSpanCopy, kSpanCount };
    extern const char* const kSpanNames[kSpanCount];

    void  ForgetTimers();
    bool  BeginTimers(VkCommandBuffer cmd, uint32_t slot);
    void  MarkTimer(VkCommandBuffer cmd, Span span);
    void  EndTimers();
    /// Smoothed milliseconds of a span; -1 until measured. span < 0: every span together.
    float SpanMs(int span);
    bool  TimersSupported();
}
