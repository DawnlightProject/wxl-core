// wxl-graphics-fog: what the host (C++) and the shaders (HLSL through DXC) share -- binding numbers,
// grid sizes, pipeline ids and the constant buffer's rows. Macros only, so both languages read it.
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

#ifndef WXL_FOG_SHARED_H
#define WXL_FOG_SHARED_H

// --- one descriptor set layout for every pipeline ---------------------------------------------------
#define FOG_B_CONSTANTS         0    // uniform buffer: the rows below
#define FOG_B_PRIMS             1    // storage buffer: float4 primitives (4 rows each)
#define FOG_B_BINS              2    // storage buffer: uint4 bin masks (FOG_LEVELS x FOG_BINS^2)
#define FOG_B_TEX0              3    // sampled images t0..t15: bindings 3..18
#define FOG_B_SAMP_POINT_CLAMP  19
#define FOG_B_SAMP_LINEAR_CLAMP 20
#define FOG_B_SAMP_LINEAR_WRAP  21
#define FOG_B_SAMP_POINT_WRAP   22
#define FOG_B_OUT0              23   // storage images u0..u5: bindings 23..28
#define FOG_B_RWBUF             29   // storage buffer written by the probe
#define FOG_B_COUNT             30
#define FOG_TEX_SLOTS           16
#define FOG_OUT_SLOTS           6
#define FOG_PUSH_BYTES          32   // uint4 a, float4 b

// --- grids -------------------------------------------------------------------------------------------
#define FOG_LEVELS      4
#define FOG_LEVEL_N     128          // cells across a level (x and y), a power of two
#define FOG_LEVEL_NZ    32           // layers above the ground
#define FOG_VEL_N       64           // the velocity field: half the level's resolution
#define FOG_VEL_NZ      16
#define FOG_OCC_N       16           // occupancy: one texel per 8 x 8 x 8 cells
#define FOG_OCC_NZ      4
#define FOG_BLOCK_TILES 8            // ADT tiles across the terrain block
#define FOG_TILE_TEXELS 128          // texels per tile edge (the bake's grid, 4.17 yd)
#define FOG_BLOCK_N     1024         // FOG_BLOCK_TILES * FOG_TILE_TEXELS
#define FOG_BLOCK_MIPS  11
#define FOG_SUNVIS_N    256          // the ground's sun visibility map over the block
#define FOG_MAX_PRIMS   128
#define FOG_BINS        8            // bins across a level, per axis
#define FOG_MAX_ROOMS   12
#define FOG_TILE_YARDS  533.3333333
#define FOG_TEXEL_YARDS 4.1666667
#define FOG_WAKE_N      256          // cells across a wake level (a 2D fluid around the player)
#define FOG_WAKE_LEVELS 2            // 0.25 yd and 1 yd cells
#define FOG_MAX_BODIES  256          // bodies, missiles, shocks, fires and frost splatted into it
#define FOG_MAX_POURS   8            // cascades gameplay places through the API
#define FOG_MAX_OCCLUDERS 16         // bodies shadowing lamps in the fog

// --- wake splat kinds (a body's second row, w) ----------------------------------------------------------
#define FOG_BODY_WALKER  0           // a player, creature or NPC: moves the air with it, clears the fog
#define FOG_BODY_MISSILE 1           // drags the air along its path
#define FOG_BODY_SHOCK   2           // an expanding ring: fog blown out, piled at the front
#define FOG_BODY_FIRE    3           // evaporates the fog
#define FOG_BODY_FROST   4           // thickens the fog
#define FOG_BODY_TORCH   5           // a carried flame: a pocket the fog cannot reform in
#define FOG_WAKE_TORCH   20.0         // a wake height from here up marks a torch pocket (height - this)

// --- wake sub-passes (push a.x; a.y = the Jacobi parity) --------------------------------------------------
#define FOG_WAKE_ADVECT  0
#define FOG_WAKE_SPLAT   1
#define FOG_WAKE_DIVERGE 2
#define FOG_WAKE_JACOBI  3
#define FOG_WAKE_PROJECT 4
#define FOG_WAKE_CURL    5

