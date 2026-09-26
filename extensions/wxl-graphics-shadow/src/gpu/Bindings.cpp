// wxl-graphics-shadow: the shadow binding set -- written into the service's own dispatches and, through
// the API, into any consumer's descriptor set: this frame's resources, or stand-ins and an "off" block.
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
#include "../core/Extension.hpp"

#include <cstring>

namespace
{
    using namespace wxl::gfx::shadow;
    using namespace wxl::gfx::shadow::gpu;

    FrameSet       g_current;
    WXL_GfxVkAlloc g_off{};          // this frame's "off" block, made on the first need
    uint32_t       g_offSerial = 0;  // the frame it was made in
    uint32_t       g_serial = 1;     // bumped by EndFrame
}

namespace wxl::gfx::shadow::gpu
{
    FrameSet& Current() { return g_current; }

    void EndFrame()
    {
        g_current.valid = false;
        ++g_serial;
    }

    bool WriteShadowSet(wxl::gfx::vk::DescriptorWriter<32>& w, uint32_t first)
    {
        const WXL_GfxVulkanApi* api = Vk();
        const Images& m = Img();
        if (!api || !m.neutralMade) return false;
        // A consumer ahead of the service's first record fills the stand-ins itself (uploads only).
        if (!m.neutralFilled) Prime(api, VK_NULL_HANDLE);
        const FrameSet& f = g_current;
        WXL_GfxVkAlloc block = f.block;
        if (!f.valid || block.buffer == VK_NULL_HANDLE)
        {
            // Every lookup returns 1 while the block's enabled flag is 0.
            if (g_offSerial != g_serial || g_off.buffer == VK_NULL_HANDLE)
            {
                if (!api->AllocUniform(WXL_SHADOW_ROWS * 16, &g_off) || !g_off.mapped) return false;
                std::memset(g_off.mapped, 0, WXL_SHADOW_ROWS * 16);
                g_offSerial = g_serial;
            }
            block = g_off;
        }
        w.Uniform(first + WXL_SHADOW_B_CONSTANTS, block);
        w.SamplerOnly(first + WXL_SHADOW_B_POINT, api->Sampler(WXL_GFX_VK_SAMPLER_POINT_CLAMP));
        w.SamplerOnly(first + WXL_SHADOW_B_LINEAR, api->Sampler(WXL_GFX_VK_SAMPLER_LINEAR_WRAP));
        const bool live = f.valid;
        const WXL_GfxVkImage& maps = live && f.mapsBound && m.atlas.image != VK_NULL_HANDLE ? m.atlas : m.farMoments;
        w.Sampled(first + WXL_SHADOW_B_MAPS, maps);
        for (uint32_t c = 0; c < WXL_SHADOW_MAX_CASCADES; ++c)
        {
            const WXL_GfxVkImage& img = live && f.cascades[c].image != VK_NULL_HANDLE ? f.cascades[c] : m.oneR32;
            w.Sampled(first + WXL_SHADOW_B_CASCADE0 + c, img);
        }
        w.Sampled(first + WXL_SHADOW_B_HORIZON, live && f.horizon.image != VK_NULL_HANDLE ? f.horizon : m.noHorizon);
        w.Sampled(first + WXL_SHADOW_B_HEIGHTS, live && f.heights.image != VK_NULL_HANDLE ? f.heights : m.noHeights);
        return true;
    }

    bool WriteBindings(VkDescriptorSet set, uint32_t first)
    {
        if (set == VK_NULL_HANDLE || Dev().generation == 0) return false;
        wxl::gfx::vk::DescriptorWriter<32> w(set);
        if (!WriteShadowSet(w, first)) return false;
        w.Apply(Dev());
        return w.Complete();
    }
}
