// wxl-graphics-extend: the post-world pass scheduler and the world pass's shared targets.
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

#include "Scheduler.hpp"
#include "SceneTargets.hpp"
#include "../core/Extension.hpp"
#include "../vulkan/Vulkan.hpp"

#include "wxl/EventScript.hpp"
#include "wxl/gfx/GpuTimer.hpp"
#include "wxl/gfx/Matrix.hpp"
#include "engine/events/Event.hpp"
#include "game/Camera.hpp"
#include "game/GBuffer.hpp"
#include "game/Gx.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace
{
    namespace ev    = wxl::events;
    namespace cam   = wxl::game::camera;
    namespace gx    = wxl::game::gx;
    namespace gb    = wxl::game::gbuffer;
    namespace mx    = wxl::gfx::matrix;
    namespace scene = wxl::gfx::frame::scene;

    // --- the pass registry ------------------------------------------------------------------------

    struct Pass
    {
        uint32_t       id = 0;
        std::string    name;
        int32_t        order = 0;
        WXL_GfxWantsFn wants = nullptr;
        WXL_GfxBeginFn begin = nullptr;
        WXL_GfxDrawFn  draw = nullptr;
        void*          user = nullptr;
        int            span = -1;       // GPU timer span; -1 without a draw, or once the slots are taken
        uint32_t       lastWants = 0;   // this frame's answer; 0 sits the pass out
        bool           ran = false;     // its draw ran on the last frame
    };

    // Heap objects: a pass's name is handed out as a pointer valid for the process, which a sort or a
    // reallocation of the vector must not move. Sorted by order, equal orders in add order.
    std::vector<std::unique_ptr<Pass>> g_passes;
    // Added from inside a wants / begin / draw callback: joins g_passes once the iteration is over.
    std::vector<std::unique_ptr<Pass>> g_pending;
    uint32_t g_nextId = 1;
    int      g_nextSpan = 0;
    int      g_iterating = 0;

    void SortPasses()
    {
        std::stable_sort(g_passes.begin(), g_passes.end(),
                         [](const std::unique_ptr<Pass>& a, const std::unique_ptr<Pass>& b) { return a->order < b->order; });
    }

    void MergePending()
    {
        if (g_iterating || g_pending.empty()) return;
        for (std::unique_ptr<Pass>& p : g_pending) g_passes.push_back(std::move(p));
        g_pending.clear();
        SortPasses();
    }

    /// Holds the pass vector still while callbacks run; a pass added meanwhile joins afterwards.
    struct Iteration
    {
        Iteration() { ++g_iterating; }
        ~Iteration() { --g_iterating; MergePending(); }
        Iteration(const Iteration&) = delete;
        Iteration& operator=(const Iteration&) = delete;
    };

    const Pass* Find(uint32_t id)
    {
        for (const std::unique_ptr<Pass>& p : g_passes)  if (p->id == id) return p.get();
        for (const std::unique_ptr<Pass>& p : g_pending) if (p->id == id) return p.get();
        return nullptr;
    }

    // --- configuration and clock --------------------------------------------------------------------

    struct Config
    {
        uint32_t supply  = WXL_GFX_NEED_DEPTH | WXL_GFX_NEED_NORMALS | WXL_GFX_NEED_HDR | WXL_GFX_NEED_ALBEDO;  // needs the service arranges
        bool     profile = true;
    } g_cfg;

    LARGE_INTEGER g_qpcFrequency{};
    LARGE_INTEGER g_qpcStart{};
    double        g_lastRunTime = 0.0;   // when the passes last ran, for deltaTime
    bool          g_hasRun = false;

    double Now()
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return double(now.QuadPart - g_qpcStart.QuadPart) / double(g_qpcFrequency.QuadPart);
    }

    // --- the frame in flight ------------------------------------------------------------------------

    IDirect3DDevice9* g_device = nullptr;    // while the begin phase is open
    bool     g_beginPhase = false;           // SetEngineLightBuffer is legal
    bool     g_active = false;               // begin ran with a request: end runs the passes
    bool     g_lightBound = false;           // a provider bound sampler 12 this frame
    uint32_t g_requested = 0;                // this frame's union of wants
    uint32_t g_lastRequested = 0, g_lastAvailable = 0;
    uint32_t g_frameIndex = 0;
    UINT     g_worldW = 0, g_worldH = 0;
    gx::DepthRange g_range = { 0.0f, 1.0f };
    float    g_time = 0.0f, g_deltaTime = 0.0f;

    WXL_GfxView g_view{};
    bool  g_prevValid = false;
    float g_prevEye[3] = {};
    float g_prevViewProjRel[16] = {};

    WXL_GfxFrame g_current{};
    bool         g_currentValid = false;

    wxl::gfx::GpuTimer g_timer;

    /// The camera the coming world pass draws with, and last frame's for reprojection.
    void ComputeView(WXL_GfxView& v)
    {
        cam::GetPosition(v.eye);
        mx::Copy(cam::GetView(), v.view);
        mx::Copy(cam::GetProjection(), v.projection);
        // The camera holds the engine's GL-form projection (clip z in -w..w); CGxDeviceD3d converts it
        // to D3D's 0..w before drawing (src/game/Shadows.cpp does the same for its ortho). Hand out the
        // form the depth buffer was written with: z' = (z + w) / 2, column by column.
        for (int r = 0; r < 4; ++r)
            v.projection[r * 4 + 2] = 0.5f * (v.projection[r * 4 + 2] + v.projection[r * 4 + 3]);
        mx::CameraRelativeView(v.view, v.eye, v.viewRel);
        mx::Mul4(v.viewRel, v.projection, v.viewProjRel);
        if (!mx::Invert4(v.viewProjRel, v.invViewProjRel)) mx::Identity(v.invViewProjRel);

        if (g_prevValid)
        {
            for (int i = 0; i < 3; ++i) v.prevEye[i] = g_prevEye[i];
            mx::Copy(g_prevViewProjRel, v.prevViewProjRel);
        }
        else
        {
            for (int i = 0; i < 3; ++i) v.prevEye[i] = v.eye[i];
            mx::Copy(v.viewProjRel, v.prevViewProjRel);
        }
        // r_prev = r + (eye - prevEye): this frame's camera-relative point in last frame's frame,
        // then last frame's clip.
        float shift[16];
        mx::Translation(v.eye[0] - v.prevEye[0], v.eye[1] - v.prevEye[1], v.eye[2] - v.prevEye[2], shift);
        mx::Mul4(shift, v.prevViewProjRel, v.reprojectRel);

        const float* p = v.projection;
        v.depthLinearize[0] = p[14];
        v.depthLinearize[1] = p[15];
        v.depthLinearize[2] = p[11];
        v.depthLinearize[3] = p[10];
    }

    /// Whether render target 0 -- what the colour override replaces -- can be stood in for by a
    /// plain FP16 texture: not multisampled, and the world's size.
    bool TargetTakesHdr(IDirect3DDevice9* dev, UINT width, UINT height)
    {
        IDirect3DSurface9* rt = nullptr;
        if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return false;
        D3DSURFACE_DESC d{};
        rt->GetDesc(&d);
        rt->Release();
        const bool ok = d.MultiSampleType == D3DMULTISAMPLE_NONE && d.Width == width && d.Height == height;
        static bool logged = false;
        if (!ok && !logged)
        {
            logged = true;
            GFX_LOG_WARN("frame: render target 0 is %ux%u msaa %u, the world %ux%u: no HDR colour",
                         d.Width, d.Height, static_cast<unsigned>(d.MultiSampleType), width, height);
        }
        return ok;
    }

    /// The world's depth as a texture: ours, or a texture another subscriber drew it into.
    IDirect3DTexture9* DepthTexture(void* sceneDepth)
    {
        auto* s = static_cast<IDirect3DSurface9*>(sceneDepth);
        IDirect3DTexture9* t = scene::Owned(scene::Kind::Depth, s);
        return t ? t : scene::ContainerTexture(s, true);
    }

    IDirect3DTexture9* TargetTexture(scene::Kind kind, void* surface)
    {
        auto* s = static_cast<IDirect3DSurface9*>(surface);
        IDirect3DTexture9* t = scene::Owned(kind, s);
        return t ? t : scene::ContainerTexture(s, false);
    }

    class Scheduler final : public wxl::ext::EventScript
    {
    public:
        Scheduler()
        {
            on<&Scheduler::OnWorldSceneBegin>(ev::Event::OnWorldSceneBegin);
            on<&Scheduler::OnWorldSceneEnd>(ev::Event::OnWorldSceneEnd);
        }

        void OnWorldSceneBegin(const ev::WorldSceneBeginArgs& a)
        {
            // Last frame's pointers end here, whether or not a new frame runs.
            g_currentValid = false;
            g_active = false;

            // The Vulkan side probes DXVK once per device; its compute passes join the request.
            wxl::gfx::vulkan::OnDevice(a.device);
            uint32_t requested = wxl::gfx::vulkan::PollWants();
            {
                Iteration guard;
                for (std::unique_ptr<Pass>& p : g_passes)
                {
                    p->ran = false;
                    p->lastWants = p->wants(p->user);
                    requested |= p->lastWants;
                }
            }
            // The albedo target is only bound beside the normals.
            if (requested & WXL_GFX_NEED_ALBEDO) requested |= WXL_GFX_NEED_NORMALS;
            g_lastRequested = requested;
            g_lastAvailable = 0;
            if (!requested) return;

            auto* dev        = static_cast<IDirect3DDevice9*>(a.device);
            auto* sceneDepth = static_cast<IDirect3DSurface9*>(a.sceneDepth);
            if (!dev || !sceneDepth) return;

            scene::Probe(dev, sceneDepth);
            const uint32_t caps = scene::Caps();

            D3DSURFACE_DESC dd{};
            sceneDepth->GetDesc(&dd);
            g_worldW = dd.Width;
            g_worldH = dd.Height;

            // What the config lets the service arrange; the rest of the request still runs the pass.
            const uint32_t wanted = requested & g_cfg.supply;
            uint32_t supplied = 0;

            // A target another subscriber already supplied is left in place: whoever owns it, the
            // end of the pass reports it to every pass.
            if ((wanted & WXL_GFX_NEED_DEPTH) && (caps & WXL_GFX_CAP_INTZ) && !(caps & WXL_GFX_CAP_PURE)
                && dd.MultiSampleType == D3DMULTISAMPLE_NONE)
            {
                if (*a.depthOverride) supplied |= WXL_GFX_NEED_DEPTH;
                else if (IDirect3DSurface9* s = scene::Ensure(scene::Kind::Depth, dev, dd.Width, dd.Height))
                {
                    *a.depthOverride = s;
                    supplied |= WXL_GFX_NEED_DEPTH;
                }
            }
            // Read before the pass: its minZ is the viewport the pass inherits from here.
            g_range = gx::WorldDepthRange();
            if (!(g_range.maxZ > g_range.minZ)) g_range = { 0.0f, 1.0f };
            static bool rangeLogged = false;
            if (!rangeLogged)
            {
                rangeLogged = true;
                const gx::DepthRange sky = gx::SkyDepthRange();
                GFX_LOG_INFO("frame: world depth range %.4f-%.4f, sky %.4f-%.4f", g_range.minZ, g_range.maxZ, sky.minZ, sky.maxZ);
            }

            if ((wanted & WXL_GFX_NEED_NORMALS) && (caps & WXL_GFX_CAP_MRT))
            {
                if (*a.normalTarget) supplied |= WXL_GFX_NEED_NORMALS;
                else if (IDirect3DSurface9* s = scene::Ensure(scene::Kind::Normals, dev, dd.Width, dd.Height))
                {
                    *a.normalTarget = s;
                    supplied |= WXL_GFX_NEED_NORMALS;
                }
            }
            if ((wanted & WXL_GFX_NEED_ALBEDO) && (supplied & WXL_GFX_NEED_NORMALS) && scene::ThreeTargets())
            {
                if (*a.albedoTarget) supplied |= WXL_GFX_NEED_ALBEDO;
                else if (IDirect3DSurface9* s = scene::Ensure(scene::Kind::Albedo, dev, dd.Width, dd.Height))
                {
                    *a.albedoTarget = s;
                    supplied |= WXL_GFX_NEED_ALBEDO;
                }
            }

            if ((wanted & WXL_GFX_NEED_HDR) && (caps & WXL_GFX_CAP_FP16_TARGET))
            {
                if (*a.colorOverride) supplied |= WXL_GFX_NEED_HDR;
                else if (TargetTakesHdr(dev, dd.Width, dd.Height))
                {
                    if (IDirect3DSurface9* s = scene::Ensure(scene::Kind::Hdr, dev, dd.Width, dd.Height))
                    {
                        *a.colorOverride = s;
                        supplied |= WXL_GFX_NEED_HDR;
                    }
                }
            }

            const double now = Now();
            g_time      = float(now);
            g_deltaTime = g_hasRun ? float(std::clamp(now - g_lastRunTime, 0.0, 1.0)) : 0.0f;
            g_lastRunTime = now;
            g_hasRun = true;

            ComputeView(g_view);
            for (int i = 0; i < 3; ++i) g_prevEye[i] = g_view.eye[i];
            mx::Copy(g_view.viewProjRel, g_prevViewProjRel);
            g_prevValid = true;

            WXL_GfxBeginFrame b{};
            b.structSize = sizeof b;
            b.device     = dev;
            b.frameIndex = g_frameIndex + 1;
            b.width      = g_worldW;
            b.height     = g_worldH;
            b.requested  = requested;
            b.supplied   = supplied;
            b.time       = g_time;
            b.deltaTime  = g_deltaTime;
            b.view       = g_view;

            g_requested  = requested;
            g_lightBound = false;
            g_device     = dev;
            g_beginPhase = true;
            {
                Iteration guard;
                for (std::unique_ptr<Pass>& p : g_passes)
                    if (p->lastWants && p->begin) p->begin(p->user, &b);
            }
            g_beginPhase = false;
            g_device     = nullptr;
            g_active     = true;
        }

        void OnWorldSceneEnd(const ev::WorldSceneEndArgs& a)
        {
            if (!g_active) return;
            g_active = false;
            auto* dev = static_cast<IDirect3DDevice9*>(a.device);
            if (!dev) return;

            // The engine's materials are done with the light buffer; nothing drawn after the world
            // pass -- the passes below, portraits, previews -- may add it.
            if (g_lightBound)
            {
                g_lightBound = false;
                dev->SetTexture(gb::kLightBufferSampler, nullptr);
                static const float kOff[4] = {};
                gb::SetEnginePixelConstants(gb::kLightBufferParams, kOff, 1);
            }

            // What the core reports is what the world was drawn into, whoever supplied it: a begin
            // subscriber after ours may have replaced what we set.
            const uint32_t wanted = g_requested & g_cfg.supply;
            uint32_t available = WXL_GFX_NEED_RUN;
            WXL_GfxFrame f{};

            if ((wanted & WXL_GFX_NEED_DEPTH) && a.sceneDepth)
            {
                if (IDirect3DTexture9* t = DepthTexture(a.sceneDepth))
                {
                    f.depthTexture = t;
                    available |= WXL_GFX_NEED_DEPTH;
                }
            }
            if ((wanted & WXL_GFX_NEED_NORMALS) && a.normalTarget)
            {
                if (IDirect3DTexture9* t = TargetTexture(scene::Kind::Normals, a.normalTarget))
                {
                    f.normalTexture = t;
                    available |= WXL_GFX_NEED_NORMALS;
                }
            }
            if ((wanted & WXL_GFX_NEED_ALBEDO) && a.albedoTarget)
            {
                if (IDirect3DTexture9* t = TargetTexture(scene::Kind::Albedo, a.albedoTarget))
                {
                    f.albedoTexture = t;
                    available |= WXL_GFX_NEED_ALBEDO;
                }
            }
            // With the world in a colour override nobody resolved yet the passes draw on it, whoever
            // owns it and whether or not one of ours asked: the back buffer holds no world yet, and
            // the resolve that follows would paint over anything drawn there.
            IDirect3DSurface9* hdr = nullptr;
            if (a.sceneColor && a.colorResolved && *a.colorResolved == 0)
            {
                hdr = static_cast<IDirect3DSurface9*>(a.sceneColor);
                if (wanted & WXL_GFX_NEED_HDR)
                {
                    if (IDirect3DTexture9* t = TargetTexture(scene::Kind::Hdr, a.sceneColor))
                    {
                        f.sceneColorTexture = t;
                        available |= WXL_GFX_NEED_HDR;
                    }
                }
            }

            IDirect3DSurface9* backBuffer = nullptr;
            if (FAILED(dev->GetRenderTarget(0, &backBuffer))) backBuffer = nullptr;
            if (!backBuffer) hdr = nullptr;   // nothing to put back afterwards: stay where we are

            f.structSize    = sizeof f;
            f.device        = dev;
            f.frameIndex    = ++g_frameIndex;
            f.width         = g_worldW;
            f.height        = g_worldH;
            f.available     = available;
            f.depthRange[0] = g_range.minZ;
            f.depthRange[1] = g_range.maxZ;
            f.target        = hdr ? hdr : backBuffer;
            f.backBuffer    = backBuffer;
            f.colorResolved = a.colorResolved;
            f.time          = g_time;
            f.deltaTime     = g_deltaTime;
            f.view          = g_view;

            // Compute first, in one submission after the world pass: every D3D9 pass below may read
            // what it produced.
            wxl::gfx::vulkan::RunComputeBlock(f);

            if (hdr) dev->SetRenderTarget(0, hdr);
            const bool timing = g_cfg.profile;
            if (timing) g_timer.Begin(dev);
            {
                Iteration guard;
                for (std::unique_ptr<Pass>& p : g_passes)
                {
                    if (!p->lastWants || !p->draw) continue;
                    p->draw(p->user, &f);
                    p->ran = true;
                    // A pass without a slot still closes its span (-1), so its time is nobody's.
                    if (timing) g_timer.Mark(p->span);
                    // Resolved: the passes after it draw on the finished image, not the HDR colour.
                    if (hdr && f.colorResolved && *f.colorResolved)
                    {
                        dev->SetRenderTarget(0, backBuffer);
                        f.target = backBuffer;
                        hdr = nullptr;
                    }
                }
            }
            if (timing) g_timer.End();
            if (hdr) dev->SetRenderTarget(0, backBuffer);

            g_lastAvailable = available;
            f.target        = backBuffer;   // the passes are done: render target 0 is back
            g_current       = f;
            g_currentValid  = true;
            if (backBuffer) backBuffer->Release();
        }
    };
}

