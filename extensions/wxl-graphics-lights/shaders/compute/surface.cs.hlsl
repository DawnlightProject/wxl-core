// wxl-graphics-lights: the surfaces, deferred, full resolution. Per pixel, from the G-buffer (depth, RT1
// normals, RT2 albedo and material): every lamp of the pixel's cluster -- window and soft core, cone,
// profile, cookie, room gate, the shadow service's slot mask, the fog's thinning -- as energy-conserving
// Burley diffuse and GGX specular, plus the glow of the lamp heads and, without the fog, the halo
// integrated along the pixel's ray. The sun and moon factor goes to alpha. docs/design.md, section 6.
// Written: rgb the added radiance (linear, scene units), a the factor on the engine's colour.
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

LIGHTS_TEX(LIGHTS_T_DEPTH)   Texture2D<float>  depthTex;
LIGHTS_TEX(LIGHTS_T_NORMALS) Texture2D<float4> normalTex;
LIGHTS_TEX(LIGHTS_T_ALBEDO)  Texture2D<float4> albedoTex;
LIGHTS_TEX(LIGHTS_T_MASKS)   Texture3D<float4> maskTex;
LIGHTS_TEX(LIGHTS_T_FIELD)   Texture3D<float4> fieldTex;
LIGHTS_TEX(LIGHTS_T_HALO)    Texture3D<float4> haloTex;

LIGHTS_OUT(0) [[vk::image_format("rgba16f")]] RWTexture2D<float4> outLight;

static const float3 kSlotColours[4] = { float3(1.0, 0.25, 0.2), float3(0.3, 1.0, 0.3), float3(0.3, 0.5, 1.0), float3(1.0, 1.0, 0.3) };

float SlotVisibility(int2 px, float slot)
{
    if (slot < -0.5 || lights.z < 0.5 || Isolated(LIGHTS_ISO_NO_SHADOWS)) return 1.0;
    int s = int(slot + 0.5);
    float4 v = maskTex.Load(int4(px, 1 + (s >> 2), 0));
    int k = s & 3;
    return k == 0 ? v.x : (k == 1 ? v.y : (k == 2 ? v.z : v.w));
}

// The shadow the engine lacks on its own sun or moon term, as a factor on its colour (gamma space):
// (amb + dif NdotL want) / (amb + dif NdotL), want the service's horizon and contact times the cascades
// deepened by sunColors.z. 1 where nothing more is shadowed, and inside rooms.
float SunFactor(int2 px, float3 n, float roomW)
{
    if (sunDir.w < 0.5 || lights.z < 0.5 || Isolated(LIGHTS_ISO_NO_SUN)) return 1.0;
    float4 m = maskTex.Load(int4(px, 0, 0));
    float total = sunWeights.x + sunWeights.y;
    if (total <= 1e-3) return 1.0;
    float body = (sunWeights.x * m.x + sunWeights.y * m.y) / total;
    float hc = m.z * m.w;
    float cascades = saturate(body / max(hc, 1e-3));
    float want = hc * lerp(1.0, cascades, sunColors.z);
    float ndl = saturate(dot(n, sunDir.xyz));
    float lit = sunColors.y + sunColors.x * ndl;
    float ratio = saturate((sunColors.y + sunColors.x * ndl * want) / max(lit, 1e-3));
    return lerp(ratio, 1.0, saturate(roomW));
}

// Burley's diffuse, normalised like Lambert (1 at normal incidence and roughness 0).
float Burley(float ndl, float ndv, float ldh, float rough)
{
    if (shade.x < 0.5) return 1.0;
    float fd90 = 0.5 + 2.0 * rough * ldh * ldh;
    float l = 1.0 + (fd90 - 1.0) * pow(1.0 - ndl, 5.0);
    float v = 1.0 + (fd90 - 1.0) * pow(1.0 - ndv, 5.0);
    return l * v;
}

