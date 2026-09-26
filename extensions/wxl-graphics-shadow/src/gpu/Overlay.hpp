// wxl-graphics-shadow: the debug view drawn over the finished frame (D3D9).
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

#pragma once

#include "wxl/GraphicsExtendApi.h"

#include <cstdint>

namespace wxl::gfx::shadow::overlay
{
    /// Before the world pass: the A16B16G16R16F texture the compute pass copies its debug view into
    /// (IDirect3DTexture9*, DEFAULT pool); null when refused.
    void* Prepare(void* device, uint32_t width, uint32_t height);

    /// Blends the debug view over the frame's target.
    void Draw(const WXL_GfxFrame& frame);

    /// Before a device reset.
    void Release();
}
