// wxl-graphics-extend: the client's BLS shader container, read and written.
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

// The layout CGxDevice::IShaderLoad (0x00684970) and CGxShader::Load (0x00689A70) read: a 12-byte
// header (magic "HSXG", version 0x00010003, permutation count), then per permutation four mask
// fields (inputs u32, outputs u32, samplers u16, extra u16), the bytecode size (u32), the D3D9
// bytecode and padding to four bytes. The file ends exactly after the last permutation.
namespace wxl::gfx::bls
{
    constexpr uint32_t kMagic   = 0x47585348;   // "HSXG"
    constexpr uint32_t kVersion = 0x00010003;

    int Count(const void* bytes, size_t size);
    int Permutation(const void* bytes, size_t size, uint32_t index, WXL_GfxBlsPermutation* out);
    int Write(const WXL_GfxBlsPermutation* permutations, uint32_t count, WXL_ByteSink* out);
}
