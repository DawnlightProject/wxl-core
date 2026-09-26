// wxl-graphics-fog: the frame's simulation primitives.
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

#include "Primitives.hpp"

#include <algorithm>
#include <cmath>

namespace
{
    using wxl::gfx::fog::prims::Prim;

    Prim g_list[FOG_MAX_PRIMS];
    int  g_count = 0;

    void Bounds(Prim& p, const float lo[3], const float hi[3], float size)
    {
        for (int k = 0; k < 3; ++k)
        {
            p.lo[k] = lo[k];
            p.hi[k] = hi[k];
        }
        p.size = size;
    }

    Prim Make(const float a[3], uint32_t kind)
    {
        Prim p;
        for (int k = 0; k < 3; ++k) p.rows[0][k] = a[k];
        p.rows[0][3] = float(kind);
        return p;
    }
}

namespace wxl::gfx::fog::prims
{
    void Begin() { g_count = 0; }

    bool Add(const Prim& p)
    {
        if (g_count >= FOG_MAX_PRIMS) return false;
        g_list[g_count++] = p;
        return true;
    }

    int Count() { return g_count; }
    const Prim* List() { return g_list; }

    Prim Sphere(const float c[3], float radius)
    {
        Prim p = Make(c, FOG_PRIM_SPHERE);
        p.rows[1][3] = radius;
        const float lo[3] = { c[0] - radius, c[1] - radius, c[2] - radius };
        const float hi[3] = { c[0] + radius, c[1] + radius, c[2] + radius };
        Bounds(p, lo, hi, radius);
        return p;
    }

    Prim Box(const float c[3], const float half[3], float yaw)
    {
        Prim p = Make(c, FOG_PRIM_BOX);
        for (int k = 0; k < 3; ++k) p.rows[1][k] = half[k];
        p.rows[1][3] = std::min(half[0], std::min(half[1], half[2]));
        p.rows[4][0] = yaw;
        const float r = std::sqrt(half[0] * half[0] + half[1] * half[1]);
        const float lo[3] = { c[0] - r, c[1] - r, c[2] - half[2] };
        const float hi[3] = { c[0] + r, c[1] + r, c[2] + half[2] };
        Bounds(p, lo, hi, p.rows[1][3]);
        return p;
    }

    Prim Capsule(const float a[3], const float b[3], float radius)
    {
        Prim p = Make(a, FOG_PRIM_CAPSULE);
        for (int k = 0; k < 3; ++k) p.rows[1][k] = b[k];
        p.rows[1][3] = radius;
        float lo[3], hi[3];
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = std::min(a[k], b[k]) - radius;
            hi[k] = std::max(a[k], b[k]) + radius;
        }
        Bounds(p, lo, hi, radius);
        return p;
    }

    Prim Cylinder(const float base[3], float radius, float height)
    {
        Prim p = Make(base, FOG_PRIM_CYLINDER);
        p.rows[1][2] = height;
        p.rows[1][3] = radius;
        const float lo[3] = { base[0] - radius, base[1] - radius, base[2] };
        const float hi[3] = { base[0] + radius, base[1] + radius, base[2] + height };
        Bounds(p, lo, hi, std::min(radius, height));
        return p;
    }

    Prim Front(const float point[3], const float dir[2], float halfWidth, float height, float depth)
    {
        Prim p = Make(point, FOG_PRIM_FRONT);
        p.rows[1][0] = dir[0];
        p.rows[1][1] = dir[1];
        p.rows[1][3] = halfWidth;
        p.rows[4][0] = height;
        p.rows[4][1] = depth;
        // Unbounded laterally (or wide): the bins take the whole level.
        const float big = 1e6f;
        const float lo[3] = { -big, -big, -big };
        const float hi[3] = { big, big, big };
        Bounds(p, lo, hi, std::max(depth, 4.0f));
        return p;
    }

    Prim Ring(const float centre[3], float radius, float height, float depth)
    {
        Prim p = Make(centre, FOG_PRIM_RING);
        p.rows[1][3] = radius;
        p.rows[4][0] = height;
        p.rows[4][1] = depth;
        const float r = radius + depth;
        const float lo[3] = { centre[0] - r, centre[1] - r, -1e6f };
        const float hi[3] = { centre[0] + r, centre[1] + r, 1e6f };
        Bounds(p, lo, hi, std::max(depth, 4.0f));
        return p;
    }

    void SetDensity(Prim& p, float rate, float smoke, float hold, float soft, float billow)
    {
        p.rows[2][0] = rate;
        p.rows[2][1] = smoke;
        p.rows[2][2] = hold;
        p.rows[2][3] = soft;
        p.rows[4][3] = billow;
    }

    void SetFlow(Prim& p, const float push[3], float radial, float swirl)
    {
        for (int k = 0; k < 3; ++k) p.rows[3][k] = push[k];
        p.rows[3][3] = radial;
        p.rows[4][2] = swirl;
    }
}
