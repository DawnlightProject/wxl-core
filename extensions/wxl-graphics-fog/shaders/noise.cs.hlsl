// wxl-graphics-fog: bakes the noise textures once per device. pc.a.x: 0 the shape noise (128^3:
// R Perlin-Worley, G B A Worley fBm at 8, 16 and 32 cells), 1 the detail noise (32^3: R G Worley fBm
// at 2 and 4 cells, B A Perlin fBm at 4 and 8), 2 the curl noise (32^3: a divergence-free field,
// xyz packed as 0.5 + 0.5 v). All tile seamlessly. Every channel is stretched to a mean of 0.5 and a
// spread near a uniform one's, from the raw statistics measured once, so coverage remaps keep their mean.
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

#include "noise.hlsli"

FOG_OUT(0) [[vk::image_format("rgba8")]] RWTexture3D<float4> outNoise;

float3 Potential(float3 uvw)
{
    return float3(Perlin(uvw * 4.0, 4, 11u), Perlin(uvw * 4.0, 4, 12u), Perlin(uvw * 4.0, 4, 13u));
}

float3 Curl(float3 uvw)
{
    const float e = 1.0 / 128.0;
    float3 dx = (Potential(uvw + float3(e, 0, 0)) - Potential(uvw - float3(e, 0, 0))) / (2.0 * e);
    float3 dy = (Potential(uvw + float3(0, e, 0)) - Potential(uvw - float3(0, e, 0))) / (2.0 * e);
    float3 dz = (Potential(uvw + float3(0, 0, e)) - Potential(uvw - float3(0, 0, e))) / (2.0 * e);
    return float3(dy.z - dz.y, dz.x - dx.z, dx.y - dy.x);
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint size = pc.a.x == 0u ? 128u : 32u;
    if (any(id >= size)) return;
    float3 uvw = (float3(id) + 0.5) / float(size);
    float4 v;
    if (pc.a.x == 0u)
    {
        float perlin = PerlinFbm(uvw, 4, 4, 1u);
        float worley = WorleyFbm(uvw, 4, 5u);
        float pw = saturate(Remap(perlin, worley - 1.0, 1.0, 0.0, 1.0));
        v = float4(pw, WorleyFbm(uvw, 8, 9u), WorleyFbm(uvw, 16, 13u), WorleyFbm(uvw, 32, 17u));
        // Raw means (0.668, 0.48, 0.48, 0.48) and spreads (0.055, 0.117, ...), stretched to sd 0.27.
        v = saturate((v - float4(0.668, 0.48, 0.48, 0.485)) * float4(4.9, 2.3, 2.3, 2.3) + 0.5);
    }
    else if (pc.a.x == 1u)
    {
        v = float4(WorleyFbm(uvw, 2, 21u), WorleyFbm(uvw, 4, 25u), PerlinFbm(uvw, 4, 3, 29u), PerlinFbm(uvw, 8, 2, 33u));
        v = saturate((v - float4(0.486, 0.476, 0.5, 0.5)) * float4(2.3, 2.3, 3.2, 2.7) + 0.5);
    }
    else
    {
        float3 c = Curl(uvw) / 8.0;
        v = float4(saturate(c * 0.5 + 0.5), 1.0);
    }
    outNoise[id] = v;
}
