// wxl.host: the core's 64-bit companion process, published for extensions.
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

#ifndef WXL_HOST_API_H
#define WXL_HOST_API_H

#include <stdint.h>

// Reached through WXL_Api::GetInterface("wxl.host", WXL_HOST_API_VERSION), or from outside the extension
// loader through WarcraftXL.dll's WXL_GetHostApi export. Every call is safe when the host is off: it
// returns WXL_HOST_E_UNAVAILABLE and the caller takes its own path. Plain C, __cdecl everywhere.
//
// Bytes come back as a WXL_HostBlock: a read-only (file) or read-write (Alloc) view that stays valid
// until Release. File views are zero-copy: they point into memory the host wrote directly.

#ifdef __cplusplus
extern "C" {
#endif

#define WXL_HOST_API_VERSION 1

#define WXL_HOST_OK              0
#define WXL_HOST_E_UNAVAILABLE  -1  // the host is off, starting, or down
#define WXL_HOST_E_NOT_FOUND    -2
#define WXL_HOST_E_TIMEOUT      -3
#define WXL_HOST_E_NO_MEMORY    -4  // the transfer window or a section could not be allocated
#define WXL_HOST_E_BAD_ARGUMENT -5
#define WXL_HOST_E_FAILED       -6
#define WXL_HOST_PENDING         1  // Poll: not finished yet

/// Store codecs.
#define WXL_HOST_CODEC_RAW  0
#define WXL_HOST_CODEC_LZ4  1
#define WXL_HOST_CODEC_ZSTD 2

/// Job kinds for SubmitJob.
#define WXL_HOST_JOB_LZ4_COMPRESS    1
#define WXL_HOST_JOB_LZ4_DECOMPRESS  2
#define WXL_HOST_JOB_ZSTD_COMPRESS   3
#define WXL_HOST_JOB_ZSTD_DECOMPRESS 4
#define WXL_HOST_JOB_PREFETCH        5  // reads a file on the host so a later read is fast; no output

/// Read flags.
#define WXL_HOST_READ_NATIVE_NAME 0u  // the name is resolved exactly as the client resolves its own files

/**
 * @brief A view of shared bytes. Fill nothing in; the host API writes it.
 *
 * data stays valid until Release. id identifies the block for Release and must not be interpreted.
 */
typedef struct WXL_HostBlock
{
    void*    data;
    uint64_t size;
    uint64_t id;
    uint64_t reserved;
} WXL_HostBlock;

/// Counters, for display. Latencies are microseconds, averaged since the process started.
typedef struct WXL_HostStats
{
    uint32_t structSize;
    uint32_t available;       // 1 when the host is serving
    uint32_t generation;      // number of host processes started this session
    uint32_t restarts;
    uint64_t requests;
    uint64_t readBytes;
    uint64_t readCount;
    uint32_t readAvgUs;
    uint32_t readMaxUs;
    uint32_t pingAvgUs;
    uint32_t pingMaxUs;
    uint64_t windowSize;
    uint64_t windowUsed;
    uint64_t windowPeak;
    uint64_t storeBytes;      // compressed bytes the host holds
    uint64_t storeRawBytes;   // the same, uncompressed
    uint64_t storeEntries;
    uint64_t hostPrivateBytes;
    uint64_t fallbacks;       // calls answered by the client's own path because the host could not
} WXL_HostStats;

typedef struct WXL_HostApi
{
    uint32_t structSize;
    uint32_t apiVersion;

    /// Non-zero while the host is serving.
    int(__cdecl* IsAvailable)(void);

    /// Fills the counters. Safe at any time.
    void(__cdecl* GetStats)(WXL_HostStats* out);

    /**
     * @brief Reads a whole file through the host, exactly as the client would find it.
     * @param name       client path ("Textures\\Foo.blp").
     * @param flags      WXL_HOST_READ_* flags.
     * @param out        receives a read-only view; Release it.
     * @param timeoutMs  0 for the default.
     * @return WXL_HOST_OK, WXL_HOST_E_NOT_FOUND, or another error.
     */
    int(__cdecl* ReadFile)(const char* name, uint32_t flags, WXL_HostBlock* out, uint32_t timeoutMs);

    /// Starts a read without waiting. Finish it with Poll or Wait on the returned ticket.
    int(__cdecl* ReadFileAsync)(const char* name, uint32_t flags, uint64_t* ticket);

    /// Checks a ticket: WXL_HOST_OK (out filled), WXL_HOST_PENDING, or an error (the ticket is finished).
    int(__cdecl* Poll)(uint64_t ticket, WXL_HostBlock* out);

    /// Waits for a ticket up to timeoutMs (0 for the default). Same results as Poll.
    int(__cdecl* Wait)(uint64_t ticket, WXL_HostBlock* out, uint32_t timeoutMs);

    /// Tests whether a file exists; size may be null.
    int(__cdecl* FileExists)(const char* name, uint64_t* size);

    /// Allocates a shared read-write block the host can read and write.
    int(__cdecl* Alloc)(uint64_t size, WXL_HostBlock* out);

    /// Releases any block this API returned. Null-safe; the block is zeroed.
    void(__cdecl* Release)(WXL_HostBlock* block);

    /**
     * @brief Parks bytes in the host under a key, out of Wow.exe's address space.
     * @param key    caller-chosen; namespace it (the core uses keys with the top bit set).
     * @param data   bytes to copy.
     * @param size   byte count.
     * @param codec  WXL_HOST_CODEC_*.
     * @param wait   non-zero to wait until the host holds them; zero to return as soon as they are copied.
     */
    int(__cdecl* StorePut)(uint64_t key, const void* data, uint64_t size, uint32_t codec, int wait);

    /// Overwrites part of a stored value; the value grows if offset + size is past its end.
    int(__cdecl* StorePatch)(uint64_t key, uint64_t offset, const void* data, uint64_t size);

    /// Copies a stored value into dst; size receives its full size (larger than cap means truncated).
    int(__cdecl* StoreGet)(uint64_t key, void* dst, uint64_t cap, uint64_t* size, uint32_t timeoutMs);

    /// Forgets a stored value.
    void(__cdecl* StoreDrop)(uint64_t key);

    /**
     * @brief Runs a host job over shared blocks.
     * @param kind     WXL_HOST_JOB_*.
     * @param in       input block from Alloc (or null for PREFETCH).
     * @param inSize   bytes of input used.
     * @param out      output block from Alloc (or null).
     * @param name     file name for PREFETCH, else null.
     * @param ticket   receives the ticket; finish it with Wait/Poll (out->size is the output size).
     */
    int(__cdecl* SubmitJob)(uint32_t kind, const WXL_HostBlock* in, uint64_t inSize, WXL_HostBlock* out,
                            const char* name, uint64_t* ticket);

    /**
     * @brief Overwrites rows of a stored 2D value without waiting (a dirty rectangle of a texture level).
     *
     * Row r of src (srcPitch apart, rowBytes long) lands at dstOffset + r * dstPitch in the stored value,
     * which grows if needed and is created zero-filled if absent.
     */
    int(__cdecl* StorePatchRows)(uint64_t key, uint64_t dstOffset, uint64_t dstPitch, const void* src,
                                 uint64_t rowBytes, uint64_t rows, uint64_t srcPitch);

    /// Forgets count consecutive keys starting at first, without waiting.
    void(__cdecl* StoreDropRange)(uint64_t first, uint32_t count);
} WXL_HostApi;

/// Signature of WarcraftXL.dll's WXL_GetHostApi export, for binaries outside the extension loader.
typedef const WXL_HostApi*(__cdecl* WXL_GetHostApiFn)(void);

#ifdef __cplusplus
}
#endif

#endif // WXL_HOST_API_H
