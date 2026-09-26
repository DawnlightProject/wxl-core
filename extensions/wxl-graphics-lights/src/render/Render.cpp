// wxl-graphics-lights: the render passes and their order. The compute pass "lights" (order
// WXL_GFX_ORDER_LIGHTING, after the shadow service's 90) records the field, its halo and the surfaces;
// the D3D9 pass "lights.composite" (same order) expands the scene and adds their light; the D3D9 pass
// "lights.resolve" (WXL_GFX_ORDER_RESOLVE) maps the HDR scene to the screen.
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

#include "Render.hpp"
#include "Field.hpp"
#include "Hdr.hpp"
#include "Surface.hpp"
#include "../core/Extension.hpp"
#include "../gpu/Gpu.hpp"
#include "../lights/Cookies.hpp"
#include "../lights/Lights.hpp"
#include "../lights/Rooms.hpp"
#include "../lights/Sky.hpp"

#include "wxl/SceneLightsApi.h"

#include "wxl/gfx/Matrix.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    namespace gl  = wxl::gfx::lights;
    namespace hd  = wxl::gfx::lights::hdr;
    namespace sf  = wxl::gfx::lights::surface;
    namespace fd  = wxl::gfx::lights::field;
    namespace gpu = wxl::gfx::lights::gpu;
    namespace mx  = wxl::gfx::matrix;

    int      g_enabled = 1;
    bool     g_registered = false;
    uint32_t g_computePass = 0, g_compositePass = 0, g_resolvePass = 0;
    bool     g_noDxvkSaid = false;
    bool     g_fieldThisFrame = false;   // decided in the wants, recorded in the block
    uint32_t g_recordedFrame = ~0u;
    char     g_status[256] = "render: waiting for wxl-graphics-extend";

    // --- the engine's own point lights -------------------------------------------------------------------
    // The engine lights terrain (the first 3 lights of each chunk), map-object groups and models with its
    // own model lights, per vertex: a second, cruder pool under the one the surfaces add here, cut at
    // chunk edges wherever a light is not among a chunk's first three. While the surfaces are lit here,
    // the core's scene-lights service is told to let no point light reach the world (its OFF policy; the
    // sun, the moon, ambient and directional light are untouched, and so is anything drawn outside the
    // world pass). The policy found is put back when the surfaces stop.
    int  g_engineLights = 0;        // WXL_GFX_LIGHTS_ENGINE_LIGHTS: 1 leaves the engine's own pools on
    bool g_policyOwned = false;
    int  g_policyBefore = WXL_LIGHT_POLICY_INFLUENCE;

    void FollowEnginePolicy(bool surfacesLit)
    {
        const WXL_SceneLightsApi* sl = gl::Lookup<WXL_SceneLightsApi>("wxl.scenelights", WXL_SCENELIGHTS_API_VERSION);
        if (!sl || sl->structSize < sizeof(WXL_SceneLightsApi) || !sl->SetPolicy || !sl->Policy) return;
        const bool want = surfacesLit && !g_engineLights;
        if (want && !g_policyOwned)
        {
            g_policyBefore = sl->Policy();
            sl->SetPolicy(WXL_LIGHT_POLICY_OFF);
            g_policyOwned = true;
            LIGHTS_LOG_INFO("render: the engine's own point lights are off while the surfaces are lit here (WXL_GFX_LIGHTS_ENGINE_LIGHTS=1 keeps them)");
        }
        else if (!want && g_policyOwned)
        {
            sl->SetPolicy(g_policyBefore);
            g_policyOwned = false;
            LIGHTS_LOG_INFO("render: the engine's own point lights are back (policy %d)", g_policyBefore);
        }
    }

    bool VkReady()
    {
        const WXL_GfxVulkanApi* vk = gl::Vk();
        if (!vk || !vk->Available())
        {
            if (vk && !g_noDxvkSaid)
            {
                g_noDxvkSaid = true;
                LIGHTS_LOG_INFO("render: DXVK not found on this device; the surfaces and the field stay inert (the HDR chain still runs)");
            }
            return false;
        }
        return gpu::FollowDevice(vk) && gpu::EnsureNeutral(vk);
    }

    // --- constants shared by every compute pass ----------------------------------------------------------

    void FillFrame(const WXL_GfxFrame& f, gpu::Constants& c, bool lightsReady, bool mono, bool masks, bool halo,
                   const WXL_GfxShadowFrame* shadow)
    {
        using gpu::Set;
        const WXL_GfxView& v = f.view;
        // The positions of the published textures are relative to the eye the list was gathered around.
        const float* eye = gl::PublishedEye();
        Set(c.eye, eye[0], eye[1], eye[2], float(f.frameIndex % 1024u));
        // The scheduler's matrices take (world - its eye); the list was published around the camera read in
        // the same begin phase, so the two agree. A difference would only offset by (eye - v.eye).
        float shift[16];
        mx::Translation(eye[0] - v.eye[0], eye[1] - v.eye[1], eye[2] - v.eye[2], shift);
        float vp[16], inv[16];
        mx::Mul4(shift, v.viewProjRel, vp);
        if (!mx::Invert4(vp, inv)) mx::Copy(v.invViewProjRel, inv);
        mx::Columns(vp, c.viewProj);
        mx::Columns(inv, c.invViewProj);
        for (int i = 0; i < 3; ++i) Set(c.viewRows[i], v.viewRel[i * 4 + 0], v.viewRel[i * 4 + 1], v.viewRel[i * 4 + 2], 0.0f);
        Set(c.depthRange, f.depthRange[0], 1.0f / std::max(f.depthRange[1] - f.depthRange[0], 1e-6f), f.depthRange[1], 0.0f);
        Set(c.screen, float(f.width), float(f.height), 1.0f / float(std::max(f.width, 1u)), 1.0f / float(std::max(f.height, 1u)));
        const float p5 = std::fabs(v.projection[5]) > 1e-6f ? std::fabs(v.projection[5]) : 1.0f;
        Set(c.proj, v.projection[0], v.projection[5], 2.0f / (float(std::max(f.height, 1u)) * p5), 0.0f);

        int count = 0;
        gl::Current(count);
        Set(c.lights, lightsReady ? float(count) : 0.0f, mono ? 1.0f : 0.0f, masks ? 1.0f : 0.0f, halo ? 1.0f : 0.0f);
        gl::ClusterConstants(c.clusterC, c.clusterD);
        gl::cookies::Constants(c.cookieC, c.cookieD, c.cookieE);
        c.cookieE[1] = float(gl::cookies::FaceTexels());
        if (!lightsReady)
        {
            // No list this frame: every cluster reads empty.
            Set(c.clusterC, 1.0f, 1.0f, 1.0f, 1.0f);
            c.cookieC[2] = 0.0f;
        }

        // Each light's shadow slot this frame, by list index.
        for (int i = 0; i < LIGHTS_MAX_LIGHTS / 4; ++i) Set(c.slots[i], -1.0f, -1.0f, -1.0f, -1.0f);
        if (shadow)
            for (uint32_t s = 0; s < std::min<uint32_t>(shadow->slotCount, WXL_GFX_SHADOW_SLOTS); ++s)
            {
                const WXL_GfxShadowSlot& slot = shadow->slots[s];
                if (!slot.lightId || slot.listIndex < 0 || slot.listIndex >= LIGHTS_MAX_LIGHTS) continue;
                c.slots[slot.listIndex >> 2][slot.listIndex & 3] = float(s);
            }

        const int rooms = std::min(gl::rooms::Count(), LIGHTS_MAX_ROOMS);
        if (rooms > 0)
        {
            std::memcpy(c.rooms, gl::rooms::Rows(), sizeof(float) * 12 * size_t(rooms));
            std::memcpy(c.roomWeights, gl::rooms::Weights(), sizeof(float) * size_t(rooms));
        }
        Set(c.roomInfo, float(rooms), gl::RoomCross(), 0.12f, 0.2f);

        // The engine's own sun or moon term, in its gamma space, and the shadow service's bodies.
        float dif[3], amb[3], to[3];
        gl::sky::EngineColours(dif, amb, to);
        const float lumD = 0.2126f * dif[0] + 0.7152f * dif[1] + 0.0722f * dif[2];
        const float lumA = 0.2126f * amb[0] + 0.7152f * amb[1] + 0.0722f * amb[2];
        const bool sunOn = sf::Config().sun && shadow && (shadow->flags & WXL_GFX_SHADOW_FRAME_MASKS) && gl::sky::Current().valid;
        Set(c.sunDir, to[0], to[1], to[2], sunOn ? 1.0f : 0.0f);
        Set(c.sunColors, lumD, lumA, std::clamp(sf::Config().deepen, 0.0f, 1.0f), 0.0f);
        Set(c.sunWeights, shadow ? shadow->sunWeight : 0.0f, shadow ? shadow->moonWeight : 0.0f, 0.0f, 0.0f);
    }

    // --- the compute pass ------------------------------------------------------------------------------------

    uint32_t __cdecl WantsCompute(void*)
    {
        try
        {
            g_fieldThisFrame = false;
            const bool surfaces = g_enabled != 0;
            const bool field = fd::Wanted();
            const bool ready = (surfaces || field) && VkReady();
            FollowEnginePolicy(surfaces && ready);
            if (!ready) return 0;
            gl::Want();
            gpu::EnsurePipelines(gl::Vk());
            if (const WXL_GraphicsShadowApi* shadow = gl::Shadow())
                shadow->Want((surfaces ? WXL_GFX_SHADOW_WANT_MASKS : 0u) | (field ? WXL_GFX_SHADOW_WANT_POINTS : 0u));
            g_fieldThisFrame = field;
            return (surfaces ? WXL_GFX_NEED_DEPTH | WXL_GFX_NEED_NORMALS | WXL_GFX_NEED_ALBEDO : 0u) | WXL_GFX_NEED_RUN;
        }
        catch (...)
        {
            return 0;
        }
    }

    bool RecordImpl(const WXL_GfxVkFrame& vk)
    {
        const WXL_GfxVulkanApi* api = gl::Vk();
        if (!api || !vk.cmd || !vk.frame) return false;
        const WXL_GfxFrame& f = *vk.frame;
        if (!gpu::FollowDevice(api) || !gpu::EnsureNeutral(api)) return false;

        gpu::FrameBindings& fb = gpu::Frame();
        fb.api = api;
        fb.cmd = vk.cmd;
        fb.samplers[0] = api->Sampler(WXL_GFX_VK_SAMPLER_POINT_CLAMP);
        fb.samplers[1] = api->Sampler(WXL_GFX_VK_SAMPLER_LINEAR_CLAMP);
        static WXL_GfxVkImage blue{};
        blue = WXL_GfxVkImage{};
        api->SharedTexture(WXL_GFX_TEX_BLUE_NOISE, &blue);
        fb.noise = blue.image != VK_NULL_HANDLE ? &blue : nullptr;
        if (fb.samplers[0] == VK_NULL_HANDLE || fb.samplers[1] == VK_NULL_HANDLE) return false;

        // The light service's textures of this frame.
        int count = 0;
        uint32_t published = 0;
        gl::CurrentAbi(count, published);
        WXL_GfxVkImage sources{}, clusters{}, cookies{};
        void* st = gl::SourceTexture();
        void* ct = gl::ClusterTexture();
        void* ck = gl::cookies::Atlas();
        const bool lightsReady = published == f.frameIndex && count > 0 && st && ct && ck && api->ImportTexture(st, &sources)
                              && api->ImportTexture(ct, &clusters) && api->ImportTexture(ck, &cookies) && sources.image
                              && clusters.image && cookies.image;
        const bool mono = lightsReady && cookies.format == VK_FORMAT_R8_UNORM;

        // The shadow service's frame: the masks for the surfaces, the slots for every light.
        WXL_GfxShadowFrame shadow{};
        shadow.structSize = sizeof shadow;
        const WXL_GraphicsShadowApi* shadowApi = gl::Shadow();
        const bool haveShadow = shadowApi && shadowApi->GetFrame(&shadow) && shadow.frameIndex == f.frameIndex;
        const bool masks = haveShadow && (shadow.flags & WXL_GFX_SHADOW_FRAME_MASKS) && shadow.masks.image != VK_NULL_HANDLE;

        const bool surfaces = g_enabled != 0;
        const bool field = g_fieldThisFrame;
        const bool halo = field && fd::HaloWanted();
        const bool surfacesReady = surfaces && sf::Ensure(api, f);
        const bool fieldReady = field && fd::Ensure(api);

        gpu::BeginTimers(vk.cmd, vk.slot);
        gpu::ClearFresh(api, vk.cmd);

        // The fog's thinning of lamp light, eased once a frame here so the surfaces and the field read
        // the same value.
        gl::FollowFogExtinction(f.deltaTime);

        static gpu::Constants c;
        std::memset(&c, 0, sizeof c);
        FillFrame(f, c, lightsReady, mono, masks, halo, haveShadow ? &shadow : nullptr);
        sf::FillConstants(c);
        fd::FillConstants(c);
        if (!api->AllocUniform(sizeof c, &fb.constants) || !fb.constants.mapped)
        {
            gpu::EndTimers();
            return false;
        }
        std::memcpy(fb.constants.mapped, &c, sizeof c);

        const WXL_GfxVkImage* src = lightsReady ? &sources : nullptr;
        const WXL_GfxVkImage* cls = lightsReady ? &clusters : nullptr;
        const WXL_GfxVkImage* cok = lightsReady ? &cookies : nullptr;
        bool fieldDone = false;
        if (fieldReady) fieldDone = fd::Record(vk, src, cls, cok, halo);
        bool surfaceDone = false;
        if (surfacesReady)
            surfaceDone = sf::Record(vk, src, cls, cok, masks ? &shadow.masks : nullptr, fieldDone ? fd::Halo() : nullptr,
                                     fieldDone ? fd::Inscatter() : nullptr);
        gpu::EndTimers();

        std::snprintf(g_status, sizeof g_status, "render: frame %u, lights %d%s, shadows %s, field %s, surfaces %s | compute %.3f ms",
                      f.frameIndex, lightsReady ? count : 0, lightsReady ? "" : " (no list this frame)",
                      haveShadow ? (masks ? "masks" : "points") : "absent (lit)", fieldDone ? (halo ? "with halo" : "on") : "off",
                      surfaceDone ? "lit" : "off", std::max(gpu::SpanMs(-1), 0.0f));
        return surfaceDone || fieldDone;
    }

    void __cdecl RecordCompute(void*, const WXL_GfxVkFrame* vk)
    {
        fd::Invalidate();
        bool ok = false;
        try
        {
            if (vk) ok = RecordImpl(*vk);
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; LIGHTS_LOG_ERROR("render: exception while recording; the frame is skipped"); }
            ok = false;
        }
        if (!ok) gpu::EndTimers();
        g_recordedFrame = ok && vk && vk->frame ? vk->frame->frameIndex : ~0u;
    }

    // --- the D3D9 passes -----------------------------------------------------------------------------------

    uint32_t __cdecl WantsComposite(void*)
    {
        try
        {
            if (!g_enabled) return 0;
            gl::Want();
            uint32_t w = WXL_GFX_NEED_DEPTH | WXL_GFX_NEED_NORMALS | WXL_GFX_NEED_ALBEDO | WXL_GFX_NEED_RUN;
            if (hd::Config().hdr) w |= WXL_GFX_NEED_HDR;
            return w;
        }
        catch (...)
        {
            return 0;
        }
    }

    void __cdecl DrawComposite(void*, const WXL_GfxFrame* frame)
    {
        try
        {
            if (!frame) return;
            const bool hdr = (frame->available & WXL_GFX_NEED_HDR) && frame->sceneColorTexture;
            void* light = sf::LightTexture(frame->frameIndex);
            // Without HDR and without light the composite is the identity: nothing to draw.
            if (!hdr && !light) return;
            hd::Composite(*frame, light, light && sf::Config().view != LIGHTS_VIEW_NONE);
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; LIGHTS_LOG_ERROR("render: exception while compositing; the frame is skipped"); }
        }
    }

    uint32_t __cdecl WantsResolve(void*)
    {
        return g_enabled && hd::Config().hdr && !hd::Claimed() ? WXL_GFX_NEED_RUN : 0u;
    }

    void __cdecl DrawResolve(void*, const WXL_GfxFrame* frame)
    {
        try
        {
            if (frame) hd::Resolve(*frame, sf::Config().view != LIGHTS_VIEW_NONE && sf::LightTexture(frame->frameIndex));
        }
        catch (...)
        {
            static bool said = false;
            if (!said) { said = true; LIGHTS_LOG_ERROR("render: exception while resolving; the frame is skipped"); }
        }
    }
}

