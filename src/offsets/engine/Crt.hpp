// The C runtime Wow.exe links statically (Visual Studio 2005): the heap functions under the engine allocator.
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

// Every engine allocation ends here: SMemAlloc (engine/Mem.hpp) is malloc or calloc, and the few
// engine paths that bypass Storm (Battle.net, expat, zlib, the Warden loader) call these directly.
// In heap mode 1 (the system heap, what the CRT picks on NT 5+) they are HeapAlloc, HeapReAlloc,
// HeapFree and HeapSize on _crtheap, so _msize returns the exact size that was asked for.
namespace wxl::offsets::engine::crt
{
    // malloc(size)
    constexpr uintptr_t kMalloc     = 0x00415074;
    // _calloc_impl(count, size, errnoOut): the one path under both calloc and _calloc_crt.
    constexpr uintptr_t kCallocImpl = 0x00416938;
    // realloc(ptr, size): a null ptr allocates (through malloc), size 0 frees (through free).
    constexpr uintptr_t kRealloc    = 0x00416A95;
    // free(ptr)
    constexpr uintptr_t kFree       = 0x00412FC7;
    // _msize(ptr): must never see null, it raises the invalid-parameter handler.
    constexpr uintptr_t kMsize      = 0x004112F8;
    // HANDLE _crtheap, created by _heap_init before the first allocation.
    constexpr uintptr_t kHeap       = 0x00B31684;
    // int __active_heap: 1 = the system heap, 3 = the V6 small-block heap.
    constexpr uintptr_t kActiveHeap = 0x00DD0348;

    using MallocFn     = void*(__cdecl*)(size_t size);
    using CallocImplFn = void*(__cdecl*)(size_t count, size_t size, int* errnoOut);
    using ReallocFn    = void*(__cdecl*)(void* ptr, size_t size);
    using FreeFn       = void(__cdecl*)(void* ptr);
    using MsizeFn      = size_t(__cdecl*)(void* ptr);
}
