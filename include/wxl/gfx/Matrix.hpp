// wxl::gfx::matrix: the 4x4 helpers render passes need (row-major, row-vector convention).
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

#include <cmath>

// Header-only: compiles into each extension, nothing crosses the ABI. The convention is the engine's
// (and D3D9's): a point is a row vector, v' = v * M, so M's last row holds the translation and
// A * B applies A first.
namespace wxl::gfx::matrix
{
    inline void Identity(float out[16])
    {
        for (int i = 0; i < 16; ++i) out[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }

    inline void Copy(const float m[16], float out[16])
    {
        for (int i = 0; i < 16; ++i) out[i] = m[i];
    }

    /// out = a * b. out may not alias a or b.
    inline void Mul4(const float a[16], const float b[16], float out[16])
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c]
                               + a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c];
    }

    inline void Transpose(const float m[16], float out[16])
    {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c)
                out[c * 4 + r] = m[r * 4 + c];
    }

    /// General 4x4 inverse by cofactors; false (out untouched) when singular.
    inline bool Invert4(const float m[16], float out[16])
    {
        float inv[16];
        inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
        inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
        inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
        inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
        inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
        inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
        inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
        inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
        inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
        inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
        inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
        inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
        inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
        inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
        inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
        inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

        const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
        if (!(std::fabs(det) > 1e-30f)) return false;
        const float s = 1.0f / det;
        for (int i = 0; i < 16; ++i) out[i] = inv[i] * s;
        return true;
    }

    /// Writes the columns of m as four float4 rows: the dp4 form a shader applies (c[k] . v).
    inline void Columns(const float m[16], float out[4][4])
    {
        for (int k = 0; k < 4; ++k)
            for (int r = 0; r < 4; ++r)
                out[k][r] = m[r * 4 + k];
    }

    /// A translation by (x, y, z).
    inline void Translation(float x, float y, float z, float out[16])
    {
        Identity(out);
        out[12] = x;
        out[13] = y;
        out[14] = z;
    }

    /// p' = (p, 1) * m with the divide by w; false when w is (near) zero.
    inline bool TransformPoint(const float m[16], const float p[3], float out[3])
    {
        const float x = p[0] * m[0] + p[1] * m[4] + p[2] * m[8]  + m[12];
        const float y = p[0] * m[1] + p[1] * m[5] + p[2] * m[9]  + m[13];
        const float z = p[0] * m[2] + p[1] * m[6] + p[2] * m[10] + m[14];
        const float w = p[0] * m[3] + p[1] * m[7] + p[2] * m[11] + m[15];
        if (!(std::fabs(w) > 1e-12f)) return false;
        out[0] = x / w;
        out[1] = y / w;
        out[2] = z / w;
        return true;
    }

    /**
     * @brief Whether a view matrix takes world points (it carries the camera's translation) or
     *        camera-relative ones.
     *
     * The engine hands both kinds depending on the path; a translation longer than a yard means world
     * points. The test matches the one the core's draw path uses.
     */
    inline bool ViewHasTranslation(const float view[16])
    {
        return std::fabs(view[12]) + std::fabs(view[13]) + std::fabs(view[14]) > 1.0f;
    }

    /// The camera-relative view: (r, 1) * out = the engine's view space, r = point - eye.
    inline void CameraRelativeView(const float view[16], const float eye[3], float out[16])
    {
        if (!ViewHasTranslation(view))
        {
            Copy(view, out);
            return;
        }
        float toWorld[16];
        Translation(eye[0], eye[1], eye[2], toWorld);
        Mul4(toWorld, view, out);
    }

    /// The camera-relative view-projection: (r, 1) * out = clip, r = point - eye.
    inline void CameraRelativeViewProj(const float view[16], const float proj[16], const float eye[3],
                                       float out[16])
    {
        float rel[16];
        CameraRelativeView(view, eye, rel);
        Mul4(rel, proj, out);
    }
}
