// wxl-graphics-fog: the host's view of the shaders' constant buffer and bindings (shaders/shared.h).
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

#include "../../shaders/shared.h"

#include <cstdint>
#include <cstring>

namespace wxl::gfx::fog::gpu
{
    /// Row for row the shaders' cbuffer: every member a float4 or an array of them.
    struct FogConstants
    {
#define FOG_HOST1(name) float name[4];
#define FOG_HOSTN(name, n) float name[n][4];
        FOG_CONSTANTS(FOG_HOST1, FOG_HOSTN)
#undef FOG_HOST1
#undef FOG_HOSTN
    };
    static_assert(sizeof(FogConstants) % 16 == 0, "the constant buffer is rows of 16 bytes");

    inline void Set(float row[4], float x, float y, float z, float w)
    {
        row[0] = x;
        row[1] = y;
        row[2] = z;
        row[3] = w;
    }

    /// A 32-bit pattern in a float slot (the shaders read it back with asuint).
    inline float Bits(uint32_t v)
    {
        float f;
        std::memcpy(&f, &v, sizeof f);
        return f;
    }

    struct PushData
    {
        uint32_t a[4];
        float    b[4];
    };
    static_assert(sizeof(PushData) == FOG_PUSH_BYTES, "push constants match the shaders");
}
