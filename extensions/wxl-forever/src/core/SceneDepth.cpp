// wxl-forever: the world's depth as a texture, shared by every render feature of the suite.
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
#include "SceneDepth.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdio>

namespace
{
    namespace gx = wxl::game::gx;

    const D3DFORMAT kIntz = static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z'));

    enum class Probe { Unknown, Usable, Inert };
    Probe g_probe = Probe::Unknown;
    char  g_status[256] = "not probed yet (enter the world)";

    IDirect3DTexture9* g_texture = nullptr;
    IDirect3DSurface9* g_surface = nullptr;
    UINT               g_width = 0, g_height = 0;
    gx::DepthRange     g_range = { 0.0f, 1.0f };

    void ReleaseTexture()
    {
        if (g_surface) { g_surface->Release(); g_surface = nullptr; }
        if (g_texture) { g_texture->Release(); g_texture = nullptr; }
        g_width = g_height = 0;
    }

    /// Logs and records the device facts the depth texture depends on.
    void RunProbe(IDirect3DDevice9* dev, IDirect3DSurface9* sceneDepth)
    {
        D3DSURFACE_DESC dd{};
        sceneDepth->GetDesc(&dd);

        D3DSURFACE_DESC bd{};
        IDirect3DSurface9* bb = nullptr;
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { bb->GetDesc(&bd); bb->Release(); }

        D3DDEVICE_CREATION_PARAMETERS cp{};
        dev->GetCreationParameters(&cp);

        HRESULT intz = E_FAIL;
        IDirect3D9* d3d = nullptr;
        if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d)
        {
            D3DDISPLAYMODE mode{};
            d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode);
            intz = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format,
                                          D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, kIntz);
            d3d->Release();
        }

        const bool intzOk = SUCCEEDED(intz);
        const bool msaa   = dd.MultiSampleType != D3DMULTISAMPLE_NONE;
        const bool pure   = (cp.BehaviorFlags & D3DCREATE_PUREDEVICE) != 0;

        WLOG_INFO("depth: INTZ depth texture %s (hr=0x%08lX)", intzOk ? "supported" : "NOT supported",
                  static_cast<unsigned long>(intz));
        WLOG_INFO("depth: scene depth format=0x%08X msaa=%u quality=%lu %ux%u",
                  static_cast<unsigned>(dd.Format), static_cast<unsigned>(dd.MultiSampleType),
                  static_cast<unsigned long>(dd.MultiSampleQuality), dd.Width, dd.Height);
        WLOG_INFO("depth: back buffer format=0x%08X msaa=%u %ux%u, device behavior=0x%08lX%s",
                  static_cast<unsigned>(bd.Format), static_cast<unsigned>(bd.MultiSampleType),
                  bd.Width, bd.Height, static_cast<unsigned long>(cp.BehaviorFlags), pure ? " (pure)" : "");

        const char* why = nullptr;
        if (!intzOk)     why = "INTZ unsupported";
        else if (msaa)   why = "depth is multisampled (INTZ cannot be)";
        else if (pure)   why = "pure device (state cannot be read back)";

        g_probe = why ? Probe::Inert : Probe::Usable;
        std::snprintf(g_status, sizeof g_status, "INTZ %s | depth 0x%08X msaa %u %ux%u | %s",
                      intzOk ? "yes" : "no", static_cast<unsigned>(dd.Format),
                      static_cast<unsigned>(dd.MultiSampleType), dd.Width, dd.Height,
                      why ? why : "active");
        if (why) WLOG_WARN("depth: inert: %s", why);
    }

    bool EnsureTexture(IDirect3DDevice9* dev, UINT w, UINT h)
    {
        if (g_surface && g_width == w && g_height == h) return true;
        ReleaseTexture();

        const HRESULT hr = dev->CreateTexture(w, h, 1, D3DUSAGE_DEPTHSTENCIL, kIntz, D3DPOOL_DEFAULT, &g_texture, nullptr);
        if (FAILED(hr) || !g_texture)
        {
            WLOG_WARN("depth: INTZ texture %ux%u create failed hr=0x%08lX; inert", w, h, static_cast<unsigned long>(hr));
            g_texture = nullptr;
            g_probe = Probe::Inert;
            std::snprintf(g_status, sizeof g_status, "INTZ create failed hr=0x%08lX", static_cast<unsigned long>(hr));
            return false;
        }
        g_texture->GetSurfaceLevel(0, &g_surface);
        g_width = w;
        g_height = h;
        WLOG_INFO("depth: INTZ depth texture %ux%u created", w, h);
        return g_surface != nullptr;
    }
}

namespace wxl::forever::depth
{
    bool Redirect(const wxl::events::WorldSceneBeginArgs& args)
    {
        auto* dev   = static_cast<IDirect3DDevice9*>(args.device);
        auto* scene = static_cast<IDirect3DSurface9*>(args.sceneDepth);
        if (!dev || !scene) return false;

        if (g_probe == Probe::Unknown) RunProbe(dev, scene);
        if (g_probe != Probe::Usable) return false;

        D3DSURFACE_DESC dd{};
        scene->GetDesc(&dd);
        if (dd.MultiSampleType != D3DMULTISAMPLE_NONE) return false;
        if (!EnsureTexture(dev, dd.Width, dd.Height)) return false;

        // Read before the pass: its minZ is the viewport it inherits from here.
        g_range = gx::WorldDepthRange();
        if (!(g_range.maxZ > g_range.minZ)) g_range = { 0.0f, 1.0f };
        static bool logged = false;
        if (!logged)
        {
            logged = true;
            const gx::DepthRange sky = gx::SkyDepthRange();
            WLOG_INFO("depth: world depth range %.4f-%.4f, sky %.4f-%.4f", g_range.minZ, g_range.maxZ, sky.minZ, sky.maxZ);
        }

        *args.depthOverride = g_surface;
        return true;
    }

    bool Owns(const void* sceneDepth) { return g_surface && sceneDepth == g_surface; }

    IDirect3DTexture9* Texture() { return g_texture; }
    unsigned Width() { return g_width; }
    unsigned Height() { return g_height; }
    wxl::game::gx::DepthRange Range() { return g_range; }
    const char* Status() { return g_status; }

    void Release()
    {
        ReleaseTexture();
        g_probe = Probe::Unknown;
    }
}
