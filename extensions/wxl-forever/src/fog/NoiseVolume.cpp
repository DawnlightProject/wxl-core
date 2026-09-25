// wxl-forever fog: a tileable 3D noise volume baked once on the CPU, sampled by the fog's density.
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

#include "../core/ExtensionApi.hpp"
#include "NoiseVolume.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{
    namespace nz = wxl::forever::fog::noise;

    IDirect3DVolumeTexture9* g_volume = nullptr;
    bool                     g_failed = false;
    std::vector<std::vector<uint32_t>> g_levels;   // baked mip chain, uploaded on the render thread
    std::atomic<bool>        g_baked{ false };
    std::atomic<bool>        g_started{ false };
    unsigned long            g_bakeMs = 0;

    inline int Wrap(int v, int period) { v %= period; return v < 0 ? v + period : v; }

    uint32_t Hash(int x, int y, int z, uint32_t seed)
    {
        uint32_t h = uint32_t(x) * 73856093u ^ uint32_t(y) * 19349663u ^ uint32_t(z) * 83492791u ^ seed * 2654435761u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        return h;
    }

    float Unit(uint32_t h) { return float(h & 0xFFFFFF) / float(0xFFFFFF); }

    float Fade(float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

    /// Gradient noise on a lattice that repeats every period cells; about -1..1.
    float Perlin(float x, float y, float z, int period, uint32_t seed)
    {
        static const float kGrad[12][3] = {
            { 1, 1, 0 }, { -1, 1, 0 }, { 1, -1, 0 }, { -1, -1, 0 }, { 1, 0, 1 }, { -1, 0, 1 },
            { 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, 1 }, { 0, -1, 1 }, { 0, 1, -1 }, { 0, -1, -1 },
        };
        const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
        const float fx = x - float(ix), fy = y - float(iy), fz = z - float(iz);
        float corner[8];
        for (int c = 0; c < 8; ++c)
        {
            const int dx = c & 1, dy = (c >> 1) & 1, dz = (c >> 2) & 1;
            const float* g = kGrad[Hash(Wrap(ix + dx, period), Wrap(iy + dy, period), Wrap(iz + dz, period), seed) % 12];
            corner[c] = g[0] * (fx - float(dx)) + g[1] * (fy - float(dy)) + g[2] * (fz - float(dz));
        }
        const float u = Fade(fx), v = Fade(fy), w = Fade(fz);
        auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
        const float x00 = lerp(corner[0], corner[1], u), x10 = lerp(corner[2], corner[3], u);
        const float x01 = lerp(corner[4], corner[5], u), x11 = lerp(corner[6], corner[7], u);
        return lerp(lerp(x00, x10, v), lerp(x01, x11, v), w);
    }

    /// Distance to the nearest feature point, one point per cell, cells repeating every period; 0..~1.
    float Worley(float x, float y, float z, int period, uint32_t seed)
    {
        const int ix = int(std::floor(x)), iy = int(std::floor(y)), iz = int(std::floor(z));
        float best = 9.0f;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int cx = ix + dx, cy = iy + dy, cz = iz + dz;
                    const uint32_t h = Hash(Wrap(cx, period), Wrap(cy, period), Wrap(cz, period), seed);
                    const float px = float(cx) + Unit(h);
                    const float py = float(cy) + Unit(h * 747796405u + 1u);
                    const float pz = float(cz) + Unit(h * 2891336453u + 7u);
                    const float d = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
                    best = std::min(best, d);
                }
        return std::sqrt(best);
    }

    void Normalise(std::vector<float>& v)
    {
        const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
        const float a = *lo, range = std::max(*hi - *lo, 1e-6f);
        for (float& x : v) x = (x - a) / range;
    }

    void Bake()
    {
        const int n = nz::kSize;
        const size_t count = size_t(n) * n * n;
        std::vector<float> pw(count), wor(count), bil(count), per(count);

        const DWORD start = GetTickCount();
        for (int z = 0; z < n; ++z)
            for (int y = 0; y < n; ++y)
                for (int x = 0; x < n; ++x)
                {
                    const float u = float(x) / n, v = float(y) / n, w = float(z) / n;
                    float perlin = 0.0f, billow = 0.0f, amp = 1.0f, norm = 0.0f;
                    for (int o = 0; o < 4; ++o)
                    {
                        const int f = 4 << o;
                        const float p = Perlin(u * f, v * f, w * f, f, 11u + o);
                        perlin += p * amp;
                        billow += (1.0f - std::fabs(p)) * amp;
                        norm   += amp;
                        amp    *= 0.5f;
                    }
                    perlin /= norm;
                    billow /= norm;

                    float worley = 0.0f;
                    const float wAmp[3] = { 0.625f, 0.25f, 0.125f };
                    for (int o = 0; o < 3; ++o)
                    {
                        const int f = 4 << o;
                        worley += (1.0f - std::min(Worley(u * f, v * f, w * f, f, 101u + o), 1.0f)) * wAmp[o];
                    }

                    float detail = 0.0f;
                    for (int o = 0; o < 3; ++o)
                    {
                        const int f = 8 << o;
                        detail += (1.0f - std::min(Worley(u * f, v * f, w * f, f, 201u + o), 1.0f)) * wAmp[o];
                    }

                    // Perlin-Worley: remap(perlin, worley - 1, 1, 0, 1).
                    const float p01 = perlin * 0.5f + 0.5f;
                    const float lo = worley - 1.0f;
                    const size_t i = (size_t(z) * n + y) * n + x;
                    pw[i]  = (p01 - lo) / std::max(1.0f - lo, 1e-4f);
                    wor[i] = detail;
                    bil[i] = billow;
                    per[i] = p01;
                }

        Normalise(pw);
        Normalise(wor);
        Normalise(bil);
        Normalise(per);

        auto byte = [](float f) { return uint32_t(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f); };
        std::vector<uint32_t> top(count);
        for (size_t i = 0; i < count; ++i)
            top[i] = (byte(per[i]) << 24) | (byte(pw[i]) << 16) | (byte(wor[i]) << 8) | byte(bil[i]);
        g_levels.push_back(std::move(top));

        // Each level averages 2x2x2 of the one above, per channel, wrapping like the texture does.
        for (int size = n / 2; size >= 1; size /= 2)
        {
            const std::vector<uint32_t>& up = g_levels.back();
            const int upSize = size * 2;
            std::vector<uint32_t> level(size_t(size) * size * size);
            for (int z = 0; z < size; ++z)
                for (int y = 0; y < size; ++y)
                    for (int x = 0; x < size; ++x)
                    {
                        uint32_t sum[4] = {};
                        for (int c = 0; c < 8; ++c)
                        {
                            const uint32_t t = up[(size_t(z * 2 + (c >> 2 & 1)) * upSize + (y * 2 + (c >> 1 & 1))) * upSize + (x * 2 + (c & 1))];
                            for (int ch = 0; ch < 4; ++ch) sum[ch] += (t >> (ch * 8)) & 0xFF;
                        }
                        uint32_t v = 0;
                        for (int ch = 0; ch < 4; ++ch) v |= ((sum[ch] + 4) / 8) << (ch * 8);
                        level[(size_t(z) * size + y) * size + x] = v;
                    }
            g_levels.push_back(std::move(level));
        }
        g_bakeMs = GetTickCount() - start;
        g_baked = true;   // published last: the render thread reads the texels only after this
    }

    bool Upload(IDirect3DDevice9* dev)
    {
        const int n = nz::kSize;
        const UINT levels = UINT(g_levels.size());
        if (FAILED(dev->CreateVolumeTexture(n, n, n, levels, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_volume, nullptr)) || !g_volume)
        {
            g_volume = nullptr;
            return false;
        }
        for (UINT level = 0; level < levels; ++level)
        {
            const int size = n >> level;
            D3DLOCKED_BOX box{};
            if (FAILED(g_volume->LockBox(level, &box, nullptr, 0)))
            {
                g_volume->Release();
                g_volume = nullptr;
                return false;
            }
            for (int z = 0; z < size; ++z)
                for (int y = 0; y < size; ++y)
                {
                    auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(box.pBits) + size_t(z) * box.SlicePitch + size_t(y) * box.RowPitch);
                    std::copy_n(&g_levels[level][(size_t(z) * size + y) * size], size, row);
                }
            g_volume->UnlockBox(level);
        }
        WLOG_INFO("fog: noise volume %d^3 baked in %lu ms on a worker, uploaded", n, g_bakeMs);
        return true;
    }
}

namespace wxl::forever::fog::noise
{
    void StartBake()
    {
        if (g_started.exchange(true)) return;
        std::thread(&Bake).detach();
    }

    IDirect3DVolumeTexture9* Get(IDirect3DDevice9* dev)
    {
        if (g_volume || g_failed || !dev) return g_volume;
        if (!g_baked) { StartBake(); return nullptr; }
        if (!Upload(dev))
        {
            g_failed = true;
            WLOG_WARN("fog: noise volume unavailable");
        }
        return g_volume;
    }

    void Release()
    {
        if (g_volume) { g_volume->Release(); g_volume = nullptr; }
        g_failed = false;
    }
}
