// wxl-graphics-fog: every image the fog keeps on the GPU, the pipelines, and one dispatch with its
// descriptor set.
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

#include "Gpu.hpp"
#include "Spirv.hpp"

#include <algorithm>
#include <vector>

namespace
{
    using namespace wxl::gfx::fog;
    using namespace wxl::gfx::fog::gpu;
    namespace vkh = wxl::gfx::vk;

    Images            g_img;
    FrameBindings     g_frame;
    WXL_GfxVkPipeline g_pipes[FOG_PIPE_COUNT] = {};
    uint32_t          g_pipeGeneration = 0;
    bool              g_pipesOk = false;
    bool              g_createWarned = false;
    bool              g_setWarned = false;
    bool              g_persistentFailed = false;
    std::vector<WXL_GfxVkImage*> g_fresh;   // made since the last block: cleared before use

    constexpr VkImageUsageFlags kUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    bool Make(const WXL_GfxVulkanApi* api, WXL_GfxVkImage& out, const char* name, VkImageType type, VkFormat format,
              uint32_t w, uint32_t h, uint32_t d, uint32_t mips = 1, bool clear = true)
    {
        if (out.image != VK_NULL_HANDLE) return true;
        WXL_GfxVkImageDesc desc{};
        desc.structSize = sizeof desc;
        desc.name = name;
        desc.type = type;
        desc.format = format;
        desc.width = std::max(w, 1u);
        desc.height = std::max(h, 1u);
        desc.depth = std::max(d, 1u);
        desc.mipLevels = mips;
        desc.usage = kUsage;
        out = WXL_GfxVkImage{};
        if (!api->CreateImage(&desc, &out) || out.image == VK_NULL_HANDLE)
        {
            out = WXL_GfxVkImage{};
            if (!g_createWarned)
            {
                g_createWarned = true;
                FOG_LOG_WARN("gpu: image %s (%ux%ux%u, format %d) refused (logged once)", name, desc.width, desc.height,
                             desc.depth, int(format));
            }
            return false;
        }
        if (clear) g_fresh.push_back(&out);
        return true;
    }

    void Drop(const WXL_GfxVulkanApi* api, WXL_GfxVkImage& img)
    {
        if (img.image != VK_NULL_HANDLE)
        {
            g_fresh.erase(std::remove(g_fresh.begin(), g_fresh.end(), &img), g_fresh.end());
            api->DestroyImage(&img);
        }
        img = WXL_GfxVkImage{};
    }

