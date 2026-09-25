// wxl-graphics-extend: the DDS header parser and the surface decoders (every format to 8-bit BGRA or
// luminance), built without Windows headers so they can be unit-tested natively.
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

#include "Dds.hpp"
#include "DdsFormat.hpp"

#include <cmath>
#include <cstring>

namespace
{
    using namespace wxl::gfx::dds;

    uint32_t Read32(const uint8_t* b, size_t o)
    {
        uint32_t v;
        std::memcpy(&v, b + o, 4);
        return v;
    }

    uint16_t Read16(const uint8_t* b)
    {
        uint16_t v;
        std::memcpy(&v, b, 2);
        return v;
    }

    // --- format identification ----------------------------------------------------------------------

    /// Bytes per texel (or per 4 x 4 block) of a format the reader knows; 0 for one it does not.
    uint32_t BlockBytesOf(uint32_t format, bool& compressed)
    {
        compressed = false;
        switch (format)
        {
        case fmt::kDxt1: case fmt::kAti1:
            compressed = true;
            return 8;
        case fmt::kDxt3: case fmt::kDxt5: case fmt::kAti2:
            compressed = true;
            return 16;
        case fmt::kL8: case fmt::kA8:
            return 1;
        case fmt::kR5G6B5: case fmt::kX1R5G5B5: case fmt::kA1R5G5B5: case fmt::kA4R4G4B4:
        case fmt::kA8L8: case fmt::kL16: case fmt::kR16F:
            return 2;
        case fmt::kR8G8B8:
            return 3;
        case fmt::kA8R8G8B8: case fmt::kX8R8G8B8: case fmt::kA8B8G8R8: case fmt::kX8B8G8R8:
        case fmt::kA2B10G10R10: case fmt::kA2R10G10B10: case fmt::kG16R16: case fmt::kG16R16F: case fmt::kR32F:
            return 4;
        case fmt::kA16B16G16R16: case fmt::kA16B16G16R16F: case fmt::kG32R32F:
            return 8;
        case fmt::kA32B32G32R32F:
            return 16;
        default:
            return 0;
        }
    }

    /// The stored format of a legacy pixel-format block: the FOURCC, else the masks (the DDPF_RGB /
    /// DDPF_LUMINANCE / DDPF_ALPHA flags only break ties; writers disagree on them).
    uint32_t LegacyFormat(uint32_t flags, uint32_t fourcc, uint32_t bits, uint32_t rm, uint32_t gm, uint32_t bm, uint32_t am)
    {
        if (flags & hdr::DDPF_FOURCC)
        {
            switch (fourcc)
            {
            case fmt::kDxt1: return fmt::kDxt1;
            case fmt::kDxt2: case fmt::kDxt3: return fmt::kDxt3;   // premultiplied alpha is the shader's business
            case fmt::kDxt4: case fmt::kDxt5: return fmt::kDxt5;
            case fmt::kAti1: case fmt::kBc4U: return fmt::kAti1;
            case fmt::kAti2: case fmt::kBc5U: return fmt::kAti2;
            case 36: return fmt::kA16B16G16R16;
            case 111: case 112: case 113: case 114: case 115: case 116: return fourcc;
            default: return fmt::kUnknown;
            }
        }
        switch (bits)
        {
        case 32:
            if (rm == 0x00FF0000 && gm == 0x0000FF00 && bm == 0x000000FF) return am == 0xFF000000 ? fmt::kA8R8G8B8 : fmt::kX8R8G8B8;
            if (rm == 0x000000FF && gm == 0x0000FF00 && bm == 0x00FF0000) return am == 0xFF000000 ? fmt::kA8B8G8R8 : fmt::kX8B8G8R8;
            if (rm == 0x000003FF && gm == 0x000FFC00 && bm == 0x3FF00000) return fmt::kA2B10G10R10;
            if (rm == 0x3FF00000 && gm == 0x000FFC00 && bm == 0x000003FF) return fmt::kA2R10G10B10;
            if (rm == 0x0000FFFF && gm == 0xFFFF0000 && bm == 0) return fmt::kG16R16;
            return fmt::kUnknown;
        case 24:
            return rm == 0xFF0000 && gm == 0xFF00 && bm == 0xFF ? fmt::kR8G8B8 : fmt::kUnknown;
        case 16:
            if (rm == 0xF800 && gm == 0x07E0 && bm == 0x001F) return fmt::kR5G6B5;
            if (rm == 0x7C00 && gm == 0x03E0 && bm == 0x001F) return am == 0x8000 ? fmt::kA1R5G5B5 : fmt::kX1R5G5B5;
            if (rm == 0x0F00 && gm == 0x00F0 && bm == 0x000F) return fmt::kA4R4G4B4;
            if (rm == 0x00FF && gm == 0 && am == 0xFF00) return fmt::kA8L8;
            if (rm == 0xFFFF && am == 0) return fmt::kL16;
            return fmt::kUnknown;
        case 8:
            if (rm == 0xFF && am == 0) return fmt::kL8;
            if (rm == 0 && am == 0xFF) return fmt::kA8;
            if (flags & hdr::DDPF_LUMINANCE) return fmt::kL8;
            if (flags & hdr::DDPF_ALPHA) return fmt::kA8;
            return fmt::kUnknown;
        default:
            return fmt::kUnknown;
        }
    }

