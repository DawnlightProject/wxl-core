// wxl-graphics-lights: a baked cube cookie softened by the size of its light's source.
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

#include "CookieBlur.hpp"

#include <algorithm>
#include <cmath>

namespace
{
    namespace ck = wxl::gfx::lights::cookies;

    constexpr float    kPi      = 3.14159265f;
    constexpr float    kHalfPi  = 1.57079633f;   // the angle a cube face spans, about its centre
    constexpr int      kMaxTaps = 32;
    constexpr unsigned kLeast   = 8;             // the smallest face the blur works at or reads from

    /// A cube of float texels, each face padded by one texel on every side with its neighbours'
    /// texels, so a bilinear fetch near an edge reads across the seam without a branch.
    struct Cube
    {
        float*   data = nullptr;
        unsigned size = 0, channels = 1, stride = 0;

        /// Texel (x, y) of face f, x and y from -1 (the padding) to size (the padding).
        float* At(unsigned f, int x, int y) const
        {
            return data + ((size_t(f) * stride + size_t(y + 1)) * stride + size_t(x + 1)) * channels;
        }
    };

    Cube Bind(std::vector<float>& storage, unsigned size, unsigned channels)
    {
        Cube c;
        c.size = size;
        c.channels = channels;
        c.stride = size + 2;
        storage.resize(size_t(6) * c.stride * c.stride * channels);
        c.data = storage.data();
        return c;
    }

    /// CubeFace's body, inlined into the taps: the major axis, its sign, then D3D's cube layout, as
    /// CookieUv (lamps.hlsli, cookies.hlsli) decides them, ties included (x wins over y and z, y over z).
    inline unsigned Project(const float d[3], float& u, float& v)
    {
        const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
        if (ax >= ay && ax >= az)
        {
            const float sgn = d[0] > 0.0f ? 1.0f : -1.0f, r = 0.5f / std::max(ax, 1e-5f);
            u = -sgn * d[2] * r + 0.5f;
            v = -d[1] * r + 0.5f;
            return sgn > 0.0f ? 0u : 1u;
        }
        if (ay >= az)
        {
            const float sgn = d[1] > 0.0f ? 1.0f : -1.0f, r = 0.5f / std::max(ay, 1e-5f);
            u = d[0] * r + 0.5f;
            v = sgn * d[2] * r + 0.5f;
            return sgn > 0.0f ? 2u : 3u;
        }
        const float sgn = d[2] > 0.0f ? 1.0f : -1.0f, r = 0.5f / std::max(az, 1e-5f);
        u = sgn * d[0] * r + 0.5f;
        v = -d[1] * r + 0.5f;
        return sgn > 0.0f ? 4u : 5u;
    }

    /// Fills each face's padding from the faces around it: the padding texel's centre, carried on past
    /// the face's edge in its plane, lands on a neighbour, whose nearest texel it takes.
    void FillBorder(const Cube& c)
    {
        const int n = int(c.size);
        for (unsigned f = 0; f < 6; ++f)
            for (int y = -1; y <= n; ++y)
                for (int x = -1; x <= n; ++x)
                {
                    if (x >= 0 && x < n && y >= 0 && y < n) continue;
                    float d[3], u = 0.0f, v = 0.0f;
                    ck::CubeDirection(f, 2.0f * (float(x) + 0.5f) / float(n) - 1.0f, 2.0f * (float(y) + 0.5f) / float(n) - 1.0f, d);
                    const unsigned g = Project(d, u, v);
                    const int sx = std::clamp(int(u * float(n)), 0, n - 1), sy = std::clamp(int(v * float(n)), 0, n - 1);
                    const float* src = c.At(g, sx, sy);
                    float* dst = c.At(f, x, y);
                    for (unsigned k = 0; k < c.channels; ++k) dst[k] = src[k];
                }
    }

    /// Face f's bilinear value at (x, y) in texels (texel i's centre at i), -1 to size: past the face's
    /// last texel centre it reads the padding, the neighbour's texels. C channels.
    template <unsigned C>
    inline void SampleFace(const Cube& c, unsigned f, float x, float y, float* out)
    {
        const float n = float(c.size);
        x = std::min(std::max(x, -1.0f), n - 1e-3f) + 1.0f;   // non-negative, so int() floors it
        y = std::min(std::max(y, -1.0f), n - 1e-3f) + 1.0f;
        const int x0 = int(x), y0 = int(y);                    // padded indices
        const float ax = x - float(x0), ay = y - float(y0);
        const float* t00 = c.data + ((size_t(f) * c.stride + size_t(y0)) * c.stride + size_t(x0)) * C;
        const float* t01 = t00 + size_t(c.stride) * C;
        for (unsigned k = 0; k < C; ++k)
        {
            const float top = t00[k] + (t00[C + k] - t00[k]) * ax;
            const float bottom = t01[k] + (t01[C + k] - t01[k]) * ax;
            out[k] = top + (bottom - top) * ay;
        }
    }

