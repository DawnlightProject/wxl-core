// wxl-graphics-extend: render target textures kept for consumers across resizes and device resets.
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

#include "TargetPool.hpp"
#include "../core/Extension.hpp"
#include "../frame/Scheduler.hpp"
#include "../textures/Dds.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace
{
    constexpr uint32_t kMaxDim = 16384;
    constexpr uint32_t kKnownFlags = WXL_GFX_TARGET_MIPMAPS | WXL_GFX_TARGET_CLEAR;

    struct Entry
    {
        uint32_t           handle = 0;
        std::string        name;
        WXL_GfxTargetDesc  desc{};           // name is null here: `name` above holds the copy
        IDirect3DTexture9* texture = nullptr;
        IDirect3DSurface9* surface = nullptr;
        uint32_t           width = 0, height = 0;
        uint32_t           generation = 0;
        // The device refused this size: logged once, tried again only at another size or after a
        // device loss (a reset may free the memory it lacked).
        bool               refused = false;
        uint32_t           refusedWidth = 0, refusedHeight = 0;
    };

    std::vector<Entry> g_entries;   // in handle order; a handful, so lookups walk it
    uint32_t           g_nextHandle = 1;
    uint32_t           g_generation = 0;

    Entry* Find(uint32_t handle)
    {
        for (Entry& e : g_entries)
            if (e.handle == handle) return &e;
        return nullptr;
    }

    void ReleaseTexture(Entry& e)
    {
        if (e.surface) { e.surface->Release(); e.surface = nullptr; }
        if (e.texture) { e.texture->Release(); e.texture = nullptr; }
        e.width = e.height = 0;
    }

    /// Bits per texel for the memory estimate; a format not listed is taken as 32.
    uint32_t BitsPerPixel(uint32_t format)
    {
        switch (format)
        {
        case D3DFMT_DXT1:
            return 4;
        case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5:
        case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_R3G3B2: case D3DFMT_P8: case D3DFMT_A4L4:
            return 8;
        case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4:
        case D3DFMT_A8R3G3B2: case D3DFMT_A8P8: case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_R16F:
        case D3DFMT_V8U8: case D3DFMT_L6V5U5: case D3DFMT_CxV8U8: case D3DFMT_R8G8_B8G8: case D3DFMT_G8R8_G8B8:
        case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_INDEX16:
        case MAKEFOURCC('D', 'F', '1', '6'):
            return 16;
        case D3DFMT_R8G8B8:
            return 24;
        case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16:
            return 64;
        case D3DFMT_A32B32G32R32F:
            return 128;
        default:
            return 32;   // the 8-bit-per-channel and 24/32-bit depth formats, G16R16(F), R32F, INTZ, ...
        }
    }

    /// Approximate video memory of one texture: the top level, plus a third with a mip chain.
    uint64_t EstimateBytes(const Entry& e)
    {
        if (!e.texture) return 0;
        uint64_t bytes = uint64_t(e.width) * e.height * BitsPerPixel(e.desc.format) / 8;
        if (e.desc.flags & WXL_GFX_TARGET_MIPMAPS) bytes += bytes / 3;
        return bytes;
    }

    /// The format's name for the log and the panel: the DDS reader's list, the depth and render
    /// formats it has no use for, a FOURCC spelled out, or the number.
    const char* FormatLabel(uint32_t format, char* buf, size_t cap)
    {
        const char* known = wxl::gfx::dds::FormatName(format);
        if (known[0] != '?') return known;
        switch (format)
        {
        case D3DFMT_D16_LOCKABLE: return "D16_LOCKABLE";
        case D3DFMT_D32: return "D32";
        case D3DFMT_D15S1: return "D15S1";
        case D3DFMT_D24S8: return "D24S8";
        case D3DFMT_D24X8: return "D24X8";
        case D3DFMT_D24X4S4: return "D24X4S4";
        case D3DFMT_D16: return "D16";
        case D3DFMT_D32F_LOCKABLE: return "D32F_LOCKABLE";
        case D3DFMT_D24FS8: return "D24FS8";
        case D3DFMT_R3G3B2: return "R3G3B2";
        case D3DFMT_X4R4G4B4: return "X4R4G4B4";
        default: break;
        }
        const char c[4] = { char(format & 0xFF), char((format >> 8) & 0xFF), char((format >> 16) & 0xFF), char(format >> 24) };
        bool fourcc = format >= 0x100;
        for (char ch : c) fourcc = fourcc && ch >= 0x20 && ch < 0x7F;
        if (fourcc) std::snprintf(buf, cap, "%c%c%c%c", c[0], c[1], c[2], c[3]);
        else std::snprintf(buf, cap, "fmt%u", format);
        return buf;
    }

    uint32_t Scaled(uint32_t v, float scale)
    {
        const float f = std::floor(float(v) * scale + 0.5f);
        if (!(f >= 1.0f)) return 1;
        return f > float(kMaxDim) ? kMaxDim : uint32_t(f);
    }

    /// The size the target should have now: the fixed one, or the world's render target (the back
    /// buffer before the first world pass) times the scale.
    bool WantedSize(const Entry& e, IDirect3DDevice9* dev, uint32_t& w, uint32_t& h)
    {
        if (e.desc.width && e.desc.height)
        {
            w = e.desc.width;
            h = e.desc.height;
            return true;
        }
        uint32_t baseW = 0, baseH = 0;
        if (!wxl::gfx::frame::WorldSize(baseW, baseH) || !baseW || !baseH)
        {
            IDirect3DSurface9* back = nullptr;
            if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) || !back) return false;
            D3DSURFACE_DESC desc{};
            const bool ok = SUCCEEDED(back->GetDesc(&desc));
            back->Release();
            if (!ok || !desc.Width || !desc.Height) return false;
            baseW = desc.Width;
            baseH = desc.Height;
        }
        const float scale = e.desc.scale > 0.0f ? e.desc.scale : 1.0f;
        w = Scaled(baseW, scale);
        h = Scaled(baseH, scale);
        return true;
    }

    bool Make(Entry& e, IDirect3DDevice9* dev, uint32_t w, uint32_t h)
    {
        const bool mips = (e.desc.flags & WXL_GFX_TARGET_MIPMAPS) != 0;
        const DWORD usage = D3DUSAGE_RENDERTARGET | (mips ? D3DUSAGE_AUTOGENMIPMAP : 0);
        HRESULT hr = dev->CreateTexture(w, h, mips ? 0 : 1, usage, D3DFORMAT(e.desc.format), D3DPOOL_DEFAULT, &e.texture, nullptr);
        if (SUCCEEDED(hr) && e.texture) hr = e.texture->GetSurfaceLevel(0, &e.surface);
        char label[16];
        if (FAILED(hr) || !e.texture || !e.surface)
        {
            ReleaseTexture(e);
            e.refused = true;
            e.refusedWidth = w;
            e.refusedHeight = h;
            GFX_LOG_WARN("target %s: %ux%u %s refused (0x%08lX)", e.name.c_str(), w, h, FormatLabel(e.desc.format, label, sizeof label),
                         static_cast<unsigned long>(hr));
            return false;
        }
        e.width = w;
        e.height = h;
        e.refused = false;
        if (++g_generation == 0) ++g_generation;   // 0 is never a generation
        e.generation = g_generation;
        if (e.desc.flags & WXL_GFX_TARGET_CLEAR) dev->ColorFill(e.surface, nullptr, 0);
        GFX_LOG_DEBUG("target %s: %ux%u %s created (gen %u)", e.name.c_str(), w, h, FormatLabel(e.desc.format, label, sizeof label),
                      e.generation);
        return true;
    }
}

