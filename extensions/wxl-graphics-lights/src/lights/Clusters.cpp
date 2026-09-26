// wxl-graphics-lights: the light texture, the per-cluster light lists and the omni slot table shaders read.
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

#include "../core/Extension.hpp"
#include "Lights.hpp"
#include "Families.hpp"
#include "Rooms.hpp"
#include "Textures.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace
{
    namespace gl = wxl::gfx::lights;
    namespace tex = wxl::gfx::lights::tex;

    constexpr int kTexW = gl::kClusterTexW;
    constexpr int kClusters = gl::kClustersX * gl::kClustersY * gl::kClustersZ;
    // One texel per cluster: its list's offset (low, high 16 bits) and length.
    constexpr int kGridRows = (kClusters + kTexW - 1) / kTexW;
    // Per light: a cookie rotation row block, then a cookie cell row block (cookies.hlsli);
    // light i at column i % kTexW, row i / kTexW of each block.
    constexpr int kBlockRows = (gl::kMaxLights + kTexW - 1) / kTexW;
    constexpr int kCookieBlocks = 2;
    // The pool holds every light in every cluster, so no list is ever cut.
    constexpr int kPoolEntries = kClusters * gl::kMaxLights;
    constexpr int kPoolRows = (kPoolEntries / 4 + kTexW - 1) / kTexW;
    constexpr int kPoolStart = kGridRows + kCookieBlocks * kBlockRows;
    constexpr int kTexH = kPoolStart + kPoolRows;

    tex::Twin g_lightTex;
    tex::Twin g_clusterTex;
    tex::Twin g_omniTex;
    tex::Twin g_sourceTex;
    bool      g_failed = false;
    float     g_rows[gl::kLightRows][gl::kMaxLights][4] = {};
    float     g_sources[gl::kSourceRows][gl::kMaxLights][4] = {};
    float     g_reach[gl::kMaxLights] = {};   // what the clusters assign by: the larger of both reaches
    // Every value is a float as the shader reads it: indices and offsets are exact integers far past
    // what a 16-bit channel sampled through half precision would keep.
    float     g_grid[kGridRows * kTexW][4];
    float     g_cookieRows[kCookieBlocks * kBlockRows][kTexW][4];
    float     g_pool[kPoolRows * kTexW * 4];
    int       g_poolUsed = 0;      // entries written by the last Publish
    int       g_peak = 0;          // the longest list of the last Publish

    /// A light's clusters: x0..x1, y0..y1, z0..z1 inclusive; none when z0 > z1.
    struct Box { int x0, x1, y0, y1, z0, z1; };
    Box g_boxes[gl::kMaxLights];

    /// The rooms a light may light (bit b for room b of rooms::Rows; 0 outside every room), as
    /// rooms::Mask gives them. r is the light camera-relative.
    uint32_t RoomMask(const gl::Light& l, const float r[3]) { return gl::rooms::Mask(l.room, r); }

    /// The HDR source texture's rows for light i (shaders/wxl/lights/sources.hlsli). cell and q are the
    /// light's cookie rows as the cluster texture holds them.
    void WriteSource(int i, const gl::Light& l, const float eye[3], bool tube, const float* cell, const float* q, uint32_t mask)
    {
        namespace fm = gl::families;
        float* s[gl::kSourceRows];
        for (int row = 0; row < gl::kSourceRows; ++row) s[row] = g_sources[row][i];
        float rgb[3];
        gl::SourceIntensity(l, rgb);
        const bool spot = !tube && l.cosCone > -1.0f;
        for (int k = 0; k < 3; ++k)
        {
            s[0][k] = l.position[k] - eye[k];
            s[1][k] = rgb[k];
            s[2][k] = spot ? l.direction[k] : 0.0f;
            s[4][k] = tube ? l.extent[k] : 0.0f;
            s[6][k] = l.rest[k] - eye[k];
        }
        s[0][3] = std::max(l.reach, 0.5f);
        s[1][3] = std::max(l.softRadius, 0.01f);
        s[2][3] = spot ? l.cosCone : -2.0f;
        // A spot reaches full strength a quarter of the way in from its edge, as the legacy cone does.
        s[3][0] = spot ? l.cosCone + (1.0f - l.cosCone) * 0.25f : -2.0f;
        s[3][1] = float(l.profile);
        s[3][2] = float(gl::SourceFlags(l));
        s[3][3] = float(l.family);
        s[4][3] = l.emissiveRadius;
        s[5][0] = gl::SourceEmissive(l);
        s[5][1] = float(l.room);
        s[5][2] = std::clamp(l.roomGate, 0.0f, 1.0f);
        s[5][3] = float(mask & 0xFFFu);
        s[6][3] = l.flickerGain;
        for (int k = 0; k < 4; ++k)
        {
            s[7][k] = q[k];
            s[8][k] = cell[k];
        }
        s[9][0] = l.flicker == gl::Flicker::Fire || l.flicker == gl::Flicker::Candle
                ? std::clamp(gl::Settings().hotCore * fm::Get(l.family).hot * 2.0f, 0.0f, 1.0f) : 0.0f;
        s[9][1] = l.kelvin;
        s[9][2] = l.fade;
        s[9][3] = 0.0f;
    }

    int   g_published = 0;
    float g_viewProj[16] = {};   // the last Publish's space, for the self-check
    float g_eye[3] = {};
    float g_nearD = 0.0f, g_logRatio = 1.0f;

    /// Gamma to linear: the client's colours are gamma-encoded, the consumers light in linear light.
    float Lin(float c) { return std::pow(std::max(c, 0.0f), 2.2f); }

    bool Ensure(IDirect3DDevice9* dev)
    {
        if (g_lightTex && g_clusterTex && g_omniTex && g_sourceTex) return true;
        if (g_failed) return false;
        if (!tex::Create(dev, gl::kMaxLights, gl::kLightRows, D3DFMT_A32B32G32R32F, g_lightTex, "light texture")
            || !tex::Create(dev, kTexW, kTexH, D3DFMT_A32B32G32R32F, g_clusterTex, "cluster texture")
            || !tex::Create(dev, gl::kMaxLights, 2, D3DFMT_A32B32G32R32F, g_omniTex, "omni table")
            || !tex::Create(dev, gl::kMaxLights, gl::kSourceRows, D3DFMT_A32B32G32R32F, g_sourceTex, "source texture"))
        {
            gl::ReleaseTextures();
            g_failed = true;
            LIGHTS_LOG_WARN("lights: light textures unavailable on this device; no light reaches the shaders");
            return false;
        }
        return true;
    }

    /// The clusters a light's sphere may touch: its screen bounds from the eight corners of its box
    /// (the whole screen when the camera is inside or behind it), its depth range on the exponential
    /// slices, each widened a little for jittered samples near a boundary. l is its first row.
    Box Bounds(const float* l, float radius, const gl::ClusterSpace& space)
    {
        Box b{ 0, -1, 0, -1, 1, 0 };
        const float* m = space.viewProjRel;
        const float nearD = space.nearDistance;
        auto sliceOf = [&](float d) {
            if (d <= nearD) return 0;
            return int(std::floor(std::log(d / nearD) / space.logRatio * float(gl::kClustersZ)));
        };
        const float dist = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
        const int z0 = std::max(sliceOf(std::max(dist - radius, 0.0f) * 0.97f), 0);
        const int z1 = std::min(sliceOf((dist + radius) * 1.03f), gl::kClustersZ - 1);
        if (z0 > gl::kClustersZ - 1) return b;

        float u0 = 0.0f, u1 = 1.0f, v0 = 0.0f, v1 = 1.0f;
        if (dist > radius * 1.05f + nearD)
        {
            u0 = v0 = 1e9f;
            u1 = v1 = -1e9f;
            bool behind = false;
            for (int k = 0; k < 8 && !behind; ++k)
            {
                const float p[3] = { l[0] + ((k & 1) ? radius : -radius), l[1] + ((k & 2) ? radius : -radius),
                                     l[2] + ((k & 4) ? radius : -radius) };
                float clip[4];
                for (int j = 0; j < 4; ++j) clip[j] = p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j];
                if (clip[3] <= 1e-3f) { behind = true; break; }
                const float u = clip[0] / clip[3] * 0.5f + 0.5f, v = 0.5f - clip[1] / clip[3] * 0.5f;
                u0 = std::min(u0, u); u1 = std::max(u1, u);
                v0 = std::min(v0, v); v1 = std::max(v1, v);
            }
            if (behind) { u0 = v0 = 0.0f; u1 = v1 = 1.0f; }
            if (u1 < 0.0f || u0 > 1.0f || v1 < 0.0f || v0 > 1.0f) return b;
        }
        b.x0 = std::clamp(int(std::floor(u0 * gl::kClustersX - 0.1f)), 0, gl::kClustersX - 1);
        b.x1 = std::clamp(int(std::floor(u1 * gl::kClustersX + 0.1f)), 0, gl::kClustersX - 1);
        b.y0 = std::clamp(int(std::floor(v0 * gl::kClustersY - 0.1f)), 0, gl::kClustersY - 1);
        b.y1 = std::clamp(int(std::floor(v1 * gl::kClustersY + 0.1f)), 0, gl::kClustersY - 1);
        b.z0 = z0;
        b.z1 = z1;
        return b;
    }

    int ClusterId(int x, int y, int z) { return x + y * gl::kClustersX + z * gl::kClustersX * gl::kClustersY; }

    /// Every light into every cluster its sphere may touch, in list order (the service's id order):
    /// the lengths, their running offsets, then the indices.
    void Assign(const gl::ClusterSpace& space)
    {
        static uint32_t counts[kClusters];
        static uint32_t cursor[kClusters];
        std::memset(counts, 0, sizeof counts);
        for (int i = 0; i < g_published; ++i)
        {
            g_boxes[i] = Bounds(g_rows[0][i], g_reach[i], space);
            const Box& b = g_boxes[i];
            for (int z = b.z0; z <= b.z1; ++z)
                for (int y = b.y0; y <= b.y1; ++y)
                    for (int x = b.x0; x <= b.x1; ++x) ++counts[ClusterId(x, y, z)];
        }
        std::memset(g_grid, 0, sizeof g_grid);
        uint32_t offset = 0;
        g_peak = 0;
        for (int c = 0; c < kClusters; ++c)
        {
            cursor[c] = offset;
            g_grid[c][0] = float(offset);
            g_grid[c][1] = float(counts[c]);
            g_peak = std::max(g_peak, int(counts[c]));
            offset += counts[c];
        }
        g_poolUsed = int(offset);
        for (int i = 0; i < g_published; ++i)
        {
            const Box& b = g_boxes[i];
            for (int z = b.z0; z <= b.z1; ++z)
                for (int y = b.y0; y <= b.y1; ++y)
                    for (int x = b.x0; x <= b.x1; ++x) g_pool[cursor[ClusterId(x, y, z)]++] = float(i);
        }
    }

    /// The CPU rows into the twins, then up: the light texture whole, the cluster texture down to
    /// the last pool row this Publish filled (nothing reads past a list's end).
    void Upload(IDirect3DDevice9* dev)
    {
        int pitch = 0;
        if (uint8_t* bits = tex::Lock(g_lightTex, nullptr, pitch))
        {
            for (int row = 0; row < gl::kLightRows; ++row)
                std::memcpy(bits + size_t(row) * size_t(pitch), g_rows[row], sizeof g_rows[row]);
            tex::Unlock(g_lightTex);
            tex::Upload(dev, g_lightTex);
        }
        if (uint8_t* bits = tex::Lock(g_sourceTex, nullptr, pitch))
        {
            for (int row = 0; row < gl::kSourceRows; ++row)
                std::memcpy(bits + size_t(row) * size_t(pitch), g_sources[row], sizeof g_sources[row]);
            tex::Unlock(g_sourceTex);
            tex::Upload(dev, g_sourceTex);
        }
        // A32B32G32R32F stores red, green, blue, alpha in that order, as a shader reads them.
        const int used = std::min((g_poolUsed / 4 + kTexW) / kTexW, kPoolRows);
        const RECT filled{ 0, 0, kTexW, kPoolStart + used };
        if (uint8_t* bits = tex::Lock(g_clusterTex, &filled, pitch))
        {
            const size_t rowBytes = size_t(kTexW) * 16;
            for (int r = 0; r < kGridRows; ++r)
                std::memcpy(bits + size_t(r) * size_t(pitch), g_grid[r * kTexW], rowBytes);
            for (int r = 0; r < kCookieBlocks * kBlockRows; ++r)
                std::memcpy(bits + size_t(kGridRows + r) * size_t(pitch), g_cookieRows[r], rowBytes);
            for (int r = 0; r < used; ++r)
                std::memcpy(bits + size_t(kPoolStart + r) * size_t(pitch), g_pool + size_t(r) * kTexW * 4, rowBytes);
            tex::Unlock(g_clusterTex);
            tex::UploadRows(dev, g_clusterTex, unsigned(kPoolStart + used));
        }
    }

    // The omni slots of the last PublishOmni.
    WXL_GfxOmniSlot g_omniSlots[WXL_GFX_LIGHTS_OMNI_MAX] = {};
    int             g_omniCount = 0;
    uint32_t        g_omniFrame = 0;
}

