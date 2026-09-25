// wxl-forever lights: light cookies, the shadow a lantern's own cage throws, baked per model light.
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
#include "../core/BakedAssets.hpp"
#include "../core/Dds.hpp"
#include "../core/Panel.hpp"
#include "../core/RenderUtil.hpp"
#include "../core/ShaderLibrary.hpp"
#include "Cookies.hpp"
#include "ModelTable.hpp"

#include "wxl/EventScript.hpp"

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
    namespace fl = wxl::forever::lights;
    namespace ck = wxl::forever::lights::cookies;
    namespace bk = wxl::forever::baked;
    namespace ui = wxl::forever::ui;
    namespace ev = wxl::events;

    constexpr int      kCellsAcross = 4;    // cookies per atlas row, kCookiesAcross in shaders/cookies.hlsli; each 4 x 2 faces
    constexpr int      kMaxBudget   = 64;   // atlas cells at most: 16 rows of 2 faces, 4096 texels at 128 a face
    constexpr uint32_t kKeepFrames  = 120;  // a cell stays this long after its cookie was last wanted
    constexpr float    kPinYards    = 30.0f; // lamps this near keep their cookie resident first
    constexpr int      kMeasuring   = 4;    // cookie reads in flight only to learn their mean
    constexpr float    kEaseSeconds = 0.5f; // a cookie's pattern fades in, and its mean moves, over this

    ck::Settings g_cfg;
    bool         g_loaded = false;
    bk::Manifest g_manifest;

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
        uint32_t    eased = 0;                      // the frame the above last moved
        bk::AssetId measure = 0;                    // a read for its mean alone, without a cell
    };
    std::vector<Record>                  g_records;
    std::unordered_map<uint64_t, int>    g_recordOfLight;  // LightKey -> record
    std::unordered_map<std::string, int> g_recordOfFile;

    /// One key per light source: its kind, the hash of its file and its index within it (a table
    /// row, an M2 light, a MOLT entry), as the manifest's kind, model and light columns name it.
    uint64_t LightKey(fl::Kind kind, uint64_t source, uint32_t index)
    {
        const uint64_t k = kind == fl::Kind::Table ? 1 : (kind == fl::Kind::M2 ? 2 : (kind == fl::Kind::Wmo ? 3 : 0));
        return (source * 0x9E3779B97F4A7C15ull) ^ (uint64_t(index) << 8) ^ k;
    }

    /// The light's key, or 0 when it has no source the manifest could name.
    uint64_t KeyOf(const fl::Light& l)
    {
        if (!l.cookieSource) return 0;
        if (l.kind == fl::Kind::Table) return LightKey(fl::Kind::Table, l.cookieSource, l.index);
        if (l.kind == fl::Kind::M2 || l.kind == fl::Kind::Wmo) return LightKey(l.kind, l.cookieSource, l.index);
        return 0;
    }

    struct Cell
    {
        int         record = -1;
        bk::AssetId asset = 0;
        bool        filled = false;
    };
    std::vector<Cell>  g_cells;
    IDirect3DTexture9* g_atlas = nullptr;
    unsigned           g_face = 64;         // texels per cube face, from the manifest
    bool               g_tinted = false;    // some cookie file holds colour: the atlas is X8R8G8B8
    int                g_atlasBudget = 0;   // cells the atlas was made for
    int                g_withCookie = 0, g_resident = 0, g_pending = 0;
    std::vector<int>   g_measuring;         // records with a mean read in flight
    char               g_status[200] = "cookies: not loaded";

    unsigned CellsDown() { return unsigned((std::max(g_cfg.budget, 1) + kCellsAcross - 1) / kCellsAcross); }
    unsigned AtlasW() { return kCellsAcross * 4 * g_face; }
    unsigned AtlasH() { return CellsDown() * 2 * g_face; }
    unsigned TexelBytes() { return g_tinted ? 4u : 1u; }

    void Load()
    {
        g_loaded = true;
        if (!g_manifest.Load("Cookies")) return;
        const int cModel = g_manifest.Column("model"), cLight = g_manifest.Column("light"), cFile = g_manifest.Column("file");
        const int cStatus = g_manifest.Column("status"), cSize = g_manifest.Column("size"), cKind = g_manifest.Column("kind");
        const int cFormat = g_manifest.Column("format");
        const int cBlocked = g_manifest.Column("blocked");
        const int cTint[3] = { g_manifest.Column("tint_r"), g_manifest.Column("tint_g"), g_manifest.Column("tint_b") };
        if (cModel < 0 || cLight < 0 || cFile < 0 || cStatus < 0)
        {
            WLOG_WARN("cookies: manifest lacks the model, light, file or status column; cookies stay off");
            return;
        }
        if (g_manifest.Version() < 2)
            WLOG_WARN("cookies: manifest format %d; this build reads format 2 (rebake with 5.tools/forever-bake)", g_manifest.Version());
        size_t byKind[4] = {};
        bool anySize = false;
        for (int i = 0; i < int(g_manifest.Count()); ++i)
        {
            if (std::strcmp(g_manifest.Field(i, cStatus), "ok") != 0 || !*g_manifest.Field(i, cFile)) continue;
            // kind: table (light = row index), m2 (L<light>), wmo (L<MOLT index>); emitter rows have
            // no runtime source yet.
            const char* kindName = cKind >= 0 ? g_manifest.Field(i, cKind) : "table";
            const char* light = g_manifest.Field(i, cLight);
            fl::Kind kind;
            if (std::strcmp(kindName, "table") == 0) kind = fl::Kind::Table;
            else if (std::strcmp(kindName, "m2") == 0) kind = fl::Kind::M2;
            else if (std::strcmp(kindName, "wmo") == 0) kind = fl::Kind::Wmo;
            else continue;
            if (*light == 'L' || *light == 'E') ++light;
            const uint32_t index = uint32_t(std::atoi(light));
            const uint64_t source = fl::table::StemHash(g_manifest.Field(i, cModel), 512);
            const std::string file = g_manifest.Field(i, cFile);
            auto it = g_recordOfFile.find(file);
            if (it == g_recordOfFile.end())
            {
                it = g_recordOfFile.emplace(file, int(g_records.size())).first;
                Record r{ file };
                // The bake's own measure, so a light has its energy from its first frame.
                if (cBlocked >= 0)
                {
                    r.open = std::clamp(1.0f - g_manifest.Number(i, cBlocked, 0.0f), 0.0f, 1.0f);
                    float t[3];
                    for (int k = 0; k < 3; ++k) t[k] = cTint[k] >= 0 ? std::max(g_manifest.Number(i, cTint[k], 1.0f), 0.0f) : 1.0f;
                    const float luma = 0.299f * t[0] + 0.587f * t[1] + 0.114f * t[2];
                    for (int k = 0; k < 3; ++k) r.tint[k] = luma > 1e-4f ? t[k] / luma : 1.0f;
                }
                g_records.push_back(r);
            }
            g_recordOfLight[LightKey(kind, source, index)] = it->second;
            ++byKind[size_t(kind)];
            // The largest bake sets the cell; smaller cookies are resampled into it.
            if (cSize >= 0)
            {
                const unsigned s = unsigned(g_manifest.Number(i, cSize, 64.0f));
                if (s >= 16 && s <= 256 && (s > g_face || !anySize)) g_face = s;
                anySize = true;
            }
            // A cookie baked with its glass's tint (format A8R8G8B8) asks for a colour atlas.
            if (cFormat >= 0 && std::strcmp(g_manifest.Field(i, cFormat), "L8") != 0) g_tinted = true;
        }
        WLOG_INFO("cookies: %zu lights map to %zu cookies (%u per face, %s): %zu model table, %zu engine M2, %zu WMO",
                  g_recordOfLight.size(), g_records.size(), g_face, g_tinted ? "some tinted" : "luminance",
                  byKind[2], byKind[0], byKind[1]);
    }

    void ResetCells()
    {
        for (Cell& c : g_cells)
            if (c.asset) bk::Release(c.asset);
        g_cells.assign(size_t(std::max(g_cfg.budget, 1)), Cell{});
        for (Record& r : g_records) r.cell = -1;
    }

    bool EnsureAtlas(IDirect3DDevice9* dev)
    {
        if (g_atlas && g_atlasBudget == g_cfg.budget) return true;
        if (g_atlas) { g_atlas->Release(); g_atlas = nullptr; }
        ResetCells();
        g_atlasBudget = g_cfg.budget;
        const D3DFORMAT format = g_tinted ? D3DFMT_X8R8G8B8 : D3DFMT_L8;
        if (FAILED(dev->CreateTexture(AtlasW(), AtlasH(), 1, 0, format, D3DPOOL_MANAGED, &g_atlas, nullptr)))
        {
            WLOG_WARN("cookies: %ux%u %s atlas unavailable on this device; cookies stay off", AtlasW(), AtlasH(),
                      g_tinted ? "X8R8G8B8" : "L8");
            g_atlas = nullptr;
            return false;
        }
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(g_atlas->LockRect(0, &lr, nullptr, 0)))
        {
            for (unsigned y = 0; y < AtlasH(); ++y)
                std::memset(static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch, 255, AtlasW() * TexelBytes());
            g_atlas->UnlockRect(0);
        }
        WLOG_INFO("cookies: atlas %ux%u %s for %d cookies of %u per face (%u KB)", AtlasW(), AtlasH(), g_tinted ? "X8R8G8B8" : "L8",
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

    bool ParseCube(const std::string& bytes, const char* name, wxl::forever::dds::Info& info)
    {
        using namespace wxl::forever::dds;
        if (Parse(bytes, info) && info.kind == Kind::Cube) return true;
        WLOG_WARN("cookies: %s is not a cube DDS", name);
        return false;
    }

    /// Decodes face f at level into texels as the atlas holds them.
    bool DecodeFace(const std::string& bytes, const wxl::forever::dds::Info& info, unsigned f, unsigned level,
                    std::vector<uint8_t>& texels, unsigned& w, unsigned& h)
    {
        using namespace wxl::forever::dds;
        return g_tinted ? DecodeColour(bytes, info, f, level, texels, w, h) : DecodeLuminance(bytes, info, f, level, texels, w, h);
    }

    /// Measures what a cookie file averages to, without a cell; false when it is not a usable cube.
    bool MeasureFile(const std::string& bytes, const char* name, Record& record)
    {
        wxl::forever::dds::Info info{};
        if (!ParseCube(bytes, name, info)) return false;
        // The smallest mip reads the same mean for less.
        const unsigned level = info.mips ? info.mips - 1 : 0;
        std::vector<uint8_t> texels;
        Mean mean;
        for (unsigned f = 0; f < 6; ++f)
        {
            unsigned w = 0, h = 0;
            if (!DecodeFace(bytes, info, f, level, texels, w, h)) return false;
            mean.Add(texels, w, h, TexelBytes());
        }
        mean.Store(record);
        return true;
    }

    /// Copies the six faces of a cookie file into a cell and measures what it averages to; false
    /// when the file is not a usable cube.
    bool FillCell(int cell, const std::string& bytes, const char* name, Record& record)
    {
        wxl::forever::dds::Info info{};
        if (!ParseCube(bytes, name, info)) return false;
        // The mip whose size is the cell's face, or the nearest smaller one, resampled if needed.
        unsigned level = 0;
        while (level + 1 < info.mips && (info.width >> level) > g_face) ++level;
        const unsigned cx = unsigned(cell % kCellsAcross), cy = unsigned(cell / kCellsAcross);
        const unsigned bpp = TexelBytes();
        std::vector<uint8_t> texels;
        Mean mean;
        for (unsigned f = 0; f < 6; ++f)
        {
            unsigned w = 0, h = 0;
            if (!DecodeFace(bytes, info, f, level, texels, w, h)) return false;
            mean.Add(texels, w, h, bpp);
            RECT r{};
            r.left = LONG(cx * 4 * g_face + (f % 4) * g_face);
            r.top = LONG(cy * 2 * g_face + (f / 4) * g_face);
            r.right = r.left + LONG(g_face);
            r.bottom = r.top + LONG(g_face);
            D3DLOCKED_RECT lr{};
            if (FAILED(g_atlas->LockRect(0, &lr, &r, 0))) return false;
            for (unsigned y = 0; y < g_face; ++y)
            {
                uint8_t* dst = static_cast<uint8_t*>(lr.pBits) + size_t(y) * lr.Pitch;
                const unsigned sy = std::min(y * h / g_face, h - 1);
                if (w == g_face) std::memcpy(dst, texels.data() + size_t(sy) * w * bpp, size_t(w) * bpp);
                else
                    for (unsigned x = 0; x < g_face; ++x)
                        std::memcpy(dst + size_t(x) * bpp, texels.data() + (size_t(sy) * w + std::min(x * w / g_face, w - 1)) * bpp, bpp);
            }
            g_atlas->UnlockRect(0);
        }
        mean.Store(record);
        return true;
    }

    /// Reads the mean of wanted cookies that have no cell and were never loaded, a few at a time,
    /// so their light keeps its colour and energy before it is ever resident.
    void RequestMeans(const std::vector<int>& wanted)
    {
        for (int rec : wanted)
        {
            if (int(g_measuring.size()) >= kMeasuring) return;
            Record& r = g_records[size_t(rec)];
            if (r.open >= 0.0f || r.failed || r.cell >= 0 || r.measure) continue;
            r.measure = bk::Request(g_manifest.Folder() + r.file, 10, bk::kWantBytes);
            if (r.measure) g_measuring.push_back(rec);
        }
    }

    /// Takes in the mean reads that finished.
    void PollMeans()
    {
        for (size_t i = 0; i < g_measuring.size();)
        {
            Record& r = g_records[size_t(g_measuring[i])];
            const bk::State state = bk::StateOf(r.measure);
            if (state == bk::State::Pending) { ++i; continue; }
            if (state == bk::State::Ready)
            {
                const std::string* bytes = bk::Bytes(r.measure);
                if (!bytes || !MeasureFile(*bytes, r.file.c_str(), r)) r.failed = true;
            }
            else if (state == bk::State::Failed) r.failed = true;
            bk::Release(r.measure);
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

    /// Draws the atlas in the top-left corner (debug view 1).
    void DrawAtlas(IDirect3DDevice9* d)
    {
        namespace render = wxl::forever::render;
        namespace shaders = wxl::forever::shaders;
        IDirect3DPixelShader9* ps = shaders::Pixel(d, "lights.cookiedebug");
        IDirect3DVertexShader9* vs = shaders::Vertex(d, "core.fullscreen");
        if (!ps || !vs || !g_atlas) return;
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
        render::Sampler(d, 0, g_atlas, false);
        const float rect[4] = { 8.0f, 8.0f, float(w), float(h) };
        d->SetPixelShaderConstantF(0, rect, 1);
        const D3DVIEWPORT9 vp{ 8, 8, w, h, 0.0f, 1.0f };
        d->SetViewport(&vp);
        const float quad[4][4] = { { -1, 1, 0, 1 }, { 1, 1, 0, 1 }, { -1, -1, 0, 1 }, { 1, -1, 0, 1 } };
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0]);
    }

    class CookieDebug final : public wxl::ext::EventScript
    {
    public:
        CookieDebug() { on<&CookieDebug::OnWorldRenderEnd>(ev::Event::OnWorldRenderEnd); }

        void OnWorldRenderEnd(const ev::WorldRenderEndArgs& a)
        {
            if (g_cfg.debug == 1 && g_cfg.enabled && a.device) DrawAtlas(static_cast<IDirect3DDevice9*>(a.device));
        }
    };
}

