// wxl-graphics-extend: the shader cache's hash and the on-disk record of one compiled program.
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

#include "CacheRecord.hpp"

#include <cstring>
#include <new>

namespace
{
    constexpr size_t kMaxName = 0xFFFF;   // a u16 length
    constexpr size_t kChecksumBytes = 8;
    constexpr size_t kFixedBytes = 4 + 4 + 8 + 4 + 4 + kChecksumBytes;   // without includes and bytecode

    void Put16(std::string& out, uint16_t v)
    {
        const char b[2] = { char(v & 0xFF), char((v >> 8) & 0xFF) };
        out.append(b, 2);
    }

    void Put32(std::string& out, uint32_t v)
    {
        const char b[4] = { char(v & 0xFF), char((v >> 8) & 0xFF), char((v >> 16) & 0xFF), char((v >> 24) & 0xFF) };
        out.append(b, 4);
    }

    void Put64(std::string& out, uint64_t v)
    {
        Put32(out, uint32_t(v & 0xFFFFFFFFu));
        Put32(out, uint32_t(v >> 32));
    }

    /// Bounds-checked little-endian reads; every fetch fails cleanly past the end.
    class Reader
    {
    public:
        Reader(const void* bytes, size_t size) : p_(static_cast<const uint8_t*>(bytes)), size_(size) {}

        size_t Remaining() const { return size_ - at_; }
        const uint8_t* Current() const { return p_ + at_; }

        bool U16(uint16_t& v)
        {
            if (Remaining() < 2) return false;
            v = uint16_t(p_[at_] | (p_[at_ + 1] << 8));
            at_ += 2;
            return true;
        }

        bool U32(uint32_t& v)
        {
            if (Remaining() < 4) return false;
            v = uint32_t(p_[at_]) | (uint32_t(p_[at_ + 1]) << 8) | (uint32_t(p_[at_ + 2]) << 16) | (uint32_t(p_[at_ + 3]) << 24);
            at_ += 4;
            return true;
        }

        bool U64(uint64_t& v)
        {
            uint32_t lo = 0, hi = 0;
            if (!U32(lo) || !U32(hi)) return false;
            v = uint64_t(lo) | (uint64_t(hi) << 32);
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
}

namespace wxl::gfx::shaders::cache
{
    void Hasher::Add(const void* data, size_t size)
    {
        const auto* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < size; ++i)
        {
            h_ ^= p[i];
            h_ *= kFnvPrime;
        }
    }

    void Hasher::Add(uint32_t value)
    {
        const uint8_t b[4] = { uint8_t(value & 0xFF), uint8_t((value >> 8) & 0xFF), uint8_t((value >> 16) & 0xFF), uint8_t((value >> 24) & 0xFF) };
        Add(b, 4);
    }

    void Hasher::AddString(const char* text, size_t size)
    {
        if (text) Add(text, size);
        const uint8_t zero = 0;
        Add(&zero, 1);
    }

    void Hasher::AddString(const char* text)
    {
        AddString(text, text ? std::strlen(text) : 0);
    }

    uint64_t Fnv1a64(const void* data, size_t size)
    {
        Hasher h;
        h.Add(data, size);
        return h.Value();
    }

    bool Encode(const Record& record, std::string& out)
    {
        if (record.includes.size() > kMaxIncludes) return false;
        if (record.bytecode.empty() || record.bytecode.size() > kMaxBytecode || (record.bytecode.size() & 3)) return false;
        uint64_t total = kFixedBytes + record.bytecode.size();
        for (const Include& inc : record.includes)
        {
            if (inc.name.size() > kMaxName) return false;
            total += 2 + inc.name.size() + 8;
        }
        if (total > 0xFFFFFFFFull) return false;

        std::string file;
        try
        {
            file.reserve(size_t(total));
            Put32(file, kMagic);
            Put32(file, kVersion);
            Put64(file, record.key);
            Put32(file, uint32_t(record.includes.size()));
            for (const Include& inc : record.includes)
            {
                Put16(file, uint16_t(inc.name.size()));
                file.append(inc.name);
                Put64(file, inc.hash);
            }
            Put32(file, uint32_t(record.bytecode.size()));
            file.append(record.bytecode);
            Put64(file, Fnv1a64(file.data(), file.size()));
            out.swap(file);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        return true;
    }

    bool Decode(const void* bytes, size_t size, Record& out)
    {
        if (!bytes || size < kFixedBytes) return false;

        // The checksum first: it turns a torn or foreign file into a plain miss before any field is
        // believed. Then every count and length against what is left.
        const auto* p = static_cast<const uint8_t*>(bytes);
        Reader tail(p + (size - kChecksumBytes), kChecksumBytes);
        uint64_t stored = 0;
        if (!tail.U64(stored) || stored != Fnv1a64(p, size - kChecksumBytes)) return false;

        Reader r(p, size - kChecksumBytes);
        uint32_t magic = 0, version = 0, includeCount = 0, bytecodeSize = 0;
        Record rec;
        if (!r.U32(magic) || !r.U32(version) || !r.U64(rec.key) || !r.U32(includeCount)) return false;
        if (magic != kMagic || version != kVersion || includeCount > kMaxIncludes) return false;
        try
        {
            rec.includes.reserve(includeCount);
            for (uint32_t i = 0; i < includeCount; ++i)
            {
                uint16_t nameSize = 0;
                if (!r.U16(nameSize) || r.Remaining() < nameSize) return false;
                Include inc;
                inc.name.assign(reinterpret_cast<const char*>(r.Current()), nameSize);
                r.Skip(nameSize);
                if (!r.U64(inc.hash)) return false;
                rec.includes.push_back(std::move(inc));
            }
            if (!r.U32(bytecodeSize)) return false;
            if (!bytecodeSize || bytecodeSize > kMaxBytecode || (bytecodeSize & 3) || r.Remaining() != bytecodeSize) return false;
            rec.bytecode.assign(reinterpret_cast<const char*>(r.Current()), bytecodeSize);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        out = std::move(rec);
        return true;
    }

    std::string FileName(uint64_t key)
    {
        static const char kHex[] = "0123456789abcdef";
        std::string name(16, '0');
        for (int i = 15; i >= 0; --i, key >>= 4) name[size_t(i)] = kHex[key & 0xF];
        return name + ".wgc";
    }
}
