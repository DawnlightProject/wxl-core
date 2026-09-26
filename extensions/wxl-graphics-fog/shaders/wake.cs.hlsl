// wxl-graphics-fog: the wake fluid, one step. A 2D incompressible flow at the ground around the player
// (stable fluids): bodies are moving obstacles that carry the air with them, so the projection makes
// it part ahead of them, flow round and close in behind; vorticity confinement keeps the small eddies
// at the wake's edges curling. The fog change it carries (cleared where a body passed, piled where a
// shock pushed, thickened by frost) moves with that flow, so a wake refills by the fog flowing back in.
// pc.a.x selects the sub-pass (FOG_WAKE_*); pc.b.x = 1 on the frame's first step (the window moved);
// SV_DispatchThreadID.z (or the group's z for splats) is the level.
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

#include "wake.hlsli"

FOG_TEX(0) Texture3D<float4> stateIn;     // last step's state (advect)
FOG_TEX(1) Texture3D<float>  curlIn;      // last step's vorticity (advect)
FOG_TEX(2) Texture3D<float>  pressureIn;  // Jacobi: the other buffer; project: the result
FOG_TEX(3) Texture3D<float>  divIn;
FOG_OUT(0) [[vk::image_format("rgba16f")]] RWTexture3D<float4> state;   // vx, vy, fog change, its height
FOG_OUT(1) [[vk::image_format("r16f")]]    RWTexture3D<float>  curlOut;
FOG_OUT(2) [[vk::image_format("r16f")]]    RWTexture3D<float>  pressureOut;
FOG_OUT(3) [[vk::image_format("r16f")]]    RWTexture3D<float>  divOut;
[[vk::binding(FOG_B_RWBUF, 0)]] RWStructuredBuffer<float4> bodies;   // two rows per splat

static const int kMask = FOG_WAKE_N - 1;

int2 CellOf(uint k, int2 texel)
{
    int2 o = int2(wakeA[k].xy);
    return o + ((texel - (o & kMask)) & kMask);
}

int3 At(int2 texel, uint k, int2 d) { return int3((texel + d) & kMask, int(k)); }

// The window's outermost ring is the boundary: still air, no pressure.
bool Boundary(uint k, int2 cell)
{
    int2 rel = cell - int2(wakeA[k].xy);
    return any(rel < 1) || any(rel > FOG_WAKE_N - 2);
}

float CatmullRom(float4 w, float a, float b, float c, float d) { return w.x * a + w.y * b + w.z * c + w.w * d; }

float4 CatmullWeights(float t)
{
    float t2 = t * t, t3 = t2 * t;
    return float4(-0.5 * t3 + t2 - 0.5 * t, 1.5 * t3 - 2.5 * t2 + 1.0, -1.5 * t3 + 2.0 * t2 + 0.5 * t, 0.5 * t3 - 0.5 * t2);
}

// The fog change and its height at a back-traced cell position (cell units): Catmull-Rom, clamped to
// the four texels around it, so edges stay crisp without overshooting.
float2 SharpSample(uint k, float2 c)
{
    float2 base = floor(c - 0.5);
    float2 f = c - 0.5 - base;
    float4 wx = CatmullWeights(f.x), wy = CatmullWeights(f.y);
    float2 rows[4];
    float2 lo = 1e9, hi = -1e9;
    [unroll] for (int j = 0; j < 4; ++j)
    {
        float2 v[4];
        [unroll] for (int i = 0; i < 4; ++i)
        {
            int2 t = (int2(base) + int2(i - 1, j - 1)) & kMask;
            v[i] = stateIn.Load(int4(t, k, 0)).zw;
            if ((i == 1 || i == 2) && (j == 1 || j == 2))
            {
                lo = min(lo, v[i]);
                hi = max(hi, v[i]);
            }
        }
        rows[j] = float2(CatmullRom(wx, v[0].x, v[1].x, v[2].x, v[3].x), CatmullRom(wx, v[0].y, v[1].y, v[2].y, v[3].y));
    }
    float2 r = float2(CatmullRom(wy, rows[0].x, rows[1].x, rows[2].x, rows[3].x), CatmullRom(wy, rows[0].y, rows[1].y, rows[2].y, rows[3].y));
    return clamp(r, lo, hi);
}

