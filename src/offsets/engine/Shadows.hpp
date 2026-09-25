// The engine's exterior shadow maps: CShadowCache's globals and CShadowQuery's four callbacks.
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

// INTERNAL to the core. CWorldScene::Render (0x0079A870) calls CShadowQuery::Update (0x007BB570)
// once per world frame; it runs CShadowCache::PreUpdate (0x00875C10), CreateResources (0x00875F80)
// and PostUpdate (0x008750B0), which render and publish everything below.
namespace wxl::offsets::engine::shadows
{
    /// int, the extShadowQuality level 0..5 (set by 0x00874210 once 0x008740D0 accepts it).
    constexpr uintptr_t kMode = 0x00D43154;
    /// int, nonzero while a mode or device change waits to recreate the textures. Set by
    /// CShadowCache::NotifyTexturesRecreate (0x00873FE0); consumed by CreateResources.
    constexpr uintptr_t kRecreatePending = 0x00B1D51C;
    /// int[6], shader tier by mode: {0,1,1,2,2,3}, read by 0x00873FF0.
    constexpr uintptr_t kTierByMode = 0x00B1D554;
    /// int, nonzero when hwPCF is on: maps are D24X8 depth textures instead of R32F colour.
    constexpr uintptr_t kHwPcf = 0x00D43014;
    /// uint, map edge in texels: 1024 for modes 1 and 3, 2048 for 2, 4 and 5 (CShadowCache::Create 0x00875D30).
    constexpr uintptr_t kMapSize = 0x00D43150;
    /// uint8, bumped by every CreateResources that renders; tells whether this frame's update ran.
    constexpr uintptr_t kUpdateCounter = 0x00D4316C;

    // --- texture handles (HTEXTURE, "ShadowCache") ---
    /// Main exterior map. Without hwPCF it is the R32F map; with hwPCF it is a dummy A8R8G8B8
    /// target and the depth map is kMainDepthTex. Receivers sample (&kMainTex)[hwPcf].
    constexpr uintptr_t kMainTex      = 0x00D43250;
    constexpr uintptr_t kMainDepthTex = 0x00D43254;
    /// Interior map, rendered first with query mask 3, sampled for interior batches.
    constexpr uintptr_t kInteriorTex  = 0x00D43148;

    /// Three cascade bands (modes >= 3), records of 0x3C bytes.
    constexpr uintptr_t kBands          = 0x00D43290;
    constexpr size_t    kBandStride     = 0x3C;
    constexpr size_t    kBandCount      = 3;
    constexpr size_t    kOffBandTex     = 0x00;  // HTEXTURE[2], a ping-pong pair below mode 5
    constexpr size_t    kOffBandExtent  = 0x08;  // float half-extent, from {40,160,640} at 0x00B1D520
    constexpr size_t    kOffBandCentre  = 0x1C;  // float[3], world centre the band was rendered around
    constexpr size_t    kOffBandCurrent = 0x38;  // int, which of the pair receivers sample

    // --- placement ---
    constexpr uintptr_t kMainExtent  = 0x00D43258;  // float half-extent of the main map (20 yd)
    constexpr uintptr_t kCentre      = 0x00D43260;  // float[3], texel-snapped world centre
    constexpr uintptr_t kLightDir    = 0x00D43180;  // float[3], world direction the maps look along
    constexpr uintptr_t kLightDirView = 0x00D4318C; // float[3], the same in view space (pixel c4)

    /// Texture-space rows, 3 x float4 per map: main first, then the three bands. The block is the
    /// 12 vertex constants c224..c235 SetShadowMapGenericGlobal (0x008744E0) uploads.
    constexpr uintptr_t kMainRows  = 0x00D43348;
    constexpr uintptr_t kBandRows  = 0x00D43378;
    constexpr size_t    kRowsStride = 0x30;

