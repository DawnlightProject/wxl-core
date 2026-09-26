// wxl-host: files and texture images for the client, read through the cache.
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

#include "host/Assets.hpp"

#include "host/Archives.hpp"
#include "host/Blp.hpp"
#include "ipc/Ring.hpp"

#include <cstring>
#include <memory>
#include <vector>

namespace wxl::hostd::assets
{
    namespace
    {
        using ipc::Status;

        ipc::HostCounters* g_counters = nullptr;

        cache::Key KeyOf(const archives::Located& at, cache::Kind kind)
        {
            cache::Key k;
            k.storage = at.storage;
            k.stamp = at.stamp;
            k.inner = cache::Normalize(at.inner);
            k.kind = kind;
            return k;
        }

        bool EndsWith(const std::string& s, const char* suffix)
        {
            const size_t n = std::strlen(suffix);
            return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
        }

        uint32_t U32(const uint8_t* p)
        {
            uint32_t v;
            std::memcpy(&v, p, 4);
            return v;
        }

        /// A decoded image serves a device whose largest texture edge still holds it.
        bool Fits(const cache::Blob& image, uint32_t maxEdge)
        {
            if (!image || image->size() < blp::kHeaderBytes || !maxEdge) return false;
            return U32(image->data() + blp::field::kWidth) <= maxEdge && U32(image->data() + blp::field::kHeight) <= maxEdge;
        }

        /// Decodes a palettized file into a new blob, or null when the client should decode it itself.
        cache::Blob DecodeBlob(const std::vector<uint8_t>& file, uint32_t maxEdge)
        {
            const uint64_t t0 = ipc::NowUs();
            auto decoded = std::make_shared<std::vector<uint8_t>>();
            if (!blp::Decode(file.data(), file.size(), maxEdge, *decoded)) return nullptr;
            if (g_counters) g_counters->textureDecodeNs.fetch_add((ipc::NowUs() - t0) * 1000, std::memory_order_relaxed);
            return decoded;
        }
    }

    void Init(ipc::HostCounters* counters)
    {
        g_counters = counters;
    }

    bool Cacheable(const std::string& lowerName)
    {
        static const char* const kSuffixes[] = { ".blp", ".m2", ".skin", ".anim", ".wmo", ".adt", ".bls", ".wdt", ".wdl" };
        for (const char* s : kSuffixes)
            if (EndsWith(lowerName, s)) return true;
        return false;
    }

    Status ReadFile(uint32_t thread, const char* name, uint32_t archive, uint8_t* dst, uint64_t cap, Served& out)
    {
        out = Served{};
        archives::Located at;
        Status s = archives::Locate(thread, name, archive, at);
        if (s != Status::Ok) return s;
        out.archive = at.id;
        out.immutable = at.kind == archives::kKindFile;
        out.key = KeyOf(at, cache::Kind::File);

        if (!cache::Enabled() || !Cacheable(out.key.inner))
        {
            s = archives::ReadLocated(thread, at, dst, cap, out.size);
            return s;
        }

        if (cache::Blob hit = cache::Find(out.key, false))
        {
            out.bytes = std::move(hit);
            out.size = out.bytes->size();
            out.flags = ipc::kServedFromCache;
            // Only a reply that carries the bytes counts as the client using them (not a NeedMore).
            if (out.size <= cap && cache::Touch(out.key)) out.flags |= ipc::kServedPrefetched;
            return Status::Ok;
        }
        cache::CountMiss();
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        s = archives::ReadLocated(thread, at, *bytes);
        if (s != Status::Ok) return s;
        out.size = bytes->size();
        out.bytes = bytes;
        cache::Insert(out.key, std::move(bytes), false);
        return Status::Ok;
    }

