// wxl::gfx client side: resolves the wxl-graphics-extend service lazily and adapts its C ABI to C++.
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

#include <cstdint>
#include <string>
#include <vector>

// Header-only; compiles into the consumer. Typical use:
//
//     const WXL_GraphicsExtendApi* gfx = wxl::gfx::Service(api);   // null until the service loaded
//     if (gfx && !g_pass) {
//         WXL_GfxPassDesc d{ sizeof d, "my-effect", WXL_GFX_ORDER_EFFECTS, &Wants, nullptr, &Draw, nullptr };
//         g_pass = gfx->AddPass(&d);
//     }
//
// Extensions load in folder order, so call Service from an event (the first OnFrame, OnWorldSceneBegin,
// ...), not only from WXL_Load: a consumer whose folder sorts before wxl-graphics-extend runs its
// WXL_Load before the service is published.
namespace wxl::gfx
{
    /**
     * @brief The service table, or null while wxl-graphics-extend is not loaded.
     *
     * Found once, then cached for the DLL. A miss is retried at most every 64 calls, so polling it
     * every frame costs nothing when the service is absent.
     */
    inline const WXL_GraphicsExtendApi* Service(const WXL_Api* api)
    {
        static const WXL_GraphicsExtendApi* s_service = nullptr;
        static uint32_t s_misses = 0;
        if (s_service || !api || !api->GetInterface) return s_service;
        if ((s_misses++ & 63u) != 0) return nullptr;
        s_service = static_cast<const WXL_GraphicsExtendApi*>(
            api->GetInterface(WXL_GRAPHICS_EXTEND_API_NAME, WXL_GRAPHICS_EXTEND_API_VERSION));
        return s_service;
    }

    /// True when the table carries the field (appended in a later version): check before calling it.
    template <class Field>
    inline bool Has(const WXL_GraphicsExtendApi* api, Field WXL_GraphicsExtendApi::* field)
    {
        if (!api) return false;
        const auto* base = reinterpret_cast<const unsigned char*>(api);
        const auto* at = reinterpret_cast<const unsigned char*>(&(api->*field));
        return size_t(at - base) + sizeof(Field) <= api->structSize;
    }

    /// A WXL_ByteSink appending to a std::string or std::vector<uint8_t> / <char>. Keep the container
    /// alive for as long as the sink is used.
    template <class Container>
    inline WXL_ByteSink SinkTo(Container& c)
    {
        struct Writer
        {
            static void __cdecl Write(void* ctx, const void* data, unsigned int len)
            {
                auto& out = *static_cast<Container*>(ctx);
                const auto* p = static_cast<const typename Container::value_type*>(data);
                out.insert(out.end(), p, p + len);
            }
        };
        return WXL_ByteSink{ &c, &Writer::Write };
    }

    /// Reads a whole client file (archives, then loose) into out. False when absent or no service.
    inline bool ReadClientFile(const WXL_GraphicsExtendApi* api, const char* path, std::string& out)
    {
        out.clear();
        if (!api) return false;
        WXL_ByteSink sink = SinkTo(out);
        return api->ReadClientFile(path, &sink) != 0;
    }

    /// Compiles HLSL through the service's cached compiler. False when absent or on error (logged).
    inline bool CompileShader(const WXL_GraphicsExtendApi* api, const WXL_GfxShaderDesc& desc,
                              std::string& bytecode)
    {
        bytecode.clear();
        if (!api) return false;
        WXL_ByteSink sink = SinkTo(bytecode);
        return api->CompileShader(&desc, &sink) != 0;
    }
}
