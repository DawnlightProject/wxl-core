// wxl-forever lights: reading the light service's textures (lights/Clusters.cpp) from a shader.
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

#ifndef WXL_FOREVER_LIGHTS_HLSLI
#define WXL_FOREVER_LIGHTS_HLSLI

// The caller binds both textures point-sampled and passes (lights::ClusterConstants):
//   clusterC  clusters across, down, deep, cluster texture width (texels)
//   clusterD  1 / cluster texture width, 1 / cluster texture height, near distance, 1 / log(far / near)
// The cluster texture (lights/Clusters.cpp, A32B32G32R32F, every value exact) holds one texel per
// cluster (its list's offset and length), the cookie rows, then the pool of light indices, four to a
// texel. Every light reaching a cluster is in its list; no list is cut.
// Lights are kLightCapacity columns of kLightRows texels: (camera-relative position, radius,
// negative when interior) (linear colour * intensity, cosCone) (spot axis, or a tube's half length,
// or for a point light the fog halo's reach and reference radius, 0 when it takes the light's own;
// reference radius) (source size in yards, profile, 1 = tube, flags: 1 an engine model light, + 2
// carried by another model, + 4 times the rooms it may light, RoomGate). Raising the capacity is
// lights::kMaxLights and the constant below, together.
static const float kLightCapacity = 128.0;
static const float kLightRows = 4.0;      // lights::kLightRows
static const float kRoomPad = 0.3;        // lights::rooms::kPad

// Rows of the cluster texture: the per-cluster texels, then the cookie rows, then the pool.
float ClusterGridRows(float4 clusterC) { return ceil(clusterC.x * clusterC.y * clusterC.z / clusterC.w); }
float ClusterPoolStart(float4 clusterC) { return ClusterGridRows(clusterC) + 2.0 * ceil(kLightCapacity / clusterC.w); }

// The texel n of the cluster texture counted from row firstRow, row by row.
float4 ClusterTexel(sampler2D clusters, float n, float firstRow, float4 clusterC, float4 clusterD)
{
    float row = floor((n + 0.5) / clusterC.w);
    float col = n - row * clusterC.w;
    return tex2Dlod(clusters, float4((col + 0.5) * clusterD.x, (firstRow + row + 0.5) * clusterD.y, 0, 0));
}

// The list of the cluster holding a point: across and down from its place on screen (0..1), deep
// from its distance to the camera on the service's exponential slices. Returns (offset, length).
float2 LightList(sampler2D clusters, float2 uv01, float dist, float4 clusterC, float4 clusterD)
{
    float depth01 = log(max(dist, clusterD.z) / clusterD.z) * clusterD.w;
    float3 c = clamp(floor(float3(uv01, depth01) * clusterC.xyz), 0.0, clusterC.xyz - 1.0);
    float id = c.x + c.y * clusterC.x + c.z * clusterC.x * clusterC.y;
    return floor(ClusterTexel(clusters, id, 0.0, clusterC, clusterD).rg + 0.5);
}

// The light index at entry offset + i of a list (i below its length).
float LightAt(sampler2D clusters, float entry, float4 clusterC, float4 clusterD)
{
    float texel = floor(entry * 0.25);
    float4 q = ClusterTexel(clusters, texel, ClusterPoolStart(clusterC), clusterC, clusterD);
    float m = entry - texel * 4.0;
    return floor((m < 0.5 ? q.r : (m < 1.5 ? q.g : (m < 2.5 ? q.b : q.a))) + 0.5);
}

// Which omni slot a light holds (1..4, 0 none) and the share of that slot's shadow, from the light
// service's omni table (lights::PublishOmni, row 0): the one place every pass reads it.
float2 OmniSlotOf(sampler2D omniTable, float index)
{
    return tex2Dlod(omniTable, float4((index + 0.5) / kLightCapacity, 0.25, 0, 0)).rg;
}

void ReadLight(sampler2D lightData, float index, out float4 a, out float4 b, out float4 c)
{
    float u = (index + 0.5) / kLightCapacity;
    a = tex2Dlod(lightData, float4(u, 0.5 / kLightRows, 0, 0));
    b = tex2Dlod(lightData, float4(u, 1.5 / kLightRows, 0, 0));
    c = tex2Dlod(lightData, float4(u, 2.5 / kLightRows, 0, 0));
}

// The fourth row: source size, profile, 1 = tube.
float4 ReadLightShape(sampler2D lightData, float index)
{
    return tex2Dlod(lightData, float4((index + 0.5) / kLightCapacity, 3.5 / kLightRows, 0, 0));
}

// The point of the light that lights p: the light itself, or for a tube the closest point of its
// length (the representative-point approximation of an area light).
float3 LightPoint(float3 p, float4 a, float4 c, float4 shape)
{
    if (shape.z < 0.5) return a.xyz;
    float t = clamp(dot(p - a.xyz, c.xyz) / max(dot(c.xyz, c.xyz), 0.0001), -1.0, 1.0);
    return a.xyz + c.xyz * t;
}

