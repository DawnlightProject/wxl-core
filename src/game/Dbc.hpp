// Reading a client DBC the client itself does not know about: the WDBC container, opened through
// the game's own file layer so it resolves out of an MPQ or a loose override like anything else.
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
#include <vector>

#include "game/Io.hpp"

/**
 * @brief One WDBC table, read whole into memory.
 *
 * The stock loader only ever sees the tables compiled into the client, so a table invented for this
 * project -- SpellTelegraph, SpellEmpower -- is invisible to it and has to be read here. WDBC is the
 * simple half of the family: a fixed header, fixed-width rows of 4-byte fields, and one string block
 * behind them. There is no id index and no sparse block, so a lookup by id is a scan of the column
 * the caller says holds it.
 *
 * Every field is four bytes. What those bytes mean is the caller's business: U32, F32 and Str are
 * three readings of the same word, and asking for the wrong one is not an error, it is a wrong
 * answer. The layout has to match the .lua that wrote the table.
 */
namespace wxl::game::dbc
{
    class Table
    {
    public:
        /// Opens "DBFilesClient\Name.dbc". A missing or malformed file leaves the table empty.
        bool Open(const char* path)
        {
            m_rows.clear();
            m_strings.clear();
            m_rowCount = m_fieldCount = m_rowSize = 0;

            void* handle = nullptr;
            if (!io::FileOpen(path, io::kOpenWholeFile, &handle) || !handle)
                return false;

            const uint32_t size = io::FileSize(handle, nullptr);

            std::vector<uint8_t> bytes(size);
            uint32_t read = 0;
            const bool ok = size >= kHeaderSize
                         && io::FileRead(handle, bytes.data(), size, &read) != 0
                         && read == size;
            io::FileClose(handle);

            return ok && Parse(bytes);
        }

        bool     Loaded() const     { return m_rowCount != 0; }
        uint32_t RowCount() const   { return m_rowCount; }
        uint32_t FieldCount() const { return m_fieldCount; }
        uint32_t RowSize() const    { return m_rowSize; }

        /// True when field N really is the N-th dword of a row. False for a table that packs
        /// columns below a dword, where U32 and F32 answer 0 and Row() is the only way in.
        bool DwordAddressable() const { return m_rowSize >= uint64_t(m_fieldCount) * 4; }

        /// The raw bytes of a row, RowSize() long, or null. The way into a packed table.
        const uint8_t* Row(uint32_t row) const
        {
            return row < m_rowCount ? &m_rows[size_t(row) * m_rowSize] : nullptr;
        }

        /// The raw word at (row, field), or 0 when either is out of range.
        uint32_t U32(uint32_t row, uint32_t field) const
        {
            if (row >= m_rowCount || field >= m_fieldCount || !DwordAddressable()) return 0;
            uint32_t value = 0;
            std::memcpy(&value, &m_rows[row * m_rowSize + field * 4], 4);
            return value;
        }

        /// The same word read as a float.
        float F32(uint32_t row, uint32_t field) const
        {
            const uint32_t raw = U32(row, field);
            float value = 0.0f;
            std::memcpy(&value, &raw, 4);
            return value;
        }

        /// The same word read as an offset into the string block. Never null; "" when out of range.
        const char* Str(uint32_t row, uint32_t field) const
        {
            const uint32_t offset = U32(row, field);
            if (offset >= m_strings.size()) return "";
            return m_strings.data() + offset;
        }

        /// The first row whose @p field holds @p value, or -1. A scan: WDBC carries no index.
        int Find(uint32_t field, uint32_t value) const
        {
            for (uint32_t row = 0; row < m_rowCount; ++row)
                if (U32(row, field) == value)
                    return int(row);
            return -1;
        }

    private:
        static constexpr uint32_t kHeaderSize = 20;

        bool Parse(const std::vector<uint8_t>& bytes)
        {
            if (std::memcmp(bytes.data(), "WDBC", 4) != 0) return false;

            uint32_t header[4] = { 0, 0, 0, 0 };
            std::memcpy(header, bytes.data() + 4, sizeof header);

            const uint32_t rowCount   = header[0];
            const uint32_t fieldCount = header[1];
            const uint32_t rowSize    = header[2];
            const uint32_t stringSize = header[3];

            // A row wider than its fields is padded and still addressable by dword; a narrower one
            // packs columns below a dword and is not. Five stock tables do the latter --
            // CharBaseInfo, CharStartOutfit, PowerDisplay, SpellChainEffects and
            // SpellItemEnchantmentCondition -- so the file opens either way and U32 refuses instead.
            if (fieldCount == 0 || rowSize == 0) return false;

            const uint64_t need = uint64_t(kHeaderSize) + uint64_t(rowCount) * rowSize + stringSize;
            if (need > bytes.size()) return false;

            const uint8_t* rows = bytes.data() + kHeaderSize;
            m_rows.assign(rows, rows + size_t(rowCount) * rowSize);

            const uint8_t* strings = rows + size_t(rowCount) * rowSize;
            m_strings.assign(reinterpret_cast<const char*>(strings),
                             reinterpret_cast<const char*>(strings) + stringSize);

            m_rowCount   = rowCount;
            m_fieldCount = fieldCount;
            m_rowSize    = rowSize;
            return true;
        }

        std::vector<uint8_t> m_rows;
        std::vector<char>    m_strings;
        uint32_t             m_rowCount   = 0;
        uint32_t             m_fieldCount = 0;
        uint32_t             m_rowSize    = 0;
    };
}
