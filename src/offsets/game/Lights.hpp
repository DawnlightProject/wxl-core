// Scene light sources: the world M2 scene's evaluated model lights and the WMO roots' MOLT tables.
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

// INTERNAL to the core. The only live point lights 3.3.5a keeps are M2 model lights. Each loaded
// instance owns one CM2Light per M2Light record; CM2Model::AnimateMT (0x0082F0F0) samples their
// tracks, CM2Model::AnimateST (0x00828A00) places them every frame the model animates, and
// CM2Light::Link (0x00834C70) files point lights into the scene grid that CM2Scene::SelectLights
// (0x0081E400) reads to light nearby models. There is no separate spell or player light: spell
// glows are M2 lights on the spell's models. WMO MOLT lights never become live lights (see below).
namespace wxl::offsets::game::lights
{
    // --- world M2 scene ---------------------------------------------------------------------------
    /// CWorldScene::s_m2Scene: the CM2Scene every world model lives in (created by M2CreateScene).
    constexpr uintptr_t kWorldM2Scene = 0x00CD754C;

    /// Head of the scene's list of every attached model, loaded or not (CM2Model::AttachToScene,
    /// 0x00834540). The animate list at +0x28 is consumed by CM2Scene::Animate, empty by render.
    constexpr size_t kOffSceneModelHead = 0x08;
    /// On a model: next model in that list.
    constexpr size_t kOffModelSceneNext = 0x0C;
    /// uint32 on the scene: frame counter, incremented at the top of CM2Scene::Animate (0x00821A20).
    constexpr size_t kOffSceneFrame = 0x14;
    /// On the scene: -> CM2Light*[64*64], the point-light grid; cells are 20 yd (scale 0.05 at
    /// 0x00AF59D4), indexed (floor(y*0.05)&63)*64 + (floor(x*0.05)&63), chained through kOffLightNext.
    constexpr size_t kOffScenePointGrid = 0x24;

    // --- per-instance light records (CM2Model) --------------------------------------------------
    /// uint32 on the instance: bit 0 = loaded; the records below exist only once it is set.
    constexpr size_t   kOffInstFlags      = 0x10;
    constexpr uint32_t kInstFlagLoaded    = 0x1;
    /// uint32 on the instance: scene frame it was last animated on (CM2Model::AnimateMT writes it).
    constexpr size_t kOffInstLastAnimFrame = 0x3C;
    /// On the instance: -> parent instance, null for a root (every placed doodad).
    constexpr size_t kOffInstParent = 0x48;
    /// On the instance: inline model-to-world matrix, row vectors, scale included.
    constexpr size_t kOffInstPlacement = 0xB4;
    /// On the instance: -> light records, one per M2Light, stride kRecordStride.
    constexpr size_t kOffInstLightRecords = 0x1D0;
    /// On the instance: -> runtime model; its +0x150 is the in-place parsed .m2 header.
    constexpr size_t kOffInstModel  = 0x2C;
    constexpr size_t kOffModelHeader = 0x150;
    /// On the runtime model: its path stem, inline and NUL-terminated, up to the header field.
    constexpr size_t kOffModelPathStem = 0x3C;
    /// On the instance: -> runtime bone states, stride 0xAC; +0x44 is the uint16 animation slot the
    /// track evaluators index the per-animation arrays with.
    constexpr size_t kOffInstBoneStates  = 0x94;
    constexpr size_t kBoneStateStride    = 0xAC;
    constexpr size_t kOffBoneStateAnimIdx = 0x44;

    /// Header M2Array<M2Light>: count and pointer (pointer-fixed by the loader).
    constexpr size_t kOffHeaderLightCount = 0x108;
    constexpr size_t kOffHeaderLights     = 0x10C;

    /// M2Light record in the file (stride 0x9C). The client samples only ambient colour/intensity,
    /// diffuse colour/intensity and visibility each frame; the attenuation tracks are never read.
    constexpr size_t kDefStride          = 0x9C;
    constexpr size_t kOffDefType         = 0x00; // uint16: 0 directional, 1 point
    constexpr size_t kOffDefBone         = 0x02; // uint16
    constexpr size_t kOffDefAttenStart   = 0x60; // M2Track<float>
    constexpr size_t kOffDefAttenEnd     = 0x74; // M2Track<float>
    /// M2Track: values M2Array<M2Array<T>>, one inner array per animation slot.
    constexpr size_t kOffTrackValuesCount = 0x0C;
    constexpr size_t kOffTrackValuesPtr   = 0x10;
    constexpr size_t kInnerArrayStride    = 0x08; // {count, pointer}

