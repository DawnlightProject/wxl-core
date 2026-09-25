// wxl-graphics-extend: the extension-wide service table pointer, log macros and config reader.
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
#include "common/ExtensionConfig.hpp"

#include <cstdlib>

namespace wxl::gfx
{
    /// The core's table, set by WXL_Load before any module is installed.
    extern const WXL_Api* g_api;

    constexpr const char* kTag = "wxl-graphics-extend";

    /// Environment first, then Extensions\wxl-graphics-extend\wxl-graphics-extend.cfg.
    inline bool ConfigRaw(const char* name, char* buf, size_t cap)
    {
        return wxl::ext::config::Raw(name, buf, cap, "Extensions\\wxl-graphics-extend\\wxl-graphics-extend.cfg");
    }

    inline bool ConfigBool(const char* name, bool fallback)
    {
        char buf[32];
        return ConfigRaw(name, buf, sizeof buf) ? wxl::ext::config::Truthy(buf, fallback) : fallback;
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
}

#define GFX_LOG_DEBUG(...) ::wxl::gfx::g_api->Log(WXL_LOG_DEBUG, ::wxl::gfx::kTag, __VA_ARGS__)
#define GFX_LOG_INFO(...)  ::wxl::gfx::g_api->Log(WXL_LOG_INFO,  ::wxl::gfx::kTag, __VA_ARGS__)
#define GFX_LOG_WARN(...)  ::wxl::gfx::g_api->Log(WXL_LOG_WARN,  ::wxl::gfx::kTag, __VA_ARGS__)
#define GFX_LOG_ERROR(...) ::wxl::gfx::g_api->Log(WXL_LOG_ERROR, ::wxl::gfx::kTag, __VA_ARGS__)
