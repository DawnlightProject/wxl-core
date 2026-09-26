// wxl-graphics-lights: what the host (C++) and the compute shaders (HLSL through DXC) share -- binding
// numbers, pipeline ids, debug views, isolates and the constant buffer's rows. Macros only, so both
// languages read it.
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

#ifndef WXL_LIGHTS_SHARED_H
#define WXL_LIGHTS_SHARED_H

// --- one descriptor set layout for every pipeline ---------------------------------------------------
#define LIGHTS_B_CONSTANTS         0    // uniform buffer: the rows below
#define LIGHTS_B_TEX0              1    // sampled images t0..t11: bindings 1..12
#define LIGHTS_B_SAMP_POINT_CLAMP  13
#define LIGHTS_B_SAMP_LINEAR_CLAMP 14
#define LIGHTS_B_OUT0              15   // storage images u0..u3: bindings 15..18
#define LIGHTS_B_COUNT             19   // wxl-graphics-shadow's bindings follow (WXL_SHADOW_BINDING)
#define LIGHTS_TEX_SLOTS           12
#define LIGHTS_OUT_SLOTS           4
#define LIGHTS_PUSH_BYTES          32   // uint4 a, float4 b

// Texture slots (a pass declares only those it reads).
#define LIGHTS_T_DEPTH     0    // the INTZ depth, full resolution
#define LIGHTS_T_NORMALS   1    // RT1
#define LIGHTS_T_ALBEDO    2    // RT2
#define LIGHTS_T_SOURCES   3    // the HDR source texture (sources.hlsli)
#define LIGHTS_T_CLUSTERS  4    // the legacy cluster texture (lists and cookie rows)
#define LIGHTS_T_COOKIES   5    // the cookie atlas
#define LIGHTS_T_MASKS     6    // wxl-graphics-shadow's masks (3D, R8G8B8A8)
#define LIGHTS_T_NOISE     7    // graphics-extend's blue noise, 128 x 128
#define LIGHTS_T_FIELD     8    // the field's inscatter (3D), read by the halo integration
#define LIGHTS_T_HALO      9    // the integrated halo (3D), read by the surfaces

#define LIGHTS_MAX_ROOMS  12
#define LIGHTS_MAX_LIGHTS 128

// --- pipelines (the order of cmake/CompileShaders.cmake's list) ---------------------------------------
#define LIGHTS_PIPE_SURFACE      0
#define LIGHTS_PIPE_FIELD        1
#define LIGHTS_PIPE_FIELD_SHADOW 2   // the field with wxl-graphics-shadow's point lookups
#define LIGHTS_PIPE_HALO         3
#define LIGHTS_PIPE_COUNT        4

// --- the field's grid ------------------------------------------------------------------------------
#define LIGHTS_FIELD_W 160
#define LIGHTS_FIELD_H 90
#define LIGHTS_FIELD_D 64

// --- debug views (debug.x) ----------------------------------------------------------------------------
#define LIGHTS_VIEW_NONE     0
#define LIGHTS_VIEW_ALBEDO   1   // RT2 as the lights see it; magenta: a normal but no albedo
#define LIGHTS_VIEW_NORMALS  2   // the world normal as colour
#define LIGHTS_VIEW_MATERIAL 3   // kind: red model, green building, blue terrain; brightness the gloss
#define LIGHTS_VIEW_LIGHT    4   // the lamps' light alone, on white surfaces
#define LIGHTS_VIEW_CLUSTERS 5   // lights in the pixel's cluster, as heat
#define LIGHTS_VIEW_SHADOWS  6   // the shadow slots' visibility, one colour per slot
#define LIGHTS_VIEW_SUN      7   // the sun and moon factor (white: the engine's own)
#define LIGHTS_VIEW_COOKIES  8   // the cookies' transmittance, summed over the lamps
#define LIGHTS_VIEW_ROOMS    9   // the pixel's room, one colour per room, weighted
#define LIGHTS_VIEW_FIELD    10  // a slice of the field (debug.z: 0..1 in depth)
#define LIGHTS_VIEW_HALO     11  // the field integrated along each pixel's ray
#define LIGHTS_VIEW_COUNT    12

// Isolate bits (debug.y): each removes one ingredient.
#define LIGHTS_ISO_NO_COOKIES   1u
#define LIGHTS_ISO_NO_SHADOWS   2u
#define LIGHTS_ISO_NO_SPECULAR  4u
#define LIGHTS_ISO_NO_ROOMS     8u
#define LIGHTS_ISO_NO_FOG       16u
#define LIGHTS_ISO_NO_SUN       32u
#define LIGHTS_ISO_WHITE        64u
#define LIGHTS_ISO_NO_EMISSIVE  128u
#define LIGHTS_ISO_NO_PREFILTER 256u
#define LIGHTS_ISO_NO_PROFILE   512u

// --- the constant buffer: every row a float4 ----------------------------------------------------------
#define LIGHTS_CONSTANTS(X1, XN)                                                                          \
    /* camera and frame */                                                                               \
    X1(eye)            /* world eye xyz, frame index mod 1024 */                                         \
    XN(viewProj, 4)    /* columns of (r, 1) -> clip, D3D form, r = world - eye */                        \
    XN(invViewProj, 4) /* columns of clip -> (r, 1) */                                                   \
    XN(viewRows, 3)    /* rows of the engine's view matrix: world normal i = dot(row i, view normal) */  \
    X1(depthRange)     /* min, 1 / (max - min), max, 0 */                                                \
    X1(screen)         /* w, h, 1 / w, 1 / h */                                                          \
    X1(proj)           /* P[0], P[5] of the projection, pixel angle (radians, vertical), 0 */            \
    /* lights */                                                                                         \
    X1(lights)         /* lights, cookie atlas single channel, shadow masks bound, halo bound */         \
    X1(clusterC)                                                                                         \
    X1(clusterD)                                                                                         \
    X1(cookieC)                                                                                          \
    X1(cookieD)                                                                                          \
    X1(cookieE)        /* glass tint share, cookie face texels, prefilter strength, 0 */                 \
    XN(slots, 32)      /* each light's shadow slot (-1 none), four to a row, by list index */            \
    XN(rooms, 36)      /* 3 rows per room, camera-relative */                                            \
    X1(roomInfo)       /* rooms, floor leak, outdoor light indoors, indoor light outdoors */             \
    XN(roomWeights, 3) /* each room's weight (fading in or out), four to a row */                         \
    /* shading */                                                                                        \
    X1(shade)          /* diffuse model (0 Lambert, 1 Burley), specular, wetness, fog extinction x share */\
    X1(shade2)         /* emissive strength, roughness scale, 0, 0 */                                    \
    /* sun and moon (the engine's own terms, gamma) */                                                   \
    X1(sunDir)         /* towards the light the engine shades with (world), on */                        \
    X1(sunColors)      /* luma of the engine's diffuse, luma of its ambient, deepen, 0 */                \
    X1(sunWeights)     /* the shadow service's sun weight, moon weight, 0, 0 */                          \
    /* the field */                                                                                      \
    X1(fieldC)         /* w, h, d, halo strength */                                                      \
    X1(fieldD)         /* near yards, 1 / log(far / near), far yards, halo density per yard */           \
    X1(phase)          /* g forward, g back, blend, 0 */                                                 \
    X1(medium)         /* extinction per yard between lamps and the air, 0, 0, 0 */                      \
    /* debug */                                                                                          \
    X1(debug)          /* view, isolate bits (asuint), field slice 0..1, 0 */

#endif
