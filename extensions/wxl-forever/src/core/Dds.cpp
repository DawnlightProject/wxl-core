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

#include "ExtensionApi.hpp"
#include "Dds.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    constexpr uint32_t kMagic = 0x20534444;   // "DDS "
    constexpr uint32_t DDSD_MIPMAPCOUNT = 0x20000, DDSD_DEPTH = 0x800000;
    constexpr uint32_t DDPF_ALPHAPIXELS = 0x1, DDPF_ALPHA = 0x2, DDPF_FOURCC = 0x4, DDPF_RGB = 0x40, DDPF_LUMINANCE = 0x20000;
    constexpr uint32_t DDSCAPS2_CUBEMAP = 0x200, DDSCAPS2_VOLUME = 0x200000;

    template <class T>
    T At(const std::string& b, size_t o)
    {
        T v{};
        std::memcpy(&v, b.data() + o, sizeof v);
        return v;
    }

    unsigned MipDim(unsigned v, unsigned level) { return std::max(1u, v >> level); }

    /// The format from the pixel-format block; UNKNOWN when unsupported.
    D3DFORMAT FormatOf(uint32_t flags, uint32_t fourcc, uint32_t bits, uint32_t rm, uint32_t gm, uint32_t bm, uint32_t am,
                       unsigned& blockBytes, bool& compressed)
    {
        compressed = false;
        if (flags & DDPF_FOURCC)
        {
            switch (fourcc)
            {
            case 0x31545844: compressed = true; blockBytes = 8;  return D3DFMT_DXT1;
            case 0x33545844: compressed = true; blockBytes = 16; return D3DFMT_DXT3;
            case 0x35545844: compressed = true; blockBytes = 16; return D3DFMT_DXT5;
            case 111: blockBytes = 2;  return D3DFMT_R16F;
            case 112: blockBytes = 4;  return D3DFMT_G16R16F;
            case 113: blockBytes = 8;  return D3DFMT_A16B16G16R16F;
            case 114: blockBytes = 4;  return D3DFMT_R32F;
            case 115: blockBytes = 8;  return D3DFMT_G32R32F;
            case 116: blockBytes = 16; return D3DFMT_A32B32G32R32F;
            default: return D3DFMT_UNKNOWN;
            }
        }
        if ((flags & DDPF_RGB) && bits == 32 && rm == 0xFF0000 && gm == 0xFF00 && bm == 0xFF)
        {
            blockBytes = 4;
            return (flags & DDPF_ALPHAPIXELS) && am == 0xFF000000 ? D3DFMT_A8R8G8B8 : D3DFMT_X8R8G8B8;
        }
        if ((flags & DDPF_LUMINANCE) && bits == 8) { blockBytes = 1; return D3DFMT_L8; }
        if ((flags & DDPF_LUMINANCE) && bits == 16 && (flags & DDPF_ALPHAPIXELS)) { blockBytes = 2; return D3DFMT_A8L8; }
        if ((flags & DDPF_ALPHA) && bits == 8) { blockBytes = 1; return D3DFMT_A8; }
        return D3DFMT_UNKNOWN;
    }

    void Copy(void* dst, int dstPitch, const uint8_t* src, size_t srcPitch, size_t rows)
    {
        for (size_t r = 0; r < rows; ++r)
            std::memcpy(static_cast<uint8_t*>(dst) + r * size_t(dstPitch), src + r * srcPitch, srcPitch);
    }

    /// Row pitch and row count of one surface in this format.
    void Layout(const wxl::forever::dds::Info& info, unsigned w, unsigned h, size_t& pitch, size_t& rows)
    {
        if (info.compressed)
        {
            pitch = size_t((w + 3) / 4) * info.blockBytes;
            rows = size_t((h + 3) / 4);
        }
        else
        {
            pitch = size_t(w) * info.blockBytes;
            rows = h;
        }
    }

    uint8_t Lum(uint8_t r, uint8_t g, uint8_t b) { return uint8_t((r * 77 + g * 151 + b * 28) >> 8); }

    void Rgb565(uint16_t c, int rgb[3])
    {
        rgb[0] = ((c >> 11) & 31) * 255 / 31;
        rgb[1] = ((c >> 5) & 63) * 255 / 63;
        rgb[2] = (c & 31) * 255 / 31;
    }

    /// One DXT colour block (the last 8 bytes of a DXT3/5 block, the whole DXT1 block) to 16 luminances.
    void DecodeColourBlock(const uint8_t* b, bool dxt1, uint8_t out[16])
    {
        const uint16_t c0 = uint16_t(b[0] | (b[1] << 8)), c1 = uint16_t(b[2] | (b[3] << 8));
        int p[4][3];
        Rgb565(c0, p[0]);
        Rgb565(c1, p[1]);
        if (c0 > c1 || !dxt1)
            for (int k = 0; k < 3; ++k) { p[2][k] = (2 * p[0][k] + p[1][k]) / 3; p[3][k] = (p[0][k] + 2 * p[1][k]) / 3; }
        else
            for (int k = 0; k < 3; ++k) { p[2][k] = (p[0][k] + p[1][k]) / 2; p[3][k] = 0; }
        const uint32_t bits = uint32_t(b[4]) | (uint32_t(b[5]) << 8) | (uint32_t(b[6]) << 16) | (uint32_t(b[7]) << 24);
        for (int i = 0; i < 16; ++i)
        {
            const int* c = p[(bits >> (2 * i)) & 3];
            out[i] = Lum(uint8_t(c[0]), uint8_t(c[1]), uint8_t(c[2]));
        }
    }
}

