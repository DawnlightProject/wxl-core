// wxl-host: a keyed store of compressed bytes, kept out of Wow.exe's address space.
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

// Values live in TLSF blocks over pagefile-backed sections of 256 MB, created as the store grows, so the
// total can pass 4 GB while no single allocation needs a contiguous range. Hot values are LZ4 (fast to
// restore); a sweep recompresses values untouched for a minute with zstd.
//
// A value written whole that equals a level of a texture image the host just served (a GPU backing copy
// of a DXT level, say) is kept as a reference to that image instead: no bytes, rebuilt from the archive
// when it is read back. Only images read from an MPQ are referenced, since those never change.
namespace wxl::hostd::store
{
    /**
     * @brief Sets the store up.
     * @param archiveSlot  archive thread slot of the store thread (references read files).
     * @param dedupe       keep whole values that equal served content as references.
     */
    void Init(uint32_t clientPid, uint64_t limitBytes, ipc::HostCounters* counters, uint32_t archiveSlot, bool dedupe);

    /// Indexes the GPU levels of a texture image the client is about to upload (worker threads).
    void IndexImage(const cache::Key& key, const cache::Blob& image);

    ipc::Status Put(uint64_t key, const uint8_t* src, uint64_t size, ipc::Codec codec);

    /// Overwrites [offset, offset + size) of a value, growing it if needed; creates it if absent.
    ipc::Status Patch(uint64_t key, uint64_t offset, const uint8_t* src, uint64_t size);

    /// Copies up to cap bytes into dst; full receives the value's size. NeedMore when truncated.
    ipc::Status Get(uint64_t key, uint8_t* dst, uint64_t cap, uint64_t& full);

    /// Copies rows packed in src (rowBytes each) to dstOffset + r * dstPitch, growing the value if needed.
    ipc::Status PatchRows(uint64_t key, uint64_t dstOffset, uint64_t dstPitch, const uint8_t* src, uint64_t rowBytes,
                          uint64_t rows);

    void Drop(uint64_t key);

    /**
     * @brief Recompresses LZ4 values untouched for idleMs with zstd, up to budgetBytes of input per call.
     * @return true when more cold values remain.
     */
    bool SweepCold(uint64_t idleMs, uint64_t budgetBytes);
}
