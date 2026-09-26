// wxl-graphics-shadow: the baked horizon block's residency, its uploads and its CPU heights.
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

#include "Horizon.hpp"
#include "../core/Extension.hpp"
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
    namespace hz  = wxl::gfx::shadow::horizon;
    namespace gpu = wxl::gfx::shadow::gpu;
    using wxl::gfx::shadow::Gfx;

    constexpr uint32_t kFmtL8 = 50;      // D3DFMT_L8
    constexpr uint32_t kFmtR16F = 111;   // D3DFMT_R16F
    constexpr uint16_t kNeutralHeight = 0xF0E2;   // -10000 as a half float: no ground known
    constexpr int      kPerFrame = 2;    // tiles uploaded a frame at most

    enum class State : uint8_t { Empty, Missing, Pending, Resident };

    struct Cell
    {
        int      cx = INT_MIN, cy = INT_MIN;
        State    state = State::Empty;
        uint32_t volume = 0, height = 0;   // asset ids
        bool     volumeDone = false, heightDone = false;
        bool     volumeOk = false;
        bool     cleared = false;          // its neutral values are in the images
        bool     uploaded = false;         // its files are in the images
        std::vector<uint8_t>  volumeBytes; // kept until uploaded
        std::vector<uint16_t> heightHalf;  // kept until uploaded
        std::vector<float>    heights;     // CPU copy for the caster
    };

    Cell        g_cells[hz::kBlock][hz::kBlock];   // [cy & 3][cx & 3]
    int         g_ox = INT_MIN, g_oy = INT_MIN;
    bool        g_haveBlock = false;
    std::string g_map;
    const WXL_GfxManifest* g_manifest = nullptr;
    int         g_colVolume = -1, g_colHeight = -1;
    uint32_t    g_deviceGeneration = 0;
    uint32_t    g_heightGeneration = 1;
    WXL_GfxVkImage g_volume{}, g_heights{};
    bool        g_imagesFailed = false;
    bool        g_fresh = false;           // made this block: cleared before anything is copied in
    int         g_resident = 0;
    char        g_status[192] = "terrain horizon: idle";

    int Wrap(int v) { return v & (hz::kBlock - 1); }

    void TileOf(const float eye[3], int& cx, int& cy)
    {
        cx = int(std::floor(32.0f - eye[1] / hz::kTileSize));
        cy = int(std::floor(32.0f - eye[0] / hz::kTileSize));
    }

    float FromHalf(uint16_t h)
    {
        const uint32_t sign = uint32_t(h & 0x8000u) << 16;
        const uint32_t exp = (h >> 10) & 0x1Fu;
        const uint32_t mant = h & 0x3FFu;
        uint32_t x;
        if (exp == 0) x = sign;
        else if (exp == 31) x = sign | 0x7F800000u | (mant << 13);
        else x = sign | ((exp + 112u) << 23) | (mant << 13);
        float f;
        std::memcpy(&f, &x, 4);
        return f;
    }

    void Release(Cell& c)
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (gfx)
        {
            if (c.volume) gfx->AssetRelease(c.volume);
            if (c.height) gfx->AssetRelease(c.height);
        }
        if (!c.heights.empty()) ++g_heightGeneration;
        c = Cell{};
    }

    void ResetAll()
    {
        for (auto& row : g_cells)
            for (Cell& c : row) Release(c);
        g_haveBlock = false;
    }

    void LoadManifest(const char* map)
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        ResetAll();
        g_map = map;
        char file[96];
        std::snprintf(file, sizeof file, "manifest_%s.csv", map);
        g_manifest = gfx->ManifestLoad("Textures\\Forever\\Horizon", file);
        g_colVolume = g_manifest ? gfx->ManifestColumn(g_manifest, "file") : -1;
        g_colHeight = g_manifest ? gfx->ManifestColumn(g_manifest, "file_height") : -1;
        if (!g_manifest || g_colVolume < 0)
            SHADOW_LOG_INFO("horizon: no %s under Textures\\Forever\\Horizon: no terrain shadows on this map (bake it with 5.tools/forever-bake)", file);
        else
            SHADOW_LOG_INFO("horizon: %s has %d baked tiles", map, gfx->ManifestCount(g_manifest));
    }

    void Assign(const float eye[3])
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        int ccx, ccy;
        TileOf(eye, ccx, ccy);
        const std::string folder = g_manifest ? gfx->ManifestFolder(g_manifest) : "";
        for (int j = 0; j < hz::kBlock; ++j)
            for (int i = 0; i < hz::kBlock; ++i)
            {
                const int cx = g_ox + Wrap(i - g_ox), cy = g_oy + Wrap(j - g_oy);
                Cell& c = g_cells[j][i];
                if (c.cx == cx && c.cy == cy) continue;
                Release(c);
                c.cx = cx;
                c.cy = cy;
                if (!g_manifest || g_colVolume < 0 || cx < 0 || cy < 0 || cx > 63 || cy > 63)
                {
                    c.state = State::Missing;
                    continue;
                }
                char key[96];
                std::snprintf(key, sizeof key, "%s_%d_%d", g_map.c_str(), cx, cy);
                const int row = gfx->ManifestFind(g_manifest, key);
                if (row < 0)
                {
                    c.state = State::Missing;
                    continue;
                }
                const int priority = 50 - std::max(std::abs(cx - ccx), std::abs(cy - ccy)) * 8;
                c.volume = gfx->AssetRequest((folder + gfx->ManifestField(g_manifest, row, g_colVolume)).c_str(), priority, WXL_GFX_ASSET_BYTES);
                c.height = g_colHeight >= 0
                         ? gfx->AssetRequest((folder + gfx->ManifestField(g_manifest, row, g_colHeight)).c_str(), priority, WXL_GFX_ASSET_BYTES)
                         : 0;
                c.heightDone = c.height == 0;
                c.state = State::Pending;
            }
    }

    /// The raw surfaces of a DDS of the expected kind, format and size, or null.
    const uint8_t* Raw(const void* bytes, size_t size, uint32_t kind, uint32_t format, uint32_t depth, size_t texelBytes)
    {
        WXL_GfxDdsInfo info{};
        info.structSize = sizeof info;
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (!gfx->DdsParse(bytes, size, &info) || info.kind != kind || info.fileFormat != format
            || info.width != uint32_t(hz::kTile) || info.height != uint32_t(hz::kTile) || info.depth != depth)
            return nullptr;
        if (info.dataOffset + size_t(hz::kTile) * hz::kTile * depth * texelBytes > size) return nullptr;
        return static_cast<const uint8_t*>(bytes) + info.dataOffset;
    }

    /// Advances one file of a cell once its bytes arrived. True when it progressed.
    template <class Fn>
    bool Step(uint32_t& id, bool& done, Fn&& take)
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
        const bool ok = bytes && size && take(bytes, size);
        if (!ok && bytes) SHADOW_LOG_WARN("horizon: a tile file this build cannot read (asset %u)", id);
        gfx->AssetRelease(id);
        id = 0;
        done = true;
        return true;
    }

    bool EnsureImages(const WXL_GfxVulkanApi* api)
    {
        if (g_volume.image != VK_NULL_HANDLE && g_heights.image != VK_NULL_HANDLE) return true;
        if (g_imagesFailed) return false;
        WXL_GfxVkImageDesc v{};
        v.structSize = sizeof v;
        v.name = "shadow.horizon";
        v.type = VK_IMAGE_TYPE_3D;
        v.format = VK_FORMAT_R8_UNORM;
        v.width = v.height = uint32_t(hz::kBlock * hz::kTile);
        v.depth = hz::kSlices;
        v.mipLevels = 1;
        v.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        WXL_GfxVkImageDesc h = v;
        h.name = "shadow.horizon.height";
        h.type = VK_IMAGE_TYPE_2D;
        h.format = VK_FORMAT_R16_SFLOAT;
        h.depth = 1;
        if (!api->CreateImage(&v, &g_volume) || g_volume.image == VK_NULL_HANDLE
            || !api->CreateImage(&h, &g_heights) || g_heights.image == VK_NULL_HANDLE)
        {
            if (g_volume.image != VK_NULL_HANDLE) api->DestroyImage(&g_volume);
            if (g_heights.image != VK_NULL_HANDLE) api->DestroyImage(&g_heights);
            g_volume = g_heights = WXL_GfxVkImage{};
            g_imagesFailed = true;
            SHADOW_LOG_WARN("horizon: the images were refused; no terrain horizon");
            return false;
        }
        g_fresh = true;
        // Fresh images hold nothing: every tile is requested and uploaded again.
        for (auto& row : g_cells)
            for (Cell& c : row) Release(c);
        return true;
    }

    /// Copies a tile's bytes (tightly packed) into its cell of an image; null bytes write the neutral value.
    bool Upload(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, const WXL_GfxVkImage& image, const Cell& c, const void* bytes,
                size_t texelBytes, uint32_t depth)
    {
        const size_t size = size_t(hz::kTile) * hz::kTile * depth * texelBytes;
        WXL_GfxVkAlloc a{};
        if (!api->AllocStaging(size, &a) || !a.mapped) return false;
        if (bytes) std::memcpy(a.mapped, bytes, size);
        else if (texelBytes == 2)
        {
            uint16_t* p = static_cast<uint16_t*>(a.mapped);
            std::fill(p, p + size / 2, kNeutralHeight);
        }
        else std::memset(a.mapped, 0, size);
        VkBufferImageCopy region{};
        region.bufferOffset = a.offset;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = { Wrap(c.cx) * hz::kTile, Wrap(c.cy) * hz::kTile, 0 };
        region.imageExtent = { uint32_t(hz::kTile), uint32_t(hz::kTile), depth };
        gpu::Dev().vkCmdCopyBufferToImage(cmd, a.buffer, image.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
        return true;
    }

    Cell* Find(int cx, int cy)
    {
        Cell& c = g_cells[Wrap(cy)][Wrap(cx)];
        return c.cx == cx && c.cy == cy ? &c : nullptr;
    }
}

