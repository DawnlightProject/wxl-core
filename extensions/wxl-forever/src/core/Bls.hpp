// wxl-forever: the client's BLS shader files, read into their permutations.
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
#include <string>
#include <vector>

// The layout CGxDevice::IShaderLoad (0x00684970) and CGxShader::Load (0x00689A70) read, as
// tools/bls.py documents field by field: a 12-byte header (magic "HSXG", version 0x00010003,
// permutation count), then per permutation four mask fields, the bytecode size, the D3D9 bytecode
// and padding to four bytes. Our programs and the client's share it.
namespace wxl::forever::bls
{
    constexpr uint32_t kMagic   = 0x47585348;
    constexpr uint32_t kVersion = 0x00010003;

    struct Permutation
    {
        uint32_t    inputs;     // CGxShader+0x3C
        uint32_t    outputs;    // CGxShader+0x40
        uint16_t    samplers;   // CGxShader+0x44
        uint16_t    extra;      // CGxShader+0x46
        std::string code;       // D3D9 token stream
    };

    /// Reads every permutation; false (and out empty) when the bytes are not a whole BLS file.
    bool Read(const std::string& bytes, std::vector<Permutation>& out);
}
