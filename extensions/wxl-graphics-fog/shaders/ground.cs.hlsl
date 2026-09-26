// wxl-graphics-fog: a level's ground map, the floor under each of its cells for the level's current
// window (toroidal like the level), from the floor's mip nearest one texel per cell. pc.a.x = the level.
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

#include "sim.hlsli"

FOG_TEX(0) Texture2D<float2> floorTex;
FOG_OUT(0) [[vk::image_format("r32f")]] RWTexture3D<float> outGround;   // N x N x FOG_LEVELS

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FOG_LEVEL_N)) return;
    uint L = pc.a.x;
    float2 xy = CellCentreXY(L, int2(id.xy), levelB[L].xy);
    outGround[int3(id.xy, L)] = FloorAt(floorTex, xy, GroundMip(L)).r;
}
