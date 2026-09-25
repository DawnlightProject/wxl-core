// wxl-graphics-extend: the world's depth read back to view depth, camera-relative positions and
// last frame's screen position.
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

#ifndef WXL_GFX_DEPTH_HLSLI
#define WXL_GFX_DEPTH_HLSLI

#include "wxl/gfx/common.hlsli"

// The INTZ texture's .r is the hardware depth, which the world drew into the viewport range
// WXL_GfxFrame::depthRange (minZ, maxZ). Everything here works on the projection's own 0..1 depth
// ("ndc" below), which this recovers first. The matrices are WXL_GfxView's, handed over as columns
// (wxl::gfx::matrix::Columns) for WxlDp4Rows; "rel" is a camera-relative point, world - eye.

float WxlDepthToNdc(float d, float2 range)
{
    return saturate((d - range.x) / (range.y - range.x));
}

// Whether nothing was drawn at a depth: the world's geometry lands inside the range, the clear
// value (1) lies past it, so the sky and everything undrawn read as more than maxZ.
bool WxlDepthIsSky(float d, float2 range)
{
    return d > range.y + 0.00001;
}

// The view-space z the projection consumed, from the 0..1 depth: L = WXL_GfxView::depthLinearize,
// (P[14], P[15], P[11], P[10]) of the projection, which makes ndc = (z P[10] + P[14]) / (z P[11] +
// P[15]) and this its inverse; right for a perspective and an orthographic projection alike. The
// engine's projection is left-handed, so the value grows positive in front of the camera, in yards.
float WxlLinearDepth(float ndc, float4 L)
{
    return (L.x - ndc * L.y) / (ndc * L.z - L.w);
}

// The camera-relative position of the surface seen at uv with 0..1 depth ndc:
// c0..c3 = Columns(invViewProjRel), clip -> (rel, 1) before the divide by w.
float3 WxlReconstructRel(float2 uv, float ndc, float4 c0, float4 c1, float4 c2, float4 c3)
{
    float4 h = WxlDp4Rows(float4(WxlUvToNdc(uv), ndc, 1.0), c0, c1, c2, c3);
    return h.xyz / h.w;
}

// Where a camera-relative point of this frame was last frame, as last frame's clip position:
// c0..c3 = Columns(reprojectRel). Check w: at or below zero the point lay behind last frame's
// camera and its uv means nothing.
float4 WxlReprojectClip(float3 rel, float4 c0, float4 c1, float4 c2, float4 c3)
{
    return WxlDp4Rows(float4(rel, 1.0), c0, c1, c2, c3);
}

// The same as last frame's texture coordinate (compare it with 0..1 to know it is on screen).
float2 WxlReprojectUv(float3 rel, float4 c0, float4 c1, float4 c2, float4 c3)
{
    float4 clip = WxlReprojectClip(rel, c0, c1, c2, c3);
    return WxlNdcToUv(clip.xy / clip.w);
}

#endif
