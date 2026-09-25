// wxl-graphics-extend: the pass-through vertex shader and the clip-space quad of full-screen passes.
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

#include "Fullscreen.hpp"
#include "Compiler.hpp"
#include "../core/Extension.hpp"

#include <windows.h>
#include <d3d9.h>

namespace
{
    // vs_3_0 "dcl_position v0 / dcl_position o0 / mov o0, v0", assembled by hand so the quad needs no
    // compiler at all. Token by token: the version; dcl (opcode 31, two tokens follow) with the usage
    // token (bit 31 set, D3DDECLUSAGE_POSITION 0) and the register v0 (bit 31, type D3DSPR_INPUT 1 in
    // bits 28-30, write mask xyzw in bits 16-19); the same for o0 (D3DSPR_OUTPUT 6); mov (opcode 1,
    // two tokens) to o0 from v0 with the identity swizzle 0xE4 in bits 16-23; the end token.
    constexpr DWORD kPassThrough[] = {
        0xFFFE0300,
        0x0200001F, 0x80000000, 0x900F0000,
        0x0200001F, 0x80000000, 0xE00F0000,
        0x02000001, 0xE00F0000, 0x90E40000,
        0x0000FFFF,
    };

    /// The two triangles of a strip covering clip space, z 0 and w 1: D3DFVF_XYZW, stride 16.
    constexpr float kQuad[4][4] = { { -1, 1, 0, 1 }, { 1, 1, 0, 1 }, { -1, -1, 0, 1 }, { 1, -1, 0, 1 } };

    // Render thread only, as every device call here. Shaders survive a device reset, so one per
    // device pointer lasts the process; a new device (a different pointer) gets its own.
    IDirect3DDevice9*       g_device = nullptr;
    IDirect3DVertexShader9* g_shader = nullptr;
    bool                    g_tried = false;

    IDirect3DVertexShader9* Create(IDirect3DDevice9* d)
    {
        IDirect3DVertexShader9* shader = nullptr;
        const HRESULT hr = d->CreateVertexShader(kPassThrough, &shader);
        if (SUCCEEDED(hr) && shader) return shader;
        GFX_LOG_WARN("fullscreen: the device refused the assembled vertex shader (hr=0x%08lX), compiling it instead",
                     static_cast<unsigned long>(hr));

        size_t size = 0;
        const char* source = wxl::gfx::shaders::IncludeFile("wxl/gfx/fullscreen.vs.hlsl", &size);
        if (source)
        {
            WXL_GfxShaderDesc desc{};
            desc.structSize = sizeof desc;
            desc.name = "wxl/gfx/fullscreen.vs.hlsl";
            desc.source = source;
            desc.sourceSize = size;
            desc.target = "vs_3_0";
            shader = static_cast<IDirect3DVertexShader9*>(wxl::gfx::shaders::CreateVertexShader(d, &desc));
        }
        if (!shader) GFX_LOG_ERROR("fullscreen: no pass-through vertex shader: full-screen passes cannot draw");
        return shader;
    }
}

namespace wxl::gfx::fullscreen
{
    void* VertexShader(void* device)
    {
        auto* d = static_cast<IDirect3DDevice9*>(device);
        if (!d) return nullptr;
        if (d != g_device)
        {
            if (g_shader) g_shader->Release();
            g_shader = nullptr;
            g_tried = false;
            g_device = d;
        }
        if (!g_shader && !g_tried)
        {
            g_tried = true;
            g_shader = Create(d);
        }
        return g_shader;
    }

    void Draw(void* device, uint32_t width, uint32_t height)
    {
        auto* d = static_cast<IDirect3DDevice9*>(device);
        if (!d || !width || !height) return;
        auto* shader = static_cast<IDirect3DVertexShader9*>(VertexShader(d));
        if (!shader) return;
        d->SetVertexShader(shader);
        d->SetFVF(D3DFVF_XYZW);
        const D3DVIEWPORT9 viewport{ 0, 0, width, height, 0.0f, 1.0f };
        d->SetViewport(&viewport);
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, kQuad, sizeof kQuad[0]);
    }
}
