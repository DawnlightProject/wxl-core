// wxl-forever terrain: the baked horizon maps around the camera, resident as three wrapping atlases.
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

#include "../core/ExtensionApi.hpp"
#include "Horizon.hpp"

#include "../core/BakedAssets.hpp"
#include "../core/Dds.hpp"
#include "../core/RenderUtil.hpp"

#include "game/Loading.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    namespace hz = wxl::forever::terrain::horizon;
    namespace bk = wxl::forever::baked;
    namespace dds = wxl::forever::dds;

    constexpr int kAtlas = hz::kTile * hz::kBlock;   // 512
    constexpr int kCopiesPerFrame = 2;               // tiles copied into the atlases per frame

    enum class CellState : uint8_t { Empty, Missing, Pending, Filled };

    struct Cell
    {
        int         cx = INT_MIN, cy = INT_MIN;   // the tile the cell is assigned to
        CellState   state = CellState::Empty;
        bk::AssetId volume = 0, sky = 0, height = 0;
        bool        volumeDone = false, skyDone = false, heightDone = false;
        float       zmin = 0.0f, zmax = 0.0f;
        std::vector<float> heights;               // kTile * kTile once heightDone
    };

    hz::Settings g_cfg;
    Cell         g_cells[hz::kBlock][hz::kBlock];   // [j][i] = (cy mod kBlock, cx mod kBlock)
    int          g_ox = INT_MIN, g_oy = INT_MIN;    // block origin tile
    bool         g_haveBlock = false;
    std::string  g_map;                             // the map the manifest belongs to
    bk::Manifest g_manifest;
    bool         g_manifestOk = false;
    int          g_colFile = -1, g_colSky = -1, g_colHeight = -1, g_colZmin = -1, g_colZmax = -1;
    uint32_t     g_generation = 1;
    int          g_resident = 0;
    char         g_status[192] = "horizon: idle";

    IDirect3DVolumeTexture9* g_volume = nullptr;
    IDirect3DTexture9*       g_sky = nullptr;
    IDirect3DTexture9*       g_height = nullptr;
    bool                     g_unavailable = false;

    // --- half floats ------------------------------------------------------------------------------

    uint16_t ToHalf(float f)
    {
        uint32_t x; std::memcpy(&x, &f, 4);
        const uint32_t sign = (x >> 16) & 0x8000u;
        int exp = int((x >> 23) & 0xFF) - 127 + 15;
        uint32_t mant = x & 0x7FFFFFu;
        if (exp <= 0) return uint16_t(sign);                    // too small: zero
        if (exp >= 31) return uint16_t(sign | 0x7C00u);          // too large: infinity
        return uint16_t(sign | (uint32_t(exp) << 10) | (mant >> 13));
    }

    float FromHalf(uint16_t h)
    {
        const uint32_t sign = uint32_t(h & 0x8000u) << 16;
        const uint32_t exp = (h >> 10) & 0x1Fu;
        const uint32_t mant = h & 0x3FFu;
        uint32_t x;
        if (exp == 0)      x = sign | (mant ? 0 : 0);              // denormals read as zero
        else if (exp == 31) x = sign | 0x7F800000u | (mant << 13);
        else               x = sign | ((exp + 112u) << 23) | (mant << 13);
        float f; std::memcpy(&f, &x, 4);
        return f;
    }

    // --- atlases ----------------------------------------------------------------------------------

    bool EnsureAtlases(IDirect3DDevice9* dev)
    {
        if (g_unavailable || !dev) return false;
        if (g_volume && g_sky && g_height) return true;
        if (!g_volume && FAILED(dev->CreateVolumeTexture(kAtlas, kAtlas, hz::kAzimuths, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &g_volume, nullptr)))
        {
            g_volume = nullptr;
            g_unavailable = true;
            WLOG_WARN("horizon: L8 volume %dx%dx%d unavailable; horizon maps off", kAtlas, kAtlas, hz::kAzimuths);
            return false;
        }
        if (!g_sky && FAILED(dev->CreateTexture(kAtlas, kAtlas, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &g_sky, nullptr)))
        {
            g_sky = nullptr;
            g_unavailable = true;
            WLOG_WARN("horizon: L8 sky atlas unavailable; horizon maps off");
            return false;
        }
        if (!g_height && FAILED(dev->CreateTexture(kAtlas, kAtlas, 1, 0, D3DFMT_R16F, D3DPOOL_MANAGED, &g_height, nullptr)))
        {
            // The height is optional: the shaders work without it.
            g_height = nullptr;
            WLOG_WARN("horizon: R16F height atlas unavailable; heights stay off the GPU");
        }
        // Everything neutral to begin with.
        for (int j = 0; j < hz::kBlock; ++j)
            for (int i = 0; i < hz::kBlock; ++i)
            {
                D3DBOX box{ UINT(i * hz::kTile), UINT(j * hz::kTile), UINT((i + 1) * hz::kTile), UINT((j + 1) * hz::kTile), 0, UINT(hz::kAzimuths) };
                D3DLOCKED_BOX lb{};
                if (SUCCEEDED(g_volume->LockBox(0, &lb, &box, 0)))
                {
                    for (int k = 0; k < hz::kAzimuths; ++k)
                        for (int r = 0; r < hz::kTile; ++r)
                            std::memset(static_cast<uint8_t*>(lb.pBits) + k * lb.SlicePitch + r * lb.RowPitch, 0, hz::kTile);
                    g_volume->UnlockBox(0);
                }
                RECT rc{ LONG(i * hz::kTile), LONG(j * hz::kTile), LONG((i + 1) * hz::kTile), LONG((j + 1) * hz::kTile) };
                D3DLOCKED_RECT lr{};
                if (SUCCEEDED(g_sky->LockRect(0, &lr, &rc, 0)))
                {
                    for (int r = 0; r < hz::kTile; ++r) std::memset(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch, 255, hz::kTile);
                    g_sky->UnlockRect(0);
                }
                if (g_height && SUCCEEDED(g_height->LockRect(0, &lr, &rc, 0)))
                {
                    const uint16_t neutral = ToHalf(hz::kNeutralHeight);
                    for (int r = 0; r < hz::kTile; ++r)
                    {
                        uint16_t* row = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch);
                        for (int c = 0; c < hz::kTile; ++c) row[c] = neutral;
                    }
                    g_height->UnlockRect(0);
                }
            }
        WLOG_INFO("horizon: atlases %dx%d (volume x%d L8, sky L8, height %s)", kAtlas, kAtlas, hz::kAzimuths, g_height ? "R16F" : "none");
        return true;
    }

    void FillNeutral(int i, int j)
    {
        if (!g_volume) return;
        D3DBOX box{ UINT(i * hz::kTile), UINT(j * hz::kTile), UINT((i + 1) * hz::kTile), UINT((j + 1) * hz::kTile), 0, UINT(hz::kAzimuths) };
        D3DLOCKED_BOX lb{};
        if (SUCCEEDED(g_volume->LockBox(0, &lb, &box, 0)))
        {
            for (int k = 0; k < hz::kAzimuths; ++k)
                for (int r = 0; r < hz::kTile; ++r)
                    std::memset(static_cast<uint8_t*>(lb.pBits) + k * lb.SlicePitch + r * lb.RowPitch, 0, hz::kTile);
            g_volume->UnlockBox(0);
        }
        RECT rc{ LONG(i * hz::kTile), LONG(j * hz::kTile), LONG((i + 1) * hz::kTile), LONG((j + 1) * hz::kTile) };
        D3DLOCKED_RECT lr{};
        if (g_sky && SUCCEEDED(g_sky->LockRect(0, &lr, &rc, 0)))
        {
            for (int r = 0; r < hz::kTile; ++r) std::memset(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch, 255, hz::kTile);
            g_sky->UnlockRect(0);
        }
        if (g_height && SUCCEEDED(g_height->LockRect(0, &lr, &rc, 0)))
        {
            const uint16_t neutral = ToHalf(hz::kNeutralHeight);
            for (int r = 0; r < hz::kTile; ++r)
            {
                uint16_t* row = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch);
                for (int c = 0; c < hz::kTile; ++c) row[c] = neutral;
            }
            g_height->UnlockRect(0);
        }
    }

    /// Decodes the volume file (L8 or DXT1 slices) into the cell; false when the file is not what
    /// the bake writes.
    bool CopyVolume(int i, int j, const std::string& bytes)
    {
        dds::Info info{};
        if (!dds::Parse(bytes, info) || info.kind != dds::Kind::Volume || info.width != unsigned(hz::kTile) ||
            info.height != unsigned(hz::kTile) || info.depth != unsigned(hz::kAzimuths)) return false;
        D3DBOX box{ UINT(i * hz::kTile), UINT(j * hz::kTile), UINT((i + 1) * hz::kTile), UINT((j + 1) * hz::kTile), 0, UINT(hz::kAzimuths) };
        D3DLOCKED_BOX lb{};
        if (FAILED(g_volume->LockBox(0, &lb, &box, 0))) return false;
        std::vector<uint8_t> slice;
        bool ok = true;
        for (int k = 0; k < hz::kAzimuths && ok; ++k)
        {
            unsigned w = 0, h = 0;
            ok = dds::DecodeLuminance(bytes, info, unsigned(k), 0, slice, w, h) && w == unsigned(hz::kTile) && h == unsigned(hz::kTile);
            if (!ok) break;
            for (int r = 0; r < hz::kTile; ++r)
                std::memcpy(static_cast<uint8_t*>(lb.pBits) + k * lb.SlicePitch + r * lb.RowPitch, slice.data() + r * hz::kTile, hz::kTile);
        }
        g_volume->UnlockBox(0);
        return ok;
    }

    bool CopySky(int i, int j, const std::string& bytes)
    {
        dds::Info info{};
        if (!dds::Parse(bytes, info) || info.kind != dds::Kind::Texture2D || info.width != unsigned(hz::kTile) || info.height != unsigned(hz::kTile))
            return false;
        std::vector<uint8_t> img;
        unsigned w = 0, h = 0;
        if (!dds::DecodeLuminance(bytes, info, 0, 0, img, w, h) || w != unsigned(hz::kTile) || h != unsigned(hz::kTile)) return false;
        RECT rc{ LONG(i * hz::kTile), LONG(j * hz::kTile), LONG((i + 1) * hz::kTile), LONG((j + 1) * hz::kTile) };
        D3DLOCKED_RECT lr{};
        if (FAILED(g_sky->LockRect(0, &lr, &rc, 0))) return false;
        for (int r = 0; r < hz::kTile; ++r) std::memcpy(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch, img.data() + r * hz::kTile, hz::kTile);
        g_sky->UnlockRect(0);
        return true;
    }

    /// The R16F height: into the atlas as it is, and into the cell's CPU copy as floats.
    bool CopyHeight(int i, int j, const std::string& bytes, Cell& cell)
    {
        dds::Info info{};
        if (!dds::Parse(bytes, info) || info.kind != dds::Kind::Texture2D || info.format != D3DFMT_R16F ||
            info.width != unsigned(hz::kTile) || info.height != unsigned(hz::kTile)) return false;
        const size_t at = dds::SurfaceOffset(info, 0, 0);
        if (at + size_t(hz::kTile) * hz::kTile * 2 > bytes.size()) return false;
        const uint16_t* src = reinterpret_cast<const uint16_t*>(bytes.data() + at);
        cell.heights.resize(size_t(hz::kTile) * hz::kTile);
        float zmin = 1e9f, zmax = -1e9f;
        for (size_t n = 0; n < cell.heights.size(); ++n)
        {
            const float z = FromHalf(src[n]);
            cell.heights[n] = z;
            zmin = std::min(zmin, z);
            zmax = std::max(zmax, z);
        }
        cell.zmin = zmin;
        cell.zmax = zmax;
        if (g_height)
        {
            RECT rc{ LONG(i * hz::kTile), LONG(j * hz::kTile), LONG((i + 1) * hz::kTile), LONG((j + 1) * hz::kTile) };
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(g_height->LockRect(0, &lr, &rc, 0)))
            {
                for (int r = 0; r < hz::kTile; ++r)
                    std::memcpy(static_cast<uint8_t*>(lr.pBits) + r * lr.Pitch, src + size_t(r) * hz::kTile, size_t(hz::kTile) * 2);
                g_height->UnlockRect(0);
            }
        }
        return true;
    }

    // --- the block --------------------------------------------------------------------------------

    void ResetCell(Cell& c, int i, int j)
    {
        if (c.volume) bk::Release(c.volume);
        if (c.sky) bk::Release(c.sky);
        if (c.height) bk::Release(c.height);
        const bool hadHeights = !c.heights.empty();
        c = Cell();
        if (hadHeights) ++g_generation;
        FillNeutral(i, j);
    }

    void LoadManifest(const char* map)
    {
        g_map = map;
        char file[96];
        std::snprintf(file, sizeof file, "manifest_%s.csv", map);
        g_manifestOk = g_manifest.Load("Horizon", file);
        g_colFile   = g_manifest.Column("file");
        g_colSky    = g_manifest.Column("file_sky");
        g_colHeight = g_manifest.Column("file_height");
        g_colZmin   = g_manifest.Column("zmin");
        g_colZmax   = g_manifest.Column("zmax");
        if (g_manifestOk && g_manifest.Version() != 1)
            WLOG_WARN("horizon: manifest format %d, this build reads format 1", g_manifest.Version());
        for (int j = 0; j < hz::kBlock; ++j)
            for (int i = 0; i < hz::kBlock; ++i) ResetCell(g_cells[j][i], i, j);
        g_haveBlock = false;
    }

    int Wrap(int v) { v %= hz::kBlock; return v < 0 ? v + hz::kBlock : v; }

    /// Assigns the block's tiles to their cells, requesting the files of the ones that changed.
    void AssignBlock(const float eye[3])
    {
        int ccx, ccy;
        bk::TileOf(eye, ccx, ccy);
        for (int j = 0; j < hz::kBlock; ++j)
            for (int i = 0; i < hz::kBlock; ++i)
            {
                // The tile in [origin, origin + kBlock) whose index is congruent to the cell's.
                const int cx = g_ox + Wrap(i - g_ox), cy = g_oy + Wrap(j - g_oy);
                Cell& c = g_cells[j][i];
                if (c.cx == cx && c.cy == cy) continue;
                ResetCell(c, i, j);
                c.cx = cx;
                c.cy = cy;
                if (!g_manifestOk || cx < 0 || cy < 0 || cx > 63 || cy > 63) { c.state = CellState::Missing; continue; }
                const int row = g_manifest.Find(bk::TileKey(g_map.c_str(), cx, cy).c_str());
                if (row < 0) { c.state = CellState::Missing; continue; }
                const int dist = std::max(std::abs(cx - ccx), std::abs(cy - ccy));
                const int priority = 50 - dist * 10;   // the camera's tile first
                const std::string& folder = g_manifest.Folder();
                c.volume = bk::Request(folder + g_manifest.Field(row, g_colFile), priority, bk::kWantBytes);
                c.sky    = bk::Request(folder + g_manifest.Field(row, g_colSky), priority, bk::kWantBytes);
                c.height = bk::Request(folder + g_manifest.Field(row, g_colHeight), priority + 1, bk::kWantBytes);
                c.zmin   = g_manifest.Number(row, g_colZmin, 0.0f);
                c.zmax   = g_manifest.Number(row, g_colZmax, 0.0f);
                c.state  = CellState::Pending;
            }
    }

    /// Finishes pending cells whose bytes arrived: copies into the atlases, releases the bytes.
    void Finish(int budget)
    {
        for (int j = 0; j < hz::kBlock && budget > 0; ++j)
            for (int i = 0; i < hz::kBlock && budget > 0; ++i)
            {
                Cell& c = g_cells[j][i];
                if (c.state != CellState::Pending) continue;
                bool progressed = false;
                auto step = [&](bk::AssetId id, bool& done, auto&& copy) {
                    if (done || !id) return;
                    const bk::State s = bk::StateOf(id);
                    if (s == bk::State::Failed) { done = true; progressed = true; return; }
                    // Evicted before it was copied (the budget is tight): the cell is assigned again next frame.
                    if (s == bk::State::Evicted) { c.cx = INT_MIN; return; }
                    if (s != bk::State::Ready) return;
                    const std::string* bytes = bk::Bytes(id);
                    if (bytes && !copy(*bytes))
                        WLOG_WARN("horizon: tile %d_%d has a file this build cannot read", c.cx, c.cy);
                    bk::Release(id);
                    done = true;
                    progressed = true;
                };
                step(c.volume, c.volumeDone, [&](const std::string& b) { return CopyVolume(i, j, b); });
                step(c.sky, c.skyDone, [&](const std::string& b) { return CopySky(i, j, b); });
                step(c.height, c.heightDone, [&](const std::string& b) { const bool ok = CopyHeight(i, j, b, c); if (ok) ++g_generation; return ok; });
                if (progressed) --budget;
                if (c.volumeDone && c.skyDone && c.heightDone) c.state = CellState::Filled;
            }
    }

    Cell* Find(int cx, int cy)
    {
        Cell& c = g_cells[Wrap(cy)][Wrap(cx)];
        return c.cx == cx && c.cy == cy ? &c : nullptr;
    }
}