    /// The stored format of a DX10 header's DXGI_FORMAT (UNORM and SRGB variants alike).
    uint32_t Dx10Format(uint32_t dxgi)
    {
        switch (dxgi)
        {
        case 71: case 72: return fmt::kDxt1;            // BC1_UNORM, BC1_UNORM_SRGB
        case 74: case 75: return fmt::kDxt3;            // BC2
        case 77: case 78: return fmt::kDxt5;            // BC3
        case 80: return fmt::kAti1;                     // BC4_UNORM
        case 83: return fmt::kAti2;                     // BC5_UNORM
        case 28: case 29: return fmt::kA8B8G8R8;        // R8G8B8A8_UNORM(_SRGB): R first in memory
        case 87: case 91: return fmt::kA8R8G8B8;        // B8G8R8A8_UNORM(_SRGB)
        case 88: case 93: return fmt::kX8R8G8B8;        // B8G8R8X8_UNORM(_SRGB)
        case 10: return fmt::kA16B16G16R16F;            // R16G16B16A16_FLOAT
        case 2:  return fmt::kA32B32G32R32F;            // R32G32B32A32_FLOAT
        case 54: return fmt::kR16F;                     // R16_FLOAT
        case 41: return fmt::kR32F;                     // R32_FLOAT
        case 34: return fmt::kG16R16F;                  // R16G16_FLOAT
        case 16: return fmt::kG32R32F;                  // R32G32_FLOAT
        case 61: return fmt::kL8;                       // R8_UNORM
        case 65: return fmt::kA8;                       // A8_UNORM
        case 24: return fmt::kA2B10G10R10;              // R10G10B10A2_UNORM
        case 35: return fmt::kG16R16;                   // R16G16_UNORM
        case 11: return fmt::kA16B16G16R16;             // R16G16B16A16_UNORM
        case 85: return fmt::kR5G6B5;                   // B5G6R5_UNORM
        case 86: return fmt::kA1R5G5B5;                 // B5G5R5A1_UNORM
        case 56: return fmt::kL16;                      // R16_UNORM
        default: return fmt::kUnknown;
        }
    }

    /// What the texture is created with: many D3D9 drivers refuse the RGBA-ordered and 24-bit formats,
    /// so those are converted to their BGRA cousins on upload.
    uint32_t DeviceFormat(uint32_t stored)
    {
        switch (stored)
        {
        case fmt::kA8B8G8R8: return fmt::kA8R8G8B8;
        case fmt::kX8B8G8R8: case fmt::kR8G8B8: return fmt::kX8R8G8B8;
        default: return stored;
        }
    }

    // --- channel expansion ------------------------------------------------------------------------------

