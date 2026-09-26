// The BLP header stand-in a texture handle returns: the image's header with its level offsets rebased.
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

#include <algorithm>
#include <cstdint>
#include <cstring>

// Pure and header-only: HostTextures writes it into the loader's buffer, and the probe runs the loader's own
// code over it. The loader reaches level i as buffer + offset[i] in 32-bit arithmetic, so an offset of
// (image + offset[i] - buffer) mod 2^32 lands on the level wherever the image is.
namespace wxl::runtime::storage::textures
{
    /// What a texture handle reports as its size: the header the loader copies (CBLPFile::Source).
    constexpr uint32_t kStubBytes = 0x494;

    namespace stub
    {
        constexpr uint32_t kOffsets = 0x14;
        constexpr uint32_t kSizes = 0x54;
        constexpr uint32_t kMaxLevels = 16;
        constexpr uint32_t kMagicBlp2 = 0x32504C42;

        inline uint32_t U32(const uint8_t* p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        inline bool IsBlp2(const uint8_t* p, uint32_t size)
        {
            return p && size >= kStubBytes && U32(p) == kMagicBlp2 && U32(p + 4) == 1;
        }
    }

    /**
     * @brief Writes [pos, pos + len) of the stand-in for an image into dst.
     * @param base  address of the reader's buffer (dst - pos when it reads into one buffer from the start).
     *
     * Anything that is not a BLP2 header is passed through unchanged, so the loader refuses it as it would the
     * file; a short image is completed with zeroes.
     */
    inline void WriteStubBytes(const uint8_t* image, uint32_t size, uintptr_t base, uint32_t pos, uint8_t* dst,
                               uint32_t len)
    {
        if (pos >= kStubBytes || !len) return;
        const uint32_t n = std::min(len, kStubBytes - pos);
        uint8_t header[kStubBytes] = {};
        if (image) std::memcpy(header, image, std::min(size, kStubBytes));
        if (stub::IsBlp2(image, size))
            for (uint32_t i = 0; i < stub::kMaxLevels; ++i)
            {
                if (!stub::U32(header + stub::kSizes + i * 4)) continue;
                const uint32_t target = uint32_t(reinterpret_cast<uintptr_t>(image)) + stub::U32(header + stub::kOffsets + i * 4);
                const uint32_t rebased = target - uint32_t(base);
                std::memcpy(header + stub::kOffsets + i * 4, &rebased, 4);
            }
        std::memcpy(dst, header + pos, n);
    }
}
