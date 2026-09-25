// wxl-forever: the extension-wide service table pointer, log macros and config reader.
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

namespace wxl_forever
{
    extern const WXL_Api* g_api;

    /// Environment first, then Extensions\wxl-forever\wxl-forever.cfg.
    inline bool ConfigRaw(const char* name, char* buf, size_t cap)
    {
        return wxl::ext::config::Raw(name, buf, cap, "Extensions\\wxl-forever\\wxl-forever.cfg");
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
}

#define WLOG_INFO(...)  ::wxl_forever::g_api->Log(WXL_LOG_INFO,  "wxl-forever", __VA_ARGS__)
#define WLOG_WARN(...)  ::wxl_forever::g_api->Log(WXL_LOG_WARN,  "wxl-forever", __VA_ARGS__)
#define WLOG_ERROR(...) ::wxl_forever::g_api->Log(WXL_LOG_ERROR, "wxl-forever", __VA_ARGS__)
