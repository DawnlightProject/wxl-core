// wxl-graphics-lights: the lamp light field for the air and its halo.
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

#include "Field.hpp"
#include "../core/Extension.hpp"
#include "../lights/Lights.hpp"

#include "wxl/GraphicsLightsFieldApi.h"
#include "wxl/gfx/Ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
    namespace fd  = wxl::gfx::lights::field;
    namespace gpu = wxl::gfx::lights::gpu;
    namespace gl  = wxl::gfx::lights;
    namespace ui  = wxl::gfx::ui;
    namespace vkh = wxl::gfx::vk;

    constexpr uint32_t kW = LIGHTS_FIELD_W, kH = LIGHTS_FIELD_H, kD = LIGHTS_FIELD_D;
    static_assert(kW % WXL_GFX_CLUSTERS_X == 0 && kH % WXL_GFX_CLUSTERS_Y == 0 && kD % WXL_GFX_CLUSTERS_Z == 0,
                  "the field subdivides the clusters exactly");

    fd::Settings g_cfg;
    WXL_GfxVkImage g_scatter{}, g_ambient{}, g_direction{}, g_halo{};
    bool     g_made = false;
    bool     g_valid = false;      // the images hold this frame's field
    bool     g_haloValid = false;
    uint32_t g_frame = 0;
    float    g_eye[3] = {};
    float    g_viewProj[16] = {};
    float    g_usedPhase[3] = {};
    bool     g_shadowed = false;

    // Demand: consumers stamp the poll they were seen in.
    uint32_t g_polls = 0, g_wantedAt = 0;
    bool     g_wantedEver = false;

    char g_status[200] = "field: not computed yet";

    // --- the C ABI ---------------------------------------------------------------------------------------

    void __cdecl ApiWant(void)
    {
        g_wantedAt = g_polls;
        g_wantedEver = true;
        gl::Want();
    }

    int __cdecl ApiGet(WXL_GfxLightsField* out)
    {
        if (!out || !g_valid) return 0;
        WXL_GfxLightsField f{};
        f.structSize = sizeof f;
        f.frameIndex = g_frame;
        f.width = kW;
        f.height = kH;
        f.depth = kD;
        f.nearDistance = WXL_GFX_CLUSTER_NEAR;
        f.farDistance = WXL_GFX_CLUSTER_FAR;
        std::memcpy(f.eye, g_eye, sizeof f.eye);
        std::memcpy(f.viewProjRel, g_viewProj, sizeof f.viewProjRel);
        f.phase[0] = g_usedPhase[0];
        f.phase[1] = g_usedPhase[1];
        f.phase[2] = g_usedPhase[2];
        f.inscatter = g_scatter;
        f.ambient = g_ambient;
        f.direction = g_direction;
        const uint32_t size = std::min<uint32_t>(out->structSize, sizeof f);
        if (size < sizeof(uint32_t)) return 0;
        f.structSize = size;
        std::memcpy(out, &f, size);
        return 1;
    }

    void __cdecl ApiSetPhase(float gForward, float gBack, float blend)
    {
        g_cfg.phase[0] = std::clamp(gForward, -0.95f, 0.95f);
        g_cfg.phase[1] = std::clamp(gBack, -0.95f, 0.95f);
        g_cfg.phase[2] = std::clamp(blend, 0.0f, 1.0f);
    }

    const char* __cdecl ApiStatus(void) { return g_status; }

    const WXL_GraphicsLightsFieldApi kApi = {
        sizeof(WXL_GraphicsLightsFieldApi),
        WXL_GRAPHICS_LIGHTS_FIELD_API_VERSION,
        &ApiWant,
        &ApiGet,
        &ApiSetPhase,
        &ApiStatus,
    };
}

namespace wxl::gfx::lights::field
{
    Settings& Config() { return g_cfg; }

