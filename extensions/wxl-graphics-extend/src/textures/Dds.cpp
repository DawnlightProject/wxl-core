// wxl-graphics-extend: DDS textures created on the device (2D, cube and volume, every mip) and the
// C-ABI forms of the reader.
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
#include "../core/Extension.hpp"
#include "../io/ClientFile.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace wxl::gfx::dds;

    /// One row of stored texels in the created format: the three formats the device is not asked for
    /// are swizzled or widened here; every other row is copied as it is (`bytes` of it).
    void ConvertRow(uint32_t fileFormat, const uint8_t* s, uint32_t w, uint8_t* d, size_t bytes)
    {
        switch (fileFormat)
        {
        case fmt::kA8B8G8R8:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3]; }
            return;
        case fmt::kX8B8G8R8:
            for (uint32_t x = 0; x < w; ++x, s += 4, d += 4) { d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 255; }
            return;
        case fmt::kR8G8B8:
            for (uint32_t x = 0; x < w; ++x, s += 3, d += 4) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 255; }
            return;
        default:
            std::memcpy(d, s, bytes);
            return;
        }
    }

    /// Copies one w x h surface into locked memory, row by row (block rows when compressed), honouring
    /// the pitch the lock reported. False when that pitch cannot hold a row.
    bool CopySurface(const WXL_GfxDdsInfo& info, const uint8_t* src, uint32_t w, uint32_t h, void* dst, INT dstPitch)
    {
        uint64_t srcPitch, rows;
        SurfaceLayout(info, w, h, srcPitch, rows);
        const size_t dstRow = info.format == info.fileFormat ? size_t(srcPitch) : size_t(w) * 4;
        if (dstPitch < 0 || size_t(dstPitch) < dstRow) return false;
        for (uint64_t r = 0; r < rows; ++r)
            ConvertRow(info.fileFormat, src + size_t(r * srcPitch), w, static_cast<uint8_t*>(dst) + size_t(r) * size_t(dstPitch),
                       size_t(srcPitch));
        return true;
    }

    const char* CallName(uint32_t kind)
    {
        return kind == WXL_GFX_DDS_CUBE ? "CreateCubeTexture" : kind == WXL_GFX_DDS_VOLUME ? "CreateVolumeTexture" : "CreateTexture";
    }

    /// "256x256 mips 9 DXT5" for the log.
    void Describe(const WXL_GfxDdsInfo& info, char* buf, size_t cap)
    {
        if (info.kind == WXL_GFX_DDS_VOLUME)
            std::snprintf(buf, cap, "%ux%ux%u mips %u %s", info.width, info.height, info.depth, info.mips, FormatName(info.format));
        else
            std::snprintf(buf, cap, "%ux%u mips %u %s", info.width, info.height, info.mips, FormatName(info.format));
    }

    IDirect3DBaseTexture9* CreateEmpty(IDirect3DDevice9* dev, const WXL_GfxDdsInfo& info, D3DPOOL pool, HRESULT& hr)
    {
        const D3DFORMAT format = D3DFORMAT(info.format);
        if (info.kind == WXL_GFX_DDS_CUBE)
        {
            IDirect3DCubeTexture9* t = nullptr;
            hr = dev->CreateCubeTexture(info.width, info.mips, 0, format, pool, &t, nullptr);
            return SUCCEEDED(hr) ? t : nullptr;
        }
        if (info.kind == WXL_GFX_DDS_VOLUME)
        {
            IDirect3DVolumeTexture9* t = nullptr;
            hr = dev->CreateVolumeTexture(info.width, info.height, info.depth, info.mips, 0, format, pool, &t, nullptr);
            return SUCCEEDED(hr) ? t : nullptr;
        }
        IDirect3DTexture9* t = nullptr;
        hr = dev->CreateTexture(info.width, info.height, info.mips, 0, format, pool, &t, nullptr);
        return SUCCEEDED(hr) ? t : nullptr;
    }

    void LogLock(const char* name, uint32_t level, HRESULT hr)
    {
        GFX_LOG_WARN("dds: %s: lock of level %u failed (0x%08lX)", name, level, static_cast<unsigned long>(hr));
    }

    void LogPitch(const char* name, uint32_t level)
    {
        GFX_LOG_WARN("dds: %s: level %u: the locked pitch is smaller than a row", name, level);
    }

    bool Fill2D(IDirect3DTexture9* t, const WXL_GfxDdsInfo& info, const uint8_t* data, const char* name)
    {
        for (uint32_t l = 0; l < info.mips; ++l)
        {
            D3DLOCKED_RECT lr{};
            const HRESULT hr = t->LockRect(l, &lr, nullptr, 0);
            if (FAILED(hr)) { LogLock(name, l, hr); return false; }
            const bool ok = CopySurface(info, data + size_t(SurfaceOffset(info, 0, l)), MipDim(info.width, l),
                                        MipDim(info.height, l), lr.pBits, lr.Pitch);
            t->UnlockRect(l);
            if (!ok) { LogPitch(name, l); return false; }
        }
        return true;
    }

    bool FillCube(IDirect3DCubeTexture9* t, const WXL_GfxDdsInfo& info, const uint8_t* data, const char* name)
    {
        for (uint32_t f = 0; f < 6; ++f)
            for (uint32_t l = 0; l < info.mips; ++l)
            {
                D3DLOCKED_RECT lr{};
                const HRESULT hr = t->LockRect(D3DCUBEMAP_FACES(f), l, &lr, nullptr, 0);
                if (FAILED(hr)) { LogLock(name, l, hr); return false; }
                const bool ok = CopySurface(info, data + size_t(SurfaceOffset(info, f, l)), MipDim(info.width, l),
                                            MipDim(info.height, l), lr.pBits, lr.Pitch);
                t->UnlockRect(D3DCUBEMAP_FACES(f), l);
                if (!ok) { LogPitch(name, l); return false; }
            }
        return true;
    }

    bool FillVolume(IDirect3DVolumeTexture9* t, const WXL_GfxDdsInfo& info, const uint8_t* data, const char* name)
    {
        for (uint32_t l = 0; l < info.mips; ++l)
        {
            D3DLOCKED_BOX box{};
            const HRESULT hr = t->LockBox(l, &box, nullptr, 0);
            if (FAILED(hr)) { LogLock(name, l, hr); return false; }
            bool ok = box.SlicePitch >= 0;
            for (uint32_t z = 0; ok && z < MipDim(info.depth, l); ++z)
                ok = CopySurface(info, data + size_t(SurfaceOffset(info, z, l)), MipDim(info.width, l), MipDim(info.height, l),
                                 static_cast<uint8_t*>(box.pBits) + size_t(z) * size_t(box.SlicePitch), box.RowPitch);
            t->UnlockBox(l);
            if (!ok) { LogPitch(name, l); return false; }
        }
        return true;
    }

    bool Fill(IDirect3DBaseTexture9* t, const WXL_GfxDdsInfo& info, const uint8_t* data, const char* name)
    {
        // The texture came from CreateEmpty with this info: the static casts match the kind.
        if (info.kind == WXL_GFX_DDS_CUBE) return FillCube(static_cast<IDirect3DCubeTexture9*>(t), info, data, name);
        if (info.kind == WXL_GFX_DDS_VOLUME) return FillVolume(static_cast<IDirect3DVolumeTexture9*>(t), info, data, name);
        return Fill2D(static_cast<IDirect3DTexture9*>(t), info, data, name);
    }
}

