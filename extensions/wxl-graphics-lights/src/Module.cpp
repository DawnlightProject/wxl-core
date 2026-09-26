// wxl-graphics-lights: entry points, the published service tables, the gather pass and the device lifecycle.
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

#include "core/Extension.hpp"
#include "lights/Cookies.hpp"
#include "lights/Lights.hpp"
#include "lights/Omni.hpp"
#include "lights/Families.hpp"
#include "lights/Rooms.hpp"
#include "lights/Sky.hpp"
#include "render/Render.hpp"
#include "ui/Panel.hpp"

#include "wxl/EventScript.hpp"
#include "wxl/GraphicsLightsApi.h"
#include "wxl/GraphicsLightsSourcesApi.h"
#include "wxl/GraphicsLightsResolveApi.h"
#include "wxl/GraphicsLightsSurfaceApi.h"
#include "wxl/gfx/Ui.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstring>
#include <new>

namespace wxl::gfx::lights
{
    const WXL_Api* g_api = nullptr;
}

namespace
{
    namespace gl = wxl::gfx::lights;
    namespace ev = wxl::events;

    /**
     * @brief Runs one service call so that no exception leaves it.
     *
     * Nothing with a C++ ABI may cross into the caller's module (PluginApi.h), and an allocation
     * failing in a 32-bit process must cost that one call, not the client.
     */
    template <class R, class F>
    R Guarded(const char* what, R fallback, F&& call) noexcept
    {
        try
        {
            return call();
        }
        catch (const std::bad_alloc&)
        {
            LIGHTS_LOG_ERROR("%s: out of memory", what);
        }
        catch (...)
        {
            LIGHTS_LOG_ERROR("%s: unexpected exception", what);
        }
        return fallback;
    }

    template <class F>
    void GuardedVoid(const char* what, F&& call) noexcept
    {
        Guarded(what, 0, [&] { call(); return 0; });
    }

    // --- the pass: first in every frame's begin phase, so the list is ready for every consumer ------

    constexpr int32_t kPassOrder = -1000000;
    uint32_t g_pass = 0;

    uint32_t __cdecl PassWants(void*)
    {
        return Guarded("lights wants", 0u, [] { return gl::PollWanted() ? WXL_GFX_NEED_RUN : 0u; });
    }

    void __cdecl PassBegin(void*, const WXL_GfxBeginFrame* frame)
    {
        if (!frame || !frame->device) return;
        GuardedVoid("lights begin", [&] { gl::Frame(static_cast<IDirect3DDevice9*>(frame->device), frame->frameIndex); });
    }

    /// Registers the pass once wxl-graphics-extend is there; extensions load in folder order.
    void EnsurePass()
    {
        if (g_pass) return;
        const WXL_GraphicsExtendApi* gfx = gl::Gfx();
        if (!gfx) return;
        WXL_GfxPassDesc desc{};
        desc.structSize = sizeof desc;
        desc.name = "lights";
        desc.order = kPassOrder;
        desc.wants = &PassWants;
        desc.begin = &PassBegin;
        desc.draw = nullptr;
        desc.user = nullptr;
        g_pass = gfx->AddPass(&desc);
        if (g_pass) LIGHTS_LOG_INFO("lights: pass registered with wxl-graphics-extend (order %d, begin only)", kPassOrder);
        else LIGHTS_LOG_WARN("lights: wxl-graphics-extend refused the pass; no list will be published");
    }

    // --- the C ABI: every entry __cdecl, as PluginApi.h requires of anything crossing --------------

    void __cdecl ApiWant(void) { gl::Want(); }

    const WXL_GfxLight* __cdecl ApiCurrent(int* count, uint32_t* frameIndex)
    {
        int n = 0;
        uint32_t frame = 0;
        const WXL_GfxLight* list = gl::CurrentAbi(n, frame);
        if (count) *count = n;
        if (frameIndex) *frameIndex = frame;
        return list;
    }

    void __cdecl ApiSetGiven(const void* key, const WXL_GfxLight* lights, int count)
    {
        GuardedVoid("SetGiven", [&] { gl::SetGiven(key, lights, count); });
    }

    void __cdecl ApiSetInteriorGate(int on) { gl::SetInteriorGate(on != 0); }

    void* __cdecl ApiLightTexture(void) { return gl::LightTexture(); }
    void* __cdecl ApiClusterTexture(void) { return gl::ClusterTexture(); }

