// wxl-graphics-lights: the halo without the fog. One thread per froxel column integrates the field's
// in-scattered light front to back through a thin, even haze (fieldD.w per yard), with its transmittance,
// and stores in each froxel what has gathered up to its centre, so a pixel reads its halo with one
// trilinear fetch at its own distance. docs/design.md, section 8.3.
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

LIGHTS_TEX(LIGHTS_T_FIELD) Texture3D<float4> fieldTex;

LIGHTS_OUT(0) [[vk::image_format("rgba16f")]] RWTexture3D<float4> outHalo;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= fieldC.xy)) return;
    float sigma = fieldD.w;
    float3 sum = 0.0;
    float transmit = 1.0;
    uint depth = uint(fieldC.z);
    [loop] for (uint z = 0; z < depth; ++z)
    {
        float d0 = FieldDistance(float(z) / fieldC.z);
        float d1 = FieldDistance(float(z + 1u) / fieldC.z);
        float span = d1 - d0;
        float3 inscatter = fieldTex.Load(int4(id.xy, z, 0)).rgb * sigma;
        // Half the froxel up to its centre (what a pixel at its centre sees), then the other half.
        float half0 = exp(-sigma * span * 0.5);
        float3 first = inscatter * transmit * (1.0 - half0) / max(sigma, 1e-6);
        outHalo[uint3(id.xy, z)] = float4(sum + first, transmit * half0);
        float whole = half0 * half0;
        sum += inscatter * transmit * (1.0 - whole) / max(sigma, 1e-6);
        transmit *= whole;
    }
}
