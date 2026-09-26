// Process-wide memory figures shared by the memory report: the address-space walk and the commit counters.
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
#include <psapi.h>

#include <algorithm>
#include <cstdint>

namespace wxl::diag::memory
{
    constexpr double kMb = 1024.0 * 1024.0;

    struct VaMap
    {
        uint64_t imageCommit = 0, imageReserve = 0;
        uint64_t mappedCommit = 0, mappedReserve = 0;
        uint64_t privateCommit = 0, privateReserve = 0;
        uint64_t freeBytes = 0, largestFree = 0;
        uint32_t freeBlocks64 = 0;   // free blocks of at least 64 MB

        uint64_t Used() const
        {
            return imageCommit + imageReserve + mappedCommit + mappedReserve + privateCommit + privateReserve;
        }
    };

    /// One pass over the address space with VirtualQuery: a few milliseconds.
    inline VaMap WalkAddressSpace()
    {
        VaMap m;
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        uintptr_t address = reinterpret_cast<uintptr_t>(info.lpMinimumApplicationAddress);
        const uintptr_t maximum = reinterpret_cast<uintptr_t>(info.lpMaximumApplicationAddress);
        while (address < maximum)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<const void*>(address), &mbi, sizeof mbi) || !mbi.RegionSize) break;
            const uint64_t bytes = mbi.RegionSize;
            if (mbi.State == MEM_FREE)
            {
                m.freeBytes += bytes;
                m.largestFree = std::max(m.largestFree, bytes);
                if (bytes >= (64ull << 20)) ++m.freeBlocks64;
            }
            else
            {
                const bool commit = mbi.State == MEM_COMMIT;
                switch (mbi.Type)
                {
                case MEM_IMAGE:  (commit ? m.imageCommit : m.imageReserve) += bytes; break;
                case MEM_MAPPED: (commit ? m.mappedCommit : m.mappedReserve) += bytes; break;
                default:         (commit ? m.privateCommit : m.privateReserve) += bytes; break;
                }
            }
            const uintptr_t next = address + mbi.RegionSize;
            if (next <= address) break;
            address = next;
        }
        return m;
    }

    struct Counters
    {
        uint64_t privateUsage = 0;
        uint64_t workingSet = 0;
        uint64_t peakWorkingSet = 0;
    };

    /// The commit charge and working set: one system call.
    inline Counters ReadCounters()
    {
        Counters c;
        PROCESS_MEMORY_COUNTERS_EX pmc{};
        pmc.cb = sizeof pmc;
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
        {
            c.privateUsage = pmc.PrivateUsage;
            c.workingSet = pmc.WorkingSetSize;
            c.peakWorkingSet = pmc.PeakWorkingSetSize;
        }
        return c;
    }
}
