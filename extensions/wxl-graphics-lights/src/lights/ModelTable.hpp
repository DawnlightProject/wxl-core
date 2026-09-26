// wxl-graphics-lights: the model table, light for models that carry none of their own.
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

#include "Lights.hpp"

#include <cstddef>
#include <cstdint>

// data/model-lights.csv (wxl-forever's tools/gen_model_lights.py), one row per light: model path stem
// (case and slash direction ignored), family, r, g, b, intensity, attenStart, attenEnd, then x, y, z
// in model space, and optionally three more: a tube light's half length in model space (windows,
// long fixtures), lit as an area by its closest point. Each row becomes a light placed by the
// instance's world matrix, for every unlit model instance of the world scene that the table names.
namespace wxl::gfx::lights::table
{
    constexpr const char* kFile = "Extensions\\wxl-graphics-lights\\model-lights.csv";

    struct Stats
    {
        uint32_t rows;        // rows loaded; 0 when the file is missing
        uint32_t instances;   // unlit root instances walked
        uint32_t matched;     // of which the table names the model
        uint32_t tooFar;
        uint32_t accepted;
    };

    /// Emits the table's lights near a point, from instances animated up to maxStale frames ago.
    /// @return how many matched, which may exceed cap; only the first cap are written.
    size_t Collect(const float center[3], float radius, uint32_t maxStale, Light* out, size_t cap, Stats* stats);

    /// FNV-1a over a path stem, blind to case, slash direction and a trailing .m2, .mdx or .wmo:
    /// the identity Light::cookieSource carries.
    uint64_t StemHash(const char* s, size_t maxLen);

    /// The instance's rotation as a quaternion taking world directions into its frame, from a
    /// row-vector placement (world = p * M) with uniform scale; identity and false when mirrored.
    bool RotationQuat(const float* m, float q[4]);

    /// One row as the file gives it, for the host tests and the panel: the parse of one CSV line.
    struct Row
    {
        uint64_t key;          // StemHash of the model column
        char     family[64];   // the family column (a longer name is cut; none is that long)
        float    rgb[3];
        float    intensity;
        float    attenStart, attenEnd;
        float    offset[3];
        float    extent[3];    // half the length of a tube light, model space; zero for a point
    };

    /// Parses one line of the table; false for a comment, the header or a malformed row.
    bool ParseLine(const char* line, Row& out);

    /// Rows loaded (0 before the first Collect, or when the file is missing).
    uint32_t Rows();
}
