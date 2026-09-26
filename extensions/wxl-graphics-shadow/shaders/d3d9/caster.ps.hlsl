// wxl-graphics-shadow: the terrain's heightfield into the engine's sun shadow maps, pixel stage (ps_3_0).
// The engine's ShadowMapSL writes max(lightView.z * c0.w, 0) with c0.w = 1 / 4000; the same here, so the
// terrain compares with the WMO and doodad casters already in the map.
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

float4 scale : register(c0);

float4 main(float3 lightView : TEXCOORD1) : COLOR0
{
    float d = max(lightView.z * scale.w, 0.0);
    return float4(d, d, 0.0, 1.0);
}
