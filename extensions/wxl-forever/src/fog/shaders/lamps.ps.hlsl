// wxl-forever fog, froxel pass: lamps.
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

#include "fog/shaders/common.hlsli"
#include "lights/shaders/cookies.hlsli"

// The lamps' light in the fog, one pixel per froxel at the point injection samples, before the
// injection: a pass of its own, so its loop has the whole register file and samples every light's
// cookie itself (lights::cookies; no per-froxel limit). Out, without the extinction on the way
// (inject applies it from the froxel's density at the mean distance, lighter for the halo):
//   COLOR0  rgb the single scattering of every lamp reaching the froxel, a the lamps' distance to
//           it, weighted by their light
//   COLOR1  rgb their multiple-scattering halo, a the heat of flames around it (it thins the fog)

sampler2D visTex : register(s5);

float4 PRow(int i, float sel) { return lerp(outP[i], inP[i], sel); }

sampler2D lightTex : register(s8);
sampler2D clusterTex : register(s9);
// Which omni slot a light holds (the light service's omni table, lights::PublishOmni, read by
// OmniSlotOf: its slot and the share of the slot's shadow, fading as the slot changes hands) and,
// per froxel, how much of each slot's light reaches it (omni.ps.hlsl).
sampler2D omniTable : register(s4);
float4 omniFogC : register(c221);
sampler2D omniVisTex : register(s1);
// The light cookies (lights::cookies, shaders/cookies.hlsli): the atlas and its constants.
sampler2D cookieAtlas : register(s15);
float4 cookieC : register(c222);
float4 cookieD : register(c223);

// A light's angular shape and colour as seen from the froxel it lights: the baked cookie of the
// light's own model (cage, glass, bracket; LightCookie, sampled in the loop for every light that has
// one) times the analytic per-family profile; where a cookie exists the cage and grille profiles give
// way to it. The halo in the mist then carries the lantern's bars and its glass colour. A light no
// cookie shapes keeps a softer round halo (cookieTintC.y). has is whether the light carries a cookie.
float3 LightMask(float4 shape, float3 fromLight, float3 cookie, float has)
{
    // A flame is no point: its logs, bowl or grate hide far less of it than the baked point sees.
    if (shape.y > 2.5 && shape.y < 3.5) cookie = lerp(cookie, 1.0, cookieD.w);
    float profile = LightProfile(shape.y, fromLight);
    if (has > 0.5 && (shape.y == 1.0 || shape.y == 4.0)) profile = 1.0;
    return profile * (has > 0.5 ? cookie : cookieTintC.y);
}

