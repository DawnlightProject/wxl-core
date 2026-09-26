// The client side of wxl-host: spawns and watches the host, owns the channels and the transfer window.
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

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "ipc/Profile.hpp"
#include "ipc/Ring.hpp"

#include "tlsf.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace wxl::host
{
    namespace
    {
        using ipc::Op;
        using ipc::Status;

        constexpr uint32_t kSliceMs = 20;            // longest single sleep, so a missed wake costs little
        constexpr uint64_t kDefaultReadCap = 256ull << 10;
        constexpr uint64_t kMinBlock = 64;
        constexpr DWORD kMonitorMs = 250;
        constexpr ULONGLONG kHeartbeatStaleMs = 2000;
        constexpr int kMaxRestartsPerMinute = 3;
        constexpr size_t kSizeHintCap = 8192;

        // --- configuration ---------------------------------------------------------------------------

        uint32_t WindowMb()  { static const uint32_t v = config::U32("WXL_HOST_WINDOW_MB", 32, 16, 1024); return v; }
        uint32_t BigKb()     { static const uint32_t v = config::U32("WXL_HOST_BIG_KB", 4096, 256, 262144); return v; }
        uint32_t SpinUs()    { static const uint32_t v = config::U32("WXL_HOST_SPIN_US", 20, 0, 2000); return v; }
        uint32_t StartMs()   { static const uint32_t v = config::U32("WXL_HOST_START_MS", 4000, 500, 30000); return v; }
        uint32_t TimeoutMs() { static const uint32_t v = config::U32("WXL_HOST_TIMEOUT_MS", 8000, 100, 120000); return v; }
        uint32_t Workers()   { static const uint32_t v = config::U32("WXL_HOST_WORKERS", 0, 0, 16); return v; }
        uint64_t StoreMb()   { static const uint64_t v = config::U64("WXL_HOST_STORE_MB", 4096, 0, 1ull << 20); return v; }
        uint64_t CacheMb()   { static const uint64_t v = config::U64("WXL_HOST_CACHE_MB", 1024, 0, 1ull << 20); return v; }
        bool Prefetch()      { static const bool v = config::Env("WXL_HOST_PREFETCH", true); return v; }
        bool BackingRefs()   { static const bool v = config::Env("WXL_HOST_BACKING_REFS", true); return v; }

        // --- session ---------------------------------------------------------------------------------

        struct ClientChannel
        {
            ipc::Channel*         shared = nullptr;
            ipc::RequestRing      req;
            ipc::ResponseRing     resp;
            SRWLOCK               produce = SRWLOCK_INIT;
            SRWLOCK               consume = SRWLOCK_INIT;
            std::atomic<uint32_t> owner{ 0 };
            HANDLE                event = nullptr;
        };

        struct Session
        {
            uint32_t      pid = 0;
            HANDLE        ctlSection = nullptr;
            HANDLE        winSection = nullptr;
            uint8_t*      ctl = nullptr;
            ipc::Header*  header = nullptr;
            uint8_t*      window = nullptr;
            uint64_t      windowSize = 0;
            HANDLE        doorbell = nullptr;
            HANDLE        ready = nullptr;
            HANDLE        job = nullptr;
            HANDLE        process = nullptr;
            uint32_t      hostPid = 0;
            std::wstring  root;
            std::wstring  exe;
        };

        Session                g_s;
        ClientChannel          g_channels[ipc::kChannelCount];
        std::atomic<State>     g_state{ State::Off };
        std::atomic<uint32_t>  g_generation{ 0 };
        std::mutex             g_startMutex;
        SRWLOCK                g_sessionLock = SRWLOCK_INIT;   // shared by calls, exclusive during a reset
        HANDLE                 g_stateEvent = nullptr;          // pulsed on every state change
        void                   (*g_listener)(State) = nullptr;
        std::atomic<uint64_t>  g_nextTicket{ 1 };
        std::atomic<uint64_t>  g_nextBig{ 1 };

        thread_local int t_channel = -1;
        std::wstring     g_rootOverride;

        // --- counters --------------------------------------------------------------------------------

        struct AtomicCounters
        {
            std::atomic<uint64_t> calls{ 0 }, fallbacks{ 0 }, restarts{ 0 };
            std::atomic<uint64_t> readCount{ 0 }, readBytes{ 0 }, readUsTotal{ 0 }, readUsMax{ 0 };
            std::atomic<uint64_t> pingCount{ 0 }, pingUsTotal{ 0 }, pingUsMax{ 0 };
            std::atomic<uint64_t> statCount{ 0 }, statUsTotal{ 0 };
            std::atomic<uint64_t> windowUsed{ 0 }, windowPeak{ 0 };
            std::atomic<uint64_t> bigCount{ 0 }, bigBytes{ 0 };
        } g_c;

        void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
        {
            uint64_t cur = a.load(std::memory_order_relaxed);
            while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
        }

        // --- pending tickets -------------------------------------------------------------------------

        struct Pending
        {
            uint32_t   channel = 0;
            uint32_t   generation = 0;
            Completion done = nullptr;
            void*      ctx = nullptr;
            bool       finished = false;
            Reply      reply;
        };

        SRWLOCK                               g_pendingLock = SRWLOCK_INIT;
        std::unordered_map<uint64_t, Pending> g_pending;

        void SetState(State s)
        {
            g_state.store(s, std::memory_order_release);
            if (g_stateEvent) SetEvent(g_stateEvent);
        }

        /// Finishes a ticket with a reply, running its completion outside the lock.
        void Finish(uint64_t ticket, const Reply& reply)
        {
            Completion done = nullptr;
            void* ctx = nullptr;
            AcquireSRWLockExclusive(&g_pendingLock);
            const auto it = g_pending.find(ticket);
            if (it != g_pending.end())
            {
                if (it->second.done)
                {
                    done = it->second.done;
                    ctx = it->second.ctx;
                    g_pending.erase(it);
                }
                else
                {
                    it->second.finished = true;
                    it->second.reply = reply;
                }
            }
            ReleaseSRWLockExclusive(&g_pendingLock);
            if (done) done(ctx, reply);
        }

        /// Fails every ticket of a lost generation.
        void FailGeneration(uint32_t generation)
        {
            std::vector<std::pair<Completion, void*>> callbacks;
            AcquireSRWLockExclusive(&g_pendingLock);
            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                if (it->second.generation != generation) { ++it; continue; }
                if (it->second.done)
                {
                    callbacks.emplace_back(it->second.done, it->second.ctx);
                    it = g_pending.erase(it);
                }
                else
                {
                    it->second.finished = true;
                    it->second.reply = Reply{};
                    ++it;
                }
            }
            ReleaseSRWLockExclusive(&g_pendingLock);
            const Reply failed{};
            for (auto& [fn, ctx] : callbacks) fn(ctx, failed);
        }

        /// Drains one channel's responses into the ticket table. Skips if another thread is draining it.
        /// The caller holds the session lock (shared), so a restart cannot reset the ring underneath.
        void DrainLocked(uint32_t c)
        {
            ClientChannel& ch = g_channels[c];
            if (!ch.shared || !TryAcquireSRWLockExclusive(&ch.consume)) return;
            while (const ipc::Response* r = ch.resp.Front())
            {
                const ipc::Response copy = *r;
                ch.resp.Pop();
                Reply reply;
                reply.status = static_cast<Status>(copy.status);
                reply.r0 = copy.r0; reply.r1 = copy.r1; reply.r2 = copy.r2; reply.r3 = copy.r3; reply.r4 = copy.r4;
                ReleaseSRWLockExclusive(&ch.consume);
                Finish(copy.ticket, reply);
                if (!TryAcquireSRWLockExclusive(&ch.consume)) return;
            }
            ReleaseSRWLockExclusive(&ch.consume);
        }

        void Drain(uint32_t c)
        {
            AcquireSRWLockShared(&g_sessionLock);
            DrainLocked(c);
            ReleaseSRWLockShared(&g_sessionLock);
        }

        bool ThreadAlive(uint32_t tid)
        {
            HANDLE t = OpenThread(SYNCHRONIZE, FALSE, tid);
            if (!t) return false;
            const bool alive = WaitForSingleObject(t, 0) == WAIT_TIMEOUT;
            CloseHandle(t);
            return alive;
        }

        /// The calling thread's channel: a dedicated one when free, else the shared channel 0.
        uint32_t ChannelForThread()
        {
            const uint32_t tid = GetCurrentThreadId();
            if (t_channel >= 0 && g_channels[t_channel].owner.load(std::memory_order_relaxed) == tid)
                return static_cast<uint32_t>(t_channel);
            if (t_channel == 0) return 0;

            for (uint32_t c = 1; c < ipc::kChannelCount; ++c)
            {
                uint32_t expected = 0;
                if (g_channels[c].owner.compare_exchange_strong(expected, tid))
                {
                    t_channel = int(c);
                    return c;
                }
            }
            // Every dedicated channel is taken: reclaim one whose thread has exited.
            for (uint32_t c = 1; c < ipc::kChannelCount; ++c)
            {
                uint32_t holder = g_channels[c].owner.load();
                if (holder && !ThreadAlive(holder) && g_channels[c].owner.compare_exchange_strong(holder, tid))
                {
                    t_channel = int(c);
                    return c;
                }
            }
            t_channel = 0;
            return 0;
        }

        /// Pushes a request onto a channel. Called with the session lock held shared.
        bool PushRequest(uint32_t c, const Params& p, uint64_t ticket)
        {
            ClientChannel& ch = g_channels[c];
            AcquireSRWLockExclusive(&ch.produce);
            ipc::Request* slot = ch.req.Reserve();
            const uint64_t giveUp = ipc::NowUs() + 1000000;
            uint32_t round = 0;
            while (!slot)
            {
                // A full request ring can mean the host waits on our full response ring: drain it.
                DrainLocked(c);
                if (ipc::NowUs() > giveUp || g_state.load() != State::Ready)
                {
                    ReleaseSRWLockExclusive(&ch.produce);
                    return false;
                }
                ipc::SpinStep(round);
                slot = ch.req.Reserve();
            }
            slot->op = static_cast<uint32_t>(p.op);
            slot->flags = p.flags;
            slot->ticket = ticket;
            slot->a0 = p.a0; slot->a1 = p.a1; slot->a2 = p.a2; slot->a3 = p.a3; slot->a4 = p.a4;
            slot->nameLen = 0;
            slot->name[0] = '\0';
            if (p.name)
            {
                const size_t n = strnlen(p.name, ipc::kNameMax - 1);
                std::memcpy(slot->name, p.name, n);
                slot->name[n] = '\0';
                slot->nameLen = static_cast<uint32_t>(n);
            }
            ch.req.Push();
            ReleaseSRWLockExclusive(&ch.produce);

            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (g_s.header->hostSleeping.load(std::memory_order_relaxed)) SetEvent(g_s.doorbell);
            return true;
        }

        uint64_t PostOn(uint32_t c, const Params& p, Completion done, void* ctx)
        {
            const uint32_t gen = g_generation.load();
            const uint64_t ticket = (uint64_t(gen) << 48) | g_nextTicket.fetch_add(1);
            AcquireSRWLockExclusive(&g_pendingLock);
            Pending& pending = g_pending[ticket];
            pending.channel = c;
            pending.generation = gen;
            pending.done = done;
            pending.ctx = ctx;
            ReleaseSRWLockExclusive(&g_pendingLock);

            AcquireSRWLockShared(&g_sessionLock);
            const bool ok = g_state.load() == State::Ready && gen == g_generation.load() && PushRequest(c, p, ticket);
            ReleaseSRWLockShared(&g_sessionLock);
            if (!ok)
            {
                AcquireSRWLockExclusive(&g_pendingLock);
                g_pending.erase(ticket);
                ReleaseSRWLockExclusive(&g_pendingLock);
                return 0;
            }
            g_c.calls.fetch_add(1, std::memory_order_relaxed);
            return ticket;
        }

        /// Takes a finished ticket's reply out of the table.
        int TakeIfDone(uint64_t ticket, Reply& out)
        {
            AcquireSRWLockExclusive(&g_pendingLock);
            const auto it = g_pending.find(ticket);
            if (it == g_pending.end()) { ReleaseSRWLockExclusive(&g_pendingLock); return -1; }
            if (!it->second.finished) { ReleaseSRWLockExclusive(&g_pendingLock); return 0; }
            out = it->second.reply;
            g_pending.erase(it);
            ReleaseSRWLockExclusive(&g_pendingLock);
            return 1;
        }

        uint32_t ChannelOf(uint64_t ticket)
        {
            AcquireSRWLockShared(&g_pendingLock);
            const auto it = g_pending.find(ticket);
            const uint32_t c = it != g_pending.end() ? it->second.channel : 0;
            ReleaseSRWLockShared(&g_pendingLock);
            return c;
        }

        /**
         * @brief Hands a ticket's cleanup to its eventual reply (or to the loss of the host).
         *
         * A caller that gives up waiting must not free what the host may still write into.
         */
        void Abandon(uint64_t ticket, Completion cleanup, void* ctx)
        {
            bool runNow = false;
            AcquireSRWLockExclusive(&g_pendingLock);
            const auto it = g_pending.find(ticket);
            if (it == g_pending.end() || it->second.finished)
            {
                if (it != g_pending.end()) g_pending.erase(it);
                runNow = true;
            }
            else
            {
                it->second.done = cleanup;
                it->second.ctx = ctx;
            }
            ReleaseSRWLockExclusive(&g_pendingLock);
            if (runNow) cleanup(ctx, Reply{});
        }

        // --- mounts, replayed after a restart --------------------------------------------------------

        struct MountRecord
        {
            std::string name;
            int         priority = 0;
            uint32_t    parent = ipc::kAnyArchive;  // client id
            uint32_t    hostId = ipc::kAnyArchive;
            uint32_t    kind = 0;
            bool        alive = true;
        };

        SRWLOCK                  g_mountLock = SRWLOCK_INIT;
        std::vector<MountRecord> g_mounts;          // client id = index + 1

        uint32_t HostArchive(uint32_t clientId)
        {
            if (clientId == ipc::kAnyArchive) return ipc::kAnyArchive;
            AcquireSRWLockShared(&g_mountLock);
            uint32_t host = ipc::kAnyArchive - 1;   // an id the host never issued: a lookup misses cleanly
            if (clientId >= 1 && clientId <= g_mounts.size() && g_mounts[clientId - 1].alive)
                host = g_mounts[clientId - 1].hostId;
            ReleaseSRWLockShared(&g_mountLock);
            return host;
        }

        uint32_t ClientArchive(uint32_t hostId)
        {
            AcquireSRWLockShared(&g_mountLock);
            uint32_t id = ipc::kAnyArchive;
            for (size_t i = 0; i < g_mounts.size(); ++i)
                if (g_mounts[i].alive && g_mounts[i].hostId == hostId) { id = uint32_t(i + 1); break; }
            ReleaseSRWLockShared(&g_mountLock);
            return id;
        }

        // --- window ----------------------------------------------------------------------------------

        SRWLOCK g_tlsfLock = SRWLOCK_INIT;
        tlsf_t  g_tlsf = nullptr;

        // --- name -> size hints, so most reads fit their first block ---------------------------------

        std::mutex                                 g_hintMutex;
        std::unordered_map<std::string, uint64_t>  g_hints;

        std::string HintKey(const char* name, uint32_t archive)
        {
            std::string k(name);
            for (char& ch : k) ch = (ch == '/') ? '\\' : char(tolower(static_cast<unsigned char>(ch)));
            k.push_back('|');
            k += std::to_string(archive);
            return k;
        }

        uint64_t SizeHint(const std::string& key)
        {
            std::lock_guard<std::mutex> lock(g_hintMutex);
            const auto it = g_hints.find(key);
            return it != g_hints.end() ? it->second : 0;
        }

        void RememberSize(const std::string& key, uint64_t size)
        {
            std::lock_guard<std::mutex> lock(g_hintMutex);
            if (g_hints.size() >= kSizeHintCap) g_hints.clear();
            g_hints[key] = size;
        }

        // --- spawning --------------------------------------------------------------------------------

        bool CreateObjects()
        {
            g_s.pid = GetCurrentProcessId();

            if (!g_rootOverride.empty())
            {
                g_s.root = g_rootOverride;
            }
            else
            {
                wchar_t path[MAX_PATH] = {};
                const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
                wchar_t* slash = n ? wcsrchr(path, L'\\') : nullptr;
                if (!slash) return false;
                *slash = L'\0';
                g_s.root = path;
            }
            g_s.exe = g_s.root + L"\\wxl-host.exe";
            if (GetFileAttributesW(g_s.exe.c_str()) == INVALID_FILE_ATTRIBUTES)
            {
                WLOG_WARN("host: wxl-host.exe is missing next to Wow.exe; native paths for this session");
                return false;
            }

            wchar_t name[128];
            const uint64_t ctlSize = ipc::ControlSize();
            ipc::ControlName(name, 128, g_s.pid);
            g_s.ctlSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                                DWORD(ctlSize >> 32), DWORD(ctlSize), name);
            if (!g_s.ctlSection) return false;
            g_s.ctl = static_cast<uint8_t*>(MapViewOfFile(g_s.ctlSection, FILE_MAP_ALL_ACCESS, 0, 0, SIZE_T(ctlSize)));
            if (!g_s.ctl) return false;

            g_s.windowSize = uint64_t(WindowMb()) << 20;
            ipc::WindowName(name, 128, g_s.pid);
            g_s.winSection = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                                DWORD(g_s.windowSize >> 32), DWORD(g_s.windowSize), name);
            if (!g_s.winSection) return false;
            g_s.window = static_cast<uint8_t*>(MapViewOfFile(g_s.winSection, FILE_MAP_ALL_ACCESS, 0, 0,
                                                             SIZE_T(g_s.windowSize)));
            if (!g_s.window)
            {
                WLOG_WARN("host: could not map a %llu MB transfer window (win32 %lu)",
                          static_cast<unsigned long long>(g_s.windowSize >> 20), GetLastError());
                return false;
            }

            static std::vector<uint8_t> tlsfControl(tlsf_size());
            g_tlsf = tlsf_create(tlsfControl.data());
            if (!g_tlsf || !tlsf_add_pool(g_tlsf, g_s.window, size_t(g_s.windowSize))) return false;

            ipc::DoorbellName(name, 128, g_s.pid);
            g_s.doorbell = CreateEventW(nullptr, FALSE, FALSE, name);
            ipc::ReadyName(name, 128, g_s.pid);
            g_s.ready = CreateEventW(nullptr, TRUE, FALSE, name);
            if (!g_s.doorbell || !g_s.ready) return false;

            g_s.header = reinterpret_cast<ipc::Header*>(g_s.ctl);
            for (uint32_t c = 0; c < ipc::kChannelCount; ++c)
            {
                ClientChannel& ch = g_channels[c];
                ch.shared = reinterpret_cast<ipc::Channel*>(g_s.ctl + ipc::ChannelOffset(c));
                ch.req = ipc::RequestRing(&ch.shared->reqWrite, &ch.shared->reqRead, ch.shared->requests);
                ch.resp = ipc::ResponseRing(&ch.shared->respWrite, &ch.shared->respRead, ch.shared->responses);
                ipc::ChannelEventName(name, 128, g_s.pid, c);
                ch.event = CreateEventW(nullptr, FALSE, FALSE, name);
                if (!ch.event) return false;
            }

            // Kill-on-close job: the host dies with Wow.exe, however Wow.exe ends.
            g_s.job = CreateJobObjectW(nullptr, nullptr);
            if (g_s.job)
            {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags =
                    JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
                SetInformationJobObject(g_s.job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
            }
            return true;
        }

        /// Writes the header a new host validates, and clears the rings. Nothing else runs on the session.
        void PrepareHeader(uint32_t generation)
        {
            ipc::Header* h = g_s.header;
            std::memset(g_s.ctl, 0, ipc::kHeaderBytes);
            h->magic = ipc::kMagic;
            h->version = ipc::kProtocolVersion;
            h->headerSize = sizeof(ipc::Header);
            h->channelSize = sizeof(ipc::Channel);
            h->requestSize = sizeof(ipc::Request);
            h->responseSize = sizeof(ipc::Response);
            h->channelCount = ipc::kChannelCount;
            h->requestSlots = ipc::kRequestSlots;
            h->responseSlots = ipc::kResponseSlots;
            h->clientPid = g_s.pid;
            h->generation = generation;
            h->windowSize = g_s.windowSize;
            h->controlSize = ipc::ControlSize();
            h->spinUs = SpinUs();
            h->workerCount = Workers();
            h->storeLimit = StoreMb() << 20;
            h->cacheLimit = CacheMb() << 20;
            h->prefetch = Prefetch() ? 1 : 0;
            h->dedupe = BackingRefs() ? 1 : 0;
            WideCharToMultiByte(CP_UTF8, 0, g_s.root.c_str(), -1, h->clientRoot, ipc::kRootMax, nullptr, nullptr);
            h->hostState.store(static_cast<uint32_t>(ipc::HostState::None));

            for (ClientChannel& ch : g_channels)
            {
                ch.req.Reset();
                ch.resp.Reset();
                ch.shared->clientWaiting.store(0);
            }
            std::atomic_thread_fence(std::memory_order_seq_cst);
        }

        /// Starts a host process for a header PrepareHeader already wrote, and waits for it to accept.
        bool Spawn(uint32_t generation)
        {
            ResetEvent(g_s.ready);

            wchar_t cmd[MAX_PATH + 96];
            _snwprintf_s(cmd, _TRUNCATE, L"\"%s\" --pid %u --gen %u", g_s.exe.c_str(), g_s.pid, generation);
            STARTUPINFOW si{};
            si.cb = sizeof si;
            PROCESS_INFORMATION pi{};
            DWORD flags = CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
            BOOL ok = CreateProcessW(g_s.exe.c_str(), cmd, nullptr, nullptr, FALSE, flags, nullptr, g_s.root.c_str(),
                                     &si, &pi);
            if (!ok && GetLastError() == ERROR_ACCESS_DENIED)
            {
                flags &= ~DWORD(CREATE_BREAKAWAY_FROM_JOB);   // Wow.exe sits in a job that forbids breaking away
                ok = CreateProcessW(g_s.exe.c_str(), cmd, nullptr, nullptr, FALSE, flags, nullptr, g_s.root.c_str(),
                                    &si, &pi);
            }
            if (!ok)
            {
                WLOG_WARN("host: could not start wxl-host.exe (win32 %lu)", GetLastError());
                return false;
            }
            if (!g_s.job || !AssignProcessToJobObject(g_s.job, pi.hProcess))
                WLOG_WARN("host: could not tie the host to Wow.exe's lifetime (win32 %lu); the host watches Wow.exe "
                          "itself", GetLastError());
            ResumeThread(pi.hThread);
            CloseHandle(pi.hThread);

            const HANDLE waitOn[2] = { g_s.ready, pi.hProcess };
            const DWORD r = WaitForMultipleObjects(2, waitOn, FALSE, StartMs());
            const auto hostState = static_cast<ipc::HostState>(g_s.header->hostState.load());
            if (r == WAIT_OBJECT_0 && hostState == ipc::HostState::Ready && g_s.header->hostGeneration == generation)
            {
                g_s.process = pi.hProcess;
                g_s.hostPid = pi.dwProcessId;
                return true;
            }
            if (hostState == ipc::HostState::Refused)
                WLOG_ERROR("host: wxl-host.exe refused the session (host protocol %u, client %u): rebuild both",
                           g_s.header->hostVersion, ipc::kProtocolVersion);
            else if (r == WAIT_OBJECT_0 + 1)
                WLOG_WARN("host: wxl-host.exe exited during start-up (see Logs\\wxl-host.log)");
            else
                WLOG_WARN("host: wxl-host.exe did not answer within %u ms", StartMs());
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 2000);
            CloseHandle(pi.hProcess);
            return false;
        }

        /// Measures a few round trips, for the start-up line.
        void LogLatency()
        {
            uint64_t best = UINT64_MAX, total = 0;
            const int n = 32;
            for (int i = 0; i < n; ++i)
            {
                Reply r;
                Params p;
                p.op = Op::Ping;
                const uint64_t t0 = ipc::NowUs();
                if (!Call(p, r, 1000)) return;
                const uint64_t dt = ipc::NowUs() - t0;
                best = std::min(best, dt);
                total += dt;
                g_c.pingCount.fetch_add(1, std::memory_order_relaxed);
                g_c.pingUsTotal.fetch_add(dt, std::memory_order_relaxed);
                AtomicMax(g_c.pingUsMax, dt);
            }
            WLOG_INFO("host: round trip %llu us best, %llu us mean over %d pings",
                      static_cast<unsigned long long>(best), static_cast<unsigned long long>(total / n), n);
        }

        bool ReplayMounts()
        {
            AcquireSRWLockExclusive(&g_mountLock);
            std::vector<MountRecord> mounts = g_mounts;
            ReleaseSRWLockExclusive(&g_mountLock);

            for (size_t i = 0; i < mounts.size(); ++i)
            {
                MountRecord& m = mounts[i];
                if (!m.alive) continue;
                uint32_t parentHost = ipc::kAnyArchive;
                if (m.parent != ipc::kAnyArchive && m.parent >= 1 && m.parent <= mounts.size())
                    parentHost = mounts[m.parent - 1].hostId;
                Params p;
                p.op = Op::Mount;
                p.name = m.name.c_str();
                p.a0 = uint64_t(int64_t(m.priority));
                p.a1 = parentHost;
                Reply r;
                if (!Call(p, r, 30000) || r.status != Status::Ok)
                {
                    WLOG_WARN("host: replaying mount '%s' failed", m.name.c_str());
                    m.alive = false;
                    continue;
                }
                m.hostId = uint32_t(r.r0);
            }

            AcquireSRWLockExclusive(&g_mountLock);
            for (size_t i = 0; i < mounts.size() && i < g_mounts.size(); ++i)
            {
                g_mounts[i].hostId = mounts[i].hostId;
                g_mounts[i].alive = mounts[i].alive;
            }
            ReleaseSRWLockExclusive(&g_mountLock);
            return true;
        }

        // --- monitor ---------------------------------------------------------------------------------

        std::deque<ULONGLONG> g_restartTimes;

        void HostLost(const char* why)
        {
            const uint32_t lostGen = g_generation.load();
            SetState(State::Down);
            WLOG_WARN("host: lost (%s), generation %u", why, lostGen);

            // The old process must be gone before its blocks are handed back.
            if (g_s.process)
            {
                TerminateProcess(g_s.process, 1);
                WaitForSingleObject(g_s.process, 3000);
                CloseHandle(g_s.process);
                g_s.process = nullptr;
            }
            for (ClientChannel& ch : g_channels) SetEvent(ch.event);
            FailGeneration(lostGen);
        }

        void TryRestart()
        {
            const ULONGLONG now = GetTickCount64();
            while (!g_restartTimes.empty() && now - g_restartTimes.front() > 60000) g_restartTimes.pop_front();
            if (int(g_restartTimes.size()) >= kMaxRestartsPerMinute)
            {
                WLOG_ERROR("host: %d restarts within a minute; the client uses its own paths for the rest of the "
                           "session", kMaxRestartsPerMinute);
                SetState(State::Failed);
                if (g_listener) g_listener(State::Failed);
                return;
            }
            Sleep(DWORD(250u << g_restartTimes.size()));   // 250 ms, 500 ms, 1 s
            g_restartTimes.push_back(GetTickCount64());

            const uint32_t gen = g_generation.load() + 1;
            AcquireSRWLockExclusive(&g_sessionLock);
            g_generation.store(gen);
            PrepareHeader(gen);
            ReleaseSRWLockExclusive(&g_sessionLock);
            const bool ok = Spawn(gen);
            if (!ok)
            {
                WLOG_WARN("host: restart as generation %u failed", gen);
                return;   // still Down; the next tick tries again or gives up
            }

            SetState(State::Ready);
            ReplayMounts();
            g_c.restarts.fetch_add(1);
            WLOG_INFO("host: restarted as generation %u (pid %u), %zu mounts replayed", gen, g_s.hostPid,
                      g_mounts.size());
            if (g_listener) g_listener(State::Ready);
        }

        DWORD WINAPI MonitorThread(LPVOID)
        {
            uint64_t lastBeat = 0;
            ULONGLONG lastBeatAt = GetTickCount64();
            ULONGLONG lastStats = 0;
            for (;;)
            {
                Sleep(kMonitorMs);
                const State s = g_state.load();
                if (s == State::Failed) return 0;
                if (s == State::Down)
                {
                    TryRestart();
                    lastBeatAt = GetTickCount64();
                    continue;
                }
                if (s != State::Ready) continue;

                const ULONGLONG now = GetTickCount64();
                const uint64_t beat = g_s.header->heartbeat.load(std::memory_order_relaxed);
                if (beat != lastBeat) { lastBeat = beat; lastBeatAt = now; }
                if (WaitForSingleObject(g_s.process, 0) != WAIT_TIMEOUT) { HostLost("process exited"); continue; }
                if (now - lastBeatAt > kHeartbeatStaleMs) { HostLost("heartbeat stopped"); continue; }

                for (uint32_t c = 0; c < ipc::kChannelCount; ++c) Drain(c);
                if (now - lastStats > 5000)
                {
                    lastStats = now;
                    Params p;
                    p.op = Op::Stats;
                    Post(p, [](void*, const Reply&) {}, nullptr);
                }
            }
        }

        // --- read bookkeeping ------------------------------------------------------------------------

        struct ReadCleanup
        {
            Block      block;
            BigSection big;
        };

        void CleanupRead(void* ctx, const Reply&)
        {
            auto* c = static_cast<ReadCleanup*>(ctx);
            if (c->block.data) Free(c->block);
            if (c->big.handle) FreeBig(c->big);
            delete c;
        }

        void FreeBlockDone(void* ctx, const Reply&)
        {
            auto* b = static_cast<Block*>(ctx);
            Free(*b);
            delete b;
        }
    }

    // ==============================================================================================
    // lifecycle
    // ==============================================================================================

    void SetClientRoot(const wchar_t* root)
    {
        g_rootOverride = root ? root : L"";
    }

    bool Enabled()
    {
        static const bool on = config::Env("WXL_HOST", true);
        return on;
    }

    bool Start()
    {
        const State s = g_state.load();
        if (s == State::Ready) return true;
        if (s == State::Disabled || s == State::Failed || s == State::Down) return false;

        std::lock_guard<std::mutex> lock(g_startMutex);
        if (g_state.load() != State::Off) return g_state.load() == State::Ready;
        if (!Enabled())
        {
            SetState(State::Disabled);
            WLOG_INFO("host: disabled (WXL_HOST=0); the client uses its own paths");
            return false;
        }
        g_stateEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        SetState(State::Starting);

        const uint64_t t0 = ipc::NowUs();
        if (!CreateObjects())
        {
            WLOG_WARN("host: could not set up the shared session (win32 %lu); native paths", GetLastError());
            SetState(State::Failed);
            return false;
        }
        g_generation.store(1);
        PrepareHeader(1);
        if (!Spawn(1))
        {
            SetState(State::Failed);
            return false;
        }
        SetState(State::Ready);
        WLOG_INFO("host: ready in %llu ms (pid %u, protocol %u, window %llu MB, big files from %u KB, spin %u us)",
                  static_cast<unsigned long long>((ipc::NowUs() - t0) / 1000), g_s.hostPid, ipc::kProtocolVersion,
                  static_cast<unsigned long long>(g_s.windowSize >> 20), BigKb(), SpinUs());
        CloseHandle(CreateThread(nullptr, 0, &MonitorThread, nullptr, 0, nullptr));
        LogLatency();
        return true;
    }

    State CurrentState() { return g_state.load(); }
    uint32_t HostPid() { return Available() ? g_s.hostPid : 0; }

    bool WaitSettled(uint32_t timeoutMs)
    {
        const ULONGLONG until = GetTickCount64() + timeoutMs;
        for (;;)
        {
            const State s = g_state.load();
            if (s == State::Ready) return true;
            if (s == State::Failed || s == State::Disabled || s == State::Off) return false;
            const ULONGLONG now = GetTickCount64();
            if (now >= until) return false;
            if (g_stateEvent)
            {
                ResetEvent(g_stateEvent);
                if (g_state.load() != s) continue;
                WaitForSingleObject(g_stateEvent, DWORD(std::min<ULONGLONG>(until - now, 100)));
            }
            else
            {
                Sleep(10);
            }
        }
    }
    bool Available() { return g_state.load(std::memory_order_acquire) == State::Ready; }
    uint32_t Generation() { return g_generation.load(); }
    void SetStateListener(void (*listener)(State)) { g_listener = listener; }

    // ==============================================================================================
    // requests
    // ==============================================================================================

    uint64_t Post(const Params& params, Completion done, void* ctx)
    {
        if (!Available()) return 0;
        const uint32_t c = ChannelForThread();
        Drain(c);
        return PostOn(c, params, done, ctx);
    }

    int Poll(uint64_t ticket, Reply& out)
    {
        const int r = TakeIfDone(ticket, out);
        if (r != 0) return r;
        Drain(ChannelOf(ticket));
        return TakeIfDone(ticket, out);
    }

    bool Wait(uint64_t ticket, Reply& out, uint32_t timeoutMs)
    {
        WXL_ZONE("host.client.wait");
        if (!ticket) return false;
        const uint32_t c = ChannelOf(ticket);
        ClientChannel& ch = g_channels[c];
        const uint64_t start = ipc::NowUs();
        const uint64_t deadline = start + uint64_t(timeoutMs ? timeoutMs : TimeoutMs()) * 1000;
        const uint64_t spinUntil = start + SpinUs();
        uint32_t round = 0;
        for (;;)
        {
            const int r = TakeIfDone(ticket, out);
            if (r == 1) return true;
            if (r < 0) return false;

            Drain(c);
            if (TakeIfDone(ticket, out) == 1) return true;
            if (g_state.load() != State::Ready) { TakeIfDone(ticket, out); return false; }

            const uint64_t now = ipc::NowUs();
            if (now >= deadline) return false;
            if (now < spinUntil) { ipc::SpinStep(round); continue; }

            ch.shared->clientWaiting.store(1, std::memory_order_seq_cst);
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (!ch.resp.Empty())
            {
                ch.shared->clientWaiting.store(0, std::memory_order_relaxed);
                continue;
            }
            const uint64_t leftMs = (deadline - now + 999) / 1000;
            WaitForSingleObject(ch.event, DWORD(std::min<uint64_t>(leftMs, kSliceMs)));
        }
    }

    bool Call(const Params& params, Reply& out, uint32_t timeoutMs)
    {
        const uint64_t ticket = Post(params);
        if (!ticket) return false;
        if (Wait(ticket, out, timeoutMs)) return true;
        Abandon(ticket, [](void*, const Reply&) {}, nullptr);
        return false;
    }

    // ==============================================================================================
    // memory
    // ==============================================================================================

    bool Alloc(uint64_t size, Block& out)
    {
        out = Block{};
        if (!g_tlsf || size > g_s.windowSize) return false;
        const uint64_t want = std::max(size, kMinBlock);
        AcquireSRWLockExclusive(&g_tlsfLock);
        void* p = tlsf_memalign(g_tlsf, 64, size_t(want));
        const uint64_t real = p ? tlsf_block_size(p) : 0;
        ReleaseSRWLockExclusive(&g_tlsfLock);
        if (!p) return false;
        out.data = static_cast<uint8_t*>(p);
        out.offset = uint64_t(out.data - g_s.window);
        out.size = size;
        const uint64_t used = g_c.windowUsed.fetch_add(real) + real;
        AtomicMax(g_c.windowPeak, used);
        return true;
    }

    void Free(Block& block)
    {
        if (!block.data || !g_tlsf) return;
        AcquireSRWLockExclusive(&g_tlsfLock);
        const uint64_t real = tlsf_block_size(block.data);
        tlsf_free(g_tlsf, block.data);
        ReleaseSRWLockExclusive(&g_tlsfLock);
        g_c.windowUsed.fetch_sub(real);
        block = Block{};
    }

    void Shrink(Block& block, uint64_t newSize)
    {
        if (!block.data || newSize >= block.size) { block.size = std::min(block.size, newSize); return; }
        AcquireSRWLockExclusive(&g_tlsfLock);
        const uint64_t before = tlsf_block_size(block.data);
        void* same = tlsf_realloc(g_tlsf, block.data, size_t(std::max(newSize, kMinBlock)));
        const uint64_t after = same ? tlsf_block_size(same) : before;
        ReleaseSRWLockExclusive(&g_tlsfLock);
        if (same)
        {
            g_c.windowUsed.fetch_sub(before - std::min(before, after));
            block.data = static_cast<uint8_t*>(same);
            block.offset = uint64_t(block.data - g_s.window);
        }
        block.size = newSize;
    }

    bool CreateBig(uint64_t size, BigSection& out)
    {
        out = BigSection{};
        if (!g_s.pid) return false;
        const uint64_t bytes = std::max<uint64_t>(size, 1);
        out.id = g_nextBig.fetch_add(1);
        wchar_t name[128];
        ipc::BigSectionName(name, 128, g_s.pid, out.id);
        out.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                        DWORD(bytes >> 32), DWORD(bytes), name);
        if (!out.handle) return false;
        out.size = size;
        return true;
    }

    void FreeBig(BigSection& big)
    {
        if (big.view) UnmapViewOfFile(big.view);
        if (big.handle) CloseHandle(big.handle);
        big = BigSection{};
    }

    uint64_t BigThreshold()
    {
        return uint64_t(BigKb()) << 10;
    }

    // ==============================================================================================
    // files and archives
    // ==============================================================================================

    struct ReadOp
    {
        std::string  name;
        std::string  hintKey;
        uint32_t     archive = ipc::kAnyArchive;
        uint32_t     hostArchive = ipc::kAnyArchive;
        uint64_t     cap = 0;
        uint64_t     ticket = 0;
        uint64_t     t0 = 0;
        int          attempt = 0;
        uint32_t     flags = 0;
        uint64_t     a4 = 0;
        ReadCleanup* dest = nullptr;
    };

    namespace
    {
        /// Allocates the destination for the current capacity and posts the read.
        bool PostRead(ReadOp& op)
        {
            Params p;
            p.op = Op::Read;
            p.name = op.name.c_str();
            p.a0 = op.hostArchive;
            p.a3 = op.cap;
            p.a4 = op.a4;
            p.flags = op.flags;

            op.dest = new ReadCleanup();
            bool window = op.cap <= BigThreshold() && Alloc(op.cap, op.dest->block);
            if (!window && !CreateBig(op.cap, op.dest->big))
            {
                delete op.dest;
                op.dest = nullptr;
                return false;
            }
            p.a1 = static_cast<uint64_t>(window ? ipc::Dest::Window : ipc::Dest::BigSection);
            p.a2 = window ? op.dest->block.offset : op.dest->big.id;

            op.ticket = Post(p);
            if (op.ticket) return true;
            CleanupRead(op.dest, Reply{});
            op.dest = nullptr;
            return false;
        }

        /// Turns a successful reply into FileData. Frees op.dest's holder.
        bool Deliver(ReadOp& op, const Reply& r, FileData& out)
        {
            const uint64_t size = r.r0;
            out.archive = ClientArchive(uint32_t(r.r1));
            out.size = size;
            out.image = uint32_t(r.r2);
            out.served = uint32_t(r.r3);
            out.hostUs = uint32_t(std::min<uint64_t>(r.r4, 0xFFFFFFFFull));
            if (op.dest->big.handle)
            {
                op.dest->big.view = static_cast<uint8_t*>(MapViewOfFile(
                    op.dest->big.handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, SIZE_T(std::max<uint64_t>(size, 1))));
                if (!op.dest->big.view) return false;
                out.big = op.dest->big;
                out.data = out.big.view;
                g_c.bigCount.fetch_add(1, std::memory_order_relaxed);
                g_c.bigBytes.fetch_add(size, std::memory_order_relaxed);
            }
            else
            {
                Shrink(op.dest->block, size);
                out.block = op.dest->block;
                out.data = out.block.data;
            }
            delete op.dest;
            op.dest = nullptr;

            RememberSize(op.hintKey, size);
            const uint64_t dt = ipc::NowUs() - op.t0;
            g_c.readCount.fetch_add(1, std::memory_order_relaxed);
            g_c.readBytes.fetch_add(size, std::memory_order_relaxed);
            g_c.readUsTotal.fetch_add(dt, std::memory_order_relaxed);
            AtomicMax(g_c.readUsMax, dt);
            return true;
        }
    }

    ReadOp* ReadBegin(const char* name, uint32_t archive, const ReadOptions* options)
    {
        if (!name || !*name || !Available()) return nullptr;
        auto* op = new ReadOp();
        op->name = name;
        op->archive = archive;
        op->hostArchive = HostArchive(archive);
        op->hintKey = HintKey(name, archive);
        if (options)
        {
            op->flags = options->flags;
            op->a4 = options->a4;
            if (op->flags & ipc::kFlagTexture) op->hintKey += "|t";   // a texture image has its own size
        }
        op->cap = SizeHint(op->hintKey);
        if (!op->cap && options) op->cap = options->sizeHint;
        if (!op->cap) op->cap = kDefaultReadCap;
        op->t0 = ipc::NowUs();
        if (PostRead(*op)) return op;
        delete op;
        return nullptr;
    }

    bool ReadStep(ReadOp* op, FileData& out, ipc::Status& status, uint32_t waitMs)
    {
        out = FileData{};
        for (;;)
        {
            Reply r;
            const bool done = waitMs ? Wait(op->ticket, r, waitMs) : Poll(op->ticket, r) == 1;
            if (!done)
            {
                if (Poll(op->ticket, r) < 0 || !Available())
                {
                    // Lost with its generation: the cleanup already ran through the failed ticket.
                    status = Status::Failed;
                    if (op->dest) Abandon(op->ticket, &CleanupRead, op->dest);
                    delete op;
                    return true;
                }
                if (!waitMs) return false;
                status = Status::Busy;   // timed out
                Abandon(op->ticket, &CleanupRead, op->dest);
                WLOG_WARN("host: read of '%s' timed out", op->name.c_str());
                delete op;
                return true;
            }

            if (r.status == Status::NeedMore && ++op->attempt < 3)
            {
                CleanupRead(op->dest, r);
                op->dest = nullptr;
                op->cap = r.r0;
                if (!PostRead(*op)) { status = Status::Failed; delete op; return true; }
                continue;
            }
            if (r.status != Status::Ok)
            {
                CleanupRead(op->dest, r);
                status = r.status == Status::NeedMore ? Status::Failed : r.status;
                delete op;
                return true;
            }
            if (!Deliver(*op, r, out))
            {
                CleanupRead(op->dest, r);
                status = Status::Failed;
                delete op;
                return true;
            }
            status = Status::Ok;
            delete op;
            return true;
        }
    }

    void ReadCancel(ReadOp* op)
    {
        if (!op) return;
        if (op->dest) Abandon(op->ticket, &CleanupRead, op->dest);
        delete op;
    }

    ipc::Status ReadFile(const char* name, uint32_t archive, FileData& out, uint32_t timeoutMs,
                         const ReadOptions* options)
    {
        WXL_ZONE("host.client.read");
        out = FileData{};
        if (!name || !*name) return Status::BadRequest;
        ReadOp* op = ReadBegin(name, archive, options);
        if (!op) return Status::Failed;
        Status status = Status::Failed;
        ReadStep(op, out, status, timeoutMs ? timeoutMs : TimeoutMs());
        return status;
    }

    void ReleaseFile(FileData& file)
    {
        if (file.block.data) Free(file.block);
        if (file.big.handle) FreeBig(file.big);
        file = FileData{};
    }

    ipc::Status StatFile(const char* name, uint32_t archive, uint32_t flags, uint64_t* size, uint32_t* where)
    {
        if (!name || !*name) return Status::BadRequest;
        Params p;
        p.op = Op::Stat;
        p.name = name;
        p.a0 = HostArchive(archive);
        p.a1 = flags;
        Reply r;
        const uint64_t t0 = ipc::NowUs();
        if (!Call(p, r, 0)) return Status::Failed;
        g_c.statCount.fetch_add(1, std::memory_order_relaxed);
        g_c.statUsTotal.fetch_add(ipc::NowUs() - t0, std::memory_order_relaxed);
        if (r.status == Status::Ok)
        {
            if (size) *size = r.r0;
            if (where) *where = ClientArchive(uint32_t(r.r1));
        }
        return r.status;
    }

    ipc::Status Mount(const char* name, int priority, uint32_t parent, uint32_t& id, uint32_t& kind,
                      uint32_t* clientError)
    {
        id = ipc::kAnyArchive;
        kind = 0;
        if (clientError) *clientError = 0;
        if (!name || !*name) return Status::BadRequest;
        Params p;
        p.op = Op::Mount;
        p.name = name;
        p.a0 = uint64_t(int64_t(priority));
        p.a1 = HostArchive(parent);
        Reply r;
        if (!Call(p, r, 30000)) return Status::Failed;
        if (clientError) *clientError = uint32_t(r.r2);
        if (r.status != Status::Ok) return r.status;

        MountRecord m;
        m.name = name;
        m.priority = priority;
        m.parent = parent;
        m.hostId = uint32_t(r.r0);
        m.kind = uint32_t(r.r1);
        AcquireSRWLockExclusive(&g_mountLock);
        g_mounts.push_back(std::move(m));
        id = uint32_t(g_mounts.size());
        ReleaseSRWLockExclusive(&g_mountLock);
        kind = uint32_t(r.r1);
        return Status::Ok;
    }

    void Unmount(uint32_t id)
    {
        const uint32_t host = HostArchive(id);
        AcquireSRWLockExclusive(&g_mountLock);
        if (id >= 1 && id <= g_mounts.size()) g_mounts[id - 1].alive = false;
        ReleaseSRWLockExclusive(&g_mountLock);
        if (host == ipc::kAnyArchive) return;
        Params p;
        p.op = Op::Unmount;
        p.a0 = host;
        Post(p, [](void*, const Reply&) {}, nullptr);
    }

    // ==============================================================================================
    // store
    // ==============================================================================================

    bool StorePut(uint64_t key, const void* data, uint64_t size, ipc::Codec codec, bool wait)
    {
        if (!Available() || (!data && size)) return false;
        auto* block = new Block();
        if (!Alloc(size, *block)) { delete block; return false; }
        if (size) std::memcpy(block->data, data, size_t(size));
        Params p;
        p.op = Op::StorePut;
        p.a0 = key;
        p.a1 = block->offset;
        p.a2 = size;
        p.a3 = static_cast<uint64_t>(codec);
        if (!wait)
        {
            if (Post(p, &FreeBlockDone, block)) return true;
            Free(*block);
            delete block;
            return false;
        }
        const uint64_t ticket = Post(p);
        Reply r;
        if (!ticket) { Free(*block); delete block; return false; }
        if (!Wait(ticket, r, 0)) { Abandon(ticket, &FreeBlockDone, block); return false; }
        Free(*block);
        delete block;
        return r.status == Status::Ok;
    }

    bool StorePatch(uint64_t key, uint64_t offset, const void* data, uint64_t size)
    {
        if (!Available() || !data || !size) return false;
        auto* block = new Block();
        if (!Alloc(size, *block)) { delete block; return false; }
        std::memcpy(block->data, data, size_t(size));
        Params p;
        p.op = Op::StorePatch;
        p.a0 = key;
        p.a1 = block->offset;
        p.a2 = size;
        p.a3 = offset;
        if (Post(p, &FreeBlockDone, block)) return true;
        Free(*block);
        delete block;
        return false;
    }

    bool StoreGet(uint64_t key, void* dst, uint64_t cap, uint64_t* full, uint32_t timeoutMs)
    {
        if (full) *full = 0;
        if (!Available()) return false;
        auto* cleanup = new ReadCleanup();
        if (!Alloc(std::max<uint64_t>(cap, 1), cleanup->block)) { delete cleanup; return false; }
        Params p;
        p.op = Op::StoreGet;
        p.a0 = key;
        p.a1 = cleanup->block.offset;
        p.a2 = cap;
        const uint64_t ticket = Post(p);
        Reply r;
        if (!ticket) { CleanupRead(cleanup, r); return false; }
        if (!Wait(ticket, r, timeoutMs)) { Abandon(ticket, &CleanupRead, cleanup); return false; }
        const bool ok = r.status == Status::Ok || r.status == Status::NeedMore;
        if (ok)
        {
            if (full) *full = r.r0;
            if (dst) std::memcpy(dst, cleanup->block.data, size_t(std::min(cap, r.r0)));
        }
        CleanupRead(cleanup, r);
        return r.status == Status::Ok;
    }

    void StoreDrop(uint64_t key)
    {
        StoreDropRange(key, 1);
    }

    void StoreDropRange(uint64_t first, uint32_t count)
    {
        if (!count) return;
        Params p;
        p.op = Op::StoreDrop;
        p.a0 = first;
        p.a1 = count;
        Post(p, [](void*, const Reply&) {}, nullptr);
    }

    bool StorePatchRows(uint64_t key, uint64_t dstOffset, uint64_t dstPitch, const void* src, uint64_t rowBytes,
                        uint64_t rows, uint64_t srcPitch)
    {
        if (!Available() || !src || !rowBytes || !rows || rowBytes > dstPitch || dstPitch > 0xFFFFFFFFull
            || rowBytes > 0xFFFFFFFFull)
            return false;
        auto* block = new Block();
        if (!Alloc(rowBytes * rows, *block)) { delete block; return false; }
        const auto* in = static_cast<const uint8_t*>(src);
        for (uint64_t r = 0; r < rows; ++r)
            std::memcpy(block->data + r * rowBytes, in + r * srcPitch, size_t(rowBytes));
        Params p;
        p.op = Op::StorePatch;
        p.a0 = key;
        p.a1 = block->offset;
        p.a2 = rowBytes * rows;
        p.a3 = dstOffset;
        p.a4 = (rowBytes << 32) | dstPitch;
        if (Post(p, &FreeBlockDone, block)) return true;
        Free(*block);
        delete block;
        return false;
    }

    // ==============================================================================================
    // counters
    // ==============================================================================================

    void ReadCounters(Counters& out)
    {
        out.calls = g_c.calls.load();
        out.fallbacks = g_c.fallbacks.load();
        out.restarts = g_c.restarts.load();
        out.readCount = g_c.readCount.load();
        out.readBytes = g_c.readBytes.load();
        out.readUsTotal = g_c.readUsTotal.load();
        out.readUsMax = g_c.readUsMax.load();
        out.pingCount = g_c.pingCount.load();
        out.pingUsTotal = g_c.pingUsTotal.load();
        out.pingUsMax = g_c.pingUsMax.load();
        out.statCount = g_c.statCount.load();
        out.statUsTotal = g_c.statUsTotal.load();
        out.windowUsed = g_c.windowUsed.load();
        out.windowPeak = g_c.windowPeak.load();
        out.windowSize = g_s.windowSize;
        out.bigCount = g_c.bigCount.load();
        out.bigBytes = g_c.bigBytes.load();
    }

    void CountFallback()
    {
        g_c.fallbacks.fetch_add(1, std::memory_order_relaxed);
    }

    const ipc::HostCounters* HostSideCounters()
    {
        return g_s.header ? &g_s.header->counters : nullptr;
    }

    bool CacheEnabled()
    {
        return Available() && g_s.header && g_s.header->cacheLimit != 0;
    }

    void CounterLine(char* out, size_t cap)
    {
        if (!out || !cap) return;
        const State s = g_state.load();
        static const char* names[] = { "off", "disabled", "starting", "ready", "down", "failed" };
        Counters c{};
        ReadCounters(c);
        constexpr double mb = 1024.0 * 1024.0;
        int n = std::snprintf(out, cap,
            "host: %s, generation %u, restarts %llu | calls %llu, fallbacks %llu | reads %llu (%.1f MB), mean %llu us, "
            "max %llu us | stats %llu, mean %llu us | window %.1f/%.1f MB (peak %.1f) | big sections %llu (%.1f MB)",
            names[static_cast<uint32_t>(s)], g_generation.load(), static_cast<unsigned long long>(c.restarts),
            static_cast<unsigned long long>(c.calls), static_cast<unsigned long long>(c.fallbacks),
            static_cast<unsigned long long>(c.readCount), c.readBytes / mb,
            static_cast<unsigned long long>(c.readCount ? c.readUsTotal / c.readCount : 0),
            static_cast<unsigned long long>(c.readUsMax), static_cast<unsigned long long>(c.statCount),
            static_cast<unsigned long long>(c.statCount ? c.statUsTotal / c.statCount : 0), c.windowUsed / mb,
            c.windowSize / mb, c.windowPeak / mb, static_cast<unsigned long long>(c.bigCount), c.bigBytes / mb);
        if (n < 0 || size_t(n) >= cap || !g_s.header) return;
        const ipc::HostCounters& h = g_s.header->counters;
        std::snprintf(out + n, cap - size_t(n),
            " | host side: store %.1f MB (%.1f MB raw, %llu entries, %.0f MB of sections), private %.1f MB, busy %.1f s, "
            "wakeups %llu, errors %llu",
            h.storeBytes.load() / mb, h.storeRawBytes.load() / mb, static_cast<unsigned long long>(h.storeEntries.load()),
            h.storeCapacity.load() / mb, h.privateBytes.load() / mb, h.busyNs.load() / 1e9,
            static_cast<unsigned long long>(h.wakeups.load()), static_cast<unsigned long long>(h.errors.load()));
    }
}
