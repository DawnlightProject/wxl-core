// wxl-graphics-lights: light cookies, the shadow a lantern's own cage throws, baked per model light.
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
#include "Cookies.hpp"
#include "CookieBlur.hpp"
#include "Families.hpp"
#include "ModelTable.hpp"
#include "Textures.hpp"

#include "wxl/gfx/RenderState.hpp"
#include "wxl/gfx/Ui.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    namespace gl  = wxl::gfx::lights;
    namespace ck  = wxl::gfx::lights::cookies;
    namespace tex = wxl::gfx::lights::tex;
    namespace ui  = wxl::gfx::ui;

    constexpr int      kCellsAcross = 4;    // cookies per atlas row, kCookiesAcross in cookies.hlsli; each 4 x 2 faces
    constexpr int      kMaxBudget   = 64;   // atlas cells at most: 16 rows of 2 faces, 4096 texels at 128 a face
    constexpr uint32_t kKeepFrames  = 120;  // a cell stays this long after its cookie was last wanted
    constexpr float    kPinYards    = 30.0f; // lamps this near keep their cookie resident first
    constexpr int      kMeasuring   = 4;    // cookie reads in flight only to learn their mean
    constexpr float    kEaseSeconds = 0.5f; // a cookie's pattern fades in and out, and its mean moves, over this
    constexpr int      kFillsPerFrame = 1;  // cells filled (decoded, softened, copied up) in one frame
    constexpr DWORD    kSoftnessRest = 500; // ms the softness must rest before every cookie is read again
    constexpr const char* kFolder   = "Textures\\Forever\\Cookies";

    ck::Settings g_cfg;
    bool         g_loaded = false;
    const WXL_GfxManifest* g_manifest = nullptr;
    char         g_folder[260] = "";   // the manifest's folder, with its trailing backslash

    /// One distinct cookie file; several table rows may share it.
    struct Record
    {
        std::string file;
        int         cell = -1;
        bool        failed = false;
        float       score = 0.0f;
        uint32_t    wanted = 0;
        // What it averages to, as lights use it: from the manifest (1 - blocked, the recorded tint)
        // at load, eased towards the file's own measure once read (-1 unknown).
        float       open = -1.0f;
        float       tint[3] = { 1.0f, 1.0f, 1.0f }; // mean colour let through over its luma
        float       target = -1.0f;                 // the file's measured mean, once read
        float       targetTint[3] = { 1.0f, 1.0f, 1.0f };
        float       resident = 0.0f;                // 0..1: how far its pattern has faded in over its mean
        bool        releasing = false;              // its pattern fades out: its cell goes to another cookie after
        uint32_t    eased = 0;                      // the frame the above last moved
        uint32_t    measure = 0;                    // a read for its mean alone, without a cell (asset id)
        // What it is softened by: the glowing source's radius and how far the cage stands from it, in
        // the model's own units (-1 until a light using it or the manifest says).
        float       sourceRadius = -1.0f;
        float       cageDistance = -1.0f;
    };
    std::vector<Record>                  g_records;
    std::unordered_map<uint64_t, int>    g_recordOfLight;  // LightKey -> record
    std::unordered_map<std::string, int> g_recordOfFile;

    /// One key per light source: its kind, the hash of its file and its index within it (a table
    /// row, an M2 light, a MOLT entry), as the manifest's kind, model and light columns name it.
    uint64_t LightKey(gl::Kind kind, uint64_t source, uint32_t index)
    {
        const uint64_t k = kind == gl::Kind::Table ? 1 : (kind == gl::Kind::M2 ? 2 : (kind == gl::Kind::Wmo ? 3 : 0));
        return (source * 0x9E3779B97F4A7C15ull) ^ (uint64_t(index) << 8) ^ k;
    }

    /// The light's key, or 0 when it has no source the manifest could name.
    uint64_t KeyOf(const gl::Light& l)
    {
        if (!l.cookieSource) return 0;
        if (l.kind == gl::Kind::Table) return LightKey(gl::Kind::Table, l.cookieSource, l.index);
        if (l.kind == gl::Kind::M2 || l.kind == gl::Kind::Wmo) return LightKey(l.kind, l.cookieSource, l.index);
        return 0;
    }

    struct Cell
    {
        int      record = -1;
        uint32_t asset = 0;   // the file read in flight (asset id), 0 once copied in
        bool     filled = false;
    };
    std::vector<Cell>  g_cells;
    tex::Twin          g_atlas;
    tex::Twin          g_white;             // 1 x 1 L8 white: what consumers bind while there is no atlas
    unsigned           g_face = 64;         // texels per cube face, from the manifest
    bool               g_tinted = false;    // some cookie file holds colour: the atlas is X8R8G8B8
    int                g_atlasBudget = 0;   // cells the atlas was made for
    int                g_withCookie = 0, g_resident = 0, g_pending = 0;
    std::vector<int>   g_measuring;         // records with a mean read in flight
    std::vector<int>   g_touched, g_beyond; // this frame's wanted records, kept for their capacity
    std::vector<uint8_t> g_texels, g_lum, g_bgra;   // decode scratch, kept for its capacity
    std::vector<uint8_t> g_faces, g_soft;           // a cookie's six faces as read, and softened
    ck::BlurScratch    g_blur;
    float              g_softness = 1.0f;          // the softness the cells were filled with
    float              g_softnessWanted = 1.0f;    // the settings' value while it rests
    DWORD              g_softnessAt = 0;
    char               g_status[200] = "cookies: not loaded";
    IDirect3DPixelShader9* g_debugPs = nullptr;   // the atlas view; shaders survive a device reset
    bool               g_debugPsFailed = false;

    /// How far a family's cage, frame or bowl stands from its flame, in the model's units, until the
    /// bake records it (a manifest column cage_distance wins): what the source's size is seen against.
    float CageDistance(uint32_t family)
    {
        switch (family)
        {
        case WXL_GFX_LIGHT_FAMILY_LANTERN:
        case WXL_GFX_LIGHT_FAMILY_GREENLAMP: return 0.10f;
        case WXL_GFX_LIGHT_FAMILY_WALLLIGHT: return 0.12f;
        case WXL_GFX_LIGHT_FAMILY_CANDLE:    return 0.05f;
        case WXL_GFX_LIGHT_FAMILY_BRAZIER:
        case WXL_GFX_LIGHT_FAMILY_CAMPFIRE:
        case WXL_GFX_LIGHT_FAMILY_HEARTH:    return 0.3f;
        default:                             return 0.1f;
        }
    }

    /// A record's softening, from the first light found using it: a table light's row gives its
    /// source (unscaled, like the cookie's own frame), any other its family's soft core.
    void LearnShape(Record& r, const gl::Light& l)
    {
        float size = -1.0f;
        uint32_t family = l.family;
        if (l.kind == gl::Kind::Table) gl::table::SourceShape(l.cookieSource, l.index, size, family);
        if (!(size > 0.0f)) size = gl::families::Get(family).softRadius;
        r.sourceRadius = size;
        if (r.cageDistance <= 0.0f) r.cageDistance = CageDistance(family);
    }

    unsigned CellsDown() { return unsigned((std::max(g_cfg.budget, 1) + kCellsAcross - 1) / kCellsAcross); }
    unsigned AtlasW() { return kCellsAcross * 4 * g_face; }
    unsigned AtlasH() { return CellsDown() * 2 * g_face; }
    unsigned TexelBytes() { return g_tinted ? 4u : 1u; }

    const WXL_GraphicsExtendApi* Gfx() { return gl::Gfx(); }

    void Load(const WXL_GraphicsExtendApi* gfx)
    {
        g_loaded = true;
        g_manifest = gfx->ManifestLoad(kFolder, nullptr);
        if (!g_manifest)
        {
            LIGHTS_LOG_INFO("cookies: no manifest under %s; cookies stay off", kFolder);
            return;
        }
        std::snprintf(g_folder, sizeof g_folder, "%s", gfx->ManifestFolder(g_manifest));
        const int cModel = gfx->ManifestColumn(g_manifest, "model"), cLight = gfx->ManifestColumn(g_manifest, "light");
        const int cFile = gfx->ManifestColumn(g_manifest, "file"), cStatus = gfx->ManifestColumn(g_manifest, "status");
        const int cSize = gfx->ManifestColumn(g_manifest, "size"), cKind = gfx->ManifestColumn(g_manifest, "kind");
        const int cFormat = gfx->ManifestColumn(g_manifest, "format");
        const int cBlocked = gfx->ManifestColumn(g_manifest, "blocked");
        const int cCage = gfx->ManifestColumn(g_manifest, "cage_distance");   // none yet: the families' defaults
        const int cTint[3] = { gfx->ManifestColumn(g_manifest, "tint_r"), gfx->ManifestColumn(g_manifest, "tint_g"),
                               gfx->ManifestColumn(g_manifest, "tint_b") };
        if (cModel < 0 || cLight < 0 || cFile < 0 || cStatus < 0)
        {
            LIGHTS_LOG_WARN("cookies: manifest lacks the model, light, file or status column; cookies stay off");
            g_manifest = nullptr;
            return;
        }
        if (gfx->ManifestVersion(g_manifest) < 2)
            LIGHTS_LOG_WARN("cookies: manifest format %d; this build reads format 2 (rebake with 5.tools/forever-bake)",
                            gfx->ManifestVersion(g_manifest));
        size_t byKind[4] = {};
        bool anySize = false;
        const int rows = gfx->ManifestCount(g_manifest);
        for (int i = 0; i < rows; ++i)
        {
            if (std::strcmp(gfx->ManifestField(g_manifest, i, cStatus), "ok") != 0 || !*gfx->ManifestField(g_manifest, i, cFile)) continue;
            // kind: table (light = row index), m2 (L<light>), wmo (L<MOLT index>); emitter rows have
            // no runtime source yet.
            const char* kindName = cKind >= 0 ? gfx->ManifestField(g_manifest, i, cKind) : "table";
            const char* light = gfx->ManifestField(g_manifest, i, cLight);
            gl::Kind kind;
            if (std::strcmp(kindName, "table") == 0) kind = gl::Kind::Table;
            else if (std::strcmp(kindName, "m2") == 0) kind = gl::Kind::M2;
            else if (std::strcmp(kindName, "wmo") == 0) kind = gl::Kind::Wmo;
            else continue;
            if (*light == 'L' || *light == 'E') ++light;
            const uint32_t index = uint32_t(std::atoi(light));
            const uint64_t source = gl::table::StemHash(gfx->ManifestField(g_manifest, i, cModel), 512);
            const std::string file = gfx->ManifestField(g_manifest, i, cFile);
            auto it = g_recordOfFile.find(file);
            if (it == g_recordOfFile.end())
            {
                it = g_recordOfFile.emplace(file, int(g_records.size())).first;
                Record r;
                r.file = file;
                // The bake's own measure, so a light has its energy from its first frame.
                if (cBlocked >= 0)
                {
                    r.open = std::clamp(1.0f - gfx->ManifestNumber(g_manifest, i, cBlocked, 0.0f), 0.0f, 1.0f);
                    float t[3];
                    for (int k = 0; k < 3; ++k) t[k] = cTint[k] >= 0 ? std::max(gfx->ManifestNumber(g_manifest, i, cTint[k], 1.0f), 0.0f) : 1.0f;
                    const float luma = 0.299f * t[0] + 0.587f * t[1] + 0.114f * t[2];
                    for (int k = 0; k < 3; ++k) r.tint[k] = luma > 1e-4f ? t[k] / luma : 1.0f;
                }
                if (cCage >= 0) r.cageDistance = gfx->ManifestNumber(g_manifest, i, cCage, -1.0f);
                g_records.push_back(r);
            }
            g_recordOfLight[LightKey(kind, source, index)] = it->second;
            ++byKind[size_t(kind)];
            // The largest bake sets the cell; smaller cookies are resampled into it.
            if (cSize >= 0)
            {
                const unsigned s = unsigned(gfx->ManifestNumber(g_manifest, i, cSize, 64.0f));
                if (s >= 16 && s <= 256 && (s > g_face || !anySize)) g_face = s;
                anySize = true;
            }
            // A cookie baked with its glass's tint (format A8R8G8B8) asks for a colour atlas.
            if (cFormat >= 0 && std::strcmp(gfx->ManifestField(g_manifest, i, cFormat), "L8") != 0) g_tinted = true;
        }
        LIGHTS_LOG_INFO("cookies: %zu lights map to %zu cookies (%u per face, %s): %zu model table, %zu engine M2, %zu WMO",
                        g_recordOfLight.size(), g_records.size(), g_face, g_tinted ? "some tinted" : "luminance",
                        byKind[2], byKind[0], byKind[1]);
    }

    void ResetCells()
    {
        if (const WXL_GraphicsExtendApi* gfx = Gfx())
            for (Cell& c : g_cells)
                if (c.asset) gfx->AssetRelease(c.asset);
        g_cells.assign(size_t(std::max(g_cfg.budget, 1)), Cell{});
        for (Record& r : g_records)
        {
            r.cell = -1;
            r.releasing = false;
        }
    }

    /// The 1 x 1 white stand-in, DEFAULT pool like the atlas, so a consumer imports either the same way.
    void EnsureWhite(IDirect3DDevice9* dev)
    {
        if (g_white) return;
        if (!tex::Create(dev, 1, 1, D3DFMT_L8, g_white, "cookie stand-in")) return;
        int pitch = 0;
        if (uint8_t* bits = tex::Lock(g_white, nullptr, pitch))
        {
            bits[0] = 255;
            tex::Unlock(g_white);
            tex::Upload(dev, g_white);
        }
    }

    bool EnsureAtlas(IDirect3DDevice9* dev)
    {
        if (g_atlas && g_atlasBudget == g_cfg.budget) return true;
        tex::Release(g_atlas);
        ResetCells();
        g_atlasBudget = g_cfg.budget;
        g_softness = g_softnessWanted = g_cfg.softness;
        const D3DFORMAT format = g_tinted ? D3DFMT_X8R8G8B8 : D3DFMT_L8;
        if (!tex::Create(dev, AtlasW(), AtlasH(), format, g_atlas, "cookie atlas"))
        {
            LIGHTS_LOG_WARN("cookies: %ux%u %s atlas unavailable on this device; cookies stay off", AtlasW(), AtlasH(),
                            g_tinted ? "X8R8G8B8" : "L8");
            return false;
        }
        int pitch = 0;
        if (uint8_t* bits = tex::Lock(g_atlas, nullptr, pitch))
        {
            for (unsigned y = 0; y < AtlasH(); ++y) std::memset(bits + size_t(y) * size_t(pitch), 255, AtlasW() * TexelBytes());
            tex::Unlock(g_atlas);
            tex::Upload(dev, g_atlas);
        }
        LIGHTS_LOG_INFO("cookies: atlas %ux%u %s for %d cookies of %u per face (%u KB)", AtlasW(), AtlasH(), g_tinted ? "X8R8G8B8" : "L8",
                        g_cfg.budget, g_face, AtlasW() * AtlasH() * TexelBytes() / 1024);
        return true;
    }

    /// What a cookie averages to, summed face by face: the colour let through and its luma.
    struct Mean
    {
        double sum[3] = {}, luma = 0.0, count = 0.0;

        /// Adds one decoded face (BGRX when bpp is 4, else luminance).
        void Add(const std::vector<uint8_t>& texels, unsigned w, unsigned h, unsigned bpp)
        {
            for (size_t t = 0; t < size_t(w) * h; ++t)
            {
                const uint8_t* p = texels.data() + t * bpp;
                const double r = bpp == 4 ? p[2] : p[0], g = bpp == 4 ? p[1] : p[0], b = p[0];
                sum[0] += r; sum[1] += g; sum[2] += b;
                luma += 0.299 * r + 0.587 * g + 0.114 * b;
            }
            count += double(w) * h;
        }

        /// The mean transmittance, and the tint over it, luma-preserving.
        void Store(Record& record) const
        {
            if (count <= 0.0) return;
            record.target = float(luma / (count * 255.0));
            for (int k = 0; k < 3; ++k) record.targetTint[k] = luma > 1.0 ? float(sum[k] / luma) : 1.0f;
            if (record.open < 0.0f)
            {
                record.open = record.target;
                std::memcpy(record.tint, record.targetTint, sizeof record.tint);
            }
        }
    };

    bool ParseCube(const void* bytes, size_t size, const char* name, WXL_GfxDdsInfo& info)
    {
        info = WXL_GfxDdsInfo{};
        info.structSize = sizeof info;
        if (Gfx()->DdsParse(bytes, size, &info) && info.kind == WXL_GFX_DDS_CUBE) return true;
        LIGHTS_LOG_WARN("cookies: %s is not a cube DDS", name);
        return false;
    }

    /// A float format: wxl-forever read its first channel as the luminance, where the service's
    /// luminance decoder would weigh the red of (r, 0, 0) by 0.299.
    bool FloatFormat(uint32_t f)
    {
        return f == D3DFMT_R16F || f == D3DFMT_G16R16F || f == D3DFMT_A16B16G16R16F || f == D3DFMT_R32F || f == D3DFMT_G32R32F
            || f == D3DFMT_A32B32G32R32F;
    }

    /// One face as 8-bit luminance, exactly as wxl-forever's DecodeLuminance read it.
    bool DecodeLuma(const void* bytes, size_t size, const WXL_GfxDdsInfo& info, unsigned f, unsigned level,
                    std::vector<uint8_t>& out, unsigned& w, unsigned& h)
    {
        const WXL_GraphicsExtendApi* gfx = Gfx();
        uint32_t ww = 0, hh = 0;
        if (FloatFormat(info.fileFormat))
        {
            // out may be g_texels itself: the colour decode goes to its own scratch.
            g_bgra.clear();
            WXL_ByteSink sink = wxl::gfx::SinkTo(g_bgra);
            if (!gfx->DdsDecodeBgra(bytes, size, f, level, &sink, &ww, &hh) || g_bgra.size() != size_t(ww) * hh * 4) return false;
            out.resize(size_t(ww) * hh);
            for (size_t i = 0; i < out.size(); ++i) out[i] = g_bgra[i * 4 + 2];
        }
        else
        {
            out.clear();
            WXL_ByteSink sink = wxl::gfx::SinkTo(out);
            if (!gfx->DdsDecodeLuminance(bytes, size, f, level, &sink, &ww, &hh)) return false;
        }
        w = ww;
        h = hh;
        return out.size() == size_t(w) * h;
    }

    /// Decodes face f at level into texels as the atlas holds them: luminance, or for a tinted atlas
    /// wxl-forever's DecodeColour -- A8R8G8B8 and X8R8G8B8 as they are, every other format its
    /// luminance in all three channels (never the service's true-colour decode of them).
    bool DecodeFace(const void* bytes, size_t size, const WXL_GfxDdsInfo& info, unsigned f, unsigned level,
                    std::vector<uint8_t>& texels, unsigned& w, unsigned& h)
    {
        if (!g_tinted) return DecodeLuma(bytes, size, info, f, level, texels, w, h);
        if (info.fileFormat == D3DFMT_A8R8G8B8 || info.fileFormat == D3DFMT_X8R8G8B8)
        {
            texels.clear();
            WXL_ByteSink sink = wxl::gfx::SinkTo(texels);
            uint32_t ww = 0, hh = 0;
            if (!Gfx()->DdsDecodeBgra(bytes, size, f, level, &sink, &ww, &hh)) return false;
            w = ww;
            h = hh;
            return texels.size() == size_t(w) * h * 4;
        }
        if (!DecodeLuma(bytes, size, info, f, level, g_lum, w, h)) return false;
        texels.resize(g_lum.size() * 4);
        for (size_t i = 0; i < g_lum.size(); ++i)
        {
            texels[i * 4] = texels[i * 4 + 1] = texels[i * 4 + 2] = g_lum[i];
            texels[i * 4 + 3] = 255;
        }
        return true;
    }

    /// Measures what a cookie file averages to, without a cell; false when it is not a usable cube.
    bool MeasureFile(const void* bytes, size_t size, const char* name, Record& record)
    {
        WXL_GfxDdsInfo info;
        if (!ParseCube(bytes, size, name, info)) return false;
        // The smallest mip reads the same mean for less.
        const unsigned level = info.mips ? info.mips - 1 : 0;
        Mean mean;
        for (unsigned f = 0; f < 6; ++f)
        {
            unsigned w = 0, h = 0;
            if (!DecodeFace(bytes, size, info, f, level, g_texels, w, h)) return false;
            mean.Add(g_texels, w, h, TexelBytes());
        }
        mean.Store(record);
        return true;
    }

    /// Reads the six faces of a cookie file, softens them by the record's source (CookieBlur.hpp) at
    /// the cell's size, copies them into a cell of the twin, copies the cell up and measures what the
    /// file averages to; false when the file is not a usable cube.
    bool FillCell(IDirect3DDevice9* dev, int cell, const void* bytes, size_t size, const char* name, Record& record)
    {
        WXL_GfxDdsInfo info;
        if (!ParseCube(bytes, size, name, info)) return false;
        // The mip whose size is the cell's face, or the nearest smaller one; the blur brings it to the cell.
        unsigned level = 0;
        while (level + 1 < info.mips && (info.width >> level) > g_face) ++level;
        const unsigned bpp = TexelBytes();
        // All six faces first: the blur reads across their seams.
        Mean mean;
        unsigned side = 0;
        for (unsigned f = 0; f < 6; ++f)
        {
            unsigned w = 0, h = 0;
            if (!DecodeFace(bytes, size, info, f, level, g_texels, w, h)) return false;
            if (!w || w != h || (f && w != side))
            {
                LIGHTS_LOG_WARN("cookies: %s has cube faces that are not square or not alike", name);
                return false;
            }
            side = w;
            mean.Add(g_texels, w, h, bpp);
            g_faces.resize(size_t(6) * side * side * bpp);
            std::memcpy(g_faces.data() + size_t(f) * side * side * bpp, g_texels.data(), size_t(side) * side * bpp);
        }
        g_soft.resize(size_t(6) * g_face * g_face * bpp);
        const float theta = ck::SoftAngle(record.sourceRadius, record.cageDistance, g_softness, g_face);
        ck::BlurCube(g_faces.data(), side, bpp, theta, g_soft.data(), g_face, g_blur);

        const unsigned cx = unsigned(cell % kCellsAcross), cy = unsigned(cell / kCellsAcross);
        RECT cellRect{};
        cellRect.left = LONG(cx * 4 * g_face);
        cellRect.top = LONG(cy * 2 * g_face);
        cellRect.right = cellRect.left + LONG(4 * g_face);
        cellRect.bottom = cellRect.top + LONG(2 * g_face);
        int pitch = 0;
        uint8_t* base = tex::Lock(g_atlas, &cellRect, pitch);
        if (!base) return false;
        for (unsigned f = 0; f < 6; ++f)
        {
            uint8_t* face = base + size_t(f / 4) * g_face * size_t(pitch) + size_t(f % 4) * g_face * bpp;
            for (unsigned y = 0; y < g_face; ++y)
                std::memcpy(face + size_t(y) * size_t(pitch), g_soft.data() + (size_t(f) * g_face + y) * g_face * bpp, size_t(g_face) * bpp);
        }
        tex::Unlock(g_atlas);
        // Only this cell reaches the GPU: the rest of the atlas is as it was.
        tex::UploadRect(dev, g_atlas, cellRect);
        mean.Store(record);
        return true;
    }

    void RequestPath(char* out, size_t cap, const Record& r)
    {
        std::snprintf(out, cap, "%s%s", g_folder, r.file.c_str());
    }

    /// Reads the mean of wanted cookies that have no cell and were never loaded, a few at a time,
    /// so their light keeps its colour and energy before it is ever resident.
    void RequestMeans(const WXL_GraphicsExtendApi* gfx, const std::vector<int>& wanted)
    {
        for (int rec : wanted)
        {
            if (int(g_measuring.size()) >= kMeasuring) return;
            Record& r = g_records[size_t(rec)];
            if (r.open >= 0.0f || r.failed || r.cell >= 0 || r.measure) continue;
            char path[512];
            RequestPath(path, sizeof path, r);
            r.measure = gfx->AssetRequest(path, 10, WXL_GFX_ASSET_BYTES);
            if (r.measure) g_measuring.push_back(rec);
        }
    }

    /// Takes in the mean reads that finished.
    void PollMeans(const WXL_GraphicsExtendApi* gfx)
    {
        for (size_t i = 0; i < g_measuring.size();)
        {
            Record& r = g_records[size_t(g_measuring[i])];
            const uint32_t state = gfx->AssetState(r.measure);
            if (state == WXL_GFX_ASSET_PENDING) { ++i; continue; }
            if (state == WXL_GFX_ASSET_READY)
            {
                size_t size = 0;
                const void* bytes = gfx->AssetBytes(r.measure, &size);
                if (!bytes || !MeasureFile(bytes, size, r.file.c_str(), r)) r.failed = true;
            }
            else if (state == WXL_GFX_ASSET_FAILED) r.failed = true;
            gfx->AssetRelease(r.measure);
            r.measure = 0;
            g_measuring[i] = g_measuring.back();
            g_measuring.pop_back();
        }
    }

    void UpdateStatus()
    {
        std::snprintf(g_status, sizeof g_status, "cookies: %zu lights mapped to %zu cookies; %d lit with one now, %d resident, %d loading, %d measuring",
                      g_recordOfLight.size(), g_records.size(), g_withCookie, g_resident, g_pending, int(g_measuring.size()));
    }

    // The atlas drawn in a corner (debug view 1): the viewport's origin and size in pixels in c0,
    // the atlas on s0.
    constexpr const char kDebugSource[] =
        "sampler2D atlas : register(s0);\n"
        "float4 rect : register(c0);\n"
        "float4 main(float2 vpos : VPOS) : COLOR\n"
        "{\n"
        "    float2 uv = (vpos + 0.5 - rect.xy) / rect.zw;\n"
        "    return float4(tex2D(atlas, uv).rgb, 1.0);\n"
        "}\n";

    IDirect3DPixelShader9* DebugShader(IDirect3DDevice9* dev)
    {
        if (g_debugPs || g_debugPsFailed) return g_debugPs;
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (!gfx) return nullptr;
        WXL_GfxShaderDesc desc{};
        desc.structSize = sizeof desc;
        desc.name = "wxl-graphics-lights/cookiedebug.ps.hlsl";
        desc.source = kDebugSource;
        desc.entry = "main";
        desc.target = "ps_3_0";
        g_debugPs = static_cast<IDirect3DPixelShader9*>(gfx->CreatePixelShader(dev, &desc));
        g_debugPsFailed = g_debugPs == nullptr;
        return g_debugPs;
    }
}