    Status ReadTexture(uint32_t thread, const char* name, uint32_t archive, uint32_t maxEdge, uint64_t cap, Served& out)
    {
        out = Served{};
        archives::Located at;
        Status s = archives::Locate(thread, name, archive, at);
        if (s != Status::Ok) return s;
        out.archive = at.id;
        out.immutable = at.kind == archives::kKindFile;
        const cache::Key textureKey = KeyOf(at, cache::Kind::Texture);
        const cache::Key fileKey = KeyOf(at, cache::Kind::File);

        cache::Key used;        // the entry a reply that carries the image marks used
        bool cached = false;
        cache::Blob image = cache::Find(textureKey, false);
        if (image && !Fits(image, maxEdge)) image = nullptr;
        if (image)
        {
            cached = true;
            used = textureKey;
            out.key = textureKey;
            out.image = ipc::TextureImage::Decoded;
        }
        else
        {
            cache::Blob file = cache::Find(fileKey, false);
            if (file)
            {
                cached = true;
                used = fileKey;
            }
            else
            {
                cache::CountMiss();
                auto bytes = std::make_shared<std::vector<uint8_t>>();
                s = archives::ReadLocated(thread, at, *bytes);
                if (s != Status::Ok) return s;
                file = std::move(bytes);
            }
            image = DecodeBlob(*file, maxEdge);
            if (image)
            {
                out.key = textureKey;
                out.image = ipc::TextureImage::Decoded;
                cache::Insert(textureKey, image, false);
            }
            else
            {
                out.key = fileKey;
                image = file;
                if (!cached) cache::Insert(fileKey, file, false);
            }
        }

        out.bytes = std::move(image);
        out.size = out.bytes->size();
        if (out.size > cap) return Status::Ok;   // NeedMore for the caller: counted when the retry carries it
        if (cached)
        {
            out.flags = ipc::kServedFromCache;
            if (cache::Touch(used)) out.flags |= ipc::kServedPrefetched;
        }
        if (g_counters)
        {
            g_counters->textureReads.fetch_add(1, std::memory_order_relaxed);
            if (out.image == ipc::TextureImage::Decoded) g_counters->textureDecoded.fetch_add(1, std::memory_order_relaxed);
            g_counters->textureBytes.fetch_add(out.size, std::memory_order_relaxed);
        }
        return Status::Ok;
    }

    cache::Blob Prefetch(uint32_t thread, const std::string& name, bool texture, uint32_t maxEdge, bool* read)
    {
        if (read) *read = false;
        archives::Located at;
        if (archives::Locate(thread, name.c_str(), ipc::kAnyArchive, at) != Status::Ok) return nullptr;
        const cache::Key fileKey = KeyOf(at, cache::Kind::File);
        if (texture)
        {
            const cache::Key textureKey = KeyOf(at, cache::Kind::Texture);
            if (cache::Contains(textureKey)) return cache::Find(textureKey, false);
        }
        if (cache::Contains(fileKey)) return cache::Find(fileKey, false);

        auto bytes = std::make_shared<std::vector<uint8_t>>();
        if (archives::ReadLocated(thread, at, *bytes) != Status::Ok) return nullptr;
        if (read) *read = true;
        if (texture && maxEdge)
        {
            if (cache::Blob decoded = DecodeBlob(*bytes, maxEdge))
            {
                cache::Insert(KeyOf(at, cache::Kind::Texture), decoded, true);
                return decoded;
            }
        }
        cache::Blob blob = std::move(bytes);
        cache::Insert(fileKey, blob, true);
        return blob;
    }

    cache::Blob Rebuild(uint32_t thread, const cache::Key& key)
    {
        if (key.stamp) return nullptr;   // a folder file may have changed since: not rebuilt
        archives::Located at;
        if (!archives::Relocate(key.storage, key.inner, at)) return nullptr;
        auto bytes = std::make_shared<std::vector<uint8_t>>();
        if (archives::ReadLocated(thread, at, *bytes) != Status::Ok) return nullptr;
        cache::Blob blob;
        if (key.kind == cache::Kind::Texture)
        {
            blob = DecodeBlob(*bytes, 0xFFFFFFFFu);
            if (!blob) return nullptr;
        }
        else
        {
            blob = std::move(bytes);
        }
        cache::Insert(key, blob, false);
        return blob;
    }
}
