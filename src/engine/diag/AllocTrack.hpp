// Engine allocations by source: Storm's allocator and the CRT heap under it, counted per call site.
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

/// Live bytes of every engine allocation, keyed by the source the allocator was given.
///
/// SMemAlloc(size, file, line, flags) receives the file name and line of each engine allocation. A
/// site is (file pointer, line, owner): the owner is a return address one or two frames up, recorded
/// only for names that say nothing about the caller (operator new, the container templates' type
/// names, BlizzardCore's allocator). Allocations that bypass Storm are counted as CRT-direct totals.
namespace wxl::diag::alloc
{
    constexpr uint32_t kSiteSlots = 8192 + 1;   // the site table, plus the overflow site
    constexpr uint32_t kFileSlots = 2048;       // one per distinct name pointer; slot 0 is the overflow

    /// One call site, as a report reads it. Counters are live values, read without a lock.
    struct SiteView
    {
        const char* raw = nullptr;     // the name as the engine passed it (a file's base name, or a type)
        uint32_t    line = 0;
        uint32_t    owner = 0;         // caller one or two frames up; generic names only
        uint32_t    ret = 0;           // the allocator's return address, first allocation
        uint16_t    file = 0;          // file-table index: one per distinct name pointer
        bool        generic = false;   // the name does not identify the caller
        uint32_t    bytes = 0;         // live bytes, without the tag
        uint32_t    count = 0;         // live blocks
        uint32_t    peak = 0;          // highest live bytes seen
        uint32_t    allocs = 0;        // allocations ever made here
    };

    struct Totals
    {
        uint64_t stormBytes = 0, stormPeak = 0, stormBlocks = 0;
        uint64_t tagBytes = 0;                    // what the tags add to the CRT heap
        uint64_t directBytes = 0, directPeak = 0, directBlocks = 0;
        uint64_t stormAllocs = 0;                 // cumulative, all sites
        uint64_t directAllocs = 0;
        uint64_t mixedFrees = 0;                  // a CRT block freed by SMemFree
        uint64_t untrackedFrees = 0;              // late start only: blocks older than the hooks
        uint32_t sites = 0, files = 0, overflow = 0;
        int      heapMode = 0;                    // __active_heap: 1 system, 3 small-block
        bool     late = false;                    // hooks installed after the client's CRT started
    };

    /// Installs the Storm and CRT hooks. Boot phase: before the client's CRT allocates anything.
    bool Install();

    /// True once the hooks are installed.
    bool Active();

    /// Sums the counters. Cheap: one pass over the site table.
    Totals ReadTotals();

    /// Number of site slots, the upper bound for ReadSite's index.
    uint32_t SiteSlots();

    /// Reads one slot; false for an empty slot or a generic name's key record.
    bool ReadSite(uint32_t index, SiteView& out);

    /// Live Storm bytes now, and the highest value since the last ResetWindowPeak.
    uint64_t StormBytes();
    uint64_t WindowPeak();
    void     ResetWindowPeak();

    /// The address range of Wow.exe's code, to tell engine return addresses apart.
    bool InEngineCode(uint32_t address);
}
