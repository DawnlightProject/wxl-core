// wxl-graphics-fog: the transport layer, a shallow gravity current of cold fog on the terrain block
// (docs/design.md, section 2). The virtual-pipe scheme: each cell pushes flux to its four neighbours
// in proportion to the difference of fog-surface height (floor + depth) times its own depth, keeps
// a damped share of last step's flux, and never sends out more than it holds. pc.a.x selects:
//   0  the fluxes (outflow to +x, -x, +y, -y in texel space, depth per second)
//   1  the depth: in minus out, then formation and decay; the velocity for the clipmaps
// pc.b.x is the step's simulated seconds (settling steps are longer); pc.a.y = 1 on the step that
// initialises the tiles blockInit names: they start at an equilibrium estimate, hollows pre-filled.
// Cascades: the baked map's spill points and reservoirs (and the API's cascades) hold banks of fog
// behind crests; the layer carries a tracer, the share of its depth that came from them, and that
// fog evaporates as it descends (it warms as it falls), the faster the steeper its fall.
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

#include "terrain.hlsli"

FOG_TEX(0) Texture2D<float2> floorTex;   // RG32F, mean and max, every mip
FOG_TEX(1) Texture2D<float>  skyTex;     // R8, sky share
FOG_TEX(2) Texture2D<float2> waterTex;   // RG16F, liquid height and kind
FOG_TEX(3) Texture2D<float>  sunVisTex;  // R8, FOG_SUNVIS_N^2
FOG_TEX(4) Texture2D<float4> cascadeTex; // B8G8R8A8: spill score, reservoir, fall azimuth / 2 pi, drop / 255
FOG_TEX(5) Texture2D<float>  tracerIn;   // last step's cascade share

FOG_OUT(0) [[vk::image_format("rgba32f")]] RWTexture2D<float4> flux;    // +x, -x, +y, -y
FOG_OUT(1) [[vk::image_format("rgba32f")]] RWTexture2D<float4> layer;   // depth, world vx, vy, net rate
FOG_OUT(2) [[vk::image_format("r16f")]]    RWTexture2D<float>  tracer;  // this step's cascade share

static const int2 kDirs[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };

int2 Wrap(int2 t) { return t & (FOG_BLOCK_N - 1); }

bool Valid(float2 g) { return blockMask.w > 0.5 && TileResident(g); }

float FloorMean(int2 t) { return floorTex.Load(int3(t, 0)).r; }

// The world direction of a texel-space step: +x runs along -Y, +y along -X.
float2 WorldDir(int2 d) { return float2(-float(d.y), -float(d.x)); }

// Formation (depth per second) and decay (per second) at a texel.
void Rates(int2 t, float2 g, float b, float d, out float form, out float decay)
{
    float2 uv = GridUv(g);
    float relief = floorTex.SampleLevel(sLinearWrap, uv, 4.0).r - b;   // + in hollows, - on ridges
    float hollow = saturate(relief / 8.0);
    float exposure = saturate(-relief / 12.0);
    float sky = skyTex.Load(int3(t, 0));
    float2 w = waterTex.Load(int3(t, 0));
    float wet = w.y > 0.5 && w.x > -5000.0 ? 1.0 : 0.0;
    float night = 1.0 - sunDir.w;
    form = transB.x * lerp(transB.y, 1.0, night) * (0.5 + 0.5 * sky) * (1.0 + transB.z * hollow + transB.w * wet);
    float sun = sunVisTex.SampleLevel(sLinearWrap, uv, 0) * sunDir.w * saturate(sunDir.z * 3.0);
    decay = transC.x + transC.y * sun + transC.z * transD.w * exposure;
    decay += transC.x * 6.0 * saturate((d - transC.w) / max(transC.w, 1.0));
}

