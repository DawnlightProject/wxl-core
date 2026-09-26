// wxl-graphics-lights' lamp light field for the air: a camera-frustum volume of the lamps' in-scattered
// radiance per unit scattering coefficient, which a participating medium (the fog) multiplies by its
// own density and transmittance. Published by the wxl-graphics-lights extension as
// WXL_Api::GetInterface(WXL_GRAPHICS_LIGHTS_FIELD_API_NAME, WXL_GRAPHICS_LIGHTS_FIELD_API_VERSION).
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

#ifndef WXL_GRAPHICS_LIGHTS_FIELD_API_H
#define WXL_GRAPHICS_LIGHTS_FIELD_API_H

#include <stdint.h>

#include "wxl/GraphicsVulkanApi.h"

// When. Call Want() from your wants callback every frame you will read the field. It is computed in
// the lights' compute pass at WXL_GFX_ORDER_LIGHTING (100), after the shadow service (90). A compute
// pass recorded after it in the same block (a higher order: the fog's is WXL_GFX_ORDER_ATMOSPHERE)
// calls Get and binds the images; they hold this frame's field, in VK_IMAGE_LAYOUT_GENERAL. Get
// returns 0 on a frame the field was not computed.
//
// The grid. Froxels over the camera frustum: x, y are the screen uv (0..1, y down) of the scheduler's
// D3D-form viewProjRel (WXL_GfxView), z is exponential in Euclidean distance from the eye:
// slice01 = log(dist / nearDistance) / log(farDistance / nearDistance). Sample with a linear clamp
// sampler at (u, v, slice01); shaders/wxl/lights/field.hlsli does it from a camera-relative point.
//
// The values. Linear, scene-referred (1 is the engine's white), per unit scattering coefficient: the
// radiance a medium of scattering coefficient sigma_s (1/yd) scatters towards the eye is
// sigma_s * inscatter.rgb. Each texel is the average over its depth span (the lamps' falloff integrated
// in closed form), so it is smooth along depth whatever the froxel size. Included: falloff, cone,
// profile, cookies (prefiltered by the froxel's footprint), room gates, the shadow service's point
// shadows when it is loaded, and a thinning between each lamp and the froxel by the medium around the
// camera. Not included: the transmittance from the froxel to the eye (the consumer's march has it).
//
// The phase. inscatter uses a dual Henyey-Greenstein phase towards the eye: lerp(HG(gForward),
// HG(gBack), blend), set with SetPhase (default 0.6, -0.2, 0.25; applies from the next frame). ambient
// and direction describe the same light phase-free, as two lobes, for any other phase or view:
//     L(view) ~= ambient.rgb * lerp(1, 4 pi p(dot(direction.xyz, view)), ambient.a)
// with view the unit direction the light leaves towards (from the froxel to the eye).
//
// Threads. Render thread only.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GRAPHICS_LIGHTS_FIELD_API_NAME    "wxl.graphics-lights.field"
#define WXL_GRAPHICS_LIGHTS_FIELD_API_VERSION 1

typedef struct WXL_GfxLightsField
{
    uint32_t       structSize;
    uint32_t       frameIndex;       ///< WXL_GfxFrame::frameIndex it was computed for
    uint32_t       width, height, depth;
    float          nearDistance;     ///< yards: the first slice starts here
    float          farDistance;      ///< yards: the last slice ends here
    float          eye[3];           ///< world eye it was computed around
    float          viewProjRel[16];  ///< the projection the grid follows (row vectors, camera-relative, D3D form)
    float          phase[4];         ///< gForward, gBack, blend, 0: the phase inscatter was computed with
    WXL_GfxVkImage inscatter;        ///< RGBA16F 3D: rgb towards the eye with the phase, a luminance of the isotropic part
    WXL_GfxVkImage ambient;          ///< RGBA16F 3D: rgb with the isotropic phase 1 / 4pi, a directionality 0..1
    WXL_GfxVkImage direction;        ///< RGBA16F 3D: xyz mean propagation direction (unit), w total luminance
} WXL_GfxLightsField;

typedef struct WXL_GraphicsLightsFieldApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// This frame and the next you will read the field: call it from your wants callback.
    void(__cdecl* Want)(void);

    /// Inside a compute pass recorded after the lights' pass: this frame's field. Set structSize first.
    int(__cdecl* Get)(WXL_GfxLightsField* out);

    /// The phase inscatter is computed with, from the next frame on (g in -0.95..0.95, blend 0..1).
    void(__cdecl* SetPhase)(float gForward, float gBack, float blend);

    /// One line for a panel: grid, lights, GPU time.
    const char*(__cdecl* Status)(void);
} WXL_GraphicsLightsFieldApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_GRAPHICS_LIGHTS_FIELD_API_H
