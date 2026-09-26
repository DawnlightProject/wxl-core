// wxl-graphics-extend: a bump allocator over one range of a per-frame buffer.
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

// Header-only and Windows-free, so a host test can exercise it. One ring per frame slot and per
// use (uniform data, staging): a slot is reused only once the GPU is done with it, so the ring is
// simply reset whole and bumped forward again -- no wrap, no per-allocation bookkeeping.
namespace wxl::gfx::vulkan
{
    class Ring
    {
    public:
        /// The range [base, base + capacity) of the buffer this ring hands out.
        void Init(uint64_t base, uint64_t capacity)
        {
            base_ = base;
            capacity_ = capacity;
            head_ = 0;
            peak_ = 0;
        }

        void Reset() { head_ = 0; }

        /**
         * @brief Takes size bytes at a multiple of alignment (any, not only a power of two); false
         *        when full. Never wraps: a request near the top of the range is refused, not granted.
         * @param offset  receives the offset within the buffer (base included).
         */
        bool Alloc(uint64_t size, uint64_t alignment, uint64_t& offset)
        {
            if (alignment == 0) return false;
            const uint64_t rem = head_ % alignment;
            const uint64_t pad = rem ? alignment - rem : 0;
            if (pad > capacity_ || head_ > capacity_ - pad) return false;   // head_ + pad past the end (or past 2^64)
            const uint64_t aligned = head_ + pad;
            if (size > capacity_ - aligned) return false;
            head_ = aligned + size;
            if (head_ > peak_) peak_ = head_;
            offset = base_ + aligned;
            return true;
        }

        /// The least common multiple of a and b (both > 0): a buffer-to-image copy needs its offset
        /// a multiple of the texel size as well as of the ring's own alignment.
        static uint64_t CommonAlignment(uint64_t a, uint64_t b)
        {
            uint64_t x = a, y = b;
            while (y) { const uint64_t t = x % y; x = y; y = t; }   // gcd
            return x ? a / x * b : 0;
        }

        uint64_t Used() const { return head_; }
        uint64_t Capacity() const { return capacity_; }
        /// The most this ring ever held in one slot use, since Init.
        uint64_t Peak() const { return peak_; }

    private:
        uint64_t base_ = 0;
        uint64_t capacity_ = 0;
        uint64_t head_ = 0;
        uint64_t peak_ = 0;
    };
}
