// wxl-forever: baked textures (5.tools/forever-bake) found through their manifests and streamed
// from the client's files within a memory budget.
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

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <string>
#include <vector>

// Every bake type lives under Textures\Forever\<Type>\ in the patch folder: a manifest.csv (first
// line "# forever-bake <type> manifest, format N. ...", then the column names, the first column
// the key) and the DDS files it names. A consumer reads its manifest, asks for files by name with
// a priority, and gets a texture once the worker thread has read the bytes and the render thread
// has created it (Pump, a few per frame). Files are kept resident by recent use within the budget;
// the least recently touched go first. A file that is missing or broken never fails a consumer:
// Get returns null and the neutral textures stand in. The log gets one line whenever residency
// changes (rate limited).
namespace wxl::forever::baked
{
    constexpr int kFormatVersion = 1;

    /// A manifest's rows, addressable by key and column name.
    class Manifest
    {
    public:
        /// Reads Textures\Forever\<type>\<file> through the client's file system (loose patch
        /// folder second). False when absent; the object is then empty.
        bool Load(const char* type, const char* file = "manifest.csv");

        int          Version() const { return version_; }
        size_t       Count() const { return rows_.size(); }
        const char*  Type() const { return type_.c_str(); }
        int          Column(const char* name) const;   // -1 when absent
        /// Row index by key, or -1.
        int          Find(const char* key) const;
        const char*  Field(int row, int column) const;   // "" when out of range
        float        Number(int row, int column, float fallback = 0.0f) const;
        const char*  Key(int row) const { return Field(row, 0); }
        /// The folder the manifest's files live in, "Textures\Forever\<type>\".
        const std::string& Folder() const { return folder_; }

    private:
        std::string type_, folder_;
        int version_ = 0;
        std::vector<std::string> columns_;
        std::vector<std::vector<std::string>> rows_;
        std::vector<std::pair<std::string, int>> index_;   // sorted keys
    };

    // --- streaming ---------------------------------------------------------------------------------

    using AssetId = uint32_t;   // 0 = none

    enum : uint32_t
    {
        kWantTexture = 0,   // create a device texture (MANAGED pool) once read
        kWantBytes   = 1,   // keep the file bytes only; the consumer decodes them itself (an atlas)
    };

    /// Asks for a file (a path relative to Textures\Forever\, or as the manifest names it with a
    /// folder). The same path returns the same id. Higher priority reads first. Touches the asset.
    AssetId Request(const std::string& path, int priority, uint32_t want = kWantTexture);

    /// The texture, or null while pending, failed or evicted (request it again to reload). Touches it.
    IDirect3DBaseTexture9* Texture(AssetId id);

    /// The bytes of a kWantBytes asset once read, or null. Valid until the asset is released.
    const std::string* Bytes(AssetId id);

    enum class State : uint8_t { None, Pending, Ready, Failed, Evicted };
    State StateOf(AssetId id);

    /// The consumer is done with it: it may be evicted once the budget needs the room.
    void Release(AssetId id);

    /// Render thread, once per frame: creates textures for finished reads (up to a few per frame),
    /// evicts least recently touched assets over the budget, logs residency changes.
    void Pump(IDirect3DDevice9* dev, uint32_t frame);

    /// Bytes the resident set may hold; default 64 MB (WXL_FOREVER_BAKED_BUDGET_MB).
    void   SetBudget(size_t bytes);
    size_t Budget();

    struct Stats
    {
        uint32_t resident, pending, failed;
        size_t   bytes;
    };
    Stats GetStats();
    const char* Status();   // one line for a panel

    /// Neutral stand-ins: 1 x 1 white (L8 255) textures of each kind, created on first use.
    IDirect3DTexture9*       White2D(IDirect3DDevice9* dev);
    IDirect3DCubeTexture9*   WhiteCube(IDirect3DDevice9* dev);
    IDirect3DVolumeTexture9* WhiteVolume(IDirect3DDevice9* dev);

    /// Frees the neutral textures and every resident texture; assets return to Evicted and reload
    /// on the next Request. Called on device loss (MANAGED textures survive a reset, but a full
    /// release keeps the accounting simple).
    void ReleaseAll();

    /// Stops the worker; called nowhere yet (the process ends with the client).
    void Shutdown();

    // --- per-tile helpers ------------------------------------------------------------------------

    constexpr float kTileSize = 533.0f + 1.0f / 3.0f;

    /// The ADT tile holding a world point: cx along -Y (west to east), cy along -X (north to south).
    void TileOf(const float world[3], int& cx, int& cy);

    /// "Map_cx_cy", the key the per-tile manifests use.
    std::string TileKey(const char* map, int cx, int cy);

    /// The tiles within ring tiles of the one holding eye, nearest first; returns how many were
    /// written (at most cap).
    int TilesAround(const float eye[3], int ring, int* cxOut, int* cyOut, int cap);
}