namespace wxl::gfx::shadow::horizon
{
    void Prepare(const WXL_GfxVulkanApi* api)
    {
        if (!api) return;
        if (gpu::Dev().generation != g_deviceGeneration)
        {
            Forget();
            g_deviceGeneration = gpu::Dev().generation;
        }
        EnsureImages(api);
    }

    void Update(const WXL_GfxVulkanApi* api, VkCommandBuffer cmd, const float eye[3])
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (!gfx) return;
        const char* map = wxl::game::world::MapName();
        if (!map || !*map) return;
        if (g_map != map) LoadManifest(map);

        int cx, cy;
        TileOf(eye, cx, cy);
        if (!g_haveBlock)
        {
            g_ox = cx - 1;
            g_oy = cy - 1;
            g_haveBlock = true;
        }
        // The camera stays in the central 2 x 2 tiles: a column or row reloads when it leaves them.
        if (cx < g_ox + 1) g_ox = cx - 1;
        if (cx > g_ox + 2) g_ox = cx - 2;
        if (cy < g_oy + 1) g_oy = cy - 1;
        if (cy > g_oy + 2) g_oy = cy - 2;
        Assign(eye);

        // Files that arrived: kept as bytes (and heights on the CPU) until uploaded.
        for (auto& row : g_cells)
            for (Cell& c : row)
            {
                if (c.state != State::Pending) continue;
                Step(c.volume, c.volumeDone, [&](const void* b, size_t n) {
                    const uint8_t* s = Raw(b, n, WXL_GFX_DDS_VOLUME, kFmtL8, kSlices, 1);
                    c.volumeOk = s != nullptr;
                    if (s) c.volumeBytes.assign(s, s + size_t(kTile) * kTile * kSlices);
                    return c.volumeOk;
                });
                Step(c.height, c.heightDone, [&](const void* b, size_t n) {
                    const uint8_t* s = Raw(b, n, WXL_GFX_DDS_2D, kFmtR16F, 1, 2);
                    if (!s) return false;
                    const uint16_t* h = reinterpret_cast<const uint16_t*>(s);
                    c.heightHalf.assign(h, h + size_t(kTile) * kTile);
                    c.heights.resize(size_t(kTile) * kTile);
                    for (size_t k = 0; k < c.heights.size(); ++k) c.heights[k] = FromHalf(h[k]);
                    ++g_heightGeneration;
                    return true;
                });
                if (c.volumeDone && c.heightDone) c.state = c.volumeOk ? State::Resident : State::Missing;
            }

