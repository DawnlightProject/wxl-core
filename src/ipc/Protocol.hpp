// The wxl-host wire contract: shared-memory layout and messages, identical in the x86 and x64 builds.
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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>

// Everything here is fixed-width, pointer-free and pinned by static_asserts, because the 32-bit client
// and the 64-bit host map the same bytes. Any change bumps kProtocolVersion; the host refuses a client
// whose version or struct sizes differ from its own, and the client falls back.
namespace wxl::ipc
{
    constexpr uint32_t kMagic           = 0x54534858; // 'XHST'
    constexpr uint32_t kProtocolVersion = 3;

    constexpr uint32_t kChannelCount  = 16;   // channel 0 is shared; 1..15 are taken by one thread each
    constexpr uint32_t kRequestSlots  = 64;   // per channel, power of two
    constexpr uint32_t kResponseSlots = 128;  // per channel, power of two
    constexpr uint32_t kNameMax       = 320;  // inline name capacity, terminator included
    constexpr uint32_t kRootMax       = 520;  // client root folder, UTF-8, terminator included

    constexpr uint32_t kAnyArchive = 0xFFFFFFFFu;

    enum class Op : uint32_t
    {
        None = 0,
        Ping,       // -> r0 host QPC
        Mount,      // name, a0 priority, a1 parent archive or kAnyArchive -> r0 archive id, r1 kind (1 file, 2 folder)
        Unmount,    // a0 archive id
        Stat,       // name, a0 archive id or kAnyArchive, a1 open flags -> r0 size, r1 archive id
        Read,       // name, a0 archive, a1 destination (Dest), a2 offset or big-section id, a3 capacity
                    //   -> r0 size, r1 archive id; NeedMore when size > capacity
                    //   with kFlagTexture: a4 the device's largest texture edge; the bytes are the texture
                    //   image (TextureImage), r2 its kind, r3 ServeFlags, r4 host microseconds
        StorePut,   // a0 key, a1 window offset, a2 size, a3 codec -> consumed
        StorePatch, // a0 key, a1 window offset, a2 size, a3 destination offset in the stored bytes;
                    //   a4 non-zero: rows, a4 = (row bytes << 32) | destination pitch, rows packed in the source
        StoreGet,   // a0 key, a1 window offset, a2 capacity -> r0 size
        StoreDrop,  // a0 key, a1 count of consecutive keys (0 means 1)
        Job,        // a0 kind, a1 input offset, a2 input size, a3 output offset, a4 output capacity -> r0 output size
        Stats,      // -> refreshes the host counters in the header
        Shutdown,   // the client is closing
        Hint,       // name map folder ("World\Maps\<Map>"), a0 x|y, a1 vx|vy (yards, yards/s, float bits),
                    //   a2 view distance, a3 map id: where the client is headed, for prefetch
        Count
    };

    /// What a texture read returns (Read with kFlagTexture, r2).
    enum class TextureImage : uint32_t
    {
        File = 0,      // the BLP file itself: the client's loader decodes it
        Decoded = 1,   // a BLP2 image in the client's upload format (ARGB8888), compression 3
    };

    /// How a read was served (r3).
    constexpr uint64_t kServedFromCache  = 1u << 0;
    constexpr uint64_t kServedPrefetched = 1u << 1;   // the cache entry came from a prefetch

    enum class Status : int32_t
    {
        Ok = 0,
        NotFound,
        NeedMore,
        BadRequest,
        Failed,
        Busy,
        Unsupported,
    };

    /// Where a Read writes.
    enum class Dest : uint32_t
    {
        Window = 0,      // a2 is an offset in the transfer window
        BigSection = 1,  // a2 is the id of a client-created section named by BigSectionName
    };

    /// Stored-bytes codecs.
    enum class Codec : uint32_t { Raw = 0, Lz4 = 1, Zstd = 2 };

    /// Host job kinds.
    enum class JobKind : uint32_t
    {
        Lz4Compress = 1,
        Lz4Decompress,
        ZstdCompress,
        ZstdDecompress,
        Prefetch,       // a request name: read the file into the host's cache, no output
    };

    /// Host lifecycle, written by the host into Header::hostState.
    enum class HostState : uint32_t { None = 0, Starting, Ready, Refused, Exiting };

