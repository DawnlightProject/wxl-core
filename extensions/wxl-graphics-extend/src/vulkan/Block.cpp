// wxl-graphics-extend: the compute pass registry and the per-frame compute block.
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

#include "Block.hpp"
#include "Barriers.hpp"
#include "Device.hpp"
#include "Formats.hpp"
#include "Frame.hpp"
#include "Resources.hpp"
#include "../core/Extension.hpp"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
    namespace device = wxl::gfx::vulkan::device;
    namespace frames = wxl::gfx::vulkan::frames;
    namespace barriers = wxl::gfx::vulkan::barriers;
    namespace formats = wxl::gfx::vulkan::formats;
    namespace resources = wxl::gfx::vulkan::resources;

    // --- the pass registry (the scheduler's, for compute) ---------------------------------------

    struct Pass
    {
        uint32_t          id = 0;
        std::string       name;
        int32_t           order = 0;
        WXL_GfxWantsFn    wants = nullptr;
        WXL_GfxVkRecordFn record = nullptr;
        void*             user = nullptr;
        uint32_t          lastWants = 0;   // this frame's answer; 0 sits the pass out
        bool              ran = false;     // its record ran in the last block
        bool              threw = false;   // an exception escaped its record: logged once
    };

    // Heap objects: a pass's name is handed out as a pointer valid for the process, which a sort or a
    // reallocation of the vector must not move. Sorted by order, equal orders in add order.
    std::vector<std::unique_ptr<Pass>> g_passes;
    // Added from inside a wants / record callback: joins g_passes once the iteration is over.
    std::vector<std::unique_ptr<Pass>> g_pending;
    uint32_t g_nextId = 1;
    int      g_iterating = 0;

    void SortPasses()
    {
        std::stable_sort(g_passes.begin(), g_passes.end(),
                         [](const std::unique_ptr<Pass>& a, const std::unique_ptr<Pass>& b) { return a->order < b->order; });
    }

    void MergePending()
    {
        if (g_iterating || g_pending.empty()) return;
        for (std::unique_ptr<Pass>& p : g_pending) g_passes.push_back(std::move(p));
        g_pending.clear();
        SortPasses();
    }

    /// Holds the pass vector still while callbacks run; a pass added meanwhile joins afterwards.
    struct Iteration
    {
        Iteration() { ++g_iterating; }
        ~Iteration() { --g_iterating; MergePending(); }
        Iteration(const Iteration&) = delete;
        Iteration& operator=(const Iteration&) = delete;
    };

    // --- the block in flight -----------------------------------------------------------------------

    /// A D3D9 image taken into this block: in GENERAL from its acquire until the block's end.
    struct Import
    {
        VkImage            image;
        VkImageLayout      restore;         // DXVK's layout, handed back at the end
        VkImageAspectFlags barrierAspect;   // every aspect: a combined depth-stencil moves as one
        WXL_GfxVkImage     info;
    };
    std::vector<Import> g_imports;   // cleared per block; a handful, found by handle with a walk

    uint32_t g_anyWants = 0;       // union of the last poll
    uint32_t g_lastImports = 0;
    bool     g_lastRan = false;

    const Import* FindImport(VkImage image)
    {
        for (const Import& i : g_imports)
            if (i.image == image) return &i;
        return nullptr;
    }

    void WarnNotRecording(const char* what)
    {
        static bool warned = false;
        if (warned) return;
        warned = true;
        GFX_LOG_WARN("vulkan: %s: only inside a record callback; ignored (this is logged once)", what);
    }

    /// The whole-resource view a pass reads: cube for a six-layer 2D image (D3D9 has no arrays, so
    /// that is a cube texture), array for any other layered one, else plain. Identity swizzle: a
    /// luminance format reads its value in .r, as DXVK's own shaders see it.
    VkImageViewType ViewTypeOf(const VkImageCreateInfo& info)
    {
        switch (info.imageType)
        {
        case VK_IMAGE_TYPE_1D: return info.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case VK_IMAGE_TYPE_3D: return VK_IMAGE_VIEW_TYPE_3D;
        default:
            if (info.arrayLayers == 6 && ((info.flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) || info.extent.width == info.extent.height))
                return VK_IMAGE_VIEW_TYPE_CUBE;
            return info.arrayLayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        }
    }

    /// Hands every import back to DXVK's layout, in one barrier.
    void ReleaseImports(VkCommandBuffer cmd)
    {
        if (g_imports.empty()) return;
        static std::vector<VkImageMemoryBarrier> release;   // reused: no allocation per block
        release.clear();
        for (const Import& i : g_imports) release.push_back(barriers::Release(i.image, i.barrierAspect, i.restore));
        device::Ctx().fn.vkCmdPipelineBarrier(cmd, barriers::kBlockStages, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
                                              uint32_t(release.size()), release.data());
    }

    void RecordPasses(const WXL_GfxFrame& frame)
    {
        VkCommandBuffer cmd = frames::Cmd();
        resources::RecordPending(cmd);

        WXL_GfxVkFrame vf{};
        vf.structSize = sizeof vf;
        vf.cmd = cmd;
        vf.slot = frames::SlotIndex();
        vf.frame = &frame;
        // Absent this frame, or without a GPU image: the field stays VK_NULL_HANDLE, as documented.
        if (frame.depthTexture)      wxl::gfx::vulkan::block::ImportTexture(frame.depthTexture, &vf.depth);
        if (frame.normalTexture)     wxl::gfx::vulkan::block::ImportTexture(frame.normalTexture, &vf.normals);
        if (frame.sceneColorTexture) wxl::gfx::vulkan::block::ImportTexture(frame.sceneColorTexture, &vf.sceneColor);
        if (frame.albedoTexture)     wxl::gfx::vulkan::block::ImportTexture(frame.albedoTexture, &vf.albedo);

        frames::TimestampBlockBegin();
        {
            Iteration guard;
            for (std::unique_ptr<Pass>& p : g_passes)
            {
                if (!p->lastWants) continue;
                const int timer = frames::TimerBegin(p->id);
                try
                {
                    p->record(p->user, &vf);
                }
                catch (...)
                {
                    if (!p->threw)
                    {
                        p->threw = true;
                        GFX_LOG_ERROR("vulkan: compute pass %s: an exception escaped its record callback (logged once)", p->name.c_str());
                    }
                }
                frames::TimerEnd(timer);
                p->ran = true;
            }
        }
        frames::TimestampBlockEnd();
        ReleaseImports(cmd);
    }
}

