// wxl-graphics-lights: the surfaces, recorded and copied for the composite.
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

#include "Surface.hpp"
#include "../core/Extension.hpp"
#include "../lights/Lights.hpp"

#include "wxl/gfx/Ui.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cstdio>

namespace
{
    namespace sf  = wxl::gfx::lights::surface;
    namespace gpu = wxl::gfx::lights::gpu;
    namespace gl  = wxl::gfx::lights;
    namespace ui  = wxl::gfx::ui;
    namespace vkh = wxl::gfx::vk;

    sf::Settings g_cfg;

    WXL_GfxVkImage     g_light{};              // the pass's output, full resolution
    uint32_t           g_w = 0, g_h = 0;
    void*              g_d3dDevice = nullptr;
    IDirect3DTexture9* g_d3dLight = nullptr;   // its D3D9 copy, for the composite
    uint32_t           g_d3dW = 0, g_d3dH = 0;
    uint32_t           g_frame = ~0u;          // the frame the copy holds
    bool               g_warned = false;
    char               g_status[200] = "surfaces: not run yet";

    void FreeD3d()
    {
        if (g_d3dLight) { g_d3dLight->Release(); g_d3dLight = nullptr; }
        g_d3dW = g_d3dH = 0;
        g_frame = ~0u;
    }

    bool EnsureD3d(void* device, uint32_t w, uint32_t h)
    {
        auto* d = static_cast<IDirect3DDevice9*>(device);
        if (!d) return false;
        if (g_d3dDevice != device) { FreeD3d(); g_d3dDevice = device; }
        if (g_d3dLight && g_d3dW == w && g_d3dH == h) return true;
        FreeD3d();
        if (FAILED(d->CreateTexture(w, h, 1, 0, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_d3dLight, nullptr)) || !g_d3dLight)
        {
            g_d3dLight = nullptr;
            if (!g_warned)
            {
                g_warned = true;
                LIGHTS_LOG_WARN("surfaces: the D3D9 light texture (%ux%u) was refused; nothing is composited", w, h);
            }
            return false;
        }
        g_d3dW = w;
        g_d3dH = h;
        return true;
    }

    bool EnsureImage(const WXL_GfxVulkanApi* api, uint32_t w, uint32_t h)
    {
        if (g_light.image && g_w == w && g_h == h) return true;
        gpu::DropImage(api, g_light);
        if (!gpu::MakeImage(api, g_light, "lights.surface", VK_IMAGE_TYPE_2D, VK_FORMAT_R16G16B16A16_SFLOAT, w, h, 1)) return false;
        g_w = w;
        g_h = h;
        LIGHTS_LOG_INFO("surfaces: %ux%u", w, h);
        return true;
    }

    bool IsoCheck(const char* label, uint32_t bit, const char* help)
    {
        int on = (g_cfg.isolate & bit) ? 1 : 0;
        if (!ui::Check(label, &on, help)) return false;
        g_cfg.isolate = on ? (g_cfg.isolate | bit) : (g_cfg.isolate & ~bit);
        return true;
    }
}

namespace wxl::gfx::lights::surface
{
    Settings& Config() { return g_cfg; }

