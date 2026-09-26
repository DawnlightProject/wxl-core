// wxl-graphics-shadow: every image the service keeps, the stand-ins, the pipelines and one dispatch.
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
#include "../core/Extension.hpp"
#include "../core/Settings.hpp"

#include <cmath>
#include <cstring>

namespace wxl::gfx::shadow::gpu
{
    bool WriteShadowSet(wxl::gfx::vk::DescriptorWriter<32>& w, uint32_t first);   // Bindings.cpp
}

namespace
{
    using namespace wxl::gfx::shadow;
    using namespace wxl::gfx::shadow::gpu;
    namespace vkh = wxl::gfx::vk;

    Images            g_img;
    WXL_GfxVkPipeline g_pipes[SH_PIPE_COUNT] = {};
    uint32_t          g_pipeGeneration = 0;
    bool              g_pipesOk = false;
    bool              g_setWarned = false;
    bool              g_refused = false;

    bool Make(const WXL_GfxVulkanApi* api, WXL_GfxVkImage& out, const char* name, VkImageType type, VkFormat format,
              uint32_t w, uint32_t h, uint32_t d, uint32_t mips = 1)
    {
        WXL_GfxVkImageDesc desc{};
        desc.structSize = sizeof desc;
        desc.name = name;
        desc.type = type;
        desc.format = format;
        desc.width = w;
        desc.height = h;
        desc.depth = d;
        desc.mipLevels = mips;
        desc.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!api->CreateImage(&desc, &out) || out.image == VK_NULL_HANDLE)
        {
            out = WXL_GfxVkImage{};
            if (!g_refused)
            {
                g_refused = true;
                SHADOW_LOG_WARN("gpu: image %s (%ux%ux%u) refused; the parts that need it stay lit", name, w, h, d);
            }
            return false;
        }
        return true;
    }

    void Drop(const WXL_GfxVulkanApi* api, WXL_GfxVkImage& image)
    {
        if (image.image != VK_NULL_HANDLE) api->DestroyImage(&image);
        image = WXL_GfxVkImage{};
    }

    void DropLevels()
    {
        const Device& d = Dev();
        for (VkImageView& v : g_img.atlasLevel)
        {
            if (v != VK_NULL_HANDLE && d.vkDestroyImageView) d.vkDestroyImageView(d.device, v, nullptr);
            v = VK_NULL_HANDLE;
        }
    }

    uint16_t Half(float f)
    {
        uint32_t x;
        std::memcpy(&x, &f, 4);
        const uint32_t sign = (x >> 16) & 0x8000u;
        const int exp = int((x >> 23) & 0xFF) - 127 + 15;
        const uint32_t mant = x & 0x7FFFFFu;
        if (exp <= 0) return uint16_t(sign);
        if (exp >= 31) return uint16_t(sign | 0x7C00u);
        return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
    }
}

namespace wxl::gfx::shadow::gpu
{
    Images& Img() { return g_img; }

    void ForgetResources()
    {
        // The service destroyed what it made with the old device; views of ours died with it.
        g_img = Images{};
        for (WXL_GfxVkPipeline& p : g_pipes) p = WXL_GfxVkPipeline{};
        g_pipeGeneration = 0;
        g_pipesOk = false;
        g_refused = false;
        Current() = FrameSet{};
    }

    void FarMoments(float out[4])
    {
        const Settings& s = Config();
        out[0] = std::exp(s.evsmPositive);
        out[1] = out[0] * out[0];
        out[2] = -std::exp(-s.evsmNegative);
        out[3] = out[2] * out[2];
    }

    bool EnsureNeutral(const WXL_GfxVulkanApi* api)
    {
        Images& m = g_img;
        if (m.neutralMade) return true;
        const bool ok = Make(api, m.oneR32, "shadow.neutral.one", VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, 1, 1, 1)
                     && Make(api, m.farMoments, "shadow.neutral.moments", VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, 1, 1, 1)
                     && Make(api, m.noHorizon, "shadow.neutral.horizon", VK_IMAGE_TYPE_3D, VK_FORMAT_R8_UNORM, 1, 1, 1)
                     && Make(api, m.noHeights, "shadow.neutral.heights", VK_IMAGE_TYPE_2D, VK_FORMAT_R16_SFLOAT, 1, 1, 1)
                     && Make(api, m.noNormals, "shadow.neutral.normals", VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1)
                     && Make(api, m.noMasks, "shadow.neutral.masks", VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, WXL_SHADOW_MASK_LAYERS);
        if (!ok)
        {
            for (WXL_GfxVkImage* i : { &m.oneR32, &m.farMoments, &m.noHorizon, &m.noHeights, &m.noNormals, &m.noMasks }) Drop(api, *i);
            return false;
        }
        m.neutralMade = true;
        m.neutralFilled = false;
        return true;
    }

