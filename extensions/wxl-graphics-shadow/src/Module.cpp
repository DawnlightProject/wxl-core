// wxl-graphics-shadow: entry points. Binds the tables and installs the service; no behaviour lives here.
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

#include "core/Extension.hpp"
#include "Shadow.hpp"

#include "wxl/EventScript.hpp"
#include "wxl/gfx/Ui.hpp"

namespace wxl::gfx::shadow
{
    const WXL_Api* g_api = nullptr;
}

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-graphics-shadow",
        1,
        WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;

    wxl::gfx::shadow::g_api = api;
    // Bound before the first subclass exists: EventScript has no table until it is handed one.
    wxl::ext::EventScript::Bind(api);
    wxl::gfx::ui::Bind(api);

    // Nothing with a C++ ABI may cross back into the core (PluginApi.h).
    try
    {
        wxl::gfx::shadow::Install();
    }
    catch (...)
    {
        api->Log(WXL_LOG_ERROR, wxl::gfx::shadow::kTag, "install failed: unexpected exception");
        return 0;
    }
    api->Log(WXL_LOG_INFO, wxl::gfx::shadow::kTag, "ready: published %s v%d", WXL_GRAPHICS_SHADOW_API_NAME,
             WXL_GRAPHICS_SHADOW_API_VERSION);
    return 1;
}