    void Install()
    {
        Settings& s = g_cfg;
        s.burley = ConfigBool("WXL_GFX_LIGHTS_SURFACE_BURLEY", s.burley != 0) ? 1 : 0;
        s.specular = ConfigFloat("WXL_GFX_LIGHTS_SURFACE_SPECULAR", s.specular, 0.0f, 4.0f);
        s.wetness = ConfigFloat("WXL_GFX_LIGHTS_SURFACE_WETNESS", s.wetness, 0.0f, 1.0f);
        s.roughness = ConfigFloat("WXL_GFX_LIGHTS_SURFACE_ROUGHNESS", s.roughness, 0.3f, 1.5f);
        s.emissive = ConfigFloat("WXL_GFX_LIGHTS_SURFACE_EMISSIVE", s.emissive, 0.0f, 4.0f);
        s.fogDims = ConfigFloat("WXL_GFX_LIGHTS_SURFACE_FOG_DIMS", s.fogDims, 0.0f, 4.0f);
        s.outdoorIndoors = ConfigFloat("WXL_GFX_LIGHTS_SURFACE_OUTDOOR_INDOORS", s.outdoorIndoors, 0.0f, 1.0f);
        s.indoorOutdoors = ConfigFloat("WXL_GFX_LIGHTS_SURFACE_INDOOR_OUTDOORS", s.indoorOutdoors, 0.0f, 1.0f);
        s.sun = ConfigBool("WXL_GFX_LIGHTS_SURFACE_SUN", s.sun != 0) ? 1 : 0;
        s.deepen = ConfigFloat("WXL_GFX_LIGHTS_SURFACE_SUN_DEEPEN", s.deepen, 0.0f, 1.0f);
        s.prefilter = ConfigFloat("WXL_GFX_LIGHTS_COOKIES_PREFILTER", s.prefilter, 0.5f, 16.0f);
        s.view = ConfigInt("WXL_GFX_LIGHTS_VIEW", s.view, 0, LIGHTS_VIEW_COUNT - 1);
        LIGHTS_LOG_INFO("surfaces: %s diffuse, specular %.2f, wetness %.2f, emissive %.2f, fog dims %.2f, sun factor %d (deepen %.2f)",
                        s.burley ? "Burley" : "Lambert", s.specular, s.wetness, s.emissive, s.fogDims, s.sun, s.deepen);
    }

    void FillConstants(gpu::Constants& c)
    {
        using gpu::Set;
        Set(c.shade, g_cfg.burley ? 1.0f : 0.0f, std::max(g_cfg.specular, 0.0f), std::clamp(g_cfg.wetness, 0.0f, 1.0f),
            FogExtinction() * std::max(g_cfg.fogDims, 0.0f));
        Set(c.shade2, std::max(g_cfg.emissive, 0.0f), std::clamp(g_cfg.roughness, 0.3f, 1.5f), 0.0f, 0.0f);
        c.roomInfo[2] = std::clamp(g_cfg.outdoorIndoors, 0.0f, 1.0f);
        c.roomInfo[3] = std::clamp(g_cfg.indoorOutdoors, 0.0f, 1.0f);
        c.cookieE[2] = std::max(g_cfg.prefilter, 0.1f);
        Set(c.debug, float(g_cfg.view), gpu::Bits(g_cfg.isolate), std::clamp(g_cfg.fieldSlice, 0.0f, 1.0f), 0.0f);
    }

    bool Ensure(const WXL_GfxVulkanApi* api, const WXL_GfxFrame& f)
    {
        return EnsureImage(api, f.width, f.height) && EnsureD3d(f.device, f.width, f.height);
    }

