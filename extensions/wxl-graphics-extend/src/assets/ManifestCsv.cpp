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

#include "ManifestCsv.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace
{
    /// ASCII only: keys are file stems and "model|light" pairs, and the C locale must not matter.
    constexpr char LowerChar(char c) noexcept
    {
        return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
    }

    std::string Lower(const std::string& s)
    {
        std::string out(s);
        for (char& c : out) c = LowerChar(c);
        return out;
    }

    /// strcmp of a lower-cased key against any-cased text, lowering the text on the fly.
    int CompareLowered(const char* lowered, const char* text) noexcept
    {
        for (;; ++lowered, ++text)
        {
            const unsigned char a = static_cast<unsigned char>(*lowered);
            const unsigned char b = static_cast<unsigned char>(LowerChar(*text));
            if (a != b) return a < b ? -1 : 1;
            if (!a) return 0;
        }
    }

    /// A line ends at LF, at CR LF, or at a CR that ends the text; a lone CR inside a line is text.
    bool AtLineEnd(const char* text, size_t size, size_t at) noexcept
    {
        if (at >= size || text[at] == '\n') return true;
        return text[at] == '\r' && (at + 1 >= size || text[at + 1] == '\n');
    }

    void SkipLineEnd(const char* text, size_t size, size_t& at) noexcept
    {
        if (at < size && text[at] == '\r') ++at;
        if (at < size && text[at] == '\n') ++at;
    }

    void SkipLine(const char* text, size_t size, size_t& at) noexcept
    {
        while (!AtLineEnd(text, size, at)) ++at;
        SkipLineEnd(text, size, at);
    }

    /// One field starting at `at`: quoted (RFC 4180: "" is one quote, separators inside are literal,
    /// an unterminated quote runs to the end of the text) or bare up to the next comma or line end.
    /// Text between a closing quote and the separator is kept as it is: a lenient reading of a
    /// malformed field beats dropping the row.
    void ReadField(const char* text, size_t size, size_t& at, std::string& field)
    {
        field.clear();
        if (at < size && text[at] == '"')
        {
            ++at;
            while (at < size)
            {
                const char c = text[at++];
                if (c != '"')
                {
                    field += c;
                    continue;
                }
                if (at < size && text[at] == '"')
                {
                    field += '"';
                    ++at;
                    continue;
                }
                break;   // the closing quote
            }
        }
        while (!AtLineEnd(text, size, at) && text[at] != ',') field += text[at++];
    }

    /// One record's fields, consuming its line end. An empty line yields one empty field.
    void ReadRecord(const char* text, size_t size, size_t& at, std::vector<std::string>& fields)
    {
        fields.clear();
        std::string field;
        for (;;)
        {
            ReadField(text, size, at, field);
            fields.push_back(field);
            if (at < size && text[at] == ',')
            {
                ++at;
                continue;
            }
            break;
        }
        SkipLineEnd(text, size, at);
    }

    /// "format N" anywhere in a comment line; 0 when absent or not a number.
    int FormatVersion(const char* text, size_t size, size_t at) noexcept
    {
        constexpr const char kWord[] = "format ";
        constexpr size_t kWordLen = sizeof kWord - 1;
        for (; at + kWordLen <= size && !AtLineEnd(text, size, at); ++at)
        {
            if (std::memcmp(text + at, kWord, kWordLen) != 0) continue;
            int v = 0;
            size_t p = at + kWordLen;
            bool digits = false;
            for (; p < size && text[p] >= '0' && text[p] <= '9' && v < 100000000; ++p, digits = true)
                v = v * 10 + (text[p] - '0');
            return digits ? v : 0;
        }
        return 0;
    }
}

namespace wxl::gfx::assets::csv
{
    void Parse(const char* text, size_t size, Table& out)
    {
        out.version = 0;
        out.columns.clear();
        out.rows.clear();
        out.index.clear();
        if (!text) size = 0;

        size_t at = 0;
        if (size >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB
            && static_cast<unsigned char>(text[2]) == 0xBF)
            at = 3;   // a UTF-8 byte order mark would otherwise become part of the first name

        bool header = false;
        bool versioned = false;
        std::vector<std::string> fields;
        while (at < size)
        {
            if (AtLineEnd(text, size, at))
            {
                SkipLineEnd(text, size, at);
                continue;
            }
            if (text[at] == '#')
            {
                if (!versioned)
                {
                    out.version = FormatVersion(text, size, at + 1);
                    versioned = out.version != 0;
                }
                SkipLine(text, size, at);
                continue;
            }
            ReadRecord(text, size, at, fields);
            if (!header)
            {
                out.columns = fields;
                header = true;
                continue;
            }
            if (fields.empty() || fields[0].empty()) continue;
            out.index.emplace_back(Lower(fields[0]), int(out.rows.size()));
            out.rows.push_back(std::move(fields));
        }
        // Sorted by (key, row): among rows sharing a key, lower_bound lands on the first one.
        std::sort(out.index.begin(), out.index.end());
    }

    int Table::Column(const char* name) const noexcept
    {
        if (!name) return -1;
        for (size_t i = 0; i < columns.size(); ++i)
            if (columns[i] == name) return int(i);
        return -1;
    }

    int Table::Find(const char* key) const noexcept
    {
        if (!key) return -1;
        const auto it = std::lower_bound(index.begin(), index.end(), key,
                                         [](const std::pair<std::string, int>& e, const char* k) noexcept {
                                             return CompareLowered(e.first.c_str(), k) < 0;
                                         });
        return it != index.end() && CompareLowered(it->first.c_str(), key) == 0 ? it->second : -1;
    }

    const char* Table::Field(int row, int column) const noexcept
    {
        if (row < 0 || size_t(row) >= rows.size()) return "";
        const std::vector<std::string>& r = rows[size_t(row)];
        if (column < 0 || size_t(column) >= r.size()) return "";
        return r[size_t(column)].c_str();
    }

    float Table::Number(int row, int column, float fallback) const noexcept
    {
        const char* f = Field(row, column);
        char* end = nullptr;
        const float v = std::strtof(f, &end);
        return end == f ? fallback : v;
    }
}
