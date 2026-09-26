// wxl-graphics-shadow: the orchestration. Three passes join wxl-graphics-extend's scheduler:
//   "shadow.choose" (D3D9, begin only, WXL_GFX_SHADOW_ORDER): the sun, the terrain caster, the bodies,
//                   the slots and the core's omni maps, before the world pass renders them;
//   "shadow"        (Vulkan compute, WXL_GFX_SHADOW_ORDER): the maps' filtering, the public block, masks;
//   "shadow.debug"  (D3D9, WXL_GFX_ORDER_OVERLAY): the debug view over the finished frame.
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

#include "Shadow.hpp"
#include "api/Api.hpp"
#include "core/Extension.hpp"
#include "core/Settings.hpp"
#include "gpu/Gpu.hpp"
#include "gpu/Overlay.hpp"
#include "gpu/Record.hpp"
#include "policy/Bodies.hpp"
#include "policy/Slots.hpp"
#include "sun/Sun.hpp"
#include "terrain/Caster.hpp"
#include "terrain/Horizon.hpp"

#include "wxl/EventScript.hpp"
#include "game/Camera.hpp"
#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

namespace
{
    namespace ev = wxl::events;
    namespace sh = wxl::gfx::shadow;
    namespace sl = wxl::gfx::shadow::slots;
    namespace bd = wxl::gfx::shadow::bodies;

    sh::Published g_pub;
    uint32_t      g_want[2] = {};
    bool          g_registered = false;
    uint32_t      g_choosePass = 0, g_computePass = 0, g_overlayPass = 0;
    void*         g_debugTexture = nullptr;
    bool          g_masksThisFrame = false;
    double        g_lastRecorded = -1e9;
    bool          g_wasEnabled = true;
    double        g_nextSummary = 10.0;
    uint32_t      g_summaryFrames = 0, g_summaryFaces = 0, g_summaryRecords = 0;
    char          g_status[256] = "graphics shadow: starting";

    double Now()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    bool Vulkan()
    {
        const WXL_GfxVulkanApi* vk = sh::Vk();
        return vk && vk->Available() != 0;
    }

    bool DebugView() { return sh::Config().view != sh::kViewNone; }

    /// This frame's slots, as GetFrame and Slots report them.
    void PublishSlots()
    {
        const sl::Slot* slots = sl::List();
        std::memset(g_pub.slotByIndex, 0xFF, sizeof g_pub.slotByIndex);
        for (int i = 0; i < WXL_GFX_SHADOW_SLOTS; ++i)
        {
            const sl::Slot& s = slots[i];
            WXL_GfxShadowSlot& out = g_pub.slots[i];
            out = WXL_GfxShadowSlot{};
            if (!s.id) { out.listIndex = -1; out.map = -1; continue; }
            out.lightId = s.id;
            out.listIndex = s.light.listIndex;
            out.weight = s.weight;
            out.mapWeight = s.map >= 0 ? s.mapWeight : 0.0f;
            out.map = s.map;
            out.flags = s.light.flags;
            if (s.light.listIndex >= 0 && s.light.listIndex < 128 && s.weight > 0.0f) g_pub.slotByIndex[s.light.listIndex] = int8_t(i);
        }
    }

    // --- the passes (C ABI callbacks: nothing may throw out of them) --------------------------------

    uint32_t __cdecl WantsChoose(void*)
    {
        try
        {
            // Polled once a frame before anything draws: last frame's block is over for consumers, and
            // the wants window rolls (a want counts this frame and the next).
            sh::gpu::EndFrame();
            g_pub.ran = false;
            g_want[1] = g_want[0];
            g_want[0] = 0;
            const sh::Settings& s = sh::Config();
            if (!s.enabled || (!sh::Wanted() && !DebugView())) return 0;
            // Shadows read from wxl-graphics-lights' list need that list gathered.
            if (sh::Lights()) sh::Lights()->Want();
            return WXL_GFX_NEED_RUN;
        }
        catch (...)
        {
            return 0;
        }
    }