    /// The cube's bilinear value towards d (any length), across seams through the padding.
    template <unsigned C>
    inline void Sample(const Cube& c, const float d[3], float* out)
    {
        float u = 0.0f, v = 0.0f;
        const unsigned f = Project(d, u, v);
        const float n = float(c.size);
        SampleFace<C>(c, f, u * n - 0.5f, v * n - 0.5f, out);
    }

    /// Half of `from`'s size, each texel the mean of four.
    void Downsample(const Cube& from, const Cube& to)
    {
        for (unsigned f = 0; f < 6; ++f)
            for (int y = 0; y < int(to.size); ++y)
                for (int x = 0; x < int(to.size); ++x)
                {
                    const float* a = from.At(f, 2 * x, 2 * y);
                    const float* b = from.At(f, 2 * x + 1, 2 * y);
                    const float* c = from.At(f, 2 * x, 2 * y + 1);
                    const float* e = from.At(f, 2 * x + 1, 2 * y + 1);
                    float* o = to.At(f, x, y);
                    for (unsigned k = 0; k < to.channels; ++k) o[k] = 0.25f * (a[k] + b[k] + c[k] + e[k]);
                }
        FillBorder(to);
    }

    void Normalize(float d[3])
    {
        const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (l > 1e-12f) for (int k = 0; k < 3; ++k) d[k] /= l;
    }

    template <unsigned C>
    void Store(const float* v, unsigned bpp, uint8_t* out)
    {
        for (unsigned k = 0; k < C; ++k) out[k] = uint8_t(std::clamp(v[k], 0.0f, 1.0f) * 255.0f + 0.5f);
        if (bpp == 4) out[3] = 255;
    }

    /// The disc's mean at every texel of a cube of `work` texels a face, into `out` (the cookie's bytes,
    /// when work is the output's size) or into `blurred`.
    template <unsigned C>
    void Convolve(const Cube& source, unsigned work, float sigma, const float* tu, const float* tv, int taps,
                  const Cube* blurred, uint8_t* out, unsigned bpp)
    {
        const float weight = 1.0f / float(taps);
        for (unsigned f = 0; f < 6; ++f)
            for (unsigned y = 0; y < work; ++y)
                for (unsigned x = 0; x < work; ++x)
                {
                    float d[3];
                    ck::CubeDirection(f, 2.0f * (float(x) + 0.5f) / float(work) - 1.0f, 2.0f * (float(y) + 0.5f) / float(work) - 1.0f, d);
                    Normalize(d);
                    // A tangent basis around d, scaled by the disc's radius: the disc lies in its plane.
                    const bool pole = std::fabs(d[2]) >= 0.9f;
                    float t1[3] = { pole ? 0.0f : -d[1], pole ? -d[2] : d[0], pole ? d[1] : 0.0f };   // (up x d)
                    Normalize(t1);
                    float t2[3] = { d[1] * t1[2] - d[2] * t1[1], d[2] * t1[0] - d[0] * t1[2], d[0] * t1[1] - d[1] * t1[0] };
                    for (int k = 0; k < 3; ++k)
                    {
                        t1[k] *= sigma;
                        t2[k] *= sigma;
                    }
                    float sum[C] = {}, tap[C];
                    for (int k = 0; k < taps; ++k)
                    {
                        const float a = tu[k], b = tv[k];
                        const float dir[3] = { d[0] + a * t1[0] + b * t2[0], d[1] + a * t1[1] + b * t2[1], d[2] + a * t1[2] + b * t2[2] };
                        Sample<C>(source, dir, tap);
                        for (unsigned c = 0; c < C; ++c) sum[c] += tap[c];
                    }
                    for (unsigned c = 0; c < C; ++c) sum[c] *= weight;
                    if (!blurred) Store<C>(sum, bpp, out + ((size_t(f) * work + y) * work + x) * bpp);
                    else
                    {
                        float* o = blurred->At(f, int(x), int(y));
                        for (unsigned c = 0; c < C; ++c) o[c] = sum[c];
                    }
                }
    }

    /// Bilinear from the blurred cube up to the output's size. An output texel's direction falls on
    /// its own face, where the padding carries the neighbours' texels, so the seams stay continuous.
    template <unsigned C>
    void Upsample(const Cube& blurred, unsigned outSize, uint8_t* out, unsigned bpp)
    {
        const float scale = float(blurred.size) / float(outSize);
        float v[C];
        for (unsigned f = 0; f < 6; ++f)
            for (unsigned y = 0; y < outSize; ++y)
                for (unsigned x = 0; x < outSize; ++x)
                {
                    SampleFace<C>(blurred, f, (float(x) + 0.5f) * scale - 0.5f, (float(y) + 0.5f) * scale - 0.5f, v);
                    Store<C>(v, bpp, out + ((size_t(f) * outSize + y) * outSize + x) * bpp);
                }
    }
}