    VkImageView MipView(const WXL_GfxVkImage& img, uint32_t mip)
    {
        const Device& d = Dev();
        VkImageViewCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ci.image = img.image;
        ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = img.format;
        ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ci.subresourceRange.baseMipLevel = mip;
        ci.subresourceRange.levelCount = 1;
        ci.subresourceRange.baseArrayLayer = 0;
        ci.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        if (!d.vkCreateImageView || d.vkCreateImageView(d.device, &ci, nullptr, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
        return view;
    }
}

namespace wxl::gfx::fog::gpu
{
    Images& Img() { return g_img; }
    FrameBindings& Frame() { return g_frame; }

    void ForgetResources()
    {
        // The device took its objects: only the handles are dropped.
        g_img = Images{};
        for (WXL_GfxVkPipeline& p : g_pipes) p = WXL_GfxVkPipeline{};
        g_pipeGeneration = 0;
        g_pipesOk = false;
        g_createWarned = false;
        g_persistentFailed = false;
        g_fresh.clear();
    }

    bool EnsurePersistent(const WXL_GfxVulkanApi* api)
    {
        Images& m = g_img;
        if (m.persistent) return true;
        if (g_persistentFailed) return false;
        const VkImageType T2 = VK_IMAGE_TYPE_2D, T3 = VK_IMAGE_TYPE_3D;
        const uint32_t N = FOG_LEVEL_N, NZ = FOG_LEVEL_NZ, L = FOG_LEVELS, B = FOG_BLOCK_N;
        bool ok = Make(api, m.shape, "fog.noise.shape", T3, VK_FORMAT_R8G8B8A8_UNORM, 128, 128, 128)
               && Make(api, m.detail, "fog.noise.detail", T3, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 32)
               && Make(api, m.curl, "fog.noise.curl", T3, VK_FORMAT_R8G8B8A8_UNORM, 32, 32, 32)
               && Make(api, m.height, "fog.terrain.height", T2, VK_FORMAT_R16_SFLOAT, B, B, 1)
               && Make(api, m.sky, "fog.terrain.sky", T2, VK_FORMAT_R8_UNORM, B, B, 1)
               && Make(api, m.water, "fog.terrain.water", T2, VK_FORMAT_R16G16_SFLOAT, B, B, 1)
               && Make(api, m.floor, "fog.terrain.floor", T2, VK_FORMAT_R32G32_SFLOAT, B, B, 1, FOG_BLOCK_MIPS)
               && Make(api, m.sunVis, "fog.terrain.sun", T2, VK_FORMAT_R8_UNORM, FOG_SUNVIS_N, FOG_SUNVIS_N, 1)
               && Make(api, m.flux, "fog.rivers.flux", T2, VK_FORMAT_R32G32B32A32_SFLOAT, B, B, 1)
               && Make(api, m.layer, "fog.rivers.layer", T2, VK_FORMAT_R32G32B32A32_SFLOAT, B, B, 1)
               && Make(api, m.tracer[0], "fog.rivers.cascade0", T2, VK_FORMAT_R16_SFLOAT, B, B, 1)
               && Make(api, m.tracer[1], "fog.rivers.cascade1", T2, VK_FORMAT_R16_SFLOAT, B, B, 1)
               && Make(api, m.cascadeMap, "fog.terrain.cascade", T2, VK_FORMAT_B8G8R8A8_UNORM, B, B, 1)
               && Make(api, m.state, "fog.clip.state", T3, VK_FORMAT_R16G16_SFLOAT, N, N, NZ * L)
               && Make(api, m.hat, "fog.clip.forward", T3, VK_FORMAT_R16G16B16A16_SFLOAT, N, N, NZ)
               && Make(api, m.vel, "fog.clip.velocity", T3, VK_FORMAT_R16G16B16A16_SFLOAT, FOG_VEL_N, FOG_VEL_N, FOG_VEL_NZ)
               && Make(api, m.ground, "fog.clip.ground", T3, VK_FORMAT_R32_SFLOAT, N, N, L)
               && Make(api, m.light, "fog.clip.light", T3, VK_FORMAT_R16G16B16A16_SFLOAT, FOG_VEL_N, FOG_VEL_N, FOG_VEL_NZ * L)
               && Make(api, m.occ, "fog.clip.occupancy", T3, VK_FORMAT_R16_SFLOAT, FOG_OCC_N, FOG_OCC_N, FOG_OCC_NZ * L)
               && Make(api, m.wake[0], "fog.wake0", T3, VK_FORMAT_R16G16B16A16_SFLOAT, FOG_WAKE_N, FOG_WAKE_N, FOG_WAKE_LEVELS)
               && Make(api, m.wake[1], "fog.wake1", T3, VK_FORMAT_R16G16B16A16_SFLOAT, FOG_WAKE_N, FOG_WAKE_N, FOG_WAKE_LEVELS)
               && Make(api, m.wakeCurl, "fog.wake.curl", T3, VK_FORMAT_R16_SFLOAT, FOG_WAKE_N, FOG_WAKE_N, FOG_WAKE_LEVELS)
               && Make(api, m.wakePressure[0], "fog.wake.p0", T3, VK_FORMAT_R16_SFLOAT, FOG_WAKE_N, FOG_WAKE_N, FOG_WAKE_LEVELS)
               && Make(api, m.wakePressure[1], "fog.wake.p1", T3, VK_FORMAT_R16_SFLOAT, FOG_WAKE_N, FOG_WAKE_N, FOG_WAKE_LEVELS)
               && Make(api, m.wakeDiv, "fog.wake.div", T3, VK_FORMAT_R16_SFLOAT, FOG_WAKE_N, FOG_WAKE_N, FOG_WAKE_LEVELS)
               && Make(api, m.neutral2D, "fog.neutral2d", T2, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 1)
               && Make(api, m.neutral3D, "fog.neutral3d", T3, VK_FORMAT_R16G16B16A16_SFLOAT, 1, 1, 1);
        if (ok)
        {
            for (uint32_t i = 0; i < FOG_BLOCK_MIPS && ok; ++i)
            {
                m.floorMip[i] = MipView(m.floor, i);
                ok = m.floorMip[i] != VK_NULL_HANDLE;
            }
            if (!ok) FOG_LOG_WARN("gpu: the floor's mip views were refused");
        }
        if (!ok)
        {
            g_persistentFailed = true;
            FOG_LOG_WARN("gpu: the fog's images could not all be created; the fog stays inert on this device");
            return false;
        }
        m.persistent = true;
        FOG_LOG_INFO("gpu: images ready: 4 clipmap levels %ux%ux%u, terrain block %u^2, noise 128^3", N, N, NZ, B);
        return true;
    }

    bool EnsureScreen(const WXL_GfxVulkanApi* api, uint32_t width, uint32_t height)
    {
        Images& m = g_img;
        const uint32_t hw = std::max((width + 1) / 2, 1u), hh = std::max((height + 1) / 2, 1u);
        const uint32_t qw = std::max((width + 3) / 4, 1u), qh = std::max((height + 3) / 4, 1u);
        if (m.nearFog.image != VK_NULL_HANDLE && m.halfW == hw && m.halfH == hh) return true;
        for (WXL_GfxVkImage* i : { &m.nearFog, &m.nearAux, &m.farFog, &m.farAux, &m.combFog, &m.combAux, &m.histFog[0],
                                   &m.histFog[1], &m.histAux[0], &m.histAux[1], &m.debug, &m.nearFront, &m.farFront,
                                   &m.combFront, &m.histFront[0], &m.histFront[1] })
            Drop(api, *i);
        const VkImageType T2 = VK_IMAGE_TYPE_2D;
        const bool ok = Make(api, m.nearFog, "fog.near", T2, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1)
                     && Make(api, m.nearAux, "fog.near.aux", T2, VK_FORMAT_R32G32B32A32_SFLOAT, hw, hh, 1)
                     && Make(api, m.farFog, "fog.far", T2, VK_FORMAT_R16G16B16A16_SFLOAT, qw, qh, 1)
                     && Make(api, m.farAux, "fog.far.aux", T2, VK_FORMAT_R32G32B32A32_SFLOAT, qw, qh, 1)
                     && Make(api, m.combFog, "fog.combined", T2, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1)
                     && Make(api, m.combAux, "fog.combined.aux", T2, VK_FORMAT_R32G32B32A32_SFLOAT, hw, hh, 1)
                     && Make(api, m.histFog[0], "fog.history0", T2, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1)
                     && Make(api, m.histFog[1], "fog.history1", T2, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1)
                     && Make(api, m.histAux[0], "fog.history0.aux", T2, VK_FORMAT_R32G32B32A32_SFLOAT, hw, hh, 1)
                     && Make(api, m.histAux[1], "fog.history1.aux", T2, VK_FORMAT_R32G32B32A32_SFLOAT, hw, hh, 1)
                     && Make(api, m.debug, "fog.debug", T2, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1)
                     && Make(api, m.nearFront, "fog.near.front", T2, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1)
                     && Make(api, m.farFront, "fog.far.front", T2, VK_FORMAT_R16G16B16A16_SFLOAT, qw, qh, 1)
                     && Make(api, m.combFront, "fog.combined.front", T2, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1)
                     && Make(api, m.histFront[0], "fog.history0.front", T2, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1)
                     && Make(api, m.histFront[1], "fog.history1.front", T2, VK_FORMAT_R16G16B16A16_SFLOAT, hw, hh, 1);
        if (!ok)
        {
            for (WXL_GfxVkImage* i : { &m.nearFog, &m.nearAux, &m.farFog, &m.farAux, &m.combFog, &m.combAux, &m.histFog[0],
                                       &m.histFog[1], &m.histAux[0], &m.histAux[1], &m.debug, &m.nearFront, &m.farFront,
                                       &m.combFront, &m.histFront[0], &m.histFront[1] })
                Drop(api, *i);
            m.halfW = m.halfH = 0;
            return false;
        }
        m.halfW = hw;
        m.halfH = hh;
        m.quarterW = qw;
        m.quarterH = qh;
        FOG_LOG_INFO("gpu: march targets %ux%u (half) and %ux%u (quarter)", hw, hh, qw, qh);
        return true;
    }

    bool EnsureLampGrid(const WXL_GfxVulkanApi* api, uint32_t x, uint32_t y, uint32_t z)
    {
        Images& m = g_img;
        if (m.lamp[0].image != VK_NULL_HANDLE && m.lampX == x && m.lampY == y && m.lampZ == z) return true;
        Drop(api, m.lamp[0]);
        Drop(api, m.lamp[1]);
        const bool ok = Make(api, m.lamp[0], "fog.lamps0", VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT, x, y, z)
                     && Make(api, m.lamp[1], "fog.lamps1", VK_IMAGE_TYPE_3D, VK_FORMAT_R16G16B16A16_SFLOAT, x, y, z);
        if (!ok)
        {
            Drop(api, m.lamp[0]);
            Drop(api, m.lamp[1]);
            m.lampX = m.lampY = m.lampZ = 0;
            return false;
        }
        m.lampX = x;
        m.lampY = y;
        m.lampZ = z;
        FOG_LOG_INFO("gpu: lamp grid %ux%ux%u", x, y, z);
        return true;
    }

    void ClearFresh(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd)
    {
        if (g_fresh.empty()) return;
        const VkClearColorValue zero{};
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.levelCount = VK_REMAINING_MIP_LEVELS;
        range.layerCount = VK_REMAINING_ARRAY_LAYERS;
        for (WXL_GfxVkImage* i : g_fresh)
            if (i->image != VK_NULL_HANDLE) Dev().vkCmdClearColorImage(cmd, i->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        g_fresh.clear();
        api->CmdBarrier(cmd);
    }

    bool EnsurePipelines(const WXL_GfxVulkanApi* api)
    {
        const uint32_t generation = Dev().generation;
        if (g_pipeGeneration == generation) return g_pipesOk;
        g_pipeGeneration = generation;
        g_pipesOk = false;
        for (WXL_GfxVkPipeline& p : g_pipes) p = WXL_GfxVkPipeline{};
        if (spirv::Count() != FOG_PIPE_COUNT)
        {
            FOG_LOG_ERROR("gpu: the SPIR-V table holds %u modules, shared.h lists %u; rebuild the shaders", spirv::Count(),
                          unsigned(FOG_PIPE_COUNT));
            return false;
        }

        WXL_GfxVkBinding layout[FOG_B_COUNT] = {};
        for (uint32_t b = 0; b < FOG_B_COUNT; ++b)
        {
            layout[b].binding = b;
            layout[b].count = 1;
            layout[b].type = b == FOG_B_CONSTANTS ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                           : b == FOG_B_PRIMS || b == FOG_B_BINS || b == FOG_B_RWBUF ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                           : b < FOG_B_SAMP_POINT_CLAMP ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                           : b < FOG_B_OUT0 ? VK_DESCRIPTOR_TYPE_SAMPLER
                           : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }

        bool ok = true;
        for (uint32_t i = 0; i < FOG_PIPE_COUNT; ++i)
        {
            const spirv::Module& m = spirv::Get(i);
            WXL_GfxVkPipelineDesc d{};
            d.structSize = sizeof d;
            d.name = m.name;
            d.spirv = m.words;
            d.spirvBytes = m.bytes;
            d.entry = nullptr;
            d.bindings = layout;
            d.bindingCount = FOG_B_COUNT;
            d.pushConstantBytes = FOG_PUSH_BYTES;
            if (!m.words || !api->CreateComputePipeline(&d, &g_pipes[i]) || g_pipes[i].pipeline == VK_NULL_HANDLE)
            {
                FOG_LOG_WARN("gpu: pipeline %s refused; the fog cannot run", m.name);
                g_pipes[i] = WXL_GfxVkPipeline{};
                ok = false;
            }
        }
        if (!ok)
        {
            for (WXL_GfxVkPipeline& p : g_pipes)
                if (p.pipeline != VK_NULL_HANDLE) { api->DestroyPipeline(&p); p = WXL_GfxVkPipeline{}; }
        }
        else FOG_LOG_INFO("gpu: %u compute pipelines ready", unsigned(FOG_PIPE_COUNT));
        g_pipesOk = ok;
        return ok;
    }

    bool Dispatch(uint32_t pipe, const Bind& bind, const PushData& push, uint32_t gx, uint32_t gy, uint32_t gz)
    {
        const FrameBindings& f = g_frame;
        if (pipe >= FOG_PIPE_COUNT || g_pipes[pipe].pipeline == VK_NULL_HANDLE || !f.cmd) return false;
        if (gx == 0 || gy == 0 || gz == 0) return true;
        const WXL_GfxVkPipeline& p = g_pipes[pipe];
        VkDescriptorSet set = f.api->AllocDescriptorSet(p.setLayout);
        if (set == VK_NULL_HANDLE)
        {
            if (!g_setWarned)
            {
                g_setWarned = true;
                FOG_LOG_WARN("gpu: the frame's descriptor pool is exhausted; part of the fog is skipped this frame");
            }
            return false;
        }
        vkh::DescriptorWriter<32> w(set);
        w.Uniform(FOG_B_CONSTANTS, f.constants);
        w.Buffer(FOG_B_PRIMS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, f.prims.buffer, f.prims.offset, f.prims.size);
        w.Buffer(FOG_B_BINS, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, f.bins.buffer, f.bins.offset, f.bins.size);
        for (uint32_t i = 0; i < FOG_TEX_SLOTS; ++i)
        {
            VkImageView view = bind.texView[i] ? bind.texView[i] : (bind.tex[i] ? bind.tex[i]->view : VK_NULL_HANDLE);
            if (view == VK_NULL_HANDLE) continue;
            const VkImageLayout layout = bind.tex[i] ? bind.tex[i]->layout : VK_IMAGE_LAYOUT_GENERAL;
            w.Image(FOG_B_TEX0 + i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, view, layout);
        }
        w.SamplerOnly(FOG_B_SAMP_POINT_CLAMP, f.samplers[0]);
        w.SamplerOnly(FOG_B_SAMP_LINEAR_CLAMP, f.samplers[1]);
        w.SamplerOnly(FOG_B_SAMP_LINEAR_WRAP, f.samplers[2]);
        w.SamplerOnly(FOG_B_SAMP_POINT_WRAP, f.samplers[3]);
        for (uint32_t i = 0; i < FOG_OUT_SLOTS; ++i)
        {
            VkImageView view = bind.outView[i] ? bind.outView[i] : (bind.out[i] ? bind.out[i]->view : VK_NULL_HANDLE);
            if (view == VK_NULL_HANDLE) continue;
            w.Image(FOG_B_OUT0 + i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, view, VK_IMAGE_LAYOUT_GENERAL);
        }
        if (bind.rw != VK_NULL_HANDLE) w.Buffer(FOG_B_RWBUF, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, bind.rw, bind.rwOffset, bind.rwSize);
        w.Apply(Dev());

        const Device& d = Dev();
        d.vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        d.vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        d.vkCmdPushConstants(f.cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, FOG_PUSH_BYTES, &push);
        d.vkCmdDispatch(f.cmd, gx, gy, gz);
        f.api->CmdBarrier(f.cmd);
        return true;
    }
}
