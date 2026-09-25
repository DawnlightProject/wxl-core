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

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Pure and Windows-free, so the format can be unit-tested on any host. A record file holds, little-
// endian: magic, format version, the key its name carries, every include the compile opened (its
// normalised name and the hash of its text, so a hit is checked against today's sources before it
// is used), the bytecode, and a checksum over everything before it. Any inconsistency is a miss.
namespace wxl::gfx::shaders::cache
{
    constexpr uint64_t kFnvBasis = 0xCBF29CE484222325ull;
    constexpr uint64_t kFnvPrime = 0x00000100000001B3ull;

    /// 64-bit FNV-1a, streamed: a program's key is built from its ingredients one by one.
    class Hasher
    {
    public:
        void Add(const void* data, size_t size);
        /// Little-endian, so a key means the same on every build.
        void Add(uint32_t value);
        /// The bytes and a terminating zero, so ("ab", "c") and ("a", "bc") hash differently.
        void AddString(const char* text, size_t size);
        void AddString(const char* text);
        uint64_t Value() const { return h_; }

    private:
        uint64_t h_ = kFnvBasis;
    };

    uint64_t Fnv1a64(const void* data, size_t size);

    struct Include
    {
        std::string name;   // normalised: forward slashes, as the resolver takes it
        uint64_t    hash;   // Fnv1a64 of its text
    };

    struct Record
    {
        uint64_t             key = 0;
        std::vector<Include> includes;
        std::string          bytecode;
    };

    constexpr uint32_t kMagic       = 0x31434757;   // "WGC1"
    constexpr uint32_t kVersion     = 1;
    constexpr size_t   kMaxIncludes = 4096;
    constexpr size_t   kMaxBytecode = 16u << 20;

    /// Serialises a record; false when a field does not fit the format (a name over 65535 bytes,
    /// more includes or bytecode than the caps above, bytecode that is not whole tokens).
    bool Encode(const Record& record, std::string& out);

    /// Parses one whole record file; false (out untouched) on anything short, foreign or corrupt.
    bool Decode(const void* bytes, size_t size, Record& out);

    /// The cache file's name for a key: sixteen lower-case hex digits and ".wgc".
    std::string FileName(uint64_t key);
}