namespace wxl::gfx::frame
{
    void Install()
    {
        g_cfg.supply = (ConfigBool("WXL_GFX_DEPTH",   true) ? WXL_GFX_NEED_DEPTH   : 0u)
                     | (ConfigBool("WXL_GFX_NORMALS", true) ? WXL_GFX_NEED_NORMALS : 0u)
                     | (ConfigBool("WXL_GFX_HDR",     true) ? WXL_GFX_NEED_HDR     : 0u)
                     | (ConfigBool("WXL_GFX_ALBEDO",  true) ? WXL_GFX_NEED_ALBEDO  : 0u);
        g_cfg.profile = ConfigBool("WXL_GFX_PROFILE", true);

        QueryPerformanceFrequency(&g_qpcFrequency);
        QueryPerformanceCounter(&g_qpcStart);
        if (g_qpcFrequency.QuadPart <= 0) g_qpcFrequency.QuadPart = 1;

        static Scheduler scheduler;
        GFX_LOG_INFO("frame: depth %s, normals %s, albedo %s, hdr %s, profiling %s",
                     (g_cfg.supply & WXL_GFX_NEED_DEPTH)   ? "on" : "off",
                     (g_cfg.supply & WXL_GFX_NEED_NORMALS) ? "on" : "off",
                     (g_cfg.supply & WXL_GFX_NEED_ALBEDO)  ? "on" : "off",
                     (g_cfg.supply & WXL_GFX_NEED_HDR)     ? "on" : "off",
                     g_cfg.profile ? "on" : "off");
    }

