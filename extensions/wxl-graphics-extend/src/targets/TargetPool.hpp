// wxl-graphics-extend: render target textures kept for consumers across resizes and device resets.
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

#include "wxl/GraphicsExtendApi.h"

#include <cstddef>
#include <cstdint>

// A handle names a declared target; the texture behind it is created on first Get, released on
// device loss, re-created at the new size when a followed size changes. Handles are never reused.
namespace wxl::gfx::targets
{
    uint32_t Create(const WXL_GfxTargetDesc* desc);
    int      Get(uint32_t handle, void* device, WXL_GfxTarget* out);
    void     Destroy(uint32_t handle);

    /// Releases every texture (DEFAULT pool) before a device reset.
    void OnDeviceLost();

    /// Live textures and their approximate bytes.
    void Stats(uint32_t& count, uint32_t& bytes);

    /// For the panel: declared targets, and one line describing target index.
    size_t Count();
    bool   Describe(size_t index, char* buf, size_t cap);
}
