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

#include "Resources.hpp"
#include "Barriers.hpp"
#include "Device.hpp"
#include "Formats.hpp"
#include "Frame.hpp"
#include "Ring.hpp"
#include "../core/Extension.hpp"
#include "../textures/BlueNoise.hpp"

#include "wxl/GraphicsExtendApi.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{
    namespace device = wxl::gfx::vulkan::device;
    namespace frames = wxl::gfx::vulkan::frames;
    namespace barriers = wxl::gfx::vulkan::barriers;
    namespace formats = wxl::gfx::vulkan::formats;
    namespace bn = wxl::gfx::textures::bluenoise;
    using wxl::gfx::vulkan::ResultName;

    constexpr uint32_t kMaxBindings = 128;
    constexpr uint32_t kMaxPushConstantBytes = 128;
    constexpr VkImageUsageFlags kImageUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                            | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    struct ServiceImage
    {
        std::string    name;
        WXL_GfxVkImage info{};
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkDeviceSize   bytes = 0;
        uint32_t       layers = 1;
        bool           transitioned = false;   // UNDEFINED -> GENERAL recorded
    };
    // Heap objects: a few dozen at most, found by handle with a walk.
    std::vector<std::unique_ptr<ServiceImage>> g_images;

    struct Pipeline
    {
        std::string       name;
        WXL_GfxVkPipeline p{};
    };
    std::vector<Pipeline> g_pipelines;

    VkSampler g_samplers[4] = {};

    constexpr uint32_t kSharedCount = WXL_GFX_TEX_BLUE_NOISE + 1;
    struct Shared
    {
        VkImage image = VK_NULL_HANDLE;   // the ServiceImage in g_images with this handle
        bool    uploaded = false;
        bool    refused = false;          // creation failed on this device: not retried every frame
    };
    Shared g_shared[kSharedCount];

    ServiceImage* Find(VkImage image)
    {
        if (!image) return nullptr;
        for (const std::unique_ptr<ServiceImage>& e : g_images)
            if (e->info.image == image) return e.get();
        return nullptr;
    }

    bool IsShared(VkImage image)
    {
        for (const Shared& s : g_shared)
            if (s.image && s.image == image) return true;
        return false;
    }

    /// The image, its memory (one allocation per image, device-local) and its whole-image view.
    bool MakeImage(const char* name, VkImageType type, VkFormat format, uint32_t width, uint32_t height, uint32_t depth,
                   uint32_t mips, uint32_t layers, VkImageUsageFlags usage, bool cube, ServiceImage& out)
    {
        const device::Context& c = device::Ctx();
        VkDevice dev = c.device;
        char fmt[24];

        VkFormatProperties props{};
        c.fn.vkGetPhysicalDeviceFormatProperties(c.physical, format, &props);
        // The transfer feature bits exist since 1.1 (maintenance1): a 1.0 device reports none at all.
        VkFormatFeatureFlags need = device::ApiVersion() >= VK_API_VERSION_1_1
                                  ? VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT : 0;
        if (usage & VK_IMAGE_USAGE_STORAGE_BIT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
        if (usage & VK_IMAGE_USAGE_SAMPLED_BIT) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((props.optimalTilingFeatures & need) != need)
        {
            GFX_LOG_WARN("vulkan: image %s refused: %s lacks %s support on this device", name, formats::Name(format, fmt, sizeof fmt),
                         (usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? "storage image"
                         : (usage & VK_IMAGE_USAGE_SAMPLED_BIT) && !(props.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ? "sampled image"
                         : "transfer");
            return false;
        }

        VkImageCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ci.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
        ci.imageType = type;
        ci.format = format;
        ci.extent = VkExtent3D{ width, height, depth };
        ci.mipLevels = mips;
        ci.arrayLayers = layers;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = usage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImage image = VK_NULL_HANDLE;
        VkResult r = c.fn.vkCreateImage(dev, &ci, nullptr, &image);
        if (r != VK_SUCCESS)
        {
            GFX_LOG_WARN("vulkan: image %s refused: vkCreateImage %s", name, ResultName(r));
            return false;
        }

        VkMemoryRequirements req{};
        c.fn.vkGetImageMemoryRequirements(dev, image, &req);
        const int32_t typeIndex = device::MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (typeIndex < 0)
        {
            c.fn.vkDestroyImage(dev, image, nullptr);
            GFX_LOG_WARN("vulkan: image %s refused: no device-local memory type", name);
            return false;
        }
        // Vulkan 1.1 and up: the driver may give a large image its own physical range.
        VkMemoryDedicatedAllocateInfo dedicated{};
        dedicated.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
        dedicated.image = image;
        VkMemoryAllocateInfo alloc{};
        alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        alloc.pNext = device::ApiVersion() >= VK_API_VERSION_1_1 ? &dedicated : nullptr;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = uint32_t(typeIndex);
        VkDeviceMemory memory = VK_NULL_HANDLE;
        r = c.fn.vkAllocateMemory(dev, &alloc, nullptr, &memory);
        if (r == VK_SUCCESS) r = c.fn.vkBindImageMemory(dev, image, memory, 0);
        if (r != VK_SUCCESS)
        {
            if (memory) c.fn.vkFreeMemory(dev, memory, nullptr);
            c.fn.vkDestroyImage(dev, image, nullptr);
            GFX_LOG_WARN("vulkan: image %s refused: memory %s (%.1f MB)", name, ResultName(r), double(req.size) / (1024.0 * 1024.0));
            return false;
        }

        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : type == VK_IMAGE_TYPE_3D ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = mips;
        vi.subresourceRange.layerCount = layers;
        VkImageView view = VK_NULL_HANDLE;
        r = c.fn.vkCreateImageView(dev, &vi, nullptr, &view);
        if (r != VK_SUCCESS)
        {
            c.fn.vkDestroyImage(dev, image, nullptr);
            c.fn.vkFreeMemory(dev, memory, nullptr);
            GFX_LOG_WARN("vulkan: image %s refused: vkCreateImageView %s", name, ResultName(r));
            return false;
        }

        out.name = name;
        out.info = WXL_GfxVkImage{};
        out.info.image = image;
        out.info.view = view;
        out.info.format = format;
        out.info.type = type;
        out.info.extent = ci.extent;
        out.info.mipLevels = mips;
        out.info.layout = VK_IMAGE_LAYOUT_GENERAL;
        out.info.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        out.memory = memory;
        out.bytes = req.size;
        out.layers = layers;
        out.transitioned = false;
        return true;
    }

    void RecordTransition(VkCommandBuffer cmd, ServiceImage& e)
    {
        const VkImageMemoryBarrier b = barriers::Initial(e.info.image);
        device::Ctx().fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, barriers::kBlockStages, 0, 0, nullptr, 0, nullptr, 1, &b);
        e.transitioned = true;
    }

    /// Mip 0 of every layer, tightly packed, through this frame's staging ring; barriers around it,
    /// since the image may have been read or written earlier in the block.
    bool RecordUpload(VkCommandBuffer cmd, const ServiceImage& e, const void* data, size_t bytes)
    {
        // bufferOffset must be a multiple of the texel size (and of 4): 16 covers every power-of-two
        // texel, a 3-, 6-, 12- or 24-byte one needs 48.
        const uint32_t texel = formats::TexelBytes(e.info.format);
        WXL_GfxVkAlloc a{};
        if (!texel || !frames::AllocStaging(bytes, a, wxl::gfx::vulkan::Ring::CommonAlignment(texel, 16))) return false;
        std::memcpy(a.mapped, data, bytes);
        const wxl::gfx::vulkan::Functions& fn = device::Ctx().fn;
        barriers::Global(fn, cmd);
        VkBufferImageCopy region{};
        region.bufferOffset = a.offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = e.layers;
        region.imageExtent = e.info.extent;
        fn.vkCmdCopyBufferToImage(cmd, a.buffer, e.info.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        barriers::Global(fn, cmd);
        return true;
    }

    // --- the shared textures ------------------------------------------------------------------------

    struct SharedDesc
    {
        const char*  name;
        VkImageType  type;
        VkFormat     format;
        uint32_t     width, height, depth, layers;
        bool         cube;
    };

    const SharedDesc kShared[kSharedCount] = {
        { "shared white",        VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, false },
        { "shared black",        VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, false },
        { "shared flat normal",  VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, false },
        { "shared white cube",   VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 6, true },
        { "shared white volume", VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, false },
        // A8R8G8B8 in memory is B, G, R, A per byte: the baked texels upload as they are.
        { "shared blue noise",   VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM, uint32_t(bn::kSize), uint32_t(bn::kSize), 1, 1, false },
    };

    /// The texels of a shared texture; null while the blue noise is still baking.
    const void* SharedTexels(uint32_t which, size_t& bytes)
    {
        static const uint8_t kWhite[4]      = { 255, 255, 255, 255 };
        static const uint8_t kBlack[4]      = { 0, 0, 0, 0 };
        static const uint8_t kFlatNormal[4] = { 128, 128, 255, 255 };
        static const uint8_t kWhiteCube[24] = { 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
                                                255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255 };
        switch (which)
        {
        case WXL_GFX_TEX_WHITE:        bytes = sizeof kWhite;      return kWhite;
        case WXL_GFX_TEX_BLACK:        bytes = sizeof kBlack;      return kBlack;
        case WXL_GFX_TEX_FLAT_NORMAL:  bytes = sizeof kFlatNormal; return kFlatNormal;
        case WXL_GFX_TEX_WHITE_CUBE:   bytes = sizeof kWhiteCube;  return kWhiteCube;
        case WXL_GFX_TEX_WHITE_VOLUME: bytes = sizeof kWhite;      return kWhite;
        default:
            bytes = size_t(bn::kSize) * size_t(bn::kSize) * 4;
            return bn::Texels();
        }
    }

    void Refuse(const char* what, const char* name, const char* why)
    {
        GFX_LOG_WARN("vulkan: %s %s refused: %s", what, name ? name : "(unnamed)", why);
    }

    const char* DescriptorTypeName(VkDescriptorType t)
    {
        switch (t)
        {
        case VK_DESCRIPTOR_TYPE_SAMPLER: return "sampler";
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER: return "combined image sampler";
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: return "sampled image";
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE: return "storage image";
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER: return "uniform buffer";
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER: return "storage buffer";
        default: return nullptr;   // not served by the frame pools
        }
    }
}

namespace wxl::gfx::vulkan::resources
{
    int CreateImage(const WXL_GfxVkImageDesc* desc, WXL_GfxVkImage* out)
    {
        if (!desc || !out) return 0;
        *out = WXL_GfxVkImage{};
        if (desc->structSize < sizeof(WXL_GfxVkImageDesc)) { Refuse("image", nullptr, "structSize too small"); return 0; }
        const char* name = desc->name && desc->name[0] ? desc->name : "(unnamed)";
        if (!device::Available()) return 0;
        const VkPhysicalDeviceLimits& l = device::Ctx().properties.limits;

        const bool volume = desc->type == VK_IMAGE_TYPE_3D;
        if (!volume && desc->type != VK_IMAGE_TYPE_2D) { Refuse("image", name, "type must be VK_IMAGE_TYPE_2D or _3D"); return 0; }
        if (desc->format == VK_FORMAT_UNDEFINED || formats::HasDepth(desc->format) || formats::HasStencil(desc->format))
        {
            Refuse("image", name, "a colour format is required");
            return 0;
        }
        const uint32_t width = desc->width, height = desc->height, depth = volume ? desc->depth : 1;
        const uint32_t maxDim = volume ? l.maxImageDimension3D : l.maxImageDimension2D;
        if (!width || !height || !depth || width > maxDim || height > maxDim || depth > maxDim)
        {
            Refuse("image", name, "size is zero or over the device's maximum");
            return 0;
        }
        uint32_t mips = desc->mipLevels ? desc->mipLevels : 1;
        uint32_t full = 1;
        for (uint32_t d = width > height ? width : height; d > 1; d >>= 1) ++full;
        if (volume) { uint32_t f3 = 1; for (uint32_t d = depth; d > 1; d >>= 1) ++f3; if (f3 > full) full = f3; }
        if (mips > full) { Refuse("image", name, "more mip levels than the size allows"); return 0; }
        if (desc->usage & ~kImageUsage) { Refuse("image", name, "usage may only hold STORAGE, SAMPLED and the TRANSFER bits"); return 0; }

        auto e = std::make_unique<ServiceImage>();
        if (!MakeImage(name, desc->type, desc->format, width, height, depth, mips, 1, desc->usage & kImageUsage, false, *e)) return 0;
        if (frames::Recording()) RecordTransition(frames::Cmd(), *e);
        *out = e->info;
        char fmt[24];
        GFX_LOG_DEBUG("vulkan: image %s: %ux%ux%u %s, %u mips, %.1f MB", name, width, height, depth,
                      formats::Name(desc->format, fmt, sizeof fmt), mips, double(e->bytes) / (1024.0 * 1024.0));
        g_images.push_back(std::move(e));
        return 1;
    }

    void DestroyImage(const WXL_GfxVkImage* image)
    {
        if (!image || !image->image) return;
        if (IsShared(image->image))
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: DestroyImage: a shared texture is the service's; ignored (logged once)");
            }
            return;
        }
        for (size_t i = 0; i < g_images.size(); ++i)
        {
            if (g_images[i]->info.image != image->image) continue;
            const ServiceImage& e = *g_images[i];
            frames::DeferImage(e.info.image, e.info.view, e.memory);
            g_images.erase(g_images.begin() + ptrdiff_t(i));
            return;
        }
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            GFX_LOG_WARN("vulkan: DestroyImage: not a live service image; ignored (logged once)");
        }
    }

    int UploadImage(const WXL_GfxVkImage* image, const void* data, size_t bytes)
    {
        if (!frames::Recording())
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: UploadImage: only inside a record callback; ignored (logged once)");
            }
            return 0;
        }
        if (!image || !data || !bytes) return 0;
        const ServiceImage* e = Find(image->image);
        if (!e)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: UploadImage: not a service image; ignored (logged once)");
            }
            return 0;
        }
        const uint32_t texel = formats::TexelBytes(e->info.format);
        if (!texel)
        {
            char fmt[24];
            GFX_LOG_WARN("vulkan: UploadImage %s: %s is not a format the service uploads", e->name.c_str(),
                         formats::Name(e->info.format, fmt, sizeof fmt));
            return 0;
        }
        const uint64_t expected = uint64_t(e->info.extent.width) * e->info.extent.height * e->info.extent.depth * e->layers * texel;
        if (bytes != expected)
        {
            GFX_LOG_WARN("vulkan: UploadImage %s: %u bytes given, mip 0 holds %u", e->name.c_str(), unsigned(bytes), unsigned(expected));
            return 0;
        }
        return RecordUpload(frames::Cmd(), *e, data, bytes) ? 1 : 0;
    }

    int CreateComputePipeline(const WXL_GfxVkPipelineDesc* desc, WXL_GfxVkPipeline* out)
    {
        if (!desc || !out) return 0;
        *out = WXL_GfxVkPipeline{};
        if (desc->structSize < sizeof(WXL_GfxVkPipelineDesc)) { Refuse("pipeline", nullptr, "structSize too small"); return 0; }
        const char* name = desc->name && desc->name[0] ? desc->name : "(unnamed)";
        if (!device::Available()) return 0;
        const device::Context& c = device::Ctx();
        VkDevice dev = c.device;

        if (!desc->spirv || desc->spirvBytes < 20 || (desc->spirvBytes % 4) != 0) { Refuse("pipeline", name, "SPIR-V missing, or not a multiple of 4 bytes"); return 0; }
        if (desc->spirv[0] != 0x07230203u) { Refuse("pipeline", name, "not SPIR-V (bad magic number)"); return 0; }
        // Word 1 is the module's version: one newer than the device accepts is undefined behaviour to create.
        if (desc->spirv[1] > c.caps.spirvVersion)
        {
            char why[128];
            std::snprintf(why, sizeof why, "SPIR-V %u.%u is newer than the device accepts (%u.%u): lower DXC's -fspv-target-env",
                          (desc->spirv[1] >> 16) & 0xFF, (desc->spirv[1] >> 8) & 0xFF, (c.caps.spirvVersion >> 16) & 0xFF,
                          (c.caps.spirvVersion >> 8) & 0xFF);
            Refuse("pipeline", name, why);
            return 0;
        }
        if (desc->bindingCount && !desc->bindings) { Refuse("pipeline", name, "bindings null with a count"); return 0; }
        if (desc->bindingCount > kMaxBindings) { Refuse("pipeline", name, "too many bindings"); return 0; }
        if ((desc->pushConstantBytes % 4) != 0 || desc->pushConstantBytes > kMaxPushConstantBytes
            || desc->pushConstantBytes > c.properties.limits.maxPushConstantsSize)
        {
            Refuse("pipeline", name, "pushConstantBytes must be a multiple of 4, at most 128 and within the device's limit");
            return 0;
        }

        std::vector<VkDescriptorSetLayoutBinding> bindings(desc->bindingCount);
        for (uint32_t i = 0; i < desc->bindingCount; ++i)
        {
            const WXL_GfxVkBinding& b = desc->bindings[i];
            if (!DescriptorTypeName(b.type))
            {
                char why[96];
                std::snprintf(why, sizeof why, "binding %u: descriptor type %u is not one the frame pools serve", b.binding, unsigned(b.type));
                Refuse("pipeline", name, why);
                return 0;
            }
            for (uint32_t j = 0; j < i; ++j)
            {
                if (desc->bindings[j].binding != b.binding) continue;
                char why[64];
                std::snprintf(why, sizeof why, "binding %u appears twice", b.binding);
                Refuse("pipeline", name, why);
                return 0;
            }
            bindings[i] = VkDescriptorSetLayoutBinding{};
            bindings[i].binding = b.binding;
            bindings[i].descriptorType = b.type;
            bindings[i].descriptorCount = b.count ? b.count : 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo sl{};
        sl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        sl.bindingCount = desc->bindingCount;
        sl.pBindings = bindings.empty() ? nullptr : bindings.data();
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkResult r = c.fn.vkCreateDescriptorSetLayout(dev, &sl, nullptr, &setLayout);
        if (r != VK_SUCCESS)
        {
            GFX_LOG_WARN("vulkan: pipeline %s refused: vkCreateDescriptorSetLayout %s", name, ResultName(r));
            return 0;
        }

        VkPushConstantRange range{};
        range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        range.offset = 0;
        range.size = desc->pushConstantBytes;
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &setLayout;
        pl.pushConstantRangeCount = desc->pushConstantBytes ? 1 : 0;
        pl.pPushConstantRanges = desc->pushConstantBytes ? &range : nullptr;
        VkPipelineLayout layout = VK_NULL_HANDLE;
        r = c.fn.vkCreatePipelineLayout(dev, &pl, nullptr, &layout);
        if (r != VK_SUCCESS)
        {
            c.fn.vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
            GFX_LOG_WARN("vulkan: pipeline %s refused: vkCreatePipelineLayout %s", name, ResultName(r));
            return 0;
        }

        VkShaderModuleCreateInfo sm{};
        sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize = desc->spirvBytes;
        sm.pCode = desc->spirv;
        VkShaderModule module = VK_NULL_HANDLE;
        r = c.fn.vkCreateShaderModule(dev, &sm, nullptr, &module);
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (r == VK_SUCCESS)
        {
            VkComputePipelineCreateInfo ci{};
            ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            ci.stage.module = module;
            ci.stage.pName = desc->entry && desc->entry[0] ? desc->entry : "main";
            ci.layout = layout;
            r = c.fn.vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline);
            c.fn.vkDestroyShaderModule(dev, module, nullptr);
        }
        if (r != VK_SUCCESS || !pipeline)
        {
            c.fn.vkDestroyPipelineLayout(dev, layout, nullptr);
            c.fn.vkDestroyDescriptorSetLayout(dev, setLayout, nullptr);
            GFX_LOG_WARN("vulkan: pipeline %s refused: %s %s (entry \"%s\", %u bytes of SPIR-V)", name,
                         module ? "vkCreateComputePipelines" : "vkCreateShaderModule", ResultName(r),
                         desc->entry && desc->entry[0] ? desc->entry : "main", unsigned(desc->spirvBytes));
            return 0;
        }

        Pipeline p;
        p.name = name;
        p.p.pipeline = pipeline;
        p.p.layout = layout;
        p.p.setLayout = setLayout;
        g_pipelines.push_back(std::move(p));
        *out = g_pipelines.back().p;
        GFX_LOG_DEBUG("vulkan: pipeline %s: %u bindings, %u push-constant bytes", name, desc->bindingCount, desc->pushConstantBytes);
        return 1;
    }

    void DestroyPipeline(const WXL_GfxVkPipeline* pipeline)
    {
        if (!pipeline || !pipeline->pipeline) return;
        for (size_t i = 0; i < g_pipelines.size(); ++i)
        {
            if (g_pipelines[i].p.pipeline != pipeline->pipeline) continue;
            const WXL_GfxVkPipeline& p = g_pipelines[i].p;
            frames::DeferPipeline(p.pipeline, p.layout, p.setLayout);
            g_pipelines.erase(g_pipelines.begin() + ptrdiff_t(i));
            return;
        }
        static bool warned = false;
        if (!warned)
        {
            warned = true;
            GFX_LOG_WARN("vulkan: DestroyPipeline: not a live service pipeline; ignored (logged once)");
        }
    }

    VkSampler Sampler(uint32_t which)
    {
        if (which > WXL_GFX_VK_SAMPLER_LINEAR_WRAP || !device::Available()) return VK_NULL_HANDLE;
        if (g_samplers[which]) return g_samplers[which];
        const bool linear = which == WXL_GFX_VK_SAMPLER_LINEAR_CLAMP || which == WXL_GFX_VK_SAMPLER_LINEAR_WRAP;
        const bool wrap = which == WXL_GFX_VK_SAMPLER_POINT_WRAP || which == WXL_GFX_VK_SAMPLER_LINEAR_WRAP;
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.minFilter = si.magFilter;
        si.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = wrap ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = si.addressModeU;
        si.addressModeW = si.addressModeU;
        si.maxAnisotropy = 1.0f;
        si.minLod = 0.0f;
        si.maxLod = VK_LOD_CLAMP_NONE;   // an explicit lod (SampleLevel) reaches every mip, point samplers included
        si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        const device::Context& c = device::Ctx();
        const VkResult r = c.fn.vkCreateSampler(c.device, &si, nullptr, &g_samplers[which]);
        if (r != VK_SUCCESS)
        {
            g_samplers[which] = VK_NULL_HANDLE;
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: vkCreateSampler failed (%s) (logged once)", ResultName(r));
            }
        }
        return g_samplers[which];
    }

    int SharedTexture(uint32_t which, WXL_GfxVkImage* out)
    {
        if (!out) return 0;
        *out = WXL_GfxVkImage{};
        if (which >= kSharedCount || !device::Available()) return 0;
        Shared& s = g_shared[which];
        if (!s.image)
        {
            if (s.refused) return 1;
            if (which == WXL_GFX_TEX_BLUE_NOISE)
            {
                bn::Start();                     // baked on first request, so an unused one costs nothing
                if (!bn::Texels()) return 1;     // null image until the worker is done
            }
            const SharedDesc& d = kShared[which];
            auto e = std::make_unique<ServiceImage>();
            if (!MakeImage(d.name, d.type, d.format, d.width, d.height, d.depth, 1, d.layers, VK_IMAGE_USAGE_SAMPLED_BIT, d.cube, *e))
            {
                s.refused = true;
                return 1;
            }
            s.image = e->info.image;
            s.uploaded = false;
            if (frames::Recording())
            {
                // Asked for from inside a pass: filled before that pass's dispatches run.
                RecordTransition(frames::Cmd(), *e);
                size_t bytes = 0;
                const void* texels = SharedTexels(which, bytes);
                if (texels && RecordUpload(frames::Cmd(), *e, texels, bytes)) s.uploaded = true;
            }
            g_images.push_back(std::move(e));
        }
        if (const ServiceImage* e = Find(s.image)) *out = e->info;
        return 1;
    }

    bool IsServiceImage(VkImage image) { return Find(image) != nullptr; }

    void RecordPending(VkCommandBuffer cmd)
    {
        const wxl::gfx::vulkan::Functions& fn = device::Ctx().fn;
        static std::vector<VkImageMemoryBarrier> initial;   // reused: no allocation per block
        initial.clear();
        for (const std::unique_ptr<ServiceImage>& e : g_images)
        {
            if (e->transitioned) continue;
            initial.push_back(barriers::Initial(e->info.image));
            e->transitioned = true;
        }
        if (!initial.empty())
            fn.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, barriers::kBlockStages, 0, 0, nullptr, 0, nullptr,
                                    uint32_t(initial.size()), initial.data());

        for (uint32_t which = 0; which < kSharedCount; ++which)
        {
            Shared& s = g_shared[which];
            if (!s.image || s.uploaded) continue;
            const ServiceImage* e = Find(s.image);
            size_t bytes = 0;
            const void* texels = SharedTexels(which, bytes);
            if (e && texels && RecordUpload(cmd, *e, texels, bytes)) s.uploaded = true;
        }
    }

    void Destroy(bool destroyObjects)
    {
        if (destroyObjects)
        {
            const device::Context& c = device::Ctx();
            VkDevice dev = c.device;
            for (const std::unique_ptr<ServiceImage>& e : g_images)
            {
                c.fn.vkDestroyImageView(dev, e->info.view, nullptr);
                c.fn.vkDestroyImage(dev, e->info.image, nullptr);
                c.fn.vkFreeMemory(dev, e->memory, nullptr);
            }
            for (const Pipeline& p : g_pipelines)
            {
                c.fn.vkDestroyPipeline(dev, p.p.pipeline, nullptr);
                c.fn.vkDestroyPipelineLayout(dev, p.p.layout, nullptr);
                c.fn.vkDestroyDescriptorSetLayout(dev, p.p.setLayout, nullptr);
            }
            for (VkSampler s : g_samplers)
                if (s) c.fn.vkDestroySampler(dev, s, nullptr);
        }
        g_images.clear();
        g_pipelines.clear();
        for (VkSampler& s : g_samplers) s = VK_NULL_HANDLE;
        for (Shared& s : g_shared) s = Shared{};
    }

    void Stats(uint32_t& images, uint64_t& imageBytes, uint32_t& pipelines)
    {
        images = uint32_t(g_images.size());
        imageBytes = 0;
        for (const std::unique_ptr<ServiceImage>& e : g_images) imageBytes += e->bytes;
        pipelines = uint32_t(g_pipelines.size());
    }
}
