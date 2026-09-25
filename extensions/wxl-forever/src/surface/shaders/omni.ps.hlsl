// wxl-forever surface lighting: the omni shadow maps' share at each lighting texel.
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

#include "surface/shaders/shading.hlsli"

// Drawn at the lighting resolution before the light pass, each pixel the full-resolution texel
// PassUv names: how much of each of the four omni slots' light reaches its surface point past the
// casters in the slot's map (OmniSlot: bias, blocker search, penumbra-wide disc, the slot's fade),
// one channel a slot, 1 on the sky and for a slot not bound. The light pass reads it per light, so
// its own loop keeps its registers.
float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = PassUv(vpos);
    float4 here = SurfaceAt(uv);
    if (here.w < 0.0 || omniC.z < 0.5) return 1.0;
    float3 n = SurfaceNormal(vpos, uv, here);
    // The filter's rotation stays put (this term joins after the history).
    float rot = BlueNoiseAt(blueTex, vpos).w * 6.2831853;
    float3 pos = here.xyz + n * (0.02 + 0.003 * here.w);
    float4 vis = 1.0;
    float face;
    [branch] if (omniLights[0].w > 0.0) vis.x = OmniSlot(omni0, 0, pos, n, omniSizes.x, rot, face);
    [branch] if (omniLights[1].w > 0.0) vis.y = OmniSlot(omni1, 1, pos, n, omniSizes.y, rot, face);
    [branch] if (omniLights[2].w > 0.0) vis.z = OmniSlot(omni2, 2, pos, n, omniSizes.z, rot, face);
    [branch] if (omniLights[3].w > 0.0) vis.w = OmniSlot(omni3, 3, pos, n, omniSizes.w, rot, face);
    return vis;
}
