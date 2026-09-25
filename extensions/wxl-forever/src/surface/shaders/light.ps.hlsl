// wxl-forever surface lighting: every light of its cluster summed into the light buffer.
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

#include "surface/shaders/shading.hlsli"

// Drawn at the lighting resolution (resC), each pixel standing for one full-resolution texel
// (PassUv). Out, linear, into the FP16 light buffer: the light reaching the surface (rgb, the
// irradiance a white surface facing it would reflect) and the soft specular it reflects towards the
// camera (a, a luminance; its colour is the light's). The buffer is cleared first, and pixels of the
// sky or of a cluster no light reaches are discarded, so the cost follows the lights overlapping a
// pixel. With movingC.w set, COLOR0 holds the light without the omni shadow maps and COLOR1 the
// share of it they let through, applied after the history so a moving light's shadow never lingers.
sampler2D lightTex : register(s8);
sampler2D clusterTex : register(s9);

sampler2D sceneTex : register(s2);
sampler2D roomTex : register(s5);   // shaders/room.ps.hlsl, while rooms gate
sampler2D omniVisTex : register(s6); // shaders/omni.ps.hlsl: each omni slot's share at this texel
sampler2D omniTable : register(s7);  // the light service's omni table: which slot each light holds
sampler2D cookieAtlas : register(s15);

// A light's angular shape and colour as seen from the point it lights: the baked cookie of the light's
// own model (the light service's atlas: its cage, glass and bracket as transmittance rgb; sampled in
// the loop for every light that has one) times the analytic per-family profile. Where a cookie exists
// the cage and grille profiles give way to it; the hood and flame profiles stay. Static, so it goes
// before the history. cookie and has are LightCookie's for this light.
float3 LightMask(float4 shape, float3 fromLight, float3 cookie, float has)
{
    // A flame is no point: its logs, bowl or grate hide far less of it than the baked point sees.
    if (shape.y > 2.5 && shape.y < 3.5) cookie = lerp(cookie, 1.0, cookieD.w);
    float profile = sourcesC.y < 0.5 ? LightProfile(shape.y, fromLight) : 1.0;
    if (has > 0.5 && (shape.y == 1.0 || shape.y == 4.0)) profile = 1.0;
    return profile * cookie;
}

// The light service's Cookie factor view: the product of the cookies of every light reaching p.
float CookieView(float3 p, float2 list)
{
    float product = 1.0;
    [loop] for (int i = 0; i < 128; ++i)
    {
        if ((float)i >= list.y) break;
        float index = LightAt(clusterTex, list.x + i, clusterC, clusterD);
        float4 a, b, c;
        ReadLight(lightTex, index, a, b, c);
        float3 toP = p - a.xyz;
        if (dot(toP, toP) >= a.w * a.w) continue;
        float has;
        product *= Luma(LightCookie(clusterTex, cookieAtlas, index, normalize(toP), clusterC, clusterD, cookieC, cookieD, cookieE, has));
    }
    return product;
}

// How much of a light reaches the surface point past what the screen shows right beside it: steps
// towards the light over shadowC.z yards from the surface (door frames, props, the terrain the
// engine never casts), each compared with the depth drawn there. The same short test for every
// light, with a shadow map or without: a longer march would take whatever the camera sees in front
// of a light for its occluder, and switch the light with the view. The rays aim at a blue-noise
// jittered point of a disc the size of the source and start at a jittered step, both rotated every
// frame; the history averages them into a penumbra that hardens towards the caster.
float ContactShadow(float3 p, float3 n, float3 lightPos, float4 jitter, float size)
{
    float3 start = p + n * (0.03 + 0.003 * length(p));
    float3 toLight = lightPos - start;
    float len = max(length(toLight), 0.001);
    float3 axis = toLight / len;
    float3 side = normalize(cross(axis, abs(axis.z) < 0.9 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0)));
    float3 up = cross(axis, side);
    float3 target = lightPos + (side * (jitter.x - 0.5) + up * (jitter.y - 0.5)) * (2.0 * size);
    float3 L = normalize(target - start);
    float reach = min(len - 0.3, shadowC.z);
    if (reach <= 0.0) return 1.0;
    float stepLen = reach / lightsC.y;
    float thick = 0.25 + stepLen;
    float vis = 1.0;
    // A real loop: unrolled, the compiler schedules every step's projection at once and runs out
    // of registers beside the cookie and the light loop's state.
    [loop] for (int i = 0; i < 6; ++i)
    {
        if ((float)i >= lightsC.y) break;
        float t = (i + jitter.z) * stepLen;
        float3 q = start + L * t;
        float4 h = float4(q, 1.0);
        float4 c = float4(dot(h, vp0), dot(h, vp1), dot(h, vp2), dot(h, vp3));
        if (c.w <= 0.1) continue;
        float2 uv = float2(c.x / c.w * 0.5 + 0.5, 0.5 - c.y / c.w * 0.5);
        if (any(uv < 0.0) || any(uv > 1.0)) continue;
        float4 s = SurfaceAt(uv);
        if (s.w < 0.0) continue;
        float behind = length(q) - s.w;
        vis *= 1.0 - smoothstep(0.04, 0.16, behind) * (1.0 - smoothstep(thick, thick * 2.0, behind));
    }
    return vis;
}

