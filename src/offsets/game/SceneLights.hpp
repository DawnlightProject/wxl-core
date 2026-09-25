// The world M2 scene's light machinery: CM2Light's methods, CM2Lighting's selection, the scene walk.
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

// INTERNAL to the core. Record layouts live in offsets/game/Lights.hpp; these are the functions.
// Every receiver (M2 model, WMO group, terrain chunk) owns a CM2Lighting that CM2Scene::SelectLights
// fills through CM2Lighting::AddLight, keeping at most 4 point lights; terrain uses the first 3.
namespace wxl::offsets::game::scenelights
{
    /// CM2Scene::Animate: __thiscall(scene, const float* cameraPos). Its first instruction
    /// increments the scene frame (+0x14), then it animates, places model lights and runs
    /// CM2Model::SetupLighting -> SelectLights for every animated model.
    constexpr uintptr_t kSceneAnimate = 0x00821A20;
    using SceneAnimateFn = void(__fastcall*)(void* scene, void* edx, const float* cameraPos);

    /// CM2Scene::SelectLights: __thiscall(scene, CM2Lighting*). Walks the directional list
    /// (+0x20) and the grid cells covering the lighting's sphere, calling AddLight on each.
    constexpr uintptr_t kSceneSelectLights = 0x0081E400;
    using SceneSelectLightsFn = void(__fastcall*)(void* scene, void* edx, void* lighting);

    /// CM2Lighting::AddLight: __thiscall(CM2Lighting*, CM2Light*).
    constexpr uintptr_t kAddLight = 0x00834F60;
    using AddLightFn = void(__fastcall*)(void* lighting, void* edx, void* light);

    // --- CM2Light methods ---
    constexpr uintptr_t kLightConstruct   = 0x00834A40; // __fastcall(light)
    using LightConstructFn = void(__fastcall*)(void* light);
    constexpr uintptr_t kLightInitialize  = 0x008348D0; // __thiscall(light, scene): +0 scene, +4 frame - 1
    using LightInitializeFn = void(__fastcall*)(void* light, void* edx, void* scene);
    constexpr uintptr_t kLightSetType     = 0x00835640; // __thiscall(light, type), relinks
    using LightSetTypeFn = void(__fastcall*)(void* light, void* edx, int type);
    constexpr uintptr_t kLightSetVisible  = 0x008356F0; // __thiscall(light, on): links or unlinks
    using LightSetVisibleFn = void(__fastcall*)(void* light, void* edx, int on);
    constexpr uintptr_t kLightSetPosition = 0x00835690; // __thiscall(light, float*): unlinks and relinks
    using LightSetPositionFn = void(__fastcall*)(void* light, void* edx, const float* pos);
    constexpr size_t kLightSize = 0x6C;

    // --- CM2Lighting (0xD4 bytes, CM2Lighting::Initialize 0x00834900) ---
    constexpr size_t kOffLightingSphere = 0x04; // float[4]: receiver centre and radius
    constexpr size_t kOffLightingLights = 0x84; // CM2Light*[4], best first
    constexpr size_t kOffLightingKeys   = 0x94; // float[4], ascending; stock stores squared distance
    constexpr size_t kOffLightingCount  = 0xA4; // uint32
    constexpr uint32_t kLightingSlots   = 4;

    /// CWorldScene::s_m2Scene's grid: 64 x 64 cells of 20 yd; wider scans wrap past 640 yd.
    constexpr uint32_t kGridCells = 64;
    constexpr float    kCellSize  = 20.0f;
}
