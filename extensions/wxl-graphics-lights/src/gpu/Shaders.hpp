// wxl-graphics-lights: the compiled compute shaders (SPIR-V) and the D3D9 shaders' sources, embedded by
// cmake/CompileShaders.cmake into Shaders.gen.cpp.
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
#include <cstring>

namespace wxl::gfx::lights::shaders
{
    struct Module
    {
        const uint32_t* words;   // null when the module was not compiled (see CompileShaders.cmake)
        size_t          bytes;
        const char*     name;
    };

    /// Module `pipe` (LIGHTS_PIPE_*, shaders/compute/shared.h).
    const Module& Get(uint32_t pipe);
    uint32_t Count();

    /// An embedded source by name ("composite.ps.hlsl", "resolve.ps.hlsl", "wxl/lights/hdr.hlsli"); null
    /// when unknown.
    const char* Text(const char* name, size_t* size);
}