namespace wxl::forever::dds
{
    bool Parse(const std::string& b, Info& out)
    {
        if (b.size() < 128 || At<uint32_t>(b, 0) != kMagic || At<uint32_t>(b, 4) != 124) return false;
        const uint32_t flags = At<uint32_t>(b, 8);
        out.height = At<uint32_t>(b, 12);
        out.width = At<uint32_t>(b, 16);
        const uint32_t depth = At<uint32_t>(b, 24);
        const uint32_t mips = At<uint32_t>(b, 28);
        const uint32_t pfFlags = At<uint32_t>(b, 80), fourcc = At<uint32_t>(b, 84), bits = At<uint32_t>(b, 88);
        const uint32_t rm = At<uint32_t>(b, 92), gm = At<uint32_t>(b, 96), bm = At<uint32_t>(b, 100), am = At<uint32_t>(b, 104);
        const uint32_t caps2 = At<uint32_t>(b, 112);
        out.format = FormatOf(pfFlags, fourcc, bits, rm, gm, bm, am, out.blockBytes, out.compressed);
        if (out.format == D3DFMT_UNKNOWN || out.width == 0 || out.height == 0) return false;
        out.kind = (caps2 & DDSCAPS2_VOLUME) ? Kind::Volume : ((caps2 & DDSCAPS2_CUBEMAP) ? Kind::Cube : Kind::Texture2D);
        out.depth = (out.kind == Kind::Volume && (flags & DDSD_DEPTH) && depth) ? depth : 1;
        out.mips = (flags & DDSD_MIPMAPCOUNT) && mips ? mips : 1;
        out.dataOffset = 128;
        size_t per = 0;
        for (unsigned l = 0; l < out.mips; ++l)
            per += SurfaceBytes(out, MipDim(out.width, l), MipDim(out.height, l)) * MipDim(out.depth, l);
        out.dataSize = per * (out.kind == Kind::Cube ? 6 : 1);
        return out.dataOffset + out.dataSize <= b.size();
    }

    size_t SurfaceBytes(const Info& info, unsigned w, unsigned h)
    {
        size_t pitch, rows;
        Layout(info, w, h, pitch, rows);
        return pitch * rows;
    }

    size_t SurfaceOffset(const Info& info, unsigned faceOrSlice, unsigned level)
    {
        size_t o = info.dataOffset;
        if (info.kind == Kind::Cube)
        {
            size_t per = 0;
            for (unsigned l = 0; l < info.mips; ++l) per += SurfaceBytes(info, MipDim(info.width, l), MipDim(info.height, l));
            o += per * faceOrSlice;
            for (unsigned l = 0; l < level; ++l) o += SurfaceBytes(info, MipDim(info.width, l), MipDim(info.height, l));
            return o;
        }
        for (unsigned l = 0; l < level; ++l)
            o += SurfaceBytes(info, MipDim(info.width, l), MipDim(info.height, l)) * MipDim(info.depth, l);
        return o + SurfaceBytes(info, MipDim(info.width, level), MipDim(info.height, level)) * faceOrSlice;
    }

