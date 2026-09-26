// The resource statistics the d3d9 proxy exports and the core's memory diagnostic reads.
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

#include <cstdint>

// Plain data, shared by two modules built from this tree: the proxy fills it, the core reads it through
// the proxy's WXL_ProxyStats export. Bump kGpuStatsVersion on any layout change.
namespace wxl::gpu
{
    constexpr uint32_t kGpuStatsVersion = 2;
    constexpr uint32_t kMaxCreators = 32;   // modules that created at least one resource
    constexpr uint32_t kCreatorNameLen = 48;

    enum class ResType : uint32_t { Texture, Cube, Volume, VertexBuffer, IndexBuffer, Surface, Count };
    enum class ResPool : uint32_t { Default, Managed, SystemMem, Scratch, Count };

    constexpr uint32_t kTypeCount = static_cast<uint32_t>(ResType::Count);
    constexpr uint32_t kPoolCount = static_cast<uint32_t>(ResPool::Count);

    /// One (creator, type, pool) cell: live objects and their estimated bytes.
    struct GpuCell
    {
        uint32_t count;
        uint32_t reserved;
        uint64_t bytes;
    };

    /// Managed-pool emulation counters (Phase C). All zero on a classic device.
    struct GpuEmulation
    {
        uint64_t emulatedObjects;   // live resources created MANAGED and emulated in DEFAULT
        uint64_t emulatedBytes;     // their estimated size: the system-memory copies no longer held
        uint64_t mirrorBytes;       // system-memory mirrors kept for resources that need one
        uint64_t stagingBytes;      // staging surfaces and buffers currently pooled
        uint64_t uploads;           // lock/unlock uploads performed
        uint64_t uploadBytes;       // bytes uploaded through staging
        uint64_t readbacks;         // read locks served from a mirror or a backing copy
        uint64_t backingSent;       // backing copies handed to the host
        uint64_t backingBytes;      // bytes handed to the host for backing
        uint64_t backingDropped;    // backing copies skipped (host unavailable or window full)
        uint64_t restores;          // resources refilled after a device loss
    };

    struct GpuStats
    {
        uint32_t structSize;
        uint32_t version;
        uint32_t d3d9ex;            // 1 when the device was created through CreateDeviceEx
        uint32_t creatorCount;
        char     creators[kMaxCreators][kCreatorNameLen];
        GpuCell  cells[kMaxCreators][kTypeCount][kPoolCount];
        uint64_t untracked;         // resources whose creator could not be attributed
        GpuEmulation emulation;
    };

    /// Signature of the proxy's WXL_ProxyStats export. Returns 0 when out is null or too small.
    using ProxyStatsFn = int(__cdecl*)(GpuStats* out, uint32_t outSize);
}
