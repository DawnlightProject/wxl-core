// Taking over a DBC table the client already holds: writing fields of a live row, and rebuilding a
// table's rows wholesale so the client's own 974 compiled call sites read our data instead.
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
#include <string>
#include <unordered_map>
#include <vector>

#include "game/ClientDb.hpp"
#include "game/Mem.hpp"
#include "offsets/game/ClientDb.hpp"

/**
 * @brief The write half of wxl::game::clientdb. Read it alongside ClientDb.hpp, which it extends.
 *
 * Two operations, in rising order of danger.
 *
 * `Editor` writes fields of rows that are already there. Same rows, same stride, different values.
 * Nothing is allocated and nothing is owned, so it works on any loaded table and cannot leak.
 *
 * `Builder` replaces a table's rows wholesale. It stages records in its own buffer, interns every
 * string into one arena, and at Commit rebuilds exactly what the client's LoadRecords builds: the
 * record block, the by-id array, and -- for the four indexed tables -- the array of record pointers
 * that is allocated in one piece with the by-id array. Every block comes from the engine allocator,
 * so the table's own destructor frees them correctly at shutdown.
 *
 * The two shapes differ in where the blocks live, and that difference is a heap error if it is got
 * wrong. Flat: records at +0x1C and the by-id array at +0x20, two separate allocations, and the
 * table's cleanup frees both. Indexed: records at +0x18, and ONE allocation at +0x1C holding the
 * ordinal array followed by the by-id array, with +0x20 pointing into its middle -- the cleanup
 * frees +0x18 and +0x1C and never touches +0x20. Builder follows the registry's `indexed` flag.
 *
 * Setting `loaded` is also a guard: the client's own Load refuses to run on a table that already
 * carries it ("already loaded! Aborting to prevent memory leak!"), so a table we published cannot
 * be overwritten by a later file load.
 *
 * Main thread only, and not reentrant: Commit publishes six object fields one at a time, and a
 * reader that ran between them would see a half-swapped table.
 */
namespace wxl::game::clientdb
{
    /// Every pointer stored in a record or an index array is one of the client's 32-bit words.
    static_assert(sizeof(void*) == 4, "the client's DBC objects hold 32-bit pointers");

    /// The caller tag the engine allocator records. It reads it only when an allocation fails, on
    /// the way to its own fatal error box; the client itself passes 0 here for the string block.
    inline constexpr const char* kEditTag = "wxl/ClientDbEdit.hpp";

    // --- writing rows that are already there -----------------------------------------------------

    /**
     * @brief One live row, open for writing.
     *
     * Bounds-checked on the table's dword count exactly as the readers are, and for the same
     * reason: an index past the record would otherwise write into the next one.
     */
    class RowWriter
    {
    public:
        RowWriter() = default;
        RowWriter(void* row, uint32_t dwords) : m_row(row), m_dwords(dwords) {}

        explicit operator bool() const { return m_row != nullptr; }

        uint32_t Dwords() const { return m_dwords; }
        void*    Address() const { return m_row; }

        bool SetU32(uint32_t dword, uint32_t value)
        {
            if (!m_row || dword >= m_dwords) return false;
            std::memcpy(static_cast<uint8_t*>(m_row) + dword * 4, &value, 4);
            return true;
        }

        bool SetF32(uint32_t dword, float value)
        {
            uint32_t raw = 0;
            std::memcpy(&raw, &value, 4);
            return SetU32(dword, raw);
        }

        uint32_t U32(uint32_t dword) const
        {
            if (!m_row || dword >= m_dwords) return 0;
            uint32_t value = 0;
            std::memcpy(&value, static_cast<const uint8_t*>(m_row) + dword * 4, 4);
            return value;
        }

        float F32(uint32_t dword) const
        {
            const uint32_t raw = U32(dword);
            float value = 0.0f;
            std::memcpy(&value, &raw, 4);
            return value;
        }

    private:
        void*    m_row    = nullptr;
        uint32_t m_dwords = 0;
    };

    /**
     * @brief Field writes into a table's existing rows.
     *
     * False, and handing out nothing, unless the table is loaded and its records are unpacked.
     * A string field cannot be written here: the pointer would have to aim at a block somebody
     * keeps alive, and only Builder owns one. Use Builder to change strings.
     */
    class Editor
    {
    public:
        Editor() = default;
        explicit Editor(const Table& table) : m_table(table) {}

