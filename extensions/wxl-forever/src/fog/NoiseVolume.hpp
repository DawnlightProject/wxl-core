// wxl-forever fog: a tileable 3D noise volume baked once on the CPU, sampled by the fog's density.
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
struct IDirect3DVolumeTexture9;

// Channels, each normalised to 0..1 over the volume and tiling in all three axes:
//   r  Perlin-Worley: Perlin fbm pushed down where Worley cells are thin -- the billowing shape
//   g  Worley fbm at twice the base frequency -- erosion detail
//   b  billow fbm (1 - |Perlin|) -- rounded, cauliflower lumps
//   a  plain Perlin fbm -- domain warp
// One tile holds four base-frequency cycles. The full mip chain is kept, so a sample can be taken
// already averaged over the footprint it stands for.
namespace wxl::forever::fog::noise
{
    constexpr int kSize = 64;

    /// Starts the CPU bake on a worker thread; called at load so the volume is ready by the world.
    void StartBake();

    /// Returns the volume, uploading it once the bake is done; null until then. Managed pool.
    IDirect3DVolumeTexture9* Get(IDirect3DDevice9* dev);

    void Release();
}
