// gbuffer: the contract between the world pass's extra targets, the rewritten engine shaders
// (extensions/wxl-graphics-lights/tools/bls_gbuffer.py; before it wxl-forever's bls_normals.py) and
// whoever consumes them.
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

#include "game/Binding.hpp"
#include "offsets/engine/Shader.hpp"

/**
 * @brief World-pass G-buffer: normals in render target 1, albedo in render target 2, and the light
 *        buffer the older rewrite read back.
 *
 * Targets. Supply them in OnWorldSceneBegin (WorldSceneBeginArgs): *normalTarget (A8R8G8B8, the
 * world target's size), optionally *albedoTarget (A8R8G8B8, taken only with a normal target) and
 * *colorOverride (A16B16G16R16F). They come back in OnWorldSceneEnd (WorldSceneEndArgs::normalTarget
 * / albedoTarget / sceneColor), unbound. Both G-buffer targets are cleared to 0 before the pass and
 * written only by opaque and alpha-tested draws (blending closes their write masks).
 *
 * Normal target (render target 1), written by the rewritten M2 (Combiners*), WMO (MapObj*) and grass
 * (DetailDoodad) pixel shaders at shadow tiers >= 1 (extShadowQuality >= 1), and by terrain once its
 * vertex shader is rewritten to hand its view-space normal over (texcoord7):
 *   rgb = view-space normal * 0.5 + 0.5 (the engine's view space: view matrix from shadows::Get or
 *         camera::GetView, applied to camera-relative positions)
 *   a   = 0    nothing written (liquids, particles, blended draws, shadow tier 0)
 *         0.5  normal only
 *         1    normal, and the surface already added lightBuffer * albedo (the older rewrite only)
 *
 * Albedo target (render target 2), written by the same shaders:
 *   rgb = the material's colour where it meets the vertex lighting: its textures after the
 *         combiner's stages (Mod, Mod2x, the terrain's layer blend), before the lighting and the
 *         engine's shadow factor, in the engine's gamma working space
 *   a   = material code v / 255: kind = v >> 6 (0 none, 1 model or grass, 2 building, 3 terrain),
 *         gloss = (v & 63) / 63 (the terrain's specular mask; 0 elsewhere)
 *
 * Light buffer (the older bls_normals.py rewrite only; bls_gbuffer.py writes no light code). During the world pass the rewritten shaders add min(albedo^2 * tex(s12, uv).rgb *
 * c30.x * fog, c30.y) to their colour in linear space (gamma 2: out = sqrt(colour^2 * c30.z + added)),
 * with uv = (dp4(P1, c26), dp4(P1, c27)) / dp4(P1, c29), P1 = (view-space position, 1), and only where
 * w > 1e-4, uv lies inside [0, 1] and the texel's alpha is 0.5 (the provider's marker); alpha 1 then.
 * The provider binds last frame's buffer on sampler kLightBufferSampler and sets c26..c29 to "this
 * frame's view space -> last frame's buffer texture space" (rows applied as dp4, the divide by w left
 * to the shader) and c30 = (strength, cap per channel, scene share 1, 0). The core writes c30 = 0
 * before each world pass, so without a provider nothing is added and alpha stays 0.5. Set the
 * constants through SetEnginePixelConstants, not the device directly: the engine uploads its own
 * constant cache over dirty ranges and would overwrite a value written behind its back; it also
 * uploads only values that changed.
 */
namespace wxl::game::gbuffer
{
    constexpr uint32_t kLightBufferSampler = 12;
    constexpr uint32_t kLightBufferRows    = 26;  // c26..c29
    constexpr uint32_t kLightBufferParams  = 30;  // c30.x = enable

    /// Writes pixel-shader constants through the engine's own cache (CGxDevice shader-constant set,
    /// target 4 = pixel), so its per-draw flush uploads them instead of overwriting them.
    inline void SetEnginePixelConstants(uint32_t firstRegister, const float* data, uint32_t count)
    {
        namespace so = wxl::offsets::engine::shader;
        Native<so::ShaderConstantsSetHelperFn>(so::kShaderConstantsSet)(4, int(firstRegister), data, int(count));
    }
}
