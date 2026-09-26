// wxl-graphics-shadow: the published C table (include/wxl/GraphicsShadowApi.h). Every entry catches, so
// nothing with a C++ ABI crosses back to a consumer.
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

#include "Api.hpp"
#include "../Shadow.hpp"
#include "../core/Extension.hpp"
#include "../gpu/Gpu.hpp"

#include "../../shaders/wxl/shadow/layout.h"

#include <algorithm>
#include <cstring>

namespace
{
    namespace sh = wxl::gfx::shadow;

    static_assert(WXL_GFX_SHADOW_SLOTS == WXL_SHADOW_MAX_SLOTS, "the header and the shaders agree on the slots");
    static_assert(WXL_GFX_SHADOW_MAPS == WXL_SHADOW_MAX_MAPS, "the header and the shaders agree on the maps");
    static_assert(WXL_GFX_SHADOW_CAPSULES == WXL_SHADOW_MAX_CAPSULES, "the header and the shaders agree on the capsules");
    static_assert(WXL_GFX_SHADOW_CASCADES == WXL_SHADOW_MAX_CASCADES, "the header and the shaders agree on the cascades");

    void __cdecl ApiWant(uint32_t what)
    {
        try { sh::NoteWant(what); } catch (...) {}
    }

    void __cdecl ApiSetLights(const WXL_GfxShadowLight* lights, int count)
    {
        try { sh::SetPushedLights(lights, lights ? std::max(count, 0) : 0); } catch (...) {}
    }

    int __cdecl ApiSlots(WXL_GfxShadowSlot* out, int max)
    {
        if (!out || max <= 0) return 0;
        const sh::Published& p = sh::Pub();
        const int n = std::min(max, WXL_GFX_SHADOW_SLOTS);
        std::memcpy(out, p.slots, sizeof(WXL_GfxShadowSlot) * size_t(n));
        return n;
    }

    int __cdecl ApiSlotOf(uint32_t lightId)
    {
        if (!lightId) return -1;
        const sh::Published& p = sh::Pub();
        for (int s = 0; s < WXL_GFX_SHADOW_SLOTS; ++s)
            if (p.slots[s].lightId == lightId && p.slots[s].weight > 0.0f) return s;
        return -1;
    }

    int __cdecl ApiGetFrame(WXL_GfxShadowFrame* out)
    {
        if (!out || out->structSize < offsetof(WXL_GfxShadowFrame, slots)) return 0;
        const sh::Published& p = sh::Pub();
        if (!p.ran) return 0;
        WXL_GfxShadowFrame f{};
        f.structSize = sizeof f;
        f.frameIndex = p.frameIndex;
        f.flags = p.flags;
        f.width = p.width;
        f.height = p.height;
        f.masks = p.masks;
        std::memcpy(f.toSun, p.toSun, sizeof f.toSun);
        f.sunWeight = p.sunWeight;
        std::memcpy(f.toMoon, p.toMoon, sizeof f.toMoon);
        f.moonWeight = p.moonWeight;
        f.slotCount = WXL_GFX_SHADOW_SLOTS;
        std::memcpy(f.slots, p.slots, sizeof f.slots);
        const uint32_t size = std::min<uint32_t>(out->structSize, sizeof f);
        std::memcpy(out, &f, size);
        out->structSize = size;
        return 1;
    }

    uint32_t __cdecl ApiBindingCount(void) { return WXL_SHADOW_B_COUNT; }

    uint32_t __cdecl ApiDescribeBindings(uint32_t first, WXL_GfxVkBinding* out, uint32_t max)
    {
        if (!out) return 0;
        const uint32_t n = std::min<uint32_t>(max, WXL_SHADOW_B_COUNT);
        for (uint32_t i = 0; i < n; ++i)
        {
            out[i].binding = first + i;
            out[i].count = 1;
            out[i].type = i == WXL_SHADOW_B_CONSTANTS                           ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                        : i == WXL_SHADOW_B_POINT || i == WXL_SHADOW_B_LINEAR ? VK_DESCRIPTOR_TYPE_SAMPLER
                                                                              : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
        return n;
    }

    int __cdecl ApiWriteBindings(VkDescriptorSet set, uint32_t first)
    {
        try { return sh::gpu::WriteBindings(set, first) ? 1 : 0; } catch (...) { return 0; }
    }

    const char* __cdecl ApiStatus(void)
    {
        try { return sh::StatusLine(); } catch (...) { return "graphics shadow: status unavailable"; }
    }

    float __cdecl ApiGpuMs(void)
    {
        try { return sh::gpu::TotalMs(); } catch (...) { return -1.0f; }
    }

    const WXL_GraphicsShadowApi g_table = {
        sizeof(WXL_GraphicsShadowApi), WXL_GRAPHICS_SHADOW_API_VERSION,
        &ApiWant, &ApiSetLights, &ApiSlots, &ApiSlotOf, &ApiGetFrame,
        &ApiBindingCount, &ApiDescribeBindings, &ApiWriteBindings,
        &ApiStatus, &ApiGpuMs,
    };
}

namespace wxl::gfx::shadow::api
{
    void Publish()
    {
        g_api->PublishInterface(WXL_GRAPHICS_SHADOW_API_NAME, WXL_GRAPHICS_SHADOW_API_VERSION,
                                const_cast<WXL_GraphicsShadowApi*>(&g_table));
    }
}
