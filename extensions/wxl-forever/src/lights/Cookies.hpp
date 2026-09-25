// wxl-forever lights: light cookies, the shadow a lantern's own cage throws, baked per model light.
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

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

// 5.tools/forever-bake renders, for each light of the model table, the transmittance of the
// model's own geometry seen from the light as a cube map in the model's local frame
// (Textures\Forever\Cookies\, manifest keyed "model|light"). Each frame this service resolves the
// gathered lights to their cookie, keeps the most important ones resident in one atlas (L8, or
// X8R8G8B8 when any cookie carries its glass's tint; each cookie a 4 x 2 grid of faces, the omni
// shadow layout, four cookies to an atlas row), measures what each cookie averages to, and gives
// every light its atlas cell, its mean and the rotation from world directions into its model's
// frame, published with the cluster data (Clusters.cpp, rows past the cluster lists). Shaders
// sample it with shaders/cookies.hlsli, inside their per-light loops.
namespace wxl::forever::lights::cookies
{
    struct Settings
    {
        int   enabled  = 1;      // resolve and publish cookies at all
        float strength = 1.0f;   // 0 no effect, 1 the baked transmittance
        float floor    = 0.10f;  // least transmittance a cookie may give (light leaks and bounces)
        float flame    = 0.35f;  // share of the cookie a flame light takes (a flame is no point)
        float tint     = 1.0f;   // how much of a tinted cookie's glass colour its light takes (0 luminance only)
        int   budget   = 32;     // cookies resident at once (atlas cells)
        int   debug    = 0;      // 0 off, 1 draw the atlas in a corner, 2 consumers show the factor
    };

    Settings& Get();

    /// Reads WXL_FOREVER_COOKIES_*; the manifest loads on first use. Called once at load.
    void Install();

    /// Per frame, after the gather and before Publish: resolves each light's cookie, streams the most
    /// important ones into the atlas (those within 30 yards first, kept), measures the mean of any
    /// wanted cookie not yet loaded, and writes cookieCell, cookieOpen and cookieTint into the lights.
    /// Cheap when disabled (clears the cells).
    void Frame(IDirect3DDevice9* dev, Light* lights, int count, const float eye[3], uint32_t frame);

    /// Binds the atlas (or the neutral white texture) to a sampler stage, bilinear, clamped.
    void Bind(IDirect3DDevice9* dev, unsigned stage);

    /// Shader constants (shaders/cookies.hlsli):
    ///   cookieC  face width, face height (atlas texture coordinates), strength (0 = cookies off), floor
    ///   cookieD  half a texel in u, half a texel in v, debug view (0 or 1), share a flame is spared
    ///   cookieE  share of a texel's glass colour a light takes, 0, 0, 0
    void Constants(float cookieC[4], float cookieD[4], float cookieE[4]);

    /// One line for a panel: manifest rows, lights with a cookie this frame, resident, pending.
    const char* Status();

    /// The Cookies tab of the lights panel.
    void Panel();

    /// Frees the atlas; it is rebuilt on the next Frame.
    void ReleaseTextures();
}
