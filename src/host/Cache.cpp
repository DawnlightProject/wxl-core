// wxl-host: a bounded cache of files and decoded texture images, kept out of Wow.exe's address space.
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

#include "host/Cache.hpp"

#include <windows.h>

#include <cctype>
#include <list>
#include <mutex>
#include <unordered_map>

namespace wxl::hostd::cache
{
    namespace
    {
        constexpr ULONGLONG kRecentMs = 20000;   // an entry used this recently is not evicted by a prefetch

        struct Node
        {
            Key       key;
            Blob      blob;
            uint64_t  bytes = 0;
            ULONGLONG used = 0;
            bool      prefetched = false;   // inserted by a prefetch and not read by the client yet
        };

        std::mutex                                                   g_mutex;
        std::list<Node>                                              g_lru;   // most recent first
        std::unordered_map<Key, std::list<Node>::iterator, KeyHash> g_map;
        uint64_t                                                     g_limit = 0;
        uint64_t                                                     g_bytes = 0;
        ipc::HostCounters*                                           g_counters = nullptr;

        void Publish()
        {
            if (!g_counters) return;
            g_counters->cacheBytes.store(g_bytes, std::memory_order_relaxed);
            g_counters->cacheEntries.store(g_map.size(), std::memory_order_relaxed);
        }

        /// Removes one node. Called with g_mutex held.
        void Remove(std::list<Node>::iterator it, bool evicted)
        {
            if (g_counters && evicted)
            {
                g_counters->cacheEvictions.fetch_add(1, std::memory_order_relaxed);
                if (it->prefetched) g_counters->prefetchWasted.fetch_add(1, std::memory_order_relaxed);
            }
            g_bytes -= it->bytes;
            g_map.erase(it->key);
            g_lru.erase(it);
        }
    }

    size_t KeyHash::operator()(const Key& k) const
    {
        size_t h = std::hash<std::string>()(k.inner);
        h ^= (size_t(k.storage) * 0x9E3779B97F4A7C15ull) + (size_t(k.kind) << 1);
        h ^= size_t(k.stamp) + 0x7F4A7C15ull + (h << 6) + (h >> 2);
        return h;
    }

    std::string Normalize(const std::string& name)
    {
        std::string s(name);
        for (char& c : s) c = c == '/' ? '\\' : char(tolower(static_cast<unsigned char>(c)));
        return s;
    }

    void Init(uint64_t limitBytes, ipc::HostCounters* counters)
    {
        g_limit = limitBytes;
        g_counters = counters;
        if (g_counters) g_counters->cacheLimit.store(g_limit);
    }

    bool Enabled()
    {
        return g_limit != 0;
    }

    Blob Find(const Key& key, bool onDemand, bool* prefetched)
    {
        if (prefetched) *prefetched = false;
        if (!g_limit) return nullptr;
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_map.find(key);
        if (it == g_map.end()) return nullptr;
        Node& n = *it->second;
        if (onDemand)
        {
            if (prefetched) *prefetched = n.prefetched;
            if (g_counters)
            {
                g_counters->cacheHits.fetch_add(1, std::memory_order_relaxed);
                if (n.prefetched) g_counters->prefetchHits.fetch_add(1, std::memory_order_relaxed);
            }
            n.prefetched = false;
            n.used = GetTickCount64();
            g_lru.splice(g_lru.begin(), g_lru, it->second);
        }
        return n.blob;
    }

    bool Touch(const Key& key)
    {
        if (!g_limit) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_map.find(key);
        if (it == g_map.end()) return false;
        Node& n = *it->second;
        const bool prefetched = n.prefetched;
        if (g_counters)
        {
            g_counters->cacheHits.fetch_add(1, std::memory_order_relaxed);
            if (prefetched) g_counters->prefetchHits.fetch_add(1, std::memory_order_relaxed);
        }
        n.prefetched = false;
        n.used = GetTickCount64();
        g_lru.splice(g_lru.begin(), g_lru, it->second);
        return prefetched;
    }

    bool Contains(const Key& key)
    {
        if (!g_limit) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_map.count(key) != 0;
    }

    void CountMiss()
    {
        if (g_counters) g_counters->cacheMisses.fetch_add(1, std::memory_order_relaxed);
    }

    bool Insert(const Key& key, Blob blob, bool prefetched)
    {
        if (!g_limit || !blob) return false;
        const uint64_t bytes = blob->size() + key.inner.size() + 96;
        if (bytes > g_limit / 8) return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto existing = g_map.find(key);
        if (existing != g_map.end())
        {
            if (prefetched) return true;   // the client read it in the meantime: keep that copy
            Remove(existing->second, false);
        }
        const ULONGLONG now = GetTickCount64();
        while (g_bytes + bytes > g_limit && !g_lru.empty())
        {
            auto last = std::prev(g_lru.end());
            if (prefetched && now - last->used < kRecentMs) return false;
            Remove(last, true);
        }
        g_lru.push_front(Node{ key, std::move(blob), bytes, prefetched ? 0 : now, prefetched });
        g_map.emplace(key, g_lru.begin());
        g_bytes += bytes;
        Publish();
        return true;
    }

    void DropStorage(uint32_t storage)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto it = g_lru.begin(); it != g_lru.end();)
        {
            auto next = std::next(it);
            if (it->key.storage == storage) Remove(it, false);
            it = next;
        }
        Publish();
    }
}