    /// n-bit to 8-bit by bit replication (exact at both ends, within half a step elsewhere).
    constexpr uint8_t Expand4(uint32_t v) { return uint8_t(v * 17u); }
    constexpr uint8_t Expand5(uint32_t v) { return uint8_t((v << 3) | (v >> 2)); }
    constexpr uint8_t Expand6(uint32_t v) { return uint8_t((v << 2) | (v >> 4)); }
    /// 10 and 16 bits to 8 with proper rounding (the divisors are odd, so no exact half occurs).
    constexpr uint8_t Expand10(uint32_t v) { return uint8_t((v * 255u + 511u) / 1023u); }
    constexpr uint8_t Expand16(uint32_t v) { return uint8_t((v * 255u + 32767u) / 65535u); }

    /// A float clamped to 0..1 as a byte; NaN (which no comparison passes) and -inf give 0, +inf 255.
    uint8_t UnitByte(float v)
    {
        if (!(v > 0.0f)) return 0;
        if (v >= 1.0f) return 255;
        return uint8_t(v * 255.0f + 0.5f);
    }

    /// IEEE half to float, denormals (m * 2^-24) and infinities included; NaN stays NaN.
    float HalfToFloat(uint16_t h)
    {
        const uint32_t sign = uint32_t(h & 0x8000u) << 16;
        const uint32_t e = (h >> 10) & 31u;
        uint32_t m = h & 1023u;
        uint32_t bits;
        if (e == 0)
        {
            if (m == 0) bits = sign;
            else
            {
                // Normalise: shift the leading one into the implicit position, one exponent per
                // step (the float's exponent field stays positive: at most ten steps from 113).
                int exponent = 113;
                while (!(m & 0x400u)) { m <<= 1; --exponent; }
                bits = sign | (uint32_t(exponent) << 23) | ((m & 0x3FFu) << 13);
            }
        }
        else if (e == 31) bits = sign | 0x7F800000u | (m << 13);
        else bits = sign | ((e + 112u) << 23) | (m << 13);
        float f;
        std::memcpy(&f, &bits, 4);
        return f;
    }

    uint8_t HalfByte(const uint8_t* s) { return UnitByte(HalfToFloat(Read16(s))); }

    uint8_t FloatByte(const uint8_t* s)
    {
        float v;
        std::memcpy(&v, s, 4);
        return UnitByte(v);
    }

    uint8_t Luma(const uint8_t* bgra)
    {
        return uint8_t((bgra[2] * 77u + bgra[1] * 151u + bgra[0] * 28u) >> 8);
    }

    // --- uncompressed rows ------------------------------------------------------------------------------

