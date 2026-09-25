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

#include "ExtensionApi.hpp"
#include "ShaderLibrary.hpp"
#include "Bls.hpp"

#include "game/Io.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    namespace io = wxl::game::io;

    // Where the patch folder sits, for a loose read when the client's file system does not have a
    // file (one written after the client mounted the folder).
    constexpr const char* kLooseRoot = "Data\\Patch-4.MPQ\\";

    enum class Origin { None, Bytecode, DevSource, Embedded };

    struct Program
    {
        IDirect3DPixelShader9*  ps = nullptr;
        IDirect3DVertexShader9* vs = nullptr;
        Origin origin = Origin::None;
        bool   tried = false;
    };

    std::unordered_map<std::string, Program> g_programs;
    char g_status[160] = "shaders: none loaded";

    /// A client file, through its file system first, then loose from the patch folder.
    bool ReadFile(const std::string& path, std::string& out)
    {
        void* handle = nullptr;
        if (io::FileOpen(path.c_str(), io::kOpenWholeFile, &handle) && handle)
        {
            uint32_t high = 0;
            const uint32_t size = io::FileSize(handle, &high);
            bool ok = false;
            if (size && !high)
            {
                out.resize(size);
                uint32_t got = 0;
                ok = io::FileRead(handle, out.data(), size, &got) != 0 && got == size;
            }
            io::FileClose(handle);
            if (ok) return true;
        }
        std::ifstream loose(kLooseRoot + path, std::ios::binary);
        if (!loose) return false;
        out.assign(std::istreambuf_iterator<char>(loose), std::istreambuf_iterator<char>());
        return !out.empty();
    }

    std::string Backslashes(std::string s)
    {
        for (char& c : s) if (c == '/') c = '\\';
        return s;
    }

    /// Resolves #include "feature/shaders/x.hlsli" from the embedded files or the deployed sources.
    class Includes final : public ID3DInclude
    {
    public:
        explicit Includes(bool deployed) : deployed_(deployed) {}

        HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override
        {
            std::string text;
            if (!Find(name, deployed_, text)) return E_FAIL;
            char* copy = new char[text.size() + 1];
            std::memcpy(copy, text.c_str(), text.size() + 1);
            *data = copy;
            *bytes = UINT(text.size());
            return S_OK;
        }

        HRESULT __stdcall Close(LPCVOID data) override
        {
            delete[] static_cast<const char*>(data);
            return S_OK;
        }

        static bool Find(const char* name, bool deployed, std::string& text)
        {
            if (deployed && ReadFile("Shaders\\Forever\\src\\" + Backslashes(name), text)) return true;
            for (int i = 0; i < wxl::forever::shaders::kEmbeddedFileCount; ++i)
            {
                if (std::strcmp(wxl::forever::shaders::kEmbeddedFiles[i].path, name) != 0) continue;
                text = wxl::forever::shaders::kEmbeddedFiles[i].text;
                return true;
            }
            return false;
        }

    private:
        bool deployed_;
    };

    pD3DCompile Compiler()
    {
        static const pD3DCompile compile = [] {
            HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
            if (!m) m = LoadLibraryA("d3dcompiler_47.dll");
            return m ? reinterpret_cast<pD3DCompile>(GetProcAddress(m, "D3DCompile")) : nullptr;
        }();
        return compile;
    }

    /// Logs the compiler's own instruction slot count for a program's bytecode.
    void LogSlots(const void* code, size_t size, const char* name, const char* from)
    {
        using Disassemble = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);
        static const Disassemble disassemble = [] {
            HMODULE m = GetModuleHandleA("d3dcompiler_47.dll");
            if (!m) m = LoadLibraryA("d3dcompiler_47.dll");
            return m ? reinterpret_cast<Disassemble>(GetProcAddress(m, "D3DDisassemble")) : nullptr;
        }();
        ID3DBlob* text = nullptr;
        const char* slots = nullptr;
        int len = 0;
        if (disassemble && SUCCEEDED(disassemble(code, size, 0, nullptr, &text)) && text)
        {
            const char* body = static_cast<const char*>(text->GetBufferPointer());
            if ((slots = std::strstr(body, "approximately ")) != nullptr)
            {
                const char* end = std::strchr(slots, '\n');
                len = end ? int(end - slots) : int(std::strlen(slots));
            }
        }
        if (slots) WLOG_INFO("shaders: %s from %s, %.*s", name, from, len, slots);
        else       WLOG_INFO("shaders: %s from %s", name, from);
        if (text) text->Release();
    }

    /// Compiles source to bytecode; the compiler's message goes to the log on failure.
    bool Compile(const std::string& source, const char* file, const char* target, bool deployed, std::string& bytecode,
                 const D3D_SHADER_MACRO* macros = nullptr)
    {
        const pD3DCompile compile = Compiler();
        if (!compile)
        {
            WLOG_WARN("shaders: d3dcompiler_47 unavailable, %s cannot be compiled", file);
            return false;
        }
        Includes includes(deployed);
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT hr = compile(source.data(), source.size(), file, macros, &includes, "main", target,
                                   D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (FAILED(hr) || !code)
            WLOG_ERROR("shaders: %s (%s) compile failed: %s", file, target,
                       err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        if (err) err->Release();
        if (FAILED(hr) || !code) return false;
        bytecode.assign(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
        code->Release();
        return true;
    }

    const char* OriginName(Origin o)
    {
        return o == Origin::Bytecode ? "the patch BLS" : o == Origin::DevSource ? "the deployed source (dev)"
                                                             : "the embedded source";
    }

    void UpdateStatus()
    {
        int counts[4] = {};
        for (const auto& [name, p] : g_programs) ++counts[int(p.origin)];
        std::snprintf(g_status, sizeof g_status, "shaders: %d from patch BLS files, %d from dev sources, %d embedded, %d failed%s",
                      counts[int(Origin::Bytecode)], counts[int(Origin::DevSource)], counts[int(Origin::Embedded)],
                      counts[int(Origin::None)], wxl::forever::shaders::DevMode() ? " (dev mode)" : "");
    }

    /// Finds the bytecode of a program, in order: dev source, patch bytecode, embedded source.
    Origin Load(const std::string& name, bool pixel, std::string& bytecode)
    {
        const size_t dot = name.find('.');
        if (dot == std::string::npos) return Origin::None;
        const std::string feature = name.substr(0, dot), stem = name.substr(dot + 1);
        const char* target = pixel ? "ps_3_0" : "vs_3_0";
        const std::string file = feature + "/shaders/" + stem + (pixel ? ".ps.hlsl" : ".vs.hlsl");

        std::string source;
        if (wxl::forever::shaders::DevMode())
        {
            if (Includes::Find(file.c_str(), true, source) && Compile(source, file.c_str(), target, true, bytecode))
                return Origin::DevSource;
            WLOG_WARN("shaders: %s: no usable deployed source, trying the patch BLS", name.c_str());
        }

        // The client's own format: a BLS file whose first permutation is this program.
        const std::string path = std::string("Shaders\\") + (pixel ? "pixel\\ps_3_0" : "vertex\\vs_3_0") + "\\Forever\\" + name + ".bls";
        std::string bytes;
        std::vector<wxl::forever::bls::Permutation> perms;
        if (ReadFile(path, bytes))
        {
            if (wxl::forever::bls::Read(bytes, perms) && !perms.empty())
            {
                bytecode = std::move(perms[0].code);
                return Origin::Bytecode;
            }
            WLOG_WARN("shaders: %s is not a valid BLS file", path.c_str());
        }

        WLOG_WARN("shaders: %s missing, compiling %s from the embedded source", path.c_str(), name.c_str());
        if (Includes::Find(file.c_str(), false, source) && Compile(source, file.c_str(), target, false, bytecode))
            return Origin::Embedded;
        return Origin::None;
    }

    /// Once per session: reads a client BLS file with the same reader and has the device create
    /// every permutation, so the format our files share with the client's is proven on this device.
    void ValidateClientBls(IDirect3DDevice9* dev)
    {
        static bool done = false;
        if (done || !dev) return;
        done = true;
        const char* path = "Shaders\\pixel\\ps_3_0\\Combiners_Add.bls";
        std::string bytes;
        std::vector<wxl::forever::bls::Permutation> perms;
        if (!ReadFile(path, bytes) || !wxl::forever::bls::Read(bytes, perms))
        {
            WLOG_WARN("shaders: BLS check: %s could not be read", path);
            return;
        }
        int created = 0;
        for (const auto& p : perms)
        {
            IDirect3DPixelShader9* ps = nullptr;
            if (SUCCEEDED(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(p.code.data()), &ps)) && ps)
            {
                ++created;
                ps->Release();
            }
        }
        WLOG_INFO("shaders: BLS check: client %s, %u permutations, %d created by the device", path,
                  unsigned(perms.size()), created);
    }

    Program& Get(IDirect3DDevice9* dev, const char* name, bool pixel)
    {
        ValidateClientBls(dev);
        Program& p = g_programs[name];
        if (p.tried || !dev) return p;
        p.tried = true;

        std::string bytecode;
        const Origin origin = Load(name, pixel, bytecode);
        HRESULT hr = E_FAIL;
        if (origin != Origin::None)
        {
            const DWORD* code = reinterpret_cast<const DWORD*>(bytecode.data());
            hr = pixel ? dev->CreatePixelShader(code, &p.ps) : dev->CreateVertexShader(code, &p.vs);
            if (FAILED(hr)) WLOG_ERROR("shaders: %s: the device rejected its bytecode (hr=0x%08lX)", name, static_cast<unsigned long>(hr));
        }
        p.origin = SUCCEEDED(hr) ? origin : Origin::None;
        if (p.origin != Origin::None) LogSlots(bytecode.data(), bytecode.size(), name, OriginName(p.origin));
        else WLOG_ERROR("shaders: %s unavailable", name);
        UpdateStatus();
        return p;
    }
}

namespace wxl::forever::shaders
{
    IDirect3DPixelShader9* Pixel(IDirect3DDevice9* dev, const char* name) { return Get(dev, name, true).ps; }

    IDirect3DVertexShader9* Vertex(IDirect3DDevice9* dev, const char* name) { return Get(dev, name, false).vs; }

    IDirect3DPixelShader9* PixelVariant(IDirect3DDevice9* dev, const char* name, const char* define, const char* value)
    {
        const std::string key = std::string(name) + "#" + define + "=" + value;
        Program& p = g_programs[key];
        if (p.tried || !dev) return p.ps;
        p.tried = true;

        const std::string full = name;
        const size_t dot = full.find('.');
        if (dot == std::string::npos) return nullptr;
        const std::string file = full.substr(0, dot) + "/shaders/" + full.substr(dot + 1) + ".ps.hlsl";
        std::string source, bytecode;
        const D3D_SHADER_MACRO macros[] = { { define, value }, { nullptr, nullptr } };
        const bool deployed = DevMode();
        if (Includes::Find(file.c_str(), deployed, source)
            && Compile(source, file.c_str(), "ps_3_0", deployed, bytecode, macros)
            && SUCCEEDED(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(bytecode.data()), &p.ps)))
            p.origin = deployed ? Origin::DevSource : Origin::Embedded;
        UpdateStatus();
        return p.ps;
    }

    void Reload()
    {
        for (auto& [name, p] : g_programs)
        {
            if (p.ps) p.ps->Release();
            if (p.vs) p.vs->Release();
        }
        g_programs.clear();
        WLOG_INFO("shaders: reloading%s", DevMode() ? " (dev mode: deployed sources first)" : "");
        UpdateStatus();
    }

    bool DevMode()
    {
        static const bool dev = wxl_forever::ConfigBool("WXL_FOREVER_SHADER_DEV", false);
        return dev;
    }

    const char* Status() { return g_status; }
}
