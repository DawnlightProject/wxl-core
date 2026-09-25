// wxl-graphics-extend: DDS files (2D, cube and volume textures with mips) parsed, decoded and created
// on the device.
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

#include "wxl/GraphicsExtendApi.h"

#include <cstddef>
#include <cstdint>
#include <vector>

// The formats and layout are listed on WXL_GfxDdsInfo in GraphicsExtendApi.h. Every function checks
// every size against the bytes it is given: a truncated or hostile file is refused, never read past.
namespace wxl::gfx::dds
{
    int   Parse(const void* bytes, size_t size, WXL_GfxDdsInfo* out);
    void* CreateTexture(void* device, const void* bytes, size_t size, uint32_t pool, const char* name);
    void* LoadTexture(void* device, const char* path, uint32_t pool);
    int   DecodeBgra(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level, WXL_ByteSink* out,
                     uint32_t* width, uint32_t* height);
    int   DecodeLuminance(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level, WXL_ByteSink* out,
                          uint32_t* width, uint32_t* height);

    /// C++ forms of the two decoders for the extension's own use.
    bool DecodeBgra(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level, std::vector<uint8_t>& out,
                    uint32_t& width, uint32_t& height);
    bool DecodeLuminance(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level,
                         std::vector<uint8_t>& out, uint32_t& width, uint32_t& height);

    /// Short name of a D3DFORMAT this reader knows ("DXT5", "A8R8G8B8", ...), "?" otherwise.
    const char* FormatName(uint32_t format);
}