// A light's angular profile, fromLight the unit direction from the light to the point it lights:
// 1 a lantern's cage (six thin bars round the side, a darker cap), 2 a street lamp's hood (mostly
// down and out), 3 a flame (a little more up than down), 4 a window's grille. The angles' multiples
// come from Chebyshev identities of their cosines (cos 3a = 4c^3 - 3c, cos 4a = 2(2c^2 - 1)^2 - 1),
// the high powers from squaring: no atan2, asin or pow.
float LightProfile(float profile, float3 fromLight)
{
    if (profile < 0.5) return 1.0;
    if (profile > 1.5 && profile < 2.5) return 0.3 + 0.7 * saturate(0.3 - fromLight.z * 1.2);
    if (profile > 2.5 && profile < 3.5) return 0.8 + 0.2 * saturate(fromLight.z + 0.5);
    float rho = length(fromLight.xy);
    float c = fromLight.x / max(rho, 0.0001);
    if (profile < 1.5)
    {
        float c3 = c * (4.0 * c * c - 3.0);
        float p2 = c3 * c3, p8 = p2 * p2;
        p8 *= p8;
        float bars = p8 * p8 * p8;   // |cos 3a|^24
        return lerp(1.0, 0.3, bars) * (fromLight.z > 0.75 ? 0.6 : 1.0);
    }
    float c2 = 2.0 * c * c - 1.0;
    float c4 = 2.0 * c2 * c2 - 1.0;          // cos 4a
    float e2 = 2.0 * rho * rho - 1.0;         // cos 2e (rho = cos e)
    float e4 = 2.0 * e2 * e2 - 1.0;
    float e8 = 2.0 * e4 * e4 - 1.0;           // cos 8e
    float2 g = float2(c4, e8);
    g *= g;                                   // ^2
    float2 g4 = g * g;                        // ^4
    float2 g16 = g4 * g4;
    g16 *= g16;                               // ^16
    float2 g20 = g16 * g4;
    return 1.0 - 0.5 * max(g20.x, g20.y);
}

// A light's strength at squared distance d2, as a share of its colour: a soft-core inverse square,
// 1 / (d^2 + r0^2) with r0 half the reference radius, normalised to 1 at the reference radius
// (at least a quarter of the radius), and windowed smoothly to zero at the radius; soft 0 keeps
// that inverse square, soft 1 an inverse distance past the core, so light reaches further before
// the window takes it to zero. core widens the soft core to at least that many yards (a carried
// light) without changing the far falloff.
float LightFalloffSoft(float d2, float radius, float reference, float soft, float core)
{
    float r = max(reference, 0.25 * radius);
    float r2 = r * r;
    float d = sqrt(d2);
    float x = d2 / (radius * radius);
    float window = saturate(1.0 - x * x);
    float r0 = max(0.5 * r, core);
    float squared = (r2 * 1.25) / (d2 + r0 * r0);
    float gentle = (r * 1.25) / (d + r0 * 0.5);
    return lerp(squared, gentle, soft) * window * window;
}

// LightFalloff with the soft core widened to at least core yards (a carried light).
float LightFalloffCore(float d2, float radius, float reference, float core)
{
    float r = max(reference, 0.25 * radius);
    float r0 = max(0.5 * r, core);
    float x = d2 / (radius * radius);
    float window = saturate(1.0 - x * x);
    return (r * r * 1.25) / (d2 + r0 * r0) * window * window;
}

// The same falloff with a core as wide as the reference radius: the softer, wider term multiple
// scattering spreads a halo with.
float LightFalloffWide(float d2, float radius, float reference)
{
    float r = max(reference, 0.25 * radius);
    float r2 = r * r;
    float x = d2 / (radius * radius);
    float window = saturate(1.0 - x * x);
    return (r2 * 2.0) / (d2 + r2) * window * window;
}

// A flame's light runs hotter near its source: within the reference radius the colour moves towards
// a brighter, whiter tint by share hot, so a torch's heart reads yellow-white and its falloff deep
// orange. The near cap keeps the brighter core from burning out.
float3 LightHot(float3 rgb, float d, float reference, float hot)
{
    if (hot <= 0.0) return rgb;
    float core = saturate(1.0 - d / max(reference, 0.05));
    float l = dot(rgb, float3(0.299, 0.587, 0.114));
    float3 white = lerp(rgb, l.xxx, 0.45) * 1.15;   // warmer and dimmer than a white-hot core: no burnt spot
    return lerp(rgb, white, core * core * hot);
}

// A spot's cone: 1 inside, a quick fade at the edge; points (cosCone <= -1) light everywhere.
float LightCone(float3 fromLight, float3 axis, float cosCone)
{
    return cosCone <= -1.0 ? 1.0 : saturate((dot(fromLight, axis) - cosCone) / max(1.0 - cosCone, 0.001) * 4.0);
}

// 1 when the camera-relative point r lies in one room box (lights::rooms, three rows into the unit
// cube, padded by kRoomPad), else 0. Level with the floor or ceiling counts only within a tenth of a
// yard, so the underside of a floor slab belongs to the room below.
float InRoom(float3 r, float4 x, float4 y, float4 z)
{
    float4 p = float4(r, 1.0);
    if (abs(dot(p, x)) > 1.0 || abs(dot(p, y)) > 1.0 || z.z == 0.0) return 0.0;
    float base = r.x * z.x + r.y * z.y + z.w;
    float a = (-1.0 - base) / z.z, c = (1.0 - base) / z.z;
    return r.z >= min(a, c) + (kRoomPad - 0.1) && r.z <= max(a, c) - (kRoomPad - 0.1) ? 1.0 : 0.0;
}

// How much of a light reaches a point in room (its index, -1 outside every room): all of it where
// the light may light that room, leak where a floor or ceiling lies between. flags is the light's
// fourth row w: bit b of floor(flags / 4) is set for the rooms it may light (lights::Publish: its
// height within the room's, and the room's floor within its own room's height; the same room, a
// hall and its gallery, rooms side by side). No bit at all: an outdoor light, it passes.
float RoomGate(float flags, float room, float leak)
{
    float rooms = floor(flags * 0.25);
    if (leak >= 1.0 || room < -0.5 || rooms < 0.5) return 1.0;
    return lerp(leak, 1.0, fmod(floor(rooms / exp2(room)), 2.0));
}

#endif