// The bank depth the cascades hold at a texel (automatic from the baked map, and the API's).
float CascadeBank(int2 t, float2 xy)
{
    float bank = 0.0;
    if (pourA.x > 0.0)
    {
        float4 c = cascadeTex.Load(int3(t, 0));
        float w = max(c.r, c.g);
        if (w > pourA.z)
        {
            float a = c.b * 6.2831853;
            float2 fall = float2(cos(a), sin(a));
            float blows = saturate(0.25 + 0.75 * dot(outG.xy, fall)) * saturate(outG.z / 2.0);
            float night = 1.0 - sunDir.w;
            // Banks of moist air outlast the night more than ground fog does: over half of it by day.
            bank = pourA.y * pourA.x * saturate((w - pourA.z) / max(1.0 - pourA.z, 0.05)) * lerp(1.0, blows, pourA.w)
                 * lerp(max(transB.y, 0.6), 1.0, night) * saturate(c.a * 255.0 / 20.0);
        }
    }
    [loop] for (uint i = 0u; i < uint(pourB.z); ++i)
    {
        float4 a = pour[i * 2u], b = pour[i * 2u + 1u];
        float2 rel = xy - a.xy;
        float along = dot(rel, b.xy);
        float across = abs(dot(rel, float2(-b.y, b.x)));
        // The bank lies behind the spill point, up to 60 yards back, as wide as the cascade.
        float w = (1.0 - smoothstep(0.7 * a.w, a.w, across)) * smoothstep(-60.0, -40.0, along) * (1.0 - smoothstep(0.0, 6.0, along));
        bank = max(bank, b.z * b.w * w);
    }
    return bank;
}

// The push the API's cascades give the layer, a surface tilt along their fall, over 250 yards.
float2 CascadePush(float2 xy)
{
    float2 tilt = 0.0;
    [loop] for (uint i = 0u; i < uint(pourB.z); ++i)
    {
        float4 a = pour[i * 2u], b = pour[i * 2u + 1u];
        float2 rel = xy - a.xy;
        float along = dot(rel, b.xy);
        float across = abs(dot(rel, float2(-b.y, b.x)));
        float w = (1.0 - smoothstep(a.w, 1.6 * a.w, across)) * smoothstep(-60.0, -30.0, along) * (1.0 - smoothstep(200.0, 250.0, along));
        tilt += b.xy * 0.12 * b.w * w;
    }
    return tilt;
}

