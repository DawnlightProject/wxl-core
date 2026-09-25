// wxl-graphics-extend: the pass-through vertex shader and the clip-space quad of full-screen passes.
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

namespace wxl::gfx::fullscreen
{
    /// IDirect3DVertexShader9*, borrowed for the process; null when the device refused it.
    void* VertexShader(void* device);

    /// Binds VertexShader, FVF XYZW and a width x height viewport, and draws the quad.
    void Draw(void* device, uint32_t width, uint32_t height);
}
