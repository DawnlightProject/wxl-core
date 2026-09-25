// wxl-forever fog, froxel pass: apply.
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
#include "fog/shaders/ground.hlsli"

// The composite: every pixel reads the one volume at its distance; past the volume's far distance
// the same medium is carried on analytically from the last slice, and the immersion veil is the
// volume's own light at the camera. No layer here has a model of its own.
sampler2D injected : register(s1);
sampler2D integrated : register(s2);
sampler3D noiseTex : register(s3);
sampler2D immersionTex : register(s4);
sampler2D sceneTex : register(s8);

float3 Encode(float3 c) { return pow(max(c, 0.0), 1.0 / 2.2); }

// Integrated slice k holds everything up to its far boundary, so a distance between boundaries k
// and k + 1 reads between slices k - 1 and k; before the first boundary nothing has accumulated.
float4 ReadIntegrated(float2 uv01, float boundary)
{
    float b = clamp(boundary, 0.0, grid.z);
    float k = floor(b);
    float4 hi = k >= grid.z ? SampleSlice(integrated, uv01, grid.z - 1.0) : SampleSlice(integrated, uv01, k);
    float4 lo = k < 1.0 ? float4(0, 0, 0, 1) : SampleSlice(integrated, uv01, k - 1.0);
    return k >= grid.z ? hi : lerp(lo, hi, b - k);
}