        static Editor Open(const char* name) { return Editor(Table::Find(name)); }

        /// True when the table can be written at all: found, loaded, and not packed by dbCompress.
        explicit operator bool() const { return m_table && m_table.Loaded() && m_table.Unpacked(); }

        const Table& Read() const { return m_table; }

        RowWriter Row(uint32_t index) const { return Writer(m_table.RowByIndex(index)); }
        RowWriter RowById(uint32_t id) const { return Writer(m_table.RowById(id)); }

    private:
        /// The rows are engine-heap blocks the read layer happens to hand out as const; casting it
        /// away writes to ordinary read-write memory, not to anything the loader made constant.
        RowWriter Writer(const void* row) const
        {
            if (!*this) return RowWriter();
            return RowWriter(const_cast<void*>(row), m_table.Dwords());
        }

        Table m_table;
    };

    // --- replacing a table's rows ----------------------------------------------------------------

    /// What Commit does with the blocks the table held before it.
    enum class Reclaim
    {
        /// Leave them allocated. The table's destructor then frees what we published and the
        /// originals leak: bounded and harmless for a one-shot takeover at startup, unbounded
        /// for anything that rebuilds while the game runs.
        Keep,

        /// Free them, which is what they are owed. Every pointer the client cached out of them --
        /// a record address it held on to, a char* out of the string block -- dangles from that
        /// moment, so this is only safe before anything has read the table.
        Free,
    };

    /**
     * @brief Stages a table's new rows, then publishes them in one step.
     *
     * The caller writes into the builder rather than handing over bytes, so the layout stays the
     * layer's problem: `Add(id)` puts the id in whichever dword this table keeps it in, min/max are
     * derived from that same dword at Commit, and a string is interned instead of pointed at.
     *
     * A staged record is addressed by ordinal, never by address, so nothing dangles when the
     * staging buffer grows.
     *
     * Every dword a caller does not write stays zero. For a string field that is a null pointer,
     * which the client never produces -- its loader points an absent string at an empty one -- so
     * every string field of every record must be given SetStr, even if only with "".
     */
    class Builder
    {
    public:
        /// A rebuild of `table` at the layout the client compiled for it.
        static Builder For(const Table& table)
        {
            return Builder(table, table.Stride(), table.IdDword());
        }

        /**
         * @brief A rebuild at a stride the client was NOT compiled against.
         *
         * Every one of the client's own readers for this table carries the old stride and the old
         * field offsets as constants. For a flat table that breaks the lookup itself, since
         * GetRecordByIndex is `records + index * stride` with the old stride baked in. For an
         * indexed table the lookup survives -- it goes through the pointer arrays -- but the
         * reader still copies out only the old record size and still reads the old field offsets.
         *
         * So this is correct only once nothing but our code reads the table. It is the door
         * Spell's restructuring goes through, and it is not safe before that.
         *
         * @param idDword  Where the id lives in the new record. The registry's answer describes
         *                 the old layout and does not carry over.
         */
        static Builder UnsafeReshape(const Table& table, uint32_t stride, uint32_t idDword)
        {
            return Builder(table, stride, idDword);
        }

        /// One staged record, addressed by ordinal so the staging buffer may grow underneath it.
        class Record
        {
        public:
            Record() = default;

            explicit operator bool() const { return m_owner != nullptr; }
            uint32_t Index() const { return m_index; }

            bool SetU32(uint32_t dword, uint32_t value);
            bool SetF32(uint32_t dword, float value);

            /// Interns `text` in the builder's arena and points this dword at it after Commit.
            /// A null or empty text still gets a real empty string, never a null pointer.
            bool SetStr(uint32_t dword, const char* text);

        private:
            friend class Builder;
            Record(Builder* owner, uint32_t index) : m_owner(owner), m_index(index) {}

            Builder* m_owner = nullptr;
            uint32_t m_index = 0;
        };

        /// True when the table can be rebuilt at all: found, not packed, and the stride is sane.
        explicit operator bool() const { return m_ok; }

        const Table& Target() const { return m_table; }

        uint32_t Stride() const { return m_stride; }
        uint32_t Dwords() const { return m_stride / 4; }
        uint32_t IdDword() const { return m_idDword; }
        uint32_t Rows() const { return m_stride ? uint32_t(m_records.size() / m_stride) : 0; }

