// wxl-forever: a tileable blue-noise texture for per-pixel sample offsets and dither.
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

// kSize x kSize texels, four independent channels, each a void-and-cluster rank map (Ulichney):
// every value 0..255 appears equally often and neighbours differ as much as possible, so an offset
// read from it leaves fine grain instead of a lattice. Baked once on a worker thread at load.
namespace wxl::forever::bluenoise
{
    constexpr int kSize = 128;

    /// Starts the bake on a worker thread; called at load.
    void StartBake();

    /// The texture (A8R8G8B8, managed pool), uploaded once the bake is done; null until then.
    IDirect3DTexture9* Get(IDirect3DDevice9* dev);

    void Release();
}
