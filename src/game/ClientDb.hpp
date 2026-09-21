// Reading any of the 241 DBC tables the client already has in memory, by name and without
// knowing its compiled record layout. One accessor serves every table, because the client
// instantiates them all from one template.
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

#include "offsets/game/ClientDb.hpp"

/**
 * @brief The client's own DBC tables, read in place.
 *
 * `wxl::game::dbc` opens a .dbc file this process does not otherwise know about. This is the other
 * half: the 241 tables the client has already parsed and holds in memory. Nothing is copied, no
 * file is reopened, and a table is addressed by its name rather than by an offset.
 *
 * Rows are reached through the client's own two pointer arrays -- by ordinal and by id -- so no
 * record stride arithmetic happens here and a wrong stride cannot produce a plausible wrong row.
 *
 * A field is read by dword index into the record, which is the layout the client compiled, NOT the
 * column order of the file: every localized string collapses sixteen columns into one pointer. A
 * table with no localized field has the two in step; Spell, with four, diverges after column 135.
 *
 * Main thread only. A table is valid from the moment the client loads it until shutdown; the
 * handle holds an address, not a copy, so it stays correct across a reload of the table's rows.
 */
namespace wxl::game::clientdb
{
    namespace off = wxl::offsets::game::clientdb;

    /// True while rows are stored packed, from the `dbCompress` CVar. Direct row reads are only
    /// meaningful when this is clear -- packed rows expand through the client's own unpacker.
    inline bool Compressed()
    {
        return *reinterpret_cast<const uint8_t*>(off::kCompressFlag) != 0;
    }

    class Table
    {
    public:
        Table() = default;

        /// The table of that name, or an empty handle. Names are the client's own ("SpellRadius").
        static Table Find(const char* name)
        {
            if (!name) return Table();
            for (size_t i = 0; i < off::kTableCount; ++i)
                if (std::strcmp(off::kTables[i].name, name) == 0)
                    return Table(&off::kTables[i]);
            return Table();
        }

        /// The i-th known table, for a walk over all of them.
        static Table At(size_t index)
        {
            return index < off::kTableCount ? Table(&off::kTables[index]) : Table();
        }

        static size_t Count() { return off::kTableCount; }

        explicit operator bool() const { return m_info != nullptr; }

        const char* Name() const    { return m_info ? m_info->name : ""; }
        uint32_t    Columns() const { return m_info ? m_info->columns : 0; }  ///< on disk
        uint32_t    RowSize() const { return m_info ? m_info->rowSize : 0; }  ///< on disk

        /// True once the client has read the file. Everything below answers empty until then.
        bool Loaded() const { return m_info && Field<uint32_t>(off::kOffLoaded) != 0; }

        uint32_t RowCount() const { return Loaded() ? Field<uint32_t>(off::kOffRecordCount) : 0; }
        uint32_t MinId() const    { return Loaded() ? Field<uint32_t>(off::kOffMinId) : 0; }
        uint32_t MaxId() const    { return Loaded() ? Field<uint32_t>(off::kOffMaxId) : 0; }

        /// The row at an ordinal, or null. Order is the file's.
        const void* RowByIndex(uint32_t index) const
        {
            if (index >= RowCount()) return nullptr;
            const auto* rows = Field<const void* const*>(off::kOffRowByIndex);
            return rows ? rows[index] : nullptr;
        }

        /// The row with that id, or null. The client's lookup is a dense array, so this is O(1).
        const void* RowById(uint32_t id) const
        {
            if (!Loaded()) return nullptr;
            const uint32_t lo = MinId();
            const uint32_t hi = MaxId();
            if (id < lo || id > hi) return nullptr;
            const auto* byId = Field<const void* const*>(off::kOffRowById);
            return byId ? byId[id - lo] : nullptr;
        }

        /**
         * @brief Bytes per record as the client compiled it, measured rather than declared.
         *
         * The stride is a compile-time constant inside the generated loader and is stored nowhere,
         * but the rows are contiguous -- so two of them give it. Returns 0 when the table holds
         * fewer than two rows, which is the only case it cannot be measured.
         */
        uint32_t Stride() const
        {
            if (RowCount() < 2) return 0;
            const auto* rows = Field<const uint8_t* const*>(off::kOffRowByIndex);
            if (!rows || !rows[0] || !rows[1]) return 0;
            return uint32_t(rows[1] - rows[0]);
        }

        /// Dwords per record, 0 when the stride could not be measured.
        uint32_t Dwords() const { return Stride() / 4; }

        /**
         * @brief True when the measured stride agrees with the declared column count.
         *
         * A localized field collapses exactly sixteen columns into one pointer, so the difference
         * has to be a non-negative multiple of sixteen. It is a cheap guard against a table whose
         * loader MSVC folded with another's, and against this header drifting from the client.
         */
        bool StrideIsConsistent() const
        {
            const uint32_t dwords = Dwords();
            if (!dwords) return true;                 // unmeasurable, not wrong
            if (dwords > Columns()) return false;
            return (Columns() - dwords) % 16 == 0;
        }

        // --- fields, by dword index into the record --------------------------------------------
        // The index is the client's memory layout. Out of range answers zero rather than reading
        // past the row: a wrong index here would otherwise return a neighbouring record's field.

        uint32_t U32(const void* row, uint32_t dword) const
        {
            if (!row || !InRange(dword)) return 0;
            uint32_t value = 0;
            std::memcpy(&value, static_cast<const uint8_t*>(row) + dword * 4, 4);
            return value;
        }

        float F32(const void* row, uint32_t dword) const
        {
            const uint32_t raw = U32(row, dword);
            float value = 0.0f;
            std::memcpy(&value, &raw, 4);
            return value;
        }

        /// A localized string field. The client never stores a null here -- an absent string points
        /// at an empty one -- so this answers "" rather than null on any failure, matching it.
        const char* Str(const void* row, uint32_t dword) const
        {
            const uint32_t raw = U32(row, dword);
            return raw ? reinterpret_cast<const char*>(raw) : "";
        }

        /// The table object itself, for a caller that needs to go further than reading.
        void* Object() const { return m_info ? reinterpret_cast<void*>(m_info->instance) : nullptr; }

    private:
        explicit Table(const off::TableInfo* info) : m_info(info) {}

        template <class T>
        T Field(size_t offset) const
        {
            T value{};
            std::memcpy(&value, reinterpret_cast<const uint8_t*>(m_info->instance) + offset,
                        sizeof(T));
            return value;
        }

        /// A measured stride bounds the read; an unmeasurable one leaves it to the caller.
        bool InRange(uint32_t dword) const
        {
            const uint32_t dwords = Dwords();
            return dwords == 0 || dword < dwords;
        }

        const off::TableInfo* m_info = nullptr;
    };
}
