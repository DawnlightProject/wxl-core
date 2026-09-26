// wxl-host-probe --textures: the host's texture images, backing references and prefetch, checked offline.
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

// The decoded images are compared with the client's own decoder, not with a second copy of ours: Wow.exe's
// image is mapped at its base (0x400000) and CBLPFile::Source + LockChain2 run on the file's bytes, exactly as
// HostTextures' verify mode does in game. Those functions are leaf code (the static CRT and Storm are inside
// the image), so nothing else of the client needs to be initialised.

#include "probe/TextureProbe.hpp"

#include "engine/storage/TextureStub.hpp"
#include "host/Blp.hpp"
#include "ipc/Ring.hpp"
#include "offsets/engine/Gx.hpp"
#include "offsets/engine/Texture.hpp"
#include "runtime/host/HostClient.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace wxl::probe
{
    namespace
    {
        namespace h = wxl::host;
        namespace blp = wxl::hostd::blp;
        namespace tex = wxl::offsets::engine::texture;
        using wxl::ipc::Status;

        int g_failures = 0;
        bool g_engine = false;   // the client's image is mapped: its loader runs here

        void Check(bool ok, const char* what)
        {
            std::printf("  [%s] %s\n", ok ? "ok" : "FAIL", what);
            if (!ok) ++g_failures;
        }

        uint32_t U32(const uint8_t* p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        bool ReadDisk(const std::wstring& path, std::vector<uint8_t>& out)
        {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER size{};
            GetFileSizeEx(f, &size);
            out.resize(size_t(size.QuadPart));
            DWORD got = 0;
            const bool ok = out.empty() || (::ReadFile(f, out.data(), DWORD(out.size()), &got, nullptr) && got == out.size());
            CloseHandle(f);
            return ok;
        }

        std::string Narrow(const std::wstring& w)
        {
            std::string s(w.size(), '\0');
            WideCharToMultiByte(CP_ACP, 0, w.data(), int(w.size()), s.data(), int(s.size()), nullptr, nullptr);
            return s;
        }

        /// Maps the client's image at its own base, sections only: enough for its leaf functions.
        bool MapClient(const std::wstring& client)
        {
            std::vector<uint8_t> pe;
            if (!ReadDisk(client + L"\\Wow.exe.orig", pe) && !ReadDisk(client + L"\\Wow.exe", pe)) return false;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(pe.data());
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(pe.data() + dos->e_lfanew);
            if (nt->OptionalHeader.ImageBase != 0x400000) return false;
            auto* base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(0x400000), nt->OptionalHeader.SizeOfImage,
                                                            MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            // A parent that reserved the range in this process before it ran (Respawn) leaves it to commit.
            if (!base)
                base = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(0x400000), nt->OptionalHeader.SizeOfImage,
                                                         MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            if (base != reinterpret_cast<uint8_t*>(0x400000))
            {
                MEMORY_BASIC_INFORMATION mbi{};
                VirtualQuery(reinterpret_cast<void*>(0x400000), &mbi, sizeof mbi);
                std::printf("    0x400000 is taken: region %p + %zu KB, state 0x%lX, type 0x%lX\n", mbi.AllocationBase,
                            mbi.RegionSize >> 10, mbi.State, mbi.Type);
                return false;
            }
            std::memcpy(base, pe.data(), nt->OptionalHeader.SizeOfHeaders);
            const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
            for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i)
            {
                const uint32_t n = std::min<uint32_t>(s[i].SizeOfRawData, s[i].Misc.VirtualSize ? s[i].Misc.VirtualSize : s[i].SizeOfRawData);
                if (s[i].PointerToRawData + uint64_t(n) <= pe.size()) std::memcpy(base + s[i].VirtualAddress, pe.data() + s[i].PointerToRawData, n);
            }
            // GetTextureFormats reads the device caps (device + 0x214): a blank device with DXT1/3/5 support.
            auto* device = static_cast<uint8_t*>(VirtualAlloc(nullptr, 0x4000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
            if (!device) return false;
            for (uint32_t cap : { 0x3Cu, 0x40u, 0x44u }) *reinterpret_cast<uint32_t*>(device + 0x214 + cap) = 1;
            *reinterpret_cast<uint32_t*>(device + tex::kDeviceMaxTextureEdge) = 16384;
            *reinterpret_cast<uint8_t**>(wxl::offsets::engine::gx::kGxDevicePtr) = device;
            return true;
        }

        using GetTextureFormatsFn = void(__cdecl*)(int* pixelFormat, int* textureFormat, uint32_t preferred, int alphaDepth);
        constexpr uintptr_t kGetTextureFormats = 0x004B5FE0;

        /**
         * @brief Runs the loader's steps over a buffer as PumpBlpTextureAsync does (0x004B7BD0): Source, the
         *        formats, LockChain2 in place into the boot scratch. levels receives what the upload callback
         *        would hand ITexUpload for each level.
         */
        /// The client-code part of RunLoader, in a leaf that can catch a fault (no C++ objects to unwind).
        int LoaderSteps(const uint8_t* buffer, uint8_t* scratch, int* pixelFormat, uint32_t* stage, DWORD* fault)
        {
            __try
            {
                alignas(16) uint8_t obj[0x500] = {};
                *stage = 1;
                reinterpret_cast<tex::BlpFileInitFn>(tex::kBlpFileInit)(obj);
                *stage = 2;
                if (!reinterpret_cast<tex::BlpFileSourceFn>(tex::kBlpFileSource)(obj, buffer)) return 0;
                int textureFormat = 0;
                *pixelFormat = 0;
                *stage = 3;
                reinterpret_cast<GetTextureFormatsFn>(kGetTextureFormats)(pixelFormat, &textureFormat,
                                                                       buffer[blp::field::kPreferredFormat],
                                                                       buffer[blp::field::kAlphaDepth]);
                void* table = scratch;
                *stage = 4;
                const int ok = reinterpret_cast<tex::BlpFileLockChain2Fn>(tex::kBlpFileLockChain2)(obj, "probe", *pixelFormat,
                                                                                                  &table, 0, 1);
                *stage = 5;
                reinterpret_cast<tex::BlpFileCloseFn>(tex::kBlpFileClose)(obj);
                return ok ? 1 : 0;
            }
            __except (*fault = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
            {
                return -1;
            }
        }

        bool RunLoader(const uint8_t* buffer, std::vector<std::vector<uint8_t>>& levels, int& pixelFormat)
        {
            levels.clear();
            static std::vector<uint8_t> scratch(48u << 20);
            std::fill(scratch.begin(), scratch.begin() + 64 * 4, uint8_t(0));
            uint32_t stage = 0;
            DWORD fault = 0;
            const int ok = LoaderSteps(buffer, scratch.data(), &pixelFormat, &stage, &fault);
            if (ok < 0)
            {
                std::printf("    the client's loader faulted (0x%08lX) at step %u\n", fault, stage);
                return false;
            }
            if (!ok) return false;
            const auto* entries = reinterpret_cast<const uint8_t* const*>(scratch.data());
            const uint32_t width = U32(buffer + blp::field::kWidth), height = U32(buffer + blp::field::kHeight);
            const bool dxt = pixelFormat == 0 || pixelFormat == 1 || pixelFormat == 7;
            // The levels LockChain2 filled: in place up to the first empty size; decoded, the file's mip count
            // (MippedImgSet lays the levels right after the table, so the table ends there).
            const uint8_t compression = buffer[blp::field::kCompression];
            const bool inPlace = compression == blp::kCompressionArgb
                || (compression == blp::kCompressionDxt && (pixelFormat < 2 || pixelFormat > 5));
            uint32_t filled = 0;
            if (inPlace)
                while (filled < blp::kMaxLevels && U32(buffer + blp::field::kSizes + filled * 4)) ++filled;
            else
                filled = (buffer[blp::field::kHasMips] & 0xF) ? blp::LevelCount(width, height) : 1;
            // ITexWHDStartEnd (0x006A5EF0): a DXT texture's chain stops once its smaller side reaches 4, and
            // the upload reads no further.
            if (dxt)
            {
                uint32_t uploaded = 1;
                for (uint32_t side = std::min(width, height); side > 4; side >>= 1) ++uploaded;
                if (std::min(width, height) < 5) uploaded = 1;
                filled = std::min(filled, uploaded);
            }
            for (uint32_t i = 0; i < filled && entries[i]; ++i)
            {
                const uint32_t w = std::max(1u, width >> i), h = std::max(1u, height >> i);
                const uint32_t bytes = dxt ? std::max(1u, (w + 3) / 4) * std::max(1u, (h + 3) / 4) * (pixelFormat == 0 ? 8 : 16)
                                           : w * h * 4;
                levels.emplace_back(entries[i], entries[i] + bytes);
            }
            return true;
        }

        /// The loader over a texture handle: its buffer holds only the stand-in, rebased on that buffer.
        bool LoaderOverStub(const h::FileData& image, std::vector<std::vector<uint8_t>>& levels, int& pixelFormat)
        {
            namespace ts = wxl::runtime::storage::textures;
            std::vector<uint8_t> buffer(ts::kStubBytes);
            ts::WriteStubBytes(image.data, uint32_t(image.size), reinterpret_cast<uintptr_t>(buffer.data()), 0, buffer.data(),
                               ts::kStubBytes);
            return RunLoader(buffer.data(), levels, pixelFormat);
        }


        /// The client's loader decode of a palettized file into ARGB8888 levels; false when it refuses.
        bool LoaderDecode(const std::vector<uint8_t>& file, std::vector<std::vector<uint8_t>>& levels)
        {
            levels.clear();
            if (!blp::IsBlp2(file.data(), file.size())) return false;
            const uint32_t width = U32(file.data() + blp::field::kWidth), height = U32(file.data() + blp::field::kHeight);
            const uint32_t count = (file[blp::field::kHasMips] & 0xF) ? blp::LevelCount(width, height) : 1;
            uint64_t chain = 0;
            for (uint32_t i = 0; i < count; ++i)
                chain += uint64_t(std::max(1u, width >> i)) * std::max(1u, height >> i) * 4;
            std::vector<uint8_t> table(size_t(count) * 4 + size_t(chain) + 64, 0);
            alignas(16) uint8_t obj[0x500] = {};
            reinterpret_cast<tex::BlpFileInitFn>(tex::kBlpFileInit)(obj);
            bool ok = reinterpret_cast<tex::BlpFileSourceFn>(tex::kBlpFileSource)(obj, file.data()) != 0;
            void* chainTable = table.data();
            ok = ok && reinterpret_cast<tex::BlpFileLockChain2Fn>(tex::kBlpFileLockChain2)(obj, "probe", tex::kPixelArgb8888,
                                                                                             &chainTable, 0, 0) != 0;
            reinterpret_cast<tex::BlpFileCloseFn>(tex::kBlpFileClose)(obj);
            if (!ok) return false;
            const auto* ptrs = reinterpret_cast<const uint8_t* const*>(table.data());
            for (uint32_t i = 0; i < count; ++i)
            {
                const size_t bytes = size_t(std::max(1u, width >> i)) * std::max(1u, height >> i) * 4;
                levels.emplace_back(ptrs[i], ptrs[i] + bytes);
            }
            return true;
        }

        /// Collects every .blp under the extracted tree (names relative to it).
        void CollectBlp(const std::wstring& root, const std::wstring& rel, std::vector<std::string>& out)
        {
            WIN32_FIND_DATAW fd{};
            HANDLE find = FindFirstFileW((root + (rel.empty() ? L"" : L"\\" + rel) + L"\\*").c_str(), &fd);
            if (find == INVALID_HANDLE_VALUE) return;
            do
            {
                if (fd.cFileName[0] == L'.') continue;
                const std::wstring child = rel.empty() ? std::wstring(fd.cFileName) : rel + L"\\" + fd.cFileName;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) CollectBlp(root, child, out);
                else
                {
                    const size_t n = wcslen(fd.cFileName);
                    if (n > 4 && _wcsicmp(fd.cFileName + n - 4, L".blp") == 0) out.push_back(Narrow(child));
                }
            } while (FindNextFileW(find, &fd));
            FindClose(find);
        }

        struct Tally
        {
            size_t files = 0, decoded = 0, identical = 0, differ = 0, declined = 0, loaderRefused = 0, missing = 0, raw = 0;
            size_t rawSame = 0, stubChecked = 0, stubSame = 0;
            uint64_t hostUs = 0, loaderUs = 0, bytes = 0;
        };

        void CompareOne(const std::string& name, const std::vector<uint8_t>& file, bool engine, Tally& t, bool print)
        {
            h::ReadOptions options;
            options.flags = wxl::ipc::kFlagTexture;
            options.a4 = 16384;
            options.sizeHint = file.size();
            h::FileData image;
            const uint64_t t0 = wxl::ipc::NowUs();
            const Status s = h::ReadFile(name.c_str(), wxl::ipc::kAnyArchive, image, 0, &options);
            t.hostUs += wxl::ipc::NowUs() - t0;
            ++t.files;
            if (s != Status::Ok)
            {
                ++t.missing;
                if (print) std::printf("    missing via host: %s (status %d)\n", name.c_str(), int(s));
                return;
            }
            if (file.size() < blp::kHeaderBytes)
            {
                // Not a texture the loader could read either (the tree holds empty .blp files): served as is.
                ++t.raw;
                if (image.size == file.size() && (file.empty() || std::memcmp(image.data, file.data(), file.size()) == 0)) ++t.rawSame;
                else ++t.differ;
                h::ReleaseFile(image);
                return;
            }
            t.bytes += image.size;
            if (g_engine)
            {
                // The loader over the file as its buffer (native), and over the stand-in a texture handle returns.
                std::vector<std::vector<uint8_t>> native, viaStub;
                int nativeFormat = -1, stubFormat = -1;
                const bool nativeOk = RunLoader(file.data(), native, nativeFormat);
                const bool stubOk = LoaderOverStub(image, viaStub, stubFormat);
                ++t.stubChecked;
                if (nativeOk == stubOk && (!nativeOk || (nativeFormat == stubFormat && native == viaStub))) ++t.stubSame;
                else if (t.stubChecked - t.stubSame <= 50)
                {
                    size_t level = 0;
                    while (level < native.size() && level < viaStub.size() && native[level] == viaStub[level]) ++level;
                    std::printf("    the loader builds a different chain from the stand-in: %s (native %d/%d levels %zu, stand-in %d/%d "
                                "levels %zu, first difference at level %zu; header: compression %u alpha %u preferred %u mips %u "
                                "%ux%u, file %zu bytes)\n", name.c_str(), int(nativeOk), nativeFormat, native.size(), int(stubOk),
                                stubFormat, viaStub.size(), level, file[8], file[9], file[10], file[11], U32(file.data() + 12),
                                U32(file.data() + 16), file.size());
                }
            }
            std::vector<uint8_t> local;
            const bool localDecodes = blp::Decode(file.data(), file.size(), 16384, local);
            if (image.image != uint32_t(wxl::ipc::TextureImage::Decoded))
            {
                ++t.raw;
                if (image.size == file.size() && std::memcmp(image.data, file.data(), file.size()) == 0) ++t.rawSame;
                else
                {
                    ++t.differ;
                    if (print) std::printf("    texture image differs from the file: %s\n", name.c_str());
                }
                if (file[blp::field::kCompression] == blp::kCompressionPalette) ++t.declined;
                if (localDecodes && print) std::printf("    host declined what this build decodes: %s\n", name.c_str());
                h::ReleaseFile(image);
                return;
            }
            ++t.decoded;
            const bool sameAsLocal = local.size() == image.size && std::memcmp(local.data(), image.data, local.size()) == 0;
            bool same = sameAsLocal;
            if (engine)
            {
                std::vector<std::vector<uint8_t>> levels;
                const uint64_t l0 = wxl::ipc::NowUs();
                const bool loaderOk = LoaderDecode(file, levels);
                t.loaderUs += wxl::ipc::NowUs() - l0;
                if (!loaderOk)
                {
                    ++t.loaderRefused;
                    same = false;
                    if (print) std::printf("    the loader refuses what the host decoded: %s\n", name.c_str());
                }
                for (size_t i = 0; loaderOk && i < levels.size(); ++i)
                {
                    const uint32_t offset = U32(image.data + blp::field::kOffsets + i * 4);
                    const uint32_t bytes = U32(image.data + blp::field::kSizes + i * 4);
                    if (bytes != levels[i].size() || uint64_t(offset) + bytes > image.size
                        || std::memcmp(image.data + offset, levels[i].data(), bytes) != 0)
                    {
                        same = false;
                        if (print) std::printf("    level %zu differs from the loader's: %s\n", i, name.c_str());
                        break;
                    }
                }
            }
            if (same) ++t.identical;
            else ++t.differ;
            h::ReleaseFile(image);
        }

        void Report(const char* what, const Tally& t)
        {
            std::printf("  %s: %zu files, %zu decoded by the host (%zu identical to the loader, %zu differ, loader refused %zu), "
                        "%zu served as the file (%zu byte-identical, %zu palettized declined), %zu missing | host %.1f ms total, "
                        "loader %.1f ms, %.1f MB of images\n",
                        what, t.files, t.decoded, t.identical, t.differ, t.loaderRefused, t.raw, t.rawSame, t.declined, t.missing,
                        t.hostUs / 1000.0, t.loaderUs / 1000.0, t.bytes / (1024.0 * 1024.0));
            if (t.stubChecked)
                std::printf("  %s through the stand-in: the loader's chain equals its native one for %zu of %zu\n", what, t.stubSame,
                            t.stubChecked);
        }

        /// The bytes the client reads for a name: the Patch-4 folder wins over the archives.
        bool Expected(const std::wstring& client, const std::wstring& extracted, const std::string& name, std::vector<uint8_t>& out)
        {
            std::wstring w(name.size(), L'\0');
            MultiByteToWideChar(CP_ACP, 0, name.data(), int(name.size()), w.data(), int(w.size()));
            if (ReadDisk(client + L"\\Data\\Patch-4.MPQ\\" + w, out)) return true;
            return ReadDisk(extracted + L"\\" + w, out);
        }
    }

    int RespawnWithClientRange(const std::wstring& client)
    {
        wchar_t flag[8] = {};
        if (GetEnvironmentVariableW(L"WXL_PROBE_CHILD", flag, 8)) return -1;
        std::vector<uint8_t> pe;
        if (!ReadDisk(client + L"\\Wow.exe.orig", pe) && !ReadDisk(client + L"\\Wow.exe", pe)) return -1;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(pe.data() + reinterpret_cast<const IMAGE_DOS_HEADER*>(pe.data())->e_lfanew);
        const SIZE_T size = nt->OptionalHeader.SizeOfImage;

        SetEnvironmentVariableW(L"WXL_PROBE_CHILD", L"1");
        wchar_t self[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        std::wstring cmd = GetCommandLineW();
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(self, cmd.data(), nullptr, nullptr, TRUE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) return -1;
        void* reserved = VirtualAllocEx(pi.hProcess, reinterpret_cast<void*>(0x400000), size, MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        if (reserved != reinterpret_cast<void*>(0x400000))
            std::printf("  (could not reserve the client range in the child: win32 %lu)\n", GetLastError());
        ResumeThread(pi.hThread);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (code > 0xFF)
        {
            std::printf("FAILED: the texture check process ended with 0x%08lX\n", code);
            return 1;
        }
        return int(code);
    }

    int Textures(const std::wstring& client, const std::wstring& extracted, size_t samples)
    {
        SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* info) -> LONG {
            std::printf("  crashed: 0x%08lX at %p\n", info->ExceptionRecord->ExceptionCode, info->ExceptionRecord->ExceptionAddress);
            return EXCEPTION_EXECUTE_HANDLER;
        });
        std::printf("textures\n");
        const bool engine = MapClient(client);
        g_engine = engine;
        Check(engine, "the client's image maps at 0x400000 (its loader decodes for comparison)");

        std::vector<std::string> all;
        CollectBlp(extracted, L"", all);
        std::printf("  %zu BLP files in the extracted tree\n", all.size());
        std::vector<std::string> palettized, other;
        for (const std::string& name : all)
        {
            std::vector<uint8_t> head;
            std::wstring w(name.size(), L'\0');
            MultiByteToWideChar(CP_ACP, 0, name.data(), int(name.size()), w.data(), int(w.size()));
            HANDLE f = CreateFileW((extracted + L"\\" + w).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f == INVALID_HANDLE_VALUE) continue;
            uint8_t hdr[16] = {};
            DWORD got = 0;
            ::ReadFile(f, hdr, sizeof hdr, &got, nullptr);
            CloseHandle(f);
            if (got == sizeof hdr && U32(hdr) == blp::kMagicBlp2 && hdr[8] == blp::kCompressionPalette) palettized.push_back(name);
            else other.push_back(name);
        }
        std::mt19937 rng(12340);
        std::shuffle(palettized.begin(), palettized.end(), rng);
        std::shuffle(other.begin(), other.end(), rng);
        if (samples && palettized.size() > samples) palettized.resize(samples);
        if (samples && other.size() > samples) other.resize(samples);

        // Prefetch first: the corpus below fills the cache with entries a prefetch must not evict.
        std::printf("prefetch\n");
        {
            const wxl::ipc::HostCounters* hc = h::HostSideCounters();
            const uint64_t doneBefore = hc ? hc->prefetchDone.load() : 0;
            // Elwynn Forest, heading south at a run.
            auto pack = [](float a, float b) {
                uint32_t x, y;
                std::memcpy(&x, &a, 4);
                std::memcpy(&y, &b, 4);
                return uint64_t(x) | (uint64_t(y) << 32);
            };
            const float x = -9460.0f, y = 62.0f;
            h::Params p;
            p.op = wxl::ipc::Op::Hint;
            p.name = "World\\Maps\\Azeroth";
            p.a0 = pack(x, y);
            p.a1 = pack(-7.0f, 0.0f);
            p.a2 = pack(500.0f, 0.0f);
            h::Reply r;
            Check(h::Call(p, r, 1000) && r.status == Status::Ok, "a hint is accepted");
            const uint64_t t0 = wxl::ipc::NowUs();
            while (hc && hc->prefetchDone.load() < doneBefore + 20 && wxl::ipc::NowUs() - t0 < 15000000) Sleep(50);
            std::printf("  after %.1f s: %llu files read ahead (%.1f MB), %llu queued\n", (wxl::ipc::NowUs() - t0) / 1e6,
                        static_cast<unsigned long long>(hc ? hc->prefetchDone.load() - doneBefore : 0),
                        hc ? hc->prefetchBytes.load() / (1024.0 * 1024.0) : 0.0,
                        static_cast<unsigned long long>(hc ? hc->prefetchQueued.load() : 0));
            Check(hc && hc->prefetchDone.load() > doneBefore, "the hint reads tiles ahead of the client");
            // The tile under the hint, then a texture it names: both should come from the prefetch.
            const int first = int((32.0f * 533.33333f - y) / 533.33333f), second = int((32.0f * 533.33333f - x) / 533.33333f);
            char tile[96];
            std::snprintf(tile, sizeof tile, "World\\Maps\\Azeroth\\Azeroth_%d_%d.adt", first, second);
            h::FileData adt;
            const bool tileOk = h::ReadFile(tile, wxl::ipc::kAnyArchive, adt) == Status::Ok;
            std::printf("    %s: %s, %llu bytes, served flags %u, host %u us\n", tile, tileOk ? "read" : "missing",
                        static_cast<unsigned long long>(adt.size), adt.served, adt.hostUs);
            Check(tileOk && (adt.served & wxl::ipc::kServedPrefetched), (std::string("the tile ") + tile + " was prefetched").c_str());
            std::string texture;
            for (uint64_t at = 0; tileOk && at + 8 <= adt.size;)
            {
                const uint32_t size = U32(adt.data + at + 4);
                if (std::memcmp(adt.data + at, "XETM", 4) == 0 && size > 1)
                {
                    texture.assign(reinterpret_cast<const char*>(adt.data + at + 8));
                    break;
                }
                at += 8 + uint64_t(size);
            }
            h::ReleaseFile(adt);
            if (!texture.empty())
            {
                Sleep(500);
                h::ReadOptions options;
                options.flags = wxl::ipc::kFlagTexture;
                options.a4 = 16384;
                h::FileData image;
                const bool ok = h::ReadFile(texture.c_str(), wxl::ipc::kAnyArchive, image, 0, &options) == Status::Ok;
                std::printf("    %s: %s\n", texture.c_str(), ok && (image.served & wxl::ipc::kServedPrefetched) ? "prefetched" :
                                                              ok && (image.served & wxl::ipc::kServedFromCache) ? "cached" : "read");
                Check(ok && (image.served & wxl::ipc::kServedFromCache), "a texture the tile names was read ahead");
                h::ReleaseFile(image);
            }
        }
        std::printf("backing references\n");
        {
            const wxl::ipc::HostCounters* hc = h::HostSideCounters();
            const uint64_t before = hc ? hc->dedupeValues.load() : 0;
            std::string pick;
            h::FileData image;
            std::vector<blp::Level> levels;
            for (const std::string& name : other)
            {
                h::ReadOptions options;
                options.flags = wxl::ipc::kFlagTexture;
                options.a4 = 16384;
                if (h::ReadFile(name.c_str(), wxl::ipc::kAnyArchive, image, 0, &options) != Status::Ok) continue;
                if (blp::GpuLevels(image.data, size_t(image.size), levels) && levels[0].bytes >= 4096)
                {
                    pick = name;
                    break;
                }
                h::ReleaseFile(image);
            }
            Check(!pick.empty(), "a DXT texture with a level of at least 4 KB is served");
            if (!pick.empty())
            {
                const uint64_t key = 0x8000000000100000ull;
                const blp::Level l = levels[0];
                std::vector<uint8_t> level(image.data + l.offset, image.data + l.offset + l.bytes);
                h::ReleaseFile(image);
                // The host indexes a served image right after replying; in game the upload comes frames later.
                Sleep(50);
                // What the proxy sends for a whole level: packed rows from the start.
                const uint64_t rowBytes = l.bytes / 16 ? l.bytes / 16 : l.bytes;
                const uint64_t rows = l.bytes / rowBytes;
                Check(h::StorePatchRows(key, 0, rowBytes, level.data(), rowBytes, rows, rowBytes), "the level is stored as a backing copy");
                std::vector<uint8_t> back(level.size());
                uint64_t full = 0;
                Check(h::StoreGet(key, back.data(), back.size(), &full, 5000) && back == level, "it reads back identical");
                h::Params p;
                p.op = wxl::ipc::Op::Stats;
                h::Reply r;
                h::Call(p, r, 1000);
                Check(hc && hc->dedupeValues.load() > before, "it is kept as a reference to the texture, not as bytes");
                std::printf("    %s level 0: %u bytes, references %llu (%.1f MB)\n", pick.c_str(), l.bytes,
                            static_cast<unsigned long long>(hc ? hc->dedupeValues.load() : 0),
                            hc ? hc->dedupeBytes.load() / (1024.0 * 1024.0) : 0.0);
                const char patch[] = "patched";
                Check(h::StorePatch(key, 64, patch, sizeof patch), "a patch on a reference");
                std::memcpy(level.data() + 64, patch, sizeof patch);
                Check(h::StoreGet(key, back.data(), back.size(), &full, 5000) && back == level,
                      "the reference becomes bytes with the patch applied");
                h::StoreDrop(key);
            }
        }

        Tally pal, rest;
        size_t printed = 0;
        for (const std::string& name : palettized)
        {
            std::vector<uint8_t> file;
            if (!Expected(client, extracted, name, file)) continue;
            CompareOne(name, file, engine, pal, printed++ < 40);
        }
        Report("palettized", pal);
        Check(pal.decoded > 0 && pal.differ == 0 && pal.missing == 0,
              engine ? "every decoded palettized image equals the loader's own decode, level by level"
                     : "every decoded palettized image equals this build's decode");
        for (const std::string& name : other)
        {
            std::vector<uint8_t> file;
            if (!Expected(client, extracted, name, file)) continue;
            CompareOne(name, file, engine, rest, printed++ < 80);
        }
        Report("DXT and ARGB", rest);
        Check(rest.raw > 0 && rest.differ == 0 && rest.decoded == 0, "DXT and ARGB textures are served as the file itself");
        if (engine)
            Check(pal.stubSame == pal.stubChecked && rest.stubSame == rest.stubChecked && pal.stubChecked && rest.stubChecked,
                  "through the header stand-in, the loader uploads exactly what it uploads from the file");

        // Second pass over the same names: every image now comes from the host cache.
        {
            Tally again;
            const size_t n = std::min<size_t>(palettized.size(), 200);
            const uint64_t t0 = wxl::ipc::NowUs();
            for (size_t i = 0; i < n; ++i)
            {
                std::vector<uint8_t> file;
                if (Expected(client, extracted, palettized[i], file)) CompareOne(palettized[i], file, false, again, false);
            }
            std::printf("  again from the cache: %zu palettized, %.0f us per texture read (%.0f us with the probe's own work)\n",
                        again.files, again.files ? double(again.hostUs) / again.files : 0.0,
                        again.files ? double(wxl::ipc::NowUs() - t0) / again.files : 0.0);
        }

        return g_failures;
    }
}