    // --- the four callbacks CShadowCache calls, installed by CShadowQuery::Initialize (0x007BD3A0)
    //     through the setters 0x00873FA0..0x00873FD0. All __cdecl, caller-cleaned.
    /// GetMatrix (0x007BAC10): (const float* centre, float halfExtent, float* out16, const float* up, int slot)
    constexpr uintptr_t kMatrixCallback  = 0x00D43158;
    /// FrustumCallback (0x007BAFD0): (void*, float* proj16, const float* up, void* query, int slot)
    constexpr uintptr_t kFrustumCallback = 0x00D4315C;
    /// QueryCallback (0x007BD200): int (void* query, uint8 counter)
    constexpr uintptr_t kQueryCallback   = 0x00D43160;
    /// Render (0x007BBC50): int (void* query, int slot, CGxTex* colour, CGxTex* depth, const float* up)
    constexpr uintptr_t kRenderCallback  = 0x00D43164;

    /// CShadowCache::PreUpdate __cdecl(const float* lightDir, const float* cameraPos): copies the
    /// frame's light direction into kLightDir before any callback runs. CShadowQuery::Update
    /// (0x007BB570) builds that direction as normalize(sun.x, sun.y, max(sun.z * 5, -1.2)), which
    /// is what keeps the engine's sun at least 50 degrees high.
    constexpr uintptr_t kPreUpdate = 0x00875C10;
    using PreUpdateFn = void(__cdecl*)(const float* lightDir, const float* cameraPos);
    /// float[3] (1, 0, 0): the up vector the main pass renders with; the bands carry theirs at +0x28.
    constexpr uintptr_t kMainUp = 0x00D43278;
    constexpr size_t    kOffBandUp = 0x28;

    // --- the CShadowQuery layout Render reads per slot (its first argument; the main pass uses
    //     CreateResources' stack frame laid out the same way). Slot = Render's second argument:
    //     0 for the interior and main passes, the band index for the bands. ---
    constexpr size_t kOffQueryCentre     = 0x930;  // float[3] per slot, stride 12: the pass's world centre
    constexpr size_t kOffQueryExtent     = 0x958;  // float per slot, stride 4: half-extent, yards
    constexpr size_t kOffQueryViewport   = 0x994;  // float[4] per slot, stride 16: x0, x1, y0, y1 fractions
    constexpr size_t kOffQueryProjection = 0x9C4;  // float[16] per slot, stride 64: GxuXformCreateOrtho output
    constexpr float  kLightDistance = 2000.0f;     // yards the light eye sits back from the centre
    constexpr float  kLightNear = 1.0f, kLightFar = 4000.0f;

    // --- what CShadowQuery::Render (0x007BBC50) does, for a pass of our own (disassembly checked) ---
    /// Draw lists the query callback fills per slot: 9 dwords each. [0] WMO group count, [1] and [2]
    /// the group lists, [3..5] the M2 opaque batch list, [6..8] the M2 alpha batch list.
    constexpr uintptr_t kDrawLists       = 0x00D25320;
    constexpr size_t    kDrawListStride  = 0x24;
    constexpr size_t    kOffQueryFrustum = 0x6C;   // + slot * 0xF4 on the query: the slot's CFrustum
    constexpr size_t    kQueryFrustumStride = 0xF4;
    constexpr uintptr_t kInstancingCVar  = 0x00D25314; // CVar*: +0x30 != 0 enables shadow instancing
    constexpr uintptr_t kMaxOpaqueBatches = 0x00D25390;
    constexpr uintptr_t kMaxAlphaBatches  = 0x00D2538C;
    constexpr uintptr_t kEffectName      = 0x00A40104; // "ShadowMapRenderSL"