// --- primitive kinds (row 0 w) -----------------------------------------------------------------------
#define FOG_PRIM_SPHERE   0
#define FOG_PRIM_BOX      1          // yawed box
#define FOG_PRIM_CAPSULE  2          // segment a..b with a radius
#define FOG_PRIM_CYLINDER 3          // vertical, from the centre's height up
#define FOG_PRIM_FRONT    4          // a line front: plane advancing along a direction
#define FOG_PRIM_RING     5          // a closing front: the outside of a sphere shrinking to a point

// --- pipelines (the order of cmake/CompileShaders.cmake's list) ---------------------------------------
#define FOG_PIPE_NOISE     0
#define FOG_PIPE_TERRAIN   1
#define FOG_PIPE_TRANSPORT 2
#define FOG_PIPE_GROUND    3
#define FOG_PIPE_VELOCITY  4
#define FOG_PIPE_ADVECT    5
#define FOG_PIPE_SOURCE    6
#define FOG_PIPE_OCCUPANCY 7
#define FOG_PIPE_LIGHT     8
#define FOG_PIPE_LAMPS     9
#define FOG_PIPE_MARCH     10
#define FOG_PIPE_COMBINE   11
#define FOG_PIPE_TEMPORAL  12
#define FOG_PIPE_PROBE     13
#define FOG_PIPE_DEBUG     14
#define FOG_PIPE_WAKE      15
#define FOG_PIPE_COUNT     16

// --- debug views (constant debug.x) ------------------------------------------------------------------
#define FOG_VIEW_NONE        0
#define FOG_VIEW_FOG_ONLY    1       // the fog's light over black
#define FOG_VIEW_TRANSMIT    2       // transmittance
#define FOG_VIEW_SLICE_TOP   3       // a level seen from above at a height over the ground
#define FOG_VIEW_SLICE_SIDE  4       // a vertical slice along the view
#define FOG_VIEW_LAYER       5       // transport layer depth (map)
#define FOG_VIEW_FLOW        6       // transport layer flow (map)
#define FOG_VIEW_SOURCES     7       // formation and decay (map)
#define FOG_VIEW_INDOOR      8       // indoor (red) and outdoor (blue) per pixel
#define FOG_VIEW_LAMPS       9       // lamp light the fog scatters, per pixel
#define FOG_VIEW_SUN         10      // sun transmittance at the fog (per pixel)
#define FOG_VIEW_TERRAIN     11      // floor, hollows and sky share (map)
#define FOG_VIEW_DEPTH       12      // representative distance and history rejection
#define FOG_VIEW_OCCUPANCY   13      // empty-space skipping (per pixel: steps skipped)
#define FOG_VIEW_WAKE        14      // the wake fluid: fog cleared or thickened, the air's flow (map)
#define FOG_VIEW_CASCADE     15      // cascades: spill points, reservoirs, cascade fog (map)
#define FOG_VIEW_COUNT       16

