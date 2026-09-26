// wxl-graphics-shadow: what the service's own passes share -- the pass constants, the push constants,
// depth reading and position reconstruction.
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

#ifndef WXL_SHADOW_COMMON_HLSLI
#define WXL_SHADOW_COMMON_HLSLI

#include "shared.h"

#define WXL_SHADOW_BINDING SH_B_SHADOW
#include "wxl/shadow/shadow.hlsli"

struct ShPush
{
    uint4  a;
    float4 b;
};
[[vk::push_constant]] ShPush push;

[[vk::binding(SH_B_PASS, 0)]] cbuffer ShPass
{
    float4 P[SH_PASS_ROWS > SH_CONVERT_ROWS ? SH_PASS_ROWS : SH_CONVERT_ROWS];
};

[[vk::binding(SH_B_DEPTH, 0)]]   Texture2D<float>  depthTex;
[[vk::binding(SH_B_NORMALS, 0)]] Texture2D<float4> normalTex;

// --- depth and positions -------------------------------------------------------------------------------

float ShNdc(float d) { return saturate((d - P[SH_ROW_DEPTH].x) / max(P[SH_ROW_DEPTH].y - P[SH_ROW_DEPTH].x, 1e-6)); }

bool ShIsSky(float d) { return d > P[SH_ROW_DEPTH].y + 0.00001; }

// View-space depth in yards from the projection's 0..1 depth (WXL_GfxView::depthLinearize).
float ShLinear(float ndc)
{
    float4 L = P[SH_ROW_LINEAR];
    return (L.x - ndc * L.y) / (ndc * L.z - L.w);
}

float4 ShDp4(float4 v, int row)
{
    return float4(dot(v, P[row]), dot(v, P[row + 1]), dot(v, P[row + 2]), dot(v, P[row + 3]));
}

// The camera-relative position seen at uv with 0..1 depth ndc.
float3 ShRel(float2 uv, float ndc)
{
    float4 h = ShDp4(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndc, 1.0), SH_ROW_INVVP);
    return h.xyz / h.w;
}

// A camera-relative point's clip position (w is its view depth, yards).
float4 ShClip(float3 rel) { return ShDp4(float4(rel, 1.0), SH_ROW_VP); }

float2 ShClipToUv(float4 clip) { return float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5); }

float3 ShRelAt(int2 px)
{
    float2 size = P[SH_ROW_SCREEN].xy;
    px = clamp(px, int2(0, 0), int2(size) - 1);
    float d = depthTex.Load(int3(px, 0));
    return ShRel((float2(px) + 0.5) / size, ShNdc(d));
}

// The receiver's world normal: the G-buffer's where a material wrote one, else from depth (the side
// with the smaller depth step on each axis, so an edge never bends it). Faces the camera.
float3 ShNormal(int2 px, float3 p)
{
    float3 n;
    // The stand-in is 1 x 1: read the G-buffer only when it is bound.
    float4 g = P[SH_ROW_DEPTH].z > 0.5 ? normalTex.Load(int3(px, 0)) : float4(0.0, 0.0, 0.0, 0.0);
    if (g.a > 0.25)
    {
        float3 v = normalize(g.rgb * 2.0 - 1.0);
        n = float3(dot(v, P[SH_ROW_VIEWROT].xyz), dot(v, P[SH_ROW_VIEWROT + 1].xyz), dot(v, P[SH_ROW_VIEWROT + 2].xyz));
    }
    else
    {
        float3 r = ShRelAt(px + int2(1, 0)), l = ShRelAt(px - int2(1, 0));
        float3 d = ShRelAt(px + int2(0, 1)), u = ShRelAt(px - int2(0, 1));
        float3 dx = abs(dot(r - p, p)) < abs(dot(p - l, p)) ? r - p : p - l;
        float3 dy = abs(dot(d - p, p)) < abs(dot(p - u, p)) ? d - p : p - u;
        n = cross(dy, dx);
    }
    n = normalize(n);
    return dot(n, p) > 0.0 ? -n : n;
}

#endif
