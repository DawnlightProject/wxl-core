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

#pragma once

#include "ipc/Protocol.hpp"

#include <windows.h>
#include <cstddef>
#include <cstdint>

// Depends only on src/common and src/ipc, so the probe tool can link it without the rest of the core.
// Every function is thread-safe and returns a failure instead of blocking past its timeout.
namespace wxl::host
{
    enum class State : uint32_t
    {
        Off = 0,    // not started yet
        Disabled,   // WXL_HOST=0
        Starting,
        Ready,
        Down,       // lost, a restart is pending
        Failed,     // could not start, or restarts exhausted: native paths for the rest of the session
    };

    struct Reply
    {
        ipc::Status status = ipc::Status::Failed;
        uint64_t    r0 = 0, r1 = 0, r2 = 0, r3 = 0, r4 = 0;
    };

    /// A block of the transfer window, named by its offset.
    struct Block
    {
        uint64_t offset = 0;
        uint8_t* data = nullptr;
        uint64_t size = 0;
    };

    /// A client-created section for one large payload.
    struct BigSection
    {
        uint64_t id = 0;
        HANDLE   handle = nullptr;
        uint8_t* view = nullptr;
        uint64_t size = 0;
    };

    /// Bytes of one file served by the host: in the window, or in a big section.
    struct FileData
    {
        const uint8_t* data = nullptr;
        uint64_t       size = 0;
        uint32_t       archive = ipc::kAnyArchive;  // client archive id it came from
        Block          block;
        BigSection     big;
        uint32_t       image = 0;    // a texture read: ipc::TextureImage
        uint32_t       served = 0;   // ipc::kServedFromCache, ipc::kServedPrefetched
        uint32_t       hostUs = 0;   // time the host spent on it
    };

    /// What a read asks for beyond a whole file.
    struct ReadOptions
    {
        uint32_t flags = 0;      // ipc::kFlagTexture
        uint64_t a4 = 0;         // a texture read: the device's largest texture edge
        uint64_t sizeHint = 0;   // first block size when none is remembered for the name
    };

    /// Called on a completed async request, on whichever thread drained it. Must not block.
    using Completion = void (*)(void* ctx, const Reply& reply);

    // --- lifecycle ---------------------------------------------------------------------------------

    /// True unless WXL_HOST=0.
    bool Enabled();

    /// Tools only: the folder holding wxl-host.exe and the archives. The client uses Wow.exe's folder.
    void SetClientRoot(const wchar_t* root);

    /**
     * @brief Starts the host once, synchronously; later calls return the current answer at once.
     * @return true when the host is serving.
     */
    bool Start();

    State CurrentState();
    /// The serving host's process id, 0 when none.
    uint32_t HostPid();
    bool Available();

    /// Waits until the host is serving or given up (a restart in progress settles either way).
    bool WaitSettled(uint32_t timeoutMs);
    uint32_t Generation();

    /**
     * @brief Registers a callback run by the monitor thread after the host restarted (mounts replayed)
     *        or was given up for the session.
     */
    void SetStateListener(void (*listener)(State state));

    // --- requests ----------------------------------------------------------------------------------

    /// A request's fields, the ticket being set by the call.
    struct Params
    {
        ipc::Op     op = ipc::Op::None;
        const char* name = nullptr;
        uint64_t    a0 = 0, a1 = 0, a2 = 0, a3 = 0, a4 = 0;
        uint32_t    flags = 0;
    };

    /// Sends a request and waits for its reply. Returns false when the host is unavailable or times out.
    bool Call(const Params& params, Reply& out, uint32_t timeoutMs);

    /**
     * @brief Sends a request without waiting.
     * @param done  optional completion; when set, the ticket is finished for the caller.
     * @return the ticket, 0 when the host is unavailable.
     */
    uint64_t Post(const Params& params, Completion done = nullptr, void* ctx = nullptr);

    /// 1 done (out filled), 0 pending, -1 unknown or lost ticket.
    int Poll(uint64_t ticket, Reply& out);

    /// Waits up to timeoutMs; true when done (out filled, status may be an error).
    bool Wait(uint64_t ticket, Reply& out, uint32_t timeoutMs);