    void __cdecl ApiClusterTextureSize(int* width, int* height)
    {
        if (width) *width = gl::ClusterTextureWidth();
        if (height) *height = gl::ClusterTextureHeight();
    }

    void* __cdecl ApiOmniTexture(void) { return gl::OmniTexture(); }

    int __cdecl ApiOmniSlots(WXL_GfxOmniSlot* out, int max)
    {
        if (!out || max <= 0) return 0;
        int count = 0;
        uint32_t frame = 0;
        const WXL_GfxOmniSlot* slots = gl::CurrentOmni(count, frame);
        const int n = std::min(count, max);
        for (int i = 0; i < n; ++i) out[i] = slots[i];
        return n;
    }

    void* __cdecl ApiCookieAtlas(void) { return gl::cookies::Atlas(); }

    void __cdecl ApiClusterConstants(float clusterC[4], float clusterD[4])
    {
        float c[4], d[4];
        gl::ClusterConstants(c, d);
        if (clusterC) std::memcpy(clusterC, c, sizeof c);
        if (clusterD) std::memcpy(clusterD, d, sizeof d);
    }

    void __cdecl ApiCarriedConstants(float out[4])
    {
        if (out) gl::CarriedConstants(out);
    }

    void __cdecl ApiCookieConstants(float cookieC[4], float cookieD[4], float cookieE[4])
    {
        float c[4], d[4], e[4];
        gl::cookies::Constants(c, d, e);
        if (cookieC) std::memcpy(cookieC, c, sizeof c);
        if (cookieD) std::memcpy(cookieD, d, sizeof d);
        if (cookieE) std::memcpy(cookieE, e, sizeof e);
    }

    float __cdecl ApiRoomCross(void) { return gl::RoomCross(); }

    int __cdecl ApiRooms(const float** rows)
    {
        if (rows) *rows = &gl::rooms::Rows()[0][0];
        return gl::rooms::Count();
    }

    int __cdecl ApiCameraIndoor(void) { return gl::rooms::CameraIndoor() ? 1 : 0; }

    int __cdecl ApiRoomOf(const float r[3], float* floor, float* ceiling)
    {
        if (!r) return -1;
        float lo = 0.0f, hi = 0.0f;
        const int room = gl::rooms::RoomOf(r, lo, hi);
        if (floor) *floor = lo;
        if (ceiling) *ceiling = hi;
        return room;
    }

    int __cdecl ApiMovingLights(WXL_GfxMovingLight* out, int max, int withCarried)
    {
        if (!out || max <= 0) return 0;
        return gl::MovingLights(out, max, withCarried != 0);
    }

    const char* __cdecl ApiStatus(void) { return gl::Status(); }

    int __cdecl ApiRoomWeights(const float** weights)
    {
        if (weights) *weights = gl::rooms::Weights();
        return gl::rooms::Count();
    }

    // --- wxl.graphics-lights.sources ------------------------------------------------------------------

    const WXL_GfxLightSource* __cdecl ApiSources(int* count, uint32_t* frameIndex)
    {
        int n = 0;
        uint32_t frame = 0;
        const WXL_GfxLightSource* list = gl::CurrentSources(n, frame);
        if (count) *count = n;
        if (frameIndex) *frameIndex = frame;
        return list;
    }

    void* __cdecl ApiSourceTexture(void) { return gl::SourceTexture(); }

    int __cdecl ApiSky(WXL_GfxSkyLight* out)
    {
        const WXL_GfxSkyLight& s = gl::sky::Current();
        if (!out || !s.valid) return 0;
        const uint32_t size = std::min<uint32_t>(out->structSize, sizeof(WXL_GfxSkyLight));
        if (size < sizeof(uint32_t)) return 0;
        WXL_GfxSkyLight copy = s;
        copy.structSize = size;
        std::memcpy(out, &copy, size);
        return 1;
    }

    const char* __cdecl ApiFamilyName(uint32_t family) { return gl::families::Name(family); }

    const WXL_GraphicsLightsSourcesApi kSourcesApi = {
        sizeof(WXL_GraphicsLightsSourcesApi),
        WXL_GRAPHICS_LIGHTS_SOURCES_API_VERSION,
        &ApiSources,
        &ApiSourceTexture,
        &ApiSky,
        &ApiFamilyName,
    };

    // --- wxl.graphics-lights.surface: kept published, its half-resolution grid is gone --------------------

