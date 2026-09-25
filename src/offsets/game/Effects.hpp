// In-flight missiles and live particle emitters: the engine's own lists and the records behind them.
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

// INTERNAL to the core.
namespace wxl::offsets::game::effects
{
    // --- CMissile (0x1C8 bytes, heap "CMissile", constructor 0x00701F50) -------------------------
    /// CMissile::s_inFlightMissileList: head of the singly linked in-flight list. Walked every frame
    /// by CMissile::UpdateStaticLists (0x00703B00, from CGWorldFrame::OnWorldUpdate), which calls
    /// CalcPosition (0x007015D0) on each.
    constexpr uintptr_t kInFlightMissiles = 0x00CA0B58;
    constexpr size_t kOffMissileNext      = 0x164;
    constexpr size_t kOffMissileCaster    = 0x00;  // uint64 guid
    constexpr size_t kOffMissileTarget    = 0x08;  // uint64 guid
    constexpr size_t kOffMissileModel     = 0x18;  // -> CM2Model instance, null when modelless
    constexpr size_t kOffMissileSpell     = 0x1C;  // uint32, matched with the cast index by StartFizzling
    constexpr size_t kOffMissileSpeed     = 0x24;  // float yd/s; zero means not moving (CalcPosition returns)
    constexpr size_t kOffMissilePosition  = 0x2C;  // float[3], world, advanced by CalcPosition
    constexpr size_t kOffMissileStart     = 0x38;  // float[3], launch point
    constexpr size_t kOffMissileImpact    = 0x44;  // float[3], impact point when no transport
    constexpr size_t kOffMissileTransport = 0x10;  // uint64 guid; nonzero -> impact is kOffMissileImpactT
    constexpr size_t kOffMissileImpactT   = 0x50;  // float[3], impact point carried by the transport
    constexpr size_t kOffMissileFlags     = 0x68;  // 0x10000: ballistic trajectory
    constexpr uint32_t kMissileBallistic  = 0x10000;

    // --- particle emitters on an M2 instance ---------------------------------------------------
    /// Header M2Array<M2Particle>, pointer-fixed by M2ModelHeader::ReadParticleEmitters (0x0083AF90).
    constexpr size_t kOffHeaderEmitterCount = 0x128;
    constexpr size_t kOffHeaderEmitters     = 0x12C;
    constexpr size_t kOffHeaderTextureCount = 0x50;
    constexpr size_t kOffHeaderTextures     = 0x54;  // stride 0x10; filename M2Array at +0x08
    constexpr size_t kTextureStride         = 0x10;
    constexpr size_t kOffTextureName        = 0x08;
    constexpr size_t kOffHeaderBoneCount    = 0x2C;
    constexpr size_t kOffHeaderBounds       = 0xA0;  // float[6], model-space min then max

    /// M2Particle as the 3.3.5a reader keeps it (stride 0x1DC). Offsets checked against client files.
    constexpr size_t kEmitterRecordStride = 0x1DC;
    constexpr size_t kOffEmPosition   = 0x08;  // float[3], bone space
    constexpr size_t kOffEmBone       = 0x14;  // uint16
    constexpr size_t kOffEmTexture    = 0x16;  // uint16, low 5 bits index the texture array
    constexpr size_t kOffEmBlend      = 0x28;  // uint8: 0 opaque, 1 alpha key, 2 alpha, 4 additive...
    constexpr size_t kOffEmType       = 0x29;  // uint8: 1 plane, 2 sphere, 3 spline
    constexpr size_t kOffEmScaleBlock = 0x124; // FBlock<C2Vector>: timestamps, then values {count, ptr}
    constexpr size_t kOffEmScaleVary  = 0x134; // float[2]

    /// On the instance: sampled emitter cells (stride 0x88, one per emitter, a track state of 0xC
    /// per field with its value at +8) and the emitter object array.
    constexpr size_t kOffInstEmitterCells = 0x2C0;
    constexpr size_t kEmitterCellStride   = 0x88;
    constexpr size_t kOffCellSpeed        = 0x08;
    constexpr size_t kOffCellLifespan     = 0x44;
    constexpr size_t kOffCellRate         = 0x50;
    constexpr size_t kOffCellEnabled      = 0x80;  // uint8
    constexpr size_t kOffInstEmitters     = 0x2C4;
    constexpr size_t kOffEmitterLiveCount = 0x50;  // on the emitter object: particles alive

    /// On the instance: bone palette (stride 0x40, view space). CM2Model::AnimateST places model
    /// lights with pos x palette[bone] x scene+0xC4 (disassembly 0x00828AC2..0x00828AF4); the same
    /// product places any bone-space point in world space.
    constexpr size_t kOffInstBonePalette  = 0x98;
    constexpr size_t kOffScenePaletteToWorld = 0xC4;
    constexpr size_t kOffModelFileSize    = 0x16C;
}