namespace wxl::forever::lights::cookies
{
    Settings& Get() { return g_cfg; }

    void Install()
    {
        g_cfg.enabled  = wxl_forever::ConfigBool("WXL_FOREVER_COOKIES_ENABLED", true) ? 1 : 0;
        g_cfg.strength = wxl_forever::ConfigFloat("WXL_FOREVER_COOKIES_STRENGTH", g_cfg.strength, 0.0f, 1.0f);
        g_cfg.floor    = wxl_forever::ConfigFloat("WXL_FOREVER_COOKIES_FLOOR", g_cfg.floor, 0.0f, 0.5f);
        g_cfg.flame    = wxl_forever::ConfigFloat("WXL_FOREVER_COOKIES_FLAME", g_cfg.flame, 0.0f, 1.0f);
        g_cfg.tint     = wxl_forever::ConfigFloat("WXL_FOREVER_COOKIES_TINT", g_cfg.tint, 0.0f, 1.0f);
        g_cfg.budget   = int(wxl_forever::ConfigFloat("WXL_FOREVER_COOKIES_BUDGET", float(g_cfg.budget), 4.0f, float(kMaxBudget)));
        g_cfg.debug    = int(wxl_forever::ConfigFloat("WXL_FOREVER_COOKIES_DEBUG", 0.0f, 0.0f, 2.0f));
        static CookieDebug debug;
        WLOG_INFO("cookies: %s, strength %.2f, floor %.2f, flame %.2f, tint %.2f, budget %d", g_cfg.enabled ? "on" : "off",
                  g_cfg.strength, g_cfg.floor, g_cfg.flame, g_cfg.tint, g_cfg.budget);
    }