    void OnDeviceLost()
    {
        scene::Release();
        g_timer.Release();
        g_current      = WXL_GfxFrame{};
        g_currentValid = false;
        g_active       = false;
        g_beginPhase   = false;
        g_device       = nullptr;
        g_lightBound   = false;
        // A reset usually means a resize: until the next world pass, followed targets size from the
        // back buffer rather than from a world that no longer exists.
        g_worldW = g_worldH = 0;
    }

    uint32_t AddPass(const WXL_GfxPassDesc* desc)
    {
        if (!desc || desc->structSize < sizeof(WXL_GfxPassDesc) || !desc->wants)
        {
            GFX_LOG_WARN("passes: pass rejected: %s",
                         !desc ? "null descriptor" : desc->structSize < sizeof(WXL_GfxPassDesc) ? "structSize too small" : "no wants callback");
            return 0;
        }

        auto p = std::make_unique<Pass>();
        p->id    = g_nextId++;
        p->order = desc->order;
        p->wants = desc->wants;
        p->begin = desc->begin;
        p->draw  = desc->draw;
        p->user  = desc->user;
        // A timer slot for the life of the pass; a pass that only asks for resources has nothing to time.
        if (desc->draw) p->span = g_nextSpan < wxl::gfx::GpuTimer::kSpans ? g_nextSpan++ : -1;
        if (desc->name && desc->name[0]) p->name = desc->name;
        else
        {
            char buf[32];
            std::snprintf(buf, sizeof buf, "pass#%u", p->id);
            p->name = buf;
        }
        GFX_LOG_INFO("passes: %s at order %d", p->name.c_str(), p->order);

        const uint32_t id = p->id;
        if (g_iterating) g_pending.push_back(std::move(p));
        else
        {
            g_passes.push_back(std::move(p));
            SortPasses();
        }
        return id;
    }

