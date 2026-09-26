// wxl-graphics-lights: a texture consumers of every kind may read -- DEFAULT pool, filled by copying a
// system-memory twin up, never by locking it.
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

#include "../core/Extension.hpp"
#include "Textures.hpp"

namespace
{
    unsigned TexelBytes(D3DFORMAT f)
    {
        switch (f)
        {
        case D3DFMT_L8: case D3DFMT_A8:                         return 1;
        case D3DFMT_A8L8: case D3DFMT_L16: case D3DFMT_R16F:    return 2;
        case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_R32F: case D3DFMT_G16R16F: return 4;
        case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F:        return 8;
        case D3DFMT_A32B32G32R32F:                              return 16;
        default:                                                return 4;
        }
    }
}

namespace wxl::gfx::lights::tex
{
    bool Create(IDirect3DDevice9* dev, unsigned width, unsigned height, D3DFORMAT format, Twin& out, const char* name)
    {
        Release(out);
        if (!dev || width == 0 || height == 0) return false;
        Twin t;
        HRESULT hr = dev->CreateTexture(width, height, 1, 0, format, D3DPOOL_DEFAULT, &t.gpu, nullptr);
        if (SUCCEEDED(hr)) hr = dev->CreateTexture(width, height, 1, 0, format, D3DPOOL_SYSTEMMEM, &t.cpu, nullptr);
        if (SUCCEEDED(hr)) hr = t.gpu->GetSurfaceLevel(0, &t.gpuSurface);
        if (SUCCEEDED(hr)) hr = t.cpu->GetSurfaceLevel(0, &t.cpuSurface);
        if (FAILED(hr))
        {
            LIGHTS_LOG_WARN("textures: %s %ux%u (format %u) unavailable on this device (0x%08lX)", name, width, height,
                            unsigned(format), static_cast<unsigned long>(hr));
            Release(t);
            return false;
        }
        t.width = width;
        t.height = height;
        t.format = format;
        t.texelBytes = TexelBytes(format);
        out = t;
        return true;
    }

    void Release(Twin& t)
    {
        if (t.cpuSurface) t.cpuSurface->Release();
        if (t.gpuSurface) t.gpuSurface->Release();
        if (t.cpu) t.cpu->Release();
        if (t.gpu) t.gpu->Release();
        t = Twin{};
    }

    uint8_t* Lock(Twin& t, const RECT* rect, int& pitch)
    {
        pitch = 0;
        if (!t.cpu) return nullptr;
        D3DLOCKED_RECT lr{};
        if (FAILED(t.cpu->LockRect(0, &lr, rect, 0))) return nullptr;
        pitch = lr.Pitch;
        return static_cast<uint8_t*>(lr.pBits);
    }

    void Unlock(Twin& t)
    {
        if (t.cpu) t.cpu->UnlockRect(0);
    }

    bool Upload(IDirect3DDevice9* dev, Twin& t)
    {
        return dev && t.gpu && t.cpu && SUCCEEDED(dev->UpdateTexture(t.cpu, t.gpu));
    }

    bool UploadRows(IDirect3DDevice9* dev, Twin& t, unsigned rows)
    {
        if (rows >= t.height) return Upload(dev, t);
        if (rows == 0) return true;
        const RECT r{ 0, 0, LONG(t.width), LONG(rows) };
        return UploadRect(dev, t, r);
    }

    bool UploadRect(IDirect3DDevice9* dev, Twin& t, const RECT& rect)
    {
        if (!dev || !t.gpuSurface || !t.cpuSurface) return false;
        const POINT at{ rect.left, rect.top };
        return SUCCEEDED(dev->UpdateSurface(t.cpuSurface, &rect, t.gpuSurface, &at));
    }
}
