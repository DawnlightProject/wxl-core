// wxl-forever: entry points. Installs each render feature of the suite; no behaviour lives here.
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

#include "core/ExtensionApi.hpp"
#include "fog/Fog.hpp"
#include "lights/Lights.hpp"
#include "core/Passes.hpp"
#include "surface/Surface.hpp"
#include "post/Post.hpp"
#include "terrain/Terrain.hpp"
#include "core/ForeverUi.hpp"

#include "wxl/EventScript.hpp"

namespace wxl_forever
{
    const WXL_Api* g_api = nullptr;
}

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-forever",
        1,
        WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;

    wxl_forever::g_api = api;

    // Bind before the first subclass exists: EventScript has no table until it is handed one.
    wxl::ext::EventScript::Bind(api);

    // Services first, then the features built on them; each feature works without the others.
    wxl::forever::passes::Install();
    wxl::forever::lights::Install();
    wxl::forever::surface::Install();
    wxl::forever::fog::Install();
    wxl::forever::post::Install();
    wxl::forever::terrain::Install();
    wxl::forever::ui::InstallWindow();

    api->Log(WXL_LOG_INFO, "wxl-forever", "ready: lights, surface, fog, post, terrain");
    return 1;
}