        /// Appends a zero-filled record carrying `id` in the table's id dword.
        Record Add(uint32_t id)
        {
            if (!m_ok) return Record();
            m_records.resize(m_records.size() + m_stride, 0);
            Record record(this, Rows() - 1);
            record.SetU32(m_idDword, id);
            return record;
        }

        /// Builds the blocks, points the table at them, and marks it loaded. False leaves the
        /// table exactly as it was.
        bool Commit(Reclaim reclaim = Reclaim::Keep);

    private:
        Builder(const Table& table, uint32_t stride, uint32_t idDword)
            : m_table(table), m_stride(stride), m_idDword(idDword)
        {
            m_ok = table && table.Unpacked() && stride >= 4 && (stride % 4) == 0
                   && idDword < stride / 4;
        }

        /// A string dword, and the arena offset it must become a pointer to. The offset is kept
        /// here rather than read back out of the record, so writing the same dword twice is not a
        /// trap: both fixups still name their own string and the later one wins.
        struct Fixup
        {
            uint32_t row;
            uint32_t dword;
            uint32_t offset;
        };

        bool WriteDword(uint32_t row, uint32_t dword, uint32_t value)
        {
            if (!m_ok || dword >= Dwords()) return false;
            const size_t at = size_t(row) * m_stride + size_t(dword) * 4;
            if (at + 4 > m_records.size()) return false;
            std::memcpy(m_records.data() + at, &value, 4);
            return true;
        }

        bool WriteString(uint32_t row, uint32_t dword, const char* text)
        {
            const uint32_t offset = Intern(text ? text : "");
            if (!WriteDword(row, dword, offset)) return false;
            m_fixups.push_back(Fixup{ row, dword, offset });
            return true;
        }

        /// The arena is one growing buffer, so a string is a byte offset until Commit turns the
        /// buffer into a single allocation and the offsets into pointers.
        uint32_t Intern(const char* text)
        {
            std::string key(text);
            auto it = m_interned.find(key);
            if (it != m_interned.end()) return it->second;

            const uint32_t offset = uint32_t(m_strings.size());
            m_strings.append(key);
            m_strings.push_back('\0');
            m_interned.emplace(std::move(key), offset);
            return offset;
        }

        static uint32_t Field(const void* object, size_t offset)
        {
            uint32_t value = 0;
            std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, 4);
            return value;
        }

        static void Publish(void* object, size_t offset, uint32_t value)
        {
            std::memcpy(static_cast<uint8_t*>(object) + offset, &value, 4);
        }

        static uint32_t Address(const void* p) { return uint32_t(reinterpret_cast<uintptr_t>(p)); }

        Table    m_table;
        uint32_t m_stride  = 0;
        uint32_t m_idDword = 0;
        bool     m_ok      = false;