namespace wxl::gfx::lights::cookies
{
    Settings& Get() { return g_cfg; }

    void Install()
    {
        g_cfg.enabled  = ConfigBool("WXL_GFX_LIGHTS_COOKIES_ENABLED", true) ? 1 : 0;
        g_cfg.strength = ConfigFloat("WXL_GFX_LIGHTS_COOKIES_STRENGTH", g_cfg.strength, 0.0f, 1.0f);
        g_cfg.floor    = ConfigFloat("WXL_GFX_LIGHTS_COOKIES_FLOOR", g_cfg.floor, 0.0f, 0.5f);
        g_cfg.flame    = ConfigFloat("WXL_GFX_LIGHTS_COOKIES_FLAME", g_cfg.flame, 0.0f, 1.0f);
        g_cfg.tint     = ConfigFloat("WXL_GFX_LIGHTS_COOKIES_TINT", g_cfg.tint, 0.0f, 1.0f);
        g_cfg.softness = ConfigFloat("WXL_GFX_LIGHTS_COOKIES_SOFTNESS", g_cfg.softness, 0.0f, 2.0f);
        g_softness = g_softnessWanted = g_cfg.softness;
        g_cfg.budget   = ConfigInt("WXL_GFX_LIGHTS_COOKIES_BUDGET", g_cfg.budget, 4, kMaxBudget);
        g_cfg.debug    = ConfigInt("WXL_GFX_LIGHTS_COOKIES_DEBUG", 0, 0, 2);
        LIGHTS_LOG_INFO("cookies: %s, strength %.2f, floor %.2f, flame %.2f, tint %.2f, softness %.2f, budget %d",
                        g_cfg.enabled ? "on" : "off", g_cfg.strength, g_cfg.floor, g_cfg.flame, g_cfg.tint, g_cfg.softness,
                        g_cfg.budget);
    }

