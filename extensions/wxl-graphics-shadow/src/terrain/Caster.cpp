// wxl-graphics-shadow: the terrain caster in the engine's sun shadow maps.
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

#include "Caster.hpp"
#include "Horizon.hpp"
#include "../core/Extension.hpp"
#include "../core/Settings.hpp"
#include "../gpu/Spirv.hpp"

#include "wxl/gfx/Matrix.hpp"
#include "game/Gx.hpp"
#include "game/Shadows.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace
{
    namespace cs = wxl::gfx::shadow::caster;
    namespace hz = wxl::gfx::shadow::horizon;
    namespace sh = wxl::game::shadows;
    namespace mx = wxl::gfx::matrix;
    namespace ws = wxl::gfx::shadow;

    constexpr int   kGrid    = hz::kTile + 1;          // vertices per tile edge, with the seam row and column
    constexpr int   kPatch   = 32;                     // cells per patch edge
    constexpr int   kPatches = hz::kTile / kPatch;     // 4 per axis
    constexpr int   kLods    = 4;                      // steps 1, 2, 4, 8
    constexpr float kUnit    = hz::kTileSize / float(hz::kTile);
    constexpr int   kBuildsPerFrame = 2;
    const int kStepBySlot[int(sh::Slot::Count)] = { 1, 1, 1, 2, 4 };   // main, interior, bands 0..2

    struct Vertex { float x, y, z; };
    struct Range { UINT start, primitives; };

    struct Mesh
    {
        int  cx = INT_MIN, cy = INT_MIN;
        IDirect3DVertexBuffer9* vb = nullptr;
        float box[kPatches][kPatches][2];   // patch (pi, pj): zmin, zmax
        float zmin = 0.0f, zmax = 0.0f;
        bool  seamComplete = false;         // both neighbours' seams were available at build time
        uint32_t builtGeneration = 0;
    };

    Mesh         g_meshes[hz::kBlock][hz::kBlock];
    IDirect3DIndexBuffer9*       g_ib[kLods] = {};
    Range                        g_ranges[kLods][kPatches][kPatches];
    IDirect3DVertexDeclaration9* g_decl = nullptr;
    IDirect3DVertexShader9*      g_vs = nullptr;
    IDirect3DPixelShader9*       g_ps = nullptr;
    bool         g_chained = false;
    bool         g_buffersFailed = false;
    bool         g_shadersFailed = false;
    cs::Stats    g_stats{};
    cs::Stats    g_frameStats{};
    uint32_t     g_totalFailures = 0;
    char         g_status[192] = "terrain caster: idle";
    bool         g_faulted = false;

    // --- meshes -------------------------------------------------------------------------------------

    bool EnsureShaders(IDirect3DDevice9* dev)
    {
        if (g_vs && g_ps) return true;
        if (g_shadersFailed) return false;
        const WXL_GraphicsExtendApi* gfx = ws::Gfx();
        if (!gfx) return false;
        auto desc = [](const char* name, const char* target) {
            WXL_GfxShaderDesc d{};
            d.structSize = sizeof d;
            d.name = name;
            size_t size = 0;
            d.source = ws::spirv::D3d9Source(name, &size);
            d.sourceSize = size;
            d.target = target;
            return d;
        };
        const WXL_GfxShaderDesc vd = desc("caster.vs", "vs_3_0");
        const WXL_GfxShaderDesc pd = desc("caster.ps", "ps_3_0");
        if (vd.source && !g_vs) g_vs = static_cast<IDirect3DVertexShader9*>(gfx->CreateVertexShader(dev, &vd));
        if (pd.source && !g_ps) g_ps = static_cast<IDirect3DPixelShader9*>(gfx->CreatePixelShader(dev, &pd));
        if (!g_vs || !g_ps)
        {
            g_shadersFailed = true;
            SHADOW_LOG_WARN("terrain caster: its shaders did not compile; the terrain casts nothing in the cascades");
            return false;
        }
        return true;
    }

    bool EnsureIndexBuffers(IDirect3DDevice9* dev)
    {
        if (g_buffersFailed) return false;
        if (g_ib[kLods - 1] && g_decl) return true;
        if (!g_decl)
        {
            const D3DVERTEXELEMENT9 elements[] = {
                { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
                D3DDECL_END()
            };
            if (FAILED(dev->CreateVertexDeclaration(elements, &g_decl))) { g_decl = nullptr; g_buffersFailed = true; return false; }
        }
        for (int l = 0; l < kLods; ++l)
        {
            if (g_ib[l]) continue;
            const int step = 1 << l;
            std::vector<uint16_t> indices;
            for (int pj = 0; pj < kPatches; ++pj)
                for (int pi = 0; pi < kPatches; ++pi)
                {
                    Range& range = g_ranges[l][pj][pi];
                    range.start = UINT(indices.size());
                    for (int r = pj * kPatch; r < (pj + 1) * kPatch; r += step)
                        for (int c = pi * kPatch; c < (pi + 1) * kPatch; c += step)
                        {
                            const uint16_t a = uint16_t(r * kGrid + c), b = uint16_t(r * kGrid + c + step);
                            const uint16_t d = uint16_t((r + step) * kGrid + c), e = uint16_t((r + step) * kGrid + c + step);
                            indices.insert(indices.end(), { a, b, e, a, e, d });
                        }
                    range.primitives = UINT(indices.size() - range.start) / 3;
                }
            const UINT bytes = UINT(indices.size() * sizeof(uint16_t));
            if (FAILED(dev->CreateIndexBuffer(bytes, 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_ib[l], nullptr)))
            {
                g_ib[l] = nullptr;
                g_buffersFailed = true;
                SHADOW_LOG_WARN("terrain caster: index buffer for step %d unavailable; the terrain casts nothing", step);
                return false;
            }
            void* p = nullptr;
            if (SUCCEEDED(g_ib[l]->Lock(0, bytes, &p, 0))) { std::memcpy(p, indices.data(), bytes); g_ib[l]->Unlock(); }
        }
        return true;
    }

    int Wrap(int v) { v %= hz::kBlock; return v < 0 ? v + hz::kBlock : v; }

    /// Height at grid (r, c) of tile (cx, cy), the seam row and column from the neighbours.
    float HeightAt(const float* own, const float* east, const float* south, const float* corner, int r, int c)
    {
        if (r < hz::kTile && c < hz::kTile) return own[r * hz::kTile + c];
        if (r < hz::kTile) return east ? east[r * hz::kTile] : own[r * hz::kTile + hz::kTile - 1];
        if (c < hz::kTile) return south ? south[c] : own[(hz::kTile - 1) * hz::kTile + c];
        if (corner) return corner[0];
        if (east) return east[(hz::kTile - 1) * hz::kTile];
        if (south) return south[hz::kTile - 1];
        return own[hz::kTile * hz::kTile - 1];
    }

    /// Builds (or rebuilds) the mesh of a tile; false when its heights are absent.
    bool Build(IDirect3DDevice9* dev, Mesh& m, int cx, int cy)
    {
        const float* own = hz::TileHeights(cx, cy);
        if (!own) return false;
        const float* east   = hz::TileHeights(cx + 1, cy);       // column 128: -Y side
        const float* south  = hz::TileHeights(cx, cy + 1);       // row 128: -X side
        const float* corner = hz::TileHeights(cx + 1, cy + 1);
        if (!m.vb && FAILED(dev->CreateVertexBuffer(kGrid * kGrid * sizeof(Vertex), 0, 0, D3DPOOL_MANAGED, &m.vb, nullptr)))
        {
            m.vb = nullptr;
            return false;
        }
        void* p = nullptr;
        if (FAILED(m.vb->Lock(0, 0, &p, 0))) return false;
        Vertex* v = static_cast<Vertex*>(p);
        const float x0 = (32.0f - float(cy)) * hz::kTileSize;
        const float y0 = (32.0f - float(cx)) * hz::kTileSize;
        for (int pj = 0; pj < kPatches; ++pj)
            for (int pi = 0; pi < kPatches; ++pi) { m.box[pj][pi][0] = 1e9f; m.box[pj][pi][1] = -1e9f; }
        m.zmin = 1e9f; m.zmax = -1e9f;
        for (int r = 0; r < kGrid; ++r)
            for (int c = 0; c < kGrid; ++c)
            {
                const float z = HeightAt(own, east, south, corner, r, c);
                Vertex& o = v[r * kGrid + c];
                o.x = x0 - float(r) * kUnit;
                o.y = y0 - float(c) * kUnit;
                o.z = z;
                m.zmin = std::min(m.zmin, z);
                m.zmax = std::max(m.zmax, z);
                // A seam vertex belongs to the patches on both sides of it.
                const int pj0 = std::min(r / kPatch, kPatches - 1), pi0 = std::min(c / kPatch, kPatches - 1);
                const int pj1 = (r % kPatch == 0 && r > 0) ? pj0 - 1 : pj0, pi1 = (c % kPatch == 0 && c > 0) ? pi0 - 1 : pi0;
                for (int pj = pj1; pj <= pj0; ++pj)
                    for (int pi = pi1; pi <= pi0; ++pi)
                    {
                        m.box[pj][pi][0] = std::min(m.box[pj][pi][0], z);
                        m.box[pj][pi][1] = std::max(m.box[pj][pi][1], z);
                    }
            }
        m.vb->Unlock();
        m.cx = cx;
        m.cy = cy;
        m.seamComplete = east && south && corner;
        m.builtGeneration = hz::Generation();
        return true;
    }

    void DropMesh(Mesh& m)
    {
        if (m.vb) m.vb->Release();
        m = Mesh();
    }

    /// Keeps one mesh per resident tile of the block, rebuilding a few per frame as heights arrive.
    void Upkeep(IDirect3DDevice9* dev)
    {
        const int ox = hz::OriginX(), oy = hz::OriginY();
        if (ox == INT_MIN) return;
        const uint32_t generation = hz::Generation();
        int builds = 0;
        g_frameStats.meshes = 0;
        for (int j = 0; j < hz::kBlock; ++j)
            for (int i = 0; i < hz::kBlock; ++i)
            {
                const int cx = ox + Wrap(i - ox), cy = oy + Wrap(j - oy);
                Mesh& m = g_meshes[j][i];
                const bool have = hz::TileHeights(cx, cy) != nullptr;
                if (!have) { if (m.vb) DropMesh(m); continue; }
                const bool stale = m.cx != cx || m.cy != cy || (m.builtGeneration != generation && !m.seamComplete);
                if (stale && builds < kBuildsPerFrame)
                {
                    if (m.vb && (m.cx != cx || m.cy != cy)) DropMesh(m);
                    if (Build(dev, m, cx, cy)) ++builds;
                }
                if (m.vb && m.cx == cx && m.cy == cy) ++g_frameStats.meshes;
            }
    }

    // --- the pass -------------------------------------------------------------------------------------

    /// Everything the pass touches on the device, read before and put back after.
    struct Saved
    {
        bool active = false;
        IDirect3DSurface9* rt = nullptr;
        IDirect3DSurface9* ds = nullptr;
        D3DVIEWPORT9 viewport{};
        IDirect3DVertexShader9* vs = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexDeclaration9* decl = nullptr;
        DWORD fvf = 0;
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT vbOffset = 0, vbStride = 0;
        IDirect3DIndexBuffer9* ib = nullptr;
        DWORD states[16] = {};
        float vsConst[10][4] = {};
        float psConst[1][4] = {};
    };
    constexpr D3DRENDERSTATETYPE kStates[16] = {
        D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_CULLMODE, D3DRS_COLORWRITEENABLE,
        D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE,
        D3DRS_STENCILENABLE, D3DRS_FILLMODE, D3DRS_CLIPPLANEENABLE, D3DRS_SLOPESCALEDEPTHBIAS,
        D3DRS_DEPTHBIAS, D3DRS_SRGBWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE,
    };
    Saved g_saved;

    void Capture(IDirect3DDevice9* d)
    {
        Saved& s = g_saved;
        d->GetRenderTarget(0, &s.rt);
        d->GetDepthStencilSurface(&s.ds);
        d->GetViewport(&s.viewport);
        d->GetVertexShader(&s.vs);
        d->GetPixelShader(&s.ps);
        d->GetVertexDeclaration(&s.decl);
        d->GetFVF(&s.fvf);
        d->GetStreamSource(0, &s.vb, &s.vbOffset, &s.vbStride);
        d->GetIndices(&s.ib);
        for (int k = 0; k < 16; ++k) d->GetRenderState(kStates[k], &s.states[k]);
        d->GetVertexShaderConstantF(0, &s.vsConst[0][0], 10);
        d->GetPixelShaderConstantF(0, &s.psConst[0][0], 1);
        s.active = true;
    }

    void Restore(IDirect3DDevice9* d)
    {
        Saved& s = g_saved;
        if (!s.active) return;
        s.active = false;
        // Targets first: the viewport is validated against them.
        d->SetRenderTarget(0, s.rt);
        d->SetDepthStencilSurface(s.ds);
        d->SetViewport(&s.viewport);
        for (int k = 0; k < 16; ++k) d->SetRenderState(kStates[k], s.states[k]);
        d->SetVertexShaderConstantF(0, &s.vsConst[0][0], 10);
        d->SetPixelShaderConstantF(0, &s.psConst[0][0], 1);
        d->SetVertexShader(s.vs);
        d->SetPixelShader(s.ps);
        if (s.decl) d->SetVertexDeclaration(s.decl); else d->SetFVF(s.fvf);
        d->SetStreamSource(0, s.vb, s.vbOffset, s.vbStride);
        d->SetIndices(s.ib);
        if (s.rt) s.rt->Release();
        if (s.ds) s.ds->Release();
        if (s.vs) s.vs->Release();
        if (s.ps) s.ps->Release();
        if (s.decl) s.decl->Release();
        if (s.vb) s.vb->Release();
        if (s.ib) s.ib->Release();
        s.rt = nullptr; s.ds = nullptr; s.vs = nullptr; s.ps = nullptr; s.decl = nullptr; s.vb = nullptr; s.ib = nullptr;
    }

    /// Whether a world box (camera-relative corners through view * proj) touches the pass's rectangle.
    bool Visible(const float m[16], const float cam[3], float xmin, float xmax, float ymin, float ymax, float zmin, float zmax)
    {
        float lo[2] = { 1e9f, 1e9f }, hi[2] = { -1e9f, -1e9f };
        for (int k = 0; k < 8; ++k)
        {
            const float p[3] = { (k & 1 ? xmax : xmin) - cam[0], (k & 2 ? ymax : ymin) - cam[1], (k & 4 ? zmax : zmin) - cam[2] };
            for (int a = 0; a < 2; ++a)
            {
                const float v = p[0] * m[a] + p[1] * m[4 + a] + p[2] * m[8 + a] + m[12 + a];
                lo[a] = std::min(lo[a], v);
                hi[a] = std::max(hi[a], v);
            }
        }
        return !(hi[0] < -1.0f || lo[0] > 1.0f || hi[1] < -1.0f || lo[1] > 1.0f);
    }

    /// Draws every resident tile mesh that touches the pass into its map.
    void DrawPass(IDirect3DDevice9* d, const sh::RenderPass& pass)
    {
        const ws::Settings& s = ws::Config();
        auto* colourTex = static_cast<IDirect3DTexture9*>(pass.colour);
        auto* depthTex = static_cast<IDirect3DTexture9*>(pass.depth);
        IDirect3DSurface9* colour = nullptr;
        IDirect3DSurface9* depth = nullptr;
        if (colourTex) colourTex->GetSurfaceLevel(0, &colour);
        if (depthTex) depthTex->GetSurfaceLevel(0, &depth);
        if (!colour && !depth) return;

        Capture(d);
        IDirect3DSurface9* sized = colour ? colour : depth;
        D3DSURFACE_DESC desc{};
        sized->GetDesc(&desc);
        if (colour) d->SetRenderTarget(0, colour);
        if (depth) d->SetDepthStencilSurface(depth);
        // Without a depth texture the engine drew with the depth buffer that was bound; if that one
        // is smaller than the map, D3D refuses the draw below and the failure is counted.
        D3DVIEWPORT9 vp{};
        vp.X = DWORD(std::max(pass.viewport[0], 0.0f) * desc.Width + 0.5f);
        vp.Y = DWORD(std::max(pass.viewport[2], 0.0f) * desc.Height + 0.5f);
        vp.Width = DWORD(std::max(pass.viewport[1] - pass.viewport[0], 0.0f) * desc.Width + 0.5f);
        vp.Height = DWORD(std::max(pass.viewport[3] - pass.viewport[2], 0.0f) * desc.Height + 0.5f);
        vp.MinZ = 0.0f;
        vp.MaxZ = 1.0f;
        if (vp.Width == 0 || vp.Height == 0) { vp.X = vp.Y = 0; vp.Width = desc.Width; vp.Height = desc.Height; }
        d->SetViewport(&vp);

        d->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        d->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        d->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
        d->SetRenderState(D3DRS_DEPTHBIAS, 0);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);

        float c[10][4];
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) { c[r][k] = pass.view[r * 4 + k]; c[4 + r][k] = pass.proj[r * 4 + k]; }
        c[8][0] = pass.cameraPos[0]; c[8][1] = pass.cameraPos[1]; c[8][2] = pass.cameraPos[2]; c[8][3] = s.casterBias;
        float m[16];
        mx::Mul4(pass.view, pass.proj, m);
        int step = kStepBySlot[int(pass.slot)] << std::max(s.casterDetail, 0);
        if (s.casterDetail < 0) step = std::max(step >> 1, 1);
        // The mesh follows only the ground's grid corners (every kUnit * step yards): where the ground dips
        // between them, a mesh at their height lies above it and shadows it in squares. Lowered straight
        // down in proportion to its cell, it never shadows its own ground; a hill still shadows what lies
        // behind it. (The bias along the light is almost horizontal under a low sun or moon.)
        c[9][0] = pass.lightDir[0]; c[9][1] = pass.lightDir[1]; c[9][2] = pass.lightDir[2];
        c[9][3] = std::max(s.casterDrop, 0.0f) * float(step);
        d->SetVertexShaderConstantF(0, &c[0][0], 10);
        const float scale[4] = { 0.0f, 0.0f, 0.0f, pass.depthScale };
        d->SetPixelShaderConstantF(0, scale, 1);
        d->SetVertexShader(g_vs);
        d->SetPixelShader(g_ps);
        d->SetVertexDeclaration(g_decl);
        int lod = 0;
        while ((1 << lod) < step && lod < kLods - 1) ++lod;
        d->SetIndices(g_ib[lod]);

        int patches = 0, triangles = 0;
        bool failed = false;
        const float T = hz::kTileSize;
        for (int j = 0; j < hz::kBlock && !failed; ++j)
            for (int i = 0; i < hz::kBlock && !failed; ++i)
            {
                const Mesh& mesh = g_meshes[j][i];
                if (!mesh.vb || mesh.cx == INT_MIN) continue;
                const float x0 = (32.0f - float(mesh.cy)) * T;
                const float y0 = (32.0f - float(mesh.cx)) * T;
                if (!Visible(m, pass.cameraPos, x0 - T, x0, y0 - T, y0, mesh.zmin - c[9][3], mesh.zmax)) continue;
                d->SetStreamSource(0, mesh.vb, 0, sizeof(Vertex));
                for (int pj = 0; pj < kPatches; ++pj)
                    for (int pi = 0; pi < kPatches; ++pi)
                    {
                        // Patch (pi, pj): rows pj * 32.., columns pi * 32..: X down by rows, Y down by columns.
                        const float px1 = x0 - float(pj * kPatch) * kUnit, px0 = px1 - float(kPatch) * kUnit;
                        const float py1 = y0 - float(pi * kPatch) * kUnit, py0 = py1 - float(kPatch) * kUnit;
                        if (!Visible(m, pass.cameraPos, px0, px1, py0, py1, mesh.box[pj][pi][0] - c[9][3], mesh.box[pj][pi][1])) continue;
                        const Range& range = g_ranges[lod][pj][pi];
                        const HRESULT hr = d->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, 0, kGrid * kGrid, range.start, range.primitives);
                        if (FAILED(hr))
                        {
                            if (!g_totalFailures)
                                SHADOW_LOG_WARN("terrain caster: draw refused (0x%08lX) into a %ux%u map, depth %s", static_cast<unsigned long>(hr),
                                                desc.Width, desc.Height, depth ? "own" : "the scene's");
                            ++g_totalFailures;
                            failed = true;
                            break;
                        }
                        ++patches;
                        triangles += int(range.primitives);
                    }
            }
        Restore(d);
        if (colour) colour->Release();
        if (depth) depth->Release();
        g_frameStats.patches[int(pass.slot)] += patches;
        g_frameStats.triangles += triangles;
        ++g_frameStats.passes;
    }

    void RestoreGuarded(IDirect3DDevice9* d)
    {
        __try { Restore(d); }
        __except (EXCEPTION_EXECUTE_HANDLER) { g_saved.active = false; }
    }

    void DrawPassGuarded(IDirect3DDevice9* d, const sh::RenderPass& pass)
    {
        __try { DrawPass(d, pass); }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            g_faulted = true;
            SHADOW_LOG_ERROR("terrain caster: fault in the pass, the terrain casts nothing for this session");
            RestoreGuarded(d);
        }
    }

    void __cdecl AfterRender(const sh::CallbackArgs& a, void*)
    {
        const ws::Settings& s = ws::Config();
        if (!s.enabled || !s.terrainCaster || g_faulted || ws::Isolated(ws::kIsoNoCaster)) return;
        sh::RenderPass pass;
        if (!sh::DescribeRender(a, pass)) return;
        if (pass.slot == sh::Slot::Interior) return;
        const int band = pass.slot == sh::Slot::Main ? 0 : int(pass.slot) - int(sh::Slot::Band0) + 1;
        if (band >= s.casterCascades) return;
        auto* d = static_cast<IDirect3DDevice9*>(wxl::game::gx::RawDevice());
        if (!d || !g_ib[kLods - 1] || !g_decl || !g_vs || !g_ps) return;
        DrawPassGuarded(d, pass);
    }
}