    // --- memory ------------------------------------------------------------------------------------

    bool Alloc(uint64_t size, Block& out);
    void Free(Block& block);
    /// Gives back the tail of a block, keeping its first newSize bytes.
    void Shrink(Block& block, uint64_t newSize);

    bool CreateBig(uint64_t size, BigSection& out);
    void FreeBig(BigSection& big);

    /// Largest payload served through the window rather than a big section.
    uint64_t BigThreshold();

    // --- files and archives ------------------------------------------------------------------------

    /// Reads a whole file. archive is a client archive id or ipc::kAnyArchive.
    ipc::Status ReadFile(const char* name, uint32_t archive, FileData& out, uint32_t timeoutMs = 0,
                         const ReadOptions* options = nullptr);
    void ReleaseFile(FileData& file);

    /// A read in flight, for callers that do not wait (a read may take a second round for large files).
    struct ReadOp;

    /// Starts a read; null when the host is unavailable.
    ReadOp* ReadBegin(const char* name, uint32_t archive, const ReadOptions* options = nullptr);

    /**
     * @brief Advances a read, waiting up to waitMs (0 polls).
     * @return true when finished: status is set, out is filled on Ok, and op is freed.
     */
    bool ReadStep(ReadOp* op, FileData& out, ipc::Status& status, uint32_t waitMs);

    /// Gives up on a read; what the host may still write into is freed when it answers.
    void ReadCancel(ReadOp* op);

    /// Looks a file up; size and where may be null.
    ipc::Status StatFile(const char* name, uint32_t archive, uint32_t flags, uint64_t* size, uint32_t* where);

    /**
     * @brief Mounts an archive on the host and records it for replay after a restart.
     * @param parent  client id of the parent archive, or ipc::kAnyArchive.
     * @param id      receives a client archive id, stable across host restarts.
     * @param kind    receives 1 for an archive file, 2 for a folder, 3 for a view inside a parent.
     * @param clientError  receives the error the client's own archive layer would report on a failure.
     */
    ipc::Status Mount(const char* name, int priority, uint32_t parent, uint32_t& id, uint32_t& kind,
                      uint32_t* clientError = nullptr);
    void Unmount(uint32_t id);

    // --- store -------------------------------------------------------------------------------------

    /// Copies bytes into a window block and hands them to the host. wait=false returns after the copy.
    bool StorePut(uint64_t key, const void* data, uint64_t size, ipc::Codec codec, bool wait);
    bool StorePatch(uint64_t key, uint64_t offset, const void* data, uint64_t size);
    /// Copies a stored value into dst. Returns false on a miss or failure; full receives the value's size.
    bool StoreGet(uint64_t key, void* dst, uint64_t cap, uint64_t* full, uint32_t timeoutMs);
    void StoreDrop(uint64_t key);

    /// Copies rows (rowBytes each, srcPitch apart) into a stored value at dstOffset + r * dstPitch. Async.
    bool StorePatchRows(uint64_t key, uint64_t dstOffset, uint64_t dstPitch, const void* src, uint64_t rowBytes,
                        uint64_t rows, uint64_t srcPitch);

    /// Forgets count consecutive keys from first.
    void StoreDropRange(uint64_t first, uint32_t count);

    // --- counters ----------------------------------------------------------------------------------

    struct Counters
    {
        uint64_t calls, fallbacks, restarts;
        uint64_t readCount, readBytes, readUsTotal, readUsMax;
        uint64_t pingCount, pingUsTotal, pingUsMax;
        uint64_t statCount, statUsTotal;
        uint64_t windowUsed, windowPeak, windowSize;
        uint64_t bigCount, bigBytes;
    };
    void ReadCounters(Counters& out);

    /// Counts one call that took the client's own path because the host could not serve it.
    void CountFallback();

    /// One human-readable line of counters for the memory report.
    void CounterLine(char* out, size_t cap);

    /// The host's own counters from the shared header, or null when there is no session.
    const ipc::HostCounters* HostSideCounters();

    /// True when the session was set up with a host cache (texture images and prefetch depend on it).
    bool CacheEnabled();
}