    /// One row of w texels of an uncompressed stored format to BGRA (the memory order of A8R8G8B8).
    /// The stored channel order is little-endian: R8G8B8 and the BGRA formats keep B first in memory,
    /// the RGBA ones and every multi-channel 16/32-bit format keep R first.
    void DecodeRow(uint32_t format, const uint8_t* s, uint32_t w, uint8_t* d)
    {
        switch (format)
        {
        case fmt::kA8R8G8B8:
            std::memcpy(d, s, size_t(w) * 4);
            return;
        case fmt::kX8R8G8B8:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
            return;
        case fmt::kA8B8G8R8:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
            return;
        case fmt::kX8B8G8R8:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; }
            return;
        case fmt::kR8G8B8:
            for (uint32_t x = 0; x < w; ++x, s += 3, d += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
            return;
        case fmt::kR5G6B5:
            for (uint32_t x = 0; x < w; ++x, s += 2, d += 4)
            {
                const uint32_t v = Read16(s);
                d[0] = Expand5(v & 31u); d[1] = Expand6((v >> 5) & 63u); d[2] = Expand5(v >> 11); d[3] = 255;
            }
            return;
        case fmt::kA1R5G5B5:
        case fmt::kX1R5G5B5:
            for (uint32_t x = 0; x < w; ++x, s += 2, d += 4)
            {
                const uint32_t v = Read16(s);
                d[0] = Expand5(v & 31u); d[1] = Expand5((v >> 5) & 31u); d[2] = Expand5((v >> 10) & 31u);
                d[3] = format == fmt::kX1R5G5B5 || (v & 0x8000u) ? 255 : 0;
            }
            return;
        case fmt::kA4R4G4B4:
            for (uint32_t x = 0; x < w; ++x, s += 2, d += 4)
            {
                const uint32_t v = Read16(s);
                d[0] = Expand4(v & 15u); d[1] = Expand4((v >> 4) & 15u); d[2] = Expand4((v >> 8) & 15u); d[3] = Expand4(v >> 12);
            }
            return;
        case fmt::kA2B10G10R10:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4)
            {
                const uint32_t v = Read32(s, 0);
                d[0] = Expand10((v >> 20) & 1023u); d[1] = Expand10((v >> 10) & 1023u); d[2] = Expand10(v & 1023u);
                d[3] = uint8_t((v >> 30) * 85u);
            }
            return;
        case fmt::kA2R10G10B10:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4)
            {
                const uint32_t v = Read32(s, 0);
                d[0] = Expand10(v & 1023u); d[1] = Expand10((v >> 10) & 1023u); d[2] = Expand10((v >> 20) & 1023u);
                d[3] = uint8_t((v >> 30) * 85u);
            }
            return;
        case fmt::kG16R16:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) { d[0] = 0; d[1] = Expand16(Read16(s + 2)); d[2] = Expand16(Read16(s)); d[3] = 255; }
            return;
        case fmt::kA16B16G16R16:
            for (uint32_t x = 0; x < w; ++x, s += 8, d += 4)
            {
                d[0] = Expand16(Read16(s + 4)); d[1] = Expand16(Read16(s + 2)); d[2] = Expand16(Read16(s)); d[3] = Expand16(Read16(s + 6));
            }
            return;
        case fmt::kL8:
            for (uint32_t x = 0; x < w; ++x, ++s, d += 4) { d[0] = d[1] = d[2] = s[0]; d[3] = 255; }
            return;
        case fmt::kA8:
            // Alpha only: white with the stored coverage, so a blend of it shows the texture's shape.
            for (uint32_t x = 0; x < w; ++x, ++s, d += 4) { d[0] = d[1] = d[2] = 255; d[3] = s[0]; }
            return;
        case fmt::kA8L8:
            for (uint32_t x = 0; x < w; ++x, s += 2, d += 4) { d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; }
            return;
        case fmt::kL16:
            for (uint32_t x = 0; x < w; ++x, s += 2, d += 4) { d[0] = d[1] = d[2] = Expand16(Read16(s)); d[3] = 255; }
            return;
        case fmt::kR16F:
            for (uint32_t x = 0; x < w; ++x, s += 2, d += 4) { d[0] = 0; d[1] = 0; d[2] = HalfByte(s); d[3] = 255; }
            return;
        case fmt::kG16R16F:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) { d[0] = 0; d[1] = HalfByte(s + 2); d[2] = HalfByte(s); d[3] = 255; }
            return;
        case fmt::kA16B16G16R16F:
            for (uint32_t x = 0; x < w; ++x, s += 8, d += 4) { d[0] = HalfByte(s + 4); d[1] = HalfByte(s + 2); d[2] = HalfByte(s); d[3] = HalfByte(s + 6); }
            return;
        case fmt::kR32F:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) { d[0] = 0; d[1] = 0; d[2] = FloatByte(s); d[3] = 255; }
            return;
        case fmt::kG32R32F:
            for (uint32_t x = 0; x < w; ++x, s += 8, d += 4) { d[0] = 0; d[1] = FloatByte(s + 4); d[2] = FloatByte(s); d[3] = 255; }
            return;
        case fmt::kA32B32G32R32F:
            for (uint32_t x = 0; x < w; ++x, s += 16, d += 4) { d[0] = FloatByte(s + 8); d[1] = FloatByte(s + 4); d[2] = FloatByte(s); d[3] = FloatByte(s + 12); }
            return;
        default:
            std::memset(d, 0, size_t(w) * 4);
            return;
        }
    }

    // --- block-compressed blocks ----------------------------------------------------------------------

    /// The four BGRA colours of a BC1 colour block (the last 8 bytes of DXT3/5 blocks, the whole DXT1
    /// block). Only DXT1 has the three-colour mode with the transparent black fourth entry; inside
    /// DXT3/5 the block is always four opaque colours.
    void ColourPalette(const uint8_t* b, bool dxt1, uint8_t p[4][4])
    {
        const uint32_t c0 = Read16(b), c1 = Read16(b + 2);
        p[0][0] = Expand5(c0 & 31u); p[0][1] = Expand6((c0 >> 5) & 63u); p[0][2] = Expand5(c0 >> 11); p[0][3] = 255;
        p[1][0] = Expand5(c1 & 31u); p[1][1] = Expand6((c1 >> 5) & 63u); p[1][2] = Expand5(c1 >> 11); p[1][3] = 255;
        if (c0 > c1 || !dxt1)
        {
            for (int k = 0; k < 3; ++k)
            {
                p[2][k] = uint8_t((2u * p[0][k] + p[1][k] + 1u) / 3u);
                p[3][k] = uint8_t((p[0][k] + 2u * p[1][k] + 1u) / 3u);
            }
            p[2][3] = p[3][3] = 255;
        }
        else
        {
            for (int k = 0; k < 3; ++k) p[2][k] = uint8_t((p[0][k] + p[1][k] + 1u) / 2u);
            p[2][3] = 255;
            p[3][0] = p[3][1] = p[3][2] = p[3][3] = 0;
        }
    }

    /// The 16 values of a BC4-style block (DXT5 alpha, ATI1, each ATI2 half): two end points and
    /// 3-bit indices into 8 interpolated values, or 6 plus 0 and 255 when a0 <= a1.
    void AlphaBlock(const uint8_t* b, uint8_t out[16])
    {
        const uint32_t a0 = b[0], a1 = b[1];
        uint8_t v[8];
        v[0] = uint8_t(a0);
        v[1] = uint8_t(a1);
        if (a0 > a1)
            for (uint32_t k = 2; k < 8; ++k) v[k] = uint8_t(((8u - k) * a0 + (k - 1u) * a1 + 3u) / 7u);
        else
        {
            for (uint32_t k = 2; k < 6; ++k) v[k] = uint8_t(((6u - k) * a0 + (k - 1u) * a1 + 2u) / 5u);
            v[6] = 0;
            v[7] = 255;
        }
        uint64_t bits = 0;
        for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
        for (int i = 0; i < 16; ++i) out[i] = v[(bits >> (3 * i)) & 7u];
    }

    /// One block to 16 BGRA texels, row-major.
    void DecodeBlock(uint32_t format, const uint8_t* b, uint8_t out[16][4])
    {
        switch (format)
        {
        case fmt::kDxt1:
        case fmt::kDxt3:
        case fmt::kDxt5:
        {
            const bool dxt1 = format == fmt::kDxt1;
            const uint8_t* colour = dxt1 ? b : b + 8;
            uint8_t p[4][4];
            ColourPalette(colour, dxt1, p);
            const uint32_t bits = Read32(colour, 4);
            for (int i = 0; i < 16; ++i) std::memcpy(out[i], p[(bits >> (2 * i)) & 3u], 4);
            if (format == fmt::kDxt3)
            {
                const uint64_t a = uint64_t(Read32(b, 0)) | (uint64_t(Read32(b, 4)) << 32);
                for (int i = 0; i < 16; ++i) out[i][3] = Expand4(uint32_t(a >> (4 * i)) & 15u);
            }
            else if (format == fmt::kDxt5)
            {
                uint8_t a[16];
                AlphaBlock(b, a);
                for (int i = 0; i < 16; ++i) out[i][3] = a[i];
            }
            return;
        }
        case fmt::kAti1:
        {
            uint8_t g[16];
            AlphaBlock(b, g);
            for (int i = 0; i < 16; ++i) { out[i][0] = out[i][1] = out[i][2] = g[i]; out[i][3] = 255; }
            return;
        }
        case fmt::kAti2:
        {
            // Two-channel normals: R = x, G = y as stored (unsigned, 128 = 0). B is the z a shader
            // would reconstruct from the unit vector (x, y in -1..1 -> z = sqrt(1 - x^2 - y^2)), mapped
            // back to 0..255 the same way, so the decoded image reads as a normal map.
            uint8_t xs[16], ys[16];
            AlphaBlock(b, xs);
            AlphaBlock(b + 8, ys);
            for (int i = 0; i < 16; ++i)
            {
                const float x = xs[i] / 127.5f - 1.0f, y = ys[i] / 127.5f - 1.0f;
                const float zz = 1.0f - x * x - y * y;
                const float z = zz > 0.0f ? std::sqrt(zz) : 0.0f;
                const float zb = z * 127.5f + 128.0f;   // 0..1 -> 128..255, rounded
                out[i][0] = uint8_t(zb > 255.0f ? 255.0f : zb);
                out[i][1] = ys[i];
                out[i][2] = xs[i];
                out[i][3] = 255;
            }
            return;
        }
        default:
            std::memset(out, 0, 64);
            return;
        }
    }

    /// Decodes one block row (4 texel rows, clipped to the surface) into dst with the given pitch.
    void DecodeBlockRow(const WXL_GfxDdsInfo& info, const uint8_t* src, uint32_t w, uint32_t rows, uint8_t* dst,
                        size_t pitch)
    {
        const uint32_t bw = (w + 3) / 4;
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            uint8_t texels[16][4];
            DecodeBlock(info.fileFormat, src + size_t(bx) * info.blockBytes, texels);
            const uint32_t cw = w - bx * 4 < 4 ? w - bx * 4 : 4;
            for (uint32_t y = 0; y < rows; ++y)
                std::memcpy(dst + size_t(y) * pitch + size_t(bx) * 16, texels[y * 4], size_t(cw) * 4);
        }
    }

    bool Locate(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level, WXL_GfxDdsInfo& info,
                const uint8_t*& surface, uint32_t& w, uint32_t& h)
    {
        if (!bytes || !Parse(bytes, size, &info)) return false;
        uint64_t offset;
        if (!LocateSurface(info, faceOrSlice, level, size, offset, w, h)) return false;
        surface = static_cast<const uint8_t*>(bytes) + size_t(offset);
        return true;
    }
}