namespace wxl::gfx::lights::render
{
    int& Enabled() { return g_enabled; }

    bool EngineLightsOff() { return g_policyOwned; }

    void Install()
    {
        g_enabled = ConfigBool("WXL_GFX_LIGHTS_SURFACE", true) ? 1 : 0;
        g_engineLights = ConfigBool("WXL_GFX_LIGHTS_ENGINE_LIGHTS", false) ? 1 : 0;
        hd::Install();
        sf::Install();
        fd::Install();
    }

    void Register()
    {
        if (g_registered) return;
        const WXL_GraphicsExtendApi* gfx = Gfx();
        const WXL_GfxVulkanApi* vk = Vk();
        if (!gfx || !vk) return;
        g_registered = true;

        WXL_GfxVkPassDesc compute{};
        compute.structSize = sizeof compute;
        compute.name = "lights";
        compute.order = WXL_GFX_ORDER_LIGHTING;
        compute.wants = &WantsCompute;
        compute.record = &RecordCompute;
        g_computePass = vk->AddComputePass(&compute);

        WXL_GfxPassDesc composite{};
        composite.structSize = sizeof composite;
        composite.name = "lights.composite";
        composite.order = WXL_GFX_ORDER_LIGHTING;
        composite.wants = &WantsComposite;
        composite.draw = &DrawComposite;
        g_compositePass = gfx->AddPass(&composite);

        WXL_GfxPassDesc resolve{};
        resolve.structSize = sizeof resolve;
        resolve.name = "lights.resolve";
        resolve.order = WXL_GFX_ORDER_RESOLVE;
        resolve.wants = &WantsResolve;
        resolve.draw = &DrawResolve;
        g_resolvePass = gfx->AddPass(&resolve);

        if (!g_computePass || !g_compositePass || !g_resolvePass)
            LIGHTS_LOG_ERROR("render: wxl-graphics-extend refused a pass (compute %u, composite %u, resolve %u)", g_computePass,
                             g_compositePass, g_resolvePass);
        else
            LIGHTS_LOG_INFO("render: passes registered: compute \"lights\" and D3D9 \"lights.composite\" at %d, \"lights.resolve\" at %d",
                            WXL_GFX_ORDER_LIGHTING, WXL_GFX_ORDER_RESOLVE);
    }

    void OnDeviceLost()
    {
        hd::OnDeviceLost();
        sf::OnDeviceLost();
        g_recordedFrame = ~0u;
    }

    const char* Status() { return g_status; }
}
