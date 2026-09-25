// wxl-forever fog, froxel pass: visibility.
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
#define HORIZON_VOLUME_SLOT s1
#define HORIZON_SKY_SLOT s2
#define HORIZON_HEIGHT_SLOT s3
#define HORIZON_REG_C c178
#define HORIZON_REG_D c179
#include "terrain/shaders/horizon.hlsli"
#include "fog/shaders/ground.hlsli"
#include "lights/shaders/lights.hlsli"

// Visibility, one pixel per froxel at the jittered point injection samples: r how much of the sun
// or moon reaches it past the world, g how much sky it sees, b 1 inside an interior box plus 2
// when the camera cannot see the froxel (it lies behind a surface) plus 4 * (its room + 1) (the
// smallest of the light service's rooms holding it, 0 for none), a the soft indoor share that
// gates the lights (0 outdoors, 1 indoors, a ramp across a margin at each box's boundary). The
// per-froxel offsets rotate every frame like the injection's, so the resolve averages them.
// What lies off screen, from the terrain: the baked horizon maps (terrain/Horizon, 400 yd around every
// point, 16 azimuths) where resident, else a few steps over the heightfield around the camera.

// How much of the sun or moon along L reaches p (camera-relative) past the terrain: the horizon
// map's visibility in L's azimuth, then the heightfield's steps within its own window where no map
// is resident.
float TerrainSunVisibility(float3 p, float3 L, float start, float soft)
{
    bool mapped;
    HorizonTileCoord(cam.xyz + p, mapped);
    if (mapped) return TerrainSunVisibility(cam.xyz + p, L);
    if (terrainC.w < 0.5) return 1.0;
    float vis = 1.0;
    float scale = 0.75 + 0.5 * start;
    for (int j = 0; j < 4; ++j)
    {
        float t = (5.0 + 55.0 * j * j / 9.0) * scale;
        float3 q = p + L * t;
        float3 g = Ground(q);
        vis *= 1.0 - g.z * saturate((g.x - (cam.z + q.z) - 0.5) / (0.5 + soft * 3.0));
    }
    return vis;
}

// The share of the sky the terrain leaves visible from p: the horizon map's cosine-weighted sky
// share where resident, one fetch; else the horizon the heightfield raises in four directions
// (valley walls, hollows, the buildings the ground traces hit), turned per froxel and per frame.
float TerrainSkyVisibility(float3 p, float turn)
{
    bool mapped;
    HorizonTileCoord(cam.xyz + p, mapped);
    if (mapped) return TerrainSkyOcclusion(cam.xyz + p);
    if (terrainC.w < 0.5) return 1.0;
    float z = cam.z + p.z;
    float blocked = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        float a = turn + i * 1.5708;
        float len = lerp(10.0, 26.0, frac(i * 0.5) * 2.0);
        float3 g = Ground(p + float3(cos(a), sin(a), 0.0) * len);
        float rise = max(g.x - z, 0.0) / len;
        // A horizon raised by angle e hides sin^2(e) of a cosine-weighted sky.
        blocked += g.z * rise * rise / (1.0 + rise * rise);
    }
    return 1.0 - blocked * 0.25;
}

// Steps from r towards the light, growing with distance, each tested against the screen's depth;
// then the terrain, which also sees what is off screen: hills and valley walls at low sun. A
// froxel the camera cannot see (behind a surface) skips the screen, which would take that surface
// for its occluder and ring silhouettes with shadow.
float SunVisibility(float3 r, float dist, float3 o, float soft, bool seen)
{
    float3 L = normalize(shadowDir.xyz + (shadowT1.xyz * o.x + shadowT2.xyz * o.y) * (soft * 0.3));
    float ramp = 0.3 + soft * 2.0;
    float4 c0 = Project(r);
    float4 cL = float4(dot(L, vp0.xyz), dot(L, vp1.xyz), dot(L, vp2.xyz), dot(L, vp3.xyz));
    float rl = dot(r, L);
    float start = frac(o.z + 0.5);
    float vis = 1.0;
    for (int i = 0; i < 12; ++i)
    {
        if (!seen || (float)i >= shadowT1.w) break;
        float s = (i + start) / shadowT1.w;
        float t = 0.3 + 50.0 * s * s;
        float d = sqrt(max(dist * dist + 2.0 * t * rl + t * t, 0.0));
        vis *= 1.0 - Hidden(c0 + cL * t, d, ramp);
    }
    return vis * TerrainSunVisibility(r, L, start, soft);
}