namespace wxl::gfx::dds
{
    int Parse(const void* bytes, size_t size, WXL_GfxDdsInfo* out)
    {
        if (!bytes || size < hdr::kLegacyOffset) return 0;
        const uint8_t* b = static_cast<const uint8_t*>(bytes);
        if (Read32(b, 0) != hdr::kMagic || Read32(b, 4) != hdr::kSize) return 0;

        WXL_GfxDdsInfo info{};
        info.structSize = sizeof(WXL_GfxDdsInfo);
        const uint32_t flags = Read32(b, hdr::kFlags);
        info.height = Read32(b, hdr::kHeight);
        info.width = Read32(b, hdr::kWidth);
        const uint32_t depth = Read32(b, hdr::kDepth), mips = Read32(b, hdr::kMips);
        const uint32_t pfFlags = Read32(b, hdr::kPfFlags), fourcc = Read32(b, hdr::kFourCc);
        const uint32_t caps2 = Read32(b, hdr::kCaps2);

        bool cube = false, volume = false;
        if ((pfFlags & hdr::DDPF_FOURCC) && fourcc == fmt::kDx10)
        {
            if (size < hdr::kDx10Offset) return 0;
            const uint32_t dimension = Read32(b, hdr::kDimension), misc = Read32(b, hdr::kMiscFlag);
            const uint32_t arraySize = Read32(b, hdr::kArraySize);
            if (arraySize > 1) return 0;
            if (dimension == hdr::D3D10_RESOURCE_DIMENSION_TEXTURE3D) volume = true;
            else if (dimension == hdr::D3D10_RESOURCE_DIMENSION_TEXTURE2D) cube = (misc & hdr::D3D10_RESOURCE_MISC_TEXTURECUBE) != 0;
            else return 0;
            info.fileFormat = Dx10Format(Read32(b, hdr::kDxgiFormat));
            info.dataOffset = uint32_t(hdr::kDx10Offset);
        }
        else
        {
            info.fileFormat = LegacyFormat(pfFlags, fourcc, Read32(b, hdr::kBits), Read32(b, hdr::kRMask),
                                           Read32(b, hdr::kGMask), Read32(b, hdr::kBMask), Read32(b, hdr::kAMask));
            volume = (caps2 & hdr::DDSCAPS2_VOLUME) != 0;
            if (!volume && (caps2 & hdr::DDSCAPS2_CUBEMAP))
            {
                // A partial cube map has no layout a texture could take.
                if ((caps2 & hdr::DDSCAPS2_CUBEMAP_ALLFACES) != hdr::DDSCAPS2_CUBEMAP_ALLFACES) return 0;
                cube = true;
            }
            info.dataOffset = uint32_t(hdr::kLegacyOffset);
        }

        bool compressed = false;
        info.blockBytes = BlockBytesOf(info.fileFormat, compressed);
        if (info.blockBytes == 0) return 0;
        info.compressed = compressed ? 1u : 0u;
        info.format = DeviceFormat(info.fileFormat);

        if (info.width == 0 || info.height == 0 || info.width > kMaxDim || info.height > kMaxDim) return 0;
        info.kind = volume ? WXL_GFX_DDS_VOLUME : cube ? WXL_GFX_DDS_CUBE : WXL_GFX_DDS_2D;
        if (cube && info.width != info.height) return 0;
        if (volume)
        {
            if (!(flags & hdr::DDSD_DEPTH) || depth == 0 || depth > kMaxDepth) return 0;
            info.depth = depth;
        }
        else info.depth = 1;

        // The mip count is what the file claims, clamped to the full chain of the top level's larger
        // side; a level below that would only repeat the 1 x 1 surface.
        uint32_t maxDim = info.width > info.height ? info.width : info.height;
        if (volume && info.depth > maxDim) maxDim = info.depth;
        uint32_t chain = 1;
        while ((maxDim >> chain) != 0) ++chain;
        info.mips = (flags & hdr::DDSD_MIPMAPCOUNT) && mips ? (mips < chain ? mips : chain) : 1u;

        uint64_t total = 0;
        for (uint32_t l = 0; l < info.mips; ++l) total += LevelBytes(info, l);
        if (info.kind == WXL_GFX_DDS_CUBE) total *= 6;
        if (total == 0 || total > kMaxDataSize || info.dataOffset + total > uint64_t(size)) return 0;
        info.dataSize = uint32_t(total);

        if (out) *out = info;
        return 1;
    }