    /// Request flags.
    constexpr uint32_t kFlagNoReply = 1u << 0;  // the client does not wait; the host still answers for bookkeeping
    constexpr uint32_t kFlagTexture = 1u << 1;  // Read: the texture image of a BLP (see TextureImage)

#pragma pack(push, 8)
    struct Request
    {
        uint32_t op;
        uint32_t flags;
        uint64_t ticket;
        uint64_t a0, a1, a2, a3, a4;
        uint32_t nameLen;
        uint32_t reserved;
        char     name[kNameMax];
    };

    struct Response
    {
        uint32_t op;
        int32_t  status;
        uint64_t ticket;
        uint64_t r0, r1, r2, r3, r4;
        uint64_t reserved;
    };
#pragma pack(pop)

    static_assert(sizeof(Request) == 384, "Request must be 384 bytes in both builds");
    static_assert(offsetof(Request, ticket) == 8 && offsetof(Request, a0) == 16 && offsetof(Request, a4) == 48
                      && offsetof(Request, nameLen) == 56 && offsetof(Request, name) == 64,
                  "Request field offsets must match in both builds");
    static_assert(sizeof(Response) == 64, "Response must be one cache line");
    static_assert(offsetof(Response, ticket) == 8 && offsetof(Response, r0) == 16 && offsetof(Response, r4) == 48,
                  "Response field offsets must match in both builds");

    static_assert(sizeof(std::atomic<uint32_t>) == 4 && std::atomic<uint32_t>::is_always_lock_free,
                  "32-bit atomics must be plain and lock-free in shared memory");
    static_assert(sizeof(std::atomic<uint64_t>) == 8 && std::atomic<uint64_t>::is_always_lock_free,
                  "64-bit atomics must be plain and lock-free in shared memory");

    /// A ring index alone on its cache line, so producer and consumer never share one.
    struct alignas(64) RingIndex
    {
        std::atomic<uint32_t> value;
        uint8_t               pad[60];
    };
    static_assert(sizeof(RingIndex) == 64, "RingIndex is one cache line");

    /// One channel: a request ring the client produces and a response ring the host produces.
    struct alignas(64) Channel
    {
        RingIndex reqWrite;    // client
        RingIndex reqRead;     // host
        RingIndex respWrite;   // host
        RingIndex respRead;    // client
        alignas(64) std::atomic<uint32_t> clientWaiting; // set by a client about to sleep on the channel event
        uint32_t owner;        // client thread id holding the channel, 0 when free (client bookkeeping)
        uint8_t  pad[56];
        Request  requests[kRequestSlots];
        Response responses[kResponseSlots];
    };
    static_assert(sizeof(Channel) == 64 * 5 + sizeof(Request) * kRequestSlots + sizeof(Response) * kResponseSlots,
                  "Channel layout must not depend on the build");
    static_assert(offsetof(Channel, requests) == 320, "Channel requests start after the control lines");

    /// Counters the host keeps and the client reads. Relaxed atomics; for the log, not for decisions.
    struct HostCounters
    {
        std::atomic<uint64_t> requests[static_cast<uint32_t>(Op::Count)];
        std::atomic<uint64_t> readBytes;
        std::atomic<uint64_t> readMisses;
        std::atomic<uint64_t> readNeedMore;
        std::atomic<uint64_t> busyNs;          // time spent serving requests
        std::atomic<uint64_t> storeBytes;      // bytes held in the store after compression
        std::atomic<uint64_t> storeRawBytes;   // bytes before compression
        std::atomic<uint64_t> storeEntries;
        std::atomic<uint64_t> storeCapacity;   // bytes of store sections created
        std::atomic<uint64_t> mounts;
        std::atomic<uint64_t> errors;
        std::atomic<uint64_t> wakeups;         // dispatcher wakeups from its event
        std::atomic<uint64_t> workingSet;      // host process working set, refreshed on Stats
        std::atomic<uint64_t> privateBytes;    // host process private bytes, refreshed on Stats