    void __cdecl BeginChoose(void*, const WXL_GfxBeginFrame* f)
    {
        try
        {
            if (!f) return;
            const sh::Settings& s = sh::Config();
            const float dt = std::clamp(f->deltaTime, 0.0f, 0.25f);
            const float* eye = f->view.eye;
            if (Vulkan())
            {
                bd::Update(eye, s.capsuleRange, s.capsuleRadius);
                sl::Choose(eye, Now(), dt);
            }
            else sl::Idle();
            PublishSlots();
            g_masksThisFrame = (sh::Wanted() & WXL_GFX_SHADOW_WANT_MASKS) != 0 || DebugView();
            g_debugTexture = DebugView() ? wxl::gfx::shadow::overlay::Prepare(f->device, f->width, f->height) : nullptr;
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; SHADOW_LOG_ERROR("exception before the world pass; the frame is skipped"); }
        }
    }

    uint32_t __cdecl WantsCompute(void*)
    {
        try
        {
            const sh::Settings& s = sh::Config();
            if (!s.enabled || !Vulkan()) return 0;
            const bool masks = (sh::Wanted() & WXL_GFX_SHADOW_WANT_MASKS) != 0 || DebugView();
            if (!sh::Wanted() && !DebugView())
            {
                // Consumers may still write the binding set: the stand-ins must exist.
                sh::gpu::Prepare(sh::Vk(), false, false);
                return 0;
            }
            if (!sh::gpu::Prepare(sh::Vk(), masks, DebugView())) return 0;
            return masks ? (WXL_GFX_NEED_DEPTH | WXL_GFX_NEED_NORMALS) : WXL_GFX_NEED_RUN;
        }
        catch (...)
        {
            return 0;
        }
    }

    void __cdecl RecordCompute(void*, const WXL_GfxVkFrame* vk)
    {
        try
        {
            if (vk && sh::gpu::Record(sh::Vk(), *vk, g_masksThisFrame, g_debugTexture)) g_lastRecorded = Now();
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; SHADOW_LOG_ERROR("exception while recording; the frame is skipped"); }
        }
    }

    uint32_t __cdecl WantsOverlay(void*) { return sh::Config().enabled && DebugView() && Vulkan() ? WXL_GFX_NEED_RUN : 0; }

    void __cdecl DrawOverlay(void*, const WXL_GfxFrame* frame)
    {
        try
        {
            if (frame && g_pub.ran && (g_pub.flags & WXL_GFX_SHADOW_FRAME_MASKS)) wxl::gfx::shadow::overlay::Draw(*frame);
        }
        catch (...) {}
    }

    void Register()
    {
        if (g_registered) return;
        const WXL_GraphicsExtendApi* gfx = sh::Gfx();
        const WXL_GfxVulkanApi* vk = sh::Vk();
        if (!gfx || !vk) return;

        WXL_GfxPassDesc choose{};
        choose.structSize = sizeof choose;
        choose.name = "shadow.choose";
        choose.order = WXL_GFX_SHADOW_ORDER;
        choose.wants = &WantsChoose;
        choose.begin = &BeginChoose;
        g_choosePass = gfx->AddPass(&choose);

        WXL_GfxVkPassDesc compute{};
        compute.structSize = sizeof compute;
        compute.name = "shadow";
        compute.order = WXL_GFX_SHADOW_ORDER;
        compute.wants = &WantsCompute;
        compute.record = &RecordCompute;
        g_computePass = vk->AddComputePass(&compute);

        WXL_GfxPassDesc overlay{};
        overlay.structSize = sizeof overlay;
        overlay.name = "shadow.debug";
        overlay.order = WXL_GFX_ORDER_OVERLAY;
        overlay.wants = &WantsOverlay;
        overlay.draw = &DrawOverlay;
        g_overlayPass = gfx->AddPass(&overlay);

        g_registered = true;
        if (!g_choosePass || !g_computePass)
            SHADOW_LOG_ERROR("wxl-graphics-extend refused the passes (choose %u, compute %u); shadows stay off", g_choosePass, g_computePass);
        else
            SHADOW_LOG_INFO("passes registered at order %d (choose %u, compute %u), debug overlay %u", WXL_GFX_SHADOW_ORDER, g_choosePass,
                            g_computePass, g_overlayPass);
    }

    class ShadowModule final : public wxl::ext::EventScript
    {
    public:
        ShadowModule()
        {
            on<&ShadowModule::OnUpdate>(ev::Event::OnUpdate);
            on<&ShadowModule::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

        void OnUpdate(const ev::UpdateArgs& a)
        {
            try
            {
                Register();
                const bool enabled = sh::Config().enabled != 0;
                if (!enabled && g_wasEnabled)
                {
                    sl::ReleaseCore();
                    wxl::gfx::shadow::sun::Shutdown();
                }
                g_wasEnabled = enabled;
                if (!enabled) return;
                // The engine's own shadows gain these whether anyone reads this service or not: the
                // cascades follow a lower, truer sun (or the moon), and the land casts into them.
                wxl::gfx::shadow::sun::Update(std::clamp(a.dt, 0.0f, 0.25f));
                // The horizon block's residency and heights (the uploads happen in the compute record).
                if (sh::Config().terrainCaster || sh::Config().horizon)
                {
                    float eye[3];
                    wxl::game::camera::GetPosition(eye);
                    wxl::gfx::shadow::horizon::Update(nullptr, VK_NULL_HANDLE, eye);
                }
                wxl::gfx::shadow::caster::Frame(static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice()));
                Summary();
            }
            catch (...) {}
        }

        /// Every 10 s, one line: what ran, how many core faces a frame cost, the GPU time.
        void Summary()
        {
            ++g_summaryFrames;
            const WXL_OmniShadowsApi* core = sl::Core();
            if (core && core->FacesLastFrame) g_summaryFaces += core->FacesLastFrame();
            if (g_pub.ran) ++g_summaryRecords;
            const double now = Now();
            if (now < g_nextSummary) return;
            g_nextSummary = now + 10.0;
            if (g_summaryRecords)
                SHADOW_LOG_INFO("last 10 s: %u frames, %u recorded, %.1f core faces a frame, GPU %.3f ms; %s", g_summaryFrames,
                                g_summaryRecords, double(g_summaryFaces) / double(std::max(g_summaryFrames, 1u)),
                                std::max(sh::gpu::TotalMs(), 0.0f), sl::Status());
            g_summaryFrames = g_summaryFaces = g_summaryRecords = 0;
        }

        void OnDeviceLost(const ev::DeviceResetArgs&)
        {
            try { wxl::gfx::shadow::overlay::Release(); } catch (...) {}
        }
    };

    void __cdecl PanelBody(void*)
    {
        try
        {
            sh::Panel();
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; SHADOW_LOG_ERROR("exception while drawing the panel"); }
        }
    }
}