namespace wxl::gfx::lights
{
    float PublishedColour(const Light& l, float rgb[3], float& radius)
    {
        // The engine's colours carry their intensity (a model light's may reach 8). Up to 1 the
        // colour is gamma-decoded whole; past 1 only its hue is and the excess stays linear (the
        // two meet at 1), then the brightest channel is capped.
        const Options& o = Settings();
        const float peak = std::max(std::max(l.color[0], l.color[1]), std::max(l.color[2], 1e-6f));
        const bool hdr = peak > 1.0f;
        float raw = 0.0f;
        for (int k = 0; k < 3; ++k)
        {
            rgb[k] = (hdr ? Lin(l.color[k] / peak) * peak : Lin(l.color[k])) * l.intensity;
            raw = std::max(raw, Lin(l.color[k]) * l.intensity);
        }
        // The families' chroma at the luminance the fog was tuned on: halos and surfaces share a hue.
        if (o.legacyChroma)
        {
            const float luma = families::Luma(rgb);
            for (int k = 0; k < 3; ++k) rgb[k] = l.chroma[k] * luma;
        }
        const float top = std::max(std::max(rgb[0], rgb[1]), rgb[2]);
        const float bound = top <= o.maxPeak ? 1.0f : o.maxPeak / top;
        for (int k = 0; k < 3; ++k) rgb[k] *= bound;
        radius = std::min(l.radius, std::max(o.maxRadius, 1.0f));
        return raw;
    }