    bool Record(const WXL_GfxVkFrame& vk, const WXL_GfxVkImage* sources, const WXL_GfxVkImage* clusters,
                const WXL_GfxVkImage* cookies, const WXL_GfxVkImage* masks, const WXL_GfxVkImage* halo,
                const WXL_GfxVkImage* field)
    {
        const WXL_GfxVulkanApi* api = Vk();
        const WXL_GfxFrame& f = *vk.frame;
        const size_t albedoEnd = offsetof(WXL_GfxVkFrame, albedo) + sizeof(WXL_GfxVkImage);
        const bool haveAlbedo = vk.structSize >= albedoEnd && vk.albedo.image != VK_NULL_HANDLE;
        if (vk.depth.image == VK_NULL_HANDLE || vk.normals.image == VK_NULL_HANDLE || !haveAlbedo)
        {
            std::snprintf(g_status, sizeof g_status, "surfaces: no G-buffer this frame (depth %d, normals %d, albedo %d): "
                          "extShadowQuality >= 1, no multisampling, three render targets, the rewritten shaders",
                          vk.depth.image != VK_NULL_HANDLE, vk.normals.image != VK_NULL_HANDLE, haveAlbedo);
            return false;
        }
        if (!gpu::HasPipeline(LIGHTS_PIPE_SURFACE))
        {
            std::snprintf(g_status, sizeof g_status, "surfaces: the pipeline is not there (see the log)");
            return false;
        }
        if (!g_light.image || g_w != f.width || g_h != f.height || !g_d3dLight) return false;

        gpu::Bind b;
        b.tex[LIGHTS_T_DEPTH] = &vk.depth;
        b.tex[LIGHTS_T_NORMALS] = &vk.normals;
        b.tex[LIGHTS_T_ALBEDO] = &vk.albedo;
        b.tex[LIGHTS_T_SOURCES] = sources;
        b.tex[LIGHTS_T_CLUSTERS] = clusters;
        b.tex[LIGHTS_T_COOKIES] = cookies;
        b.tex[LIGHTS_T_MASKS] = masks;
        b.tex[LIGHTS_T_FIELD] = field;
        b.tex[LIGHTS_T_HALO] = halo;
        b.out[0] = &g_light;
        gpu::PushData push{};
        const uint32_t volumes = (1u << LIGHTS_T_MASKS) | (1u << LIGHTS_T_FIELD) | (1u << LIGHTS_T_HALO);
        if (!gpu::Dispatch(LIGHTS_PIPE_SURFACE, b, push, vkh::Groups(f.width, 8), vkh::Groups(f.height, 8), 1, volumes)) return false;
        gpu::MarkTimer(vk.cmd, gpu::kSpanSurface);
        const bool copied = api->CopyToTexture(&g_light, g_d3dLight) != 0;
        gpu::MarkTimer(vk.cmd, gpu::kSpanCopy);
        if (!copied)
        {
            std::snprintf(g_status, sizeof g_status, "surfaces: the copy into the D3D9 texture was refused");
            return false;
        }
        g_frame = f.frameIndex;
        const float ms = gpu::SpanMs(gpu::kSpanSurface), copy = gpu::SpanMs(gpu::kSpanCopy);
        std::snprintf(g_status, sizeof g_status, "surfaces: %ux%u, shadow masks %s, halo %s | GPU %.3f ms + copy %.3f ms", f.width, f.height,
                      masks ? "on" : "off", halo ? "on" : "off", std::max(ms, 0.0f), std::max(copy, 0.0f));
        return true;
    }

    void* LightTexture(uint32_t frameIndex) { return g_frame == frameIndex ? g_d3dLight : nullptr; }

    void OnDeviceLost() { FreeD3d(); }

    const char* Status() { return g_status; }