        if (api && cmd && g_volume.image != VK_NULL_HANDLE && g_heights.image != VK_NULL_HANDLE)
        {
            api->CmdBarrier(cmd);
            if (g_fresh)
            {
                VkClearColorValue zero{};
                VkImageSubresourceRange range{};
                range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                range.levelCount = 1;
                range.layerCount = 1;
                gpu::Dev().vkCmdClearColorImage(cmd, g_volume.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
                VkClearColorValue neutral{};
                neutral.float32[0] = -10000.0f;
                gpu::Dev().vkCmdClearColorImage(cmd, g_heights.image, VK_IMAGE_LAYOUT_GENERAL, &neutral, 1, &range);
                api->CmdBarrier(cmd);
                g_fresh = false;
                for (auto& row : g_cells)
                    for (Cell& c : row) c.cleared = true;
            }
            int budget = kPerFrame;
            for (auto& row : g_cells)
                for (Cell& c : row)
                {
                    if (budget <= 0) break;
                    // A cell that changed tiles holds the old tile's values until its own arrive: neutral first.
                    if (!c.cleared && c.state != State::Empty)
                    {
                        c.cleared = Upload(api, cmd, g_volume, c, nullptr, 1, kSlices) && Upload(api, cmd, g_heights, c, nullptr, 2, 1);
                        --budget;
                        continue;
                    }
                    if (c.state != State::Resident || c.uploaded) continue;
                    c.uploaded = Upload(api, cmd, g_volume, c, c.volumeBytes.data(), 1, kSlices)
                              && (c.heightHalf.empty() || Upload(api, cmd, g_heights, c, c.heightHalf.data(), 2, 1));
                    if (c.uploaded)
                    {
                        c.volumeBytes.clear();
                        c.volumeBytes.shrink_to_fit();
                        c.heightHalf.clear();
                        c.heightHalf.shrink_to_fit();
                    }
                    --budget;
                }
            api->CmdBarrier(cmd);
        }

        int pending = 0, missing = 0;
        g_resident = 0;
        for (auto& row : g_cells)
            for (Cell& c : row)
            {
                if (c.state == State::Resident && c.uploaded) ++g_resident;
                else if (c.state == State::Pending || (c.state == State::Resident && !c.uploaded)) ++pending;
                else if (c.state == State::Missing) ++missing;
            }
        std::snprintf(g_status, sizeof g_status, "terrain horizon: %s, tiles %d..%d x %d..%d, %d resident, %d loading, %d not baked",
                      g_manifest ? g_map.c_str() : "no baked tiles", g_ox, g_ox + kBlock - 1, g_oy, g_oy + kBlock - 1, g_resident, pending,
                      missing);
    }

    const WXL_GfxVkImage* Volume() { return g_volume.image != VK_NULL_HANDLE ? &g_volume : nullptr; }
    const WXL_GfxVkImage* Heights() { return g_heights.image != VK_NULL_HANDLE ? &g_heights : nullptr; }
    int  OriginX() { return g_ox; }
    int  OriginY() { return g_oy; }
    bool Ready() { return g_resident > 0 && Volume() && Heights(); }

    const float* TileHeights(int cx, int cy)
    {
        const Cell* c = Find(cx, cy);
        return c && !c->heights.empty() ? c->heights.data() : nullptr;
    }

    uint32_t Generation() { return g_heightGeneration; }

    void Forget()
    {
        // The images went with the device (the service destroyed them): every tile uploads again.
        for (auto& row : g_cells)
            for (Cell& c : row) Release(c);
        g_volume = g_heights = WXL_GfxVkImage{};
        g_imagesFailed = false;
        g_haveBlock = false;
        g_resident = 0;
    }

    const char* Status() { return g_status; }
}