    bool DecodeBgra(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level, std::vector<uint8_t>& out,
                    uint32_t& width, uint32_t& height)
    {
        WXL_GfxDdsInfo info;
        const uint8_t* s = nullptr;
        uint32_t w, h;
        if (!Locate(bytes, size, faceOrSlice, level, info, s, w, h)) return false;
        out.resize(size_t(w) * h * 4);
        const size_t pitch = size_t(w) * 4;
        if (info.compressed)
        {
            const size_t srcPitch = size_t((w + 3) / 4) * info.blockBytes;
            for (uint32_t by = 0; by < (h + 3) / 4; ++by)
            {
                const uint32_t rows = h - by * 4 < 4 ? h - by * 4 : 4;
                DecodeBlockRow(info, s + size_t(by) * srcPitch, w, rows, out.data() + size_t(by) * 4 * pitch, pitch);
            }
        }
        else
        {
            const size_t srcPitch = size_t(w) * info.blockBytes;
            for (uint32_t y = 0; y < h; ++y) DecodeRow(info.fileFormat, s + size_t(y) * srcPitch, w, out.data() + size_t(y) * pitch);
        }
        width = w;
        height = h;
        return true;
    }

    bool DecodeLuminance(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level, std::vector<uint8_t>& out,
                         uint32_t& width, uint32_t& height)
    {
        WXL_GfxDdsInfo info;
        const uint8_t* s = nullptr;
        uint32_t w, h;
        if (!Locate(bytes, size, faceOrSlice, level, info, s, w, h)) return false;
        out.resize(size_t(w) * h);
        switch (info.fileFormat)
        {
        case fmt::kL8:
        case fmt::kA8:
            std::memcpy(out.data(), s, out.size());
            break;
        case fmt::kA8L8:
            for (size_t i = 0; i < out.size(); ++i) out[i] = s[i * 2];
            break;
        default:
        {
            // Everything else through its colours, one row (or block row) at a time: a scratch of
            // at most 4 x 16384 texels rather than a whole second image.
            const uint32_t strip = info.compressed ? 4u : 1u;
            std::vector<uint8_t> bgra(size_t(w) * strip * 4);
            const size_t pitch = size_t(w) * 4;
            if (info.compressed)
            {
                const size_t srcPitch = size_t((w + 3) / 4) * info.blockBytes;
                for (uint32_t by = 0; by < (h + 3) / 4; ++by)
                {
                    const uint32_t rows = h - by * 4 < 4 ? h - by * 4 : 4;
                    DecodeBlockRow(info, s + size_t(by) * srcPitch, w, rows, bgra.data(), pitch);
                    for (uint32_t y = 0; y < rows; ++y)
                    {
                        uint8_t* d = out.data() + size_t(by * 4 + y) * w;
                        const uint8_t* p = bgra.data() + size_t(y) * pitch;
                        for (uint32_t x = 0; x < w; ++x, p += 4) d[x] = Luma(p);
                    }
                }
            }
            else
            {
                const size_t srcPitch = size_t(w) * info.blockBytes;
                for (uint32_t y = 0; y < h; ++y)
                {
                    DecodeRow(info.fileFormat, s + size_t(y) * srcPitch, w, bgra.data());
                    uint8_t* d = out.data() + size_t(y) * w;
                    const uint8_t* p = bgra.data();
                    for (uint32_t x = 0; x < w; ++x, p += 4) d[x] = Luma(p);
                }
            }
            break;
        }
        }
        width = w;
        height = h;
        return true;
    }

