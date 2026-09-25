// wxl-forever fog, froxel pass: resolve.
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

#include "fog/shaders/common.hlsli"

// Temporal resolve: blends last frame's reprojected volume into this frame's, with the history
// clamped to what this frame's neighbourhood allows rather than accepted or thrown away whole.
// Within the radius of a light that moved lately the clamp has no slack. Where the clamp had to
// move the history by more than the neighbourhood's own spread the fog changed there (a carried
// torch's halo passed) and the history weighs down to movingC.y (movingC.w on); elsewhere it keeps
// its full weight, so a halo leaves no trail and the fog around a runner shows no grain.
sampler2D history : register(s1);
sampler2D current : register(s4);

// Resolve only (Froxel.cpp): moving lights' count, the history weight where the fog changed, last
// frame's log(far / near) (0 = this frame's), 1 = the change-based cut is on; then camera-relative
// position and radius of each light.
float4 movingC : register(c216);
float4 movingLights[4] : register(c217);

float4 Fetch(float2 cell, float k)
{
    cell = clamp(cell, 0.0, grid.xy - 1.0);
    k = clamp(k, 0.0, grid.z - 1.0);
    return tex2Dlod(current, float4(AtlasUV(cell + 0.5, k), 0, 0));
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 tile = floor(vpos / grid.xy);
    float k = tile.y * grid.w + tile.x;
    if (k >= grid.z) return 0.0;
    float2 cell = vpos - tile * grid.xy;

    float4 now = Fetch(cell, k);
    bool showClamp = screen.w > 8.5 && screen.w < 9.5;
    if (range.w <= 0.0) return showClamp ? float4(0, 0, 1, 1) : now;

    float3 rp = FroxelCentre(cell, k) + moved.xyz;
    float4 pc = float4(dot(float4(rp, 1.0), prev0), dot(float4(rp, 1.0), prev1),
                       dot(float4(rp, 1.0), prev2), dot(float4(rp, 1.0), prev3));
    if (pc.w <= 0.0001) return showClamp ? float4(0, 0, 1, 1) : now;
    float2 puv = float2(pc.x / pc.w * 0.5 + 0.5, 0.5 - pc.y / pc.w * 0.5);
    // Last frame's slices: the far distance eases across a doorway, and the history follows it.
    float ps = DistToSliceL(length(rp), movingC.z > 0.0 ? movingC.z : atlas.w);
    if (any(puv < 0.0) || any(puv > 1.0) || ps < 0.0 || ps > grid.z)
        return showClamp ? float4(0, 0, 1, 1) : now;

    float4 past = SampleVolume(history, puv, ps);
    float weight = range.w, slackShare = 0.25;
    float3 centre = FroxelCentre(cell, k);
    for (int i = 0; i < 4; ++i)
    {
        if ((float)i >= movingC.x) break;
        float3 v = centre - movingLights[i].xyz;
        if (dot(v, v) < movingLights[i].w * movingLights[i].w)
        {
            if (movingC.w < 0.5) weight = min(weight, movingC.y);
            slackShare = 0.0;
        }
    }
    if (counts.z > 0.5) return lerp(now, past, weight);

    float4 lo = now, hi = now, n;
    n = Fetch(cell + float2(1, 0), k);  lo = min(lo, n); hi = max(hi, n);
    n = Fetch(cell - float2(1, 0), k);  lo = min(lo, n); hi = max(hi, n);
    n = Fetch(cell + float2(0, 1), k);  lo = min(lo, n); hi = max(hi, n);
    n = Fetch(cell - float2(0, 1), k);  lo = min(lo, n); hi = max(hi, n);
    n = Fetch(cell, k + 1.0);           lo = min(lo, n); hi = max(hi, n);
    n = Fetch(cell, k - 1.0);           lo = min(lo, n); hi = max(hi, n);

    // A little slack around the neighbourhood so jittered noise does not clamp every frame.
    float4 slack = (hi - lo) * slackShare + 0.00001;
    float4 kept = clamp(past, lo - slack, hi + slack);
    if (movingC.w > 0.5)
    {
        // How far the history had to move to fit, against the spread it had to fit into.
        float3 lum = float3(0.299, 0.587, 0.114);
        float shiftL = abs(dot(kept.rgb - past.rgb, lum)) / max(dot(hi.rgb - lo.rgb, lum), 0.00002);
        float shiftA = abs(kept.a - past.a) / max(hi.a - lo.a, 0.000005);
        float change = saturate(max(shiftL, shiftA) - 0.5);
        weight = lerp(weight, min(weight, movingC.y), change);
    }

    if (showClamp)
    {
        float3 lum = float3(0.299, 0.587, 0.114);
        float shift = abs(dot(kept.rgb - past.rgb, lum)) / max(dot(past.rgb, lum), 0.0001);
        return float4(saturate(shift * 4.0), 0.0, 0.0, 1.0);
    }
    return lerp(now, kept, weight);
}
