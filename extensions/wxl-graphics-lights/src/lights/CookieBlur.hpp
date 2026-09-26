// wxl-graphics-lights: a baked cube cookie softened by the size of its light's source.
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
#include <vector>

// forever-bake renders a lamp's own model from its light as seen from a point. A real flame or bulb
// has a size: a bar at distance b from a source of radius r0 throws a penumbra about 2 r0 / b radians
// wide (0.3 rad for a 2 cm flame behind bars a tenth of a yard away), where the bake draws a hard
// edge. On a wall beside a lantern the point bake prints the lantern's own frame, magnified and
// stair-stepped by the cube's texels. The cookie service softens each cookie once, as it fills an
// atlas cell: every texel becomes the mean of the disc of directions the source spans seen from the
// cage. The work is in direction space, with bilinear fetches that cross the cube's faces, so the
// seams stay continuous. Plain C++ without the platform's headers, so a host test compiles it alone.
namespace wxl::gfx::lights::cookies
{
    /// The widest blur, radians: past it the pattern is gone into its mean anyway.
    constexpr float kMaxSoftAngle = 0.6f;

    /// The direction (not unit length) through the point (s, t), each -1..1, of cube face `face`, in
    /// D3D's layout (+X -X +Y -Y +Z -Z) exactly as cookies.hlsli's CookieUv reads it.
    void CubeDirection(unsigned face, float s, float t, float d[3]);

    /// The face a direction falls on and its coordinates there, each 0..1 across the face: CookieUv's
    /// twin, without its half-texel inset.
    unsigned CubeFace(const float d[3], float& u, float& v);

    /**
     * @brief The blur's angle for a source of radius sourceRadius behind a cage cageDistance away (the
     *        same units): softness x 2 x sourceRadius / cageDistance, at least one texel of a face of
     *        faceTexels, at most kMaxSoftAngle.
     */
    float SoftAngle(float sourceRadius, float cageDistance, float softness, unsigned faceTexels);

    /// What the blur keeps between calls, for its capacity.
    struct BlurScratch
    {
        std::vector<std::vector<float>> levels;   // the source's box pyramid, padded faces
        std::vector<float> blurred;               // the blur at its working size, padded faces
    };

    /**
     * @brief Softens a cube cookie by a source of angular diameter theta.
     *
     * Every output texel is the mean of 8 to 32 taps over the disc of that diameter around its
     * direction (a Vogel spiral, the same for every texel, so the result is a smooth convolution and
     * not noise). The taps read a coarser level of the source where they lie far apart, and a wide blur
     * is worked at a smaller size and brought up to outSize with the same cross-face filter.
     * @param in       six square faces of inSize texels, bpp bytes each (1 luminance, 4 BGRX), face f
     *                 at f * inSize * inSize * bpp.
     * @param out      six faces of outSize texels, the same bpp and layout; X is written as 255.
     */
    void BlurCube(const uint8_t* in, unsigned inSize, unsigned bpp, float theta, uint8_t* out, unsigned outSize,
                  BlurScratch& scratch);
}
