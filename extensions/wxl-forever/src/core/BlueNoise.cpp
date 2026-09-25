// wxl-forever: a tileable blue-noise texture for per-pixel sample offsets and dither.
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

#include "ExtensionApi.hpp"
#include "BlueNoise.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{
    namespace bn = wxl::forever::bluenoise;

    constexpr int   kN      = bn::kSize;
    constexpr int   kCount  = kN * kN;
    constexpr int   kRadius = 6;       // the Gaussian is cut at about three sigma
    constexpr float kSigma  = 1.9f;

    std::vector<uint32_t> g_texels;
    std::atomic<bool>     g_baked{ false };
    std::atomic<bool>     g_started{ false };
    bool                  g_failed = false;
    IDirect3DTexture9*    g_texture = nullptr;
    DWORD                 g_bakeMs = 0;

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

    void Bake()
    {
        const DWORD start = GetTickCount();
        std::vector<uint32_t> texels(kCount, 0);
        const uint32_t seeds[4] = { 0x9E3779B9u, 0x85EBCA6Bu, 0xC2B2AE35u, 0x27D4EB2Fu };
        // Byte 2 is red, 1 green, 0 blue, 3 alpha in A8R8G8B8.
        const int shifts[4] = { 16, 8, 0, 24 };
        for (int ch = 0; ch < 4; ++ch)
        {
            const std::vector<int> rank = Channel(seeds[ch]);
            for (int i = 0; i < kCount; ++i)
                texels[i] |= uint32_t(rank[i] * 256 / kCount) << shifts[ch];
        }
        g_texels = std::move(texels);
        g_bakeMs = GetTickCount() - start;
        g_baked = true;   // published last: the render thread reads the texels only after this
    }

    bool Upload(IDirect3DDevice9* dev)
    {
        if (FAILED(dev->CreateTexture(kN, kN, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_texture, nullptr)) || !g_texture)
        {
            g_texture = nullptr;
            return false;
        }
        D3DLOCKED_RECT rect{};
        if (FAILED(g_texture->LockRect(0, &rect, nullptr, 0)))
        {
            g_texture->Release();
            g_texture = nullptr;
            return false;
        }
        for (int y = 0; y < kN; ++y)
        {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(rect.pBits) + size_t(y) * rect.Pitch);
            for (int x = 0; x < kN; ++x) row[x] = g_texels[size_t(y) * kN + x];
        }
        g_texture->UnlockRect(0);
        WLOG_INFO("blue noise %dx%d, 4 channels, baked in %lu ms on a worker, uploaded", kN, kN, g_bakeMs);
        return true;
    }
}

namespace wxl::forever::bluenoise
{
    void StartBake()
    {
        if (g_started.exchange(true)) return;
        std::thread(&Bake).detach();
    }

    IDirect3DTexture9* Get(IDirect3DDevice9* dev)
    {
        if (g_texture || g_failed || !dev) return g_texture;
        if (!g_baked) { StartBake(); return nullptr; }
        if (!Upload(dev))
        {
            g_failed = true;
            WLOG_WARN("blue noise texture unavailable");
        }
        return g_texture;
    }

    void Release()
    {
        if (g_texture) { g_texture->Release(); g_texture = nullptr; }
        g_failed = false;
    }
}