namespace wxl::gfx::shadow::caster
{
    void Frame(IDirect3DDevice9* dev)
    {
        if (!dev) return;
        const Settings& s = Config();
        if (!s.enabled || !s.terrainCaster) return;
        if (!g_chained) g_chained = sh::ChainAfter(sh::Callback::Render, &AfterRender, nullptr);
        if (!g_faulted && EnsureShaders(dev) && EnsureIndexBuffers(dev)) Upkeep(dev);
        // Stats of the render callbacks that ran since the last Frame.
        g_stats = g_frameStats;
        g_stats.failures = g_totalFailures;
        const int meshes = g_frameStats.meshes;
        std::memset(&g_frameStats, 0, sizeof g_frameStats);
        g_frameStats.meshes = meshes;
        std::snprintf(g_status, sizeof g_status, "terrain caster: %s, %d meshes, %d passes, patches main %d bands %d/%d/%d, %d tris%s",
                      g_faulted ? "faulted" : (g_chained ? "on" : "waiting for the engine"), g_stats.meshes, g_stats.passes,
                      g_stats.patches[0], g_stats.patches[2], g_stats.patches[3], g_stats.patches[4], g_stats.triangles,
                      g_stats.failures ? ", draws refused" : "");
    }

    Stats GetStats() { return g_stats; }
    const char* Status() { return g_status; }

    void Release()
    {
        for (int j = 0; j < hz::kBlock; ++j)
            for (int i = 0; i < hz::kBlock; ++i) DropMesh(g_meshes[j][i]);
        for (int l = 0; l < kLods; ++l) if (g_ib[l]) { g_ib[l]->Release(); g_ib[l] = nullptr; }
        if (g_decl) { g_decl->Release(); g_decl = nullptr; }
        if (g_vs) { g_vs->Release(); g_vs = nullptr; }
        if (g_ps) { g_ps->Release(); g_ps = nullptr; }
    }
}