    int CurrentFrame(WXL_GfxFrame* out)
    {
        if (!out || !g_currentValid) return 0;
        *out = g_current;
        out->structSize = sizeof(WXL_GfxFrame);
        return 1;
    }

    float PassGpuMs(uint32_t passId)
    {
        if (!g_cfg.profile || !passId) return -1.0f;
        const Pass* p = Find(passId);
        return p && p->span >= 0 ? g_timer.Milliseconds(p->span) : -1.0f;
    }

    int SetEngineLightBuffer(void* texture, const float rows[16], const float params[4])
    {
        if (!g_beginPhase || !g_device)
        {
            static bool warned = false;
            if (!warned)
            {
                warned = true;
                GFX_LOG_WARN("SetEngineLightBuffer: only inside a begin callback; ignored");
            }
            return 0;
        }
        if (!texture || !rows || !params) return 0;

        // The engine's constant cache uploads only what changed: zeros first, so a still camera
        // (the same rows as last frame) still reaches the device.
        static const float kZero[16] = {};
        gb::SetEnginePixelConstants(gb::kLightBufferRows, kZero, 4);
        gb::SetEnginePixelConstants(gb::kLightBufferRows, rows, 4);
        gb::SetEnginePixelConstants(gb::kLightBufferParams, params, 1);

        IDirect3DDevice9* d = g_device;
        const DWORD s = gb::kLightBufferSampler;
        d->SetTexture(s, static_cast<IDirect3DBaseTexture9*>(texture));
        d->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        d->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        d->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
        d->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        d->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
        g_lightBound = true;
        return 1;
    }