    constexpr uintptr_t kGetEffect        = 0x00876530; // __cdecl(const char*) -> CShaderEffect*
    using GetEffectFn = void*(__cdecl*)(const char* name);
    constexpr uintptr_t kEffectSetCurrent = 0x00872F90; // __fastcall(effect)
    using EffectSetCurrentFn = void(__fastcall*)(void* effect);
    constexpr uintptr_t kUpdateProjMatrix = 0x00872C10; // __cdecl()
    using UpdateProjMatrixFn = void(__cdecl*)();
    constexpr uintptr_t kSetTexMtxIdentity = 0x00873480; // __cdecl(int stage)
    using SetTexMtxIdentityFn = void(__cdecl*)(int stage);
    constexpr uintptr_t kRsPush  = 0x00409670;          // __thiscall(device)
    constexpr uintptr_t kRsPop   = 0x00685FB0;          // __thiscall(device)
    using RsFn = void(__fastcall*)(void* device, void* edx);
    constexpr uintptr_t kRsDirty = 0x00685970;          // __thiscall(device, int state)
    using RsDirtyFn = void(__fastcall*)(void* device, void* edx, int state);
    constexpr uintptr_t kLookAt  = 0x006C0050;          // GxuXformCreateLookAtXXX __cdecl(eye, target, up, out)
    using LookAtFn = void(__cdecl*)(const float* eye, const float* target, const float* up, float* out);
    constexpr uintptr_t kInstancingPrepare = 0x00832DD0; // __thiscall(list)
    using InstancingPrepareFn = void(__fastcall*)(void* list, void* edx);
    /// CMapObj::RenderMapObjGroupsShadowMap __cdecl(list[1], list[0], list[2], const C44Matrix* camRelative, CFrustum*)
    constexpr uintptr_t kRenderWmoShadow = 0x007AB760;
    using RenderWmoShadowFn = void(__cdecl*)(uint32_t a, uint32_t b, uint32_t c, const float* toCamRel, void* frustum);
    /// CM2Model::RenderModelBatchesShadowMap __cdecl(opaqueList, alphaList)
    constexpr uintptr_t kRenderM2Shadow  = 0x0082DA40;
    using RenderM2ShadowFn = void(__cdecl*)(void* opaque, void* alpha);
    /// Engine device vtable: +0xA0 projection, +0xA4 view (__thiscall(device, const float*)),
    /// +0x118 shader constants (__thiscall(device, target 0 vertex / 4 pixel, reg, data, count)).
    constexpr size_t kVtProjection = 0xA0;
    constexpr size_t kVtView       = 0xA4;
    constexpr size_t kVtConstants  = 0x118;
    using SetMatrixFn    = void(__fastcall*)(void* device, void* edx, const float* m);
    using SetConstantsFn = void(__fastcall*)(void* device, void* edx, int target, int reg, const float* data, int count);
    constexpr size_t kOffProjection = 0xF88;   // on the device: the current projection, 16 floats
    constexpr size_t kOffRsState    = 0x28F4;  // on the device: -> render-state cache
    constexpr size_t kOffRsEnabled  = 0xF58;   // on the device: nonzero when the state cache is live

    // --- helpers ---
    /// __cdecl(HTEXTURE, int = 1, void* = null) -> CGxTex*.
    constexpr uintptr_t kTextureGetGxTex = 0x004B6CB0;
    using TextureGetGxTexFn = void*(__cdecl*)(void* handle, int mode, void* notify);
    /// On a CGxTex: the IDirect3DTexture9* (CGxDeviceD3d::ITexCreate 0x006A2C00 writes it,
    /// ISetTexture 0x006A4900 binds it).
    constexpr size_t kOffGxTexD3d = 0x38;

    /// The device and its view-matrix stack: matrix = device + 0x1B00 + index * 0x40.
    constexpr uintptr_t kGxDevicePtr     = 0x00C5DF88;
    constexpr size_t    kOffViewIndex    = 0x1AF8;
    constexpr size_t    kOffViewStack    = 0x1B00;
    /// CWorldScene::camPos, float[3].
    constexpr uintptr_t kCameraPos = 0x00CD8F5C;
}