namespace wxl::gfx::dds
{
    void* CreateTexture(void* device, const void* bytes, size_t size, uint32_t pool, const char* name)
    {
        if (!name) name = "?";
        auto* dev = static_cast<IDirect3DDevice9*>(device);
        if (!dev) { GFX_LOG_WARN("dds: %s: no device", name); return nullptr; }
        WXL_GfxDdsInfo info;
        if (!Parse(bytes, size, &info))
        {
            GFX_LOG_WARN("dds: %s: not a DDS file this reader takes (%u bytes)", name, unsigned(size));
            return nullptr;
        }
        if (pool > uint32_t(D3DPOOL_SCRATCH)) { GFX_LOG_WARN("dds: %s: pool %u is not a D3DPOOL", name, pool); return nullptr; }
        char what[96];
        Describe(info, what, sizeof what);
        const uint8_t* data = static_cast<const uint8_t*>(bytes);

        // A DEFAULT-pool texture cannot be locked: fill a SYSTEMMEM twin and UpdateTexture it across.
        const D3DPOOL fillPool = pool == uint32_t(D3DPOOL_DEFAULT) ? D3DPOOL_SYSTEMMEM : D3DPOOL(pool);
        HRESULT hr = S_OK;
        IDirect3DBaseTexture9* filled = CreateEmpty(dev, info, fillPool, hr);
        if (!filled)
        {
            GFX_LOG_WARN("dds: %s: %s %s (pool %d) failed (0x%08lX)", name, CallName(info.kind), what, int(fillPool),
                         static_cast<unsigned long>(hr));
            return nullptr;
        }
        if (!Fill(filled, info, data, name)) { filled->Release(); return nullptr; }
        if (pool != uint32_t(D3DPOOL_DEFAULT)) return filled;

        IDirect3DBaseTexture9* result = CreateEmpty(dev, info, D3DPOOL_DEFAULT, hr);
        if (!result)
        {
            GFX_LOG_WARN("dds: %s: %s %s (pool 0) failed (0x%08lX)", name, CallName(info.kind), what, static_cast<unsigned long>(hr));
            filled->Release();
            return nullptr;
        }
        hr = dev->UpdateTexture(filled, result);
        filled->Release();
        if (FAILED(hr))
        {
            GFX_LOG_WARN("dds: %s: UpdateTexture %s failed (0x%08lX)", name, what, static_cast<unsigned long>(hr));
            result->Release();
            return nullptr;
        }
        return result;
    }

    void* LoadTexture(void* device, const char* path, uint32_t pool)
    {
        if (!path || !*path) { GFX_LOG_WARN("dds: no path given"); return nullptr; }
        std::string bytes;
        if (!wxl::gfx::io::Read(path, bytes)) { GFX_LOG_WARN("dds: %s: file not found", path); return nullptr; }
        return CreateTexture(device, bytes.data(), bytes.size(), pool, path);
    }

    int DecodeBgra(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level, WXL_ByteSink* out, uint32_t* width,
                   uint32_t* height)
    {
        if (!out || !out->Write) return 0;
        std::vector<uint8_t> texels;
        uint32_t w = 0, h = 0;
        if (!DecodeBgra(bytes, size, faceOrSlice, level, texels, w, h)) return 0;
        out->Write(out->ctx, texels.data(), unsigned(texels.size()));   // at most 16384^2 x 4: fits
        if (width) *width = w;
        if (height) *height = h;
        return 1;
    }

    int DecodeLuminance(const void* bytes, size_t size, uint32_t faceOrSlice, uint32_t level, WXL_ByteSink* out,
                        uint32_t* width, uint32_t* height)
    {
        if (!out || !out->Write) return 0;
        std::vector<uint8_t> texels;
        uint32_t w = 0, h = 0;
        if (!DecodeLuminance(bytes, size, faceOrSlice, level, texels, w, h)) return 0;
        out->Write(out->ctx, texels.data(), unsigned(texels.size()));
        if (width) *width = w;
        if (height) *height = h;
        return 1;
    }
}
