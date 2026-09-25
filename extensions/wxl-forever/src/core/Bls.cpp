// wxl-forever: the client's BLS shader files, read into their permutations.
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

#include "Bls.hpp"

#include <cstring>

namespace
{
    template <class T>
    bool Take(const std::string& bytes, size_t& at, T& value)
    {
        if (at + sizeof(T) > bytes.size()) return false;
        std::memcpy(&value, bytes.data() + at, sizeof(T));
        at += sizeof(T);
        return true;
    }
}

namespace wxl::forever::bls
{
    bool Read(const std::string& bytes, std::vector<Permutation>& out)
    {
        out.clear();
        size_t at = 0;
        uint32_t magic = 0, version = 0, count = 0;
        if (!Take(bytes, at, magic) || !Take(bytes, at, version) || !Take(bytes, at, count)) return false;
        if (magic != kMagic || version != kVersion || count > 4096) return false;

        for (uint32_t i = 0; i < count; ++i)
        {
            Permutation p{};
            uint32_t size = 0;
            if (!Take(bytes, at, p.inputs) || !Take(bytes, at, p.outputs) || !Take(bytes, at, p.samplers)
                || !Take(bytes, at, p.extra) || !Take(bytes, at, size) || at + size > bytes.size())
            {
                out.clear();
                return false;
            }
            p.code.assign(bytes.data() + at, size);
            at += (size_t(size) + 3) & ~size_t(3);
            out.push_back(std::move(p));
        }
        if (at != bytes.size())
        {
            out.clear();
            return false;
        }
        return true;
    }
}