    /// Per-instance light record (stride 0xD4), laid out by CM2Model::InitializeLoaded (0x00832EA0)
    /// and filled each frame by CM2Model::AnimateMT and CM2Model::AnimateST.
    constexpr size_t kRecordStride         = 0xD4;
    constexpr size_t kOffRecDiffuseColor   = 0x28; // float[3], diffuse colour track value
    constexpr size_t kOffRecDiffuseIntensity = 0x3C; // float, diffuse intensity track value
    constexpr size_t kOffRecVisible        = 0x60; // uint8, visibility track value
    constexpr size_t kOffRecEnabled        = 0x64; // uint32, per-instance enable (0x008240F0)
    constexpr size_t kOffRecLight          = 0x68; // the embedded CM2Light

    // --- CM2Light (0x6C bytes; constructor 0x00834A40) -------------------------------------------
    constexpr size_t kOffLightStamp    = 0x04; // uint32: scene frame it was last evaluated on
    constexpr size_t kOffLightType     = 0x08; // uint32: 0 directional, 1 point
    constexpr size_t kOffLightPosition = 0x0C; // float[3], world space
    constexpr size_t kOffLightAmbient  = 0x30; // float[3], colour x intensity x instance factor
    constexpr size_t kOffLightDiffuse  = 0x3C; // float[3], colour x intensity x instance factor
    /// float[3]: falloff terms handed to the shader as c25..c27 (CShaderEffect::ComputeLocalLights,
    /// 0x00872900) and to D3D as Attenuation0/1/2; constant 0, 0.7, 0.03 from the constructor.
    constexpr size_t kOffLightAtten    = 0x54;
    constexpr size_t kOffLightVisible  = 0x60; // uint32: linked into the scene
    constexpr size_t kOffLightNext     = 0x68; // CM2Light*: next in the same grid cell
    constexpr uint32_t kLightTypePoint = 1;

    // --- WMO placements and MOLT ---------------------------------------------------------------
    /// The loaded CMapObjDef list (walked by CMap::PrepareMapObjDefs, 0x007B6110): a TSList whose
    /// head is at kMapObjDefHead and whose link offset is stored at kMapObjDefLinkOffset; next =
    /// *(def + linkOffset + 4). A pointer with bit 0 set, or zero, ends the list.
    constexpr uintptr_t kMapObjDefLinkOffset = 0x00D25438;
    constexpr uintptr_t kMapObjDefHead       = 0x00D25440;

    constexpr size_t   kOffDefFlags      = 0x0C;  // bit 0x20: excluded from light linking
    constexpr uint32_t kDefFlagNoLights  = 0x20;
    constexpr size_t   kOffDefBoundsMin  = 0x48;  // float[3], world AABB
    constexpr size_t   kOffDefBoundsMax  = 0x54;
    constexpr size_t   kOffDefToWorld    = 0x70;  // C44Matrix, model to world, row vectors
    constexpr size_t   kOffDefMapObj     = 0xF4;  // -> CMapObj root

    constexpr size_t kOffRootLoaded     = 0x1E0; // nonzero once the root is parsed
    constexpr size_t kOffRootMolt       = 0x148; // -> SMOLight[]
    constexpr size_t kOffRootMoltCount  = 0x184;

    /// SMOLight (0x30). CMapObjDef::UpdateMoved (0x007B64F0) transforms +0x08 by the placement
    /// matrix for a per-def CMapLight array (+0x13C) that CMap::PrepareMapObjDef (0x007B5D00)
    /// sizes to the MOLT count but only fills with zeros: CMap::CreateLight (0x007D9BD0) has one
    /// caller, CMap::Load, which makes the sun. MOLT never lights anything in 3.3.5a.
    constexpr size_t kMoltStride        = 0x30;
    constexpr size_t kOffMoltType       = 0x00; // uint8: 0 omni, 1 spot, 2 directional, 3 ambient
    constexpr size_t kOffMoltUseAtten   = 0x01; // uint8
    constexpr size_t kOffMoltColor      = 0x04; // BGRA bytes
    constexpr size_t kOffMoltPosition   = 0x08; // float[3], model space
    constexpr size_t kOffMoltIntensity  = 0x14; // float
    constexpr size_t kOffMoltAttenStart = 0x28; // float
    constexpr size_t kOffMoltAttenEnd   = 0x2C; // float
}
