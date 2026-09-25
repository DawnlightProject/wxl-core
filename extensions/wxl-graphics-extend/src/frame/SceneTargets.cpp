// wxl-graphics-extend: the world pass's shared textures (INTZ depth, G-buffer normals, HDR colour)
// and the device facts they depend on.
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

#include "SceneTargets.hpp"
#include "../core/Extension.hpp"

#include "wxl/GraphicsExtendApi.h"

#include <cstdio>

namespace
{
    namespace scene = wxl::gfx::frame::scene;

    const D3DFORMAT kIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

    struct Spec
    {
        const char* name;
        D3DFORMAT   format;
        DWORD       usage;
    };

    /// Indexed by scene::Kind.
    const Spec kSpecs[] = {
        { "INTZ depth",    kIntz,                D3DUSAGE_DEPTHSTENCIL },
        { "normal target", D3DFMT_A8R8G8B8,      D3DUSAGE_RENDERTARGET },
        { "HDR colour",    D3DFMT_A16B16G16R16F, D3DUSAGE_RENDERTARGET },
    };

    struct Target
    {
        IDirect3DTexture9* texture = nullptr;
        IDirect3DSurface9* surface = nullptr;
        UINT               width = 0, height = 0;
        bool               refused = false;   // the device said no: logged once, retried after Release
    };

    Target   g_targets[static_cast<int>(scene::Kind::Count)];
    uint32_t g_caps = 0;
    char     g_status[256] = "not probed yet (enter the world)";

    void ReleaseTarget(Target& t)
    {
        if (t.surface) { t.surface->Release(); t.surface = nullptr; }
        if (t.texture) { t.texture->Release(); t.texture = nullptr; }
        t.width = t.height = 0;
    }
}

