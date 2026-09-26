// wxl-graphics-fog: the terrain block, 8 x 8 ADT tiles around the camera. The baked tiles of
// 5.tools/forever-bake (Textures\Forever\Horizon: _height R16F and _sky L8; Textures\Forever\Water:
// G16R16F liquid height and kind) stream through wxl-graphics-extend's baked assets and are uploaded
// tile by tile straight into the GPU atlases, cell (cx mod 8, cy mod 8): no CPU atlas is kept. The
// camera stays in the central 2 x 2 tiles, so the block reaches at least 1600 yards around it.
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

#include "wxl/GraphicsVulkanApi.h"

#include <cstdint>

namespace wxl::gfx::fog::terrain
{
    /// Follows the camera: the map's manifests, the block's origin, the tiles each cell holds, their
    /// requests. Once per frame, before RecordUploads.
    void Update(const float eye[3]);

    /// Uploads the files that arrived (at most `budget` tiles) into the images; a tile whose files
    /// are all in becomes resident. Inside the fog's record callback.
    int RecordUploads(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, int budget);

    /// The GPU images were lost (a new device): every tile uploads again.
    void Forget();

    int      OriginX();
    int      OriginY();
    uint64_t ResidentMask();      ///< bit (cy & 7) * 8 + (cx & 7) per resident tile
    uint64_t TakeInitMask();      ///< tiles that became resident since the last call (and clears it)
    bool     TakeFloorDirty();    ///< the floor must be rebuilt (and clears the flag)
    int      Resident();
    bool     HasManifest();
    /// The map the client has loaded now ("" before a world), for the orchestration's jump test.
    const char* LiveMapName();
    const char* Status();
}
