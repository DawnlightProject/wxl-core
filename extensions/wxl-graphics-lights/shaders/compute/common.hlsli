// wxl-graphics-lights: the constant buffer, the shared samplers, the push constants and the helpers every
// compute pass includes.
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

#ifndef LIGHTS_COMMON_HLSLI
#define LIGHTS_COMMON_HLSLI

#include "shared.h"

#define LIGHTS_DECL1(name) float4 name;
#define LIGHTS_DECLN(name, n) float4 name[n];
[[vk::binding(LIGHTS_B_CONSTANTS, 0)]] cbuffer LightsConstants
{
    LIGHTS_CONSTANTS(LIGHTS_DECL1, LIGHTS_DECLN)
};
#undef LIGHTS_DECL1
#undef LIGHTS_DECLN

[[vk::binding(LIGHTS_B_SAMP_POINT_CLAMP, 0)]]  SamplerState sPointClamp;
[[vk::binding(LIGHTS_B_SAMP_LINEAR_CLAMP, 0)]] SamplerState sLinearClamp;

struct LightsPush
{
    uint4  a;
    float4 b;
};
[[vk::push_constant]] LightsPush pc;

#define LIGHTS_TEX(slot) [[vk::binding(LIGHTS_B_TEX0 + (slot), 0)]]
#define LIGHTS_OUT(slot) [[vk::binding(LIGHTS_B_OUT0 + (slot), 0)]]

static const float kPi = 3.14159265;

bool Isolated(uint bit) { return (asuint(debug.y) & bit) != 0u; }
int  View() { return int(debug.x + 0.5); }

float Luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// v * M for a matrix handed over as its four columns (the dp4 form).
float4 Mul4(float4 v, float4 c0, float4 c1, float4 c2, float4 c3)
{
    return float4(dot(v, c0), dot(v, c1), dot(v, c2), dot(v, c3));
}

float4 ToClip(float3 r) { return Mul4(float4(r, 1.0), viewProj[0], viewProj[1], viewProj[2], viewProj[3]); }

// Screen uv (0..1, y down) and the projection's depth (0..1) -> camera-relative point.
float3 FromScreen(float2 uv, float ndcZ)
{
    float4 h = Mul4(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, ndcZ, 1.0), invViewProj[0], invViewProj[1], invViewProj[2],
                    invViewProj[3]);
    return h.xyz / h.w;
}

// The unit direction from the eye through screen uv.
float3 RayAt(float2 uv)
{
    float3 far = FromScreen(uv, 1.0);
    return normalize(far);
}

// The INTZ value -> the projection's 0..1 depth (the world is drawn into depthRange.x..z).
float DepthToNdc(float d) { return saturate((d - depthRange.x) * depthRange.y); }
bool  DepthIsSky(float d) { return d > depthRange.z + 0.00001; }

// The engine's view-space normal (RT1) turned into world axes.
float3 ViewToWorld(float3 n)
{
    return float3(dot(viewRows[0].xyz, n), dot(viewRows[1].xyz, n), dot(viewRows[2].xyz, n));
}

// The material code of RT2's alpha: kind (0 none, 1 model, 2 building, 3 terrain) and gloss.
float MaterialKind(float v) { return floor(v / 64.0); }
float MaterialGloss(float v) { return (v - 64.0 * floor(v / 64.0)) / 63.0; }

// The engine's gamma working space, as the composite decodes it.
float3 DecodeGamma(float3 c) { return pow(max(c, 0.0), 2.2); }

// --- rooms (wxl-graphics-lights' Rooms.cpp: 3 rows per box, padded by 0.3 yd) -----------------------------
//
// A point's rooms are soft, never a flip at a box face: a WMO group's box is only an approximation of the
// room (it cuts through doorways, open halls, the ground around a hut), so a hard in-or-out test drew its
// straight edges and rectangles on the floor. Each room gets a membership 0..1:
//   - horizontally, it ramps up across the box's padding, so a point at the group's own bounds is fully
//     in and the membership eases to nothing over the 0.3 yd outside them;
//   - vertically, floors and ceilings stay sharp (they are real surfaces, never seen as a line);
//   - a surface near a box face and facing out of the box is the outside of that wall: it is not in it;
//   - the room's own fade (joining or leaving the working set) scales it.

