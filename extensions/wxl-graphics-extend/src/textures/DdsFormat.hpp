// wxl-graphics-extend: the DDS file layout and the D3DFORMAT values the reader knows, spelled out so
// the parser and the decoders build without Windows headers (they are unit-tested natively).
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

// Private to the textures folder. Every size here is computed in 64 bits: the header's fields are
// untrusted, and 16384 x 16384 x 16 bytes already overflows a 32-bit size_t.
namespace wxl::gfx::dds
{
    constexpr uint32_t FourCc(char a, char b, char c, char d)
    {
        return uint32_t(uint8_t(a)) | (uint32_t(uint8_t(b)) << 8) | (uint32_t(uint8_t(c)) << 16)
             | (uint32_t(uint8_t(d)) << 24);
    }

    /// D3DFORMAT values (d3d9types.h) and the FOURCC formats, as numbers.
    namespace fmt
    {
        constexpr uint32_t kUnknown       = 0;
        constexpr uint32_t kR8G8B8        = 20;
        constexpr uint32_t kA8R8G8B8      = 21;
        constexpr uint32_t kX8R8G8B8      = 22;
        constexpr uint32_t kR5G6B5        = 23;
        constexpr uint32_t kX1R5G5B5      = 24;
        constexpr uint32_t kA1R5G5B5      = 25;
        constexpr uint32_t kA4R4G4B4      = 26;
        constexpr uint32_t kA8            = 28;
        constexpr uint32_t kA2B10G10R10   = 31;
        constexpr uint32_t kA8B8G8R8      = 32;
        constexpr uint32_t kX8B8G8R8      = 33;
        constexpr uint32_t kG16R16        = 34;
        constexpr uint32_t kA2R10G10B10   = 35;
        constexpr uint32_t kA16B16G16R16  = 36;
        constexpr uint32_t kL8            = 50;
        constexpr uint32_t kA8L8          = 51;
        constexpr uint32_t kL16           = 81;
        constexpr uint32_t kR16F          = 111;
        constexpr uint32_t kG16R16F       = 112;
        constexpr uint32_t kA16B16G16R16F = 113;
        constexpr uint32_t kR32F          = 114;
        constexpr uint32_t kG32R32F       = 115;
        constexpr uint32_t kA32B32G32R32F = 116;
        constexpr uint32_t kDxt1          = FourCc('D', 'X', 'T', '1');
        constexpr uint32_t kDxt2          = FourCc('D', 'X', 'T', '2');
        constexpr uint32_t kDxt3          = FourCc('D', 'X', 'T', '3');
        constexpr uint32_t kDxt4          = FourCc('D', 'X', 'T', '4');
        constexpr uint32_t kDxt5          = FourCc('D', 'X', 'T', '5');
        constexpr uint32_t kAti1          = FourCc('A', 'T', 'I', '1');
        constexpr uint32_t kAti2          = FourCc('A', 'T', 'I', '2');
        constexpr uint32_t kBc4U          = FourCc('B', 'C', '4', 'U');
        constexpr uint32_t kBc5U          = FourCc('B', 'C', '5', 'U');
        constexpr uint32_t kDx10          = FourCc('D', 'X', '1', '0');
    }

    /// The header ("DDS ", 124 bytes), the optional DX10 header (20 bytes) and the field offsets.
    namespace hdr
    {
        constexpr uint32_t kMagic        = 0x20534444;   // "DDS "
        constexpr uint32_t kSize         = 124;
        constexpr size_t   kLegacyOffset = 128;          // data after the legacy header
        constexpr size_t   kDx10Offset   = 148;          // data after the DX10 header

        constexpr size_t kFlags = 8, kHeight = 12, kWidth = 16, kDepth = 24, kMips = 28;
        constexpr size_t kPfFlags = 80, kFourCc = 84, kBits = 88, kRMask = 92, kGMask = 96, kBMask = 100, kAMask = 104;
        constexpr size_t kCaps2 = 112;
        constexpr size_t kDxgiFormat = 128, kDimension = 132, kMiscFlag = 136, kArraySize = 140;

        constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000, DDSD_DEPTH = 0x800000;
        constexpr uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40, DDPF_LUMINANCE = 0x20000;
        constexpr uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_CUBEMAP_ALLFACES = 0xFC00, DDSCAPS2_VOLUME = 0x200000;
        constexpr uint32_t D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3, D3D10_RESOURCE_DIMENSION_TEXTURE3D = 4;
        constexpr uint32_t D3D10_RESOURCE_MISC_TEXTURECUBE = 0x4;
    }

    /// The largest texture the reader takes; the device's own limits are lower still.
    constexpr uint32_t kMaxDim = 16384, kMaxDepth = 4096;

    /// Bytes of one whole file the reader takes: the offset plus every surface fits a uint32_t.
    constexpr uint64_t kMaxDataSize = 0xFFFFFFFFull;

    constexpr uint32_t MipDim(uint32_t v, uint32_t level)
    {
        return (v >> level) ? (v >> level) : 1u;
    }

    /// Row pitch and row count of one w x h surface: block rows when compressed, texel rows otherwise.
    inline void SurfaceLayout(const WXL_GfxDdsInfo& info, uint32_t w, uint32_t h, uint64_t& pitch, uint64_t& rows)
    {
        if (info.compressed)
        {
            pitch = uint64_t((w + 3) / 4) * info.blockBytes;
            rows = uint64_t((h + 3) / 4);
        }
        else
        {
            pitch = uint64_t(w) * info.blockBytes;
            rows = h;
        }
    }

    inline uint64_t SurfaceBytes(const WXL_GfxDdsInfo& info, uint32_t w, uint32_t h)
    {
        uint64_t pitch, rows;
        SurfaceLayout(info, w, h, pitch, rows);
        return pitch * rows;
    }

    /// Bytes of one mip level with every slice (one surface unless a volume).
    inline uint64_t LevelBytes(const WXL_GfxDdsInfo& info, uint32_t level)
    {
        return SurfaceBytes(info, MipDim(info.width, level), MipDim(info.height, level)) * MipDim(info.depth, level);
    }

    /// Where one surface starts, from the start of the file: every mip of each cube face in turn,
    /// every slice of each mip for a volume. faceOrSlice and level must be in range.
    inline uint64_t SurfaceOffset(const WXL_GfxDdsInfo& info, uint32_t faceOrSlice, uint32_t level)
    {
        uint64_t o = info.dataOffset;
        if (info.kind == WXL_GFX_DDS_CUBE)
        {
            uint64_t face = 0;
            for (uint32_t l = 0; l < info.mips; ++l) face += LevelBytes(info, l);
            o += face * faceOrSlice;
            for (uint32_t l = 0; l < level; ++l) o += LevelBytes(info, l);
            return o;
        }
        for (uint32_t l = 0; l < level; ++l) o += LevelBytes(info, l);
        return o + SurfaceBytes(info, MipDim(info.width, level), MipDim(info.height, level)) * faceOrSlice;
    }

    /// Locates one surface: false when the face or slice or the level is out of range, or when the
    /// surface would reach past `size` bytes (a Parse that succeeded on those bytes makes this a
    /// formality, but the decoders never rely on that).
    inline bool LocateSurface(const WXL_GfxDdsInfo& info, uint32_t faceOrSlice, uint32_t level, size_t size,
                              uint64_t& offset, uint32_t& w, uint32_t& h)
    {
        if (level >= info.mips) return false;
        const uint32_t surfaces = info.kind == WXL_GFX_DDS_CUBE ? 6u
                                : info.kind == WXL_GFX_DDS_VOLUME ? MipDim(info.depth, level) : 1u;
        if (faceOrSlice >= surfaces) return false;
        w = MipDim(info.width, level);
        h = MipDim(info.height, level);
        offset = SurfaceOffset(info, faceOrSlice, level);
        return offset + SurfaceBytes(info, w, h) <= uint64_t(size);
    }
}
