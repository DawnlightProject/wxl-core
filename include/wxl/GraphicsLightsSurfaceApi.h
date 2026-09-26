// wxl-graphics-lights' surface lighting outputs: what a later Vulkan compute pass of the same block (the
// fog, an effect) may read of this frame's lit surfaces -- the lighting grid's positions and normals,
// the filtered omni shadow and sun visibility, and the lamps' light on surfaces. Published by the
// wxl-graphics-lights extension as
// WXL_Api::GetInterface(WXL_GRAPHICS_LIGHTS_SURFACE_API_NAME, WXL_GRAPHICS_LIGHTS_SURFACE_API_VERSION).
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

#ifndef WXL_GRAPHICS_LIGHTS_SURFACE_API_H
#define WXL_GRAPHICS_LIGHTS_SURFACE_API_H

#include <stdint.h>

#include "wxl/GraphicsVulkanApi.h"

// When. The surface lighting records in the compute block at WXL_GFX_ORDER_LIGHTING (100). A compute pass
// recorded after it in the same block (a higher order: the fog's is WXL_GFX_ORDER_ATMOSPHERE) may read
// these images: they hold this frame's results, in VK_IMAGE_LAYOUT_GENERAL, and the block's barriers
// between passes make them visible. Outside that window, or on a frame the surface lighting did not run,
// Outputs returns 0. Render thread only.
//
// The grid. The images are the lighting grid: half the screen by default (each texel one real pixel of
// its 2 x 2 quad, the nearest depth on even texels and the farthest on odd ones), the full screen when the
// user chose full resolution; `scale` says which. Positions are camera-relative (world - eye).
//
// The same shadows. The omni visibility is the filter shaders/wxl/lights/shadows_vk.hlsli evaluates on the
// service's omni table: a halo read with the same function darkens where the ground does.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GRAPHICS_LIGHTS_SURFACE_API_NAME    "wxl.graphics-lights.surface"
#define WXL_GRAPHICS_LIGHTS_SURFACE_API_VERSION 1

typedef struct WXL_GfxSurfaceOutputs
{
    uint32_t       structSize;
    uint32_t       frameIndex;   ///< WXL_GfxFrame::frameIndex these were computed for
    uint32_t       width, height;///< the lighting grid
    uint32_t       scale;        ///< full-resolution pixels per grid texel, per axis (1 or 2)
    WXL_GfxVkImage position;     ///< RGBA32F: camera-relative position, distance (0: no surface)
    WXL_GfxVkImage normal;       ///< RGBA16F: world normal, material code (-1 a normal but no albedo, -2 no G-buffer)
    WXL_GfxVkImage omni;         ///< RGBA16F: the filtered visibility of omni slots 0..3 (1 lit)
    WXL_GfxVkImage sun;          ///< RGBA16F: x the sun's extra visibility, y the engine's cascades, z the terrain horizon, w contact
    WXL_GfxVkImage diffuse;      ///< RGBA16F: rgb the lamps' diffuse light a white surface reflects (linear), a = sun.x
} WXL_GfxSurfaceOutputs;

typedef struct WXL_GraphicsLightsSurfaceApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// This frame's outputs, inside a compute pass recorded after the surface pass in the same block. Set
    /// out->structSize first. 0 when the surface lighting did not run this frame.
    int(__cdecl* Outputs)(WXL_GfxSurfaceOutputs* out);

    /// Non-zero while the surface lighting runs (enabled, on DXVK, the G-buffer supplied).
    int(__cdecl* Active)(void);

    /// One line for a panel: state, grid, GPU time.
    const char*(__cdecl* Status)(void);
} WXL_GraphicsLightsSurfaceApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_GRAPHICS_LIGHTS_SURFACE_API_H