namespace wxl::gfx::targets
{
    uint32_t Create(const WXL_GfxTargetDesc* desc)
    {
        if (!desc || desc->structSize < sizeof(WXL_GfxTargetDesc))
        {
            GFX_LOG_WARN("target: refused, the desc's structSize is too small");
            return 0;
        }
        const char* name = desc->name ? desc->name : "target";
        const char* fault = nullptr;
        if (desc->format == 0) fault = "no format";
        else if (!(desc->scale >= 0.0f && desc->scale <= 4.0f)) fault = "scale outside 0..4";
        else if ((desc->width == 0) != (desc->height == 0)) fault = "width and height must both be zero or both be set";
        else if (desc->width > kMaxDim || desc->height > kMaxDim) fault = "larger than 16384";
        else if (desc->flags & ~kKnownFlags) fault = "unknown flags";
        else if (g_nextHandle == 0) fault = "out of handles";
        if (fault)
        {
            GFX_LOG_WARN("target %s: refused, %s", name, fault);
            return 0;
        }

        Entry e;
        e.handle = g_nextHandle++;
        e.name = name;
        e.desc = *desc;
        e.desc.structSize = sizeof(WXL_GfxTargetDesc);
        e.desc.name = nullptr;
        char label[16];
        if (e.desc.width)
            GFX_LOG_DEBUG("target %s: declared, %ux%u %s", name, e.desc.width, e.desc.height, FormatLabel(e.desc.format, label, sizeof label));
        else
            GFX_LOG_DEBUG("target %s: declared, world x %.3g %s", name, double(e.desc.scale > 0.0f ? e.desc.scale : 1.0f),
                          FormatLabel(e.desc.format, label, sizeof label));
        g_entries.push_back(std::move(e));
        return g_entries.back().handle;
    }