    void Install()
    {
        Settings& s = g_cfg;
        s.enabled = ConfigBool("WXL_GFX_LIGHTS_FIELD", s.enabled != 0) ? 1 : 0;
        s.shadows = ConfigBool("WXL_GFX_LIGHTS_FIELD_SHADOWS", s.shadows != 0) ? 1 : 0;
        s.thinning = ConfigFloat("WXL_GFX_LIGHTS_FIELD_THINNING", s.thinning, 0.0f, 4.0f);
        s.halo = ConfigBool("WXL_GFX_LIGHTS_HALO", s.halo != 0) ? 1 : 0;
        s.haloDensity = ConfigFloat("WXL_GFX_LIGHTS_HALO_DENSITY", s.haloDensity, 0.0f, 0.2f);
        s.haloStrength = ConfigFloat("WXL_GFX_LIGHTS_HALO_STRENGTH", s.haloStrength, 0.0f, 8.0f);
        s.phase[0] = ConfigFloat("WXL_GFX_LIGHTS_FIELD_PHASE_FORWARD", s.phase[0], -0.95f, 0.95f);
        s.phase[1] = ConfigFloat("WXL_GFX_LIGHTS_FIELD_PHASE_BACK", s.phase[1], -0.95f, 0.95f);
        s.phase[2] = ConfigFloat("WXL_GFX_LIGHTS_FIELD_PHASE_BLEND", s.phase[2], 0.0f, 1.0f);
        g_api->PublishInterface(WXL_GRAPHICS_LIGHTS_FIELD_API_NAME, WXL_GRAPHICS_LIGHTS_FIELD_API_VERSION,
                                const_cast<WXL_GraphicsLightsFieldApi*>(&kApi));
        LIGHTS_LOG_INFO("field: %s, %ux%ux%u, point shadows %d, halo without the fog %d (density %.3f)", s.enabled ? "on" : "off",
                        kW, kH, kD, s.shadows, s.halo, s.haloDensity);
    }

    bool Wanted()
    {
        ++g_polls;
        if (!g_cfg.enabled) return false;
        const bool asked = g_wantedEver && g_polls - g_wantedAt <= 1;
        return asked || HaloWanted();
    }

    bool HaloWanted()
    {
        return g_cfg.enabled && g_cfg.halo && g_cfg.haloDensity > 0.0f && g_cfg.haloStrength > 0.0f && !FogActive();
    }

    void FillConstants(gpu::Constants& c)
    {
        using gpu::Set;
        const bool halo = HaloWanted();
        Set(c.fieldC, float(kW), float(kH), float(kD), halo ? g_cfg.haloStrength : 0.0f);
        Set(c.fieldD, WXL_GFX_CLUSTER_NEAR, 1.0f / std::log(WXL_GFX_CLUSTER_FAR / WXL_GFX_CLUSTER_NEAR), WXL_GFX_CLUSTER_FAR,
            std::max(g_cfg.haloDensity, 0.0f));
        Set(c.phase, g_cfg.phase[0], g_cfg.phase[1], g_cfg.phase[2], 0.0f);
        Set(c.medium, FogExtinction() * std::max(g_cfg.thinning, 0.0f), 0.0f, 0.0f, 0.0f);
    }

    bool Ensure(const WXL_GfxVulkanApi* api)
    {
        if (g_made && g_scatter.image && g_ambient.image && g_direction.image && g_halo.image) return true;
        const VkImageType T3 = VK_IMAGE_TYPE_3D;
        const VkFormat F16 = VK_FORMAT_R16G16B16A16_SFLOAT;
        g_made = gpu::MakeImage(api, g_scatter, "lights.field.inscatter", T3, F16, kW, kH, kD)
              && gpu::MakeImage(api, g_ambient, "lights.field.ambient", T3, F16, kW, kH, kD)
              && gpu::MakeImage(api, g_direction, "lights.field.direction", T3, F16, kW, kH, kD)
              && gpu::MakeImage(api, g_halo, "lights.field.halo", T3, F16, kW, kH, kD);
        if (g_made) LIGHTS_LOG_INFO("field: images %ux%ux%u ready", kW, kH, kD);
        return g_made;
    }