    bool EnsureAtlas(const WXL_GfxVulkanApi* api, uint32_t face)
    {
        Images& m = g_img;
        if (m.atlas.image != VK_NULL_HANDLE && m.atlasFace == face) return true;
        DropLevels();
        Drop(api, m.atlas);
        m.atlasFace = 0;
        if (!Make(api, m.atlas, "shadow.maps", VK_IMAGE_TYPE_2D, VK_FORMAT_R32G32B32A32_SFLOAT, face * WXL_SHADOW_ATLAS_FACES_X,
                  face * WXL_SHADOW_ATLAS_FACES_Y, 1, WXL_SHADOW_MIPS))
            return false;
        const Device& d = Dev();
        for (uint32_t level = 0; level < WXL_SHADOW_MIPS; ++level)
        {
            VkImageViewCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ci.image = m.atlas.image;
            ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ci.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ci.subresourceRange.baseMipLevel = level;
            ci.subresourceRange.levelCount = 1;
            ci.subresourceRange.layerCount = 1;
            if (d.vkCreateImageView(d.device, &ci, nullptr, &m.atlasLevel[level]) != VK_SUCCESS)
            {
                m.atlasLevel[level] = VK_NULL_HANDLE;
                DropLevels();
                Drop(api, m.atlas);
                SHADOW_LOG_WARN("gpu: the map atlas's level views were refused; lamps shadow with capsules only");
                return false;
            }
        }
        m.atlasFace = face;
        m.atlasFresh = true;
        SHADOW_LOG_INFO("gpu: map atlas %ux%u (faces of %u, %d levels, %.0f MB)", face * WXL_SHADOW_ATLAS_FACES_X,
                        face * WXL_SHADOW_ATLAS_FACES_Y, face, WXL_SHADOW_MIPS,
                        double(face) * face * 48.0 * 16.0 * 4.0 / 3.0 / (1024.0 * 1024.0));
        return true;
    }

    bool EnsureScreen(const WXL_GfxVulkanApi* api, uint32_t width, uint32_t height, bool half, bool debug)
    {
        Images& m = g_img;
        if (width == 0 || height == 0) return false;
        if (m.masks.image == VK_NULL_HANDLE || m.width != width || m.height != height)
        {
            Drop(api, m.masks);
            Drop(api, m.halfMasks);
            Drop(api, m.halfDepth);
            Drop(api, m.debug);
            m.width = m.height = m.halfW = m.halfH = 0;
            if (!Make(api, m.masks, "shadow.masks", VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, width, height, WXL_SHADOW_MASK_LAYERS))
                return false;
            m.width = width;
            m.height = height;
            SHADOW_LOG_INFO("gpu: masks %ux%u x %d layers", width, height, WXL_SHADOW_MASK_LAYERS);
        }
        const uint32_t hw = (width + 1) / 2, hh = (height + 1) / 2;
        if (half && (m.halfMasks.image == VK_NULL_HANDLE || m.halfW != hw || m.halfH != hh))
        {
            Drop(api, m.halfMasks);
            Drop(api, m.halfDepth);
            if (!Make(api, m.halfMasks, "shadow.masks.half", VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, hw, hh, WXL_SHADOW_MASK_LAYERS)
                || !Make(api, m.halfDepth, "shadow.depth.half", VK_IMAGE_TYPE_2D, VK_FORMAT_R32_SFLOAT, hw, hh, 1))
            {
                Drop(api, m.halfMasks);
                Drop(api, m.halfDepth);
                return false;
            }
            m.halfW = hw;
            m.halfH = hh;
        }
        if (debug && m.debug.image == VK_NULL_HANDLE)
            Make(api, m.debug, "shadow.debug", VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, width, height, 1);
        return true;
    }

