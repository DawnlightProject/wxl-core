// wxl-forever: every shader program of the suite, loaded by name from the client's files.
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

#pragma once

struct IDirect3DDevice9;
struct IDirect3DPixelShader9;
struct IDirect3DVertexShader9;

// A program is named "<feature>.<stem>" for src/<feature>/shaders/<stem>.ps.hlsl (or .vs.hlsl).
// The build compiles each with fxc, wraps it in the client's own shader format (BLS, core/Bls.hpp)
// and deploys it to the client patch folder as Shaders\pixel\ps_3_0\Forever\<name>.bls
// (Shaders\vertex\vs_3_0\... for vertex programs), read at runtime through the client's file
// system. When that file is missing the program is compiled from the source embedded in the DLL,
// and the log says so. With WXL_FOREVER_SHADER_DEV=1 the sources deployed under
// Shaders\Forever\src are compiled first, so an edit and a Reload show at once.
namespace wxl::forever::shaders
{
    /// One shader source file, embedded by the build (cmake/EmbedShaders.cmake).
    struct EmbeddedFile
    {
        const char* path;   // relative to src, forward slashes: "fog/shaders/common.hlsli"
        const char* text;
    };
    extern const EmbeddedFile kEmbeddedFiles[];
    extern const int kEmbeddedFileCount;

    /// The program, loaded on first use and kept until Reload; null when every source failed.
    IDirect3DPixelShader9* Pixel(IDirect3DDevice9* dev, const char* name);
    IDirect3DVertexShader9* Vertex(IDirect3DDevice9* dev, const char* name);

    /// A variant of a pixel program compiled at runtime from its source with one macro defined
    /// (define=value): for diagnostics, never shipped as a file. Cached until Reload.
    IDirect3DPixelShader9* PixelVariant(IDirect3DDevice9* dev, const char* name, const char* define, const char* value);

    /// Releases every program; each is loaded again from the files on its next use.
    void Reload();

    /// True when WXL_FOREVER_SHADER_DEV asks for the deployed sources first.
    bool DevMode();

    /// One line for a panel: how many programs came from bytecode, dev sources, or the fallback.
    const char* Status();
}
