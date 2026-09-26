// wxl-graphics-fog: the camera-frustum lamp grid (lamps only; the sun, moon and sky are evaluated per
// sample in the march). Per froxel:
//   rgb  the light every lamp of its cluster scatters towards the camera per unit of scattering:
//        falloff and cone, the family's profile, the baked cookie (cage bars, tinted glass), the omni
//        shadow maps (beams through the mist), a flame's hot core, the phase towards the camera, a
//        wider multiple-scattering halo, room gating (an interior lamp lights indoor air only), the
//        bodies near the camera shadowing each lamp from its froxel (capsules, where no omni map
//        holds them: a lamp without one, a carried torch), and
//        the medium around the froxel thinning the light over its way from the lamp: thick fog keeps
//        a halo tight, thin air lets it spread
//   a    the froxel's indoor share, eased from last frame's grid so a room that streams in fades
// Distances are exponential from lampGrid2.x to lampGrid2.z along each ray.
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

#include "lamps.hlsli"
#include "indoor.hlsli"
#include "clip.hlsli"

FOG_TEX(0) Texture3D<float4> history;
FOG_TEX(1) Texture3D<float2> stateTex;
FOG_TEX(2) Texture3D<float>  groundTex;

// Extinction per yard of the outdoor media at a world point: the finest level holding it, and the
// atmosphere's smooth profile (no patches). Indoors the dust is thin enough to leave out.
float MediumAround(float3 p)
{
    float ext = 0.0;
    [loop] for (uint L = 0u; L < uint(FOG_LEVELS); ++L)
    {
        if (LevelInside(L, p.xy) < 2.0) continue;
        float h = p.z - LevelGround(groundTex, L, p.xy);
        if (h < HMin(L) || h > HMax(L) - CellZ(L)) continue;
        float2 v = SampleLevel(stateTex, L, p.xy, h);
        ext = (v.x + v.y * outH.w) * march2.w;
        break;
    }
    return ext + airA.x * march2.w * exp(-max(p.z - airA.z - airB.w, 0.0) * airA.y);
}
FOG_OUT(0) [[vk::image_format("rgba16f")]] RWTexture3D<float4> outGrid;

// How much of a lamp at camera-relative l reaches camera-relative r past the bodies near the camera:
// vertical capsules, soft by how far behind a body the point lies. A body the lamp sits in (its
// carrier's hand is outside it) and the last few inches before the point are left out.
float BodyVisibility(float index, float3 l, float3 r)
{
    float vis = 1.0;
    float3 seg = r - l;
    float len = length(seg);
    if (len < 0.3) return 1.0;
    uint i4 = uint(index);
    uint bits = occInfo.z > 0.5 ? asuint(occMask[i4 >> 2u][i4 & 3u]) : (1u << uint(occInfo.x)) - 1u;
    [loop] while (bits != 0u)
    {
        uint i = firstbitlow(bits);
        bits &= bits - 1u;
        float4 o = occl[i];
        float radius = clamp(o.w * 0.17, 0.22, 1.2);
        float3 a = o.xyz - eye.xyz + float3(0.0, 0.0, radius);
        float3 axis = float3(0.0, 0.0, max(o.w - 2.0 * radius, 0.0));
        // The lamp inside the body: it is the carrier's own light source, not a shadow.
        float2 flat = l.xy - a.xy;
        if (dot(flat, flat) < radius * radius * 0.5 && l.z > a.z - radius && l.z < a.z + axis.z + radius) continue;
        // Closest points between the lamp-to-point segment and the body's axis.
        float3 w0 = l - a;
        float A = dot(seg, seg), B = dot(seg, axis), C = dot(axis, axis), D = dot(seg, w0), E = dot(axis, w0);
        float den = A * C - B * B;
        float t = den > 1e-5 ? saturate((B * E - C * D) / den) : 0.0;
        float u = C > 1e-5 ? saturate((B * t + E) / C) : 0.0;
        t = saturate((B * u - D) / A);
        float d = length(l + seg * t - (a + axis * u));
        float behind = (1.0 - t) * len;
        if (t * len < 0.3 || behind < 0.15) continue;
        float soft = 0.1 + 0.08 * behind;
        vis *= 1.0 - (1.0 - smoothstep(radius - soft, radius + soft, d));
    }
    return lerp(1.0, vis, occInfo.y);
}

