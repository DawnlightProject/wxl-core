// wxl-host: the 64-bit companion process. Maps the client's session, serves its rings, dies with it.
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

// Threads: the dispatcher (enkiTS thread 0) polls every channel, spins a little, then sleeps on the doorbell;
// enkiTS workers serve reads and jobs; one store thread applies store operations in arrival order; one
// background thread prefetches; the heartbeat thread proves liveness every 100 ms and exits the process when
// Wow.exe is gone. Archive thread slots: 0 dispatcher, 1..workers, then the store thread, then prefetch.

#include "host/Archives.hpp"
#include "host/Assets.hpp"
#include "host/Cache.hpp"
#include "host/Prefetch.hpp"
#include "host/Store.hpp"

#include "common/Log.hpp"
#include "ipc/Profile.hpp"
#include "ipc/Protocol.hpp"
#include "ipc/Ring.hpp"

#include "TaskScheduler.h"
#include "lz4.h"
#include "zstd.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wxl::hostd
{
    namespace
    {
        using ipc::Op;
        using ipc::Request;
        using ipc::Status;

        constexpr uint32_t kMaxTasks = 512;
        constexpr uint64_t kSweepIdleMs = 60000;
        constexpr uint64_t kSweepBudget = 32ull << 20;

        struct HostChannel
        {
            ipc::Channel*     shared = nullptr;
            ipc::RequestRing  req;
            ipc::ResponseRing resp;
            std::atomic_flag  produce = ATOMIC_FLAG_INIT;
            HANDLE            event = nullptr;
        };

        struct Session
        {
            uint32_t      pid = 0;
            uint32_t      generation = 0;
            HANDLE        ctlSection = nullptr;
            HANDLE        winSection = nullptr;
            uint8_t*      ctl = nullptr;
            ipc::Header*  header = nullptr;
            uint8_t*      window = nullptr;
            uint64_t      windowSize = 0;
            HANDLE        doorbell = nullptr;
            HANDLE        ready = nullptr;
            HANDLE        parent = nullptr;
            uint32_t      spinUs = 20;
        };

        Session              g_s;
        HostChannel          g_ch[ipc::kChannelCount];
        std::atomic<bool>    g_exit{ false };
        enki::TaskScheduler  g_ts;
        uint32_t             g_storeSlot = 0;
        uint32_t             g_prefetchSlot = 0;

        ipc::HostCounters& Counters() { return g_s.header->counters; }

        /// A window range, or null when it falls outside the window.
        uint8_t* WindowRange(uint64_t offset, uint64_t size)
        {
            if (offset > g_s.windowSize || size > g_s.windowSize - offset) return nullptr;
            return g_s.window + offset;
        }

        // --- responses -------------------------------------------------------------------------------

        void Respond(uint32_t c, const Request& req, Status status, uint64_t r0 = 0, uint64_t r1 = 0,
                     uint64_t r2 = 0, uint64_t r3 = 0, uint64_t r4 = 0)
        {
            HostChannel& ch = g_ch[c];
            const uint64_t giveUp = ipc::NowUs() + 2000000;
            uint32_t round = 0;
            for (;;)
            {
                while (ch.produce.test_and_set(std::memory_order_acquire)) ipc::SpinStep(round);
                if (ipc::Response* slot = ch.resp.Reserve())
                {
                    slot->op = req.op;
                    slot->status = static_cast<int32_t>(status);
                    slot->ticket = req.ticket;
                    slot->r0 = r0; slot->r1 = r1; slot->r2 = r2; slot->r3 = r3; slot->r4 = r4;
                    slot->reserved = 0;
                    ch.resp.Push();
                    ch.produce.clear(std::memory_order_release);
                    break;
                }
                ch.produce.clear(std::memory_order_release);
                // The client has not drained this channel: wait for it rather than drop the answer.
                if (ipc::NowUs() > giveUp || g_exit.load())
                {
                    Counters().errors.fetch_add(1, std::memory_order_relaxed);
                    WLOG_WARN("host: response ring %u stayed full; answer to op %u dropped", c, req.op);
                    return;
                }
                if (ch.shared->clientWaiting.exchange(0)) SetEvent(ch.event);
                Sleep(0);
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (ch.shared->clientWaiting.exchange(0, std::memory_order_seq_cst)) SetEvent(ch.event);
        }

        // --- work ------------------------------------------------------------------------------------

        void HandleRead(uint32_t c, const Request& req, uint32_t thread)
        {
            WXL_ZONE("host.read");
            const uint64_t t0 = ipc::NowUs();
            const auto dest = static_cast<ipc::Dest>(req.a1);
            const uint64_t cap = req.a3;
            uint8_t* dst = nullptr;
            HANDLE section = nullptr;
            void* view = nullptr;

            if (dest == ipc::Dest::Window)
            {
                dst = WindowRange(req.a2, cap);
                if (!dst) { Respond(c, req, Status::BadRequest); return; }
            }
            else if (dest == ipc::Dest::BigSection)
            {
                wchar_t name[128];
                ipc::BigSectionName(name, 128, g_s.pid, req.a2);
                section = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
                if (section) view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, SIZE_T(std::max<uint64_t>(cap, 1)));
                if (!view)
                {
                    if (section) CloseHandle(section);
                    Respond(c, req, Status::Failed);
                    return;
                }
                dst = static_cast<uint8_t*>(view);
            }
            else
            {
                Respond(c, req, Status::BadRequest);
                return;
            }

            const bool texture = (req.flags & ipc::kFlagTexture) != 0;
            assets::Served served;
            Status s;
            if (texture)
            {
                const uint32_t maxEdge = uint32_t(req.a4);
                prefetch::SetMaxEdge(maxEdge);
                s = assets::ReadTexture(thread, req.name, uint32_t(req.a0), maxEdge, cap, served);
            }
            else
            {
                s = assets::ReadFile(thread, req.name, uint32_t(req.a0), dst, cap, served);
            }
            if (s == Status::Ok && served.bytes)
            {
                if (served.size > cap) s = Status::NeedMore;
                else std::memcpy(dst, served.bytes->data(), size_t(served.size));
            }
            if (view) UnmapViewOfFile(view);
            if (section) CloseHandle(section);

            if (s == Status::Ok) Counters().readBytes.fetch_add(served.size, std::memory_order_relaxed);
            else if (s == Status::NotFound) Counters().readMisses.fetch_add(1, std::memory_order_relaxed);
            else if (s == Status::NeedMore) Counters().readNeedMore.fetch_add(1, std::memory_order_relaxed);
            Respond(c, req, s, served.size, served.archive, uint64_t(served.image), served.flags, ipc::NowUs() - t0);
            if (s != Status::Ok) return;
            // The upload that follows is at least a main-thread completion away; a late index only costs a
            // backing copy stored as bytes.
            if (texture && served.immutable) store::IndexImage(served.key, served.bytes);
            prefetch::Served(req.name, served.bytes);
        }

        void HandleJob(uint32_t c, const Request& req, uint32_t thread)
        {
            WXL_ZONE("host.job");
            const auto kind = static_cast<ipc::JobKind>(req.a0);
            if (kind == ipc::JobKind::Prefetch)
            {
                // Into the cache when there is one, so the read that follows is served from memory.
                if (cache::Enabled())
                {
                    const cache::Blob bytes = assets::Prefetch(thread, req.name, false, 0, nullptr);
                    Respond(c, req, bytes ? Status::Ok : Status::NotFound, bytes ? bytes->size() : 0);
                    return;
                }
                std::vector<uint8_t> bytes;
                const Status s = archives::ReadAll(thread, req.name, bytes);
                Respond(c, req, s, bytes.size());
                return;
            }

            const uint8_t* in = WindowRange(req.a1, req.a2);
            uint8_t* out = WindowRange(req.a3, req.a4);
            if (!in || !out || req.a2 > 0x7FFFFFFF || req.a4 > 0x7FFFFFFF) { Respond(c, req, Status::BadRequest); return; }

            int64_t produced = -1;
            switch (kind)
            {
            case ipc::JobKind::Lz4Compress:
                produced = LZ4_compress_default(reinterpret_cast<const char*>(in), reinterpret_cast<char*>(out),
                                                int(req.a2), int(req.a4));
                if (produced == 0) produced = -1;
                break;
            case ipc::JobKind::Lz4Decompress:
                produced = LZ4_decompress_safe(reinterpret_cast<const char*>(in), reinterpret_cast<char*>(out),
                                               int(req.a2), int(req.a4));
                break;
            case ipc::JobKind::ZstdCompress:
            {
                const size_t n = ZSTD_compress(out, size_t(req.a4), in, size_t(req.a2), 3);
                produced = ZSTD_isError(n) ? -1 : int64_t(n);
                break;
            }
            case ipc::JobKind::ZstdDecompress:
            {
                const size_t n = ZSTD_decompress(out, size_t(req.a4), in, size_t(req.a2));
                produced = ZSTD_isError(n) ? -1 : int64_t(n);
                break;
            }
            default:
                Respond(c, req, Status::Unsupported);
                return;
            }
            if (produced < 0) Respond(c, req, Status::Failed);
            else Respond(c, req, Status::Ok, uint64_t(produced));
        }

        /// A request carried to a worker. Pooled: reused once complete.
        struct RequestTask final : enki::ITaskSet
        {
            uint32_t channel = 0;
            Request  req{};

            void ExecuteRange(enki::TaskSetPartition, uint32_t thread) override
            {
                const uint64_t t0 = ipc::NowUs();
                if (static_cast<Op>(req.op) == Op::Read)
                {
                    HandleRead(channel, req, thread);
                    prefetch::OnDemandEnd();
                }
                else
                {
                    HandleJob(channel, req, thread);
                }
                Counters().busyNs.fetch_add((ipc::NowUs() - t0) * 1000, std::memory_order_relaxed);
            }
        };

        std::vector<std::unique_ptr<RequestTask>> g_tasks;
        size_t g_taskCursor = 0;

        /// A finished task to reuse, or a new one; waits for one when the pool is at its cap.
        RequestTask* FreeTask()
        {
            for (size_t n = 0; n < g_tasks.size(); ++n)
            {
                RequestTask* t = g_tasks[g_taskCursor].get();
                g_taskCursor = (g_taskCursor + 1) % g_tasks.size();
                if (t->GetIsComplete()) return t;
            }
            if (g_tasks.size() < kMaxTasks)
            {
                g_tasks.push_back(std::make_unique<RequestTask>());
                return g_tasks.back().get();
            }
            RequestTask* t = g_tasks[g_taskCursor].get();
            g_ts.WaitforTask(t);
            return t;
        }

        // --- store thread ----------------------------------------------------------------------------

        std::mutex                                   g_storeMutex;
        std::condition_variable                      g_storeWake;
        std::deque<std::pair<uint32_t, Request>>     g_storeQueue;

        void HandleStore(uint32_t c, const Request& req)
        {
            WXL_ZONE("host.store");
            switch (static_cast<Op>(req.op))
            {
            case Op::StorePut:
            {
                const uint8_t* src = WindowRange(req.a1, req.a2);
                if (!src || req.a3 > uint64_t(ipc::Codec::Zstd)) { Respond(c, req, Status::BadRequest); return; }
                Respond(c, req, store::Put(req.a0, src, req.a2, static_cast<ipc::Codec>(req.a3)));
                return;
            }
            case Op::StorePatch:
            {
                const uint8_t* src = WindowRange(req.a1, req.a2);
                if (!src) { Respond(c, req, Status::BadRequest); return; }
                if (!req.a4)
                {
                    Respond(c, req, store::Patch(req.a0, req.a3, src, req.a2));
                    return;
                }
                const uint64_t rowBytes = req.a4 >> 32;
                const uint64_t pitch = req.a4 & 0xFFFFFFFFull;
                if (!rowBytes || rowBytes > pitch || req.a2 % rowBytes) { Respond(c, req, Status::BadRequest); return; }
                Respond(c, req, store::PatchRows(req.a0, req.a3, pitch, src, rowBytes, req.a2 / rowBytes));
                return;
            }
            case Op::StoreGet:
            {
                uint8_t* dst = WindowRange(req.a1, req.a2);
                if (!dst) { Respond(c, req, Status::BadRequest); return; }
                uint64_t full = 0;
                const Status s = store::Get(req.a0, dst, req.a2, full);
                Respond(c, req, s, full);
                return;
            }
            case Op::StoreDrop:
                for (uint64_t i = 0, n = req.a1 ? req.a1 : 1; i < n; ++i) store::Drop(req.a0 + i);
                Respond(c, req, Status::Ok);
                return;
            default:
                Respond(c, req, Status::BadRequest);
                return;
            }
        }

        void StoreThread()
        {
            WXL_THREAD_NAME("wxl-host store");
            ULONGLONG lastSweep = GetTickCount64();
            for (;;)
            {
                std::pair<uint32_t, Request> item;
                {
                    std::unique_lock<std::mutex> lock(g_storeMutex);
                    g_storeWake.wait_for(lock, std::chrono::seconds(5),
                                         [] { return g_exit.load() || !g_storeQueue.empty(); });
                    if (g_exit.load()) return;
                    if (g_storeQueue.empty())
                    {
                        lock.unlock();
                        // Idle: move a slice of cold values to zstd.
                        if (GetTickCount64() - lastSweep > 30000)
                        {
                            if (!store::SweepCold(kSweepIdleMs, kSweepBudget)) lastSweep = GetTickCount64();
                        }
                        continue;
                    }
                    item = g_storeQueue.front();
                    g_storeQueue.pop_front();
                }
                const uint64_t t0 = ipc::NowUs();
                HandleStore(item.first, item.second);
                Counters().busyNs.fetch_add((ipc::NowUs() - t0) * 1000, std::memory_order_relaxed);
            }
        }

        // --- dispatch --------------------------------------------------------------------------------

        void RefreshProcessCounters()
        {
            PROCESS_MEMORY_COUNTERS_EX pmc{};
            pmc.cb = sizeof pmc;
            if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof pmc))
            {
                Counters().workingSet.store(pmc.WorkingSetSize);
                Counters().privateBytes.store(pmc.PrivateUsage);
            }
        }

        void Handle(uint32_t c, const Request& req)
        {
            const auto op = static_cast<Op>(req.op);
            if (req.op < static_cast<uint32_t>(Op::Count))
                Counters().requests[req.op].fetch_add(1, std::memory_order_relaxed);

            switch (op)
            {
            case Op::Ping:
            {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);
                Respond(c, req, Status::Ok, uint64_t(now.QuadPart));
                return;
            }
            case Op::Mount:
            {
                uint32_t id = 0, kind = 0, error = 0;
                const Status s = archives::Mount(0, req.name, int(int64_t(req.a0)), uint32_t(req.a1), id, kind, error);
                if (s == Status::Ok) Counters().mounts.fetch_add(1, std::memory_order_relaxed);
                Respond(c, req, s, id, kind, error);
                return;
            }
            case Op::Unmount:
                archives::Unmount(uint32_t(req.a0));
                cache::DropStorage(uint32_t(req.a0));
                Respond(c, req, Status::Ok);
                return;
            case Op::Stat:
            {
                uint64_t size = 0;
                uint32_t where = ipc::kAnyArchive;
                const Status s = archives::Stat(0, req.name, uint32_t(req.a0), size, where);
                Respond(c, req, s, size, where);
                return;
            }
            case Op::Read:
            case Op::Job:
            {
                if (op == Op::Read) prefetch::OnDemandBegin();
                RequestTask* t = FreeTask();
                t->channel = c;
                t->req = req;
                g_ts.AddTaskSetToPipe(t);
                return;
            }
            case Op::Hint:
            {
                auto f = [](uint64_t v, int half) {
                    const uint32_t bits = uint32_t(v >> (half * 32));
                    float out;
                    std::memcpy(&out, &bits, 4);
                    return out;
                };
                prefetch::Hint(req.name, f(req.a0, 0), f(req.a0, 1), f(req.a1, 0), f(req.a1, 1), f(req.a2, 0));
                Respond(c, req, Status::Ok);
                return;
            }
            case Op::StorePut:
            case Op::StorePatch:
            case Op::StoreGet:
            case Op::StoreDrop:
            {
                {
                    std::lock_guard<std::mutex> lock(g_storeMutex);
                    g_storeQueue.emplace_back(c, req);
                }
                g_storeWake.notify_one();
                return;
            }
            case Op::Stats:
                RefreshProcessCounters();
                Respond(c, req, Status::Ok);
                return;
            case Op::Shutdown:
                Respond(c, req, Status::Ok);
                g_exit.store(true);
                return;
            default:
                Respond(c, req, Status::BadRequest);
                return;
            }
        }

        bool AnyRequest()
        {
            for (HostChannel& ch : g_ch)
                if (!ch.req.Empty()) return true;
            return false;
        }

        void Dispatch()
        {
            WXL_THREAD_NAME("wxl-host dispatcher");
            uint64_t lastWork = ipc::NowUs();
            uint32_t round = 0;
            while (!g_exit.load(std::memory_order_relaxed))
            {
                bool any = false;
                for (uint32_t c = 0; c < ipc::kChannelCount; ++c)
                {
                    HostChannel& ch = g_ch[c];
                    while (const Request* r = ch.req.Front())
                    {
                        const Request copy = *r;
                        ch.req.Pop();
                        Handle(c, copy);
                        any = true;
                    }
                }
                if (any) { lastWork = ipc::NowUs(); continue; }
                if (ipc::NowUs() - lastWork < g_s.spinUs) { ipc::SpinStep(round); continue; }

                g_s.header->hostSleeping.store(1, std::memory_order_seq_cst);
                std::atomic_thread_fence(std::memory_order_seq_cst);
                if (!AnyRequest())
                {
                    WaitForSingleObject(g_s.doorbell, 100);
                    Counters().wakeups.fetch_add(1, std::memory_order_relaxed);
                }
                g_s.header->hostSleeping.store(0, std::memory_order_relaxed);
                lastWork = ipc::NowUs();
            }
        }

        void Heartbeat()
        {
            WXL_THREAD_NAME("wxl-host heartbeat");
            for (uint32_t tick = 0; !g_exit.load(); ++tick)
            {
                g_s.header->heartbeat.fetch_add(1, std::memory_order_relaxed);
                if (g_s.parent && WaitForSingleObject(g_s.parent, 0) != WAIT_TIMEOUT)
                {
                    WLOG_INFO("host: Wow.exe is gone; exiting");
                    wxl::log::Flush();
                    ExitProcess(0);
                }
                if (tick % 10 == 0)
                {
                    RefreshProcessCounters();
                    wxl::log::Flush();   // the job kills this process with Wow.exe, before any orderly exit
                }
                Sleep(100);
            }
        }

        // --- session ---------------------------------------------------------------------------------

        bool OpenSession()
        {
            wchar_t name[128];
            ipc::ControlName(name, 128, g_s.pid);
            g_s.ctlSection = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
            if (!g_s.ctlSection) { WLOG_ERROR("host: no session for pid %u (win32 %lu)", g_s.pid, GetLastError()); return false; }
            g_s.ctl = static_cast<uint8_t*>(MapViewOfFile(g_s.ctlSection, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(ipc::ControlSize())));
            if (!g_s.ctl) return false;
            g_s.header = reinterpret_cast<ipc::Header*>(g_s.ctl);

            ipc::ReadyName(name, 128, g_s.pid);
            g_s.ready = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);

            ipc::Header* h = g_s.header;
            h->hostVersion = ipc::kProtocolVersion;
            const bool layoutOk = h->magic == ipc::kMagic && h->version == ipc::kProtocolVersion
                && h->headerSize == sizeof(ipc::Header) && h->channelSize == sizeof(ipc::Channel)
                && h->requestSize == sizeof(ipc::Request) && h->responseSize == sizeof(ipc::Response)
                && h->channelCount == ipc::kChannelCount && h->requestSlots == ipc::kRequestSlots
                && h->responseSlots == ipc::kResponseSlots && h->controlSize == ipc::ControlSize();
            if (!layoutOk || h->generation != g_s.generation)
            {
                WLOG_ERROR("host: session refused: client protocol %u (header %u, channel %u), host protocol %u "
                           "(header %zu, channel %zu), generation %u vs %u",
                           h->version, h->headerSize, h->channelSize, ipc::kProtocolVersion, sizeof(ipc::Header),
                           sizeof(ipc::Channel), h->generation, g_s.generation);
                h->hostState.store(static_cast<uint32_t>(ipc::HostState::Refused));
                if (g_s.ready) SetEvent(g_s.ready);
                return false;
            }

            g_s.windowSize = h->windowSize;
            g_s.spinUs = h->spinUs;
            ipc::WindowName(name, 128, g_s.pid);
            g_s.winSection = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
            if (!g_s.winSection) return false;
            g_s.window = static_cast<uint8_t*>(MapViewOfFile(g_s.winSection, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(g_s.windowSize)));
            if (!g_s.window) return false;

            ipc::DoorbellName(name, 128, g_s.pid);
            g_s.doorbell = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
            if (!g_s.doorbell) return false;
            for (uint32_t c = 0; c < ipc::kChannelCount; ++c)
            {
                HostChannel& ch = g_ch[c];
                ch.shared = reinterpret_cast<ipc::Channel*>(g_s.ctl + ipc::ChannelOffset(c));
                ch.req = ipc::RequestRing(&ch.shared->reqWrite, &ch.shared->reqRead, ch.shared->requests);
                ch.resp = ipc::ResponseRing(&ch.shared->respWrite, &ch.shared->respRead, ch.shared->responses);
                ch.req.Resync();
                ch.resp.Resync();
                ipc::ChannelEventName(name, 128, g_s.pid, c);
                ch.event = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, name);
                if (!ch.event) return false;
            }
            g_s.parent = OpenProcess(SYNCHRONIZE, FALSE, g_s.pid);
            return true;
        }

        LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info)
        {
            WLOG_ERROR("host: crashed with exception 0x%08lX at %p", info->ExceptionRecord->ExceptionCode,
                       info->ExceptionRecord->ExceptionAddress);
            wxl::log::Flush();
            return EXCEPTION_EXECUTE_HANDLER;
        }

        uint32_t WorkerCount(uint32_t requested)
        {
            if (requested) return requested;
            SYSTEM_INFO si{};
            GetSystemInfo(&si);
            const uint32_t cores = si.dwNumberOfProcessors;
            return cores >= 8 ? 3 : 2;   // the client reads from two or three threads at most
        }
    }

    int Run(uint32_t pid, uint32_t generation)
    {
        g_s.pid = pid;
        g_s.generation = generation;
        SetUnhandledExceptionFilter(&CrashFilter);

        CreateDirectoryW(L"Logs", nullptr);
        char logName[64];
        if (generation <= 1) std::snprintf(logName, sizeof logName, "Logs\\wxl-host.log");
        else std::snprintf(logName, sizeof logName, "Logs\\wxl-host-%u.log", generation);
        wxl::log::Open(logName);
        WLOG_INFO("wxl-host starting (build %s %s), client pid %u, generation %u", __DATE__, __TIME__, pid, generation);

        if (!OpenSession())
        {
            wxl::log::Flush();
            return 2;
        }

        wchar_t root[ipc::kRootMax];
        MultiByteToWideChar(CP_UTF8, 0, g_s.header->clientRoot, -1, root, ipc::kRootMax);
        const uint32_t workers = WorkerCount(g_s.header->workerCount);
        g_ts.Initialize(workers + 1);   // thread 0 is this dispatcher
        g_storeSlot = workers + 1;
        g_prefetchSlot = workers + 2;
        archives::Init(root, workers + 3);
        cache::Init(g_s.header->cacheLimit, &g_s.header->counters);
        assets::Init(&g_s.header->counters);
        store::Init(pid, g_s.header->storeLimit, &g_s.header->counters, g_storeSlot, g_s.header->dedupe != 0);
        prefetch::Init(g_prefetchSlot, workers, &g_s.header->counters, g_s.header->prefetch != 0);
        prefetch::Start();

        std::thread storeThread(&StoreThread);
        std::thread heartbeat(&Heartbeat);

        g_s.header->hostPid = GetCurrentProcessId();
        g_s.header->hostGeneration = generation;
        g_s.header->hostState.store(static_cast<uint32_t>(ipc::HostState::Ready), std::memory_order_release);
        SetEvent(g_s.ready);
        WLOG_INFO("host: serving (window %llu MB, %u workers, spin %u us, store up to %llu MB, cache %llu MB, "
                  "prefetch %s, backing references %s)",
                  static_cast<unsigned long long>(g_s.windowSize >> 20), workers, g_s.spinUs,
                  static_cast<unsigned long long>(g_s.header->storeLimit >> 20),
                  static_cast<unsigned long long>(g_s.header->cacheLimit >> 20),
                  g_s.header->prefetch && g_s.header->cacheLimit ? "on" : "off",
                  g_s.header->dedupe && g_s.header->cacheLimit ? "on" : "off");
        wxl::log::Flush();

        Dispatch();

        WLOG_INFO("host: shutting down");
        g_s.header->hostState.store(static_cast<uint32_t>(ipc::HostState::Exiting));
        prefetch::Stop();
        g_storeWake.notify_all();
        storeThread.join();
        heartbeat.join();
        g_ts.WaitforAllAndShutdown();
        wxl::log::Flush();
        return 0;
    }
}

int wmain(int argc, wchar_t** argv)
{
    uint32_t pid = 0, generation = 1;
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!wcscmp(argv[i], L"--pid")) pid = uint32_t(_wtoi(argv[i + 1]));
        else if (!wcscmp(argv[i], L"--gen")) generation = uint32_t(_wtoi(argv[i + 1]));
    }
    if (!pid)
    {
        std::fwprintf(stderr, L"wxl-host: started by WarcraftXL.dll, not by hand (--pid <client pid> --gen <n>)\n");
        return 1;
    }
    return wxl::hostd::Run(pid, generation);
}
