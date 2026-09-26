// wxl-graphics-lights: the extension-wide service table pointer, log macros, config reader and the
// lazy lookups of the services it leans on (graphics-extend, its Vulkan side, the shadow service, the fog).
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

#include "wxl/PluginApi.h"
#include "wxl/GraphicsExtendApi.h"
#include "wxl/GraphicsFogApi.h"
#include "wxl/GraphicsShadowApi.h"
#include "wxl/GraphicsVulkanApi.h"
#include "wxl/gfx/Client.hpp"
#include "common/ExtensionConfig.hpp"

#include <cstddef>
#include <cstdlib>

namespace wxl::gfx::lights
{
    /// The core's table, set by WXL_Load before any module is installed.
    extern const WXL_Api* g_api;

    constexpr const char* kTag = "wxl-graphics-lights";

    /// Environment first, then Extensions\wxl-graphics-lights\wxl-graphics-lights.cfg.
    inline bool ConfigRaw(const char* name, char* buf, size_t cap)
    {
        return wxl::ext::config::Raw(name, buf, cap, "Extensions\\wxl-graphics-lights\\wxl-graphics-lights.cfg");
    }

    inline bool ConfigBool(const char* name, bool fallback)
    {
        char buf[32];
        return ConfigRaw(name, buf, sizeof buf) ? wxl::ext::config::Truthy(buf, fallback) : fallback;
    }

    inline float ConfigFloat(const char* name, float fallback, float minValue, float maxValue)
    {
        char buf[32];
        if (!ConfigRaw(name, buf, sizeof buf)) return fallback;
        char* end = nullptr;
        const float v = std::strtof(buf, &end);
        if (end == buf) return fallback;
        return v < minValue ? minValue : (v > maxValue ? maxValue : v);
    }

    inline int ConfigInt(const char* name, int fallback, int minValue, int maxValue)
    {
        char buf[32];
        if (!ConfigRaw(name, buf, sizeof buf)) return fallback;
        char* end = nullptr;
        const long v = std::strtol(buf, &end, 10);
        if (end == buf) return fallback;
        return v < minValue ? minValue : (v > maxValue ? maxValue : int(v));
    }

    /// wxl-graphics-extend's table, or null while it is not loaded (looked up from events, cached).
    inline const WXL_GraphicsExtendApi* Gfx() { return wxl::gfx::Service(g_api); }

    /// An interface looked up lazily: found once and cached, a miss retried every 128 calls.
    template <class T>
    inline const T* Lookup(const char* name, int version)
    {
        static const T* s = nullptr;
        static unsigned misses = 0;
        if (!s && g_api && (misses++ & 127u) == 0) s = static_cast<const T*>(g_api->GetInterface(name, version));
        return s;
    }

    inline const WXL_GfxVulkanApi* Vk() { return Lookup<WXL_GfxVulkanApi>(WXL_GRAPHICS_VULKAN_API_NAME, WXL_GRAPHICS_VULKAN_API_VERSION); }

    /// The shadow service, when loaded and new enough for every function this extension calls.
    inline const WXL_GraphicsShadowApi* Shadow()
    {
        const WXL_GraphicsShadowApi* s = Lookup<WXL_GraphicsShadowApi>(WXL_GRAPHICS_SHADOW_API_NAME, WXL_GRAPHICS_SHADOW_API_VERSION);
        return s && s->structSize >= offsetof(WXL_GraphicsShadowApi, GpuMs) + sizeof(void*) ? s : nullptr;
    }

    inline const WXL_GraphicsFogApi* Fog() { return Lookup<WXL_GraphicsFogApi>(WXL_GRAPHICS_FOG_API_NAME, WXL_GRAPHICS_FOG_API_VERSION); }

    /// Whether the fog draws this frame.
    inline bool FogActive()
    {
        const WXL_GraphicsFogApi* f = Fog();
        return f && f->Active && f->Active() != 0;
    }

    /// The fog's extinction per yard around the camera, 0 without the fog.
    inline float FogExtinction()
    {
        const WXL_GraphicsFogApi* f = Fog();
        if (!f || f->structSize < offsetof(WXL_GraphicsFogApi, CameraDensity) + sizeof(void*) || !f->CameraDensity) return 0.0f;
        if (f->Active && !f->Active()) return 0.0f;
        const float d = f->CameraDensity();
        return d > 0.0f ? (d < 2.0f ? d : 2.0f) : 0.0f;
    }
}

#define LIGHTS_LOG_DEBUG(...) ::wxl::gfx::lights::g_api->Log(WXL_LOG_DEBUG, ::wxl::gfx::lights::kTag, __VA_ARGS__)
#define LIGHTS_LOG_INFO(...)  ::wxl::gfx::lights::g_api->Log(WXL_LOG_INFO,  ::wxl::gfx::lights::kTag, __VA_ARGS__)
#define LIGHTS_LOG_WARN(...)  ::wxl::gfx::lights::g_api->Log(WXL_LOG_WARN,  ::wxl::gfx::lights::kTag, __VA_ARGS__)
#define LIGHTS_LOG_ERROR(...) ::wxl::gfx::lights::g_api->Log(WXL_LOG_ERROR, ::wxl::gfx::lights::kTag, __VA_ARGS__)
