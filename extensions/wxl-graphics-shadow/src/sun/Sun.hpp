// wxl-graphics-shadow: the sun and the moon -- where they are, the direction the engine's cascades are
// rendered along (the core's wxl.shadowlight adjuster, claimed here: a lower, truer sun by day, the
// moon by night) and those cascades read back for the uniform block.
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

namespace wxl::gfx::shadow::sun
{
    struct Bodies
    {
        bool  valid = false;
        float toSun[3] = { 0.0f, 0.0f, 1.0f }, toMoon[3] = { 0.0f, 0.0f, 1.0f };
        float sunWeight = 0.0f, moonWeight = 0.0f;
        int   night = 0;   ///< the moon is the higher body
    };

    /// Once a frame on the render thread: reads the sky, eases the bodies and keeps the adjuster.
    void Update(float dt);

    const Bodies& Current();

    /// The engine's cascades for camera-relative points, finest first.
    struct Cascades
    {
        int      count = 0;
        int      body = -1;                 ///< 0 rendered along the sun, 1 the moon, -1 neither
        uint32_t size = 0;
        void*    textures[4] = {};          ///< IDirect3DTexture9*, R32F: light depth x depthScale
        float    rows[4][3][4] = {};        ///< (rel, 1) -> (u', v', depth)
        float    extent[4] = {};            ///< half extent, yards
        float    towardsLight[3] = { 0.0f, 0.0f, 1.0f };
        float    depthScale = 1.0f / 4000.0f;
        int      mode = 0;
        bool     hwPcf = false;
    };

    /// Reads the maps the engine rendered this frame (after the world pass). False when none are usable
    /// (extShadowQuality 0, hardware PCF maps, or not rendered this frame).
    bool ReadCascades(const float eye[3], Cascades& out);

    /// The adjuster goes (the engine's own direction comes back).
    void Shutdown();

    const char* Status();
}
