// Wow.exe's process memory outside the engine allocator: heaps, VirtualAlloc regions, stacks, modules.
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
#include <string>
#include <vector>

/// Who owns the rest of the address space: every heap with the module that created it, every
/// VirtualAlloc reservation with the module that asked for it, thread stacks, and loaded images.
namespace wxl::diag::process
{
    /// Dxvk: the Vulkan translation of D3D9 that wxl-graphics-extend loads into Wow.exe.
    enum class Group : uint8_t { Game, Ours, Dxvk, System, Driver, Other, Count };

    /// Short label of a module group.
    const char* GroupName(Group g);

    struct Module
    {
        uintptr_t   base = 0;
        uint32_t    size = 0;          // SizeOfImage: the span it takes in the address space
        std::string name;              // file name
        std::string path;
        Group       group = Group::Other;
    };

    struct Heap
    {
        uintptr_t   handle = 0;
        std::string owner;             // "process", "Wow.exe CRT", or the creating module
        Group       group = Group::Other;
        bool        walked = false;    // HeapSummary ran
        bool        skipped = false;   // not walked this time (the CRT heap, or a slow heap)
        uint64_t    allocated = 0, committed = 0, reserved = 0;
        double      walkMs = 0.0;
    };

    /// One reservation (all regions sharing an allocation base).
    struct Allocation
    {
        uintptr_t base = 0;
        uint64_t  reserved = 0, committed = 0;
        uint32_t  type = 0;            // MEM_IMAGE, MEM_MAPPED or MEM_PRIVATE
        uint32_t  creator = 0;         // VirtualAlloc's caller, 0 when unknown
        bool      gpuMapped = false;   // write-combined or uncached pages: a GPU allocation the kernel mapped
        Group     group = Group::Other;// the creating module's group, for VirtualAlloc regions and heaps
        std::string label;             // what it is: a module, a heap, a stack, a mapped file
    };

    /// Installs the HeapCreate/HeapDestroy and VirtualAlloc/VirtualFree hooks (Boot phase).
    bool Install();

    /// Every loaded module, biggest first.
    std::vector<Module> Modules();

    /// The group and file name of the module holding an address; "?" outside every image.
    std::string ModuleOf(uintptr_t address, Group* group = nullptr);

    /**
     * @brief Every heap of the process, summarised.
     * @param walkAll  also walk the CRT heap and heaps that were slow last time (the full dump only).
     */
    std::vector<Heap> Heaps(bool walkAll);

    /// The base of each thread's stack reservation.
    std::vector<uintptr_t> StackBases();

    /**
     * @brief Every live reservation of the address space, labelled.
     * @param heaps   the heap list, to label heap segments.
     * @param names   also resolve mapped files' names (slower; the full dump only).
     */
    std::vector<Allocation> Allocations(const std::vector<Heap>& heaps, bool names);
}
