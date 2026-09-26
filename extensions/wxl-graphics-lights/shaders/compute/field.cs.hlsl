// wxl-graphics-lights: the lamp light field for the air (GraphicsLightsFieldApi.h). One thread per froxel of
// the camera-frustum grid, which subdivides the light clusters exactly (10 x 10 x 4 froxels a cluster).
// For each lamp of the froxel's cluster, the in-scattered light per unit scattering coefficient,
// averaged over the froxel's depth span: the soft-core inverse square integrated in closed form along
// the view ray, times the window, cone, profile, cookie (prefiltered by the froxel's footprint), room
// gate, the shadow service's point shadow (the .shadow variant) and the medium's thinning, all taken at
// the span's point closest to the lamp. Written:
//   outScatter    rgb towards the eye with the dual Henyey-Greenstein phase, a luminance of the isotropic part
//   outAmbient    rgb with the isotropic phase, a directionality 0..1
//   outDirection  xyz mean propagation direction (unit), w total luminance
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

#ifdef WXL_LIGHTS_POINT_SHADOWS
#define WXL_SHADOW_BINDING LIGHTS_B_COUNT
#include "wxl/shadow/shadow.hlsli"
#endif

LIGHTS_OUT(0) [[vk::image_format("rgba16f")]] RWTexture3D<float4> outScatter;
LIGHTS_OUT(1) [[vk::image_format("rgba16f")]] RWTexture3D<float4> outAmbient;
LIGHTS_OUT(2) [[vk::image_format("rgba16f")]] RWTexture3D<float4> outDirection;

// Henyey-Greenstein, cosTheta between the light's propagation and the direction it leaves towards.
float Hg(float g, float cosTheta)
{
    float g2 = g * g;
    return (1.0 - g2) / (4.0 * kPi * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}

float Phase(float cosTheta) { return lerp(Hg(phase.x, cosTheta), Hg(phase.y, cosTheta), phase.z); }

float PointShadow(uint index, float3 q)
{
#ifdef WXL_LIGHTS_POINT_SHADOWS
    if (Isolated(LIGHTS_ISO_NO_SHADOWS)) return 1.0;
    int slot = ShadowSlotOf(int(index));
    return slot >= 0 ? ShadowLight(slot, q) : 1.0;
#else
    return 1.0;
#endif
}

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(float3(id) >= fieldC.xyz)) return;
    float2 uv = (float2(id.xy) + 0.5) / fieldC.xy;
    float3 dir = RayAt(uv);
    float d0 = FieldDistance(float(id.z) / fieldC.z);
    float d1 = FieldDistance(float(id.z + 1u) / fieldC.z);
    float span = max(d1 - d0, 1e-3);
    float3 centre = dir * (0.5 * (d0 + d1));
    // The froxel's size across, for the cookies' prefilter: the larger of its depth span and its width.
    float across = max(span, 0.5 * (d0 + d1) * proj.z * screen.y / fieldC.y);

    RoomSet rs = RoomsAt(centre, float3(0.0, 0.0, 0.0), false);

    float3 cell = floor(float3(id) / float3(fieldC.x / clusterC.x, fieldC.y / clusterC.y, fieldC.z / clusterC.z));
    float2 list = LightListAt(cell);
    uint count = uint(list.y);

    float3 scatter = 0.0, iso = 0.0, flux = 0.0;
    float total = 0.0;
    [loop] for (uint e = 0; e < count; ++e)
    {
        uint i = LightAt(list.x + float(e));
        WxlLightSource s = WxlReadSource(sourceTex, i);
        if (s.reach <= 0.0) continue;
        float3 P = LightPoint(centre, s);
        // The ray's closest approach to the lamp, and the closest point of the froxel's span.
        float t0 = dot(P, dir);
        float3 perp = P - dir * t0;
        float h2 = dot(perp, perp) + s.softRadius * s.softRadius;
        float tc = clamp(t0, d0, d1);
        float3 q = dir * tc;
        float3 L = q - P;
        float d2 = dot(L, L);
        if (d2 >= s.reach * s.reach) continue;
        float h = sqrt(h2);
        // The mean of 1 / (h^2 + (t - t0)^2) over the span, in closed form.
        float mean = (atan((d1 - t0) / h) - atan((d0 - t0) / h)) / (h * span);
        float dl = sqrt(d2);
        float3 l = L / max(dl, 1e-4);
        float falloff = mean * WxlSourceWindow(d2, s.reach) * LightCone(s, l);
        if (falloff <= 0.0) continue;
        float footprint = across / max(dl, 0.05);
        float3 shape = LightShape(i, s, l, footprint);
        float gate = LightRoomGate(s, rs);
        float vis = PointShadow(i, q);
        float3 rgb = LightHot(s.intensity, dl, s.softRadius, s.hot) * (falloff * gate * vis * LightMedium(dl)) * shape;
        float lum = Luma(rgb);
        if (lum <= 0.0) continue;
        // The light travels along l; it leaves towards the eye along -dir.
        scatter += rgb * Phase(dot(l, -dir));
        iso += rgb;
        flux += l * lum;
        total += lum;
    }
    iso *= 1.0 / (4.0 * kPi);
    float directionality = total > 0.0 ? saturate(length(flux) / total) : 0.0;
    float3 mean = total > 0.0 && dot(flux, flux) > 1e-12 ? normalize(flux) : float3(0.0, 0.0, -1.0);
    outScatter[id] = float4(scatter, Luma(iso));
    outAmbient[id] = float4(iso, directionality);
    outDirection[id] = float4(mean, total);
}
