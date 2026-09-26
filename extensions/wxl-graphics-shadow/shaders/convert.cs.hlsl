// wxl-graphics-shadow: one light's core omni maps (static or every caster, and units) into its cube of
// the filtered atlas: the nearest caster per direction, the light's own housing dropped, warped to EVSM
// moments and averaged over 2 x 2 samples per output texel.
//   push a.x  the faces to convert (bit f)
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

[[vk::binding(SH_B_SRC0, 0)]] Texture2D<float> mainAtlas;
[[vk::binding(SH_B_SRC1, 0)]] Texture2D<float> unitAtlas;
[[vk::binding(SH_B_OUT0, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> outAtlas;

// The direction through a face's cell uv (the inverse of shadow.hlsli's WxlShadowFace).
float3 FaceDir(uint face, float2 uv)
{
    float2 side = uv * 2.0 - 1.0;
    float lead = (face & 1u) ? -1.0 : 1.0;
    uint axis = face >> 1;
    return axis == 0u ? float3(lead, side.x, side.y) : (axis == 1u ? float3(side.y, lead, side.x) : float3(side.x, side.y, lead));
}

// The core atlas's depth over the radius along the face's axis, 1 where nothing was drawn.
float CoreDepth(Texture2D<float> atlas, int rows, uint face, float3 p, float2 size)
{
    float4 h = float4(p, 1.0);
    int r = rows + int(face) * 3;
    float3 s = float3(dot(h, P[r]), dot(h, P[r + 1]), dot(h, P[r + 2]));
    if (s.z <= 1e-5) return 1.0;
    // The face's cell (face % 4, face / 4) of the core's 4 x 2 atlas, never a neighbour's texel.
    float2 cellSize = size * float2(0.25, 0.5);
    float2 lo = float2(float(face % 4u), float(face / 4u)) * cellSize;
    int2 texel = int2(clamp(floor(s.xy / s.z * size), lo, lo + cellSize - 1.0));
    return atlas.Load(int3(texel, 0));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint face = id.z;
    if (face >= 6u || ((push.a.x >> face) & 1u) == 0u) return;
    float4 info = P[SH_CROW_INFO];
    uint F = uint(info.z);
    if (any(id.xy >= F)) return;
    float4 L = P[SH_CROW_LIGHT];
    float4 sizes = P[SH_CROW_SIZES];
    float2 e = wxlShadow[WXL_SHADOW_ROW_EVSM].xy;
    float4 moments = 0.0;
    [unroll] for (uint k = 0u; k < 4u; ++k)
    {
        float2 sub = float2(float(k & 1u), float(k >> 1u)) * 0.5 + 0.25;
        float3 dir = FaceDir(face, (float2(id.xy) + sub) / float(F));
        float3 p = L.xyz + dir;
        float d = CoreDepth(mainAtlas, SH_CROW_MAIN, face, p, sizes.xy);
        if (info.y > 0.5) d = min(d, CoreDepth(unitAtlas, SH_CROW_UNITS, face, p, sizes.zw));
        // The light's own fixture (a lamp's cage and post; a torch and the hand holding it) casts nothing.
        if (d < info.x) d = 1.0;
        float x = 2.0 * saturate(d) - 1.0;
        float pos = exp(e.x * x), neg = -exp(-e.y * x);
        moments += float4(pos, pos * pos, neg, neg * neg);
    }
    outAtlas[uint2(P[SH_CROW_ORIGIN].xy) + face % 3u * F * uint2(1, 0) + face / 3u * F * uint2(0, 1) + id.xy] = moments * 0.25;
}
