// wxl-graphics-extend: the shared stand-in textures (white, black, flat normal, white cube and volume)
// and the blue-noise texture.
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

#include "Neutral.hpp"
#include "BlueNoise.hpp"
#include "../core/Extension.hpp"

#include "wxl/GraphicsExtendApi.h"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>

namespace
{
    namespace bn = wxl::gfx::textures::bluenoise;

    constexpr uint32_t kCount = WXL_GFX_TEX_BLUE_NOISE + 1;

    // The device the textures were created on. When the client recreates its device (not a reset: a
    // new object) the old ones are released and made again on the new one; a texture keeps its device
    // alive, so the address cannot be reused while any is held and the comparison is exact.
    IDirect3DDevice9*      g_device = nullptr;
    IDirect3DBaseTexture9* g_textures[kCount] = {};
    bool                   g_failed[kCount] = {};   // refused by this device: logged once, not retried

    const char* Name(uint32_t which)
    {
        switch (which)
        {
        case WXL_GFX_TEX_WHITE: return "white";
        case WXL_GFX_TEX_BLACK: return "black";
        case WXL_GFX_TEX_FLAT_NORMAL: return "flat normal";
        case WXL_GFX_TEX_WHITE_CUBE: return "white cube";
        case WXL_GFX_TEX_WHITE_VOLUME: return "white volume";
        default: return "blue noise";
        }
    }

    /// A 1 x 1 A8R8G8B8 texture of one colour (0xAARRGGBB).
    IDirect3DBaseTexture9* Solid2D(IDirect3DDevice9* dev, uint32_t argb)
    {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
        D3DLOCKED_RECT lr{};
        if (FAILED(t->LockRect(0, &lr, nullptr, 0))) { t->Release(); return nullptr; }
        std::memcpy(lr.pBits, &argb, 4);
        t->UnlockRect(0);
        return t;
    }

    IDirect3DBaseTexture9* SolidCube(IDirect3DDevice9* dev, uint32_t argb)
    {
        IDirect3DCubeTexture9* t = nullptr;
        if (FAILED(dev->CreateCubeTexture(1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
        for (int f = 0; f < 6; ++f)
        {
            D3DLOCKED_RECT lr{};
            if (FAILED(t->LockRect(D3DCUBEMAP_FACES(f), 0, &lr, nullptr, 0))) { t->Release(); return nullptr; }
            std::memcpy(lr.pBits, &argb, 4);
            t->UnlockRect(D3DCUBEMAP_FACES(f), 0);
        }
        return t;
    }

    IDirect3DBaseTexture9* SolidVolume(IDirect3DDevice9* dev, uint32_t argb)
    {
        IDirect3DVolumeTexture9* t = nullptr;
        if (FAILED(dev->CreateVolumeTexture(1, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
        D3DLOCKED_BOX box{};
        if (FAILED(t->LockBox(0, &box, nullptr, 0))) { t->Release(); return nullptr; }
        std::memcpy(box.pBits, &argb, 4);
        t->UnlockBox(0);
        return t;
    }

    /// The baked texels as a texture; only once bluenoise::Texels() is non-null.
    IDirect3DBaseTexture9* BlueNoise(IDirect3DDevice9* dev, const uint32_t* texels)
    {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(dev->CreateTexture(bn::kSize, bn::kSize, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
        D3DLOCKED_RECT lr{};
        if (FAILED(t->LockRect(0, &lr, nullptr, 0)) || lr.Pitch < bn::kSize * 4) { t->Release(); return nullptr; }
        for (int y = 0; y < bn::kSize; ++y)
            std::memcpy(static_cast<uint8_t*>(lr.pBits) + size_t(y) * size_t(lr.Pitch), texels + size_t(y) * bn::kSize, size_t(bn::kSize) * 4);
        t->UnlockRect(0);
        uint32_t ms = 0;
        bn::Ready(ms);
        GFX_LOG_INFO("blue noise %dx%d, 4 channels, baked in %u ms on a worker, uploaded", bn::kSize, bn::kSize, ms);
        return t;
    }

    void ReleaseAll()
    {
        for (uint32_t i = 0; i < kCount; ++i)
        {
            if (g_textures[i]) { g_textures[i]->Release(); g_textures[i] = nullptr; }
            g_failed[i] = false;
        }
    }
}

namespace wxl::gfx::textures
{
    void StartBlueNoiseBake()
    {
        bn::Start();
    }

    void* Texture(void* device, uint32_t which)
    {
        auto* dev = static_cast<IDirect3DDevice9*>(device);
        if (!dev || which >= kCount) return nullptr;
        if (dev != g_device)
        {
            ReleaseAll();
            g_device = dev;
        }
        if (g_textures[which] || g_failed[which]) return g_textures[which];

        IDirect3DBaseTexture9* t = nullptr;
        switch (which)
        {
        case WXL_GFX_TEX_WHITE:        t = Solid2D(dev, 0xFFFFFFFFu); break;
        case WXL_GFX_TEX_BLACK:        t = Solid2D(dev, 0x00000000u); break;
        case WXL_GFX_TEX_FLAT_NORMAL:  t = Solid2D(dev, 0xFF8080FFu); break;   // (128, 128, 255, 255)
        case WXL_GFX_TEX_WHITE_CUBE:   t = SolidCube(dev, 0xFFFFFFFFu); break;
        case WXL_GFX_TEX_WHITE_VOLUME: t = SolidVolume(dev, 0xFFFFFFFFu); break;
        default:
        {
            // Baked on first request, so an unused blue noise costs nothing; null until it is ready.
            bn::Start();
            const uint32_t* texels = bn::Texels();
            if (!texels) return nullptr;
            t = BlueNoise(dev, texels);
            break;
        }
        }
        if (!t)
        {
            g_failed[which] = true;
            GFX_LOG_WARN("textures: the %s texture could not be created", Name(which));
            return nullptr;
        }
        g_textures[which] = t;
        return t;
    }

    bool BlueNoiseReady(uint32_t& bakeMs)
    {
        return bn::Ready(bakeMs);
    }
}