struct Out
{
    float4 light : COLOR0;
    float4 ratio : COLOR1;
};

Out Result(float4 light, float4 ratio)
{
    Out o;
    o.light = light;
    o.ratio = ratio;
    return o;
}

Out main(float2 vpos : VPOS)
{
    float2 uv = PassUv(vpos);
    float4 here = SurfaceAt(uv);
    clip(here.w);
    // A cluster no light reaches leaves the cleared targets as they are, before any normal is built.
    bool normalsView = screen.z > 1.5 && screen.z < 2.5;
    float2 list = LightList(clusterTex, uv, here.w, clusterC, clusterD);
    [branch] if (!normalsView && list.y < 0.5) return Result(0.0, 1.0);
    float3 n = SurfaceNormal(vpos, uv, here);
    if (normalsView) return Result(float4(n * 0.5 + 0.5, 1.0), 1.0);   // the Normals view shows them as colour

    // The surface, as far as the scene tells: an albedo, and a roughness from it (dark stone reads
    // shinier than pale plaster) lowered by wetness.
    float3 sceneHere = Linear(tex2Dlod(sceneTex, float4(uv, 0, 0)).rgb);
    float3 albedo = Albedo(sceneHere, 0.0, Luma(sceneHere));   // only the roughness reads it
    float rough = clamp(lerp(0.35, 0.85, saturate(Luma(albedo) * 2.0)) - shading.y * 0.45, 0.08, 1.0);
    float alpha2 = rough * rough * rough * rough;
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;

    float3 V = -here.xyz / here.w;
    float NdotV = saturate(dot(n, V)) + 0.0001;
    // The engine already lights units with its own model lights: here they count only by unitsC.z.
    float unit = OnUnit(here.xyz);
    // Blue noise: the shadow rays' jitter turns every frame (the history averages it); the omni
    // filter's rotation stays put, since that term joins after the history.
    float4 noise = BlueNoiseAt(blueTex, vpos);
    float4 jitter = frac(noise + blueRot);
    // Its w (unused by the shadow rays) carries the texel's room for the lamps' room gate (the room
    // pass found it; -1 outside every room or with the gate off).
    jitter.w = unitsGiC.z < 1.0 && unitsGiC.y > 0.5
             ? floor(tex2Dlod(roomTex, float4((vpos + 0.5) * resC.xy, 0, 0)).r * 255.0 + 0.5) - 1.0 : -1.0;
    // The omni shadow maps of the most important lights, looked up once per pixel (each slot matches
    // one light), the receiver pushed off its surface a little; the loop reads the share of its light.
    // The Omni faces view: which face of the first map each pixel reads.
    [branch] if (screen.z > 5.5 && screen.z < 6.5)
    {
        float face = -1.0;
        float lit = omniLights[0].w > 0.0 ? OmniSlot(omni0, 0, here.xyz + n * (0.02 + 0.003 * here.w), n, omniSizes.x,
                                                     noise.w * 6.2831853, face) : 1.0;
        return Result(float4(FaceColour(face, lit), 0.0), 1.0);
    }
    // The light with its omni shadows (diffuse, specular) and, for the share the maps let through
    // (applied after the history), the luminance with and without them.
    // The light service's Cookie factor view.
    [branch] if (cookieD.z > 0.5) return Result(float4(CookieView(here.xyz, list).xxx, 0.0), 1.0);
    float3 diffuse = 0.0;
    float specular = 0.0, openLuma = 0.0, shadowLuma = 0.0;
    [loop] for (int i = 0; i < 128; ++i)
    {
        if ((float)i >= list.y) break;
        float index = LightAt(clusterTex, list.x + i, clusterC, clusterD);
        // Its cookie first, from its position alone, while nothing else of it is live.
        float4 a = tex2Dlod(lightTex, float4((index + 0.5) / kLightCapacity, 0.5 / kLightRows, 0, 0));
        float3 away = here.xyz - a.xyz;
        if (dot(away, away) >= a.w * a.w) continue;
        float has;
        float3 shaped = LightCookie(clusterTex, cookieAtlas, index, normalize(away), clusterC, clusterD, cookieC, cookieD, cookieE, has);
        float4 b, c;
        ReadLight(lightTex, index, a, b, c);
        float4 shape = ReadLightShape(lightTex, index);
        // Its shape towards this point (the cookie read above, the family profile) and, a lamp's
        // light staying on its own floor of a building, the room gate: its colour carries both, so
        // they scale diffuse and specular alike.
        b.rgb *= LightMask(shape, normalize(away), shaped, has);
        if (jitter.w > -0.5) b.rgb *= RoomGate(shape.w, jitter.w, unitsGiC.z);
        float3 lightPos = LightPoint(here.xyz, a, c, shape);
        float3 toLight = lightPos - here.xyz;
        float d2 = dot(toLight, toLight);
        float radius = abs(a.w);
        if (d2 < radius * radius)
        {
            float d = sqrt(d2);
            float3 L = toLight / max(d, 0.001);
            float NdotL = dot(n, L);
            // Lambert with a wrap, so a lamp's light rolls a little past the terminator.
            float wrapped = saturate((NdotL + lightsC.w) / (1.0 + lightsC.w));
            // A carried light's core is wider, and no light passes a set strength near its source.
            bool carried = carriedC.w > 0.5 && fmod(shape.w, 4.0) > 1.5;
            float reach = min(LightFalloffSoft(d2, radius, c.w, albedoC.y, carried ? carriedC.x : 0.0), carriedC.z)
                        * LightCone(-L, c.xyz, b.w);
            // On units: a carried light lights its own carrier by a share; other engine model lights by
            // unitsC.z, as the engine already lights units with them.
            if (unit > 0.5)
            {
                // A carried light this close to a character's point is its carrier's own: the torch
                // in a hand, a yard and a half across, two and a half up or down.
                if (carried && dot(toLight.xy, toLight.xy) < 2.25 && abs(toLight.z) < 2.5) reach *= carriedC.y;
                else if (fmod(shape.w, 2.0) > 0.5) reach *= unitsC.z;
            }
            float amount = reach * wrapped;
            if (amount > 0.001)
            {
                // A light with an omni shadow map takes its slot's share (the slot from the light
                // service's table, by index); the screen-space steps stay as contact detail.
                float omni = 1.0;
                if (omniC.z > 0.5)
                {
                    float2 slot = OmniSlotOf(omniTable, index);
                    if (slot.x > 0.5)
                        omni = dot(slot.x == float4(1.0, 2.0, 3.0, 4.0) ? 1.0 : 0.0,
                                   tex2Dlod(omniVisTex, float4((vpos + 0.5) * resC.xy, 0, 0)));
                }
                float visible = 1.0;
                [branch] if (lightsC.y > 0.5) visible = ContactShadow(here.xyz, n, lightPos, jitter, shape.x * albedoC.w);
                // The fog between the light and the surface dims it.
                float passed = visible * exp(-shading.w * d);
                // A flame's light is whiter near its source; a lamp's a little.
                float3 colour = LightHot(b.rgb, d, c.w, sourcesC.z * (shape.y > 2.5 && shape.y < 3.5 ? 1.0 : 0.35));
                // GGX, energy conserving: what the Fresnel term reflects is taken from the diffuse.
                float3 H = normalize(L + V);
                float VdotH = saturate(dot(V, H));
                float fresnel = 0.04 + 0.96 * pow(1.0 - VdotH, 5.0);
                float3 lit = colour * amount * passed * (1.0 - fresnel);
                diffuse += lit * omni;
                openLuma += Luma(lit);
                shadowLuma += Luma(lit) * omni;
                if (shading.x > 0.0 && NdotL > 0.0)
                {
                    float NdotH = saturate(dot(n, H));
                    float q = NdotH * NdotH * (alpha2 - 1.0) + 1.0;
                    float D = alpha2 / (3.14159 * q * q);
                    float G = (NdotL / (NdotL * (1.0 - k) + k)) * (NdotV / (NdotV * (1.0 - k) + k));
                    float shine = Luma(colour) * reach * passed * D * G * fresnel / (4.0 * NdotV);
                    specular += shine * omni;
                }
            }
        }
    }
    float4 shadowed = float4(diffuse, specular * shading.x);
    if (movingC.w < 0.5) return Result(shadowed, 1.0);
    // Apart: the light without the maps, and the share they let through (one ratio, by luminance:
    // the maps' shadow is achromatic per light).
    float ratio = openLuma > 0.00001 ? shadowLuma / openLuma : 1.0;
    return Result(shadowed / max(ratio, 0.001), ratio.xxxx);
}