namespace wxl::gfx::lights::cookies
{
    void CubeDirection(unsigned face, float s, float t, float d[3])
    {
        // CookieUv: +-X take s = -+z, t = -y; +-Y take s = x, t = +-z; +-Z take s = +-x, t = -y.
        switch (face)
        {
        case 0:  d[0] = 1.0f;  d[1] = -t;    d[2] = -s;    break;
        case 1:  d[0] = -1.0f; d[1] = -t;    d[2] = s;     break;
        case 2:  d[0] = s;     d[1] = 1.0f;  d[2] = t;     break;
        case 3:  d[0] = s;     d[1] = -1.0f; d[2] = -t;    break;
        case 4:  d[0] = s;     d[1] = -t;    d[2] = 1.0f;  break;
        default: d[0] = -s;    d[1] = -t;    d[2] = -1.0f; break;
        }
    }

    unsigned CubeFace(const float d[3], float& u, float& v) { return Project(d, u, v); }

    float SoftAngle(float sourceRadius, float cageDistance, float softness, unsigned faceTexels)
    {
        const float texel = kHalfPi / float(std::max(faceTexels, 1u));
        const float theta = sourceRadius > 0.0f && cageDistance > 0.0f ? std::max(softness, 0.0f) * 2.0f * sourceRadius / cageDistance : 0.0f;
        return std::min(std::max(theta, texel), std::max(kMaxSoftAngle, texel));
    }

    void BlurCube(const uint8_t* in, unsigned inSize, unsigned bpp, float theta, uint8_t* out, unsigned outSize,
                  BlurScratch& scratch)
    {
        if (!in || !out || !inSize || !outSize) return;
        const unsigned channels = bpp == 4 ? 3u : 1u;
        const float sigma = 0.5f * std::max(theta, 0.0f);   // the disc's radius

        // A wide blur is smooth: it is worked at the smallest size whose texels stay within a third of
        // its radius, then brought up.
        unsigned work = outSize;
        while (work / 2 >= kLeast && kHalfPi / float(work / 2) <= sigma / 3.0f) work /= 2;
        // Taps enough to cover the disc at the working size: a disc of a few texels needs few.
        const float across = sigma / (kHalfPi / float(work));
        const int taps = std::clamp(int(std::ceil(3.5f * across * across)), 4, kMaxTaps);
        // Taps lie about sqrt(pi / taps) radii apart: they read the coarsest level whose texels are no
        // wider, so a thin strut between two taps still reaches them.
        const float spacing = sigma * std::sqrt(kPi / float(taps));
        unsigned level = 0;
        while ((inSize >> (level + 1)) >= kLeast && kHalfPi / float(inSize >> (level + 1)) <= spacing) ++level;

        if (scratch.levels.size() < level + 1) scratch.levels.resize(level + 1);
        Cube source = Bind(scratch.levels[0], inSize, channels);
        for (unsigned f = 0; f < 6; ++f)
            for (unsigned y = 0; y < inSize; ++y)
                for (unsigned x = 0; x < inSize; ++x)
                {
                    const uint8_t* p = in + ((size_t(f) * inSize + y) * inSize + x) * bpp;
                    float* o = source.At(f, int(x), int(y));
                    for (unsigned k = 0; k < channels; ++k) o[k] = float(p[k]) / 255.0f;
                }
        FillBorder(source);
        for (unsigned l = 1; l <= level; ++l)
        {
            const Cube coarser = Bind(scratch.levels[l], inSize >> l, channels);
            Downsample(source, coarser);
            source = coarser;
        }

        // The Vogel spiral over the unit disc: equal areas per tap, the golden angle between them.
        float tu[kMaxTaps], tv[kMaxTaps];
        for (int k = 0; k < taps; ++k)
        {
            const float r = std::sqrt((float(k) + 0.5f) / float(taps)), a = float(k) * 2.39996323f;
            tu[k] = r * std::cos(a);
            tv[k] = r * std::sin(a);
        }

        if (work == outSize)
        {
            if (channels == 3) Convolve<3>(source, work, sigma, tu, tv, taps, nullptr, out, bpp);
            else Convolve<1>(source, work, sigma, tu, tv, taps, nullptr, out, bpp);
            return;
        }
        const Cube blurred = Bind(scratch.blurred, work, channels);
        if (channels == 3) Convolve<3>(source, work, sigma, tu, tv, taps, &blurred, out, bpp);
        else Convolve<1>(source, work, sigma, tu, tv, taps, &blurred, out, bpp);
        FillBorder(blurred);
        if (channels == 3) Upsample<3>(blurred, outSize, out, bpp);
        else Upsample<1>(blurred, outSize, out, bpp);
    }
}