// The lights of the froxel's cluster, from the light service. Each has a soft-core inverse-square
// falloff windowed to zero at its radius (lights.hlsli), the profile's phase towards the camera
// (brighter looking at the lamp), and one isotropic multiple-scattering octave with a wider core
// (inject gives it less extinction) that spreads the halo softly. Interior lights light the fog inside their boxes,
// fading across the margin, and stay on their own floor (RoomGate, by counts.w), and a light with
// an omni shadow map is shadowed by it (beams through the mist). Nothing on screen occludes a halo:
// what the camera sees in front of a lamp says nothing about the fog around it. A flame's light runs hotter near its
// source, and its heat thins the fog around it (heat, by heatStrength, a few source sizes wide).
// The lighting rows (phase, multiple scattering, heat, scatter and cap) are read at their use,
// blended by sel (the cell's indoor share), so no row lives across the loop. room is the froxel's
// room (the visibility pass finds it; -1 outside every room). Out: single and multi as COLOR0 and
// COLOR1 hold them.
void Lamps(float2 vpos, float3 r, float3 dir, float2 cell, float k, float softIndoor, float sel, float room,
           out float4 single, out float4 multi)
{
    single = 0.0;
    multi = 0.0;
    if (lightsC.x < 0.5) return;
    float2 list = LightList(clusterTex, (cell + 0.5) / grid.xy, length(r), clusterC, clusterD);
    // Most froxels lie in a cluster no light reaches: nothing more is read for them.
    [branch] if (list.y < 0.5) return;
    float4 omniVis = tex2Dlod(omniVisTex, float4((vpos + 0.5) * atlas.xy, 0, 0));
    float weight = 0.0, reach = 0.0, heat = 0.0;
    [loop] for (int i = 0; i < 128; ++i)
    {
        if ((float)i >= list.y) break;
        float index = LightAt(clusterTex, list.x + i, clusterC, clusterD);
        // Its cookie and its omni shadow first, from its position alone, while nothing else of it
        // is live: they need the most registers.
        float4 a = tex2Dlod(lightTex, float4((index + 0.5) / kLightCapacity, 0.5 / kLightRows, 0, 0));
        float3 away = r - a.xyz;
        if (dot(away, away) >= a.w * a.w) continue;
        float has;
        float3 shaped = LightCookie(clusterTex, cookieAtlas, index, normalize(away), clusterC, clusterD, cookieC, cookieD, cookieTintC, has);
        if (omniFogC.x > 0.5)
        {
            float2 slot = OmniSlotOf(omniTable, index);
            if (slot.x > 0.5) shaped *= lerp(1.0, dot(slot.x == float4(1.0, 2.0, 3.0, 4.0) ? 1.0 : 0.0, omniVis), slot.y);
        }
        float4 b, c;
        ReadLight(lightTex, index, a, b, c);
        float4 shape = ReadLightShape(lightTex, index);
        // A merged lamp's halo keeps its own reach (a point light's axis texels carry it); its
        // light on the ground keeps the larger one.
        if (c.x > 0.0 && b.w <= -1.0 && shape.z < 0.5) { a.w = a.w < 0.0 ? -c.x : c.x; c.w = c.y; }
        float3 toSample = r - LightPoint(r, a, c, shape);
        float d2 = dot(toSample, toSample);
        float radius = abs(a.w);
        if (d2 < radius * radius)
        {
            // Its shape towards this froxel, folded into its colour: cone, profile, and the cookie and
            // omni shadow read above (the omni map gives beams through the mist).
            float3 fromLight = toSample * rsqrt(max(d2, 0.000001));
            b.rgb *= LightCone(fromLight, c.xyz, b.w) * LightMask(shape, fromLight, shaped, has);
            float d = sqrt(d2);
            float3 toLight = -toSample / max(d, 0.001);
            bool flame = shape.y > 2.5 && shape.y < 3.5;
            if (flame) heat += PRow(11, sel).y * saturate(1.0 - d / (4.0 * max(shape.x, 0.05)));
            float gate = a.w < 0.0 ? softIndoor : 1.0 - softIndoor;
            if (room > -0.5) gate *= RoomGate(shape.w, room, counts.w);
            if (gate > 0.0)
            {
                // A carried light's core is wider, and no light passes the cap near its source. The
                // extinction on the way is inject's (it knows the froxel's density): left out here.
                float once = Phase(dot(dir, toLight), PRow(5, sel))
                           * min(LightFalloffCore(d2, radius, c.w, fmod(shape.w, 4.0) > 1.5 ? toSun.w : 0.0), toMoon.w);
                float wide = PRow(10, sel).w >= 0.5 ? PRow(10, sel).x * LightFalloffWide(d2, radius, c.w) : 0.0;
                float3 colour = LightHot(b.rgb, d, c.w, omniFogC.z * (flame ? 1.0 : 0.35)) * gate;
                single.rgb += colour * once;
                multi.rgb += colour * wide;
                float w = dot(colour, float3(0.299, 0.587, 0.114)) * (once + wide);
                weight += w;
                reach += w * d;
            }
        }
    }
    single.a = weight > 0.000001 ? reach / weight : 0.0;
    multi.a = heat;
}

struct Out
{
    float4 single : COLOR0;
    float4 multi : COLOR1;
};

Out main(float2 vpos : VPOS)
{
    Out o;
    o.single = 0.0;
    o.multi = 0.0;
    float2 tile = floor(vpos / grid.xy);
    float k = tile.y * grid.w + tile.x;
    if (k >= grid.z) return o;
    float2 cell = vpos - tile * grid.xy;
    float3 r = FroxelPoint(cell, k);
    float3 dir = r / max(length(r), 0.0001);
    // As inject reads it: indoor share, + 2 when unseen, + 4 * (the froxel's room + 1).
    float4 seen = tex2Dlod(visTex, float4((vpos + 0.5) * atlas.xy, 0, 0));
    float roomCode = floor(seen.b * 0.25);
    float state = seen.b - roomCode * 4.0;
    float indoor = state < 1.5 ? state : state - 2.0;
    Lamps(vpos, r, dir, cell, k, seen.a, indoor, roomCode - 1.0, o.single, o.multi);
    return o;
}
