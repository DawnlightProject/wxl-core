// CDataStore: the byte buffer with a cursor that every serialized message in the client is built
// in. Its layout, its writers, and the teardown they need.
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

// INTERNAL to the core. Modules use wxl::game::OutMessage / InMessage.
//
// Every address below was read off the instruction stream rather than the Ghidra export, because
// the export gets the calling conventions wrong in at least one place here. Each Put was identified
// by the two bytes that differ between them: the `lea ecx,[eax+N]` that reserves N bytes, and the
// `ret N` that says how much the callee pops.
namespace wxl::offsets::game::datastore
{
    // --- the object ------------------------------------------------------------------------------
    // The read cursor arithmetic is CDataStore::GetDataInSitu's own: the byte at the cursor lives at
    // (m_data - m_base) + m_read. m_base is the logical offset the buffer starts at and is 0 for the
    // stack-built store an inbound message is wrapped in.
#pragma pack(push, 1)
    struct DataStore
    {
        void*    vtable;  // +0x00
        uint8_t* data;    // +0x04
        uint32_t base;    // +0x08
        uint32_t alloc;   // +0x0C  (-1 = the buffer is not owned)
        uint32_t size;    // +0x10  bytes present
        uint32_t read;    // +0x14  read cursor; -1 means the store is in write mode
    };
#pragma pack(pop)
    static_assert(sizeof(DataStore) == 0x18, "CDataStore is 0x18 bytes");
    static_assert(offsetof(DataStore, data) == 0x04, "CDataStore::m_data");
    static_assert(offsetof(DataStore, size) == 0x10, "CDataStore::m_size");
    static_assert(offsetof(DataStore, read) == 0x14, "CDataStore::m_read");

    /// CDataStore's vtable, written into the object before anything else touches it.
    constexpr uintptr_t kVTable = 0x009E0E24;

    /// Write mode. A store is built with this in m_read and put back to 0 when the message is
    /// finished -- NetClient::Send treats m_size - m_read == 0 as "nothing to send", so this is a
    /// state flag and not a cursor position.
    constexpr uint32_t kWriteMode = 0xFFFFFFFFu;

    /// "the buffer is not owned", the value the teardown refuses to free.
    constexpr uint32_t kUnowned = 0xFFFFFFFFu;

    // --- the writers -----------------------------------------------------------------------------
    // All __thiscall with the store in ECX. The one-dword forms pop 4, the eight-byte form pops 8.

    using PutFn    = void(__fastcall*)(void* self, void* edx, uint32_t value);
    using Put64Fn  = void(__fastcall*)(void* self, void* edx, uint64_t value);
    using PutArrayFn = void(__fastcall*)(void* self, void* edx, const void* data, uint32_t bytes);

    constexpr uintptr_t kPutInt8  = 0x0047AFE0;  ///< lea ecx,[eax+1] / ret 4
    constexpr uintptr_t kPutInt16 = 0x0047B040;  ///< lea ecx,[eax+2] / ret 4
    constexpr uintptr_t kPutInt32 = 0x0047B0A0;  ///< lea ecx,[eax+4] / ret 4
    constexpr uintptr_t kPutInt64 = 0x0047B100;  ///< lea ecx,[eax+8] / ret 8

    /// CDataStore::PutArray(src, bytes). Asserts on a null src with a non-zero count.
    /// CDataStore::PutData (0x0047B280) is a twelve-byte thunk that jumps straight here.
    constexpr uintptr_t kPutArray = 0x0047B1C0;

    /// CDataStore::InternalDestroy(&m_data, &m_base, &m_alloc). Frees the grown buffer and zeroes
    /// all three. NOT a method: it ends in `ret 0xC` and takes its three pointers on the stack, so
    /// calling it as a thiscall pops twelve bytes off the caller's frame. Its own first act is to
    /// return when *alloc is 0; the client still guards the call with m_alloc != -1.
    constexpr uintptr_t kInternalDestroy = 0x0047AE50;
    using InternalDestroyFn = void(__stdcall*)(uint8_t** data, uint32_t* base, uint32_t* alloc);
}
