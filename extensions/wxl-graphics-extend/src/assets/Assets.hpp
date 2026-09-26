// wxl-graphics-extend: baked assets -- manifests and files streamed from the client within a memory budget.
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

#include "wxl/GraphicsExtendApi.h"

#include <cstddef>
#include <cstdint>

// wxl-forever's core/BakedAssets, generalised to any folder and behind the C ABI (GraphicsExtendApi.h,
// "baked assets"). Reads run on a worker thread started on the first request; textures are created
// by Pump on the render thread, a few per frame.
namespace wxl::gfx::assets
{
    /// Reads the config (WXL_GFX_ASSET_BUDGET_MB, WXL_GFX_ASSET_ASYNC). Called once from WXL_Load.
    void Install();

    /// Once per frame on the render thread (Module.cpp calls it from OnEndScene): creates textures for
    /// finished reads (a few per frame), evicts the least recently touched over the budget, reads on
    /// the render thread what only the client's archives hold.
    void Pump(void* device, uint32_t frame);

    /// Before a device reset: WXL_GFX_ASSET_TEXTURE_GPU textures (DEFAULT pool) are released and read
    /// again on their next request; MANAGED ones survive.
    void OnDeviceLost();

    const WXL_GfxManifest* ManifestLoad(const char* folder, const char* file);
    int         ManifestVersion(const WXL_GfxManifest* manifest);
    int         ManifestCount(const WXL_GfxManifest* manifest);
    int         ManifestColumn(const WXL_GfxManifest* manifest, const char* name);
    int         ManifestFind(const WXL_GfxManifest* manifest, const char* key);
    const char* ManifestField(const WXL_GfxManifest* manifest, int row, int column);
    float       ManifestNumber(const WXL_GfxManifest* manifest, int row, int column, float fallback);
    const char* ManifestFolder(const WXL_GfxManifest* manifest);

    uint32_t    Request(const char* path, int priority, uint32_t want);
    void*       Texture(uint32_t id);
    const void* Bytes(uint32_t id, size_t* size);
    uint32_t    State(uint32_t id);
    void        Release(uint32_t id);
    const char* Status();
}