namespace wxl::gfx::frame::scene
{
    void Probe(IDirect3DDevice9* dev, IDirect3DSurface9* sceneDepth)
    {
        if (g_caps & WXL_GFX_CAP_PROBED) return;

        D3DSURFACE_DESC dd{};
        sceneDepth->GetDesc(&dd);

        D3DSURFACE_DESC bd{};
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { bb->GetDesc(&bd); bb->Release(); }

        D3DDEVICE_CREATION_PARAMETERS cp{};
        dev->GetCreationParameters(&cp);

        D3DCAPS9 caps{};
        const bool mrt = SUCCEEDED(dev->GetDeviceCaps(&caps)) && caps.NumSimultaneousRTs >= 2;

        HRESULT intz = E_FAIL, fp16 = E_FAIL, fp16Blend = E_FAIL;
        IDirect3D9* d3d = nullptr;
        if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d)
        {
            D3DDISPLAYMODE mode{};
            d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode);
            intz = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                          D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kIntz);
            fp16 = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                          D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F);
            fp16Blend = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                               D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING,
                                               D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F);
            d3d->Release();
        }

        const bool intzOk = SUCCEEDED(intz);
        const bool msaa   = dd.MultiSampleType != D3DMULTISAMPLE_NONE;
        const bool pure   = (cp.BehaviorFlags & D3DCREATE_PUREDEVICE) != 0;

        g_caps = WXL_GFX_CAP_PROBED
               | (intzOk              ? WXL_GFX_CAP_INTZ        : 0u)
               | (mrt                 ? WXL_GFX_CAP_MRT         : 0u)
               | (SUCCEEDED(fp16)     ? WXL_GFX_CAP_FP16_TARGET : 0u)
               | (SUCCEEDED(fp16Blend) ? WXL_GFX_CAP_FP16_BLEND : 0u)
               | (msaa                ? WXL_GFX_CAP_MSAA        : 0u)
               | (pure                ? WXL_GFX_CAP_PURE        : 0u);

        GFX_LOG_INFO("scene: INTZ depth texture %s (hr=0x%08lX), %u simultaneous render targets, FP16 target %s (blend %s)",
                     intzOk ? "supported" : "NOT supported", static_cast<unsigned long>(intz),
                     static_cast<unsigned>(caps.NumSimultaneousRTs),
                     SUCCEEDED(fp16) ? "yes" : "no", SUCCEEDED(fp16Blend) ? "yes" : "no");
        GFX_LOG_INFO("scene: scene depth format=0x%08X msaa=%u quality=%lu %ux%u",
                     static_cast<unsigned>(dd.Format), static_cast<unsigned>(dd.MultiSampleType),
                     static_cast<unsigned long>(dd.MultiSampleQuality), dd.Width, dd.Height);
        GFX_LOG_INFO("scene: back buffer format=0x%08X msaa=%u %ux%u, device behavior=0x%08lX%s",
                     static_cast<unsigned>(bd.Format), static_cast<unsigned>(bd.MultiSampleType),
                     bd.Width, bd.Height, static_cast<unsigned long>(cp.BehaviorFlags), pure ? " (pure)" : "");

        const char* why = nullptr;
        if (!intzOk)     why = "INTZ unsupported";
        else if (msaa)   why = "depth is multisampled (INTZ cannot be)";
        else if (pure)   why = "pure device (state cannot be read back)";

        std::snprintf(g_status, sizeof g_status, "INTZ %s | depth 0x%08X msaa %u %ux%u | %s",
                      intzOk ? "yes" : "no", static_cast<unsigned>(dd.Format),
                      static_cast<unsigned>(dd.MultiSampleType), dd.Width, dd.Height,
                      why ? why : "active");
        if (why) GFX_LOG_WARN("scene: depth texture inert: %s", why);
    }

    uint32_t Caps() { return g_caps; }

    const char* Status() { return g_status; }

    IDirect3DSurface9* Ensure(Kind kind, IDirect3DDevice9* dev, UINT width, UINT height)
    {
        const int i = static_cast<int>(kind);
        if (i < 0 || i >= static_cast<int>(Kind::Count) || !dev || !width || !height) return nullptr;
        Target& t = g_targets[i];
        const Spec& spec = kSpecs[i];
        if (t.surface && t.width == width && t.height == height) return t.surface;
        if (t.refused) return nullptr;
        ReleaseTarget(t);

        const HRESULT hr = dev->CreateTexture(width, height, 1, spec.usage, spec.format, D3DPOOL_DEFAULT, &t.texture, nullptr);
        if (FAILED(hr) || !t.texture || FAILED(t.texture->GetSurfaceLevel(0, &t.surface)) || !t.surface)
        {
            GFX_LOG_WARN("scene: %s texture %ux%u create failed hr=0x%08lX; not retried until the device resets",
                         spec.name, width, height, static_cast<unsigned long>(hr));
            ReleaseTarget(t);
            t.refused = true;
            if (kind == Kind::Depth)
                std::snprintf(g_status, sizeof g_status, "INTZ create failed hr=0x%08lX", static_cast<unsigned long>(hr));
            return nullptr;
        }
        t.width = width;
        t.height = height;
        GFX_LOG_INFO("scene: %s texture %ux%u created", spec.name, width, height);
        return t.surface;
    }

    IDirect3DTexture9* Owned(Kind kind, const void* surface)
    {
        const int i = static_cast<int>(kind);
        if (i < 0 || i >= static_cast<int>(Kind::Count) || !surface) return nullptr;
        const Target& t = g_targets[i];
        return t.surface && surface == t.surface ? t.texture : nullptr;
    }

    IDirect3DTexture9* ContainerTexture(IDirect3DSurface9* surface, bool requireIntz)
    {
        if (!surface) return nullptr;
        IDirect3DTexture9* texture = nullptr;
        if (FAILED(surface->GetContainer(__uuidof(IDirect3DTexture9), reinterpret_cast<void**>(&texture))) || !texture)
            return nullptr;
        bool accept = true;
        if (requireIntz)
        {
            D3DSURFACE_DESC ld{};
            accept = SUCCEEDED(texture->GetLevelDesc(0, &ld)) && ld.Format == kIntz;
        }
        texture->Release();
        return accept ? texture : nullptr;
    }

    void Release()
    {
        for (Target& t : g_targets)
        {
            ReleaseTarget(t);
            t.refused = false;
        }
        g_caps = 0;
        std::snprintf(g_status, sizeof g_status, "not probed yet (enter the world)");
    }
}
