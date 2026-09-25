// wxl-forever: DDS files (2D, cube and volume textures with mips) parsed and created on the device.
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

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <string>
#include <vector>

// The legacy DDS layout (no DX10 header), as 5.tools/forever-bake writes it: "DDS ", a 124-byte
// header, then every mip of each cube face in +X -X +Y -Y +Z -Z order, or every slice of each mip
// for a volume. Formats: L8, A8, A8L8, A8R8G8B8 (X8R8G8B8 read as it), R16F, G16R16F,
// A16B16G16R16F, R32F, G32R32F, A32B32G32R32F, DXT1, DXT3, DXT5.
namespace wxl::forever::dds
{
    enum class Kind : uint8_t { Texture2D = 0, Cube = 1, Volume = 2 };

    struct Info
    {
        Kind      kind;
        unsigned  width, height, depth;   // depth 1 unless a volume
        unsigned  mips;
        D3DFORMAT format;
        unsigned  blockBytes;             // bytes per texel, or per 4 x 4 block for DXT
        bool      compressed;
        size_t    dataOffset;             // 128
        size_t    dataSize;               // every surface, as the header implies
    };

    /// Reads the header; false when the bytes are not a DDS file this loader takes.
    bool Parse(const std::string& bytes, Info& out);

    /// Bytes of one surface w x h in this format (a whole mip level of one face or slice).
    size_t SurfaceBytes(const Info& info, unsigned w, unsigned h);

    /// Where one surface starts: face (cube) or slice (volume) and mip level.
    size_t SurfaceOffset(const Info& info, unsigned faceOrSlice, unsigned level);

    /// Creates the texture (2D, cube or volume, all mips) in the given pool and uploads the data.
    /// Null when the device refuses the format or size; the reason goes to the log.
    IDirect3DBaseTexture9* Create(IDirect3DDevice9* dev, const std::string& bytes, const Info& info,
                                  D3DPOOL pool = D3DPOOL_MANAGED, const char* name = "");

    /// Decodes one surface to 8-bit luminance (w x h bytes): L8, A8, A8L8, A8R8G8B8 and the DXT
    /// formats; float formats take the red channel clamped to 0..1. For packing into an atlas.
    bool DecodeLuminance(const std::string& bytes, const Info& info, unsigned faceOrSlice, unsigned level,
                         std::vector<uint8_t>& out, unsigned& w, unsigned& h);

    /// Decodes one surface to 8-bit BGRX (w x h x 4 bytes, X8R8G8B8 in memory): A8R8G8B8 and
    /// X8R8G8B8 as they are, every other format its luminance in all three channels.
    bool DecodeColour(const std::string& bytes, const Info& info, unsigned faceOrSlice, unsigned level,
                      std::vector<uint8_t>& out, unsigned& w, unsigned& h);

    const char* FormatName(D3DFORMAT f);
}