        std::vector<uint8_t>                     m_records;
        std::string                              m_strings;
        std::unordered_map<std::string, uint32_t> m_interned;
        std::vector<Fixup>                       m_fixups;
    };

    inline bool Builder::Record::SetU32(uint32_t dword, uint32_t value)
    {
        return m_owner && m_owner->WriteDword(m_index, dword, value);
    }

    inline bool Builder::Record::SetF32(uint32_t dword, float value)
    {
        uint32_t raw = 0;
        std::memcpy(&raw, &value, 4);
        return SetU32(dword, raw);
    }

    inline bool Builder::Record::SetStr(uint32_t dword, const char* text)
    {
        return m_owner && m_owner->WriteString(m_index, dword, text);
    }

    inline bool Builder::Commit(Reclaim reclaim)
    {
        namespace off = wxl::offsets::game::clientdb;

        const uint32_t rows = Rows();
        void* object = m_table.Object();
        if (!m_ok || rows == 0 || !object) return false;

        // min/max off the id dword of the staged records, the way every loader derives them.
        // Compared unsigned here; the client compares them signed, in the loader and again in its
        // by-id lookup, so an id with the top bit set is outside what either side handles.
        uint32_t minId = 0xFFFFFFFFu;
        uint32_t maxId = 0;
        for (uint32_t i = 0; i < rows; ++i)
        {
            uint32_t id = 0;
            std::memcpy(&id, m_records.data() + size_t(i) * m_stride + size_t(m_idDword) * 4, 4);
            if (id < minId) minId = id;
            if (id > maxId) maxId = id;
        }

        // The by-id array is dense over the whole id span, so a sparse table with a huge span asks
        // for a huge block. Refuse only what will not fit a 32-bit size, which is the allocator's
        // own limit.
        const uint64_t recordBytes  = uint64_t(rows) * m_stride;
        const uint64_t ordinalBytes = m_table.Indexed() ? uint64_t(rows) * 4 : 0;
        const uint64_t byIdBytes    = (uint64_t(maxId) - minId + 1) * 4;
        if (recordBytes > 0xFFFFFFFFull || ordinalBytes + byIdBytes > 0xFFFFFFFFull) return false;

        auto* records = static_cast<uint8_t*>(mem::Alloc(uint32_t(recordBytes), kEditTag, __LINE__));
        if (!records) return false;
        std::memcpy(records, m_records.data(), size_t(recordBytes));

        // The string block is only replaced when there is something to put in it; leaving the
        // client's own in place keeps every char* it already handed out valid.
        char* strings = nullptr;
        if (!m_strings.empty())
        {
            strings = static_cast<char*>(
                mem::Alloc(uint32_t(m_strings.size()), kEditTag, __LINE__));
            if (!strings)
            {
                mem::Free(records, kEditTag, __LINE__);
                return false;
            }
            std::memcpy(strings, m_strings.data(), m_strings.size());

            for (const Fixup& fix : m_fixups)
            {
                const uint32_t pointer = Address(strings + fix.offset);
                std::memcpy(records + size_t(fix.row) * m_stride + size_t(fix.dword) * 4,
                            &pointer, 4);
            }
        }

        auto* index = static_cast<uint8_t*>(
            mem::Alloc(uint32_t(ordinalBytes + byIdBytes), kEditTag, __LINE__));
        if (!index)
        {
            mem::Free(records, kEditTag, __LINE__);
            mem::Free(strings, kEditTag, __LINE__);
            return false;
        }

        // Indexed: the ordinal array first, the by-id array carved out behind it in the same block.
        // Flat: one block, all of it by-id. Either way only the by-id part is zeroed, because the
        // ordinal part is written in full below -- this is the client's own sequence.
        uint8_t* byId = index + size_t(ordinalBytes);
        std::memset(byId, 0, size_t(byIdBytes));

        for (uint32_t i = 0; i < rows; ++i)
        {
            uint8_t* record = records + size_t(i) * m_stride;
            const uint32_t pointer = Address(record);

            if (m_table.Indexed())
                std::memcpy(index + size_t(i) * 4, &pointer, 4);

            uint32_t id = 0;
            std::memcpy(&id, record + size_t(m_idDword) * 4, 4);
            std::memcpy(byId + size_t(id - minId) * 4, &pointer, 4);
        }

        // What the table held, read before anything is overwritten.
        const bool     wasLoaded      = m_table.Loaded();
        const uint32_t oldRecordBlock = Field(object, off::kOffRecordBlock);
        const uint32_t oldRecords     = Field(object, off::kOffRecords);
        const uint32_t oldRowById     = Field(object, off::kOffRowById);
        const uint32_t oldStrings     = Field(object, off::kOffStringBlock);

        if (m_table.Indexed())
        {
            Publish(object, off::kOffRecordBlock, Address(records));
            Publish(object, off::kOffRecords, Address(index));
        }
        else
        {
            Publish(object, off::kOffRecords, Address(records));
        }
        Publish(object, off::kOffRowById, Address(byId));
        if (strings) Publish(object, off::kOffStringBlock, Address(strings));

        Publish(object, off::kOffMaxId, maxId);
        Publish(object, off::kOffMinId, minId);
        Publish(object, off::kOffRecordCount, rows);
        Publish(object, off::kOffLoaded, 1);

        if (reclaim == Reclaim::Free && wasLoaded)
        {
            // Indexed: +0x20 was carved out of the +0x1C block and is not its own allocation.
            mem::Free(reinterpret_cast<void*>(oldRecords), kEditTag, __LINE__);
            if (m_table.Indexed())
                mem::Free(reinterpret_cast<void*>(oldRecordBlock), kEditTag, __LINE__);
            else
                mem::Free(reinterpret_cast<void*>(oldRowById), kEditTag, __LINE__);

            if (strings) mem::Free(reinterpret_cast<void*>(oldStrings), kEditTag, __LINE__);
        }

        return true;
    }
}
