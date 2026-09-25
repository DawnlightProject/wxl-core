// wxl-forever fog: a small heightfield around the camera, so the fog can hug the ground.
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
#include "Terrain.hpp"

#include "game/Pick.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <vector>

namespace
{
    namespace tr    = wxl::forever::fog::terrain;
    namespace world = wxl::game::world;

    constexpr int kN = tr::kCells;

    struct Cell
    {
        int   i = INT_MIN, j = INT_MIN;   // the world cell stored here; INT_MIN = never measured
        float height = 0.0f;
    };

    Cell  g_cells[kN * kN];
    float g_fallback = 0.0f;
    bool  g_dirty = true;
    int   g_centreI = 0, g_centreJ = 0;

    IDirect3DTexture9* g_texture = nullptr;
    bool               g_unavailable = false;

    int Wrap(int v) { v %= kN; return v < 0 ? v + kN : v; }

    Cell& At(int i, int j) { return g_cells[Wrap(j) * kN + Wrap(i)]; }

    bool Current(const Cell& c, int i, int j) { return c.i == i && c.j == j; }

    /// Ground under a cell centre, searched from a little above the camera downwards so a high roof
    /// or a bridge overhead is not taken for the ground.
    bool Measure(int i, int j, float eyeZ, float& out)
    {
        const float x = (float(i) + 0.5f) * tr::kCellSize;
        const float y = (float(j) + 0.5f) * tr::kCellSize;
        const float from[3] = { x, y, eyeZ + 8.0f };
        const float to[3]   = { x, y, eyeZ - 150.0f };
        world::WorldHit hit;
        if (!world::TraceLine(from, to, hit)) return false;
        out = hit.pos.z;
        return true;
    }
}

namespace wxl::forever::fog::terrain
{
    void Update(const float eye[3], int budget)
    {
        g_centreI = int(std::floor(eye[0] / kCellSize));
        g_centreJ = int(std::floor(eye[1] / kCellSize));

        float here;
        if (Measure(g_centreI, g_centreJ, eye[2], here)) g_fallback = here;

        // Rings outwards from the camera's cell: the nearest stale cells are measured first.
        int done = 0;
        const int half = kN / 2;
        for (int ring = 0; ring < half && done < budget; ++ring)
            for (int dj = -ring; dj <= ring && done < budget; ++dj)
                for (int di = -ring; di <= ring && done < budget; ++di)
                {
                    if (std::max(std::abs(di), std::abs(dj)) != ring) continue;
                    const int i = g_centreI + di, j = g_centreJ + dj;
                    Cell& c = At(i, j);
                    if (Current(c, i, j)) continue;
                    float h;
                    if (!Measure(i, j, eye[2], h)) h = g_fallback;
                    c.i = i;
                    c.j = j;
                    c.height = h;
                    g_dirty = true;
                    ++done;
                }
    }

    IDirect3DTexture9* Texture(IDirect3DDevice9* dev)
    {
        if (g_unavailable || !dev) return nullptr;
        if (!g_texture)
        {
            if (FAILED(dev->CreateTexture(kN, kN, 1, 0, D3DFMT_G32R32F, D3DPOOL_MANAGED, &g_texture, nullptr)) || !g_texture)
            {
                g_texture = nullptr;
                g_unavailable = true;
                WLOG_WARN("fog: G32R32F texture unavailable; the fog keeps an absolute base height");
                return nullptr;
            }
            g_dirty = true;
        }
        if (!g_dirty) return g_texture;

        // Unmeasured cells take the fallback so the mean below never mixes in stale ground.
        static float height[kN * kN];
        for (int j = 0; j < kN; ++j)
            for (int i = 0; i < kN; ++i)
            {
                const int wi = g_centreI - kN / 2 + i, wj = g_centreJ - kN / 2 + j;
                const Cell& c = At(wi, wj);
                height[Wrap(wj) * kN + Wrap(wi)] = Current(c, wi, wj) ? c.height : g_fallback;
            }

        D3DLOCKED_RECT lr{};
        if (FAILED(g_texture->LockRect(0, &lr, nullptr, 0))) return g_texture;
        for (int y = 0; y < kN; ++y)
        {
            float* row = reinterpret_cast<float*>(static_cast<unsigned char*>(lr.pBits) + size_t(y) * lr.Pitch);
            for (int x = 0; x < kN; ++x)
            {
                float sum = 0.0f;
                for (int dy = -2; dy <= 2; ++dy)
                    for (int dx = -2; dx <= 2; ++dx)
                        sum += height[Wrap(y + dy) * kN + Wrap(x + dx)];
                row[x * 2 + 0] = height[y * kN + x];   // G32R32F stores r first
                row[x * 2 + 1] = sum / 25.0f;
            }
        }
        g_texture->UnlockRect(0);
        g_dirty = false;
        return g_texture;
    }

    float Fallback() { return g_fallback; }

    int Filled()
    {
        int n = 0;
        for (int j = 0; j < kN; ++j)
            for (int i = 0; i < kN; ++i)
            {
                const int wi = g_centreI - kN / 2 + i, wj = g_centreJ - kN / 2 + j;
                if (Current(At(wi, wj), wi, wj)) ++n;
            }
        return n;
    }

    void Release()
    {
        if (g_texture) { g_texture->Release(); g_texture = nullptr; }
        g_unavailable = false;
        g_dirty = true;
    }
}
