// wxl-graphics-extend: the client's BLS shader container, read and written.
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
#include <new>
#include <vector>

namespace
{
    constexpr uint32_t kMaxPermutations = 4096;
    constexpr size_t   kHeaderBytes = 12;              // magic, version, count
    constexpr size_t   kPermutationHeaderBytes = 16;   // inputs, outputs, samplers, extra, code size

    /// Bounds-checked little-endian reads over the caller's bytes; nothing is copied.
    class Reader
    {
    public:
        Reader(const void* bytes, size_t size) : p_(static_cast<const uint8_t*>(bytes)), size_(bytes ? size : 0) {}

        size_t Remaining() const { return size_ - at_; }
        const uint8_t* Current() const { return p_ + at_; }

        bool U16(uint16_t& v)
        {
            if (Remaining() < 2) return false;
            std::memcpy(&v, p_ + at_, 2);
            at_ += 2;
            return true;
        }

        bool U32(uint32_t& v)
        {
            if (Remaining() < 4) return false;
            std::memcpy(&v, p_ + at_, 4);
            at_ += 4;
            return true;
        }

        bool Skip(size_t n)
        {
            if (Remaining() < n) return false;
            at_ += n;
            return true;
        }

    private:
        const uint8_t* p_;
        size_t size_;
        size_t at_ = 0;
    };

    /// The zero bytes after a permutation's code, so the next one starts on a four-byte boundary.
    size_t Padding(uint32_t codeSize)
    {
        return (4 - codeSize % 4) % 4;
    }

    /// Walks the whole container, every size checked against the bytes, and fills *out with the
    /// permutation at wanted when it is there. Returns the count, -1 when the bytes are not one
    /// whole BLS file. The walk never stops early: a truncated file is invalid, whichever
    /// permutation was asked for.
    int Walk(const void* bytes, size_t size, uint32_t wanted, WXL_GfxBlsPermutation* out)
    {
        Reader r(bytes, size);
        uint32_t magic = 0, version = 0, count = 0;
        if (!r.U32(magic) || !r.U32(version) || !r.U32(count)) return -1;
        if (magic != wxl::gfx::bls::kMagic || version != wxl::gfx::bls::kVersion || count > kMaxPermutations) return -1;

        WXL_GfxBlsPermutation found{};
        bool have = false;
        for (uint32_t i = 0; i < count; ++i)
        {
            WXL_GfxBlsPermutation p{};
            if (!r.U32(p.inputs) || !r.U32(p.outputs) || !r.U16(p.samplers) || !r.U16(p.extra) || !r.U32(p.codeSize)) return -1;
            if (r.Remaining() < p.codeSize) return -1;
            p.code = r.Current();
            if (!r.Skip(p.codeSize) || !r.Skip(Padding(p.codeSize))) return -1;
            if (i == wanted)
            {
                found = p;
                have = true;
            }
        }
        if (r.Remaining() != 0) return -1;
        if (have && out) *out = found;
        return int(count);
    }

    void Put16(std::vector<uint8_t>& out, uint16_t v)
    {
        out.push_back(uint8_t(v & 0xFF));
        out.push_back(uint8_t(v >> 8));
    }

    void Put32(std::vector<uint8_t>& out, uint32_t v)
    {
        out.push_back(uint8_t(v & 0xFF));
        out.push_back(uint8_t((v >> 8) & 0xFF));
        out.push_back(uint8_t((v >> 16) & 0xFF));
        out.push_back(uint8_t((v >> 24) & 0xFF));
    }
}

namespace wxl::gfx::bls
{
    int Count(const void* bytes, size_t size)
    {
        return Walk(bytes, size, kMaxPermutations, nullptr);
    }

    int Permutation(const void* bytes, size_t size, uint32_t index, WXL_GfxBlsPermutation* out)
    {
        if (!out || index >= kMaxPermutations) return 0;
        const int count = Walk(bytes, size, index, out);
        return count > 0 && index < uint32_t(count) ? 1 : 0;
    }

    int Write(const WXL_GfxBlsPermutation* permutations, uint32_t count, WXL_ByteSink* out)
    {
        if (!out || !out->Write || count > kMaxPermutations || (count && !permutations)) return 0;

        // Every input checked before a byte is produced: a token stream is whole DWORDs, so a size
        // that is not a multiple of four is a caller's mistake, not something to pad over.
        uint64_t total = kHeaderBytes;
        for (uint32_t i = 0; i < count; ++i)
        {
            const WXL_GfxBlsPermutation& p = permutations[i];
            if ((p.codeSize && !p.code) || (p.codeSize % 4)) return 0;
            total += kPermutationHeaderBytes + p.codeSize + Padding(p.codeSize);
        }
        if (total > 0xFFFFFFFFull) return 0;   // one sink write takes an unsigned int

        std::vector<uint8_t> file;
        try
        {
            file.reserve(size_t(total));
            Put32(file, kMagic);
            Put32(file, kVersion);
            Put32(file, count);
            for (uint32_t i = 0; i < count; ++i)
            {
                const WXL_GfxBlsPermutation& p = permutations[i];
                Put32(file, p.inputs);
                Put32(file, p.outputs);
                Put16(file, p.samplers);
                Put16(file, p.extra);
                Put32(file, p.codeSize);
                const auto* code = static_cast<const uint8_t*>(p.code);
                file.insert(file.end(), code, code + p.codeSize);
                file.insert(file.end(), Padding(p.codeSize), uint8_t(0));
            }
        }
        catch (const std::bad_alloc&)
        {
            return 0;
        }
        out->Write(out->ctx, file.data(), static_cast<unsigned int>(file.size()));
        return 1;
    }
}
