// wxl-graphics-extend: the blue-noise bake (void-and-cluster, four channels) on a worker thread.
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

// Private to the textures folder. kSize x kSize texels, A8R8G8B8, each channel a void-and-cluster
// rank map (Ulichney): every value 0..255 appears equally often and neighbours differ as much as
// possible, so an offset read from it leaves fine grain instead of a lattice. The bake takes a few
// seconds of one core; nothing waits for it.
namespace wxl::gfx::textures::bluenoise
{
    constexpr int kSize = 128;

    /// Starts the bake on a detached worker thread; idempotent, any thread.
    void Start();

    /// The finished texels (kSize * kSize, row-major), or null while the bake runs. Once non-null the
    /// buffer is immutable and lives for the process.
    const uint32_t* Texels();

    /// Whether the bake finished, and how long it took.
    bool Ready(uint32_t& bakeMs);
}