void Advect(int2 texel, uint k)
{
    int2 cell = CellOf(k, texel);
    float s = WakeCell(k);
    float dt = wakeC.x;
    float2 prevO = pc.b.x > 0.5 ? wakeA[k].zw : wakeA[k].xy;
    bool had = all(float2(cell) >= prevO) && all(float2(cell) < prevO + kWakeN);
    if (!had || wakeD.w < 0.5 || Boundary(k, cell))
    {
        state[int3(texel, k)] = 0.0;
        return;
    }
    float2 vel = stateIn.Load(int4(texel, k, 0)).xy;

    // Vorticity confinement: a push along the vortices' edges that keeps small eddies alive.
    float w = curlIn.Load(int4(texel, k, 0));
    float2 eta = float2(abs(curlIn.Load(int4(At(texel, k, int2(1, 0)), 0))) - abs(curlIn.Load(int4(At(texel, k, int2(-1, 0)), 0))),
                        abs(curlIn.Load(int4(At(texel, k, int2(0, 1)), 0))) - abs(curlIn.Load(int4(At(texel, k, int2(0, -1)), 0))));
    float len = length(eta);
    if (len > 1e-5) vel += wakeC.w * s * float2(eta.y, -eta.x) / len * w * dt;

    // Back along the flow, in cells; outside last step's window there was nothing.
    float2 back = (float2(cell) + 0.5) - vel * dt / s;
    float2 rel = back - prevO;
    if (any(rel < 1.0) || any(rel > kWakeN - 1.0))
    {
        state[int3(texel, k)] = 0.0;
        return;
    }
    float2 v = stateIn.SampleLevel(sLinearWrap, float3(back / kWakeN, (float(k) + 0.5) / float(FOG_WAKE_LEVELS)), 0).xy;
    float2 gh = SharpSample(k, back);
    v *= exp(-wakeC.y * dt);
    // A torch's pocket refills at its own pace once the flame is gone; the rest at the wakes'.
    gh.x *= exp(-(gh.y >= FOG_WAKE_TORCH ? torchA.z : wakeC.z) * dt);
    if (abs(gh.x) < 0.02) gh.y = gh.y >= FOG_WAKE_TORCH ? gh.y - FOG_WAKE_TORCH : gh.y * exp(-0.5 * dt);
    state[int3(texel, k)] = float4(v, gh);
}

