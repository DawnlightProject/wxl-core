// wxl-host: BLP2 texture images in the form the client's texture loader reads them.
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
#include <vector>

// The client's loader (PumpBlpTextureAsync, 0x004B7BD0) copies a BLP2 header and builds its mip table from
// it: DXT and raw ARGB files are used in place (the table points into the file), palettized files are decoded
// on the main thread into a scratch chain. A decoded image is the same header with compression 3 and its
// levels already in ARGB8888, which the loader then takes in place too. Pure code: no Windows, no host state,
// so the probe compiles it as well.
namespace wxl::hostd::blp
{
    /// Magic through palette: the bytes the loader copies out of a file (CBLPFile::Source, 0x006AE900).
    constexpr uint32_t kHeaderBytes = 0x494;
    constexpr uint32_t kMaxLevels = 16;

    /// File header fields, at their offsets in the file.
    namespace field
    {
        constexpr size_t kMagic = 0x00;
        constexpr size_t kType = 0x04;
        constexpr size_t kCompression = 0x08;
        constexpr size_t kAlphaDepth = 0x09;
        constexpr size_t kPreferredFormat = 0x0A;
        constexpr size_t kHasMips = 0x0B;
        constexpr size_t kWidth = 0x0C;
        constexpr size_t kHeight = 0x10;
        constexpr size_t kOffsets = 0x14;
        constexpr size_t kSizes = 0x54;
        constexpr size_t kPalette = 0x94;
    }

    constexpr uint32_t kMagicBlp2 = 0x32504C42;   // 'BLP2'
    constexpr uint8_t kCompressionJpeg = 0, kCompressionPalette = 1, kCompressionDxt = 2, kCompressionArgb = 3;

    /// One level as the loader reaches it: bytes at offset, and how many a GPU level of it holds.
    struct Level
    {
        uint32_t offset;
        uint32_t bytes;
    };

    /// True for a BLP2 header the loader accepts (magic and type, CBLPFile::Source).
    bool IsBlp2(const uint8_t* file, size_t size);

    /**
     * @brief Levels of an image whose GPU copy is its own bytes: DXT and ARGB8888 files, and decoded images.
     *
     * Stops where the loader's in-place walk stops (the first empty size), and for DXT where the upload stops
     * (the smaller side at 4). A level is listed with the bytes a GPU level of its dimensions holds, and only
     * when the file holds them all.
     * @return false for an image whose levels are converted on upload (nothing to list).
     */
    bool GpuLevels(const uint8_t* image, size_t size, std::vector<Level>& out);

    /**
     * @brief Decodes a palettized BLP2 into the image the client's loader would build from it.
     *
     * Decodes only what the loader turns into ARGB8888 levels with a full mip chain, in a texture no larger
     * than the device allows (no level is skipped). Everything else is left to the client: false.
     * @param maxEdge  the device's largest texture edge (CGxDevice caps +0x6C).
     * @param out      receives the decoded image (a BLP2 header then the levels).
     */
    bool Decode(const uint8_t* file, size_t size, uint32_t maxEdge, std::vector<uint8_t>& out);

    /// Number of levels the loader walks for a width and height (CalcLevelCount, 0x006AB700).
    uint32_t LevelCount(uint32_t width, uint32_t height);
}