// --- the constant buffer: every row a float4 ----------------------------------------------------------
// X1(name) is one row, XN(name, n) an array of rows. The host declares float name[4] / name[n][4]; the
// shaders a cbuffer of float4, so the layouts match row for row.
#define FOG_CONSTANTS(X1, XN)                                                                             \
    /* camera and frame */                                                                               \
    X1(eye)            /* world eye xyz, w sim clock (s, wrapped at 4096) */                             \
    X1(frame)          /* frame index mod 1024, dt, blue-noise offset x, y (texels) */                   \
    X1(prevEye)        /* last frame's eye xyz, 0 */                                                     \
    XN(viewProj, 4)    /* columns of (r, 1) -> clip, D3D form, r = world - eye */                        \
    XN(invViewProj, 4) /* columns of clip -> (r, 1) */                                                   \
    XN(reproject, 4)   /* columns of (r, 1) -> last frame's clip */                                      \
    X1(depthRange)     /* min, 1 / (max - min), max, 0 */                                                \
    X1(depthLin)       /* WXL_GfxView::depthLinearize */                                                 \
    X1(screenFull)     /* w, h, 1 / w, 1 / h */                                                          \
    X1(screenHalf)                                                                                       \
    X1(screenQuarter)                                                                                    \
    X1(march)          /* near range, far distance, near steps, far steps */                             \
    X1(march2)         /* detail distance, occupancy epsilon, start distance, global intensity */        \
    X1(temporal)       /* new share, clip gamma, history valid, reject depth ratio */                    \
    /* clipmap levels */                                                                                 \
    XN(levelA, 4)      /* cell xy, cell z, h min, 1 / cell xy */                                         \
    XN(levelB, 4)      /* window origin x, y (cells), last step's origin x, y */                         \
    XN(levelC, 4)      /* step dt, reset, noise cutoff scale, restrict weight */                         \
    /* terrain block */                                                                                  \
    X1(block)          /* origin tile x, y, texel yards, fallback floor */                               \
    X1(blockMask)      /* resident tiles: bits 0..31, 32..63 (asuint), tiles resident, 1 = any */        \
    X1(blockInit)      /* tiles to initialise: bits 0..31, 32..63 (asuint), 1 = all, 0 */                \
    /* transport */                                                                                      \
    X1(transA)         /* drainage (yd/s per unit slope), friction (1/s), wind push (slope/(yd/s)), 0 */\
    X1(transB)         /* formation (yd/s), day share, hollow boost, water boost */                      \
    X1(transC)         /* decay (1/s), sun burn-off (1/s), wind scour (1/s per yd/s), pool depth */      \
    X1(transD)         /* 0, max speed, initial fill share, wind speed */                                \
    /* lighting */                                                                                       \
    X1(sunDir)         /* towards the sun xyz, sun weight */                                             \
    X1(moonDir)        /* towards the moon xyz, moon weight */                                           \
    X1(lightDir)       /* towards the active body xyz, its weight */                                     \
    X1(lightColor)     /* linear rgb of the active body, 0 */                                            \
    X1(skyZenith)      /* linear rgb */                                                                  \
    X1(skyHorizon)                                                                                       \
    X1(zoneFog)        /* the zone's fog colour, linear rgb */                                           \
    X1(phase)          /* forward g, back g, back share, powder */                                       \
    X1(scatterMs)      /* multiple scattering strength, octaves, sky shadow, soft white */               \
    X1(shadow)         /* self-shadow density scale, terrain shadow, penumbra (rad), world shadow */     \
    XN(cascade, 9)     /* three sun cascades: 3 rows each, (r, 1) -> (u, v, depth) */                    \
    X1(cascadeInfo)    /* bands, hwPcf, 0, 0 */                                                          \
    /* outdoor profile */                                                                                \
    X1(outA)           /* layer density, ground mist density, mist height, haze density */               \
    X1(outB)           /* haze base z, haze falloff, bank coverage, bank strength */                     \
    X1(outC)           /* albedo rgb (linear, brightness in), 0 */                                       \
    X1(outD)           /* ambient, ambient floor, valley darkening, lamp scatter */                      \
    X1(outE)           /* sun scatter, moon scatter, exposure, 0 */                                      \
    X1(outF)           /* shape contrast, dense contrast, top softness, top billow */                    \
    X1(outG)           /* wind x, y (unit), wind speed, turbulence */                                    \
    X1(outH)           /* smoke albedo rgb, smoke absorption */                                          \
    X1(outI)           /* renewal rate (1/s), heat, erosion, curl */                                     \
    X1(outJ)           /* shape scale, detail scale, bank scale, layer flow share */                     \
    X1(outK)           /* climb (yd), 0, 0, 0 */                                                         \
    X1(detailA)        /* fine fraying period share, radians per half-res pixel, 0, 0 */                \
    /* atmosphere (the air layer) */                                                                     \
    X1(airA)           /* density, 1 / scale height, camera ground z, terrain follow */                  \
    X1(airB)           /* noise strength, 1 / noise scale, forward g, base (yd over the ground) */       \
    X1(airC)           /* albedo rgb, sky share exponent */                                              \
    X1(airD)           /* body light rgb (linear, its scatter in), lamp scatter */                       \
    X1(airE)           /* sky light rgb (linear, its scatter in), 0 */                                   \
    X1(airF)           /* noise drift xyz (periods), 0 */                                                \
    /* the wake fluid (2D, around the player) */                                                         \
    XN(wakeA, 2)       /* per level: window origin x, y (cells), last step's origin x, y */              \
    XN(wakeB, 2)       /* per level: cell yards, 1 / cell, 0, 0 */                                       \
    X1(wakeC)          /* step dt, damping (1/s), refill (1/s), vorticity confinement */                 \
    X1(wakeD)          /* splats this step, extra height (yd), clearing strength, on */                  \
    X1(torchA)         /* torch pocket: mist floor (per yd), atmosphere share, 1 / recovery (1/s), 0 */  \
    /* cascades: fog banks behind crests pouring over them */                                            \
    X1(pourA)          /* automatic: strength, full bank depth (yd), threshold, wind share */            \
    X1(pourB)          /* 1 / evaporation descent (1/yd), streaks, API cascades, least lying depth (yd) */\
    XN(pour, 16)       /* API cascades, 2 rows each: spill point xyz, width; fall xy, bank depth, weight */\
    /* bodies that shadow lamps in the fog (a map-less lamp, or a carried one) */                        \
    X1(occInfo)        /* bodies, shadow strength, 1 = per-lamp masks valid, 0 */                        \
    XN(occl, 16)       /* per body: world feet xyz, height (radius from it) */                           \
    XN(occMask, 32)    /* per lamp (4 a row, asuint): the bodies that can shadow it */                   \
    /* indoor profile */                                                                                 \
    X1(inA)            /* dust density, floor haze, floor haze height, noise strength */                 \
    X1(inB)            /* albedo rgb, 0 */                                                               \
    X1(inC)            /* ambient, lamp scatter, noise scale, drift */                                   \
    X1(inD)            /* forward g, back g, back share, seep yards */                                   \
    X1(indoorState)    /* camera indoor eased, actual, lag radius, lag active */                         \
    /* noise offsets, wrapped on the host */                                                             \
    XN(boil, 3)        /* per scale: offset xyz (periods), period yards */                               \
    X1(drift)          /* bank x, y (periods), detail x, y (periods) */                                  \
    X1(drift2)         /* detail z, curl x, y, z (periods) */                                            \
    /* rooms and lights (wxl-graphics-lights) */                                                        \
    XN(rooms, 36)      /* 3 rows per room, camera-relative */                                            \
    X1(roomInfo)       /* rooms, room cross, 0, 0 */                                                     \
    X1(lights)         /* lights, cookie atlas single channel, omni slots bound, lamp grid on */         \
    X1(clusterC)                                                                                         \
    X1(clusterD)                                                                                         \
    X1(cookieC)                                                                                          \
    X1(cookieD)                                                                                          \
    X1(cookieE)                                                                                          \
    X1(carried)        /* carried core yards, near cap, hot core, 0 */                                   \
    X1(lampGrid)       /* x, y, z, history share */                                                      \
    X1(lampGrid2)      /* near, 1 / log(far / near), far, omni bias */                                   \
    /* primitives, debug */                                                                              \
    X1(prims)          /* count, wake strength, 0, 0 */                                                  \
    X1(debug)          /* view, level, height over ground, map scale (yards per texel) */                \
    X1(debug2)         /* isolate bits (asuint), 0, 0, 0 */

// Isolate bits (debug2.x): each removes one ingredient.
#define FOG_ISO_NO_DETAIL      1u
#define FOG_ISO_NO_SHADOW      2u
#define FOG_ISO_NO_TERRAIN_SH  4u
#define FOG_ISO_NO_WORLD_SH    8u
#define FOG_ISO_NO_MS          16u
#define FOG_ISO_NO_AMBIENT     32u
#define FOG_ISO_NO_LAMPS       64u
#define FOG_ISO_NO_INDOOR      128u
#define FOG_ISO_NO_HAZE        256u
#define FOG_ISO_NO_FAR         512u
#define FOG_ISO_NO_NEAR        1024u
#define FOG_ISO_NO_HISTORY     2048u
#define FOG_ISO_NO_CLIP        4096u
#define FOG_ISO_NO_SKIP        8192u
#define FOG_ISO_FLAT           16384u
#define FOG_ISO_NO_LAYER       32768u
#define FOG_ISO_NO_AIR         65536u
#define FOG_ISO_ONLY_AIR       131072u

#endif