    void Frame(IDirect3DDevice9* dev, Light* lights, int count, const float eye[3], uint32_t frame)
    {
        for (int i = 0; i < count; ++i) lights[i].cookieCell = 0;
        g_withCookie = 0;
        if (dev) EnsureWhite(dev);
        const WXL_GraphicsExtendApi* gfx = Gfx();
        if (!g_cfg.enabled || !dev || !lights || !gfx) { UpdateStatus(); return; }
        if (!g_loaded) Load(gfx);
        if (g_records.empty() || !EnsureAtlas(dev)) { UpdateStatus(); return; }
        // A new softness reads every cookie again, once the slider has rested: the blur is baked into
        // the cells.
        if (g_cfg.softness != g_softness)
        {
            if (g_cfg.softness != g_softnessWanted)
            {
                g_softnessWanted = g_cfg.softness;
                g_softnessAt = GetTickCount();
            }
            else if (GetTickCount() - g_softnessAt > kSoftnessRest)
            {
                g_softness = g_cfg.softness;
                ResetCells();
            }
        }

        // Which cookies the frame wants, by the importance of the brightest light using each.
        std::vector<int>& touched = g_touched;
        touched.clear();
        for (int i = 0; i < count; ++i)
        {
            const Light& l = lights[i];
            const uint64_t key = KeyOf(l);
            if (!key) continue;
            const auto it = g_recordOfLight.find(key);
            if (it == g_recordOfLight.end()) continue;
            Record& r = g_records[size_t(it->second)];
            if (r.failed) continue;
            const float dx = l.position[0] - eye[0], dy = l.position[1] - eye[1], dz = l.position[2] - eye[2];
            const float luma = (l.color[0] * 0.299f + l.color[1] * 0.587f + l.color[2] * 0.114f) * l.intensity;
            const float d2 = dx * dx + dy * dy + dz * dz;
            // Lamps within kPinYards stay resident whatever else wants a cell: they are what the
            // player sees shaped, and a cookie reloading would show the lamp unshaped meanwhile.
            const float score = luma / (1.0f + d2 / std::max(l.radius * l.radius, 1.0f)) + (d2 < kPinYards * kPinYards ? 1000.0f : 0.0f);
            if (r.score <= 0.0f) touched.push_back(it->second);
            r.score = std::max(r.score, score + 1e-6f);
            if (r.sourceRadius < 0.0f) LearnShape(r, l);
        }
        std::sort(touched.begin(), touched.end(), [](int a, int b) { return g_records[size_t(a)].score > g_records[size_t(b)].score; });
        // Past the budget a cookie gets no cell, only its mean (RequestMeans, below).
        std::vector<int>& beyond = g_beyond;
        beyond.clear();
        if (touched.size() > size_t(g_cfg.budget))
        {
            beyond.assign(touched.begin() + g_cfg.budget, touched.end());
            touched.resize(size_t(g_cfg.budget));
        }
        for (int rec : touched) g_records[size_t(rec)].wanted = frame;

        // Give every wanted cookie a cell: an empty one, else the one least recently wanted. A pattern
        // some lamp still shows fades out over its mean first (releasing), and its cell changes hands
        // once it is gone, so no lamp's pattern snaps to its mean. A wanted cookie keeps its cell.
        int releasing = 0;
        for (int rec : touched) g_records[size_t(rec)].releasing = false;
        for (const Cell& cell : g_cells)
            if (cell.record >= 0 && g_records[size_t(cell.record)].releasing) ++releasing;
        for (size_t rank = 0; rank < touched.size(); ++rank)
        {
            Record& r = g_records[size_t(touched[rank])];
            if (r.cell >= 0) continue;
            int best = -1, fading = -1;
            uint32_t oldest = frame, oldestShown = frame;
            for (size_t c = 0; c < g_cells.size(); ++c)
            {
                const Cell& cell = g_cells[c];
                if (cell.record < 0) { best = int(c); break; }
                const Record& o = g_records[size_t(cell.record)];
                const uint32_t w = o.wanted;
                if (w == frame || frame - w <= kKeepFrames) continue;
                // Shown last frame with some of its pattern: not free until its fade has run.
                const bool shown = o.resident > 0.0f && frame - o.eased <= 1;
                if (!shown && w < oldest) { oldest = w; best = int(c); }
                if (shown && !o.releasing && w < oldestShown) { oldestShown = w; fading = int(c); }
            }
            if (best < 0)
            {
                // A fade already running serves this cookie; else the oldest pattern shown starts one.
                if (releasing > 0) --releasing;
                else if (fading >= 0) g_records[size_t(g_cells[size_t(fading)].record)].releasing = true;
                else break;
                continue;
            }
            Cell& cell = g_cells[size_t(best)];
            if (cell.record >= 0)
            {
                Record& old = g_records[size_t(cell.record)];
                old.cell = -1;
                old.releasing = false;
                old.resident = 0.0f;
            }
            if (cell.asset) gfx->AssetRelease(cell.asset);
            cell = Cell{};
            cell.record = touched[rank];
            char path[512];
            RequestPath(path, sizeof path, r);
            cell.asset = gfx->AssetRequest(path, 100 - int(rank), WXL_GFX_ASSET_BYTES);
            r.cell = best;
        }
        for (Record& r : g_records) r.score = 0.0f;
        RequestMeans(gfx, touched);   // a wanted cookie no cell could take this frame
        RequestMeans(gfx, beyond);

        // The service streams the reads itself (its OnEndScene pump); here the finished ones are
        // taken in.
        PollMeans(gfx);

        // Copy finished reads into their cells, kFillsPerFrame at a time: each is decoded and softened
        // here, on the render thread; the rest wait, read, for the next frames.
        g_resident = g_pending = 0;
        int fills = 0;
        for (size_t c = 0; c < g_cells.size(); ++c)
        {
            Cell& cell = g_cells[c];
            if (cell.record < 0) continue;
            if (cell.filled) { ++g_resident; continue; }
            const uint32_t state = gfx->AssetState(cell.asset);
            if (state == WXL_GFX_ASSET_READY && fills >= kFillsPerFrame) ++g_pending;
            else if (state == WXL_GFX_ASSET_READY)
            {
                ++fills;
                size_t size = 0;
                const void* bytes = gfx->AssetBytes(cell.asset, &size);
                Record& r = g_records[size_t(cell.record)];
                if (bytes && FillCell(dev, int(c), bytes, size, r.file.c_str(), r)) { cell.filled = true; ++g_resident; }
                else { r.failed = true; r.cell = -1; gfx->AssetRelease(cell.asset); cell = Cell{}; continue; }
                gfx->AssetRelease(cell.asset);
                cell.asset = 0;
            }
            else if (state == WXL_GFX_ASSET_FAILED || state == WXL_GFX_ASSET_NONE)
            {
                Record& r = g_records[size_t(cell.record)];
                r.failed = true;
                r.cell = -1;
                gfx->AssetRelease(cell.asset);
                cell = Cell{};
            }
            else if (state == WXL_GFX_ASSET_EVICTED)
            {
                // Read again: the request was dropped for room before it was copied in.
                char path[512];
                RequestPath(path, sizeof path, g_records[size_t(cell.record)]);
                cell.asset = gfx->AssetRequest(path, 50, WXL_GFX_ASSET_BYTES);
                ++g_pending;
            }
            else ++g_pending;
        }

        LARGE_INTEGER counter{}, frequency{};
        QueryPerformanceCounter(&counter);
        QueryPerformanceFrequency(&frequency);
        static LONGLONG last = 0;
        const float dt = last ? std::min(float(double(counter.QuadPart - last) / double(frequency.QuadPart)), 0.25f) : 0.0f;
        last = counter.QuadPart;
        const float step = dt / kEaseSeconds;
        for (int i = 0; i < count; ++i)
        {
            Light& l = lights[i];
            const uint64_t key = KeyOf(l);
            if (!key) continue;
            const auto it = g_recordOfLight.find(key);
            if (it == g_recordOfLight.end()) continue;
            Record& r = g_records[size_t(it->second)];
            const bool filled = r.cell >= 0 && g_cells[size_t(r.cell)].filled;
            if (r.eased != frame)
            {
                // Nothing jumps: the pattern fades in over the mean once resident, and out again before
                // its cell goes to another cookie (releasing); the manifest's mean eases to the file's.
                r.eased = frame;
                r.resident = !filled ? 0.0f : (r.releasing ? std::max(r.resident - step, 0.0f) : std::min(r.resident + step, 1.0f));
                if (r.target >= 0.0f)
                {
                    const float a = std::min(step, 1.0f);
                    r.open += (r.target - r.open) * a;
                    for (int k = 0; k < 3; ++k) r.tint[k] += (r.targetTint[k] - r.tint[k]) * a;
                }
            }
            // The glass tint, by how much of it the settings let through.
            const float share = std::clamp(g_cfg.tint, 0.0f, 1.0f);
            l.cookieOpen = r.open;
            for (int k = 0; k < 3; ++k) l.cookieTint[k] = 1.0f + (r.tint[k] - 1.0f) * share;
            if (filled)
            {
                // Cell + 1, plus 128 times the pattern's share on 8 bits (cookies.hlsli).
                l.cookieCell = uint16_t((r.cell + 1) + 128 * int(r.resident * 255.0f + 0.5f));
                ++g_withCookie;
            }
        }
        UpdateStatus();
    }