// One splat over the texels of its box at one level: this group's 8 x 8 threads walk it in tiles.
void Splat(uint index, uint k, uint2 thread)
{
    if (index >= uint(wakeD.x)) return;
    float4 r0 = bodies[index * 2u];
    float4 r1 = bodies[index * 2u + 1u];
    uint kind = uint(r1.w + 0.5);
    float s = WakeCell(k);
    float dt = wakeC.x;
    float R = max(r0.w, 0.05);
    // A walker clears twice its own size and sheds eddies behind it; the rest reach their own size.
    float reach = kind == FOG_BODY_SHOCK ? R + r1.y : kind == FOG_BODY_WALKER ? R * 3.6 : kind == FOG_BODY_TORCH ? R * 1.35 : R * 1.3;
    // Bodies smaller than a cell of the coarse level leave it to the fine one where it holds them.
    if (k == 1u && R < s * 0.75 && WakeInside(0u, r0.xy) > 4.0) return;
    if (WakeInside(k, r0.xy) < -reach / s) return;
    int n = min(int(ceil(reach / s)) + 1, 160);
    int2 centre = int2(floor(r0.xy / s));
    int span = 2 * n + 1;
    int tiles = (span + 7) / 8;
    float strength = wakeD.z;
    float speed = length(r1.xy);
    float2 dir = speed > 0.05 ? r1.xy / speed : float2(1.0, 0.0);
    // Eddies are shed from alternate flanks at a Strouhal number of 0.2 (each body its own phase).
    float shedSide = sin(6.2831853 * (0.2 * speed / (2.0 * R) * eye.w + float(index) * 0.37)) > 0.0 ? 1.0 : -1.0;
    float2 eddy = r0.xy - dir * 1.6 * R + float2(-dir.y, dir.x) * shedSide * 0.6 * R;
    for (int ty = 0; ty < tiles; ++ty)
    {
        for (int tx = 0; tx < tiles; ++tx)
        {
            int2 off = int2(tx * 8 + int(thread.x), ty * 8 + int(thread.y)) - n;
            if (any(off > n)) continue;
            int2 cell = centre + off;
            int2 rel = cell - int2(wakeA[k].xy);
            if (any(rel < 1) || any(rel > FOG_WAKE_N - 2)) continue;
            int3 t = int3(cell & kMask, int(k));
            float2 xy = (float2(cell) + 0.5) * s;
            float2 away = xy - r0.xy;
            float d = length(away);
            if (d > reach) continue;
            float4 v = state[t];
            float inside = 1.0 - smoothstep(0.65 * R, R, d);
            if (kind == FOG_BODY_WALKER)
            {
                // The air inside moves with the body, the air around is dragged a little; the fog is
                // pushed out of a space two and a half times the body's, the more the faster it goes
                // (at rest it closes in round it).
                float around = 1.0 - smoothstep(1.2 * R, 2.5 * R, d);
                v.xy = lerp(v.xy, r1.xy, max(0.85 * inside, 0.35 * around));
                float clear = strength * (0.35 + 0.65 * saturate(speed / 2.5));
                v.z = lerp(v.z, -clear, (1.0 - exp(-14.0 * dt)) * around);
                if (around > 0.05) v.w = max(v.w, r1.z);
                // The eddy being shed turns the air behind the body, up to a share of its speed.
                float2 fromEddy = xy - eddy;
                float de = length(fromEddy);
                if (speed > 0.3 && de < 1.2 * R && de > 1e-3)
                {
                    float2 turn = float2(-fromEddy.y, fromEddy.x) / de * shedSide;
                    float target = speed * 0.9 * (1.0 - de / (1.2 * R));
                    float now = dot(v.xy, turn);
                    if (now < target) v.xy += turn * min(target - now, target * dt * 6.0);
                }
            }
            else if (kind == FOG_BODY_MISSILE)
            {
                v.xy = lerp(v.xy, r1.xy, 0.6 * inside);
            }
            else if (kind == FOG_BODY_SHOCK)
            {
                // A ring running outwards: blown clear inside, piled up at its front, the air kicked out.
                float front = 1.0 - saturate(abs(d - R) / max(r1.y, 0.1));
                float core = 1.0 - smoothstep(0.7 * R, R, d);
                v.z = min(v.z, lerp(v.z, -r1.x, core));
                v.z = max(v.z, 0.6 * r1.x * front);
                if (d > 1e-3) v.xy += away / d * r1.x * 4.0 * front;
                if (core + front > 0.05) v.w = max(v.w, r1.z);
            }
            else if (kind == FOG_BODY_TORCH)
            {
                // The pocket breathes: its edge wanders with the angle and the time.
                float angle = atan2(away.y, away.x);
                float edge = R * (1.0 + 0.08 * sin(eye.w * 1.3 + angle * 3.0 + float(index)) + 0.05 * sin(eye.w * 0.7 - angle * 5.0));
                float pocket = 1.0 - smoothstep(0.7 * edge, edge, d);
                // Whatever fog drifts in is dried towards the floor, gently, so the wall at the edge rolls.
                v.z = lerp(v.z, -r1.x, (1.0 - exp(-3.0 * dt)) * pocket);
                if (pocket > 0.05) v.w = max(max(v.w, FOG_WAKE_TORCH) , FOG_WAKE_TORCH + r1.z);
                // A soft push outwards at the rim, turning a little, so the wall swirls.
                float rim = saturate(1.0 - abs(d - edge) / max(0.3 * edge, 0.3));
                if (d > 1e-3)
                {
                    float2 out2 = away / d, turn = float2(-out2.y, out2.x);
                    v.xy += (out2 * 0.6 + turn * 0.35 * sin(eye.w * 0.9 + float(index))) * rim * dt * 4.0;
                }
            }
            else if (kind == FOG_BODY_FIRE)
            {
                v.z = lerp(v.z, -1.0, (1.0 - exp(-r1.x * 4.0 * dt)) * inside);
                if (inside > 0.05) v.w = max(v.w, r1.z);
            }
            else
            {
                v.z = lerp(v.z, 0.6 * r1.x, (1.0 - exp(-2.0 * dt)) * inside);
                if (inside > 0.05) v.w = max(v.w, r1.z);
            }
            state[t] = v;
        }
    }
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    uint pass = pc.a.x;
    if (pass == FOG_WAKE_SPLAT)
    {
        Splat(group.x, group.z, thread.xy);
        return;
    }
    if (any(id.xy >= FOG_WAKE_N) || id.z >= FOG_WAKE_LEVELS) return;
    int2 texel = int2(id.xy);
    uint k = id.z;
    float s = WakeCell(k);
    int2 cell = CellOf(k, texel);
    int3 me = int3(texel, k);

    if (pass == FOG_WAKE_ADVECT)
    {
        Advect(texel, k);
    }
    else if (pass == FOG_WAKE_DIVERGE)
    {
        float div = 0.0;
        if (!Boundary(k, cell))
            div = (state[At(texel, k, int2(1, 0))].x - state[At(texel, k, int2(-1, 0))].x
                 + state[At(texel, k, int2(0, 1))].y - state[At(texel, k, int2(0, -1))].y) / (2.0 * s);
        divOut[me] = div;
    }
    else if (pass == FOG_WAKE_JACOBI)
    {
        float p = 0.0;
        if (!Boundary(k, cell))
            p = (pressureIn.Load(int4(At(texel, k, int2(1, 0)), 0)) + pressureIn.Load(int4(At(texel, k, int2(-1, 0)), 0))
               + pressureIn.Load(int4(At(texel, k, int2(0, 1)), 0)) + pressureIn.Load(int4(At(texel, k, int2(0, -1)), 0))
               - divIn.Load(int4(me, 0)) * s * s) * 0.25;
        pressureOut[me] = p;
    }
    else if (pass == FOG_WAKE_PROJECT)
    {
        float4 v = state[me];
        if (Boundary(k, cell)) v.xy = 0.0;
        else
            v.xy -= float2(pressureIn.Load(int4(At(texel, k, int2(1, 0)), 0)) - pressureIn.Load(int4(At(texel, k, int2(-1, 0)), 0)),
                           pressureIn.Load(int4(At(texel, k, int2(0, 1)), 0)) - pressureIn.Load(int4(At(texel, k, int2(0, -1)), 0))) / (2.0 * s);
        state[me] = v;
    }
    else
    {
        float w = 0.0;
        if (!Boundary(k, cell))
            w = (state[At(texel, k, int2(1, 0))].y - state[At(texel, k, int2(-1, 0))].y
               - state[At(texel, k, int2(0, 1))].x + state[At(texel, k, int2(0, -1))].x) / (2.0 * s);
        curlOut[me] = w;
    }
}
