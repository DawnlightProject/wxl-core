// wxl-graphics-shadow: the core's table, the services shadows are built on, the config reader and the log.
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
#include "wxl/GraphicsLightsApi.h"
#include "wxl/GraphicsShadowApi.h"
#include "wxl/GraphicsVulkanApi.h"
#include "wxl/gfx/Client.hpp"
#include "common/ExtensionConfig.hpp"

#include <cstdlib>

namespace wxl::gfx::shadow
{
    /// The core's table, set by WXL_Load before anything is installed.
    extern const WXL_Api* g_api;

    constexpr const char* kTag = "wxl-graphics-shadow";
    constexpr const char* kConfigPath = "Extensions\\wxl-graphics-shadow\\wxl-graphics-shadow.cfg";

    /// Environment first, then Extensions\wxl-graphics-shadow\wxl-graphics-shadow.cfg.
    inline bool ConfigRaw(const char* name, char* buf, size_t cap)
    {
        return wxl::ext::config::Raw(name, buf, cap, kConfigPath);
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

    // The services, resolved lazily: extensions load in folder order. Null while absent.

    inline const WXL_GraphicsExtendApi* Gfx() { return wxl::gfx::Service(g_api); }

    inline const WXL_GfxVulkanApi* Vk()
    {
        static const WXL_GfxVulkanApi* s = nullptr;
        static uint32_t misses = 0;
        if (!s && g_api && (misses++ & 63u) == 0)
            s = static_cast<const WXL_GfxVulkanApi*>(g_api->GetInterface(WXL_GRAPHICS_VULKAN_API_NAME,
                                                                         WXL_GRAPHICS_VULKAN_API_VERSION));
        return s;
    }

    inline const WXL_GraphicsLightsApi* Lights()
    {
        static const WXL_GraphicsLightsApi* s = nullptr;
        static uint32_t misses = 0;
        if (!s && g_api && (misses++ & 63u) == 0)
            s = static_cast<const WXL_GraphicsLightsApi*>(g_api->GetInterface(WXL_GRAPHICS_LIGHTS_API_NAME,
                                                                              WXL_GRAPHICS_LIGHTS_API_VERSION));
        return s;
    }
}

#define SHADOW_LOG_DEBUG(...) ::wxl::gfx::shadow::g_api->Log(WXL_LOG_DEBUG, ::wxl::gfx::shadow::kTag, __VA_ARGS__)
#define SHADOW_LOG_INFO(...)  ::wxl::gfx::shadow::g_api->Log(WXL_LOG_INFO,  ::wxl::gfx::shadow::kTag, __VA_ARGS__)
#define SHADOW_LOG_WARN(...)  ::wxl::gfx::shadow::g_api->Log(WXL_LOG_WARN,  ::wxl::gfx::shadow::kTag, __VA_ARGS__)
#define SHADOW_LOG_ERROR(...) ::wxl::gfx::shadow::g_api->Log(WXL_LOG_ERROR, ::wxl::gfx::shadow::kTag, __VA_ARGS__)
