// wxl-graphics-extend: HLSL compiled with d3dcompiler_47, cached in memory and on disk.
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

#include "Compiler.hpp"
#include "CacheRecord.hpp"
#include "../core/Extension.hpp"

#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    namespace cache = wxl::gfx::shaders::cache;

    /// Part of every key: bump it when what goes into a key, or what a compile means, changes.
    constexpr const char* kKeyTag = "wxl-gfx-shader-cache-1";
    constexpr const char* kCacheDir = "Extensions\\wxl-graphics-extend\\cache";
    constexpr size_t kMaxRecordFile = 32u << 20;

    // --- d3dcompiler_47 ------------------------------------------------------------------------------

    using DisassembleFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

    struct CompilerModule
    {
        pD3DCompile   compile = nullptr;
        DisassembleFn disassemble = nullptr;
    };

    /// GetProcAddress's FARPROC through void*, which neither compiler flags as a function cast.
    template <class Fn>
    Fn Proc(HMODULE m, const char* name)
    {
        return reinterpret_cast<Fn>(reinterpret_cast<void*>(GetProcAddress(m, name)));
    }

    std::atomic<int> g_moduleState{ 0 };   // 0 not tried, 1 loaded, 2 missing (for Status)

    /// Loaded on first use, once, on whichever thread gets there first; missing is reported once.
    const CompilerModule& Module()
    {
        static const CompilerModule module = [] {
            CompilerModule m;
            HMODULE h = GetModuleHandleA("d3dcompiler_47.dll");
            if (!h) h = LoadLibraryA("d3dcompiler_47.dll");
            if (h)
            {
                m.compile = Proc<pD3DCompile>(h, "D3DCompile");
                m.disassemble = Proc<DisassembleFn>(h, "D3DDisassemble");
            }
            g_moduleState.store(m.compile ? 1 : 2);
            if (!m.compile) GFX_LOG_WARN("shaders: d3dcompiler_47.dll not found: no shader can be compiled this session");
            return m;
        }();
        return module;
    }

    /// Releases a blob on every path out of a compile.
    struct Blob
    {
        ID3DBlob* p = nullptr;
        ~Blob() { if (p) p->Release(); }
        const char* Text() const { return p ? static_cast<const char*>(p->GetBufferPointer()) : nullptr; }
    };

    // --- includes ------------------------------------------------------------------------------------

    /// Include names as the resolver and the cache records see them: forward slashes only.
    std::string Normalise(const char* name)
    {
        std::string s = name ? name : "";
        for (char& c : s) if (c == '\\') c = '/';
        return s;
    }

    bool IsBuiltIn(const std::string& name)
    {
        return name.compare(0, 4, "wxl/") == 0;
    }

    const char* FindBuiltIn(const std::string& name, size_t& size)
    {
        for (int i = 0; i < wxl::gfx::shaders::kEmbeddedFileCount; ++i)
        {
            const wxl::gfx::shaders::EmbeddedFile& f = wxl::gfx::shaders::kEmbeddedFiles[i];
            if (name != f.path) continue;
            size = f.size;
            return f.text;
        }
        return nullptr;
    }

    /// The one resolver a compile and every later check of its includes go through: "wxl/..." from
    /// the built-in library, the rest from the descriptor's callback. Null when unknown.
    const char* Resolve(const WXL_GfxShaderDesc& desc, const std::string& name, size_t& size)
    {
        size = 0;
        if (IsBuiltIn(name)) return FindBuiltIn(name, size);
        if (!desc.include) return nullptr;
        return desc.include(desc.includeUser, name.c_str(), &size);
    }

    /// Hands the compiler each include's text (valid for the whole compile, so no copy is made)
    /// and records what was opened, with the text's hash, for the cache entry.
    class IncludeHandler final : public ID3DInclude
    {
    public:
        IncludeHandler(const WXL_GfxShaderDesc& desc, std::vector<cache::Include>& records)
            : desc_(desc), records_(records) {}

        HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data, UINT* bytes) override
        {
            try
            {
                const std::string normalised = Normalise(name);
                size_t size = 0;
                const char* text = Resolve(desc_, normalised, size);
                if (!text) return E_FAIL;
                bool known = false;
                for (const cache::Include& r : records_) known = known || r.name == normalised;
                if (!known) records_.push_back({ normalised, cache::Fnv1a64(text, size) });
                *data = text;
                *bytes = UINT(size);
                return S_OK;
            }
            catch (const std::bad_alloc&)
            {
                return E_OUTOFMEMORY;
            }
        }

        HRESULT __stdcall Close(LPCVOID) override { return S_OK; }

    private:
        const WXL_GfxShaderDesc& desc_;
        std::vector<cache::Include>& records_;
    };

    /// Whether every include a cached compile opened still resolves to the same text.
    bool IncludesUnchanged(const WXL_GfxShaderDesc& desc, const std::vector<cache::Include>& includes)
    {
        for (const cache::Include& inc : includes)
        {
            size_t size = 0;
            const char* text = Resolve(desc, inc.name, size);
            if (!text || cache::Fnv1a64(text, size) != inc.hash) return false;
        }
        return true;
    }

    // --- the caches ----------------------------------------------------------------------------------

    struct Entry
    {
        std::vector<cache::Include> includes;
        std::string bytecode;
    };

    std::mutex g_mutex;
    std::unordered_map<uint64_t, std::shared_ptr<const Entry>> g_memory;   // guarded by g_mutex
    std::atomic<uint32_t> g_compiled{ 0 };
    std::atomic<uint32_t> g_cached{ 0 };

    std::shared_ptr<const Entry> FindMemory(uint64_t key)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_memory.find(key);
        return it == g_memory.end() ? nullptr : it->second;
    }

    void StoreMemory(uint64_t key, std::shared_ptr<const Entry> entry)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_memory[key] = std::move(entry);
    }

    enum class Disk { Off, On, Unavailable };

    /// The disk cache's folder, created on first use; off by configuration or when it cannot exist.
    Disk DiskState()
    {
        static const Disk state = [] {
            if (!wxl::gfx::ConfigBool("WXL_GFX_SHADER_DISK_CACHE", true)) return Disk::Off;
            for (const char* dir : { "Extensions", "Extensions\\wxl-graphics-extend", kCacheDir })
            {
                if (CreateDirectoryA(dir, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) continue;
                GFX_LOG_WARN("shaders: cannot create %s (error %lu): the disk cache is off", dir,
                             static_cast<unsigned long>(GetLastError()));
                return Disk::Unavailable;
            }
            return Disk::On;
        }();
        return state;
    }

    std::string DiskPath(uint64_t key)
    {
        return std::string(kCacheDir) + "\\" + cache::FileName(key);
    }

    bool ReadWhole(const std::string& path, std::string& out)
    {
        HANDLE h = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        bool ok = GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= LONGLONG(kMaxRecordFile);
        if (ok) out.resize(size_t(size.QuadPart));
        DWORD got = 0;
        if (ok) ok = ReadFile(h, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size();
        CloseHandle(h);
        return ok;
    }

    /// A sibling temp file, flushed, then moved over the target: a reader sees the old record or
    /// the new one, never a torn one. The temp name is unique per process and write, so two
    /// threads (or two clients) storing the same key do not trip over each other.
    bool WriteAtomic(const std::string& path, const std::string& bytes)
    {
        static std::atomic<uint32_t> s_serial{ 0 };
        char suffix[48];
        std::snprintf(suffix, sizeof suffix, ".%lx-%u.tmp", static_cast<unsigned long>(GetCurrentProcessId()),
                      unsigned(s_serial.fetch_add(1)));
        const std::string tmp = path + suffix;
        HANDLE h = CreateFileA(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        DWORD put = 0;
        bool ok = WriteFile(h, bytes.data(), DWORD(bytes.size()), &put, nullptr) && put == bytes.size();
        ok = FlushFileBuffers(h) && ok;
        CloseHandle(h);
        if (ok) ok = MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        if (!ok) DeleteFileA(tmp.c_str());
        return ok;
    }

    /// A disk record for key whose includes still match today's sources; loaded into memory too.
    std::shared_ptr<const Entry> LoadDisk(const WXL_GfxShaderDesc& desc, uint64_t key)
    {
        std::string bytes;
        cache::Record record;
        if (!ReadWhole(DiskPath(key), bytes) || !cache::Decode(bytes.data(), bytes.size(), record)) return nullptr;
        if (record.key != key || !IncludesUnchanged(desc, record.includes)) return nullptr;
        auto entry = std::make_shared<Entry>();
        entry->includes = std::move(record.includes);
        entry->bytecode = std::move(record.bytecode);
        StoreMemory(key, entry);
        return entry;
    }

    void StoreDisk(uint64_t key, const Entry& entry, const char* name)
    {
        cache::Record record;
        record.key = key;
        record.includes = entry.includes;
        record.bytecode = entry.bytecode;
        std::string bytes;
        if (!cache::Encode(record, bytes) || !WriteAtomic(DiskPath(key), bytes))
            GFX_LOG_DEBUG("shaders: %s: could not be stored in the disk cache", name);
    }

    // --- one compile ---------------------------------------------------------------------------------

    struct Inputs
    {
        const char* name;
        const char* entry;
        size_t sourceSize;
        UINT flags;                            // D3DCOMPILE_*
        std::vector<D3D_SHADER_MACRO> macros;  // NULL-terminated for the compiler
    };

    /// The descriptor's NULL-terminated name, value list as the compiler's array. A missing value
    /// ends the list (the pair cannot be read past it) and defines the name as empty.
    void ReadDefines(const char* const* defines, std::vector<D3D_SHADER_MACRO>& out)
    {
        if (defines)
        {
            for (size_t i = 0; defines[i]; i += 2)
            {
                const char* value = defines[i + 1];
                out.push_back({ defines[i], value ? value : "" });
                if (!value) break;
            }
        }
        out.push_back({ nullptr, nullptr });
    }

    /// Everything the bytecode depends on except the includes, which the entry records by hash.
    uint64_t Key(const WXL_GfxShaderDesc& d, const Inputs& in)
    {
        cache::Hasher h;
        h.AddString(kKeyTag);
        h.AddString(d.target);
        h.AddString(in.entry);
        h.Add(uint32_t(in.flags));
        h.Add(uint32_t(in.macros.size() - 1));
        for (const D3D_SHADER_MACRO& m : in.macros)
        {
            if (!m.Name) break;
            h.AddString(m.Name);
            h.AddString(m.Definition);
        }
        h.Add(uint32_t(in.sourceSize));
        h.Add(d.source, in.sourceSize);
        return h.Value();
    }

    /// The compiler's messages without the trailing newline it leaves on them.
    std::string Trimmed(const char* text)
    {
        std::string s = text ? text : "";
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\0')) s.pop_back();
        return s;
    }

    /// The compiler's own instruction slot line from the disassembly ("approximately N instruction
    /// slots used (...)"), empty when it cannot be had.
    std::string Slots(const std::string& bytecode)
    {
        const CompilerModule& m = Module();
        Blob text;
        if (!m.disassemble || FAILED(m.disassemble(bytecode.data(), bytecode.size(), 0, nullptr, &text.p)) || !text.p) return "";
        const char* body = text.Text();
        const char* at = body ? std::strstr(body, "approximately ") : nullptr;
        if (!at) return "";
        const char* end = std::strchr(at, '\n');
        return std::string(at, end ? size_t(end - at) : std::strlen(at));
    }

    double Milliseconds(const LARGE_INTEGER& from, const LARGE_INTEGER& to)
    {
        LARGE_INTEGER f{};
        QueryPerformanceFrequency(&f);
        return f.QuadPart ? double(to.QuadPart - from.QuadPart) * 1000.0 / double(f.QuadPart) : 0.0;
    }

    bool CompileNow(const WXL_GfxShaderDesc& d, const Inputs& in, std::shared_ptr<const Entry>& out)
    {
        const CompilerModule& m = Module();
        if (!m.compile)
        {
            GFX_LOG_DEBUG("shaders: %s (%s) not compiled: no compiler", in.name, d.target);
            return false;
        }

        auto entry = std::make_shared<Entry>();
        IncludeHandler includes(d, entry->includes);
        Blob code, messages;
        LARGE_INTEGER t0{}, t1{};
        QueryPerformanceCounter(&t0);
        const HRESULT hr = m.compile(d.source, in.sourceSize, in.name, in.macros.data(), &includes, in.entry, d.target,
                                     in.flags, 0, &code.p, &messages.p);
        QueryPerformanceCounter(&t1);

        const std::string text = Trimmed(messages.Text());
        if (FAILED(hr) || !code.p)
        {
            GFX_LOG_ERROR("shaders: %s (%s) failed to compile (hr=0x%08lX):\n%s", in.name, d.target,
                          static_cast<unsigned long>(hr), text.empty() ? "no message from the compiler" : text.c_str());
            return false;
        }
        if (!text.empty()) GFX_LOG_DEBUG("shaders: %s (%s) warnings:\n%s", in.name, d.target, text.c_str());

        entry->bytecode.assign(static_cast<const char*>(code.p->GetBufferPointer()), code.p->GetBufferSize());
        const std::string slots = Slots(entry->bytecode);
        GFX_LOG_INFO("shaders: %s (%s) compiled in %.1f ms%s%s", in.name, d.target, Milliseconds(t0, t1),
                     slots.empty() ? "" : ", ", slots.c_str());
        g_compiled.fetch_add(1);
        out = std::move(entry);
        return true;
    }

    bool CompileChecked(const WXL_GfxShaderDesc& d, std::string& bytecode)
    {
        Inputs in;
        in.name = d.name && d.name[0] ? d.name : "(unnamed shader)";
        in.entry = d.entry && d.entry[0] ? d.entry : "main";
        in.sourceSize = d.sourceSize ? d.sourceSize : std::strlen(d.source);
        in.flags = (d.flags & WXL_GFX_SHADER_DEBUG) ? (D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION)
                                                    : D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ReadDefines(d.defines, in.macros);
        const bool useCache = !(d.flags & WXL_GFX_SHADER_NO_CACHE);

        uint64_t key = 0;
        if (useCache)
        {
            key = Key(d, in);
            std::shared_ptr<const Entry> hit = FindMemory(key);
            if (hit && !IncludesUnchanged(d, hit->includes))
            {
                GFX_LOG_DEBUG("shaders: %s (%s): a file it includes changed, recompiling", in.name, d.target);
                hit = nullptr;
            }
            else if (!hit && DiskState() == Disk::On)
            {
                hit = LoadDisk(d, key);
            }
            if (hit)
            {
                g_cached.fetch_add(1);
                bytecode = hit->bytecode;
                return true;
            }
        }

        std::shared_ptr<const Entry> fresh;
        if (!CompileNow(d, in, fresh)) return false;
        if (useCache)
        {
            StoreMemory(key, fresh);
            if (DiskState() == Disk::On) StoreDisk(key, *fresh, in.name);
        }
        bytecode = fresh->bytecode;
        return true;
    }

    bool ValidDesc(const WXL_GfxShaderDesc* desc)
    {
        if (!desc || desc->structSize < sizeof(WXL_GfxShaderDesc))
        {
            GFX_LOG_ERROR("shaders: CompileShader: invalid descriptor");
            return false;
        }
        if (!desc->source || !desc->target || !desc->target[0])
        {
            GFX_LOG_ERROR("shaders: %s: a source and a target are required", desc->name ? desc->name : "(unnamed shader)");
            return false;
        }
        return true;
    }

    const char* DescName(const WXL_GfxShaderDesc* desc)
    {
        return desc && desc->name && desc->name[0] ? desc->name : "(unnamed shader)";
    }
}

namespace wxl::gfx::shaders
{
    bool Compile(const WXL_GfxShaderDesc* desc, std::string& bytecode)
    {
        if (!ValidDesc(desc)) return false;
        try
        {
            return CompileChecked(*desc, bytecode);
        }
        catch (const std::bad_alloc&)
        {
            GFX_LOG_ERROR("shaders: %s: out of memory", DescName(desc));
            return false;
        }
    }

    int Compile(const WXL_GfxShaderDesc* desc, WXL_ByteSink* out)
    {
        if (!out || !out->Write) return 0;
        std::string bytecode;
        if (!Compile(desc, bytecode)) return 0;
        out->Write(out->ctx, bytecode.data(), static_cast<unsigned int>(bytecode.size()));
        return 1;
    }

    void* CreatePixelShader(void* device, const WXL_GfxShaderDesc* desc)
    {
        auto* d = static_cast<IDirect3DDevice9*>(device);
        std::string bytecode;
        if (!d || !Compile(desc, bytecode)) return nullptr;
        IDirect3DPixelShader9* shader = nullptr;
        const HRESULT hr = d->CreatePixelShader(reinterpret_cast<const DWORD*>(bytecode.data()), &shader);
        if (FAILED(hr) || !shader)
        {
            GFX_LOG_ERROR("shaders: %s: the device rejected the pixel shader (hr=0x%08lX)", DescName(desc),
                          static_cast<unsigned long>(hr));
            return nullptr;
        }
        return shader;
    }

    void* CreateVertexShader(void* device, const WXL_GfxShaderDesc* desc)
    {
        auto* d = static_cast<IDirect3DDevice9*>(device);
        std::string bytecode;
        if (!d || !Compile(desc, bytecode)) return nullptr;
        IDirect3DVertexShader9* shader = nullptr;
        const HRESULT hr = d->CreateVertexShader(reinterpret_cast<const DWORD*>(bytecode.data()), &shader);
        if (FAILED(hr) || !shader)
        {
            GFX_LOG_ERROR("shaders: %s: the device rejected the vertex shader (hr=0x%08lX)", DescName(desc),
                          static_cast<unsigned long>(hr));
            return nullptr;
        }
        return shader;
    }

    const char* IncludeFile(const char* name, size_t* size)
    {
        if (size) *size = 0;
        if (!name) return nullptr;
        try
        {
            size_t n = 0;
            const char* text = FindBuiltIn(Normalise(name), n);
            if (text && size) *size = n;
            return text;
        }
        catch (const std::bad_alloc&)
        {
            return nullptr;
        }
    }

    void ClearCache()
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_memory.clear();
    }

    void Stats(uint32_t& compiled, uint32_t& cached)
    {
        compiled = g_compiled.load();
        cached = g_cached.load();
    }

    const char* Status()
    {
        static char line[192];
        const int module = g_moduleState.load();
        const Disk disk = DiskState();
        std::snprintf(line, sizeof line, "shaders: d3dcompiler_47 %s, disk cache %s, %u compiled, %u cache hits",
                      module == 1 ? "loaded" : module == 2 ? "missing" : "not needed yet",
                      disk == Disk::On ? "on" : disk == Disk::Off ? "off (WXL_GFX_SHADER_DISK_CACHE=0)" : "unavailable",
                      unsigned(g_compiled.load()), unsigned(g_cached.load()));
        return line;
    }
}