    const char* FormatName(uint32_t format)
    {
        switch (format)
        {
        case fmt::kR8G8B8: return "R8G8B8";
        case fmt::kA8R8G8B8: return "A8R8G8B8";
        case fmt::kX8R8G8B8: return "X8R8G8B8";
        case fmt::kR5G6B5: return "R5G6B5";
        case fmt::kX1R5G5B5: return "X1R5G5B5";
        case fmt::kA1R5G5B5: return "A1R5G5B5";
        case fmt::kA4R4G4B4: return "A4R4G4B4";
        case fmt::kA8: return "A8";
        case fmt::kA2B10G10R10: return "A2B10G10R10";
        case fmt::kA8B8G8R8: return "A8B8G8R8";
        case fmt::kX8B8G8R8: return "X8B8G8R8";
        case fmt::kG16R16: return "G16R16";
        case fmt::kA2R10G10B10: return "A2R10G10B10";
        case fmt::kA16B16G16R16: return "A16B16G16R16";
        case fmt::kL8: return "L8";
        case fmt::kA8L8: return "A8L8";
        case fmt::kL16: return "L16";
        case fmt::kR16F: return "R16F";
        case fmt::kG16R16F: return "G16R16F";
        case fmt::kA16B16G16R16F: return "A16B16G16R16F";
        case fmt::kR32F: return "R32F";
        case fmt::kG32R32F: return "G32R32F";
        case fmt::kA32B32G32R32F: return "A32B32G32R32F";
        case fmt::kDxt1: return "DXT1";
        case fmt::kDxt3: return "DXT3";
        case fmt::kDxt5: return "DXT5";
        case fmt::kAti1: return "ATI1";
        case fmt::kAti2: return "ATI2";
        default: return "?";
        }
    }
}
