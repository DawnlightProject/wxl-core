// The d3d9 proxy's reach to wxl-host, through WarcraftXL.dll's WXL_GetHostApi export.
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

#include "wxl/HostApi.h"

#include <windows.h>

namespace wxl::gpu::hostlink
{
    /// The host API table, or null while WarcraftXL.dll has none (not loaded, or older). Looked up once found.
    inline const WXL_HostApi* Api()
    {
        static const WXL_HostApi* api = nullptr;
        if (api) return api;
        HMODULE core = GetModuleHandleA("WarcraftXL.dll");
        if (!core) return nullptr;
        const auto get = reinterpret_cast<WXL_GetHostApiFn>(GetProcAddress(core, "WXL_GetHostApi"));
        const WXL_HostApi* found = get ? get() : nullptr;
        if (found && found->structSize >= sizeof(WXL_HostApi)) api = found;
        return api;
    }

    /// The API when the host is serving, else null.
    inline const WXL_HostApi* Live()
    {
        const WXL_HostApi* api = Api();
        return api && api->IsAvailable() ? api : nullptr;
    }
}
