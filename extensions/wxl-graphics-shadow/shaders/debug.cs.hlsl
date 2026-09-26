// wxl-graphics-shadow: the debug views, into an RGBA16F image the overlay draws over the finished frame
// (alpha: how much of the view covers the scene).
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

#include "common.hlsli"

[[vk::binding(SH_B_IN0, 0)]] Texture3D<float4> masks;
[[vk::binding(SH_B_OUT0, 0)]] [[vk::image_format("rgba16f")]] RWTexture2D<float4> outDebug;

static const float3 kSlotColours[8] = {
    float3(1.0, 0.35, 0.2), float3(0.2, 0.8, 1.0), float3(0.4, 1.0, 0.3), float3(1.0, 0.9, 0.2),
    float3(0.9, 0.3, 1.0), float3(0.3, 0.4, 1.0), float3(1.0, 0.6, 0.8), float3(0.6, 1.0, 0.9) };

float SlotMask(uint2 px, int s)
{
    float4 v = masks.Load(int4(px, 1 + s / 4, 0));
    int c = s & 3;
    return c == 0 ? v.x : (c == 1 ? v.y : (c == 2 ? v.z : v.w));
}

// Which cascade holds p (0..3), 4 none.
int CascadeOf(float3 p)
{
    int maps = int(wxlShadow[WXL_SHADOW_ROW_CASCADE].x);
    float4 h = float4(p, 1.0);
    for (int m = 0; m < maps; ++m)
    {
        int row = WXL_SHADOW_ROW_CASCADEROWS + m * 3;
        float2 s = float2(dot(h, wxlShadow[row]), dot(h, wxlShadow[row + 1]));
        if (max(abs(s.x), abs(s.y)) < 0.97) return m;
    }
    return 4;
}

// How near the view ray to p passes each capsule: a red silhouette over everything.
float CapsuleHeat(float3 p)
{
    float4 info = wxlShadow[WXL_SHADOW_ROW_CAPSULE];
    float heat = 0.0;
    for (int k = 0; k < int(info.x); ++k)
    {
        float4 A = wxlShadow[WXL_SHADOW_ROW_CAPSULES + k * 2];
        float4 B = wxlShadow[WXL_SHADOW_ROW_CAPSULES + k * 2 + 1];
        float t, s;
        WxlShadowClosest(float3(0.0, 0.0, 0.0), p, A.xyz, B.xyz, t, s);
        float d = length(p * t - (A.xyz + (B.xyz - A.xyz) * s));
        heat = max(heat, (1.0 - smoothstep(A.w * 0.8, A.w, d)) * saturate(B.w));
    }
    return heat;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    float2 size = P[SH_ROW_SCREEN].xy;
    if (any(float2(id.xy) >= size)) return;
    int view = int(P[SH_ROW_DEBUG].x);
    int slot = int(P[SH_ROW_DEBUG].y);
    float2 uv = (float2(id.xy) + 0.5) / size;
    float d = depthTex.Load(int3(id.xy, 0));
    bool sky = ShIsSky(d);
    float3 p = ShRel(uv, ShNdc(d));
    float4 L0 = masks.Load(int4(id.xy, 0, 0));
    float4 o = float4(0.0, 0.0, 0.0, 1.0);
    if (view == SH_VIEW_SUN) o.rgb = L0.xxx;
    else if (view == SH_VIEW_MOON) o.rgb = L0.yyy;
    else if (view == SH_VIEW_TERRAIN) o.rgb = L0.zzz;
    else if (view == SH_VIEW_CONTACT) o.rgb = L0.www;
    else if (view == SH_VIEW_SLOT) o.rgb = SlotMask(id.xy, clamp(slot, 0, WXL_SHADOW_MAX_SLOTS - 1)).xxx;
    else if (view == SH_VIEW_SLOTS)
    {
        // Every slot's shadow in its colour, over a mid grey: what each light loses where.
        float3 c = 0.5;
        for (int s = 0; s < WXL_SHADOW_MAX_SLOTS; ++s)
        {
            float loss = 1.0 - SlotMask(id.xy, s);
            c = lerp(c, kSlotColours[s & 7] * 0.35, loss * 0.8);
        }
        o.rgb = c;
    }
    else if (view == SH_VIEW_CASCADES)
    {
        int m = sky ? 4 : CascadeOf(p);
        float3 tint = m == 0 ? float3(1.0, 0.3, 0.3) : (m == 1 ? float3(0.3, 1.0, 0.3) : (m == 2 ? float3(0.3, 0.4, 1.0) : (m == 3 ? float3(1.0, 1.0, 0.3) : 0.2)));
        o.rgb = tint * (0.35 + 0.65 * L0.x);
    }
    else if (view == SH_VIEW_CAPSULES)
    {
        o = float4(1.0, 0.15, 0.1, 0.6 * CapsuleHeat(sky ? normalize(p) * 200.0 : p));
    }
    else if (view == SH_VIEW_NORMALS)
    {
        o.rgb = sky ? 0.0 : ShNormal(int2(id.xy), p) * 0.5 + 0.5;
    }
    else if (view == SH_VIEW_ATLAS)
    {
        // The whole atlas, stretched over the screen: each texel's mean caster depth (white: nothing).
        float2 e = wxlShadow[WXL_SHADOW_ROW_EVSM].xy;
        float4 m = wxlShadowMaps.SampleLevel(wxlShadowPoint, uv, 0);
        float x = m.x > 0.0 ? log(max(m.x, 1e-30)) / e.x : 1.0;
        float depth = saturate(x * 0.5 + 0.5);
        o.rgb = float3(depth, depth, depth * 0.8 + 0.2);
    }
    if (sky && view != SH_VIEW_ATLAS && view != SH_VIEW_CAPSULES) o.rgb = float3(0.05, 0.05, 0.1);
    outDebug[id.xy] = o;
}
