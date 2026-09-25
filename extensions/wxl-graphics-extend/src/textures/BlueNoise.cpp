// wxl-graphics-extend: the blue-noise bake (void-and-cluster, four channels) on a worker thread.
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

#include "BlueNoise.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <new>
#include <thread>
#include <vector>

namespace
{
    namespace bn = wxl::gfx::textures::bluenoise;

    constexpr int   kN      = bn::kSize;
    constexpr int   kCount  = kN * kN;
    constexpr int   kRadius = 6;       // the Gaussian is cut at about three sigma
    constexpr float kSigma  = 1.9f;

    // The finished buffer is published through this pointer (release) and read through it (acquire):
    // the texels and g_bakeMs, written before the store, are visible to any reader that saw it. The
    // buffer is heap memory that is never freed: the worker is detached, and a static destructor
    // running under it at process exit would be a race for nothing.
    std::atomic<const uint32_t*> g_texels{ nullptr };
    std::atomic<bool>            g_started{ false };
    uint32_t                     g_bakeMs = 0;

    struct Rng
    {
        uint32_t s;
        uint32_t Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        float Unit() { return float(Next() >> 8) / 16777216.0f; }
    };

    /// One channel: every cell's rank in the void-and-cluster order, 0..kCount-1.
    std::vector<int> Channel(uint32_t seed)
    {
        constexpr int kSide = 2 * kRadius + 1;
        float kernel[kSide * kSide];
        for (int dy = -kRadius; dy <= kRadius; ++dy)
            for (int dx = -kRadius; dx <= kRadius; ++dx)
                kernel[(dy + kRadius) * kSide + dx + kRadius] = std::exp(-float(dx * dx + dy * dy) / (2.0f * kSigma * kSigma));

        std::vector<float>   energy(kCount);
        std::vector<uint8_t> on(kCount, 0);
        auto splat = [&](int index, float sign) {
            const int x = index % kN, y = index / kN;
            for (int dy = -kRadius; dy <= kRadius; ++dy)
            {
                const int row = ((y + dy) & (kN - 1)) * kN;
                for (int dx = -kRadius; dx <= kRadius; ++dx)
                    energy[row + ((x + dx) & (kN - 1))] += sign * kernel[(dy + kRadius) * kSide + dx + kRadius];
            }
        };
        auto tightest = [&]() {
            int best = 0;
            float e = -1e30f;
            for (int i = 0; i < kCount; ++i)
                if (on[i] && energy[i] > e) { e = energy[i]; best = i; }
            return best;
        };
        auto largestVoid = [&]() {
            int best = 0;
            float e = 1e30f;
            for (int i = 0; i < kCount; ++i)
                if (!on[i] && energy[i] < e) { e = energy[i]; best = i; }
            return best;
        };

        // A tiny random bias breaks the ties of a regular grid.
        Rng rng{ seed };
        for (float& e : energy) e = rng.Unit() * 1e-4f;

        int ones = 0;
        while (ones < kCount / 10)
        {
            const int i = int(rng.Next() % uint32_t(kCount));
            if (on[i]) continue;
            on[i] = 1;
            splat(i, 1.0f);
            ++ones;
        }

        // Relax the initial pattern: move the tightest cluster to the largest void until stable.
        for (int guard = 0; guard < kCount; ++guard)
        {
            const int c = tightest();
            on[c] = 0;
            splat(c, -1.0f);
            const int v = largestVoid();
            on[v] = 1;
            splat(v, 1.0f);
            if (v == c) break;
        }

        std::vector<int> rank(kCount, 0);
        const std::vector<uint8_t> startOn = on;
        const std::vector<float>   startEnergy = energy;

        // Ranks below the initial count: take out the tightest cluster, highest rank first.
        for (int r = ones - 1; r >= 0; --r)
        {
            const int c = tightest();
            on[c] = 0;
            splat(c, -1.0f);
            rank[c] = r;
        }

        // Ranks above it: fill the largest void, lowest rank first.
        on = startOn;
        energy = startEnergy;
        for (int r = ones; r < kCount; ++r)
        {
            const int v = largestVoid();
            on[v] = 1;
            splat(v, 1.0f);
            rank[v] = r;
        }
        return rank;
    }

    /// The worker. An allocation failure (the only thing in here that can throw) leaves the texels
    /// unpublished for good: the texture then simply never becomes available, which every consumer
    /// already handles; an exception on a detached thread would end the process instead.
    void Bake()
    {
        try
        {
            const auto start = std::chrono::steady_clock::now();
            uint32_t* texels = new (std::nothrow) uint32_t[kCount]();
            if (!texels) return;
            const uint32_t seeds[4] = { 0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu };
            // Byte 2 is red, 1 green, 0 blue, 3 alpha in A8R8G8B8.
            const int shifts[4] = { 16, 8, 0, 24 };
            for (int ch = 0; ch < 4; ++ch)
            {
                const std::vector<int> rank = Channel(seeds[ch]);
                for (int i = 0; i < kCount; ++i)
                    texels[i] |= uint32_t(rank[i] * 256 / kCount) << shifts[ch];
            }
            g_bakeMs = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
            g_texels.store(texels, std::memory_order_release);   // published last
        }
        catch (...)
        {
        }
    }
}

namespace wxl::gfx::textures::bluenoise
{
    void Start()
    {
        if (g_started.exchange(true)) return;
        // Detached: the process ends with the client, and nothing may wait on the render thread.
        try
        {
            std::thread(&Bake).detach();
        }
        catch (...)
        {
            g_started.store(false);   // no thread could be made: the next request tries again
        }
    }

    const uint32_t* Texels()
    {
        return g_texels.load(std::memory_order_acquire);
    }

    bool Ready(uint32_t& bakeMs)
    {
        if (!Texels()) return false;
        bakeMs = g_bakeMs;
        return true;
    }
}