namespace wxl::forever::terrain::horizon
{
    Settings& Get() { return g_cfg; }

    void Install()
    {
        using wxl_forever::ConfigBool;
        using wxl_forever::ConfigFloat;
        g_cfg.enabled  = ConfigBool("WXL_FOREVER_HORIZON", true) ? 1 : 0;
        g_cfg.strength = ConfigFloat("WXL_FOREVER_HORIZON_STRENGTH", g_cfg.strength, 0.0f, 1.0f);
        g_cfg.penumbra = ConfigFloat("WXL_FOREVER_HORIZON_PENUMBRA", g_cfg.penumbra, 0.1f, 15.0f);
        g_cfg.occluder = ConfigFloat("WXL_FOREVER_HORIZON_OCCLUDER", g_cfg.occluder, 10.0f, 400.0f);
    }

    void Update(IDirect3DDevice9* dev, const float eye[3], uint32_t frame)
    {
        if (!g_cfg.enabled || !EnsureAtlases(dev)) return;
        const char* map = wxl::game::world::MapName();
        if (!map || !*map) return;
        if (g_map != map) LoadManifest(map);

        // The block follows the camera's tile with a one-tile margin: it shifts by one column or
        // row only when the camera reaches the block's outer ring.
        int cx, cy;
        bk::TileOf(eye, cx, cy);
        if (!g_haveBlock) { g_ox = cx - 1; g_oy = cy - 1; g_haveBlock = true; }
        if (cx - g_ox < 1) g_ox = cx - 1;
        if (cx - g_ox > kBlock - 2) g_ox = cx - (kBlock - 2);
        if (cy - g_oy < 1) g_oy = cy - 1;
        if (cy - g_oy > kBlock - 2) g_oy = cy - (kBlock - 2);
        AssignBlock(eye);
        bk::Pump(dev, frame);
        Finish(kCopiesPerFrame);

        g_resident = 0;
        int pending = 0, missing = 0;
        for (int j = 0; j < kBlock; ++j)
            for (int i = 0; i < kBlock; ++i)
            {
                const Cell& c = g_cells[j][i];
                if (c.state == CellState::Filled) ++g_resident;
                else if (c.state == CellState::Pending) ++pending;
                else if (c.state == CellState::Missing) ++missing;
            }
        std::snprintf(g_status, sizeof g_status, "horizon: %s, block %d..%d x %d..%d, %d resident, %d loading, %d not baked",
                      g_manifestOk ? g_map.c_str() : "no manifest", g_ox, g_ox + kBlock - 1, g_oy, g_oy + kBlock - 1,
                      g_resident, pending, missing);
    }