        // Texture images (Read with kFlagTexture).
        std::atomic<uint64_t> textureReads;
        std::atomic<uint64_t> textureDecoded;  // served as a decoded image
        std::atomic<uint64_t> textureBytes;    // image bytes written for the client
        std::atomic<uint64_t> textureDecodeNs; // time spent decoding
        // The host cache (raw files and decoded images).
        std::atomic<uint64_t> cacheBytes;
        std::atomic<uint64_t> cacheEntries;
        std::atomic<uint64_t> cacheLimit;
        std::atomic<uint64_t> cacheHits;       // on-demand lookups served from the cache
        std::atomic<uint64_t> cacheMisses;     // on-demand lookups that read the archives
        std::atomic<uint64_t> cacheEvictions;
        // Prefetch.
        std::atomic<uint64_t> prefetchQueued;  // files queued (hints and what served files reference)
        std::atomic<uint64_t> prefetchDone;    // files read into the cache
        std::atomic<uint64_t> prefetchBytes;
        std::atomic<uint64_t> prefetchHits;    // on-demand reads served by a prefetched entry
        std::atomic<uint64_t> prefetchWasted;  // prefetched entries evicted unread
        std::atomic<uint64_t> prefetchHints;
        // Backing copies replaced by a reference to the file they came from.
        std::atomic<uint64_t> dedupeValues;
        std::atomic<uint64_t> dedupeBytes;
        std::atomic<uint64_t> dedupeRegenerated;
        std::atomic<uint64_t> dedupeFailed;
        std::atomic<uint64_t> reserved[8];
    };

    /// The control section header. 4 KB, followed by the channels.
    struct alignas(64) Header
    {
        uint32_t magic;
        uint32_t version;
        uint32_t headerSize;
        uint32_t channelSize;
        uint32_t requestSize;
        uint32_t responseSize;
        uint32_t channelCount;
        uint32_t requestSlots;
        uint32_t responseSlots;
        uint32_t clientPid;
        uint32_t generation;       // bumped by the client for every spawn
        uint32_t hostPid;          // written by the host
        uint32_t hostGeneration;   // written by the host once it accepted the session
        uint32_t hostVersion;      // the host's own protocol version, for a refusal message
        uint64_t windowSize;       // transfer section size
        uint64_t controlSize;      // control section size
        uint32_t spinUs;           // how long either side spins before sleeping
        uint32_t workerCount;      // requested host worker threads, 0 for automatic
        uint64_t storeLimit;       // bytes the store may grow to
        uint64_t cacheLimit;       // bytes the host cache may hold, 0 to disable it
        uint32_t prefetch;         // non-zero: prefetch what the client is likely to read next
        uint32_t dedupe;           // non-zero: backing copies of file content become references
        uint64_t reservedA[2];

        alignas(64) std::atomic<uint32_t> hostState;
        alignas(64) std::atomic<uint64_t> heartbeat;     // bumped by the host every 100 ms
        alignas(64) std::atomic<uint32_t> hostSleeping;  // the dispatcher is about to wait on the doorbell
        alignas(64) HostCounters counters;
        alignas(64) char clientRoot[kRootMax];           // the client folder, for relative archive paths
    };
    static_assert(sizeof(Header) == 1344, "Header must have one layout in both builds");
    static_assert(sizeof(Header) <= 4096, "Header must fit its 4 KB page");

    constexpr uint32_t kHeaderBytes = 4096;

    /// Byte offset of channel i in the control section.
    constexpr uint64_t ChannelOffset(uint32_t i)
    {
        return kHeaderBytes + uint64_t(i) * sizeof(Channel);
    }

    /// Total control section size.
    constexpr uint64_t ControlSize()
    {
        return ChannelOffset(kChannelCount);
    }

    // Object names. The client PID keeps two clients apart; the host opens what the client created.
    inline void ControlName(wchar_t* out, size_t cap, uint32_t pid)  { _snwprintf_s(out, cap, _TRUNCATE, L"Local\\wxl-host-%u-ctl", pid); }
    inline void WindowName(wchar_t* out, size_t cap, uint32_t pid)   { _snwprintf_s(out, cap, _TRUNCATE, L"Local\\wxl-host-%u-xfer", pid); }
    inline void DoorbellName(wchar_t* out, size_t cap, uint32_t pid) { _snwprintf_s(out, cap, _TRUNCATE, L"Local\\wxl-host-%u-door", pid); }
    inline void ReadyName(wchar_t* out, size_t cap, uint32_t pid)    { _snwprintf_s(out, cap, _TRUNCATE, L"Local\\wxl-host-%u-ready", pid); }
    inline void ChannelEventName(wchar_t* out, size_t cap, uint32_t pid, uint32_t channel)
    {
        _snwprintf_s(out, cap, _TRUNCATE, L"Local\\wxl-host-%u-ch%u", pid, channel);
    }
    inline void BigSectionName(wchar_t* out, size_t cap, uint32_t pid, uint64_t id)
    {
        _snwprintf_s(out, cap, _TRUNCATE, L"Local\\wxl-host-%u-big-%llu", pid, static_cast<unsigned long long>(id));
    }
}