    int __cdecl ApiSurfaceOutputs(WXL_GfxSurfaceOutputs*) { return 0; }
    int __cdecl ApiSurfaceActive(void) { return 0; }
    const char* __cdecl ApiSurfaceStatus(void) { return "surface outputs: retired (the surfaces are lit at full resolution; see wxl.graphics-lights.field)"; }

    const WXL_GraphicsLightsSurfaceApi kSurfaceApi = {
        sizeof(WXL_GraphicsLightsSurfaceApi),
        WXL_GRAPHICS_LIGHTS_SURFACE_API_VERSION,
        &ApiSurfaceOutputs,
        &ApiSurfaceActive,
        &ApiSurfaceStatus,
    };

    const WXL_GraphicsLightsApi kApi = {
        sizeof(WXL_GraphicsLightsApi),
        WXL_GRAPHICS_LIGHTS_API_VERSION,
        &ApiWant,
        &ApiCurrent,
        &ApiSetGiven,
        &ApiSetInteriorGate,
        &ApiLightTexture,
        &ApiClusterTexture,
        &ApiClusterTextureSize,
        &ApiOmniTexture,
        &ApiOmniSlots,
        &ApiCookieAtlas,
        &ApiClusterConstants,
        &ApiCarriedConstants,
        &ApiCookieConstants,
        &ApiRoomCross,
        &ApiRooms,
        &ApiCameraIndoor,
        &ApiRoomOf,
        &ApiMovingLights,
        &ApiStatus,
        &ApiRoomWeights,
    };

    /// The pass joins the scheduler as soon as the service is found; DEFAULT-pool textures go before
    /// the engine resets the device and come back with the next publish.
    class Lifecycle final : public wxl::ext::EventScript
    {
    public:
        Lifecycle()
        {
            on<&Lifecycle::OnFrame>(ev::Event::OnFrame);
            on<&Lifecycle::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

        void OnFrame(const ev::FrameArgs&)
        {
            GuardedVoid("register render passes", [] { gl::render::Register(); });
            if (g_pass) return;
            GuardedVoid("register pass", [] { EnsurePass(); });
        }

        void OnDeviceLost(const ev::DeviceResetArgs&)
        {
            GuardedVoid("device lost", [] { gl::OnDeviceLost(); gl::render::OnDeviceLost(); });
        }
    };
}

const WXL_PluginInfo* __cdecl WXL_Query(void)
{
    static const WXL_PluginInfo info = {
        sizeof(WXL_PluginInfo),
        WXL_API_VERSION,
        "wxl-graphics-lights",
        1,
        WXL_CLIENT_BUILD,
    };
    return &info;
}

int __cdecl WXL_Load(const WXL_Api* api)
{
    if (!api || api->apiVersion != WXL_API_VERSION) return 0;

    gl::g_api = api;

    // Bind before the first subclass exists: EventScript has no table until it is handed one.
    wxl::ext::EventScript::Bind(api);
    wxl::gfx::ui::Bind(api);

    gl::Install();
    gl::cookies::Install();
    gl::rooms::Install();
    gl::omni::Install();
    gl::render::Install();
    static Lifecycle lifecycle;
    gl::ui::Install();

    api->PublishInterface(WXL_GRAPHICS_LIGHTS_API_NAME, WXL_GRAPHICS_LIGHTS_API_VERSION,
                          const_cast<WXL_GraphicsLightsApi*>(&kApi));
    api->PublishInterface(WXL_GRAPHICS_LIGHTS_SOURCES_API_NAME, WXL_GRAPHICS_LIGHTS_SOURCES_API_VERSION,
                          const_cast<WXL_GraphicsLightsSourcesApi*>(&kSourcesApi));
    api->PublishInterface(WXL_GRAPHICS_LIGHTS_SURFACE_API_NAME, WXL_GRAPHICS_LIGHTS_SURFACE_API_VERSION,
                          const_cast<WXL_GraphicsLightsSurfaceApi*>(&kSurfaceApi));

    api->Log(WXL_LOG_INFO, gl::kTag, "ready: published %s v%d, %s v%d, %s v%d; the HDR chain and the surfaces follow the render settings",
             WXL_GRAPHICS_LIGHTS_API_NAME, WXL_GRAPHICS_LIGHTS_API_VERSION, WXL_GRAPHICS_LIGHTS_SOURCES_API_NAME,
             WXL_GRAPHICS_LIGHTS_SOURCES_API_VERSION, WXL_GRAPHICS_LIGHTS_RESOLVE_API_NAME, WXL_GRAPHICS_LIGHTS_RESOLVE_API_VERSION);
    return 1;
}
