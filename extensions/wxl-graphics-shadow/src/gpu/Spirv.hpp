// wxl-graphics-shadow: the compute shaders' SPIR-V and the D3D9 shaders' HLSL sources, embedded in the DLL.
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

#include <cstddef>
#include <cstdint>

// Compiled offline by DXC (cs_6_0, -spirv, Vulkan 1.3: SPIR-V 1.6) into Spirv.gen.cpp, which is
// committed so a build needs no shader compiler (cmake/CompileShaders.cmake). The D3D9 shaders
// (shaders/d3d9) are compiled at runtime by wxl-graphics-extend from the sources embedded here.
namespace wxl::gfx::shadow::spirv
{
    struct Module
    {
        const uint32_t* words;
        size_t          bytes;   ///< a multiple of 4
        const char*     name;    ///< "shadow.mask", for the log
    };

    /// The module of a SH_PIPE_* pipeline; words null when out of range.
    const Module& Get(uint32_t pipe);

    /// Modules in the table (SH_PIPE_COUNT when the table matches shared.h).
    uint32_t Count();

    /// A D3D9 shader's source by name ("overlay.ps", "caster.vs", "caster.ps"), NUL-terminated; null
    /// when unknown.
    const char* D3d9Source(const char* name, size_t* size);
}