    void Prime(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd)
    {
        Images& m = g_img;
        if (m.neutralMade && !m.neutralFilled)
        {
            const float one = 1.0f;
            float far[4];
            FarMoments(far);
            const uint8_t zero = 0;
            const uint16_t noGround = Half(-10000.0f);
            const uint32_t noNormal = 0;
            uint32_t lit[WXL_SHADOW_MASK_LAYERS];
            for (uint32_t& v : lit) v = 0xFFFFFFFFu;
            m.neutralFilled = api->UploadImage(&m.oneR32, &one, sizeof one) && api->UploadImage(&m.farMoments, far, sizeof far)
                           && api->UploadImage(&m.noHorizon, &zero, sizeof zero) && api->UploadImage(&m.noHeights, &noGround, sizeof noGround)
                           && api->UploadImage(&m.noNormals, &noNormal, sizeof noNormal) && api->UploadImage(&m.noMasks, lit, sizeof lit);
        }
        if (m.atlasFresh && cmd != VK_NULL_HANDLE && m.atlas.image != VK_NULL_HANDLE)
        {
            float far[4];
            FarMoments(far);
            VkClearColorValue value{};
            std::memcpy(value.float32, far, sizeof far);
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.levelCount = VK_REMAINING_MIP_LEVELS;
            range.layerCount = VK_REMAINING_ARRAY_LAYERS;
            Dev().vkCmdClearColorImage(cmd, m.atlas.image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
            api->CmdBarrier(cmd);
            m.atlasFresh = false;
        }
    }

    bool EnsurePipelines(const WXL_GfxVulkanApi* api)
    {
        const uint32_t generation = Dev().generation;
        if (g_pipeGeneration == generation) return g_pipesOk;
        g_pipeGeneration = generation;
        g_pipesOk = false;
        for (WXL_GfxVkPipeline& p : g_pipes) p = WXL_GfxVkPipeline{};
        if (spirv::Count() != SH_PIPE_COUNT)
        {
            SHADOW_LOG_ERROR("gpu: the SPIR-V table holds %u modules, shared.h lists %u; rebuild the shaders", spirv::Count(),
                             unsigned(SH_PIPE_COUNT));
            return false;
        }

        WXL_GfxVkBinding layout[SH_B_COUNT] = {};
        for (uint32_t b = 0; b < SH_B_COUNT; ++b)
        {
            layout[b].binding = b;
            layout[b].count = 1;
            layout[b].type = b == WXL_SHADOW_B_CONSTANTS || b == SH_B_PASS           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                           : b == WXL_SHADOW_B_POINT || b == WXL_SHADOW_B_LINEAR      ? VK_DESCRIPTOR_TYPE_SAMPLER
                           : b == SH_B_OUT0 || b == SH_B_OUT1                         ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                                      : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }

        bool ok = true;
        for (uint32_t i = 0; i < SH_PIPE_COUNT; ++i)
        {
            const spirv::Module& m = spirv::Get(i);
            WXL_GfxVkPipelineDesc d{};
            d.structSize = sizeof d;
            d.name = m.name;
            d.spirv = m.words;
            d.spirvBytes = m.bytes;
            d.bindings = layout;
            d.bindingCount = SH_B_COUNT;
            d.pushConstantBytes = SH_PUSH_BYTES;
            if (!m.words || !api->CreateComputePipeline(&d, &g_pipes[i]) || g_pipes[i].pipeline == VK_NULL_HANDLE)
            {
                SHADOW_LOG_WARN("gpu: pipeline %s refused; shadows cannot run", m.name);
                g_pipes[i] = WXL_GfxVkPipeline{};
                ok = false;
            }
        }
        if (!ok)
        {
            for (WXL_GfxVkPipeline& p : g_pipes)
                if (p.pipeline != VK_NULL_HANDLE) { api->DestroyPipeline(&p); p = WXL_GfxVkPipeline{}; }
        }
        else SHADOW_LOG_INFO("gpu: %u compute pipelines ready", unsigned(SH_PIPE_COUNT));
        g_pipesOk = ok;
        return ok;
    }

    bool Dispatch(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, uint32_t pipe, const Bind& bind, const Push& push,
                  uint32_t gx, uint32_t gy, uint32_t gz)
    {
        if (pipe >= SH_PIPE_COUNT || g_pipes[pipe].pipeline == VK_NULL_HANDLE || !cmd) return false;
        if (gx == 0 || gy == 0 || gz == 0) return true;
        const WXL_GfxVkPipeline& p = g_pipes[pipe];
        VkDescriptorSet set = api->AllocDescriptorSet(p.setLayout);
        if (set == VK_NULL_HANDLE)
        {
            if (!g_setWarned)
            {
                g_setWarned = true;
                SHADOW_LOG_WARN("gpu: the frame's descriptor pool is exhausted; part of the shadows is skipped this frame");
            }
            return false;
        }
        const Images& m = g_img;
        vkh::DescriptorWriter<32> w(set);
        WriteShadowSet(w, SH_B_SHADOW);
        if (bind.pass.buffer != VK_NULL_HANDLE) w.Uniform(SH_B_PASS, bind.pass);
        auto sampled = [&](uint32_t binding, const WXL_GfxVkImage* image, const WXL_GfxVkImage& fallback) {
            const WXL_GfxVkImage& use = image && image->image != VK_NULL_HANDLE ? *image : fallback;
            w.Image(binding, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, use.view, use.layout);
        };
        sampled(SH_B_DEPTH, bind.depth, m.oneR32);
        sampled(SH_B_NORMALS, bind.normals, m.noNormals);
        sampled(SH_B_SRC0, bind.src0, m.oneR32);
        sampled(SH_B_SRC1, bind.src1, m.oneR32);
        sampled(SH_B_IN0, bind.in0, m.noMasks);
        sampled(SH_B_IN1, bind.in1, m.oneR32);
        if (bind.out0 != VK_NULL_HANDLE) w.Image(SH_B_OUT0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, bind.out0, VK_IMAGE_LAYOUT_GENERAL);
        if (bind.out1 != VK_NULL_HANDLE) w.Image(SH_B_OUT1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, bind.out1, VK_IMAGE_LAYOUT_GENERAL);
        w.Apply(Dev());

        const Device& d = Dev();
        d.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.pipeline);
        d.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
        d.vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, SH_PUSH_BYTES, &push);
        d.vkCmdDispatch(cmd, gx, gy, gz);
        api->CmdBarrier(cmd);
        return true;
    }
}