    bool Publish(IDirect3DDevice9* dev, const Light* lights, int count, const ClusterSpace& space)
    {
        if (!dev || !Ensure(dev)) return false;
        g_published = lights ? std::clamp(count, 0, kMaxLights) : 0;
        // Columns past the list hold no light (radius 0), never a light of an earlier frame.
        for (int row = 0; row < kLightRows; ++row)
            std::memset(g_rows[row][g_published], 0, sizeof(float) * 4 * size_t(kMaxLights - g_published));
        std::memset(g_cookieRows, 0, sizeof g_cookieRows);
        std::memset(g_sources, 0, sizeof g_sources);
        for (int i = 0; i < g_published; ++i)
        {
            const Light& l = lights[i];
            // Cookie rows: the rotation, then the cell code (0 = not resident) and the cookie's mean
            // transmittance rgb, known from the manifest or the file, resident or not (0 = no cookie
            // known): what the light averages out to where its cookie cannot be sampled.
            float* q = g_cookieRows[i / kTexW][i % kTexW];
            for (int k = 0; k < 4; ++k) q[k] = l.cookieRotation[k];
            float* cell = g_cookieRows[kBlockRows + i / kTexW][i % kTexW];
            cell[0] = l.cookieOpen >= 0.0f ? float(l.cookieCell) : 0.0f;
            if (l.cookieOpen >= 0.0f)
            {
                for (int k = 0; k < 3; ++k) cell[1 + k] = std::clamp(l.cookieOpen * l.cookieTint[k], 0.0f, 1.0f);
                // A cookie that lets nothing through still reads as known.
                if (cell[1] + cell[2] + cell[3] <= 0.0f) cell[2] = 1e-4f;
            }
            float* r0 = g_rows[0][i];
            float* r1 = g_rows[1][i];
            float* r2 = g_rows[2][i];
            float* r3 = g_rows[3][i];
            const bool tube = l.extent[0] != 0.0f || l.extent[1] != 0.0f || l.extent[2] != 0.0f;
            float rgb[3], radius = 0.0f;
            PublishedColour(l, rgb, radius);
            for (int k = 0; k < 3; ++k)
            {
                r0[k] = l.position[k] - space.eye[k];
                r1[k] = rgb[k];
                r2[k] = tube ? l.extent[k] : l.direction[k];
            }
            // A negative radius marks a light inside an interior space.
            r0[3] = std::max(radius, 0.01f) * (l.interior ? -1.0f : 1.0f);
            r1[3] = l.cosCone;
            r2[3] = std::clamp(l.innerRadius, 0.0f, std::fabs(r0[3]) * 0.95f);
            r3[0] = std::max(l.size, 0.0f);
            r3[1] = float(l.profile);
            r3[2] = tube ? 1.0f : 0.0f;
            // Flags: 1 the engine lights models with it already (an M2 light, or a table light merged
            // with one), 2 carried by another model, then 4 times the rooms it may light.
            r3[3] = (l.engineLit ? 1.0f : 0.0f) + (l.carried ? 2.0f : 0.0f) + 4.0f * float(RoomMask(l, r0));
            if (tube) r1[3] = -2.0f;   // a tube is never a spot
            g_reach[i] = std::max(std::fabs(r0[3]), std::min(l.reach, std::max(Settings().maxRadius, 4.0f)));
            WriteSource(i, l, space.eye, tube, cell, q, RoomMask(l, r0));
            // A point light has no axis: those texels carry its fog halo's reach and reference.
            if (!tube && r1[3] <= -1.0f)
            {
                const float halo = l.haloRadius > 0.0f ? std::min(l.haloRadius, std::fabs(r0[3])) : 0.0f;
                r2[0] = halo;
                r2[1] = halo > 0.0f ? std::clamp(l.haloInner, 0.0f, halo * 0.95f) : 0.0f;
                r2[2] = 0.0f;
            }
        }
        std::memcpy(g_viewProj, space.viewProjRel, sizeof g_viewProj);
        std::memcpy(g_eye, space.eye, sizeof g_eye);
        g_nearD = space.nearDistance;
        g_logRatio = space.logRatio;
        Assign(space);
        Upload(dev);
        return true;
    }

