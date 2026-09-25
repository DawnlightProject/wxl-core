// wxl-forever fog, froxel pass: omni.
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

// Before the lamps: at the point injection samples, how much of each omni slot's light reaches the
// froxel past the casters its map holds, one channel a slot (1 for a slot not bound). The lamps then
// read one texel, whichever of the four maps their light holds.

// This frame's omni shadow maps, from the light service's omni table (lights::PublishOmni): row 1
// holds 20 texels a slot (position and radius; 1 / atlas width, 1 / atlas height, face size,
// own-housing yards; then 3 rows a face taking (x, y, z, 1) to (u * w, v * w, w)).
sampler2D omniTable : register(s4);
sampler2D omni0 : register(s11);
sampler2D omni1 : register(s12);
sampler2D omni2 : register(s13);
sampler2D omni3 : register(s14);
float4 omniFogC : register(c221);

// How much of omni slot's light reaches the froxel at r (camera-relative) past the casters its map
// holds: the face whose axis r lies along, its rows, four atlas reads half a texel around the point
// (kept inside the face's cell), so a beam's edge is a gradient, not a switch. Lit wherever the map
// cannot answer (past the radius, before the near plane, a face not rendered, off its cell), and
// only a froxel within the radius reads more than the slot's first texel.
float OmniFog(sampler2D map, float slot, float3 r)
{
    float u = (slot * 20.0 + 0.5) * omniFogC.w;
    float4 L = tex2Dlod(omniTable, float4(u, 0.75, 0, 0));
    float3 v = r - L.xyz;
    float lit = 1.0;
    [branch] if (dot(v, v) < L.w * L.w)
    {
        float3 ax = abs(v);
        float f0 = ax.x >= ax.y && ax.x >= ax.z ? (v.x > 0.0 ? 0.0 : 1.0) : (ax.y >= ax.z ? (v.y > 0.0 ? 2.0 : 3.0) : (v.z > 0.0 ? 4.0 : 5.0));
        float ur = u + (2.0 + f0 * 3.0) * omniFogC.w;
        float4 h = float4(r, 1.0);
        float3 s = float3(dot(h, tex2Dlod(omniTable, float4(ur, 0.75, 0, 0))),
                          dot(h, tex2Dlod(omniTable, float4(ur + omniFogC.w, 0.75, 0, 0))),
                          dot(h, tex2Dlod(omniTable, float4(ur + 2.0 * omniFogC.w, 0.75, 0, 0))));
        float2 uv = s.xy / max(s.z, 0.05);
        float2 cell = float2(fmod(f0, 4.0), floor(f0 / 4.0)) * float2(0.25, 0.5);
        bool inside = s.z > 0.05 && all(uv >= cell) && all(uv <= cell + float2(0.25, 0.5));
        float depth = (s.z - omniFogC.y) / L.w;
        // Second texel: 1 / atlas width, 1 / atlas height, face size, own-housing yards (casters that
        // near the light do not shadow it).
        float4 T = tex2Dlod(omniTable, float4(u + omniFogC.w, 0.75, 0, 0));
        if (inside)
        {
            float2 lo = cell + 0.5 * T.xy, hi = cell + float2(0.25, 0.5) - 0.5 * T.xy;
            float blocked = 0.0;
            for (int t = 0; t < 4; ++t)
            {
                float2 o = (float2(fmod(t, 2.0), floor(t * 0.5)) - 0.5) * T.xy;
                float z = tex2Dlod(map, float4(clamp(uv + o, lo, hi), 0, 0)).r;
                blocked += z < depth && z * L.w >= T.w ? 0.25 : 0.0;
            }
            lit = 1.0 - blocked;
        }
    }
    return lit;
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 tile = floor(vpos / grid.xy);
    float k = tile.y * grid.w + tile.x;
    if (k >= grid.z) return 1.0;
    float2 cell = vpos - tile * grid.xy;
    float3 r = FroxelPoint(cell, k);
    float4 vis = 1.0;
    [branch] if (omniFogC.x > 0.5) vis.x = OmniFog(omni0, 0.0, r);
    [branch] if (omniFogC.x > 1.5) vis.y = OmniFog(omni1, 1.0, r);
    [branch] if (omniFogC.x > 2.5) vis.z = OmniFog(omni2, 2.0, r);
    [branch] if (omniFogC.x > 3.5) vis.w = OmniFog(omni3, 3.0, r);
    return vis;
}
