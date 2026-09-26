// wxl-graphics-fog: the compute shaders' SPIR-V and the composite's HLSL source, embedded in the DLL.
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

#include <cstddef>
#include <cstdint>

// Compiled offline by DXC (cs_6_0, -spirv, Vulkan 1.3: SPIR-V 1.6) into Spirv.gen.cpp, which is
// committed so a build needs no shader compiler (cmake/CompileShaders.cmake). The composite is ps_3_0,
// compiled at runtime by wxl-graphics-extend from the source embedded here.
namespace wxl::gfx::fog::spirv
{
    struct Module
    {
        const uint32_t* words;
        size_t          bytes;   ///< a multiple of 4
        const char*     name;    ///< "fog.march", for the log
    };

    /// The module of a FOG_PIPE_* pipeline; words null when out of range.
    const Module& Get(uint32_t pipe);

    /// Modules in the table (FOG_PIPE_COUNT when the table matches shared.h).
    uint32_t Count();

    /// The composite pixel shader's source (shaders/apply.ps.hlsl), NUL-terminated.
    const char* ApplySource(size_t* size);
}