    bool Bind(IDirect3DDevice9* dev, int volumeStage, int skyStage, int heightStage)
    {
        namespace render = wxl::forever::render;
        const bool ready = Ready() && g_cfg.enabled;
        if (volumeStage >= 0) render::Sampler(dev, DWORD(volumeStage), ready ? static_cast<IDirect3DBaseTexture9*>(g_volume) : bk::WhiteVolume(dev), true, true);
        if (skyStage >= 0)    render::Sampler(dev, DWORD(skyStage), ready ? static_cast<IDirect3DBaseTexture9*>(g_sky) : bk::White2D(dev), true, true);
        if (heightStage >= 0) render::Sampler(dev, DWORD(heightStage), ready && g_height ? static_cast<IDirect3DBaseTexture9*>(g_height) : bk::White2D(dev), true, true);
        return ready;
    }

    void Constants(float c[4], float d[4], bool withHeight)
    {
        const bool ready = Ready() && g_cfg.enabled;
        c[0] = float(g_ox);
        c[1] = float(g_oy);
        c[2] = float(kBlock);
        c[3] = ready ? g_cfg.strength : 0.0f;
        d[0] = g_cfg.penumbra * 3.14159265f / 180.0f;
        d[1] = float(kAzimuths);
        d[2] = g_cfg.occluder;
        d[3] = withHeight && g_height ? 1.0f : 0.0f;
    }