namespace wxl::gfx::vulkan::block
{
    uint32_t AddComputePass(const WXL_GfxVkPassDesc* desc)
    {
        if (!desc || desc->structSize < sizeof(WXL_GfxVkPassDesc) || !desc->wants || !desc->record)
        {
            GFX_LOG_WARN("vulkan: compute pass rejected: %s",
                         !desc ? "null descriptor" : desc->structSize < sizeof(WXL_GfxVkPassDesc) ? "structSize too small"
                         : !desc->wants ? "no wants callback" : "no record callback");
            return 0;
        }

        auto p = std::make_unique<Pass>();
        p->id     = g_nextId++;
        p->order  = desc->order;
        p->wants  = desc->wants;
        p->record = desc->record;
        p->user   = desc->user;
        if (desc->name && desc->name[0]) p->name = desc->name;
        else
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "compute#%u", p->id);
            p->name = buf;
        }
        GFX_LOG_INFO("vulkan: compute pass %s at order %d", p->name.c_str(), p->order);

        const uint32_t id = p->id;
        if (g_iterating) g_pending.push_back(std::move(p));
        else
        {
            g_passes.push_back(std::move(p));
            SortPasses();
        }
        return id;
    }

    uint32_t PollWants()
    {
        g_lastRan = false;
        if (!device::Available())
        {
            g_anyWants = 0;
            return 0;
        }
        // Blocks known done release what they held (D3D9 references, old views) without waiting,
        // so nothing lingers while the passes sit out.
        frames::Poll();
        uint32_t requested = 0;
        {
            Iteration guard;
            for (std::unique_ptr<Pass>& p : g_passes)
            {
                p->ran = false;
                try
                {
                    p->lastWants = p->wants(p->user);
                }
                catch (...)
                {
                    p->lastWants = 0;
                }
                requested |= p->lastWants;
            }
        }
        g_anyWants = requested;
        return requested;
    }

    void Run(const WXL_GfxFrame& frame)
    {
        if (!g_anyWants || !device::Available()) return;
        if (!frames::Begin()) return;
        // The one flush of the frame, before any import: the handles and layouts read below are the
        // ones the queue sees, since nothing D3D9 is submitted again until the block is.
        void* anyImage = frame.depthTexture ? frame.depthTexture
                       : frame.normalTexture ? frame.normalTexture
                       : frame.sceneColorTexture ? frame.sceneColorTexture
                       : frame.target ? frame.target : frame.backBuffer;
        device::SettleD3D9(anyImage);
        g_imports.clear();
        try
        {
            RecordPasses(frame);
        }
        catch (...)
        {
            // Our own code ran out of memory mid-record: nothing of this block is submitted.
            frames::Abandon();
            g_lastImports = uint32_t(g_imports.size());
            g_imports.clear();
            throw;
        }
        g_lastImports = uint32_t(g_imports.size());
        g_imports.clear();
        g_lastRan = frames::Submit();
    }

    int ImportTexture(void* d3dResource, WXL_GfxVkImage* out)
    {
        if (!out) return 0;
        *out = WXL_GfxVkImage{};
        if (!frames::Recording()) { WarnNotRecording("ImportTexture"); return 0; }
        if (!d3dResource) return 0;

        auto* unknown = static_cast<IUnknown*>(d3dResource);
        ID3D9VkInteropTexture* interop = nullptr;
        if (FAILED(unknown->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&interop))) || !interop)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: ImportTexture: the resource is not a DXVK texture or surface (logged once)");
            }
            return 0;
        }
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout reported = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;   // pNext null, no queue family array: as DXVK requires
        const HRESULT hr = interop->GetVulkanImageInfo(&image, &reported, &info);
        interop->Release();
        if (FAILED(hr) || !image) return 0;   // SYSTEMMEM / SCRATCH: no GPU image

        if (const Import* cached = FindImport(image))
        {
            *out = cached->info;
            return 1;
        }

        const device::Context& c = device::Ctx();
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = image;
        vi.viewType = ViewTypeOf(info);
        vi.format = info.format;
        vi.subresourceRange.aspectMask = formats::ViewAspect(info.format);
        vi.subresourceRange.levelCount = info.mipLevels;
        vi.subresourceRange.layerCount = info.arrayLayers;
        VkImageView view = VK_NULL_HANDLE;
        const VkResult r = c.fn.vkCreateImageView(c.device, &vi, nullptr, &view);
        if (r != VK_SUCCESS)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                char fmt[24];
                GFX_LOG_WARN("vulkan: ImportTexture: vkCreateImageView %s for a %s image (logged once)", wxl::gfx::vulkan::ResultName(r),
                             formats::Name(info.format, fmt, sizeof fmt));
            }
            return 0;
        }
        frames::DeferImageView(view);        // lives until this block's GPU work is done
        frames::HoldResource(unknown);       // and so does the D3D9 resource, hence the VkImage

        Import imp{};
        imp.image = image;
        imp.barrierAspect = formats::BarrierAspect(info.format);
        // DXVK never reports an image without a layout; should it, GENERAL is the only one to leave it in.
        imp.restore = (reported == VK_IMAGE_LAYOUT_UNDEFINED || reported == VK_IMAGE_LAYOUT_PREINITIALIZED) ? VK_IMAGE_LAYOUT_GENERAL : reported;
        imp.info.image = image;
        imp.info.view = view;
        imp.info.format = info.format;
        imp.info.type = info.imageType;
        imp.info.extent = info.extent;
        imp.info.mipLevels = info.mipLevels;
        imp.info.layout = VK_IMAGE_LAYOUT_GENERAL;
        imp.info.aspect = vi.subresourceRange.aspectMask;

        const VkImageMemoryBarrier acquire = barriers::Acquire(image, imp.barrierAspect, reported);
        c.fn.vkCmdPipelineBarrier(frames::Cmd(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, barriers::kBlockStages, 0, 0, nullptr, 0, nullptr, 1, &acquire);

        g_imports.push_back(imp);
        *out = imp.info;
        return 1;
    }

    int CopyToTexture(const WXL_GfxVkImage* source, void* d3dTexture)
    {
        if (!frames::Recording()) { WarnNotRecording("CopyToTexture"); return 0; }
        if (!source || !source->image || !d3dTexture) return 0;
        // The source must be in GENERAL with TRANSFER_SRC: a service image, or an image of this block.
        if (!resources::IsServiceImage(source->image) && !FindImport(source->image))
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: CopyToTexture: the source is neither a service image nor an import of this block (logged once)");
            }
            return 0;
        }
        WXL_GfxVkImage dst{};
        if (!ImportTexture(d3dTexture, &dst)) return 0;

        const char* why = nullptr;
        if (dst.image == source->image) why = "the texture is the source itself";
        else if (dst.aspect != VK_IMAGE_ASPECT_COLOR_BIT || source->aspect != VK_IMAGE_ASPECT_COLOR_BIT) why = "both must be colour images";
        else if (dst.type != source->type) why = "a 2D image copies into a 2D texture, a 3D one into a volume texture";
        else if (dst.extent.width != source->extent.width || dst.extent.height != source->extent.height || dst.extent.depth != source->extent.depth)
            why = "the extents differ (mip 0)";
        else if (!formats::TexelBytes(source->format) || formats::TexelBytes(source->format) != formats::TexelBytes(dst.format))
            why = "the formats are not copy-compatible (same texel size, uncompressed)";
        if (why)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                char a[24], b[24];
                GFX_LOG_WARN("vulkan: CopyToTexture refused: %s (%ux%ux%u %s -> %ux%ux%u %s) (logged once)", why,
                             source->extent.width, source->extent.height, source->extent.depth, formats::Name(source->format, a, sizeof a),
                             dst.extent.width, dst.extent.height, dst.extent.depth, formats::Name(dst.format, b, sizeof b));
            }
            return 0;
        }

        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent = source->extent;
        device::Ctx().fn.vkCmdCopyImage(frames::Cmd(), source->image, VK_IMAGE_LAYOUT_GENERAL, dst.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        return 1;
    }

    void CmdBarrier(VkCommandBuffer cmd)
    {
        if (!frames::Recording()) { WarnNotRecording("CmdBarrier"); return; }
        if (cmd != frames::Cmd())
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("vulkan: CmdBarrier: not this block's command buffer; ignored (logged once)");
            }
            return;
        }
        barriers::Global(device::Ctx().fn, cmd);
    }

    size_t PassCount() { return g_passes.size(); }

    bool GetPassInfo(size_t index, PassInfo& out)
    {
        if (index >= g_passes.size()) return false;
        const Pass& p = *g_passes[index];
        out.name      = p.name.c_str();
        out.order     = p.order;
        out.lastWants = p.lastWants;
        out.ran       = p.ran;
        out.gpuMs     = frames::PassGpuMs(p.id);
        return true;
    }

    uint32_t LastImports() { return g_lastImports; }

    bool LastBlockRan() { return g_lastRan; }
}
