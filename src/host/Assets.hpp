// wxl-host: files and texture images for the client, read through the cache.
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

#include "host/Cache.hpp"
#include "ipc/Protocol.hpp"

#include <cstdint>
#include <string>

// The layer between a request and the archives: a lookup resolves as the client's archive layer would, then
// the bytes come from the cache or are read (and, for a texture, decoded) and kept there. Asset files the
// client streams (textures, models, tiles) are cached; the rest is read straight into the reply.
namespace wxl::hostd::assets
{
    /// Archive thread slot of a caller that is not an enkiTS worker.
    constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

    void Init(ipc::HostCounters* counters);

    /// What a read resolved to.
    struct Served
    {
        cache::Blob       bytes;           // null when the file was read straight into the destination
        uint64_t          size = 0;
        uint32_t          archive = ipc::kAnyArchive;
        ipc::TextureImage image = ipc::TextureImage::File;
        uint64_t          flags = 0;       // ipc::kServedFromCache, ipc::kServedPrefetched
        cache::Key        key;             // where the bytes live (for the dedupe index)
        bool              immutable = false;   // read from an MPQ: the same key always rebuilds the same bytes
    };

    /**
     * @brief Serves a client read of a whole file.
     * @param dst, cap  where an uncached file is read directly; a cached one is copied by the caller.
     * @return Ok, NotFound, NeedMore (size set) or Failed.
     */
    ipc::Status ReadFile(uint32_t thread, const char* name, uint32_t archive, uint8_t* dst, uint64_t cap,
                         Served& out);

    /**
     * @brief Serves a client texture read: the BLP's texture image (blp::Decode, or the file itself).
     * @param maxEdge  the device's largest texture edge.
     * @param cap      what the reply can carry: a larger image is sized for the retry but not counted used.
     */
    ipc::Status ReadTexture(uint32_t thread, const char* name, uint32_t archive, uint32_t maxEdge, uint64_t cap,
                            Served& out);

    /// True for a file worth keeping in the cache (textures, models, tiles).
    bool Cacheable(const std::string& lowerName);

    /**
     * @brief Reads a file into the cache ahead of the client.
     * @param texture  keep a BLP as its texture image.
     * @return the bytes (cached or just read), or null when absent.
     */
    cache::Blob Prefetch(uint32_t thread, const std::string& name, bool texture, uint32_t maxEdge, bool* read);

    /// Rebuilds an entry's bytes from its archive (an MPQ key), for a reference whose entry was evicted.
    cache::Blob Rebuild(uint32_t thread, const cache::Key& key);
}
