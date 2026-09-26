// Publishes wxl.host: the C API extensions (and the d3d9 proxy) use to reach wxl-host.
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

#include "runtime/host/HostClient.hpp"

#include "common/Log.hpp"
#include "engine/diag/MemoryDiag.hpp"
#include "engine/storage/HostTextures.hpp"
#include "runtime/Extensions.hpp"
#include "wxl/HostApi.h"

#include <windows.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace
{
    namespace h = wxl::host;
    using wxl::ipc::Status;

    int ToApi(Status s)
    {
        switch (s)
        {
        case Status::Ok:         return WXL_HOST_OK;
        case Status::NotFound:   return WXL_HOST_E_NOT_FOUND;
        case Status::Busy:       return WXL_HOST_E_TIMEOUT;
        case Status::BadRequest: return WXL_HOST_E_BAD_ARGUMENT;
        default:                 return h::Available() ? WXL_HOST_E_FAILED : WXL_HOST_E_UNAVAILABLE;
        }
    }

    // --- blocks handed out ----------------------------------------------------------------------
    // Every block the API returns is recorded under an id, so Release frees exactly what was given.

    struct Held
    {
        h::FileData file;   // a file read (window block or big section)
        h::Block    block;  // an Alloc
    };

    std::mutex                              g_heldMutex;
    std::unordered_map<uint64_t, Held>      g_held;
    std::atomic<uint64_t>                   g_nextId{ 1 };

    void Hand(Held&& held, void* data, uint64_t size, WXL_HostBlock* out)
    {
        const uint64_t id = g_nextId.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(g_heldMutex);
            g_held.emplace(id, std::move(held));
        }
        out->data = data;
        out->size = size;
        out->id = id;
        out->reserved = 0;
    }

    // --- tickets --------------------------------------------------------------------------------

    struct ApiTicket
    {
        h::ReadOp*     read = nullptr;   // a ReadFileAsync
        uint64_t       job = 0;          // a SubmitJob host ticket
        WXL_HostBlock* jobOut = nullptr;
    };

    std::mutex                              g_ticketMutex;
    std::unordered_map<uint64_t, ApiTicket> g_tickets;

    int Finish(uint64_t ticket, WXL_HostBlock* out, uint32_t waitMs)
    {
        ApiTicket t;
        {
            std::lock_guard<std::mutex> lock(g_ticketMutex);
            const auto it = g_tickets.find(ticket);
            if (it == g_tickets.end()) return WXL_HOST_E_BAD_ARGUMENT;
            t = it->second;
        }

        int result = WXL_HOST_PENDING;
        if (t.read)
        {
            h::FileData file;
            Status status = Status::Failed;
            if (!h::ReadStep(t.read, file, status, waitMs)) return WXL_HOST_PENDING;
            result = ToApi(status);
            if (status == Status::Ok && out)
            {
                const void* data = file.data;
                const uint64_t size = file.size;
                Held held;
                held.file = file;
                Hand(std::move(held), const_cast<void*>(data), size, out);
            }
            else if (status == Status::Ok)
            {
                h::ReleaseFile(file);
            }
        }
        else
        {
            h::Reply r;
            const bool done = waitMs ? h::Wait(t.job, r, waitMs) : h::Poll(t.job, r) == 1;
            if (!done)
            {
                if (waitMs || h::Poll(t.job, r) < 0) result = WXL_HOST_E_TIMEOUT;
                else return WXL_HOST_PENDING;
            }
            else
            {
                result = ToApi(r.status);
                if (r.status == Status::Ok && out) out->size = r.r0;
            }
        }

        std::lock_guard<std::mutex> lock(g_ticketMutex);
        g_tickets.erase(ticket);
        return result;
    }

    uint64_t NewTicket(const ApiTicket& t)
    {
        static std::atomic<uint64_t> next{ 1 };
        const uint64_t id = next.fetch_add(1);
        std::lock_guard<std::mutex> lock(g_ticketMutex);
        g_tickets.emplace(id, t);
        return id;
    }

    // --- the table ------------------------------------------------------------------------------

    int __cdecl ApiIsAvailable()
    {
        return h::Available() ? 1 : 0;
    }

    void __cdecl ApiGetStats(WXL_HostStats* out)
    {
        if (!out) return;
        std::memset(out, 0, sizeof *out);
        out->structSize = sizeof *out;
        out->available = h::Available() ? 1 : 0;
        out->generation = h::Generation();
        h::Counters c{};
        h::ReadCounters(c);
        out->restarts = static_cast<uint32_t>(c.restarts);
        out->requests = c.calls;
        out->readBytes = c.readBytes;
        out->readCount = c.readCount;
        out->readAvgUs = static_cast<uint32_t>(c.readCount ? c.readUsTotal / c.readCount : 0);
        out->readMaxUs = static_cast<uint32_t>(c.readUsMax);
        out->pingAvgUs = static_cast<uint32_t>(c.pingCount ? c.pingUsTotal / c.pingCount : 0);
        out->pingMaxUs = static_cast<uint32_t>(c.pingUsMax);
        out->windowSize = c.windowSize;
        out->windowUsed = c.windowUsed;
        out->windowPeak = c.windowPeak;
        out->fallbacks = c.fallbacks;
        if (const wxl::ipc::HostCounters* hc = h::HostSideCounters())
        {
            out->storeBytes = hc->storeBytes.load();
            out->storeRawBytes = hc->storeRawBytes.load();
            out->storeEntries = hc->storeEntries.load();
            out->hostPrivateBytes = hc->privateBytes.load();
        }
    }

    int __cdecl ApiReadFile(const char* name, uint32_t, WXL_HostBlock* out, uint32_t timeoutMs)
    {
        if (!name || !out) return WXL_HOST_E_BAD_ARGUMENT;
        std::memset(out, 0, sizeof *out);
        if (!h::Available()) return WXL_HOST_E_UNAVAILABLE;
        h::FileData file;
        const Status s = h::ReadFile(name, wxl::ipc::kAnyArchive, file, timeoutMs);
        if (s != Status::Ok) return ToApi(s);
        const void* data = file.data;
        const uint64_t size = file.size;
        Held held;
        held.file = file;
        Hand(std::move(held), const_cast<void*>(data), size, out);
        return WXL_HOST_OK;
    }

    int __cdecl ApiReadFileAsync(const char* name, uint32_t, uint64_t* ticket)
    {
        if (!name || !ticket) return WXL_HOST_E_BAD_ARGUMENT;
        *ticket = 0;
        h::ReadOp* op = h::ReadBegin(name, wxl::ipc::kAnyArchive);
        if (!op) return WXL_HOST_E_UNAVAILABLE;
        ApiTicket t;
        t.read = op;
        *ticket = NewTicket(t);
        return WXL_HOST_OK;
    }

    int __cdecl ApiPoll(uint64_t ticket, WXL_HostBlock* out)
    {
        return Finish(ticket, out, 0);
    }

    int __cdecl ApiWait(uint64_t ticket, WXL_HostBlock* out, uint32_t timeoutMs)
    {
        return Finish(ticket, out, timeoutMs ? timeoutMs : 8000);
    }

    int __cdecl ApiFileExists(const char* name, uint64_t* size)
    {
        if (!name) return WXL_HOST_E_BAD_ARGUMENT;
        if (!h::Available()) return WXL_HOST_E_UNAVAILABLE;
        return ToApi(h::StatFile(name, wxl::ipc::kAnyArchive, 0, size, nullptr));
    }

    int __cdecl ApiAlloc(uint64_t size, WXL_HostBlock* out)
    {
        if (!out) return WXL_HOST_E_BAD_ARGUMENT;
        std::memset(out, 0, sizeof *out);
        if (!h::Available()) return WXL_HOST_E_UNAVAILABLE;
        Held held;
        if (!h::Alloc(size, held.block)) return WXL_HOST_E_NO_MEMORY;
        void* data = held.block.data;
        Hand(std::move(held), data, size, out);
        return WXL_HOST_OK;
    }

    void __cdecl ApiRelease(WXL_HostBlock* block)
    {
        if (!block || !block->id) return;
        Held held;
        {
            std::lock_guard<std::mutex> lock(g_heldMutex);
            const auto it = g_held.find(block->id);
            if (it == g_held.end()) return;
            held = it->second;
            g_held.erase(it);
        }
        if (held.file.data) h::ReleaseFile(held.file);
        if (held.block.data) h::Free(held.block);
        std::memset(block, 0, sizeof *block);
    }

    int __cdecl ApiStorePut(uint64_t key, const void* data, uint64_t size, uint32_t codec, int wait)
    {
        if (!data && size) return WXL_HOST_E_BAD_ARGUMENT;
        if (codec > WXL_HOST_CODEC_ZSTD) return WXL_HOST_E_BAD_ARGUMENT;
        if (!h::Available()) return WXL_HOST_E_UNAVAILABLE;
        return h::StorePut(key, data, size, static_cast<wxl::ipc::Codec>(codec), wait != 0) ? WXL_HOST_OK
                                                                                           : WXL_HOST_E_FAILED;
    }

    int __cdecl ApiStorePatch(uint64_t key, uint64_t offset, const void* data, uint64_t size)
    {
        if (!data || !size) return WXL_HOST_E_BAD_ARGUMENT;
        if (!h::Available()) return WXL_HOST_E_UNAVAILABLE;
        return h::StorePatch(key, offset, data, size) ? WXL_HOST_OK : WXL_HOST_E_FAILED;
    }

    int __cdecl ApiStoreGet(uint64_t key, void* dst, uint64_t cap, uint64_t* size, uint32_t timeoutMs)
    {
        if (!h::Available()) return WXL_HOST_E_UNAVAILABLE;
        uint64_t full = 0;
        const bool ok = h::StoreGet(key, dst, cap, &full, timeoutMs);
        if (size) *size = full;
        if (ok) return WXL_HOST_OK;
        return full ? WXL_HOST_OK : WXL_HOST_E_NOT_FOUND;   // a truncated copy still reports the full size
    }

    void __cdecl ApiStoreDrop(uint64_t key)
    {
        h::StoreDrop(key);
    }

    /// Looks up the window block behind an Alloc'd API block.
    bool HeldBlock(const WXL_HostBlock* b, h::Block& out)
    {
        if (!b || !b->id) return false;
        std::lock_guard<std::mutex> lock(g_heldMutex);
        const auto it = g_held.find(b->id);
        if (it == g_held.end() || !it->second.block.data) return false;
        out = it->second.block;
        return true;
    }

    int __cdecl ApiSubmitJob(uint32_t kind, const WXL_HostBlock* in, uint64_t inSize, WXL_HostBlock* out,
                             const char* name, uint64_t* ticket)
    {
        if (!ticket) return WXL_HOST_E_BAD_ARGUMENT;
        *ticket = 0;
        if (!h::Available()) return WXL_HOST_E_UNAVAILABLE;
        h::Block inBlock, outBlock;
        const bool prefetch = kind == WXL_HOST_JOB_PREFETCH;
        if (!prefetch)
        {
            if (kind < WXL_HOST_JOB_LZ4_COMPRESS || kind > WXL_HOST_JOB_ZSTD_DECOMPRESS) return WXL_HOST_E_BAD_ARGUMENT;
            if (!HeldBlock(in, inBlock) || !HeldBlock(out, outBlock) || inSize > inBlock.size)
                return WXL_HOST_E_BAD_ARGUMENT;
        }
        else if (!name)
        {
            return WXL_HOST_E_BAD_ARGUMENT;
        }

        h::Params p;
        p.op = wxl::ipc::Op::Job;
        p.a0 = kind;
        p.a1 = inBlock.offset;
        p.a2 = inSize;
        p.a3 = outBlock.offset;
        p.a4 = outBlock.size;
        p.name = name;
        ApiTicket t;
        t.job = h::Post(p);
        if (!t.job) return WXL_HOST_E_UNAVAILABLE;
        t.jobOut = out;
        *ticket = NewTicket(t);
        return WXL_HOST_OK;
    }

    int __cdecl ApiStorePatchRows(uint64_t key, uint64_t dstOffset, uint64_t dstPitch, const void* src,
                                  uint64_t rowBytes, uint64_t rows, uint64_t srcPitch)
    {
        if (!src || !rowBytes || !rows) return WXL_HOST_E_BAD_ARGUMENT;
        if (!h::Available()) return WXL_HOST_E_UNAVAILABLE;
        return h::StorePatchRows(key, dstOffset, dstPitch, src, rowBytes, rows, srcPitch) ? WXL_HOST_OK
                                                                                          : WXL_HOST_E_FAILED;
    }

    void __cdecl ApiStoreDropRange(uint64_t first, uint32_t count)
    {
        h::StoreDropRange(first, count);
    }

    const WXL_HostApi g_api = {
        sizeof(WXL_HostApi), WXL_HOST_API_VERSION,
        &ApiIsAvailable, &ApiGetStats,
        &ApiReadFile, &ApiReadFileAsync, &ApiPoll, &ApiWait, &ApiFileExists,
        &ApiAlloc, &ApiRelease,
        &ApiStorePut, &ApiStorePatch, &ApiStoreGet, &ApiStoreDrop,
        &ApiSubmitJob,
        &ApiStorePatchRows, &ApiStoreDropRange,
    };

    void FillHostLine(wxl::diag::memory::HostLine& out)
    {
        h::CounterLine(out.text, sizeof out.text);
        // Texture and host-cache counters get lines of their own: the host line is full.
        wxl::runtime::storage::textures::LogCounters("textures");
    }

    struct Registrar
    {
        Registrar()
        {
            wxl::runtime::extensions::PublishInterface("wxl.host", WXL_HOST_API_VERSION,
                                                       const_cast<WXL_HostApi*>(&g_api));
            wxl::diag::memory::SetExtraLine(&FillHostLine);
        }
    } g_registrar;
}

/**
 * @brief The host API for binaries outside the extension loader (the d3d9 proxy).
 * @return the process-lifetime API table.
 */
extern "C" __declspec(dllexport) const WXL_HostApi* __cdecl WXL_GetHostApi()
{
    return &g_api;
}