    IDirect3DBaseTexture9* Create(IDirect3DDevice9* dev, const std::string& bytes, const Info& info, D3DPOOL pool,
                                  const char* name)
    {
        if (!dev) return nullptr;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(bytes.data());
        HRESULT hr = E_FAIL;
        if (info.kind == Kind::Texture2D)
        {
            IDirect3DTexture9* t = nullptr;
            hr = dev->CreateTexture(info.width, info.height, info.mips, 0, info.format, pool, &t, nullptr);
            if (FAILED(hr)) { WLOG_WARN("dds: %s: CreateTexture %ux%u %s failed (0x%08lX)", name, info.width, info.height, FormatName(info.format), static_cast<unsigned long>(hr)); return nullptr; }
            for (unsigned l = 0; l < info.mips; ++l)
            {
                D3DLOCKED_RECT lr{};
                size_t pitch, rows;
                Layout(info, MipDim(info.width, l), MipDim(info.height, l), pitch, rows);
                if (SUCCEEDED(t->LockRect(l, &lr, nullptr, 0)))
                {
                    Copy(lr.pBits, lr.Pitch, data + SurfaceOffset(info, 0, l), pitch, rows);
                    t->UnlockRect(l);
                }
            }
            return t;
        }
        if (info.kind == Kind::Cube)
        {
            IDirect3DCubeTexture9* t = nullptr;
            hr = dev->CreateCubeTexture(info.width, info.mips, 0, info.format, pool, &t, nullptr);
            if (FAILED(hr)) { WLOG_WARN("dds: %s: CreateCubeTexture %u %s failed (0x%08lX)", name, info.width, FormatName(info.format), static_cast<unsigned long>(hr)); return nullptr; }
            for (unsigned f = 0; f < 6; ++f)
                for (unsigned l = 0; l < info.mips; ++l)
                {
                    D3DLOCKED_RECT lr{};
                    size_t pitch, rows;
                    Layout(info, MipDim(info.width, l), MipDim(info.height, l), pitch, rows);
                    if (SUCCEEDED(t->LockRect(D3DCUBEMAP_FACES(f), l, &lr, nullptr, 0)))
                    {
                        Copy(lr.pBits, lr.Pitch, data + SurfaceOffset(info, f, l), pitch, rows);
                        t->UnlockRect(D3DCUBEMAP_FACES(f), l);
                    }
                }
            return t;
        }
        IDirect3DVolumeTexture9* t = nullptr;
        hr = dev->CreateVolumeTexture(info.width, info.height, info.depth, info.mips, 0, info.format, pool, &t, nullptr);
        if (FAILED(hr)) { WLOG_WARN("dds: %s: CreateVolumeTexture %ux%ux%u %s failed (0x%08lX)", name, info.width, info.height, info.depth, FormatName(info.format), static_cast<unsigned long>(hr)); return nullptr; }
        for (unsigned l = 0; l < info.mips; ++l)
        {
            D3DLOCKED_BOX box{};
            size_t pitch, rows;
            Layout(info, MipDim(info.width, l), MipDim(info.height, l), pitch, rows);
            if (SUCCEEDED(t->LockBox(l, &box, nullptr, 0)))
            {
                for (unsigned z = 0; z < MipDim(info.depth, l); ++z)
                    Copy(static_cast<uint8_t*>(box.pBits) + size_t(z) * box.SlicePitch, box.RowPitch,
                         data + SurfaceOffset(info, z, l), pitch, rows);
                t->UnlockBox(l);
            }
        }
        return t;
    }