    void Frame(IDirect3DDevice9* dev, Light* lights, int count, const float eye[3], uint32_t frame)
    {
        for (int i = 0; i < count; ++i) lights[i].cookieCell = 0;
        g_withCookie = 0;
        if (!g_cfg.enabled || !dev || !lights) { UpdateStatus(); return; }
        if (!g_loaded) Load();
        if (g_records.empty() || !EnsureAtlas(dev)) { UpdateStatus(); return; }

        // Which cookies the frame wants, by the importance of the brightest light using each.
        std::vector<int> touched;
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
        }
        std::sort(touched.begin(), touched.end(), [](int a, int b) { return g_records[size_t(a)].score > g_records[size_t(b)].score; });
        // Past the budget a cookie gets no cell, only its mean (RequestMeans, below).
        std::vector<int> beyond;
        if (touched.size() > size_t(g_cfg.budget))
        {
            beyond.assign(touched.begin() + g_cfg.budget, touched.end());
            touched.resize(size_t(g_cfg.budget));
        }
        for (int rec : touched) g_records[size_t(rec)].wanted = frame;

        // Give every wanted cookie a cell: an empty one, else the one least recently wanted.
        for (size_t rank = 0; rank < touched.size(); ++rank)
        {
            Record& r = g_records[size_t(touched[rank])];
            if (r.cell >= 0) continue;
            int best = -1;
            uint32_t oldest = frame;
            for (size_t c = 0; c < g_cells.size(); ++c)
            {
                const Cell& cell = g_cells[c];
                if (cell.record < 0) { best = int(c); break; }
                const uint32_t w = g_records[size_t(cell.record)].wanted;
                if (w != frame && frame - w > kKeepFrames && w < oldest) { oldest = w; best = int(c); }
            }
            if (best < 0) break;
            Cell& cell = g_cells[size_t(best)];
            if (cell.record >= 0) g_records[size_t(cell.record)].cell = -1;
            if (cell.asset) bk::Release(cell.asset);
            cell = Cell{};
            cell.record = touched[rank];
            cell.asset = bk::Request(g_manifest.Folder() + r.file, 100 - int(rank), bk::kWantBytes);
            r.cell = best;
        }
        for (Record& r : g_records) r.score = 0.0f;
        RequestMeans(touched);   // a wanted cookie no cell could take this frame
        RequestMeans(beyond);