float4 main(float2 vpos : VPOS) : COLOR0
{
    float2 uv = (vpos + 0.5) * screen.xy;

    // The raw per-pixel offsets, the three blue-noise channels the dither and the froxels read:
    // fine grain that changes every frame; any lattice would show here.
    if (screen.w > 19.5 && screen.w < 20.5) return float4(Blue(vpos).rgb, 1.0);

    if ((screen.w > 4.5 && screen.w < 5.5) || (screen.w > 8.5 && screen.w < 9.5))
    {
        float4 v = tex2Dlod(injected, float4(uv, 0, 0));
        return float4(saturate(screen.w > 8.5 ? v.rgb : v.rgb * 20.0), 1.0);
    }

    bool sky;
    float rawDepth = tex2Dlod(depthTex, float4(uv, 0, 0)).r;
    float dist = SurfaceDist(rawDepth, uv, sky);
    float3 view = RayDir(uv);

    // The depth views the whole volume rests on, and where the indoor boxes fall.
    if (screen.w > 13.5 && screen.w < 16.5)
    {
        if (screen.w < 14.5)
        {
            float g = sky ? 1.0 : saturate(dist / realismA.w);
            return float4(g, g, g, 1.0);
        }
        if (screen.w < 15.5)
            return float4(saturate((rawDepth - 0.9) * 10.0), saturate((rawDepth - 0.99) * 100.0), rawDepth, 1.0);
        float shade = sky ? 0.25 : 1.0 - 0.75 * saturate(dist / realismA.w);
        float3 tint = (!sky && IndoorAt(view * dist) > 0.5) ? float3(1.0, 0.3, 0.2) : float3(0.2, 0.4, 1.0);
        return float4(tint * shade, 1.0);
    }

    float4 fog = ReadIntegrated(uv, DistToSlice(dist));

    // Visibility views: the fog's mean light along the ray, where the injection wrote the visibility
    // as its light; clear air shows dark blue.
    if ((screen.w > 16.5 && screen.w < 19.5) || screen.w > 20.5)
    {
        float opacity = 1.0 - fog.a;
        float3 mean = saturate(fog.rgb / max(opacity, 0.0001));
        return float4(lerp(float3(0.05, 0.08, 0.2), mean, saturate(opacity * 6.0)), 1.0);
    }

    if (screen.w > 12.5)
    {
        float3 g = Ground(view * dist);
        float valley = saturate((g.y - g.x) / 6.0);
        float above = saturate((cam.z + view.z * dist - g.x) / 10.0);
        return float4(valley, above * g.z, g.z * 0.3, 1.0);
    }

    // Contact: a thin extra layer right at the surface, as dense as the fog there, so geometry sits
    // in the fog instead of standing behind it.
    if (!sky && contactC.z > 0.5)
    {
        float4 v = SampleVolume(injected, uv, DistToSlice(dist));
        float e = exp(-contactC.x * v.a * contactC.y);
        fog.rgb += fog.a * (v.rgb / max(v.a, 0.000001)) * (1.0 - e);
        fog.a *= e;
    }

    // Past the far distance the medium goes on: the last slice's own extinction and light along
    // this ray, thinning upwards with the outdoor profile's height falloff from the height the
    // slice reached, over what remains to the surface, or over hazeC.y yards for the sky. The
    // horizon haze is that continuation, so the sky and the ground beyond the volume take the same
    // fog as the volume's own edge and nothing meets at a seam.
    float far = frame2.x;
    if (hazeC.x > 0.0 && (sky || dist > far))
    {
        float4 last = SampleSlice(injected, uv, grid.z - 1.0);
        float sigma = last.a * hazeC.x;
        float beyond = sky ? hazeC.y : min(dist - far, hazeC.y);
        float kUp = max(outP[0].y, 0.0005) * max(view.z, 0.0);
        float length = kUp > 0.00001 ? (1.0 - exp(-kUp * beyond)) / kUp : beyond;
        float t = exp(-sigma * length);
        fog.rgb += fog.a * (last.rgb / max(last.a, 0.000001)) * (1.0 - t);
        fog.a *= t;
    }

    // Exposure, then a soft shoulder above 1 that levels off at the white point.
    float3 scatter = fog.rgb * moved.w;
    float white = jitter.w;
    if (white > 1.001)
    {
        float lum = dot(scatter, float3(0.299, 0.587, 0.114));
        if (lum > 1.0)
        {
            float over = lum - 1.0;
            float mapped = 1.0 + over / (1.0 + over / (white - 1.0));
            scatter *= mapped / lum;
        }
    }

    // Chromatic extinction: each channel's transmittance is the grey one raised to its extinction
    // ratio, exact while the ratio is the same along the ray, and the in-scatter follows it.
    float3 trans = pow(max(fog.aaa, 0.00001), realismA.rgb);
    scatter *= (1.0 - trans) / max(1.0 - fog.a, 0.0001);

    if (screen.w > 5.5 && screen.w < 6.5) return float4((1.0 - fog.aaa), 1.0);
    if ((screen.w > 6.5 && screen.w < 7.5) || (screen.w > 10.5 && screen.w < 11.5)) return float4(Encode(saturate(scatter)), 1.0);
    if (screen.w > 7.5 && screen.w < 8.5) return float4(trans, 1.0);

    // Scene * transmittance + in-scatter, in linear light, then a little noise so smooth
    // fog gradients do not band in 8 bits.
    float3 scene = tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb;
    scene = pow(max(scene, 0.0), 2.2);

    // Standing in fog or smoke (the volume's density at the camera, immersion.ps): bright lights
    // soften into a glow, then a veil of the fog's own light there thickens towards the screen's
    // edges. The centre keeps little of it: the volume itself already fogs the first yards.
    float4 immersed = tex2Dlod(immersionTex, float4(0.5, 0.5, 0, 0));
    float immersion = immersed.a * immersionC.z;
    float veil = 0.0;
    if (immersion > 0.002)
    {
        if (immersionC.w > 0.0)
        {
            float2 reach = screen.xy * (2.0 + 10.0 * immersion);
            float3 glow = pow(max(tex2Dlod(sceneTex, float4(uv + float2(reach.x, 0.0), 0, 0)).rgb, 0.0), 2.2);
            glow += pow(max(tex2Dlod(sceneTex, float4(uv - float2(reach.x, 0.0), 0, 0)).rgb, 0.0), 2.2);
            glow += pow(max(tex2Dlod(sceneTex, float4(uv + float2(0.0, reach.y), 0, 0)).rgb, 0.0), 2.2);
            glow += pow(max(tex2Dlod(sceneTex, float4(uv - float2(0.0, reach.y), 0, 0)).rgb, 0.0), 2.2);
            scene = max(scene, lerp(scene, glow * 0.25, immersionC.w * immersion));
        }
        float2 centred = uv * 2.0 - 1.0;
        veil = immersion * (0.2 + 0.8 * saturate(dot(centred, centred) * 0.5)) * 0.6;
    }
    // Aerial perspective: what lies behind the fog loses colour with the fog in front of it.
    scene = lerp(scene, dot(scene, float3(0.2126, 0.7152, 0.0722)).xxx, realismB.w * (1.0 - fog.a));
    float3 composite = scene * trans + scatter;
    if (veil > 0.0) composite = lerp(composite, immersed.rgb * moved.w, veil);
    float3 result = Encode(composite);
    result += (Blue(vpos).b - 0.5) * (realismB.y / 255.0);
    return float4(result, 1.0);
}