    bool DecodeLuminance(const std::string& bytes, const Info& info, unsigned faceOrSlice, unsigned level,
                         std::vector<uint8_t>& out, unsigned& w, unsigned& h)
    {
        w = MipDim(info.width, level);
        h = MipDim(info.height, level);
        const size_t start = SurfaceOffset(info, faceOrSlice, level);
        if (start + SurfaceBytes(info, w, h) > bytes.size()) return false;
        const uint8_t* s = reinterpret_cast<const uint8_t*>(bytes.data()) + start;
        out.assign(size_t(w) * h, 0);
        switch (info.format)
        {
        case D3DFMT_L8:
        case D3DFMT_A8:
            std::memcpy(out.data(), s, out.size());
            return true;
        case D3DFMT_A8L8:
            for (size_t i = 0; i < out.size(); ++i) out[i] = s[i * 2];
            return true;
        case D3DFMT_A8R8G8B8:
        case D3DFMT_X8R8G8B8:
            for (size_t i = 0; i < out.size(); ++i) out[i] = Lum(s[i * 4 + 2], s[i * 4 + 1], s[i * 4]);
            return true;
        case D3DFMT_R32F:
        case D3DFMT_G32R32F:
        case D3DFMT_A32B32G32R32F:
            for (size_t i = 0; i < out.size(); ++i)
            {
                float v;
                std::memcpy(&v, s + i * info.blockBytes, 4);
                out[i] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            return true;
        case D3DFMT_R16F:
        case D3DFMT_G16R16F:
        case D3DFMT_A16B16G16R16F:
            for (size_t i = 0; i < out.size(); ++i)
            {
                // Half to float: enough of the format for values in 0..1.
                uint16_t hv;
                std::memcpy(&hv, s + i * info.blockBytes, 2);
                const int e = (hv >> 10) & 31, m = hv & 1023;
                float v = e == 0 ? m / 1024.0f / 16384.0f : (1.0f + m / 1024.0f) * std::ldexp(1.0f, e - 15);
                if (hv & 0x8000) v = -v;
                out[i] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
            }
            return true;
        case D3DFMT_DXT1:
        case D3DFMT_DXT3:
        case D3DFMT_DXT5:
        {
            const unsigned bw = (w + 3) / 4, bh = (h + 3) / 4;
            const bool dxt1 = info.format == D3DFMT_DXT1;
            for (unsigned by = 0; by < bh; ++by)
                for (unsigned bx = 0; bx < bw; ++bx)
                {
                    const uint8_t* block = s + (size_t(by) * bw + bx) * info.blockBytes + (dxt1 ? 0 : 8);
                    uint8_t l[16];
                    DecodeColourBlock(block, dxt1, l);
                    for (unsigned y = 0; y < 4 && by * 4 + y < h; ++y)
                        for (unsigned x = 0; x < 4 && bx * 4 + x < w; ++x)
                            out[size_t(by * 4 + y) * w + bx * 4 + x] = l[y * 4 + x];
                }
            return true;
        }
        default:
            return false;
        }
    }

    bool DecodeColour(const std::string& bytes, const Info& info, unsigned faceOrSlice, unsigned level,
                      std::vector<uint8_t>& out, unsigned& w, unsigned& h)
    {
        if (info.format == D3DFMT_A8R8G8B8 || info.format == D3DFMT_X8R8G8B8)
        {
            w = MipDim(info.width, level);
            h = MipDim(info.height, level);
            const size_t start = SurfaceOffset(info, faceOrSlice, level);
            const size_t size = size_t(w) * h * 4;
            if (start + size > bytes.size()) return false;
            out.assign(bytes.begin() + ptrdiff_t(start), bytes.begin() + ptrdiff_t(start + size));
            return true;
        }
        std::vector<uint8_t> lum;
        if (!DecodeLuminance(bytes, info, faceOrSlice, level, lum, w, h)) return false;
        out.resize(lum.size() * 4);
        for (size_t i = 0; i < lum.size(); ++i)
        {
            out[i * 4] = out[i * 4 + 1] = out[i * 4 + 2] = lum[i];
            out[i * 4 + 3] = 255;
        }
        return true;
    }

    const char* FormatName(D3DFORMAT f)
    {
        switch (f)
        {
        case D3DFMT_L8: return "L8";
        case D3DFMT_A8: return "A8";
        case D3DFMT_A8L8: return "A8L8";
        case D3DFMT_A8R8G8B8: return "A8R8G8B8";
        case D3DFMT_X8R8G8B8: return "X8R8G8B8";
        case D3DFMT_R16F: return "R16F";
        case D3DFMT_G16R16F: return "G16R16F";
        case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
        case D3DFMT_R32F: return "R32F";
        case D3DFMT_G32R32F: return "G32R32F";
        case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
        case D3DFMT_DXT1: return "DXT1";
        case D3DFMT_DXT3: return "DXT3";
        case D3DFMT_DXT5: return "DXT5";
        default: return "?";
        }
    }
}