    int Get(uint32_t handle, void* device, WXL_GfxTarget* out)
    {
        Entry* e = Find(handle);
        auto* dev = static_cast<IDirect3DDevice9*>(device);
        if (!e || !dev || !out) return 0;
        uint32_t w, h;
        if (!WantedSize(*e, dev, w, h)) return 0;
        if (e->texture && (e->width != w || e->height != h)) ReleaseTexture(*e);
        if (!e->texture)
        {
            if (e->refused && e->refusedWidth == w && e->refusedHeight == h) return 0;
            if (!Make(*e, dev, w, h)) return 0;
        }
        out->texture = e->texture;
        out->surface = e->surface;
        out->width = e->width;
        out->height = e->height;
        out->generation = e->generation;
        return 1;
    }

    void Destroy(uint32_t handle)
    {
        for (size_t i = 0; i < g_entries.size(); ++i)
        {
            if (g_entries[i].handle != handle) continue;
            ReleaseTexture(g_entries[i]);
            GFX_LOG_DEBUG("target %s: destroyed", g_entries[i].name.c_str());
            g_entries.erase(g_entries.begin() + ptrdiff_t(i));
            return;
        }
    }

    void OnDeviceLost()
    {
        for (Entry& e : g_entries)
        {
            ReleaseTexture(e);
            e.refused = false;
        }
    }

    void Stats(uint32_t& count, uint32_t& bytes)
    {
        count = 0;
        uint64_t total = 0;
        for (const Entry& e : g_entries)
        {
            if (!e.texture) continue;
            ++count;
            total += EstimateBytes(e);
        }
        bytes = total > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(total);
    }

    size_t Count()
    {
        return g_entries.size();
    }

    bool Describe(size_t index, char* buf, size_t cap)
    {
        if (index >= g_entries.size() || !buf || !cap) return false;
        const Entry& e = g_entries[index];
        char label[16];
        const char* format = FormatLabel(e.desc.format, label, sizeof label);
        if (e.texture)
            std::snprintf(buf, cap, "%s %ux%u %s gen %u  %.1f MB", e.name.c_str(), e.width, e.height, format, e.generation,
                          double(EstimateBytes(e)) / 1048576.0);
        else if (e.refused)
            std::snprintf(buf, cap, "%s %ux%u %s refused", e.name.c_str(), e.refusedWidth, e.refusedHeight, format);
        else
            std::snprintf(buf, cap, "%s %s not created", e.name.c_str(), format);
        return true;
    }
}
