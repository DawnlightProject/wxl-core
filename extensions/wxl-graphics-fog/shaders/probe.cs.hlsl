// wxl-graphics-fog: the medium around the camera, read back by the host a few frames later: the
// camera's immersion and the density and visibility the threat API reports. Seven samples, the camera
// and a yard either side of it on each axis, averaged. Writes two float4:
//   0  extinction (all media), outdoor fog, indoor share, smoke
//   1  visibility (yards to 5 % transmittance), sun reaching the camera, lamp light, 1 (written)
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

#include "medium.hlsli"

[[vk::binding(FOG_B_RWBUF, 0)]] RWStructuredBuffer<float4> probeOut;

[numthreads(1, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    static const float3 kOffsets[7] = { float3(0, 0, 0), float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0),
                                        float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
    float4 a = 0.0, b = 0.0;
    float3 view = RayDir(float2(0.5, 0.5));
    [unroll] for (int i = 0; i < 7; ++i)
    {
        float3 p = eye.xyz + kOffsets[i];
        Sample s = SampleMedium(p, 0.5, float2(0.5, 0.5), true);
        float ext = Extinction(s);
        a += float4(ext, s.fog, s.indoor, s.smoke);
        float3 airL;
        float3 Lin = InScatter(s, p, view, 0.5, true, airL);
        b += float4(0.0, Luma(Lin - s.lamps), Luma(s.lamps), 0.0);
    }
    a /= 7.0;
    b /= 7.0;
    b.x = a.x > 1e-5 ? 3.0 / a.x : 100000.0;
    b.w = 1.0;
    probeOut[0] = a;
    probeOut[1] = b;
}