        bk::Pump(dev, frame);
        PollMeans();

        // Copy finished reads into their cells.
        g_resident = g_pending = 0;
        for (size_t c = 0; c < g_cells.size(); ++c)
        {
            Cell& cell = g_cells[c];
            if (cell.record < 0) continue;
            if (cell.filled) { ++g_resident; continue; }
            const bk::State state = bk::StateOf(cell.asset);
            if (state == bk::State::Ready)
            {
                const std::string* bytes = bk::Bytes(cell.asset);
                Record& r = g_records[size_t(cell.record)];
                if (bytes && FillCell(int(c), *bytes, r.file.c_str(), r)) { cell.filled = true; ++g_resident; }
                else { r.failed = true; r.cell = -1; bk::Release(cell.asset); cell = Cell{}; continue; }
                bk::Release(cell.asset);
                cell.asset = 0;
            }
            else if (state == bk::State::Failed)
            {
                Record& r = g_records[size_t(cell.record)];
                r.failed = true;
                r.cell = -1;
                bk::Release(cell.asset);
                cell = Cell{};
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
                // Nothing jumps: the pattern fades in over the mean once resident (it can only leave
                // with a cell no light wanted for kKeepFrames), and the manifest's mean eases to the file's.
                r.eased = frame;
                r.resident = filled ? std::min(r.resident + step, 1.0f) : 0.0f;
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
                // Cell + 1, plus 128 times the pattern's share on 8 bits (shaders/cookies.hlsli).
                l.cookieCell = uint16_t((r.cell + 1) + 128 * int(r.resident * 255.0f + 0.5f));
                ++g_withCookie;
            }
        }
        UpdateStatus();
    }

    void Bind(IDirect3DDevice9* dev, unsigned stage)
    {
        IDirect3DBaseTexture9* tex = g_atlas ? static_cast<IDirect3DBaseTexture9*>(g_atlas) : bk::White2D(dev);
        wxl::forever::render::Sampler(dev, stage, tex, true);
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

    const char* Status() { return g_status; }

    void Panel()
    {
        ui::Text(g_status);
        ui::Text(bk::Status());
        ui::Separator();
        ui::Check("Cookies", &g_cfg.enabled,
                  "Lanterns, lamps and braziers throw the shadow of their own cage and frame: a baked pattern per model light (Textures/Forever/Cookies). Off leaves every light smooth.");
        ui::Slider("Cookie strength", &g_cfg.strength, 0.0f, 1.0f,
                   "How strongly the pattern shapes the light: 1 the baked transmittance, 0 no effect. Lower it if lamp patterns look too hard.");
        ui::Slider("Cookie floor", &g_cfg.floor, 0.0f, 0.5f,
                   "The least a pattern lets through in any direction, for the light that leaks and bounces inside a fixture. Lower it for struts and pane grids that read clearly on walls and in the fog; 0 lets struts block completely.");
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

    void ReleaseTextures()
    {
        if (g_atlas) { g_atlas->Release(); g_atlas = nullptr; }
        ResetCells();
    }
}