    void Panel()
    {
        Settings& s = g_cfg;
        ui::Text(g_status);
        ui::Separator();
        ui::Check("Burley diffuse", &s.burley,
                  "Rough surfaces scatter lamp light a little more at grazing angles and less at the rim, as dirt, cloth and stone do. Off, plain Lambert.");
        ui::Slider("Specular", &s.specular, 0.0f, 3.0f,
                   "The glossy highlight lamps leave on surfaces (GGX). Terrain with a specular mask, stone and wet ground shine the most.");
        ui::Slider("Roughness", &s.roughness, 0.3f, 1.5f,
                   "Every surface's roughness, times this. Lower gives tighter, brighter highlights; higher spreads them.");
        ui::Slider("Wet ground", &s.wetness, 0.0f, 1.0f,
                   "Upward-facing outdoor surfaces turn glossy and a little darker, so lamps leave long reflections on the ground as after rain.");
        ui::Slider("Lamp heads glow", &s.emissive, 0.0f, 3.0f,
                   "How brightly the glass and flame of a lamp glow, whatever way they face. Bloom will pick this up later.");
        ui::Slider("Fog dims lamps", &s.fogDims, 0.0f, 3.0f,
                   "How much the fog around the camera thins a lamp's light on its way to a surface: in thick fog a lamp lights less and glows more. 0 ignores the fog.");
        ui::Slider("Outdoor light indoors", &s.outdoorIndoors, 0.0f, 1.0f,
                   "How much of a street lamp reaches inside a building (through windows and doors). Walls are not always drawn, so without this lamps light interiors through them.");
        ui::Slider("Indoor light outdoors", &s.indoorOutdoors, 0.0f, 1.0f,
                   "How much of a room's lamp reaches outside every room: the spill through doorways and windows.");
        ui::Separator();
        ui::Check("Sun and moon shadows", &s.sun,
                  "The shadow the game's own sun and moon light lacks (terrain, contact, deeper cascades), from wxl-graphics-shadow. The game's sun is never added twice.");
        ui::Slider("Deepen engine shadows", &s.deepen, 0.0f, 1.0f,
                   "The game's cascade shadows only take part of the sun away. This removes the rest of it in them, as a real shadow would.");
        ui::Slider("Cookie prefilter", &s.prefilter, 0.5f, 12.0f,
                   "How many of a cookie's texels one pixel may cover, seen from its lamp, before the pattern gives way to its average. Lower is calmer on distant walls; higher keeps bars sharper further away.");
    }

    void DebugPanel()
    {
        Settings& s = g_cfg;
        static const char* const kViews[] = { "Off", "Albedo", "Normals", "Material", "Light only", "Clusters", "Shadow slots",
                                              "Sun factor", "Cookies", "Rooms", "Field slice", "Field along the ray" };
        ui::Combo("View", &s.view, kViews, LIGHTS_VIEW_COUNT,
                  "Albedo: the colour the game's materials wrote (magenta: a normal but no albedo). Normals: world normals. Material: red models, green buildings, blue terrain, brighter where glossy. Light only: the lamps on white surfaces. Clusters: lamps per cluster, blue few, red many. Shadow slots: where each shadowed lamp is blocked. Sun factor: white where the game's sun is kept, darker where a shadow is added. Cookies: the lamps' patterns. Rooms: one colour per room. Field slice: the air's lamp light at one depth. Field along the ray: the halo each pixel would get.");
        if (s.view == LIGHTS_VIEW_FIELD)
            ui::Slider("Field slice depth", &s.fieldSlice, 0.0f, 1.0f, "Which depth of the field the view shows: 0 at the lens, 1 at 250 yards (exponential).");
        ui::Text("Isolates: each removes one ingredient, to find which one causes an artefact.");
        IsoCheck("No cookies", LIGHTS_ISO_NO_COOKIES, "Lamps light evenly, without their cage patterns and tinted glass.");
        IsoCheck("No cookie prefilter", LIGHTS_ISO_NO_PREFILTER, "Cookie patterns stay sharp at any distance (and may shimmer).");
        IsoCheck("No profiles", LIGHTS_ISO_NO_PROFILE, "Lamps shine the same in every direction, without their family's shape.");
        IsoCheck("No shadows", LIGHTS_ISO_NO_SHADOWS, "Lamps light through everything the shadow service would block.");
        IsoCheck("No specular", LIGHTS_ISO_NO_SPECULAR, "No glossy highlights.");
        IsoCheck("No room gate", LIGHTS_ISO_NO_ROOMS, "Lamps reach every room and floor in their reach, indoors and out.");
        IsoCheck("No fog dimming", LIGHTS_ISO_NO_FOG, "The fog does not thin lamp light on its way to surfaces or through the air.");
        IsoCheck("No sun factor", LIGHTS_ISO_NO_SUN, "The scene keeps the game's own sun shading only.");
        IsoCheck("No lamp heads", LIGHTS_ISO_NO_EMISSIVE, "The glass and flames of lamps do not glow.");
        IsoCheck("White albedo", LIGHTS_ISO_WHITE, "Every surface takes lamp light as if it were white.");
    }
}