    void FillStatus(WXL_GfxStatus* out)
    {
        if (!out) return;
        out->caps          = scene::Caps();
        out->frameIndex    = g_frameIndex;
        out->lastRequested = g_lastRequested;
        out->lastAvailable = g_lastAvailable;
        out->passCount     = static_cast<uint32_t>(g_passes.size() + g_pending.size());
        out->depthStatus   = scene::Status();
    }

    bool WorldSize(uint32_t& width, uint32_t& height)
    {
        if (!g_worldW || !g_worldH) return false;
        width  = g_worldW;
        height = g_worldH;
        return true;
    }

    size_t PassCount() { return g_passes.size(); }

    bool GetPassInfo(size_t index, PassInfo& out)
    {
        if (index >= g_passes.size()) return false;
        const Pass& p = *g_passes[index];
        out.name      = p.name.c_str();
        out.order     = p.order;
        out.lastWants = p.lastWants;
        out.ran       = p.ran;
        out.gpuMs     = g_cfg.profile && p.span >= 0 ? g_timer.Milliseconds(p.span) : -1.0f;
        return true;
    }

    bool ProfilingEnabled() { return g_cfg.profile; }

    float TotalGpuMs() { return g_cfg.profile ? g_timer.Milliseconds() : -1.0f; }
}
