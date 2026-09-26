// wxl-graphics-shadow: what the host (C++) and the service's own shaders share -- the binding layout of
// every pipeline, the pipeline ids and the pass constants' rows. Macros only, so both languages read it.
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

#ifndef WXL_SHADOW_SHARED_H
#define WXL_SHADOW_SHARED_H

#include "wxl/shadow/layout.h"

// --- one descriptor set layout for every pipeline ---------------------------------------------------
// Bindings 0..WXL_SHADOW_B_COUNT-1 are the public shadow set (shadow.hlsli with WXL_SHADOW_BINDING 0).
#define SH_B_SHADOW   0
#define SH_B_PASS     10   // uniform buffer: the pass rows below
#define SH_B_DEPTH    11   // Texture2D<float>: the world's INTZ depth
#define SH_B_NORMALS  12   // Texture2D<float4>: the G-buffer normals (neutral: a = 0)
#define SH_B_SRC0     13   // Texture2D<float>: a core omni atlas (static or every caster)
#define SH_B_SRC1     14   // Texture2D<float>: a core omni atlas (units)
#define SH_B_OUT0     15   // storage image
#define SH_B_OUT1     16   // storage image
#define SH_B_IN0      17   // Texture3D<float4>: the half-resolution masks
#define SH_B_IN1      18   // Texture2D<float>: the half-resolution linear depth
#define SH_B_COUNT    19
#define SH_PUSH_BYTES 32   // uint4 a, float4 b

// --- pipelines (the order of cmake/CompileShaders.cmake's list) ---------------------------------------
#define SH_PIPE_CONVERT  0
#define SH_PIPE_MIPS     1
#define SH_PIPE_MASK     2
#define SH_PIPE_UPSAMPLE 3
#define SH_PIPE_DEBUG    4
#define SH_PIPE_COUNT    5

// --- the mask, upsample and debug passes' rows ---------------------------------------------------------
#define SH_ROW_INVVP     0    // 4 columns: clip -> (rel, 1) before the divide
#define SH_ROW_VP        4    // 4 columns: (rel, 1) -> clip
#define SH_ROW_VIEWROT   8    // 3 rows: the view matrix's rotation rows (view normal -> world: dot with each)
#define SH_ROW_DEPTH     11   // depth range min, max; normals bound (1); 0
#define SH_ROW_LINEAR    12   // WXL_GfxView::depthLinearize
#define SH_ROW_SCREEN    13   // width, height, 1 / width, 1 / height
#define SH_ROW_TRACE     14   // trace width, height; pixels per trace texel (1 or 2); 0
#define SH_ROW_CONTACT   15   // sun length, lamp length, thickness, strength (0 off)
#define SH_ROW_CONTACT2  16   // most steps towards the sun, towards a lamp (one every 1.5 px up to these); body that shines (0 sun, 1 moon, -1 none), distance fade yards
#define SH_ROW_DEBUG     17   // view, slot, 0, 0
#define SH_ROW_ATLASVIEW 18   // the atlas view: uv scale x, y; 0; 0
#define SH_ROW_CSLOTS    19   // 8 rows, two slots each: slot s's contact share (0 none; it fades) and housing yards (the march skips its own fixture) in row s / 2, xy (s even) or zw (s odd)
#define SH_PASS_ROWS     27

// --- the convert pass's rows -----------------------------------------------------------------------------
#define SH_CROW_LIGHT    0    // where the core rendered the maps from (rel) xyz, radius
#define SH_CROW_INFO     1    // housing over the radius, units map bound (1), output face texels, 0
#define SH_CROW_ORIGIN   2    // output rectangle origin x, y in texels; 0; 0
#define SH_CROW_SIZES    3    // main atlas width, height; units atlas width, height
#define SH_CROW_MAIN     4    // 6 faces x 3 rows: (rel, 1) -> (u w, v w, w) in the main atlas (zero: face not drawn)
#define SH_CROW_UNITS    22   // the same for the units atlas
#define SH_CONVERT_ROWS  40

// --- debug views (SH_ROW_DEBUG.x), as src/core/Settings.hpp numbers them ---------------------------------
#define SH_VIEW_NONE      0
#define SH_VIEW_SUN       1
#define SH_VIEW_MOON      2
#define SH_VIEW_SLOT      3
#define SH_VIEW_SLOTS     4
#define SH_VIEW_CONTACT   5
#define SH_VIEW_TERRAIN   6
#define SH_VIEW_CASCADES  7
#define SH_VIEW_CAPSULES  8
#define SH_VIEW_ATLAS     9
#define SH_VIEW_NORMALS   10

#endif