float InitialDepth(int2 t, float2 g, float b)
{
    float form, decay;
    Rates(t, g, b, 0.0, form, decay);
    float hollowFill = max(floorTex.SampleLevel(sLinearWrap, GridUv(g), 4.0).r - b, 0.0);
    return min(form / max(decay, 1e-4), transC.w) + transD.z * hollowFill;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= FOG_BLOCK_N)) return;
    int2 t = int2(id.xy);
    float2 g = TexelToGrid(t);
    bool valid = Valid(g);
    float dt = pc.b.x;
    bool init = pc.a.y != 0u && TileInit(g);

    if (pc.a.x == 0u)
    {
        float4 f = flux[t];
        float d = layer[t].x;
        if (!valid || init || d <= 0.0)
        {
            flux[t] = 0.0;
            return;
        }
        float H = FloorMean(t) + d;
        float2 wind = outG.xy * outG.z * transA.z + CascadePush(GridToWorld(g));   // surface tilt, per yard
        // Steady flow runs at drainage x slope: gain = drainage x friction / dx^2; friction damps.
        float gain = dt * transA.x * transA.y / (kTexelYards * kTexelYards);
        float kept = exp(-transA.y * dt);
        float out4[4] = { f.x, f.y, f.z, f.w };
        float sum = 0.0;
        [unroll] for (int k = 0; k < 4; ++k)
        {
            int2 n = Wrap(t + kDirs[k]);
            float2 gn = g + float2(kDirs[k]);
            float fk = 0.0;
            if (Valid(gn))
            {
                float Hn = FloorMean(n) + layer[n].x;
                float tilt = dot(wind, WorldDir(kDirs[k])) * kTexelYards;
                fk = max(0.0, out4[k] * kept + gain * (H - Hn + tilt) * d);
                fk = min(fk, transD.y * d / kTexelYards);
                // Never more than would level the two surfaces this step: no overshoot, no checkerboard.
                fk = min(fk, max(H - Hn + tilt, 0.0) * 0.25 / max(dt, 1e-4));
            }
            out4[k] = fk;
            sum += fk;
        }
        float scale = sum * dt > d ? d / (sum * dt) : 1.0;
        flux[t] = float4(out4[0], out4[1], out4[2], out4[3]) * scale;
        return;
    }

    float b = FloorMean(t);
    if (!valid)
    {
        layer[t] = float4(0.0, 0.0, 0.0, 0.0);
        tracer[t] = 0.0;
        return;
    }
    float2 xy = GridToWorld(g);
    if (init)
    {
        float bank = CascadeBank(t, xy);
        float d0 = max(InitialDepth(t, g, b), bank);
        layer[t] = float4(d0, 0.0, 0.0, 0.0);
        tracer[t] = d0 > 1e-3 ? saturate(bank / d0) : 0.0;
        return;
    }
    float4 own = flux[t];
    float4 L = layer[t];
    // Inflow: each neighbour's flux towards this cell (+x's -x, -x's +x, +y's -y, -y's +y).
    float inPX = flux[Wrap(t + int2(1, 0))].y;
    float inNX = flux[Wrap(t + int2(-1, 0))].x;
    float inPY = flux[Wrap(t + int2(0, 1))].w;
    float inNY = flux[Wrap(t + int2(0, -1))].z;
    float outSum = own.x + own.y + own.z + own.w;
    float inSum = inPX + inNX + inPY + inNY;
    float d = max(L.x + dt * (inSum - outSum), 0.0);

    // The cascade share rides the same fluxes: what flows in brings its source's share.
    float c0 = tracerIn.Load(int3(t, 0));
    float inTracer = inPX * tracerIn.Load(int3(Wrap(t + int2(1, 0)), 0)) + inNX * tracerIn.Load(int3(Wrap(t + int2(-1, 0)), 0))
                   + inPY * tracerIn.Load(int3(Wrap(t + int2(0, 1)), 0)) + inNY * tracerIn.Load(int3(Wrap(t + int2(0, -1)), 0));
    float mass = max(L.x * c0 + dt * (inTracer - outSum * c0), 0.0);

    // A cascade's bank: filled towards its depth over some twenty seconds, all of it cascade fog.
    float bank = CascadeBank(t, xy);
    if (bank > d)
    {
        float add = (bank - d) * (1.0 - exp(-dt / 20.0));
        d += add;
        mass += add;
    }

    // Falling cascade fog warms and evaporates, the more the faster it descends.
    if (pourB.x > 0.0 && mass > 1e-4)
    {
        float2 grad = FloorGradient(floorTex, xy, 0.0);
        float descent = max(-dot(L.yz, grad), 0.0);
        float gone = mass * (1.0 - exp(-descent * pourB.x * dt));
        d = max(d - gone, 0.0);
        mass -= gone;
    }

    float form, decay;
    Rates(t, g, b, d, form, decay);
    float before = d;
    d = (d + form * dt) * exp(-decay * dt);
    mass *= exp(-decay * dt);
    tracer[t] = d > 1e-3 ? saturate(mass / d) : 0.0;

    // Velocity in texel space from the net flux through each axis, then in world axes.
    float mean = max(0.5 * (L.x + d), 0.05);
    float vx = 0.5 * (own.x - own.y + inNX - inPX) * kTexelYards / mean;
    float vy = 0.5 * (own.z - own.w + inNY - inPY) * kTexelYards / mean;
    float2 v = clamp(float2(-vy, -vx), -transD.y, transD.y);
    layer[t] = float4(d, v, (d - before) / max(dt, 1e-4));
}