    IDirect3DTexture9* Atlas()
    {
        return g_atlas ? g_atlas.gpu : g_white.gpu;
    }

    void Constants(float cookieC[4], float cookieD[4], float cookieE[4])
    {
        const float w = float(AtlasW()), h = float(AtlasH());
        cookieC[0] = float(g_face) / w;
        cookieC[1] = float(g_face) / h;
        cookieC[2] = g_cfg.enabled && g_atlas ? std::clamp(g_cfg.strength, 0.0f, 1.0f) : 0.0f;
        cookieC[3] = std::clamp(g_cfg.floor, 0.0f, 1.0f);
        cookieD[0] = 0.5f / w;
        cookieD[1] = 0.5f / h;
        cookieD[2] = g_cfg.debug == 2 ? 1.0f : 0.0f;
        cookieD[3] = 1.0f - std::clamp(g_cfg.flame, 0.0f, 1.0f);   // spared share: 0 gives the full pattern
        cookieE[0] = std::clamp(g_cfg.tint, 0.0f, 1.0f);
        cookieE[1] = cookieE[2] = cookieE[3] = 0.0f;
    }

    unsigned FaceTexels() { return g_face; }

    const char* Status() { return g_status; }

    void Panel()
    {
        ui::Text(g_status);
        if (const WXL_GraphicsExtendApi* gfx = Gfx()) ui::Text(gfx->AssetStatus());
        else ui::Text("baked assets: wxl-graphics-extend is not loaded; no cookie can be read");
        ui::Separator();
        ui::Check("Cookies", &g_cfg.enabled,
                  "Lanterns, lamps and braziers throw the shadow of their own cage and frame: a baked pattern per model light (Textures/Forever/Cookies). Off leaves every light smooth.");
        ui::Slider("Cookie strength", &g_cfg.strength, 0.0f, 1.0f,
                   "How strongly the pattern shapes the light: 1 the baked transmittance, 0 no effect. Lower it if lamp patterns look too hard.");
        ui::Slider("Cookie floor", &g_cfg.floor, 0.0f, 0.5f,
                   "The least a pattern lets through in any direction, for the light that leaks and bounces inside a fixture. 0.2 keeps a lamp's cap and base from blacking out the wall beside it. Lower it for struts and pane grids that read harder on walls and in the fog; 0 lets struts block completely.");
        if (ui::Slider("Cookie softness", &g_cfg.softness, 0.0f, 2.0f,
                       "How soft the shadows of a lamp's own frame are. A flame or bulb is no point: a bar at distance b from a source of radius r throws a penumbra about 2 r / b radians wide (at most 0.6), where the bake drew a hard edge as seen from a point. 1 gives that penumbra, 0 the bake's own edges (one texel of smoothing), 2 twice as soft. Every cookie is read again once the slider rests."))
            g_cfg.softness = std::clamp(g_cfg.softness, 0.0f, 2.0f);
        ui::Slider("Cookie on flames", &g_cfg.flame, 0.0f, 1.0f,
                   "How much of its pattern a flame light (torch, brazier, campfire, hearth, candle) takes. A flame is a volume, not the point the pattern was baked from, so its logs or bowl hide far less of it. 1 the full pattern.");
        ui::Slider("Cookie glass tint", &g_cfg.tint, 0.0f, 1.0f,
                   "How much of a tinted cookie's colour (green or amber panes, baked from the glass's texture) the lamp's light takes, pane by pane, on surfaces and in the fog. 0 uses the pattern's brightness only. Needs a bake with colour.");
        char help[360];
        std::snprintf(help, sizeof help, "How many distinct cookies stay loaded at once: those of lamps within 30 yards first, and kept, then the nearest and brightest. Each costs one atlas cell (%u KB at %u per face). Past it a lamp keeps its cookie's colour and brightness, without the bars. Changing it rebuilds the atlas.",
                      8 * g_face * g_face * TexelBytes() / 1024, g_face);
        if (ui::Slider("Resident cookies", &g_cfg.budget, 4, kMaxBudget, help))
            g_cfg.budget = std::clamp(g_cfg.budget, 4, kMaxBudget);
        static const char* const kViews[] = { "Off", "Atlas in a corner", "Cookie factor" };
        ui::Combo("Cookie debug view", &g_cfg.debug, kViews, 3,
                  "Atlas in a corner draws the loaded cookies (each a 4 x 2 grid of cube faces). Cookie factor asks the lighting shader to show the pattern factor instead of the lit scene, where it supports it.");
    }