float3 LightMask(float4 shape, float3 fromLight, float3 cookie, float has)
{
    if (shape.y > 2.5 && shape.y < 3.5) cookie = lerp(cookie, 1.0, cookieD.w);
    float profile = LightProfile(shape.y, fromLight);
    if (has > 0.5 && (shape.y == 1.0 || shape.y == 4.0)) profile = 1.0;
    return profile * (has > 0.5 ? cookie : 0.75);
}

float3 Lamps(float2 uv, float dist, float3 r, float3 view, float indoor, float room)
{
    float3 sum = 0.0;
    if (lights.x < 0.5 || Isolated(FOG_ISO_NO_LAMPS)) return sum;
    float2 list = LightList(uv, dist);
    [branch] if (list.y < 0.5) return sum;
    float around = MediumAround(r + eye.xyz) * (1.0 - indoor);
    float4 ph = lerp(phase, inD, indoor);
    float msShare = lerp(scatterMs.x, 0.5 * scatterMs.x, indoor);
    [loop] for (int i = 0; i < 128; ++i)
    {
        if (float(i) >= list.y) break;
        float index = LightAt(list.x + float(i));
        float4 a = LightRow(index, 0);
        float3 away = r - a.xyz;
        if (dot(away, away) >= a.w * a.w) continue;
        float has;
        float3 shaped = LightCookie(index, normalize(away), has);
        float2 slot = lights.z > 0.5 ? OmniSlotOf(index) : float2(0.0, 0.0);
        if (slot.x > 0.5) shaped *= lerp(1.0, OmniVisibility(slot.x - 1.0, r), slot.y);
        // Bodies shadow the lamps no map covers (and, as its map fades in, one that has).
        if (occInfo.x > 0.5) shaped *= lerp(BodyVisibility(index, a.xyz, r), 1.0, slot.x > 0.5 ? slot.y : 0.0);
        float4 b = LightRow(index, 1);
        float4 c = LightRow(index, 2);
        float4 shape = LightRow(index, 3);
        // A merged lamp's halo keeps its own reach (a point light's axis texels carry it).
        if (c.x > 0.0 && b.w <= -1.0 && shape.z < 0.5) { a.w = a.w < 0.0 ? -c.x : c.x; c.w = c.y; }
        float3 toSample = r - LightPoint(r, a, c, shape);
        float d2 = dot(toSample, toSample);
        float radius = abs(a.w);
        if (d2 >= radius * radius) continue;
        float gate = a.w < 0.0 ? indoor : 1.0 - indoor;
        if (room > -0.5) gate *= RoomGate(shape.w, room, roomInfo.y);
        if (gate <= 0.0) continue;
        float d = sqrt(d2);
        float3 fromLight = toSample / max(d, 0.001);
        float3 colour = b.rgb * LightCone(fromLight, c.xyz, b.w) * LightMask(shape, fromLight, shaped, has);
        bool flame = shape.y > 2.5 && shape.y < 3.5;
        bool carriedLight = fmod(shape.w, 4.0) > 1.5;
        // The way from the lamp: the direct light thins exponentially, the wide halo as a diffusion.
        float tau = around * d;
        float once = PhaseDual(dot(view, -fromLight), ph)
                   * min(LightFalloffCore(d2, radius, c.w, carriedLight ? carried.x : 0.0), carried.y) * exp(-tau);
        float wide = msShare * LightFalloffWide(d2, radius, c.w) / (1.0 + 0.75 * tau);
        sum += LightHot(colour, d, c.w, carried.z * (flame ? 1.0 : 0.35)) * gate * (once + wide);
    }
    // Each medium scales this by its own lamp strength when it samples the grid.
    return sum;
}

[numthreads(8, 8, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(float3(id) >= lampGrid.xyz)) return;
    float2 uv = (float2(id.xy) + 0.5) / lampGrid.xy;
    float dist = GridDistance((float(id.z) + 0.5) / lampGrid.z);
    float3 view = RayDir(uv);
    float3 r = view * dist;

    float room = -1.0;
    float indoor = Isolated(FOG_ISO_NO_INDOOR) ? 0.0 : IndoorWeight(r, room);
    if (temporal.z > 0.5 && lampGrid.w > 0.0)
    {
        float3 prev = ReprojectUv(r);
        float wPrev = GridW(length(r + eye.xyz - prevEye.xyz));
        if (prev.z > 0.0 && all(prev.xy >= 0.0) && all(prev.xy <= 1.0) && wPrev <= 1.0)
            indoor = lerp(indoor, history.SampleLevel(sLinearClamp, float3(prev.xy, wPrev), 0).a, lampGrid.w);
    }
    outGrid[id] = float4(Lamps(uv, dist, r, view, indoor, room), indoor);
}