// GGX with height-correlated Smith visibility; the returned value times pi E NdotL is the radiance.
float GgxSpecular(float ndl, float ndv, float ndh, float rough)
{
    float a = max(rough * rough, 0.002);
    float a2 = a * a;
    float d = ndh * ndh * (a2 - 1.0) + 1.0;
    float D = a2 / (kPi * d * d);
    float gv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    float gl = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    float V = 0.5 / max(gv + gl, 1e-5);
    return D * V;
}

// The halo the air around a lamp shows without the fog: the integrated field at this distance.
float3 Halo(float2 uv, float dist)
{
    if (lights.w < 0.5 || fieldC.w <= 0.0) return 0.0;
    float z = log(max(dist, fieldD.x) / fieldD.x) * fieldD.y;
    return haloTex.SampleLevel(sLinearClamp, float3(uv, saturate(z)), 0).rgb * fieldC.w;
}

float3 DebugField(float2 uv)
{
    return fieldTex.SampleLevel(sLinearClamp, float3(uv, saturate(debug.z)), 0).rgb;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(float2(id.xy) >= screen.xy)) return;
    int2 px = int2(id.xy);
    float2 uv = (float2(px) + 0.5) * screen.zw;
    int view = View();

    float d = depthTex.Load(int3(px, 0));
    bool sky = DepthIsSky(d);
    float3 r = sky ? RayAt(uv) * fieldD.z : FromScreen(uv, DepthToNdc(d));
    float dist = length(r);

    if (view == LIGHTS_VIEW_FIELD) { outLight[px] = float4(DebugField(uv), 1.0); return; }
    if (view == LIGHTS_VIEW_HALO) { outLight[px] = float4(Halo(uv, dist) / max(fieldC.w, 1e-4), 1.0); return; }

    float3 halo = Halo(uv, dist);
    float4 g1 = normalTex.Load(int3(px, 0));
    float4 g2 = albedoTex.Load(int3(px, 0));
    bool hasNormal = g1.a > 0.25;
    float code = floor(g2.a * 255.0 + 0.5);
    float kind = MaterialKind(code);
    if (sky || (!hasNormal && kind < 0.5))
    {
        // The sky, liquids, particles, blended draws: no surface to light, only the air in front.
        outLight[px] = view == LIGHTS_VIEW_NONE ? float4(halo, 1.0) : float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float3 V = -r / max(dist, 1e-4);
    float3 n = hasNormal ? normalize(ViewToWorld(g1.rgb * 2.0 - 1.0)) : V;
    // A normal facing away from the camera (an interpolated one at a grazing edge) is folded back.
    if (dot(n, V) < 0.0) n = normalize(n - V * (dot(n, V) * 1.01));
    float3 albedo = kind > 0.5 ? DecodeGamma(g2.rgb) : DecodeGamma(float3(0.35, 0.35, 0.35));
    if (Isolated(LIGHTS_ISO_WHITE) || view == LIGHTS_VIEW_LIGHT) albedo = 1.0;

    // Roughness from the material: terrain by its gloss mask, buildings, models; wet ground is glossier.
    float gloss = MaterialGloss(code);
    float rough = kind > 2.5 ? lerp(0.8, 0.35, gloss) : (kind > 1.5 ? 0.6 : 0.7);
    float room = RoomAt(r);
    float roomW = RoomWeight(room);
    float wet = shade.z * saturate(n.z * 2.0 - 1.0) * (1.0 - roomW);
    rough = saturate(lerp(rough, 0.2, wet) * shade2.y);
    albedo *= 1.0 - 0.3 * wet;

    float ndv = saturate(dot(n, V));
    // The footprint of this pixel in yards, for the cookies' prefilter.
    float pixelYards = dist * proj.z / max(ndv, 0.25);

    float3 diffuse = 0.0, specular = 0.0, emissive = 0.0, cookieSum = 0.0, shadowView = 0.0;
    float2 list = LightList(uv, dist);
    uint count = uint(list.y);
    [loop] for (uint e = 0; e < count; ++e)
    {
        uint i = LightAt(list.x + float(e));
        WxlLightSource s = WxlReadSource(sourceTex, i);
        if (s.reach <= 0.0) continue;
        float3 P = LightPoint(r, s);
        float3 L = P - r;
        float d2 = dot(L, L);
        if (d2 >= s.reach * s.reach) continue;
        float dl = sqrt(d2);
        float3 l = L / max(dl, 1e-4);

        // The lamp's own head: glass and flame glow, whatever their normal.
        if (s.emissive > 0.0 && dl < s.emissiveRadius && !Isolated(LIGHTS_ISO_NO_EMISSIVE))
        {
            float g = 1.0 - dl / s.emissiveRadius;
            emissive += s.intensity / max(Luma(s.intensity), 1e-4) * s.emissive * shade2.x * g * g;
        }

        float ndl = dot(n, l);
        if (ndl <= 0.0) continue;
        float E = WxlSourceFalloff(d2, s.reach, s.softRadius) * LightCone(s, -l);
        if (E <= 0.0) continue;
        float footprint = pixelYards / max(dl, 0.05);
        float3 shape = LightShape(i, s, -l, footprint);
        float gate = LightRoomGate(s, room, roomW);
        float vis = SlotVisibility(px, SlotOf(i));
        float thin = Isolated(LIGHTS_ISO_NO_FOG) ? 1.0 : exp(-shade.w * dl);
        float3 rgb = LightHot(s.intensity, dl, s.softRadius, s.hot) * (E * gate * vis * thin) * shape;

        float3 H = normalize(l + V);
        float ldh = saturate(dot(l, H));
        float ndh = saturate(dot(n, H));
        float F = 0.04 + 0.96 * pow(1.0 - ldh, 5.0);
        diffuse += rgb * (ndl * Burley(ndl, ndv, ldh, rough) * (1.0 - F));
        if (!Isolated(LIGHTS_ISO_NO_SPECULAR))
            specular += rgb * (kPi * ndl * GgxSpecular(ndl, max(ndv, 1e-3), ndh, rough) * F * shade.y);

        cookieSum += shape * saturate(E * 4.0);
        float slot = SlotOf(i);
        if (slot > -0.5) shadowView += kSlotColours[int(slot + 0.5) & 3] * (1.0 - vis);
    }

    float sun = SunFactor(px, n, roomW);
    float3 added = albedo * diffuse + specular + emissive;

    if (view != LIGHTS_VIEW_NONE)
    {
        float3 c = 0.0;
        if (view == LIGHTS_VIEW_ALBEDO) c = kind > 0.5 ? albedo : float3(1.0, 0.0, 1.0);
        else if (view == LIGHTS_VIEW_NORMALS) c = DecodeGamma(n * 0.5 + 0.5);
        else if (view == LIGHTS_VIEW_MATERIAL)
        {
            float3 k = kind < 1.5 ? float3(1.0, 0.2, 0.2) : (kind < 2.5 ? float3(0.2, 1.0, 0.2) : float3(0.2, 0.4, 1.0));
            c = kind > 0.5 ? DecodeGamma(k * (0.4 + 0.6 * gloss)) : float3(1.0, 0.0, 1.0);
        }
        else if (view == LIGHTS_VIEW_LIGHT) c = added;
        else if (view == LIGHTS_VIEW_CLUSTERS)
        {
            float t = saturate(list.y / 24.0);
            c = DecodeGamma(float3(saturate(t * 2.0), saturate(2.0 - t * 2.0) * saturate(t * 4.0), 1.0 - t) * (list.y > 0.0 ? 1.0 : 0.1));
        }
        else if (view == LIGHTS_VIEW_SHADOWS) c = DecodeGamma(saturate(1.0 - shadowView));
        else if (view == LIGHTS_VIEW_SUN) c = sun.xxx;
        else if (view == LIGHTS_VIEW_COOKIES) c = saturate(cookieSum);
        else if (view == LIGHTS_VIEW_ROOMS)
        {
            float b = room;
            c = b < -0.5 ? float3(0.05, 0.05, 0.05) : DecodeGamma(frac(float3(0.37, 0.61, 0.83) * (b + 1.0)) * (0.3 + 0.7 * roomW));
        }
        outLight[px] = float4(c, 1.0);
        return;
    }
    outLight[px] = float4(added + halo, sun);
}