    bool Ready() { return g_haveBlock && g_resident > 0 && g_volume && g_sky; }

    const float* Heights(int cx, int cy)
    {
        const Cell* c = Find(cx, cy);
        return c && c->heightDone && !c->heights.empty() ? c->heights.data() : nullptr;
    }

    bool Bounds(int cx, int cy, float& zmin, float& zmax)
    {
        const Cell* c = Find(cx, cy);
        if (!c || !c->heightDone || c->heights.empty()) return false;
        zmin = c->zmin;
        zmax = c->zmax;
        return true;
    }

    void Block(int& ox, int& oy, float& zmin, float& zmax)
    {
        ox = g_ox;
        oy = g_oy;
        zmin = 1e9f;
        zmax = -1e9f;
        for (int j = 0; j < kBlock; ++j)
            for (int i = 0; i < kBlock; ++i)
            {
                const Cell& c = g_cells[j][i];
                if (!c.heightDone || c.heights.empty()) continue;
                zmin = std::min(zmin, c.zmin);
                zmax = std::max(zmax, c.zmax);
            }
        if (zmin > zmax) { zmin = 0.0f; zmax = 1.0f; }
    }

    uint32_t Generation() { return g_generation; }
    int Resident() { return g_resident; }
    const char* Status() { return g_status; }

    void ReleaseTextures()
    {
        for (int j = 0; j < kBlock; ++j)
            for (int i = 0; i < kBlock; ++i)
            {
                Cell& c = g_cells[j][i];
                if (c.volume) bk::Release(c.volume);
                if (c.sky) bk::Release(c.sky);
                if (c.height) bk::Release(c.height);
                c = Cell();
            }
        if (g_volume) { g_volume->Release(); g_volume = nullptr; }
        if (g_sky)    { g_sky->Release();    g_sky = nullptr; }
        if (g_height) { g_height->Release(); g_height = nullptr; }
        g_haveBlock = false;
        g_resident = 0;
        ++g_generation;
    }
}
