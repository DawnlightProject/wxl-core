// wxl-graphics-extend: reading the world pass's G-buffer normal target.
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

#ifndef WXL_GFX_GBUFFER_HLSLI
#define WXL_GFX_GBUFFER_HLSLI

// The A8R8G8B8 target the engine's rewritten materials write during the world pass
// (WXL_GfxFrame::normalTexture; the contract is src/game/GBuffer.hpp):
//   rgb  the view-space normal * 0.5 + 0.5 (the engine's view space, WXL_GfxView::view applied to
//        camera-relative positions)
//   a    0    nothing was written here: terrain, liquids, particles, blended draws, shadow tier 0
//        0.5  a normal
//        1    a normal, and the material already added the light buffer times its albedo, so a
//             lighting pass must not add it again
// The tests below sit between the three values, so the 8-bit rounding of 0.5 never matters.

// The view-space normal, unit length; meaningless where WxlGBufferHasNormal is false.
float3 WxlGBufferNormal(float4 texel)
{
    return normalize(texel.rgb * 2.0 - 1.0);
}

bool WxlGBufferHasNormal(float a)
{
    return a > 0.25;
}

bool WxlGBufferEngineLit(float a)
{
    return a > 0.75;
}

#endif
