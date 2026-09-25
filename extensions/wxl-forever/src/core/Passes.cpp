// wxl-forever: the order the suite's render features draw in after the world pass.
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
#include "Passes.hpp"
#include "Matrix.hpp"
#include "SceneDepth.hpp"

#include "wxl/EventScript.hpp"
#include "game/Camera.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <vector>

namespace
{
    namespace ev  = wxl::events;
    namespace cam = wxl::game::camera;
    namespace ps  = wxl::forever::passes;

    struct Pass
    {
        int          order;
        const char*  name;
        ps::WantsFn  wants;
        ps::DrawFn   draw;
    };

    std::vector<Pass> g_passes;
    std::vector<ps::BeginFn> g_begins;
    uint32_t          g_index = 0;

    bool AnyWants()
    {
        for (const Pass& p : g_passes) if (p.wants()) return true;
        return false;
    }

    class Scheduler final : public wxl::ext::EventScript
    {
    public:
        Scheduler()
        {
            on<&Scheduler::OnWorldSceneBegin>(ev::Event::OnWorldSceneBegin);
            on<&Scheduler::OnWorldSceneEnd>(ev::Event::OnWorldSceneEnd);
            on<&Scheduler::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

        void OnWorldSceneBegin(const ev::WorldSceneBeginArgs& a)
        {
            if (!AnyWants()) return;
            wxl::forever::depth::Redirect(a);
            ps::BeginFrame b{};
            b.device = static_cast<IDirect3DDevice9*>(a.device);
            b.sceneDepth = a.sceneDepth;
            b.normalTarget = a.normalTarget;
            b.colorOverride = a.colorOverride;
            cam::GetPosition(b.eye);
            wxl::forever::matrix::CameraRelativeView(cam::GetView(), b.eye, b.viewRel);
            wxl::forever::matrix::CameraRelativeViewProj(cam::GetView(), cam::GetProjection(), b.eye, b.viewProjRel);
            b.index = g_index + 1;
            for (ps::BeginFn begin : g_begins) begin(b);
        }

        void OnWorldSceneEnd(const ev::WorldSceneEndArgs& a)
        {
            // Only a pass that actually drew into our texture is sampled.
            if (!wxl::forever::depth::Owns(a.sceneDepth)) return;

            ps::Frame f{};
            f.device   = static_cast<IDirect3DDevice9*>(a.device);
            f.depth    = wxl::forever::depth::Texture();
            f.width    = wxl::forever::depth::Width();
            f.height   = wxl::forever::depth::Height();
            f.rangeMin = wxl::forever::depth::Range().minZ;
            f.rangeMax = wxl::forever::depth::Range().maxZ;
            cam::GetPosition(f.eye);
            wxl::forever::matrix::CameraRelativeViewProj(cam::GetView(), cam::GetProjection(), f.eye, f.viewProjRel);
            f.index = ++g_index;
            f.normals = a.normalTarget;
            f.sceneColor = a.sceneColor;
            f.colorResolved = a.colorResolved;

            // With the world in HDR, the passes draw on it; the back buffer receives the result.
            IDirect3DSurface9* backBuffer = nullptr;
            f.device->GetRenderTarget(0, &backBuffer);
            f.backBuffer = backBuffer;
            if (a.sceneColor) f.device->SetRenderTarget(0, static_cast<IDirect3DSurface9*>(a.sceneColor));
            for (const Pass& p : g_passes)
                if (p.wants()) p.draw(f);
            if (a.sceneColor) f.device->SetRenderTarget(0, backBuffer);
            if (backBuffer) backBuffer->Release();
        }

        /// The depth texture is DEFAULT pool; it goes before the reset and the device is probed again after.
        void OnDeviceLost(const ev::DeviceResetArgs&) { wxl::forever::depth::Release(); }
    };
}

namespace wxl::forever::passes
{
    void Add(int order, const char* name, WantsFn wants, DrawFn draw)
    {
        g_passes.push_back({ order, name, wants, draw });
        std::stable_sort(g_passes.begin(), g_passes.end(), [](const Pass& a, const Pass& b) { return a.order < b.order; });
        WLOG_INFO("passes: %s at order %d", name, order);
    }

    uint32_t FrameIndex() { return g_index; }

    void AddBegin(BeginFn begin) { g_begins.push_back(begin); }

    void Install()
    {
        static Scheduler scheduler;
    }
}
