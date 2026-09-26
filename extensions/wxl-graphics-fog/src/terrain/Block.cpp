// wxl-graphics-fog: the terrain block's residency and uploads.
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

#include "Block.hpp"
#include "../gpu/Gpu.hpp"

#include "game/Loading.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    using namespace wxl::gfx::fog;

    constexpr int      kTiles    = FOG_BLOCK_TILES;
    constexpr int      kTexels   = FOG_TILE_TEXELS;
    constexpr float    kTileSize = float(FOG_TILE_YARDS);
    constexpr uint32_t kFmtR16F    = 111;   // D3DFMT_R16F
    constexpr uint32_t kFmtG16R16F = 112;   // D3DFMT_G16R16F
    constexpr uint32_t kFmtA8R8G8B8 = 21;   // D3DFMT_A8R8G8B8

    enum class State : uint8_t { Empty, Missing, Pending, Resident };

    struct Cell
    {
        int      cx = INT_MIN, cy = INT_MIN;
        State    state = State::Empty;
        uint32_t height = 0, sky = 0, water = 0, cascade = 0;   // asset ids
        bool     heightDone = false, skyDone = false, waterDone = false, cascadeDone = false;
        bool     heightOk = false;
    };

    Cell        g_cells[kTiles][kTiles];   // [cy & 7][cx & 7]
    int         g_ox = INT_MIN, g_oy = INT_MIN;
    bool        g_haveBlock = false;
    std::string g_map;
    const WXL_GfxManifest* g_horizon = nullptr;
    const WXL_GfxManifest* g_water = nullptr;
    const WXL_GfxManifest* g_cascade = nullptr;
    int         g_colHeight = -1, g_colSky = -1, g_colWater = -1, g_colCascade = -1;
    uint64_t    g_resident = 0, g_init = 0;
    bool        g_floorDirty = true;
    char        g_status[192] = "terrain: idle";

    int Wrap(int v) { return v & (kTiles - 1); }
    uint64_t Bit(int cx, int cy) { return 1ull << uint32_t(Wrap(cy) * kTiles + Wrap(cx)); }

    void TileOf(const float eye[3], int& cx, int& cy)
    {
        cx = int(std::floor(32.0f - eye[1] / kTileSize));
        cy = int(std::floor(32.0f - eye[0] / kTileSize));
    }

    void Release(Cell& c)
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (gfx)
        {
            if (c.height) gfx->AssetRelease(c.height);
            if (c.sky) gfx->AssetRelease(c.sky);
            if (c.water) gfx->AssetRelease(c.water);
            if (c.cascade) gfx->AssetRelease(c.cascade);
        }
        if (c.state == State::Resident) g_floorDirty = true;
        if (c.cx != INT_MIN) g_resident &= ~Bit(c.cx, c.cy);
        c = Cell{};
    }

    void ResetAll()
    {
        for (auto& row : g_cells)
            for (Cell& c : row) Release(c);
        g_resident = 0;
        g_init = 0;
        g_haveBlock = false;
        g_floorDirty = true;
    }

    void LoadManifests(const char* map)
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        ResetAll();
        g_map = map;
        char file[96];
        std::snprintf(file, sizeof file, "manifest_%s.csv", map);
        g_horizon = gfx->ManifestLoad("Textures\\Forever\\Horizon", file);
        g_colHeight = g_horizon ? gfx->ManifestColumn(g_horizon, "file_height") : -1;
        g_colSky = g_horizon ? gfx->ManifestColumn(g_horizon, "file_sky") : -1;
        g_water = gfx->ManifestLoad("Textures\\Forever\\Water", file);
        g_colWater = g_water ? gfx->ManifestColumn(g_water, "file") : -1;
        g_cascade = gfx->ManifestLoad("Textures\\Forever\\Cascade", file);
        g_colCascade = g_cascade ? gfx->ManifestColumn(g_cascade, "file") : -1;
        if (!g_horizon || g_colHeight < 0)
            FOG_LOG_INFO("terrain: no %s under Textures\\Forever\\Horizon: no fog rivers on this map (bake it with 5.tools/forever-bake)", file);
        else
            FOG_LOG_INFO("terrain: %s: %d baked tiles%s%s", map, gfx->ManifestCount(g_horizon),
                         g_water ? ", with water" : ", no water maps", g_cascade ? ", with cascades" : ", no cascade maps");
    }

    void Assign(const float eye[3])
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        int ccx, ccy;
        TileOf(eye, ccx, ccy);
        const std::string hFolder = g_horizon ? gfx->ManifestFolder(g_horizon) : "";
        const std::string wFolder = g_water ? gfx->ManifestFolder(g_water) : "";
        const std::string cFolder = g_cascade ? gfx->ManifestFolder(g_cascade) : "";
        for (int j = 0; j < kTiles; ++j)
            for (int i = 0; i < kTiles; ++i)
            {
                const int cx = g_ox + Wrap(i - g_ox), cy = g_oy + Wrap(j - g_oy);
                Cell& c = g_cells[j][i];
                if (c.cx == cx && c.cy == cy) continue;
                Release(c);
                c.cx = cx;
                c.cy = cy;
                if (!g_horizon || g_colHeight < 0 || cx < 0 || cy < 0 || cx > 63 || cy > 63)
                {
                    c.state = State::Missing;
                    continue;
                }
                char key[96];
                std::snprintf(key, sizeof key, "%s_%d_%d", g_map.c_str(), cx, cy);
                const int row = gfx->ManifestFind(g_horizon, key);
                if (row < 0)
                {
                    c.state = State::Missing;
                    continue;
                }
                const int dist = std::max(std::abs(cx - ccx), std::abs(cy - ccy));
                const int priority = 60 - dist * 8;
                c.height = gfx->AssetRequest((hFolder + gfx->ManifestField(g_horizon, row, g_colHeight)).c_str(), priority + 1,
                                             WXL_GFX_ASSET_BYTES);
                c.sky = g_colSky >= 0 ? gfx->AssetRequest((hFolder + gfx->ManifestField(g_horizon, row, g_colSky)).c_str(),
                                                          priority, WXL_GFX_ASSET_BYTES) : 0;
                const int wet = g_water && g_colWater >= 0 ? gfx->ManifestFind(g_water, key) : -1;
                c.water = wet >= 0 ? gfx->AssetRequest((wFolder + gfx->ManifestField(g_water, wet, g_colWater)).c_str(), priority,
                                                       WXL_GFX_ASSET_BYTES) : 0;
                const int pours = g_cascade && g_colCascade >= 0 ? gfx->ManifestFind(g_cascade, key) : -1;
                c.cascade = pours >= 0 ? gfx->AssetRequest((cFolder + gfx->ManifestField(g_cascade, pours, g_colCascade)).c_str(),
                                                           priority - 1, WXL_GFX_ASSET_BYTES) : 0;
                c.skyDone = c.sky == 0;
                c.state = State::Pending;
            }
    }

    /// Copies one tile's texels (tightly packed rows) into its cell of a block image.
    bool Upload(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, const WXL_GfxVkImage& image, const Cell& c,
                const void* texels, size_t texelBytes)
    {
        const size_t bytes = size_t(kTexels) * kTexels * texelBytes;
        WXL_GfxVkAlloc a{};
        if (!api->AllocStaging(bytes, &a) || !a.mapped) return false;
        if (texels) std::memcpy(a.mapped, texels, bytes);
        else std::memset(a.mapped, 0, bytes);
        VkBufferImageCopy region{};
        region.bufferOffset = a.offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { Wrap(c.cx) * kTexels, Wrap(c.cy) * kTexels, 0 };
        region.imageExtent = { uint32_t(kTexels), uint32_t(kTexels), 1 };
        gpu::Dev().vkCmdCopyBufferToImage(cmd, a.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        return true;
    }

    /// A raw DDS surface of the expected format and size, or null.
    const uint8_t* RawSurface(const void* bytes, size_t size, uint32_t format, size_t texelBytes)
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        WXL_GfxDdsInfo info{};
        info.structSize = sizeof info;
        if (!gfx->DdsParse(bytes, size, &info) || info.kind != WXL_GFX_DDS_2D || info.fileFormat != format
            || info.width != uint32_t(kTexels) || info.height != uint32_t(kTexels))
            return nullptr;
        if (info.dataOffset + size_t(kTexels) * kTexels * texelBytes > size) return nullptr;
        return static_cast<const uint8_t*>(bytes) + info.dataOffset;
    }

    /// Advances one file of a cell: uploads it when its bytes arrived. True when it progressed.
    template <class Fn>
    bool Step(uint32_t& id, bool& done, Fn&& upload)
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (done || !id) return false;
        const uint32_t s = gfx->AssetState(id);
        if (s == WXL_GFX_ASSET_FAILED || s == WXL_GFX_ASSET_EVICTED)
        {
            done = true;
            return true;
        }
        if (s != WXL_GFX_ASSET_READY) return false;
        size_t size = 0;
        const void* bytes = gfx->AssetBytes(id, &size);
        const bool ok = bytes && size && upload(bytes, size);
        if (!ok && bytes) FOG_LOG_WARN("terrain: a tile file this build cannot read (asset %u)", id);
        gfx->AssetRelease(id);
        done = true;
        return true;
    }
}