namespace wxl::gfx::shadow
{
    Published& Pub() { return g_pub; }

    void NoteWant(uint32_t what) { g_want[0] |= what; }

    uint32_t Wanted() { return g_want[0] | g_want[1]; }

    void SetPushedLights(const WXL_GfxShadowLight* lights, int count) { sl::SetPushed(lights, count, 0); }

    bool Active() { return Config().enabled && Vulkan() && Now() - g_lastRecorded < 1.0; }

    const char* StatusLine()
    {
        const Settings& s = Config();
        std::snprintf(g_status, sizeof g_status, "graphics shadow: %s%s; %s",
                      !s.enabled ? "off" : (!Vulkan() ? "no DXVK (everything lit)" : (Active() ? "drawing" : "idle (nobody reads shadows)")),
                      Active() && (g_pub.flags & WXL_GFX_SHADOW_FRAME_MASKS) ? ", masks written" : "", sl::Status());
        return g_status;
    }

    void Install()
    {
        LoadSettings();
        std::memset(g_pub.slotByIndex, 0xFF, sizeof g_pub.slotByIndex);
        for (WXL_GfxShadowSlot& s : g_pub.slots) { s.listIndex = -1; s.map = -1; }
        api::Publish();
        static ShadowModule module;
        g_api->UiAddPanel("Graphics Shadow", &PanelBody, nullptr);
        Register();
    }
}
