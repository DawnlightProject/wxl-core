// wxl-graphics-shadow: the terrain's heightfield into the engine's sun shadow maps, vertex stage (vs_3_0).
// Vertices hold absolute world positions. Constants (terrain/Caster.cpp):
//   c0..c3  rows of the light view (camera-relative world -> light view), as the engine built it
//   c4..c7  rows of the pass's projection (light view -> clip, D3D depth range)
//   c8      camera world position xyz, depth bias in yards (pushed along the light's travel)
//   c9      light travel direction xyz
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
#pragma once

float4 view0 : register(c0);
float4 view1 : register(c1);
float4 view2 : register(c2);
float4 view3 : register(c3);
float4 proj0 : register(c4);
float4 proj1 : register(c5);
float4 proj2 : register(c6);
float4 proj3 : register(c7);
float4 camera : register(c8);
float4 lightDir : register(c9);

struct Output
{
    float4 clip : POSITION;
    float3 lightView : TEXCOORD1;   // what the engine's caster shader writes as depth
};

Output main(float3 world : POSITION)
{
    Output o;
    float3 r = world - camera.xyz + lightDir.xyz * camera.w;
    float4 v = r.x * view0 + r.y * view1 + r.z * view2 + view3;
    o.clip = v.x * proj0 + v.y * proj1 + v.z * proj2 + v.w * proj3;
    o.lightView = v.xyz;
    return o;
}