    void DescribePublished(int i, const Light* source, char* out, size_t size)
    {
        if (i < 0 || i >= g_published || !source)
        {
            std::snprintf(out, size, "light %d not published", i);
            return;
        }
        // The layout the shaders derive (lights.hlsli ClusterGridRows, ClusterPoolStart; cookies.hlsli
        // CookieRow) from clusterC against the one written here.
        const int shaderGridRows = int(std::ceil(float(kClustersX * kClustersY * kClustersZ) / float(kTexW)));
        const int shaderPerBlock = int(std::ceil(float(kMaxLights) / float(kTexW)));
        const bool layout = shaderGridRows == kGridRows && shaderPerBlock == kBlockRows
                         && shaderGridRows + 2 * shaderPerBlock == kPoolStart;
        // ReadLight's addressing: column (i + 0.5) / capacity, row k at v (k + 0.5) / kLightRows.
        const int col = int(std::floor((float(i) + 0.5f) / float(kMaxLights) * float(kMaxLights)));
        const float* a = g_rows[0][col];
        const float* b = g_rows[1][col];
        const float* c = g_rows[2][col];
        const float* e = g_rows[3][col];
        float rgb[3], radius = 0.0f;
        PublishedColour(*source, rgb, radius);
        const float src[3] = { source->position[0] - g_eye[0], source->position[1] - g_eye[1], source->position[2] - g_eye[2] };
        const float posErr = std::fabs(a[0] - src[0]) + std::fabs(a[1] - src[1]) + std::fabs(a[2] - src[2]);
        const float colErr = std::fabs(b[0] - rgb[0]) + std::fabs(b[1] - rgb[1]) + std::fabs(b[2] - rgb[2]);
        // LightList at the light's own place on screen: its list must hold it.
        float clip[4];
        for (int j = 0; j < 4; ++j) clip[j] = a[0] * g_viewProj[j] + a[1] * g_viewProj[4 + j] + a[2] * g_viewProj[8 + j] + g_viewProj[12 + j];
        char cluster[96] = "behind the camera";
        if (clip[3] > 1e-3f)
        {
            const float u = clip[0] / clip[3] * 0.5f + 0.5f, v = 0.5f - clip[1] / clip[3] * 0.5f;
            const float dist = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            const float depth01 = std::log(std::max(dist, g_nearD) / g_nearD) / g_logRatio;
            const int x = std::clamp(int(std::floor(u * kClustersX)), 0, kClustersX - 1);
            const int y = std::clamp(int(std::floor(v * kClustersY)), 0, kClustersY - 1);
            const int z = std::clamp(int(std::floor(depth01 * kClustersZ)), 0, kClustersZ - 1);
            const float* t = g_grid[ClusterId(x, y, z)];
            const uint32_t offset = uint32_t(t[0]), length = uint32_t(t[1]);
            bool listed = false;
            for (uint32_t s = 0; s < length; ++s) listed = listed || g_pool[offset + s] == float(i);
            std::snprintf(cluster, sizeof cluster, "cluster (%d %d %d) of %d lights %s it", x, y, z, int(length),
                          listed ? "lists" : (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f ? "(off screen) omits" : "OMITS"));
        }
        std::snprintf(out, size,
                      "light %d (id %08X) as the shader reads it: at (%.2f %.2f %.2f) radius %.2f colour (%.2f %.2f %.2f) reference %.2f | "
                      "source at (%.2f %.2f %.2f) radius %.2f colour (%.2f %.2f %.2f) | position off %.3f, colour off %.3f, "
                      "radius %s | room %d, floor %.1f ceiling %.1f camera-relative (%s) | layout %s (%dx%d, pool at row %d, "
                      "%d entries, longest list %d) | %s",
                      i, source->id, a[0], a[1], a[2], a[3], b[0], b[1], b[2], c[3], src[0], src[1], src[2], radius, rgb[0], rgb[1], rgb[2],
                      posErr, colErr, std::fabs(std::fabs(a[3]) - std::max(radius, 0.01f)) < 1e-3f ? "matches" : "DIFFERS",
                      source->room, source->roomFloor - g_eye[2], source->roomCeiling - g_eye[2],
                      e[3] >= 4.0f ? "may light rooms mask published" : "no room mask",
                      layout ? "matches" : "DIFFERS", kTexW, kTexH, kPoolStart, g_poolUsed, g_peak, cluster);
    }

