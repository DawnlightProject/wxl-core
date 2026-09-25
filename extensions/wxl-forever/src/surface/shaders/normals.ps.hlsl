// wxl-forever surface lighting: normals reconstructed from the depth, at half resolution.
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

#include "surface/shaders/surface.hlsli"

// Out: (normal xyz, distance), w < 0 for the sky. On each axis the side is chosen whose second
// neighbour continues the first one in a straight line (the smaller second difference): a crease or
// a silhouette breaks that line on one side only, so the normal is taken from the other, and a
// curved surface such as a character's shoulder is followed rather than cut into facets.
float4 main(float2 vpos : VPOS) : COLOR0
{
    // Each half-resolution texel stands for the first full texel of its 2 x 2 block, as PassUv does.
    float2 uv = (vpos * 2.0 + 0.5) * screen.xy;
    float4 here = SurfaceAt(uv);
    if (here.w < 0.0) return float4(0.0, 0.0, 1.0, -1.0);

    float2 sx = float2(screen.x, 0.0), sy = float2(0.0, screen.y);
    float4 r1 = SurfaceAt(uv + sx), r2 = SurfaceAt(uv + 2.0 * sx);
    float4 l1 = SurfaceAt(uv - sx), l2 = SurfaceAt(uv - 2.0 * sx);
    float4 d1 = SurfaceAt(uv + sy), d2 = SurfaceAt(uv + 2.0 * sy);
    float4 u1 = SurfaceAt(uv - sy), u2 = SurfaceAt(uv - 2.0 * sy);

    float errRight = (r1.w > 0.0 && r2.w > 0.0) ? abs(2.0 * r1.w - r2.w - here.w) : 1e9;
    float errLeft  = (l1.w > 0.0 && l2.w > 0.0) ? abs(2.0 * l1.w - l2.w - here.w) : 1e9;
    float errDown  = (d1.w > 0.0 && d2.w > 0.0) ? abs(2.0 * d1.w - d2.w - here.w) : 1e9;
    float errUp    = (u1.w > 0.0 && u2.w > 0.0) ? abs(2.0 * u1.w - u2.w - here.w) : 1e9;

    float3 dx = errRight <= errLeft ? r1.xyz - here.xyz : here.xyz - l1.xyz;
    float3 dy = errDown <= errUp ? d1.xyz - here.xyz : here.xyz - u1.xyz;
    float3 n = normalize(cross(dx, dy));
    n = dot(n, here.xyz) > 0.0 ? -n : n;
    return float4(n, here.w);
}
