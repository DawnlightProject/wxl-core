// wxl-forever fog: projectiles punch holes through the fog that refill slowly, in billows.
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

// Every missile in flight (core's wxl::game::effects) leaves its path as a trail of segments, one
// every 0.08 s of flight, each born with the time it was flown. A trail outlives its missile: where
// the missile stops, a crater is added. Each segment holds its hole open for a moment, then refills
// over the refill time with an ease in and out: the hole shrinks from its edge inwards, the edge
// perturbed by noise, while a shell of thicker fog at the edge rolls around the segment's axis and
// drifts with the wind, so the fog reads as smoke curling back in. A fire spell's trail widens
// slowly while it refills and its billows are darker and warmer. Nothing pops: a segment leaves the
// pool only once refilled, and a full pool merges its oldest segments.
//
// The shader reads the pool from a texture (kMaxSegments x 4 texels) and each froxel tests only the
// segments of its screen tile (a 16 x 9 grid, up to 8 per tile).
namespace wxl::forever::fog::projectiles
{
    struct Tuning
    {
        float carve     = 0.9f;   // share of the fog removed inside a fresh hole
        float radius    = 1.0f;   // scale on the missile model's radius
        float hold      = 0.45f;  // seconds a hole stays open before it refills
        float refill    = 4.0f;   // seconds the refill takes
        float heat      = 0.35f;  // how much a fire spell's hole widens as it refills
        float billow    = 0.8f;   // strength of the thick fog rolling in at the edge
        float billowSize = 1.5f;  // size of those billows, yards
        float roll      = 0.6f;   // how fast they roll around the trail, radians per second
        float smoke     = 0.5f;   // how much darker and warmer a fire spell's billows are
        float crater    = 1.3f;   // crater size at the impact, as a share of the hole
    };

    constexpr int kMaxSegments = 64;
    constexpr int kTilesX = 16, kTilesY = 9, kTileSlots = 8;

    /// Samples the missiles, grows the trails and retires refilled segments. Once per frame.
    void Update(const float eye[3], float seconds, const Tuning& tuning);

    /// Writes the pool and the tile lists for this frame's camera; false when the device cannot take
    /// the textures. Rows: (head - eye, radius) (tail - eye, refill progress 0..1)
    /// (billow strength, roll angle, fire share, 1 / billow size) (wind drift xyz, carve).
    bool Publish(IDirect3DDevice9* dev, const float eye[3], const float viewProjRel[16]);

    IDirect3DTexture9* SegmentTexture();
    IDirect3DTexture9* TileTexture();
    int   Live();          // segments in the pool
    float Reach();         // yards from the camera past which no segment reaches
    int   Tracked();       // missiles in flight on the last Update

    void ReleaseTextures();
}
