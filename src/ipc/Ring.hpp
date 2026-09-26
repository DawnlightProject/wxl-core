// Single-producer single-consumer ring over shared memory, in the style of rigtorp/SPSCQueue.
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

#include "ipc/Protocol.hpp"

#include <intrin.h>
#include <windows.h>

#include <atomic>
#include <cstdint>

namespace wxl::ipc
{
    /**
     * @brief One side's view of a ring whose indices and slots live in shared memory.
     *
     * The indices are free-running 32-bit counters (slot = index & (N - 1)), so the ring holds N items.
     * Each side caches the other side's index locally and only reloads it when the cache says full or
     * empty, which keeps the shared cache lines quiet. The cache is process-local state: a view must be
     * used by one producer and one consumer at a time (callers lock around a shared producer).
     */
    template <class T, uint32_t N>
    class RingView
    {
        static_assert((N & (N - 1)) == 0, "ring size must be a power of two");

    public:
        RingView() = default;
        RingView(RingIndex* write, RingIndex* read, T* slots) : m_write(write), m_read(read), m_slots(slots) {}

        // --- producer ---

        /// Returns the next free slot, or null when the ring is full. Commit it with Push.
        T* Reserve()
        {
            const uint32_t w = m_write->value.load(std::memory_order_relaxed);
            if (w - m_cachedRead >= N)
            {
                m_cachedRead = m_read->value.load(std::memory_order_acquire);
                if (w - m_cachedRead >= N) return nullptr;
            }
            return &m_slots[w & (N - 1)];
        }

        /// Publishes the slot Reserve returned.
        void Push()
        {
            const uint32_t w = m_write->value.load(std::memory_order_relaxed);
            m_write->value.store(w + 1, std::memory_order_release);
        }

        // --- consumer ---

        /// Returns the oldest item, or null when the ring is empty. Release it with Pop.
        T* Front()
        {
            const uint32_t r = m_read->value.load(std::memory_order_relaxed);
            if (r == m_cachedWrite)
            {
                m_cachedWrite = m_write->value.load(std::memory_order_acquire);
                if (r == m_cachedWrite) return nullptr;
            }
            return &m_slots[r & (N - 1)];
        }

        /// Frees the slot Front returned.
        void Pop()
        {
            const uint32_t r = m_read->value.load(std::memory_order_relaxed);
            m_read->value.store(r + 1, std::memory_order_release);
        }

        /// True when the consumer has nothing to read (reloads the producer index).
        bool Empty() const
        {
            return m_read->value.load(std::memory_order_acquire) == m_write->value.load(std::memory_order_acquire);
        }

        /// Resets both indices. Only valid while neither side runs (session setup).
        void Reset()
        {
            m_write->value.store(0, std::memory_order_relaxed);
            m_read->value.store(0, std::memory_order_relaxed);
            m_cachedRead = 0;
            m_cachedWrite = 0;
        }

        /// Re-reads the shared indices into the caches, for a new owner of this view.
        void Resync()
        {
            m_cachedRead = m_read->value.load(std::memory_order_acquire);
            m_cachedWrite = m_write->value.load(std::memory_order_acquire);
        }

    private:
        RingIndex* m_write = nullptr;
        RingIndex* m_read = nullptr;
        T*         m_slots = nullptr;
        uint32_t   m_cachedRead = 0;   // producer's copy of the consumer index
        uint32_t   m_cachedWrite = 0;  // consumer's copy of the producer index
    };

    using RequestRing  = RingView<Request, kRequestSlots>;
    using ResponseRing = RingView<Response, kResponseSlots>;

    /// Microseconds on the performance counter.
    inline uint64_t NowUs()
    {
        static const uint64_t freq = [] {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            return static_cast<uint64_t>(f.QuadPart);
        }();
        LARGE_INTEGER c{};
        QueryPerformanceCounter(&c);
        return static_cast<uint64_t>(c.QuadPart) * 1000000ull / freq;
    }

    /// One spin step: a pause, and a yield every so often so a spinner never starves a sibling thread.
    inline void SpinStep(uint32_t& round)
    {
        if ((++round & 63) == 0) SwitchToThread();
        else _mm_pause();
    }
}
