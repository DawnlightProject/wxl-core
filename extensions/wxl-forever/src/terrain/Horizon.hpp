// wxl-forever terrain: the baked horizon maps around the camera, resident as three wrapping atlases.
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

#pragma once

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;
struct IDirect3DVolumeTexture9;

// 5.tools/forever-bake writes, per ADT tile, the terrain horizon in 16 azimuths (L8 volume 128 x
// 128 x 16, elevation over 90 degrees), the share of sky left visible (L8) and the height (R16F)
// under Textures\Forever\Horizon\, keyed "<Map>_<cx>_<cy>" in manifest_<Map>.csv. This service keeps
// a kBlock x kBlock block of tiles around the camera resident: each tile streams through
// core/BakedAssets and is copied into its cell (cx mod kBlock, cy mod kBlock) of three MANAGED
// atlases (a volume, a sky map, a height map), so the atlases wrap and a block shift refills only the
// cells that changed. A cell whose tile is missing or still loading holds neutral values (no horizon,
// full sky, height -10000). Shaders read them through shaders/horizon.hlsli; the CPU keeps each
// resident tile's heights for the shadow caster (Caster.cpp).
namespace wxl::forever::terrain::horizon
{
    constexpr int kTile     = 128;   // texels per tile edge, the bake's size
    constexpr int kBlock    = 4;     // tiles per atlas edge: 512 texels, a power of two, so the atlases wrap
    constexpr int kAzimuths = 16;
    constexpr float kNeutralHeight = -10000.0f;

    struct Settings
    {
        int   enabled  = 1;
        float strength = 1.0f;    // 0 no effect .. 1 the baked visibility
        float penumbra = 2.5f;    // degrees of sun elevation the shadow edge ramps over
        float occluder = 120.0f;  // yards away the horizon's terrain is assumed to be, for points above the ground
    };

    Settings& Get();

    /// Reads WXL_FOREVER_HORIZON_*. Called once at load.
    void Install();

    /// Once per frame: follows the camera's tile, requests the block's tiles, copies finished reads
    /// into the atlases (a few per frame). Loads the map's manifest on a map change.
    void Update(IDirect3DDevice9* dev, const float eye[3], uint32_t frame);

    /// Binds the atlases (linear, wrapping) at the given stages; a negative stage is skipped. False
    /// when nothing is resident (the neutral textures are bound then, so a shader still runs).
    bool Bind(IDirect3DDevice9* dev, int volumeStage, int skyStage, int heightStage);

    /// horizonC and horizonD of shaders/horizon.hlsli. withHeight says whether the height atlas is bound.
    void Constants(float horizonC[4], float horizonD[4], bool withHeight);

    /// True while at least one tile is resident.
    bool Ready();

    /// The heights of a resident tile, kTile x kTile floats (row r along -X, column c along -Y,
    /// texel (c, r) at world (x0 - r * T / 128, y0 - c * T / 128)), or null.
    const float* Heights(int cx, int cy);

    /// A resident tile's height range; false when not resident.
    bool Bounds(int cx, int cy, float& zmin, float& zmax);

    /// The block's origin tile and the height range over its resident tiles.
    void Block(int& ox, int& oy, float& zmin, float& zmax);

    /// Generation, bumped whenever a tile's heights arrive or leave (the caster rebuilds then).
    uint32_t Generation();

    int         Resident();   // tiles copied into the atlases
    const char* Status();     // one line for the panel

    /// Frees the atlases (MANAGED, so only for a clean teardown); they come back on the next Update.
    void ReleaseTextures();
}