    void DrawDebug(IDirect3DDevice9* d)
    {
        namespace render = wxl::gfx::render;
        if (g_cfg.debug != 1 || !g_cfg.enabled || !d || !g_atlas) return;
        const WXL_GraphicsExtendApi* gfx = Gfx();
        IDirect3DPixelShader9* ps = DebugShader(d);
        auto* vs = gfx ? static_cast<IDirect3DVertexShader9*>(gfx->FullscreenVertexShader(d)) : nullptr;
        if (!ps || !vs) return;
        IDirect3DSurface9* rt = nullptr;
        if (FAILED(d->GetRenderTarget(0, &rt)) || !rt) return;
        D3DSURFACE_DESC desc{};
        rt->GetDesc(&desc);
        rt->Release();
        const UINT w = std::max(desc.Width / 3, 64u), h = std::max(w * AtlasH() / AtlasW(), 32u);
        render::StateGuard guard(d, 1);
        render::PlainState(d);
        d->SetVertexShader(vs);
        d->SetPixelShader(ps);
        render::Sampler(d, 0, g_atlas.gpu, false);
        const float rect[4] = { 8.0f, 8.0f, float(w), float(h) };
        d->SetPixelShaderConstantF(0, rect, 1);
        const D3DVIEWPORT9 vp{ 8, 8, w, h, 0.0f, 1.0f };
        d->SetViewport(&vp);
        static const float kQuad[4][4] = { { -1, 1, 0, 1 }, { 1, 1, 0, 1 }, { -1, -1, 0, 1 }, { 1, -1, 0, 1 } };
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, kQuad, sizeof kQuad[0]);
    }

    void ReleaseTextures()
    {
        tex::Release(g_atlas);
        tex::Release(g_white);
        ResetCells();
    }
}
