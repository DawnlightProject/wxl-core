// wxl-forever fog: a small heightfield around the camera, so the fog can hug the ground.
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

struct IDirect3DDevice9;
struct IDirect3DTexture9;

// kCells x kCells cells of kCellSize yards, addressed toroidally: world cell (i, j) lives at texel
// (i mod kCells, j mod kCells), so walking only re-queries the row or column that came into range.
// A few ground traces per tick fill it nearest-first. The texture holds the ground height in r and
// the mean ground height over the surrounding 5x5 cells in g, which is what valley pooling compares.
namespace wxl::forever::fog::terrain
{
    constexpr int   kCells    = 64;
    constexpr float kCellSize = 4.0f;

    /// Queries up to budget cells around the camera, nearest stale ones first. Call on the tick.
    void Update(const float eye[3], int budget);

    /// The texture, uploaded if anything changed; null when float textures are unavailable.
    IDirect3DTexture9* Texture(IDirect3DDevice9* dev);

    /// The ground height used where no cell has been measured yet.
    float Fallback();

    /// Cells measured and currently in range, out of kCells * kCells.
    int Filled();

    void Release();
}