static const float kRoomPad  = 0.3;    // Rooms.hpp kPad
static const float kRoomWall = 0.8;    // yards inside a padded face where an outward-facing surface is a wall's outside

float RoomWeight(float b)
{
    if (b < -0.5) return 0.0;
    int i = int(b + 0.5);
    float4 row = roomWeights[i >> 2];
    int k = i & 3;
    return k == 0 ? row.x : (k == 1 ? row.y : (k == 2 ? row.z : row.w));
}

// How much a camera-relative point belongs to room b; n its surface normal, or zero for a point in the air.
float RoomMembership(float3 r, float3 n, int b)
{
    float4 x = rooms[b * 3], y = rooms[b * 3 + 1], z = rooms[b * 3 + 2];
    float4 p = float4(r, 1.0);
    float qx = dot(p, x), qy = dot(p, y);
    if (abs(qx) >= 1.0 || abs(qy) >= 1.0 || z.z == 0.0) return 0.0;
    float base = r.x * z.x + r.y * z.y + z.w;
    float a = (-1.0 - base) / z.z, c = (1.0 - base) / z.z;
    if (r.z < min(a, c) + 0.2 || r.z > max(a, c) - 0.2) return 0.0;
    // Yards inside the padded box along its two horizontal axes (each row's length is 1 / its half size).
    float lx = max(length(x.xyz), 1e-6), ly = max(length(y.xyz), 1e-6);
    float dx = (1.0 - abs(qx)) / lx, dy = (1.0 - abs(qy)) / ly;
    float m = saturate(min(dx, dy) / kRoomPad);
    if (dot(n, n) > 0.5)
    {
        float outX = dx < kRoomWall ? saturate((dot(n, x.xyz / lx) * sign(qx) - 0.35) / 0.3) : 0.0;
        float outY = dy < kRoomWall ? saturate((dot(n, y.xyz / ly) * sign(qy) - 0.35) / 0.3) : 0.0;
        m *= 1.0 - max(outX, outY);
    }
    return m * RoomWeight(float(b));
}

struct RoomSet
{
    float m[LIGHTS_MAX_ROOMS];   // each room's membership
    float indoor;                // the largest: how much the point is inside any room
    float main;                  // the room holding it most (-1 none), for the debug view
};

RoomSet RoomsAt(float3 r, float3 n, bool never)
{
    RoomSet s;
    s.indoor = 0.0;
    s.main = -1.0;
    int count = int(roomInfo.x + 0.5);
    [unroll] for (int b = 0; b < LIGHTS_MAX_ROOMS; ++b)
    {
        float m = (never || b >= count) ? 0.0 : RoomMembership(r, n, b);
        s.m[b] = m;
        if (m > s.indoor) { s.indoor = m; s.main = float(b); }
    }
    return s;
}

// The largest membership among the rooms of a mask (bit b: room b).
float RoomsIn(RoomSet s, uint mask)
{
    float best = 0.0;
    [unroll] for (int b = 0; b < LIGHTS_MAX_ROOMS; ++b)
        if ((mask >> uint(b)) & 1u) best = max(best, s.m[b]);
    return best;
}

// A light's shadow slot this frame (-1 none), from the per-list-index table.
float SlotOf(uint index)
{
    float4 row = slots[min(index, uint(LIGHTS_MAX_LIGHTS - 1)) >> 2];
    uint k = index & 3u;
    return k == 0u ? row.x : (k == 1u ? row.y : (k == 2u ? row.z : row.w));
}

// --- the field's grid ---------------------------------------------------------------------------------

// Froxel texture coordinates (0..1) of a camera-relative point: screen uv, exponential depth.
float3 FieldUvw(float3 r)
{
    float4 c = ToClip(r);
    float iw = 1.0 / max(abs(c.w), 1e-5);
    float2 uv = float2(c.x * iw * 0.5 + 0.5, 0.5 - c.y * iw * 0.5);
    float dist = length(r);
    float z = log(max(dist, fieldD.x) / fieldD.x) * fieldD.y;
    return float3(uv, z);
}

// The distance at a depth coordinate of the grid (0..1).
float FieldDistance(float z01) { return fieldD.x * exp(z01 / fieldD.y); }

#endif
