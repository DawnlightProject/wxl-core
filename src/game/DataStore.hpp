// Building and reading a client message: CDataStore, with types instead of byte counts.
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
#include <cstring>

#include "game/Binding.hpp"
#include "offsets/game/DataStore.hpp"

/**
 * @brief The two halves of a client message: one you build, one you are handed.
 *
 * OutMessage owns a CDataStore in write mode and grows through the client's own writers, so a
 * message this builds is byte-identical to one the client builds. InMessage borrows a store the
 * client already has -- an inbound packet -- and only reads.
 *
 * Both are main-thread only, and neither is copyable: a CDataStore owns a heap buffer and a second
 * owner would free it twice.
 */
namespace wxl::game
{
    namespace dsoff = wxl::offsets::game::datastore;

    /**
     * @brief A message being built.
     *
     * Constructed in write mode and finished by Finish(), which is what NetClient::Send takes as
     * the signal that the message is complete -- it refuses a store whose size and read cursor are
     * equal. Sending is in game/Net.hpp; this type knows nothing about the network.
     */
    class OutMessage
    {
    public:
        OutMessage()
        {
            m_store.vtable = reinterpret_cast<void*>(dsoff::kVTable);
            m_store.read   = dsoff::kWriteMode;
        }

        ~OutMessage()
        {
            // -1 means the buffer is not owned; anything else was grown by a Put.
            if (m_store.alloc != dsoff::kUnowned)
                Native<dsoff::InternalDestroyFn>(dsoff::kInternalDestroy)(
                    &m_store.data, &m_store.base, &m_store.alloc);
        }

        OutMessage(const OutMessage&) = delete;
        OutMessage& operator=(const OutMessage&) = delete;

        OutMessage& PutU8(uint8_t v)   { Put(dsoff::kPutInt8, v);  return *this; }
        OutMessage& PutU16(uint16_t v) { Put(dsoff::kPutInt16, v); return *this; }
        OutMessage& PutU32(uint32_t v) { Put(dsoff::kPutInt32, v); return *this; }
        OutMessage& PutI8(int8_t v)    { return PutU8(uint8_t(v)); }
        OutMessage& PutI16(int16_t v)  { return PutU16(uint16_t(v)); }
        OutMessage& PutI32(int32_t v)  { return PutU32(uint32_t(v)); }

        OutMessage& PutU64(uint64_t v)
        {
            Native<dsoff::Put64Fn>(dsoff::kPutInt64)(&m_store, nullptr, v);
            return *this;
        }

        /// A float goes in as its four bytes: the client's float writer reserves the same four and
        /// writes the same word, so there is nothing a separate call would do differently.
        OutMessage& PutF32(float v)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &v, 4);
            return PutU32(bits);
        }

        OutMessage& PutBytes(const void* data, uint32_t bytes)
        {
            if (data && bytes)
                Native<dsoff::PutArrayFn>(dsoff::kPutArray)(&m_store, nullptr, data, bytes);
            return *this;
        }

        /// A NUL-terminated string, terminator included, as every CString field in the protocol is.
        OutMessage& PutCString(const char* text)
        {
            const char* s = text ? text : "";
            return PutBytes(s, uint32_t(std::strlen(s)) + 1);
        }

        /**
         * @brief A packed guid: a mask byte, then only the non-zero bytes, low to high.
         *
         * This is the protocol's own compression and it is not optional -- a handler that expects a
         * packed guid cannot read a plain one.
         */
        OutMessage& PutPackedGuid(uint64_t guid)
        {
            uint8_t mask = 0;
            uint8_t bytes[8];
            uint8_t count = 0;

            for (uint8_t i = 0; i < 8; ++i)
            {
                const uint8_t byte = uint8_t(guid >> (i * 8));
                if (byte)
                {
                    mask |= uint8_t(1 << i);
                    bytes[count++] = byte;
                }
            }

            PutU8(mask);
            return PutBytes(bytes, count);
        }

        /// Leaves write mode. Call once, when the message is complete.
        void Finish() { m_store.read = 0; }

        uint32_t Size() const { return m_store.size; }

        /// The store itself, for the one caller that hands it to the client.
        void* Raw() { return &m_store; }

    private:
        void Put(uintptr_t fn, uint32_t value)
        {
            Native<dsoff::PutFn>(fn)(&m_store, nullptr, value);
        }

        dsoff::DataStore m_store{};
    };

    /**
     * @brief A message being read, borrowed from the client.
     *
     * The cursor is the store's own, so reading here advances the store -- a caller that must leave
     * it untouched saves Cursor() and restores it. Every read refuses rather than truncates: a
     * short message leaves the cursor where it was and answers false, so a half-parsed message
     * cannot be mistaken for a parsed one.
     */
    class InMessage
    {
    public:
        explicit InMessage(void* store) : m_store(static_cast<dsoff::DataStore*>(store)) {}

        explicit operator bool() const { return m_store != nullptr; }

        uint32_t Remaining() const
        {
            if (!m_store) return 0;
            return m_store->size > m_store->read ? m_store->size - m_store->read : 0;
        }

        uint32_t Cursor() const          { return m_store ? m_store->read : 0; }
        void     Seek(uint32_t position) { if (m_store) m_store->read = position; }

        bool Read(void* out, uint32_t bytes)
        {
            if (!m_store || Remaining() < bytes) return false;
            std::memcpy(out, m_store->data - m_store->base + m_store->read, bytes);
            m_store->read += bytes;
            return true;
        }

        bool ReadU8(uint8_t& out)   { return Read(&out, 1); }
        bool ReadU16(uint16_t& out) { return Read(&out, 2); }
        bool ReadU32(uint32_t& out) { return Read(&out, 4); }
        bool ReadU64(uint64_t& out) { return Read(&out, 8); }

        bool ReadF32(float& out)
        {
            uint32_t bits = 0;
            if (!ReadU32(bits)) return false;
            std::memcpy(&out, &bits, 4);
            return true;
        }

        /**
         * @brief A packed guid. Answers false, cursor unmoved, on a message too short for its mask.
         */
        bool ReadPackedGuid(uint64_t& out)
        {
            const uint32_t start = Cursor();

            uint8_t mask = 0;
            if (!ReadU8(mask)) return false;

            uint64_t guid = 0;
            for (uint8_t i = 0; i < 8; ++i)
            {
                if (!(mask & (1 << i))) continue;
                uint8_t byte = 0;
                if (!ReadU8(byte)) { Seek(start); return false; }
                guid |= uint64_t(byte) << (i * 8);
            }

            out = guid;
            return true;
        }

        /**
         * @brief A NUL-terminated string, advancing past its terminator.
         * @return false, cursor unmoved, when no terminator is in what remains or it does not fit.
         */
        bool ReadCString(char* out, size_t cap)
        {
            if (!m_store || !out || cap == 0) return false;

            const uint8_t* cursor = m_store->data - m_store->base + m_store->read;
            const uint32_t left   = Remaining();

            uint32_t length = 0;
            while (length < left && cursor[length] != 0) ++length;
            if (length >= left) return false;        // no terminator inside the message
            if (length + 1 > cap) return false;

            std::memcpy(out, cursor, length);
            out[length] = '\0';
            m_store->read += length + 1;
            return true;
        }

    private:
        dsoff::DataStore* m_store = nullptr;
    };
}