namespace wxl::gfx::fog::terrain
{
    void Update(const float eye[3])
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (!gfx) return;
        const char* map = wxl::game::world::MapName();
        if (!map || !*map) return;
        if (g_map != map) LoadManifests(map);

        int cx, cy;
        TileOf(eye, cx, cy);
        if (!g_haveBlock)
        {
            g_ox = cx - 3;
            g_oy = cy - 3;
            g_haveBlock = true;
        }
        // The camera stays in the central 2 x 2 tiles: a column or row reloads when it leaves them.
        if (cx < g_ox + 3) g_ox = cx - 3;
        if (cx > g_ox + 4) g_ox = cx - 4;
        if (cy < g_oy + 3) g_oy = cy - 3;
        if (cy > g_oy + 4) g_oy = cy - 4;
        Assign(eye);
    }

    int RecordUploads(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, int budget)
    {
        const gpu::Images& img = gpu::Img();
        if (!Gfx() || !img.persistent) return 0;
        int uploaded = 0;
        bool barrier = false;
        for (int j = 0; j < kTiles && budget > 0; ++j)
            for (int i = 0; i < kTiles && budget > 0; ++i)
            {
                Cell& c = g_cells[j][i];
                if (c.state != State::Pending) continue;
                if (!barrier)
                {
                    api->CmdBarrier(cmd);
                    barrier = true;
                }
                bool progressed = false;
                progressed |= Step(c.height, c.heightDone, [&](const void* b, size_t n) {
                    const uint8_t* s = RawSurface(b, n, kFmtR16F, 2);
                    c.heightOk = s && Upload(api, cmd, img.height, c, s, 2);
                    return c.heightOk;
                });
                progressed |= Step(c.sky, c.skyDone, [&](const void* b, size_t n) {
                    std::vector<uint8_t> lum;
                    WXL_ByteSink sink = wxl::gfx::SinkTo(lum);
                    uint32_t w = 0, h = 0;
                    if (!Gfx()->DdsDecodeLuminance(b, n, 0, 0, &sink, &w, &h) || w != uint32_t(kTexels) || h != uint32_t(kTexels))
                        return false;
                    return Upload(api, cmd, img.sky, c, lum.data(), 1);
                });
                if (c.water)
                {
                    progressed |= Step(c.water, c.waterDone, [&](const void* b, size_t n) {
                        const uint8_t* s = RawSurface(b, n, kFmtG16R16F, 4);
                        return s && Upload(api, cmd, img.water, c, s, 4);
                    });
                }
                else if (!c.waterDone)
                {
                    // A dry tile: no liquid anywhere.
                    c.waterDone = Upload(api, cmd, img.water, c, nullptr, 4);
                    progressed |= c.waterDone;
                }
                if (c.cascade)
                {
                    progressed |= Step(c.cascade, c.cascadeDone, [&](const void* b, size_t n) {
                        const uint8_t* s = RawSurface(b, n, kFmtA8R8G8B8, 4);
                        return s && Upload(api, cmd, img.cascadeMap, c, s, 4);
                    });
                }
                else if (!c.cascadeDone)
                {
                    // No spill point on this tile.
                    c.cascadeDone = Upload(api, cmd, img.cascadeMap, c, nullptr, 4);
                    progressed |= c.cascadeDone;
                }
                if (c.skyDone && !c.sky && c.state == State::Pending && c.heightDone)
                {
                    // No sky file: full sky everywhere.
                    std::vector<uint8_t> full(size_t(kTexels) * kTexels, 255);
                    Upload(api, cmd, img.sky, c, full.data(), 1);
                }
                if (c.heightDone && c.skyDone && c.waterDone && c.cascadeDone)
                {
                    c.height = c.sky = c.water = c.cascade = 0;
                    if (c.heightOk)
                    {
                        c.state = State::Resident;
                        g_resident |= Bit(c.cx, c.cy);
                        g_init |= Bit(c.cx, c.cy);
                        g_floorDirty = true;
                    }
                    else c.state = State::Missing;
                }
                if (progressed)
                {
                    --budget;
                    ++uploaded;
                }
            }
        if (barrier) api->CmdBarrier(cmd);

        int pending = 0, missing = 0;
        for (auto& row : g_cells)
            for (Cell& c : row)
            {
                if (c.state == State::Pending) ++pending;
                else if (c.state == State::Missing) ++missing;
            }
        std::snprintf(g_status, sizeof g_status, "terrain: %s, tiles %d..%d x %d..%d, %d resident, %d loading, %d not baked",
                      g_horizon ? g_map.c_str() : "no baked tiles", g_ox, g_ox + kTiles - 1, g_oy, g_oy + kTiles - 1,
                      Resident(), pending, missing);
        return uploaded;
    }

    void Forget()
    {
        // The images went with the device: every resident tile uploads again.
        for (auto& row : g_cells)
            for (Cell& c : row) c = Cell{};
        g_resident = 0;
        g_init = 0;
        g_floorDirty = true;
    }

    int OriginX() { return g_ox; }
    int OriginY() { return g_oy; }
    uint64_t ResidentMask() { return g_resident; }

    uint64_t TakeInitMask()
    {
        const uint64_t m = g_init;
        g_init = 0;
        return m;
    }

    bool TakeFloorDirty()
    {
        const bool d = g_floorDirty;
        g_floorDirty = false;
        return d;
    }

    int Resident()
    {
        int n = 0;
        for (uint64_t m = g_resident; m; m &= m - 1) ++n;
        return n;
    }

    bool HasManifest() { return g_horizon != nullptr; }

    const char* LiveMapName()
    {
        const char* map = wxl::game::world::MapName();
        return map ? map : "";
    }
    const char* Status() { return g_status; }
}
