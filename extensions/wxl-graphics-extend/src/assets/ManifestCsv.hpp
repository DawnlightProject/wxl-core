// wxl-graphics-extend: the manifest CSV (5.tools/forever-bake) parsed into rows addressable by key and
// column name. No Windows header: unit-tested natively.
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
#include <string>
#include <utility>
#include <vector>

// The format: lines ending in LF or CRLF; a line starting with '#' is a comment, and the first one
// saying "format N" gives the version ("# forever-bake cookies manifest, format 2. ..."); the first
// other line names the columns; every line after it is a row, its first field the key. Fields are
// split on commas; a field starting with a double quote runs to the closing quote (a doubled quote
// inside it is one quote, a comma or line break inside it is literal). A row without a key is
// dropped, and when two rows share a key (compared without case) the first one wins. A row may hold
// fewer or more fields than there are columns: Field reads "" past its end.
namespace wxl::gfx::assets::csv
{
    struct Table
    {
        int version = 0;
        std::vector<std::string> columns;
        std::vector<std::vector<std::string>> rows;
        std::vector<std::pair<std::string, int>> index;   // (lower-cased key, row), sorted

        /// Column index by exact name; -1 when absent (or name is null).
        int Column(const char* name) const noexcept;

        /// Row index by key, compared without case (ASCII); -1 when absent. Allocates nothing.
        int Find(const char* key) const noexcept;

        /// The field's text, "" when the row or column is out of range.
        const char* Field(int row, int column) const noexcept;

        /// The field as a number; fallback when out of range or when it does not start with one.
        float Number(int row, int column, float fallback) const noexcept;
    };

    /// Parses text (size bytes, not NUL-terminated) into out, replacing its content.
    void Parse(const char* text, size_t size, Table& out);
}
