// BLP textures served by wxl-host: the loader reads a header stand-in, its levels live in shared memory.
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

#include "engine/storage/TextureStub.hpp"
#include "runtime/host/HostClient.hpp"

#include <cstdint>

// The client's texture loader reads a BLP whole into a heap buffer the size SFile reports, then only ever
// reads its 0x494-byte header and each level at buffer + offset (offsets/engine/Texture.hpp). So a texture
// handle reports the header's size, and the header it reads carries offsets that land, modulo 2^32, on the
// levels wxl-host wrote into the transfer window: already decoded to ARGB8888 for a palettized file, the
// file's own levels otherwise. The loader then takes its zero-copy path and uploads from shared memory.
// Nothing else about the load changes, and a texture the host cannot serve is read the native way.
namespace wxl::runtime::storage::textures
{
    /**
     * @brief Decides whether an archive-layer open is the texture loader asking for a BLP.
     *
     * True once per TextureCreate call, for a .blp at least a header long that no client transform
     * reshapes. The open then gets a texture handle.
     */
    bool Claim(const char* name, uint64_t size);

    /// The image a texture handle reads from.
    struct Image
    {
        host::FileData file;             // in the window or a big section
        uint8_t*       owned = nullptr;  // or bytes read natively
        const uint8_t* data = nullptr;
        uint32_t       size = 0;
        uint32_t       fileSize = 0;     // the BLP's own size, for the counters
        bool           decoded = false;
    };

    /**
     * @brief Fetches the image of a claimed texture: the host's texture image, checked in verify mode.
     * @return false when the host could not serve it (the caller reads the file natively).
     */
    bool Fetch(const char* name, uint32_t archive, uint32_t fileSize, Image& out);

    /// Uses bytes read natively as the image (the fallback). Takes ownership of bytes (malloc).
    void Adopt(uint8_t* bytes, uint32_t size, uint32_t fileSize, Image& out);

    /**
     * @brief Writes [pos, pos + len) of the header stand-in into dst.
     * @param base  where the reader's buffer starts: the offsets are rebased on it.
     */
    void WriteStub(const Image& image, uintptr_t base, uint32_t pos, uint8_t* dst, uint32_t len);

    /// Releases an image and counts the texture done.
    void Release(Image& image);

    /// One log line of counters, for the memory report.
    void LogCounters(const char* reason);
}