    void PublishOmni(IDirect3DDevice9* dev, const WXL_GfxOmniSlot* slots, int count, const float* housing,
                     const float* receiverSkip, uint32_t frame)
    {
        g_omniCount = slots ? std::clamp(count, 0, WXL_GFX_LIGHTS_OMNI_MAX) : 0;
        g_omniFrame = frame;
        for (int k = 0; k < g_omniCount; ++k) g_omniSlots[k] = slots[k];
        if (!dev || !Ensure(dev)) return;
        static float rows[2][kMaxLights][4];
        std::memset(rows, 0, sizeof rows);
        for (int k = 0; k < g_omniCount; ++k)
        {
            const WXL_GfxOmniSlot& sl = g_omniSlots[k];
            if (sl.lightIndex >= 0 && sl.lightIndex < kMaxLights && sl.weight > 0.0f)
            {
                rows[0][sl.lightIndex][0] = float(k + 1);
                rows[0][sl.lightIndex][1] = std::clamp(sl.weight, 0.0f, 1.0f);
                rows[0][sl.lightIndex][2] = receiverSkip ? std::max(receiverSkip[k], 0.0f) : 0.0f;
            }
            float* t = rows[1][k * 20];
            for (int j = 0; j < 3; ++j) t[j] = sl.position[j];
            t[3] = sl.radius;
            t[4] = sl.faceSize > 0.0f ? 1.0f / (4.0f * sl.faceSize) : 0.0f;
            t[5] = sl.faceSize > 0.0f ? 1.0f / (2.0f * sl.faceSize) : 0.0f;
            t[6] = sl.faceSize;
            t[7] = housing ? housing[k] : 0.0f;
            std::memcpy(t + 8, sl.rows, sizeof sl.rows);
        }
        int pitch = 0;
        uint8_t* bits = tex::Lock(g_omniTex, nullptr, pitch);
        if (!bits) return;
        for (int r = 0; r < 2; ++r)
            std::memcpy(bits + size_t(r) * size_t(pitch), rows[r], sizeof rows[r]);
        tex::Unlock(g_omniTex);
        tex::Upload(dev, g_omniTex);
    }

    IDirect3DTexture9* OmniTexture() { return g_omniTex.gpu; }

    const WXL_GfxOmniSlot* CurrentOmni(int& count, uint32_t& frame)
    {
        count = g_omniCount;
        frame = g_omniFrame;
        return g_omniSlots;
    }

    IDirect3DTexture9* LightTexture() { return g_lightTex.gpu; }
    IDirect3DTexture9* SourceTexture() { return g_sourceTex.gpu; }
    IDirect3DTexture9* ClusterTexture() { return g_clusterTex.gpu; }
    int ClusterTextureWidth() { return kTexW; }
    int ClusterTextureHeight() { return kTexH; }
    int Published() { return g_published; }
    int ClusterPeak() { return g_peak; }
    int ClusterEntries() { return g_poolUsed; }

    void ReleaseTextures()
    {
        tex::Release(g_lightTex);
        tex::Release(g_clusterTex);
        tex::Release(g_omniTex);
        tex::Release(g_sourceTex);
        g_omniCount = 0;
        g_failed = false;
    }
}
