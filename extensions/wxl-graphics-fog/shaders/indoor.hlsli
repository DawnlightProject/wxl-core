// wxl-graphics-fog: indoor or outdoor at a point, from wxl-graphics-lights' rooms (the interior WMO
// groups around the camera as boxes, smallest first): the weight rises from 0 at a box's face to 1
// `seep` yards inside it, so the outdoor fog reaches that far through a doorway.
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

#ifndef FOG_INDOOR_HLSLI
#define FOG_INDOOR_HLSLI

#include "common.hlsli"

// Indoor share of a camera-relative point, and the smallest room holding it (-1 outside all).
float IndoorWeight(float3 r, out float room)
{
    room = -1.0;
    float best = 0.0;
    int n = min(int(roomInfo.x), FOG_MAX_ROOMS);
    float4 h = float4(r, 1.0);
    [loop] for (int i = 0; i < n; ++i)
    {
        float4 x = rooms[i * 3 + 0], y = rooms[i * 3 + 1], z = rooms[i * 3 + 2];
        float3 q = float3(dot(h, x), dot(h, y), dot(h, z));
        float3 len = float3(length(x.xyz), length(y.xyz), length(z.xyz));
        float3 inside = (1.0 - abs(q)) / max(len, 1e-4);
        float d = min(inside.x, min(inside.y, inside.z));
        if (d > 0.0 && room < 0.0) room = float(i);
        best = max(best, saturate(d / max(inD.w, 0.05)));
    }
    return best;
}

float IndoorWeight(float3 r)
{
    float room;
    return IndoorWeight(r, room);
}

// The froxel grid's distance mapping: exponential from lampGrid2.x to lampGrid2.z.
float GridW(float dist) { return log(max(dist, lampGrid2.x) / lampGrid2.x) * lampGrid2.y; }
float GridDistance(float w) { return lampGrid2.x * exp(w / lampGrid2.y); }

#endif