    bool Record(const WXL_GfxVkFrame& vk, const WXL_GfxVkImage* sources, const WXL_GfxVkImage* clusters,
                const WXL_GfxVkImage* cookies, bool halo)
    {
        g_valid = false;
        g_haloValid = false;
        const WXL_GfxFrame& f = *vk.frame;
        if (!g_made) return false;
        const bool shadowed = g_cfg.shadows && Shadow() && gpu::HasPipeline(LIGHTS_PIPE_FIELD_SHADOW);
        const uint32_t pipe = shadowed ? LIGHTS_PIPE_FIELD_SHADOW : LIGHTS_PIPE_FIELD;
        if (!gpu::HasPipeline(pipe))
        {
            std::snprintf(g_status, sizeof g_status, "field: the pipeline is not there (see the log)");
            return false;
        }
        gpu::Bind b;
        b.tex[LIGHTS_T_SOURCES] = sources;
        b.tex[LIGHTS_T_CLUSTERS] = clusters;
        b.tex[LIGHTS_T_COOKIES] = cookies;
        b.out[0] = &g_scatter;
        b.out[1] = &g_ambient;
        b.out[2] = &g_direction;
        gpu::PushData push{};
        const uint32_t outVolumes = (1u << 16) | (1u << 17) | (1u << 18) | (1u << 19);
        bool ok = gpu::Dispatch(pipe, b, push, vkh::Groups(kW, 4), vkh::Groups(kH, 4), vkh::Groups(kD, 4), outVolumes);
        // The shadow variant falls back to the plain one when the service could not write its bindings.
        if (!ok && shadowed) ok = gpu::Dispatch(LIGHTS_PIPE_FIELD, b, push, vkh::Groups(kW, 4), vkh::Groups(kH, 4), vkh::Groups(kD, 4), outVolumes);
        gpu::MarkTimer(vk.cmd, gpu::kSpanField);
        if (!ok) return false;
        g_valid = true;
        g_shadowed = shadowed;
        g_frame = f.frameIndex;
        std::memcpy(g_eye, f.view.eye, sizeof g_eye);
        std::memcpy(g_viewProj, f.view.viewProjRel, sizeof g_viewProj);
        std::memcpy(g_usedPhase, g_cfg.phase, sizeof g_usedPhase);

        if (halo && gpu::HasPipeline(LIGHTS_PIPE_HALO))
        {
            gpu::Bind h;
            h.tex[LIGHTS_T_FIELD] = &g_scatter;
            h.out[0] = &g_halo;
            g_haloValid = gpu::Dispatch(LIGHTS_PIPE_HALO, h, push, vkh::Groups(kW, 8), vkh::Groups(kH, 8), 1,
                                        (1u << LIGHTS_T_FIELD) | (1u << 16));
        }
        gpu::MarkTimer(vk.cmd, gpu::kSpanHalo);
        std::snprintf(g_status, sizeof g_status, "field: %ux%ux%u, frame %u, point shadows %s, halo %s | GPU %.3f ms + halo %.3f ms",
                      kW, kH, kD, g_frame, shadowed ? "on" : "off", g_haloValid ? "on" : "off",
                      std::max(gpu::SpanMs(gpu::kSpanField), 0.0f), std::max(gpu::SpanMs(gpu::kSpanHalo), 0.0f));
        return true;
    }

    const WXL_GfxVkImage* Inscatter() { return g_valid ? &g_scatter : nullptr; }
    const WXL_GfxVkImage* Halo() { return g_haloValid ? &g_halo : nullptr; }

    void Invalidate()
    {
        g_valid = false;
        g_haloValid = false;
    }

    const char* Status() { return g_status; }

    void Panel()
    {
        Settings& s = g_cfg;
        ui::Text(g_status);
        ui::Text(FogActive() ? "the fog is active: it reads the field (once adapted); no halo is drawn here"
                             : "the fog is not active: the halo below stands in for it");
        ui::Separator();
        ui::Check("Lamp light in the air", &s.enabled,
                  "Computes the lamps' light in the air around the camera, which the fog multiplies by its own density. Off, the fog gets no lamp light from here and no halo is drawn.");
        ui::Check("Shadows in the air", &s.shadows,
                  "The shadow service's shadows cut the lamps' light in the air too: a passer-by throws a wedge of darkness through the mist. Needs wxl-graphics-shadow.");
        ui::Slider("Mist thins lamps", &s.thinning, 0.0f, 3.0f,
                   "How much the fog around the camera thins a lamp's light on its way through the air: in thick fog a lamp's glow stays close to it.");
        ui::Slider("Forward scattering", &s.phase[0], 0.0f, 0.95f,
                   "How strongly the air throws lamp light onwards, so a lamp between you and the mist glows brighter. The fog sets this itself once adapted.");
        ui::Slider("Back scattering", &s.phase[1], -0.95f, 0.0f,
                   "The second lobe, thrown back towards the lamp: a soft glow when the lamp is behind you.");
        ui::Slider("Back share", &s.phase[2], 0.0f, 1.0f, "How much of the light takes the back lobe.");
        ui::Separator();
        ui::Check("Halo without the fog", &s.halo,
                  "When the fog is off or absent, a thin even haze shows each lamp's glow in the air, with its cookie bars and shadows.");
        ui::Slider("Halo haze density", &s.haloDensity, 0.0f, 0.08f,
                   "How thick that haze is, per yard. Higher gives larger, brighter halos.");
        ui::Slider("Halo strength", &s.haloStrength, 0.0f, 4.0f, "The halo's brightness, times this.");
    }
}