// How much sky a froxel sees: the terrain's horizon around it, times what the screen shows above
// it (canopy, roofs) in four upward steps, rotated per froxel and per frame, averaged by the resolve.
float SkyVisibility(float3 r, float dist, float3 o, bool seen)
{
    float vis = TerrainSkyVisibility(r, (o.x + 0.5) * 1.5708);

    float3 U = normalize(float3(o.x * 0.8, o.y * 0.8, 1.0));
    float4 c0 = Project(r);
    float4 cU = float4(dot(U, vp0.xyz), dot(U, vp1.xyz), dot(U, vp2.xyz), dot(U, vp3.xyz));
    float ru = dot(r, U);
    float start = frac(o.z + 0.25);
    for (int j = 0; j < 4; ++j)
    {
        if (!seen) break;
        float s = (j + start) * 0.25;
        float t = 1.5 + 14.0 * s * s;
        float d = sqrt(max(dist * dist + 2.0 * t * ru + t * t, 0.0));
        vis *= 1.0 - 0.6 * Hidden(c0 + cU * t, d, 1.0);
    }
    return vis;
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 tile = floor(vpos / grid.xy);
    float k = tile.y * grid.w + tile.x;
    if (k >= grid.z) return float4(1, 1, 0, 1);
    float2 cell = vpos - tile * grid.xy;

    float3 o = FroxelOffset(cell, k);
    float dist = SliceToDist(k + 0.5 + o.z);
    float2 uv01 = (cell + 0.5 + o.xy) / grid.xy;
    float3 r = RayDir(uv01) * dist;

    float inside = IndoorDistance(r);
    float indoor = inside >= 0.0 ? 1.0 : 0.0;
    // Near the camera, cells in the camera's own state ease with it through a doorway (lightsC.w is
    // how far its eased indoorness lags); cells of the other kind, seen through the door, keep theirs.
    indoor = saturate(indoor - lightsC.w * (1.0 - smoothstep(12.0, 30.0, dist)));
    float softIndoor = saturate(0.5 + inside / max(lightsC.z, 0.01));
    float4 Q0 = lerp(outQ[0], inQ[0], indoor);
    bool doSun = shadowT1.w > 0.5 && indoor < 0.999 && Q0.x > 0.0;
    bool doSky = shadowT2.w > 0.5 && Q0.z > 0.0;
    bool seen = true;
    if (doSun || doSky)
    {
        bool open;
        float surf = SurfaceDist(tex2Dlod(depthTex, float4(uv01, 0, 0)).r, uv01, open);
        seen = open || dist < surf + 1.0 + 0.03 * dist;
    }
    float sun = doSun ? SunVisibility(r, dist, o, Q0.y, seen) : 1.0;
    float skyVis = doSky ? SkyVisibility(r, dist, o, seen) : 1.0;
    // The froxel's room, for the lamps' room gate (counts.w < 1 when rooms gate).
    float room = -1.0;
    if (counts.w < 1.0 && inside >= 0.0)
        [loop] for (int b = 0; b < 12; ++b)
        {
            if ((float)b >= cam.w) break;
            if (InRoom(r, boxes[b * 3], boxes[b * 3 + 1], boxes[b * 3 + 2]) > 0.5) { room = (float)b; break; }
        }
    return float4(sun, skyVis, indoor + (seen ? 0.0 : 2.0) + 4.0 * (room + 1.0), softIndoor);
}
