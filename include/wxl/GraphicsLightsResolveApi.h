// wxl-graphics-lights' HDR scene contract: how the world's FP16 colour is encoded between the lights'
// composite and the resolve, the curve that resolves it, and who resolves it. Published by the
// wxl-graphics-lights extension as
// WXL_Api::GetInterface(WXL_GRAPHICS_LIGHTS_RESOLVE_API_NAME, WXL_GRAPHICS_LIGHTS_RESOLVE_API_VERSION).
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

#ifndef WXL_GRAPHICS_LIGHTS_RESOLVE_API_H
#define WXL_GRAPHICS_LIGHTS_RESOLVE_API_H

#include <stdint.h>

// The chain (extensions/wxl-graphics-lights/docs/design.md, section 2). The world draws into
// wxl-graphics-extend's FP16 colour. The lights' composite (WXL_GFX_ORDER_LIGHTING) expands each pixel
// into scene-referred linear light by the inverse of the curve below, adds the lamps, and writes
// pow(x, 1 / gamma) back. The fog and effects draw on that. A resolve maps T(exposure * x) to the back
// buffer. Pixels no lamp touched come out exactly as the engine drew them when the resolve uses T with
// the published knee and exposure 1.
//
// The curve, per channel, linear light: T(x) = x up to the knee k, then k + (1 - k)(1 - exp(-(x - k) /
// (1 - k))). shaders/wxl/lights/hdr.hlsli implements T, its inverse and the encoding.
//
// Ownership. Until a pass claims it, the lights run an interim resolve at WXL_GFX_ORDER_RESOLVE (350)
// on every frame they expanded and nobody resolved before. wxl-graphics-post calls ClaimResolve once
// and resolves itself at WXL_GFX_ORDER_POST (300); the interim resolve then never runs again.
//
// Threads. Render thread only.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_GRAPHICS_LIGHTS_RESOLVE_API_NAME    "wxl.graphics-lights.resolve"
#define WXL_GRAPHICS_LIGHTS_RESOLVE_API_VERSION 1

#define WXL_GFX_RESOLVED_NONE    0   ///< not resolved yet this frame
#define WXL_GFX_RESOLVED_LIGHTS  1   ///< by the lights' interim resolve
#define WXL_GFX_RESOLVED_OTHER   2   ///< by a pass ordered before it (the claimed owner, or another)

typedef struct WXL_GfxHdrScene
{
    uint32_t structSize;
    uint32_t frameIndex;   ///< WXL_GfxFrame::frameIndex this describes
    int32_t  hdr;          ///< the world was drawn in the FP16 colour this frame
    int32_t  expanded;     ///< the lights' composite turned it into scene-referred values this frame
    float    gamma;        ///< the target holds pow(x, 1 / gamma) of linear scene-referred x
    float    knee;         ///< the curve's knee k
    float    exposure;     ///< what the interim resolve multiplies by before the curve
    int32_t  resolvedBy;   ///< WXL_GFX_RESOLVED_*, as of the last resolve step
    int32_t  claimed;      ///< another pass claimed the resolve (ClaimResolve)
} WXL_GfxHdrScene;

typedef struct WXL_GraphicsLightsResolveApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// This frame's scene contract. Set structSize first; returns 0 before the first frame.
    int(__cdecl* Scene)(WXL_GfxHdrScene* out);

    /// Takes the resolve for the process: the interim resolve stands down. owner is copied for the log.
    /// Returns non-zero when taken (a second claim replaces the first owner's name).
    int(__cdecl* ClaimResolve)(const char* owner);

    /// One line for a panel: the curve, who resolves, and the last frame's state.
    const char*(__cdecl* Status)(void);
} WXL_GraphicsLightsResolveApi;

#ifdef __cplusplus
}
#endif

#endif // WXL_GRAPHICS_LIGHTS_RESOLVE_API_H
