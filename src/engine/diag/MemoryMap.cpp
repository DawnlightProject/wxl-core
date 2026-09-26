// The memory map: engine allocations by source and subsystem, process memory by owner, loaded modules.
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

// Reads the allocation tracker and the process map and turns them into memory[...] lines and the full
// map file. A site whose name says nothing (operator new, a container of a built-in type) is attributed
// through its owner: the nearest allocation site in Wow.exe's code whose name is known, marked "~".
// Peak windows open on a loading screen or a zone change and log what grew up to the window's peak.

#include "engine/diag/MemoryMap.hpp"

#include "common/Log.hpp"
#include "engine/diag/AllocTrack.hpp"
#include "engine/diag/MemoryStats.hpp"
#include "engine/diag/ProcessMap.hpp"
#include "game/Loading.hpp"
#include "offsets/engine/Crt.hpp"
#include "offsets/game/World.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    namespace alloc = wxl::diag::alloc;
    namespace process = wxl::diag::process;
    namespace memory = wxl::diag::memory;
    using memory::kMb;

    constexpr uint32_t kSlots = alloc::kSiteSlots;
    constexpr uint32_t kFiles = alloc::kFileSlots;
    constexpr uint32_t kInferRange = 0x10000;        // an owner farther than this from a named site stays unnamed
    constexpr ULONGLONG kLoadSettleMs = 5000;         // a loading window stays open this long after the screen
    constexpr ULONGLONG kZoneWindowMs = 20000;
    constexpr ULONGLONG kWindowVaGapMs = 1000;        // address-space walks inside a window, at most
    constexpr size_t kHistory = 24;

    // --- subsystems ----------------------------------------------------------------------------------

    enum Sub : uint8_t
    {
        kM2, kM2Anim, kTerrain, kWmo, kTextures, kDb, kLuaUi, kFonts, kSound, kObjects, kNetwork, kMapSub,
        kParticles, kLiquids, kArchives, kGx, kOther, kSubCount
    };
    const char* const kSubNames[kSubCount] = {
        "M2", "M2 anim", "terrain", "WMO", "textures", "DBC/DB", "Lua/UI", "fonts", "sound", "objects", "network",
        "map", "particles", "liquids", "archives", "gx", "other"
    };

    struct Rule
    {
        const char* needle;
        Sub         sub;
    };

    // First match wins, on the lower-case display name: a file's base name or a demangled type.
    const Rule kRules[] = {
        { "csimple", kLuaUi },   // UI regions (CSimpleTexture, CSimpleFontString) before textures and fonts
        { "liquid", kLiquids },
        { "m2sequence", kM2Anim }, { "sequenceload", kM2Anim }, { "sequenceplayback", kM2Anim },
        { "particle", kParticles }, { "ribbon", kParticles }, { "emitter", kParticles }, { "spellvisual", kParticles },
        { "objecteffect", kParticles }, { "obj_effect", kParticles }, { "lightning", kParticles }, { "ffx", kParticles },
        { "glow", kParticles }, { "shadereffect", kParticles },
        { "font", kFonts }, { "glyph", kFonts }, { "kern", kFonts }, { "charcodedesc", kFonts }, { "cgxstring", kFonts },
        { "embeddedtexture", kFonts },
        { "m2", kM2 }, { "modelblob", kM2 }, { "modelrecord", kM2 }, { "modelfadeout", kM2 },
        { "mapobj", kWmo }, { "wmo", kWmo }, { "portal", kWmo },
        { "mapchunk", kTerrain }, { "maparea", kTerrain }, { "maplowdetail", kTerrain }, { "mapshadow", kTerrain },
        { "detaildoodad", kTerrain }, { "mapmem", kTerrain }, { "vbblist", kTerrain }, { "terrain", kTerrain },
        { "texture", kTextures }, { "blp", kTextures }, { "tga", kTextures },
        { "sound", kSound }, { "fmod", kSound }, { "comsat", kSound }, { "sechannel", kSound }, { "sedriver", kSound },
        { "dsp_", kSound }, { "voice", kSound }, { "footstep", kSound },
        { "wowclientdb", kDb }, { "dbcache", kDb }, { "wdatastore", kDb }, { "namecache", kDb }, { "dancecache", kDb },
        { "npctext", kDb }, { "questcache", kDb }, { "dbclient", kDb }, { "clientdb", kDb },
        { "objectheap", kObjects },
        { "minimap", kMapSub },
        { "netclient", kNetwork }, { "netinternal", kNetwork }, { "connection", kNetwork }, { "tcp", kNetwork },
        { "cdatastore", kNetwork }, { "grunt", kNetwork }, { "battlenet", kNetwork }, { "realm", kNetwork },
        { "warden", kNetwork }, { "clientservices", kNetwork }, { "wowsvcs", kNetwork }, { "download", kNetwork },
        { "netevent", kNetwork }, { "heldmessage", kNetwork }, { "netselsock", kNetwork },
        { "object_c", kObjects }, { "unit_c", kObjects }, { "player_c", kObjects }, { "gameobject_c", kObjects },
        { "corpse_c", kObjects }, { "missile_c", kObjects }, { "vehicle", kObjects }, { "objectmgr", kObjects },
        { "movement", kObjects }, { "spell_c", kObjects }, { "aura", kObjects }, { "cgobject", kObjects },
        { "unitthreat", kObjects }, { "pendingspell", kObjects }, { "spellcast", kObjects }, { "spellhistory", kObjects },
        { "cooldown", kObjects }, { "nearestunit", kObjects }, { "posdelta", kObjects }, { "mounttransition", kObjects },
        { "trajectory", kObjects }, { "minigame", kObjects },
        { "worldframe", kMapSub },
        { "lmempool", kLuaUi }, { "lua", kLuaUi }, { "frame", kLuaUi }, { "csimple", kLuaUi }, { "script", kLuaUi },
        { "xml", kLuaUi }, { "addon", kLuaUi }, { "macro", kLuaUi }, { "binding", kLuaUi }, { "taint", kLuaUi },
        { "layout", kLuaUi }, { "button", kLuaUi }, { "editbox", kLuaUi }, { "html", kLuaUi }, { "gameui", kLuaUi },
        { "glue", kLuaUi }, { "console", kLuaUi }, { "cvar", kLuaUi }, { "chat", kLuaUi }, { "portrait", kLuaUi },
        { "tradeskill", kLuaUi }, { "talent", kLuaUi }, { "spellbook", kLuaUi }, { "auction", kLuaUi },
        { "mail", kLuaUi }, { "calendar", kLuaUi }, { "lfg", kLuaUi }, { "achievement", kLuaUi }, { "guild", kLuaUi },
        { "friend", kLuaUi }, { "raid", kLuaUi }, { "equipment", kLuaUi }, { "reputation", kLuaUi }, { "skill", kLuaUi },
        { "trainer", kLuaUi }, { "quest", kLuaUi }, { "worldstate", kLuaUi }, { "combatlog", kLuaUi },
        { "profanity", kLuaUi }, { "spam", kLuaUi }, { "muted", kLuaUi }, { "throttle", kLuaUi }, { "click", kLuaUi },
        { "keycommand", kLuaUi }, { "inputcontrol", kLuaUi }, { "lcd", kLuaUi }, { "accountdata", kLuaUi },
        { "arenateam", kLuaUi }, { "battlefield", kLuaUi }, { "gmticket", kLuaUi }, { "knowledgebase", kLuaUi },
        { "lootroll", kLuaUi }, { "petition", kLuaUi }, { "encounter", kLuaUi }, { "commentator", kLuaUi },
        { "capturepoint", kLuaUi }, { "taxi", kLuaUi }, { "poi", kLuaUi }, { "declinedword", kLuaUi },
        { "wordlist", kLuaUi }, { "dictionary", kLuaUi }, { "autocomplete", kLuaUi }, { "profile", kLuaUi },
        { "savedvariable", kLuaUi }, { "character", kLuaUi }, { "racecla", kLuaUi }, { "moviecaption", kLuaUi },
        { "osime", kLuaUi }, { "gradient", kLuaUi }, { "simpleanim", kLuaUi }, { "status", kLuaUi },
        { "world.cpp", kMapSub }, { "worldscene", kMapSub }, { "worldparam", kMapSub }, { "weather", kMapSub },
        { "maplight", kMapSub }, { "lightlist", kMapSub }, { "arealight", kMapSub }, { "lightref", kMapSub },
        { "daynight", kMapSub }, { "sky", kMapSub }, { "shippath", kMapSub }, { "camera", kMapSub }, { "mist", kMapSub },
        { "rain", kMapSub }, { "snow", kMapSub }, { "sand", kMapSub },
        { "mopaq", kArchives }, { "sfile", kArchives }, { "iounit", kArchives }, { "ioalignunit", kArchives },
        { "iofileunit", kArchives }, { "filestack", kArchives }, { "memorystorm", kArchives }, { "sarchive", kArchives },
        { "newzerofill", kArchives }, { "prefetch", kArchives }, { "streaming", kArchives },
        { "cgx", kGx }, { "gxu", kGx }, { "gxvertex", kGx }, { "gxdraw", kGx }, { "emergencymem", kGx }, { "shader", kGx },
        { "vertexdecl", kGx }, { "renderstate", kGx }, { "d3d", kGx }, { "opengl", kGx },
    };

    Sub Classify(const std::string& display, uint32_t line)
    {
        std::string s = display;
        for (char& c : s) c = char(tolower(static_cast<unsigned char>(c)));
        if (s == "m2shared.cpp" && line == 0x1F9) return kM2Anim;   // CM2Shared::LoadLowPrioritySequence: .anim files
        if (s == "map.cpp") return kMapSub;
        if (s.find("rec*") != std::string::npos) return kDb;   // an array of DBC record pointers: "ChrClassesRec*[]"
        for (const Rule& r : kRules)
            if (s.find(r.needle) != std::string::npos) return r.sub;
        return kOther;
    }

    // --- names -----------------------------------------------------------------------------------------

    /// "Name@Scope@@" -> "Scope::Name"; a template segment "?$T@<arg>..." becomes "T<arg>" and ends the walk.
    std::string ClassPath(const char*& s)
    {
        std::vector<std::string> parts;
        while (*s && *s != '@')
        {
            if (s[0] == '?' && s[1] == '$')
            {
                s += 2;
                std::string t;
                while (*s && *s != '@') t += *s++;
                if (*s) ++s;
                std::string arg;
                if (*s == 'U' || *s == 'V')
                {
                    ++s;
                    arg = ClassPath(s);
                }
                parts.push_back(t + "<" + arg + ">");
                while (*s) ++s;   // further arguments are not needed
                break;
            }
            std::string p;
            while (*s && *s != '@') p += *s++;
            if (*s) ++s;
            if (!(p.size() > 1 && p[0] == '?' && p[1] == 'A')) parts.push_back(p);   // anonymous namespace
        }
        if (*s == '@') ++s;
        std::string out;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        {
            if (!out.empty()) out += "::";
            out += *it;
        }
        return out;
    }

    /// A readable name: file names as they are, type descriptors (".?AUName@@", ".E") demangled.
    std::string Pretty(const char* raw)
    {
        if (raw[0] != '.') return raw;
        static const struct { const char* code; const char* name; } kBuiltins[] = {
            { "D", "char" }, { "E", "uchar" }, { "F", "short" }, { "G", "ushort" }, { "H", "int" }, { "I", "uint" },
            { "J", "long" }, { "K", "ulong" }, { "M", "float" }, { "N", "double" }, { "X", "void" }, { "_J", "int64" },
            { "_K", "uint64" }, { "_N", "bool" }, { "_W", "wchar" },
        };
        const char* s = raw + 1;
        std::string stars;
        while (s[0] == 'P' && (s[1] == 'A' || s[1] == 'B'))
        {
            stars += '*';
            s += 2;
        }
        if (s[0] == 'P' && s[1] == '6') return "fn*" + stars + "[]";
        if (s[0] == '?' && s[1] == 'A' && (s[2] == 'U' || s[2] == 'V' || (s[2] == 'W' && s[3] == '4')))
        {
            s += s[2] == 'W' ? 4 : 3;   // ?AU struct, ?AV class, ?AW4 enum
            return ClassPath(s) + stars + (stars.empty() ? "" : "[]");
        }
        if (s[0] == 'U' || s[0] == 'V')
        {
            ++s;
            return ClassPath(s) + stars + "[]";
        }
        for (const auto& b : kBuiltins)
            if (std::strcmp(s, b.code) == 0) return std::string(b.name) + stars + "[]";
        return raw;
    }

    /// Everything the report keeps per site and per name, allocated by Install: nothing when the diagnostic is off.
    struct State
    {
        std::vector<std::string> fileNames = std::vector<std::string>(kFiles);   // display name per file index
        uint8_t  siteSub[kSlots] = {};         // cached subsystem per site, for the per-second samples
        bool     siteKnown[kSlots] = {};
        uint32_t fileMax[kFiles] = {};         // highest sampled live bytes per name pointer
        uint64_t subMax[kSubCount] = {};       // highest sampled live bytes per subsystem
        uint32_t sampleBytes[kSlots] = {};     // the last sample, per site
        uint32_t fileBytes[kFiles] = {};       // scratch for Sample
        uint32_t winStart[kSlots] = {};        // the open window's first sample
        uint32_t winPeak[kSlots] = {};         // its sample at the peak
        std::deque<std::string> history;       // closed windows, for the dump
    };
    State* g_s = nullptr;

    const std::string& FileName(uint16_t file, const char* raw)
    {
        std::string& n = g_s->fileNames[file];
        if (n.empty()) n = Pretty(raw);
        return n;
    }

    std::string Hex(uint32_t v)
    {
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%06X", v);
        return buf;
    }

    // --- rows ------------------------------------------------------------------------------------------

    struct Row
    {
        uint32_t        index = 0;
        alloc::SiteView v;
        std::string     name;      // display name of the source
        std::string     owner;     // generic sites: "~File.cpp" when inferred, else the address
        std::string     key;       // attribution: the name, or "name <- owner" for a generic site
        Sub             sub = kOther;
    };

    /// Every live site with its name, owner and subsystem.
    std::vector<Row> Rows()
    {
        std::vector<Row> rows;
        rows.reserve(4096);
        for (uint32_t i = 0; i < kSlots; ++i)
        {
            Row r;
            if (!alloc::ReadSite(i, r.v)) continue;
            r.index = i;
            r.name = FileName(r.v.file, r.v.raw);
            rows.push_back(std::move(r));
        }

        // Named sites in engine code, by return address: the owners of generic sites are placed among them.
        std::vector<std::pair<uint32_t, const Row*>> named;
        for (const Row& r : rows)
            if (!r.v.generic && alloc::InEngineCode(r.v.ret)) named.emplace_back(r.v.ret, &r);
        std::sort(named.begin(), named.end(), [](const auto& a, const auto& b) { return a.first < b.first; });

        for (Row& r : rows)
        {
            if (!r.v.generic)
            {
                r.sub = Classify(r.name, r.v.line);
                r.key = r.name;
            }
            else
            {
                const Row* best = nullptr;
                uint32_t bestDistance = kInferRange;
                const auto consider = [&](const std::pair<uint32_t, const Row*>& cand) {
                    const uint32_t d = cand.first > r.v.owner ? cand.first - r.v.owner : r.v.owner - cand.first;
                    if (d < bestDistance)
                    {
                        bestDistance = d;
                        best = cand.second;
                    }
                };
                if (r.v.owner)
                {
                    const auto it = std::lower_bound(named.begin(), named.end(), r.v.owner,
                                                     [](const auto& a, uint32_t v) { return a.first < v; });
                    if (it != named.end()) consider(*it);
                    if (it != named.begin()) consider(*(it - 1));
                }
                r.owner = best ? "~" + best->name : (r.v.owner ? Hex(r.v.owner) : "?");
                r.sub = best ? Classify(best->name, best->v.line) : kOther;
                r.key = r.name + " <- " + r.owner;
            }
            g_s->siteSub[r.index] = uint8_t(r.sub);
            g_s->siteKnown[r.index] = true;
        }
        return rows;
    }

    // --- sampled peaks ---------------------------------------------------------------------------------

    /// One pass over the site table: per-file and per-subsystem sums, their maxima, and a copy per site.
    void Sample()
    {
        uint32_t* fileBytes = g_s->fileBytes;
        std::fill(fileBytes, fileBytes + kFiles, 0u);
        uint64_t sub[kSubCount] = {};
        for (uint32_t i = 0; i < kSlots; ++i)
        {
            alloc::SiteView v;
            if (!alloc::ReadSite(i, v))
            {
                g_s->sampleBytes[i] = 0;
                continue;
            }
            if (!g_s->siteKnown[i])
            {
                // Named sites are classified at once; generic ones wait for the next report's inference.
                g_s->siteSub[i] = uint8_t(v.generic ? kOther : Classify(FileName(v.file, v.raw), v.line));
                g_s->siteKnown[i] = !v.generic;
            }
            g_s->sampleBytes[i] = v.bytes;
            fileBytes[v.file] += v.bytes;
            sub[g_s->siteSub[i]] += v.bytes;
        }
        for (uint32_t f = 0; f < kFiles; ++f) g_s->fileMax[f] = std::max(g_s->fileMax[f], fileBytes[f]);
        for (uint32_t s = 0; s < kSubCount; ++s) g_s->subMax[s] = std::max(g_s->subMax[s], sub[s]);
    }

    // --- formatting ------------------------------------------------------------------------------------

    /// A growing line: printf-style appends.
    struct Line
    {
        std::string s;

        void Add(const char* fmt, ...)
        {
            char buf[1024];
            va_list args;
            va_start(args, fmt);
            std::vsnprintf(buf, sizeof buf, fmt, args);
            va_end(args);
            s += buf;
        }
    };

    /// Logs "prefix item, item, ..." over as many lines as the logger's 1 KB limit needs.
    void LogList(const std::string& prefix, const std::vector<std::string>& items, const char* sep = ",")
    {
        constexpr size_t kLineMax = 960;
        std::string line = prefix;
        bool first = true;
        for (const std::string& item : items)
        {
            if (!first && line.size() + item.size() + 2 > kLineMax)
            {
                WLOG_INFO("%s", line.c_str());
                line = prefix + " (cont.)";
                first = true;
            }
            line += first ? " " : std::string(sep) + " ";
            line += item;
            first = false;
        }
        WLOG_INFO("%s", line.c_str());
    }

    std::string Fmt(const char* fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, args);
        va_end(args);
        return buf;
    }

    struct FileAgg
    {
        uint64_t bytes = 0, count = 0, peak = 0, allocs = 0;
        Sub      sub = kOther;
        bool     generic = false;
    };

    /// Totals per display name. A name kept at two pointers adds its two peaks.
    std::vector<std::pair<std::string, FileAgg>> ByFile(const std::vector<Row>& rows)
    {
        std::unordered_map<std::string, FileAgg> map;
        std::unordered_map<std::string, std::vector<uint16_t>> indexes;
        for (const Row& r : rows)
        {
            FileAgg& a = map[r.name];
            a.bytes += r.v.bytes;
            a.count += r.v.count;
            a.allocs += r.v.allocs;
            a.generic = r.v.generic;
            if (!r.v.generic) a.sub = r.sub;
            auto& ix = indexes[r.name];
            if (std::find(ix.begin(), ix.end(), r.v.file) == ix.end()) ix.push_back(r.v.file);
        }
        std::vector<std::pair<std::string, FileAgg>> out(map.begin(), map.end());
        for (auto& [name, a] : out)
        {
            for (uint16_t f : indexes[name]) a.peak += g_s->fileMax[f];
            a.peak = std::max(a.peak, a.bytes);
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second.bytes > b.second.bytes; });
        return out;
    }

    /// Generic names, each with its owners by live bytes.
    struct Generic
    {
        std::string name;
        uint64_t    bytes = 0;
        std::vector<std::pair<std::string, uint64_t>> owners;
    };

    std::vector<Generic> ByGeneric(const std::vector<Row>& rows)
    {
        std::map<std::string, std::map<std::string, uint64_t>> map;
        for (const Row& r : rows)
            if (r.v.generic) map[r.name][r.owner] += r.v.bytes;
        std::vector<Generic> out;
        for (auto& [name, owners] : map)
        {
            Generic g;
            g.name = name;
            for (auto& [owner, bytes] : owners)
            {
                g.bytes += bytes;
                g.owners.emplace_back(owner, bytes);
            }
            std::sort(g.owners.begin(), g.owners.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            out.push_back(std::move(g));
        }
        std::sort(out.begin(), out.end(), [](const Generic& a, const Generic& b) { return a.bytes > b.bytes; });
        return out;
    }

    // --- process memory ----------------------------------------------------------------------------------

    struct PrivateSplit
    {
        uint64_t privateCommit = 0, privateReserve = 0;
        uint64_t heapsCommit = 0;            // every heap but the CRT's, as last walked
        uint32_t heapCount = 0;
        double   walkMs = 0.0;
        uint64_t stackCommit = 0, stackReserve = 0;
        uint32_t stacks = 0;
        uint64_t vaCommit = 0, vaReserve = 0;
        uint64_t gpuCommit = 0;              // write-combined mappings the kernel made for a driver
        uint64_t gpuPrivate = 0;             // the part of them typed private (else mapped)
        uint64_t dxvkHeaps = 0, dxvkVa = 0, dxvkVaReserve = 0, dxvkImage = 0;
        uint64_t driverVa = 0;
        std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> vaByModule;   // commit, reserve
        std::vector<process::Heap> heaps;
    };

    PrivateSplit SplitPrivate(bool walkAll, std::vector<process::Allocation>* keep = nullptr, bool names = false)
    {
        PrivateSplit p;
        p.heaps = process::Heaps(walkAll);
        const uintptr_t crtHeap = *reinterpret_cast<const uintptr_t*>(wxl::offsets::engine::crt::kHeap);
        for (const process::Heap& h : p.heaps)
        {
            p.walkMs += h.skipped ? 0.0 : h.walkMs;
            if (h.handle == crtHeap) continue;
            p.heapsCommit += h.committed;
            ++p.heapCount;
            if (h.group == process::Group::Dxvk) p.dxvkHeaps += h.committed;
        }
        std::vector<process::Allocation> all = process::Allocations(p.heaps, names);
        std::map<std::string, std::pair<uint64_t, uint64_t>> va;
        for (const process::Allocation& a : all)
        {
            if (a.type == MEM_IMAGE)
            {
                if (a.group == process::Group::Dxvk) p.dxvkImage += a.reserved;
                continue;
            }
            if (a.gpuMapped && !a.creator) p.gpuCommit += a.committed;
            if (a.type != MEM_PRIVATE) continue;
            p.privateCommit += a.committed;
            p.privateReserve += a.reserved - a.committed;
            if (a.label == "gpu mapping")
                p.gpuPrivate += a.committed;
            else if (a.label == "stack")
            {
                p.stackCommit += a.committed;
                p.stackReserve += a.reserved;
                ++p.stacks;
            }
            else if (a.creator && a.label.rfind("VirtualAlloc ", 0) == 0)
            {
                auto& slot = va[a.label.substr(std::strlen("VirtualAlloc "))];
                slot.first += a.committed;
                slot.second += a.reserved;
                p.vaCommit += a.committed;
                p.vaReserve += a.reserved;
                if (a.group == process::Group::Dxvk)
                {
                    p.dxvkVa += a.committed;
                    p.dxvkVaReserve += a.reserved;
                }
                if (a.group == process::Group::Driver) p.driverVa += a.committed;
            }
        }
        p.vaByModule.assign(va.begin(), va.end());
        std::sort(p.vaByModule.begin(), p.vaByModule.end(),
                  [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
        if (keep) *keep = std::move(all);
        return p;
    }

    // --- peak windows ------------------------------------------------------------------------------------

    enum class Kind { None, Loading, Zone };

    struct Window
    {
        Kind      kind = Kind::None;
        ULONGLONG opened = 0, screenDown = 0, peakAt = 0, lastVaWalk = 0;
        uint32_t  zoneFrom = 0, zoneTo = 0;
        uint64_t  startPrivate = 0, peakPrivate = 0;
        uint64_t  startStorm = 0, peakStorm = 0, startDirect = 0, peakDirect = 0;
        uint64_t  startVa = 0, peakVa = 0, minLargestFree = ~0ull;
    };

    Window   g_window;
    uint32_t g_lastZone = 0;

    void OpenWindow(Kind kind, ULONGLONG now, uint32_t zoneFrom, uint32_t zoneTo)
    {
        const alloc::Totals t = alloc::ReadTotals();
        const memory::VaMap va = memory::WalkAddressSpace();
        g_window = Window{};
        g_window.kind = kind;
        g_window.opened = now;
        g_window.lastVaWalk = now;
        g_window.zoneFrom = zoneFrom;
        g_window.zoneTo = zoneTo;
        g_window.startPrivate = g_window.peakPrivate = memory::ReadCounters().privateUsage;
        g_window.startStorm = g_window.peakStorm = t.stormBytes;
        g_window.startDirect = g_window.peakDirect = t.directBytes;
        g_window.startVa = g_window.peakVa = va.Used();
        g_window.minLargestFree = va.largestFree;
        g_window.peakAt = now;
        Sample();
        std::memcpy(g_s->winStart, g_s->sampleBytes, sizeof g_s->winStart);
        std::memcpy(g_s->winPeak, g_s->sampleBytes, sizeof g_s->winPeak);
        alloc::ResetWindowPeak();
    }

    void SampleWindow(ULONGLONG now)
    {
        Sample();
        const uint64_t priv = memory::ReadCounters().privateUsage;
        if (priv > g_window.peakPrivate)
        {
            // The top sources at the moment of the peak: a copy of every site.
            g_window.peakPrivate = priv;
            g_window.peakAt = now;
            std::memcpy(g_s->winPeak, g_s->sampleBytes, sizeof g_s->winPeak);
            const alloc::Totals t = alloc::ReadTotals();
            g_window.peakStorm = t.stormBytes;
            g_window.peakDirect = t.directBytes;
        }
        if (now - g_window.lastVaWalk >= kWindowVaGapMs)
        {
            g_window.lastVaWalk = now;
            const memory::VaMap va = memory::WalkAddressSpace();
            g_window.peakVa = std::max(g_window.peakVa, va.Used());
            g_window.minLargestFree = std::min(g_window.minLargestFree, va.largestFree);
        }
    }

    void CloseWindow(ULONGLONG now)
    {
        const Window w = g_window;
        g_window.kind = Kind::None;
        const char* tag = w.kind == Kind::Loading ? "load-peak" : "zone-peak";
        const std::vector<Row> rows = Rows();
        const uint64_t nowPrivate = memory::ReadCounters().privateUsage;
        const uint64_t exactStormPeak = alloc::WindowPeak();

        Line head;
        if (w.kind == Kind::Loading)
            head.Add("memory[%s]: loading screen, %.1f s", tag, (now - w.opened) / 1000.0);
        else
            head.Add("memory[%s]: zone %u -> %u, %.1f s", tag, w.zoneFrom, w.zoneTo, (now - w.opened) / 1000.0);
        head.Add(" | private %.1f -> peak %.1f MB (%+.1f) at +%.1f s, now %.1f | address space peak %.1f MB (%+.1f), "
                 "smallest largest-free %.1f MB | Storm %+.1f MB and CRT direct %+.1f at the private peak, Storm's own "
                 "peak %.1f MB",
                 w.startPrivate / kMb, w.peakPrivate / kMb, (double(w.peakPrivate) - double(w.startPrivate)) / kMb,
                 (w.peakAt - w.opened) / 1000.0, nowPrivate / kMb, w.peakVa / kMb,
                 (double(w.peakVa) - double(w.startVa)) / kMb, w.minLargestFree / kMb,
                 (double(w.peakStorm) - double(w.startStorm)) / kMb, (double(w.peakDirect) - double(w.startDirect)) / kMb,
                 exactStormPeak / kMb);

        // Growth from the window's start to its peak, by subsystem and by attributed source.
        double sub[kSubCount] = {};
        std::unordered_map<std::string, double> byKey;
        for (const Row& r : rows)
        {
            const double d = double(g_s->winPeak[r.index]) - double(g_s->winStart[r.index]);
            if (d == 0.0) continue;
            sub[r.sub] += d;
            byKey[r.key] += d;
        }
        std::vector<int> order(kSubCount);
        for (int i = 0; i < kSubCount; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return sub[a] > sub[b]; });
        std::vector<std::string> subs;
        for (int i : order)
            if (std::abs(sub[i]) >= 0.05 * kMb) subs.push_back(Fmt("%s %+.1f", kSubNames[i], sub[i] / kMb));

        std::vector<std::pair<std::string, double>> top(byKey.begin(), byKey.end());
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::vector<std::string> srcs;
        for (size_t i = 0; i < top.size() && i < 15 && top[i].second > 0; ++i)
            srcs.push_back(Fmt("%s %+.1f", top[i].first.c_str(), top[i].second / kMb));

        const std::string subsHead = Fmt("memory[%s]: growth to the peak by subsystem (MB):", tag);
        const std::string srcsHead = Fmt("memory[%s]: top growth (MB):", tag);
        WLOG_INFO("%s", head.s.c_str());
        LogList(subsHead, subs);
        LogList(srcsHead, srcs);

        const auto join = [](const std::string& prefix, const std::vector<std::string>& list) {
            std::string out = prefix;
            for (size_t i = 0; i < list.size(); ++i) out += (i ? ", " : " ") + list[i];
            return out;
        };
        char stamp[32];
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::snprintf(stamp, sizeof stamp, "[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
        g_s->history.push_back(stamp + head.s + "\n" + stamp + join(subsHead, subs) + "\n" + stamp + join(srcsHead, srcs));
        while (g_s->history.size() > kHistory) g_s->history.pop_front();
    }

    // --- panel summary ---------------------------------------------------------------------------------

    std::mutex  g_summaryMutex;
    std::string g_summary = "memory map: waiting for the first sample";

    void UpdateSummary()
    {
        const alloc::Totals t = alloc::ReadTotals();
        const memory::Counters c = memory::ReadCounters();
        Line l;
        l.Add("Storm %.1f MB in %llu blocks (peak %.1f)\nCRT direct %.1f MB (peak %.1f)\nprivate usage %.1f MB",
              t.stormBytes / kMb, static_cast<unsigned long long>(t.stormBlocks), t.stormPeak / kMb,
              t.directBytes / kMb, t.directPeak / kMb, c.privateUsage / kMb);
        if (g_window.kind != Kind::None)
            l.Add("\npeak window open: %s", g_window.kind == Kind::Loading ? "loading screen" : "zone change");
        std::lock_guard<std::mutex> lock(g_summaryMutex);
        g_summary = std::move(l.s);
    }

    /// Wow.exe's folder + Logs\<file>, the folder created if missing.
    std::wstring LogsPath(const wchar_t* file)
    {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) slash[1] = 0;
        const std::wstring logs = std::wstring(exe) + L"Logs";
        CreateDirectoryW(logs.c_str(), nullptr);
        return logs + L"\\" + file;
    }
}

namespace wxl::diag::memory::map
{
    bool Install()
    {
        g_s = new State{};
        std::fill(std::begin(g_s->siteSub), std::end(g_s->siteSub), uint8_t(kOther));
        const bool a = alloc::Install();
        const bool p = process::Install();
        return a && p;
    }

    void LogSections(const char* reason)
    {
        if (!alloc::Active() || !g_s) return;
        const alloc::Totals t = alloc::ReadTotals();
        const std::vector<Row> rows = Rows();

        // Allocation rate since the last report.
        static uint64_t lastAllocs = 0;
        static ULONGLONG lastMs = 0;
        const ULONGLONG now = GetTickCount64();
        const uint64_t allocs = t.stormAllocs + t.directAllocs;
        const double rate = lastMs && now > lastMs ? double(allocs - lastAllocs) * 1000.0 / double(now - lastMs) : 0.0;
        lastAllocs = allocs;
        lastMs = now;

        Line heap;
        heap.Add("memory[%s]: engine heap: Storm %.1f MB in %llu blocks (peak %.1f), tags +%.1f MB", reason,
                 t.stormBytes / kMb, static_cast<unsigned long long>(t.stormBlocks), t.stormPeak / kMb, t.tagBytes / kMb);
        if (t.late)
            heap.Add(" | CRT direct not counted (late start), %llu untracked frees",
                     static_cast<unsigned long long>(t.untrackedFrees));
        else
            heap.Add(" | CRT direct %.1f MB in %llu (peak %.1f)", t.directBytes / kMb,
                     static_cast<unsigned long long>(t.directBlocks), t.directPeak / kMb);
        heap.Add(" | %.0f allocs/s | %u sites, %u names%s%s", rate, t.sites, t.files,
                 t.overflow ? ", site table full" : "", t.heapMode == 1 ? "" : ", CRT not on the system heap");
        if (t.mixedFrees) heap.Add(" | %llu CRT blocks freed by Storm", static_cast<unsigned long long>(t.mixedFrees));
        WLOG_INFO("%s", heap.s.c_str());

        // Subsystems, live/peak.
        uint64_t sub[kSubCount] = {};
        for (const Row& r : rows) sub[r.sub] += r.v.bytes;
        std::vector<int> order(kSubCount);
        for (int i = 0; i < kSubCount; ++i) order[i] = i;
        std::sort(order.begin(), order.end(), [&](int a, int b) { return sub[a] > sub[b]; });
        std::vector<std::string> items;
        for (int i : order)
            if (sub[i] || g_s->subMax[i])
                items.push_back(Fmt("%s %.1f/%.1f", kSubNames[i], sub[i] / kMb, std::max<uint64_t>(sub[i], g_s->subMax[i]) / kMb));
        LogList(Fmt("memory[%s]: subsystems (MB live/peak):", reason), items);

        // Top 40 sources, as the allocator was told.
        const auto files = ByFile(rows);
        items.clear();
        for (size_t i = 0; i < files.size() && i < 40; ++i)
            items.push_back(Fmt("%s %.1f/%.1f", files[i].first.c_str(), files[i].second.bytes / kMb, files[i].second.peak / kMb));
        LogList(Fmt("memory[%s]: top 40 sources (MB live/peak):", reason), items);

        // Names that do not say who allocates: their owners.
        const auto generics = ByGeneric(rows);
        items.clear();
        for (size_t g = 0; g < generics.size() && g < 5; ++g)
        {
            std::string item = Fmt("%s %.1f (", generics[g].name.c_str(), generics[g].bytes / kMb);
            for (size_t o = 0; o < generics[g].owners.size() && o < 3; ++o)
                item += Fmt("%s%s %.1f", o ? ", " : "", generics[g].owners[o].first.c_str(), generics[g].owners[o].second / kMb);
            items.push_back(item + ")");
        }
        if (!items.empty()) LogList(Fmt("memory[%s]: by owner:", reason), items, " |");

        // Private memory by owner.
        const PrivateSplit p = SplitPrivate(false);
        const uint64_t crtLive = t.stormBytes + t.tagBytes + t.directBytes;
        const int64_t rest = int64_t(p.privateCommit) - int64_t(crtLive) - int64_t(p.heapsCommit) - int64_t(p.vaCommit) -
                             int64_t(p.stackCommit) - int64_t(p.gpuPrivate);
        WLOG_INFO("memory[%s]: private %.1f MB (+%.1f reserved) | CRT heap live %.1f | %u other heaps %.1f (walk %.1f ms) | "
                  "VirtualAlloc %.1f/%.1f | stacks %.1f/%.1f in %u | gpu mappings %.1f | rest %.1f (CRT heap overhead, "
                  "unhooked)",
                  reason, p.privateCommit / kMb, p.privateReserve / kMb, crtLive / kMb, p.heapCount, p.heapsCommit / kMb,
                  p.walkMs, p.vaCommit / kMb, p.vaReserve / kMb, p.stackCommit / kMb, p.stackReserve / kMb, p.stacks,
                  p.gpuCommit / kMb, rest / kMb);
        items.clear();
        std::vector<process::Heap> heaps = p.heaps;
        std::sort(heaps.begin(), heaps.end(), [](const auto& a, const auto& b) { return a.committed > b.committed; });
        const uintptr_t crtHeap = *reinterpret_cast<const uintptr_t*>(wxl::offsets::engine::crt::kHeap);
        for (size_t i = 0; i < heaps.size() && items.size() < 6; ++i)
            if (heaps[i].handle != crtHeap && heaps[i].committed >= (1u << 20))
                items.push_back(Fmt("%s %.1f/%.1f%s", heaps[i].owner.c_str(), heaps[i].allocated / kMb,
                                    heaps[i].committed / kMb, heaps[i].skipped ? " (last walk)" : ""));
        if (!items.empty()) LogList(Fmt("memory[%s]: other heaps by owner (MB allocated/committed):", reason), items);
        items.clear();
        for (size_t i = 0; i < p.vaByModule.size() && i < 10; ++i)
            items.push_back(Fmt("%s %.1f/%.1f", p.vaByModule[i].first.c_str(), p.vaByModule[i].second.first / kMb,
                                p.vaByModule[i].second.second / kMb));
        if (!items.empty()) LogList(Fmt("memory[%s]: VirtualAlloc by caller (MB committed/reserved):", reason), items);

        // Modules.
        const std::vector<process::Module> mods = process::Modules();
        uint64_t groupBytes[size_t(process::Group::Count)] = {};
        uint32_t groupCount[size_t(process::Group::Count)] = {};
        uint64_t imageTotal = 0;
        for (const process::Module& m : mods)
        {
            groupBytes[size_t(m.group)] += m.size;
            ++groupCount[size_t(m.group)];
            imageTotal += m.size;
        }
        std::string groups;
        for (size_t g = 0; g < size_t(process::Group::Count); ++g)
            if (groupCount[g])
                groups += Fmt("%s%s %.1f in %u", groups.empty() ? "" : ", ", process::GroupName(process::Group(g)),
                              groupBytes[g] / kMb, groupCount[g]);
        items.clear();
        for (size_t i = 0; i < mods.size() && i < 10; ++i)
            items.push_back(Fmt("%s %.1f", mods[i].name.c_str(), mods[i].size / kMb));
        LogList(Fmt("memory[%s]: modules: %zu images %.1f MB | %s | biggest:", reason, mods.size(), imageTotal / kMb,
                    groups.c_str()),
                items);

        // DXVK runs inside Wow.exe: its image, heaps and reservations, and the driver's under it.
        if (groupCount[size_t(process::Group::Dxvk)])
            WLOG_INFO("memory[%s]: dxvk: image %.1f MB | heaps %.1f | VirtualAlloc %.1f/%.1f | drivers' VirtualAlloc %.1f | "
                      "gpu mappings %.1f (write-combined, all drivers)",
                      reason, p.dxvkImage / kMb, p.dxvkHeaps / kMb, p.dxvkVa / kMb, p.dxvkVaReserve / kMb,
                      p.driverVa / kMb, p.gpuCommit / kMb);
    }

    void Tick(unsigned long long nowMs)
    {
        static uint32_t tick = 0;
        ++tick;
        if (!alloc::Active() || !g_s) return;

        const bool loading = wxl::game::world::LoadingScreenVisible();
        const uint32_t zone = *reinterpret_cast<const uint32_t*>(wxl::offsets::game::world::kZoneId);

        if (loading)
        {
            if (g_window.kind == Kind::Zone) CloseWindow(nowMs);
            if (g_window.kind == Kind::None) OpenWindow(Kind::Loading, nowMs, 0, 0);
            g_window.screenDown = 0;
        }
        else if (g_window.kind == Kind::Loading)
        {
            if (!g_window.screenDown) g_window.screenDown = nowMs;
            else if (nowMs - g_window.screenDown >= kLoadSettleMs) CloseWindow(nowMs);
        }

        if (zone != g_lastZone)
        {
            if (g_lastZone && zone && g_window.kind == Kind::None) OpenWindow(Kind::Zone, nowMs, g_lastZone, zone);
            g_lastZone = zone;
        }
        if (g_window.kind == Kind::Zone && nowMs - g_window.opened >= kZoneWindowMs) CloseWindow(nowMs);

        if (g_window.kind != Kind::None)
            SampleWindow(nowMs);
        else if (tick % 4 == 0)
            Sample();
        if (tick % 4 == 0) UpdateSummary();
    }

    bool WriteDump(const char* reason)
    {
        const ULONGLONG started = GetTickCount64();
        const std::wstring path = LogsPath(L"memory-map.txt");
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        {
            WLOG_WARN("memory: cannot write Logs\\memory-map.txt");
            return false;
        }

        SYSTEMTIME st;
        GetLocalTime(&st);
        const memory::VaMap va = memory::WalkAddressSpace();
        const memory::Counters c = memory::ReadCounters();
        const alloc::Totals t = alloc::ReadTotals();
        std::fprintf(f, "WarcraftXL memory map, %04u-%02u-%02u %02u:%02u:%02u (%s)\n\n", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond, reason);

        std::fprintf(f, "== address space\n");
        std::fprintf(f, "used %.1f MB, free %.1f MB, largest free %.1f MB, %u free blocks >= 64 MB\n", va.Used() / kMb,
                     va.freeBytes / kMb, va.largestFree / kMb, va.freeBlocks64);
        std::fprintf(f, "image %.1f MB committed (+%.1f reserved) | mapped %.1f (+%.1f) | private %.1f (+%.1f)\n",
                     va.imageCommit / kMb, va.imageReserve / kMb, va.mappedCommit / kMb, va.mappedReserve / kMb,
                     va.privateCommit / kMb, va.privateReserve / kMb);
        std::fprintf(f, "private usage %.1f MB, working set %.1f MB (peak %.1f)\n\n", c.privateUsage / kMb,
                     c.workingSet / kMb, c.peakWorkingSet / kMb);

        if (alloc::Active() && g_s)
        {
            const std::vector<Row> rows = Rows();
            std::fprintf(f, "== engine heap (SMemAlloc and the CRT under it)\n");
            std::fprintf(f, "Storm %.1f MB in %llu blocks, peak %.1f MB, tags +%.1f MB, %llu allocations ever\n",
                         t.stormBytes / kMb, static_cast<unsigned long long>(t.stormBlocks), t.stormPeak / kMb,
                         t.tagBytes / kMb, static_cast<unsigned long long>(t.stormAllocs));
            if (t.late)
                std::fprintf(f, "CRT direct: not counted, tracking started after the client's CRT (%llu untracked frees)\n",
                             static_cast<unsigned long long>(t.untrackedFrees));
            else
                std::fprintf(f, "CRT direct %.1f MB in %llu blocks, peak %.1f MB, %llu allocations ever, %llu freed by Storm\n",
                             t.directBytes / kMb, static_cast<unsigned long long>(t.directBlocks), t.directPeak / kMb,
                             static_cast<unsigned long long>(t.directAllocs), static_cast<unsigned long long>(t.mixedFrees));
            std::fprintf(f, "%u sites, %u names%s; CRT heap mode %d\n\n", t.sites, t.files,
                         t.overflow ? ", site table full" : "", t.heapMode);

            uint64_t sub[kSubCount] = {}, subCount[kSubCount] = {};
            for (const Row& r : rows)
            {
                sub[r.sub] += r.v.bytes;
                subCount[r.sub] += r.v.count;
            }
            std::fprintf(f, "== subsystems (peak: highest of the per-second samples)\n");
            std::fprintf(f, "%-12s %10s %10s %10s\n", "subsystem", "live MB", "blocks", "peak MB");
            std::vector<int> order(kSubCount);
            for (int i = 0; i < kSubCount; ++i) order[i] = i;
            std::sort(order.begin(), order.end(), [&](int a, int b) { return sub[a] > sub[b]; });
            for (int i : order)
                if (sub[i] || subCount[i] || g_s->subMax[i])
                    std::fprintf(f, "%-12s %10.2f %10llu %10.2f\n", kSubNames[i], sub[i] / kMb,
                                 static_cast<unsigned long long>(subCount[i]), std::max<uint64_t>(sub[i], g_s->subMax[i]) / kMb);

            std::fprintf(f, "\n== sources, as named to the allocator\n");
            std::fprintf(f, "%-48s %10s %10s %10s %12s  %s\n", "source", "live MB", "blocks", "peak MB", "allocs", "subsystem");
            for (const auto& [name, a] : ByFile(rows))
                std::fprintf(f, "%-48s %10.2f %10llu %10.2f %12llu  %s\n", name.c_str(), a.bytes / kMb,
                             static_cast<unsigned long long>(a.count), a.peak / kMb,
                             static_cast<unsigned long long>(a.allocs), a.generic ? "(by owner)" : kSubNames[a.sub]);

            std::fprintf(f, "\n== names that do not say who allocates, by owner (~ = nearest named site in Wow.exe's code)\n");
            for (const Generic& g : ByGeneric(rows))
            {
                std::fprintf(f, "%s  %.2f MB\n", g.name.c_str(), g.bytes / kMb);
                for (const auto& [owner, bytes] : g.owners)
                    if (bytes) std::fprintf(f, "    %-44s %10.2f MB\n", owner.c_str(), bytes / kMb);
            }

            std::vector<const Row*> sorted;
            for (const Row& r : rows) sorted.push_back(&r);
            std::sort(sorted.begin(), sorted.end(), [](const Row* a, const Row* b) {
                return a->v.bytes != b->v.bytes ? a->v.bytes > b->v.bytes : a->v.peak > b->v.peak;
            });
            std::fprintf(f, "\n== call sites (source:line, owner for generic names; ret = the allocator's return address)\n");
            std::fprintf(f, "%-56s %10s %9s %10s %10s  %-10s %s\n", "site", "live MB", "blocks", "peak MB", "allocs", "ret",
                         "subsystem");
            for (const Row* r : sorted)
            {
                char site[160];
                std::snprintf(site, sizeof site, "%s:%d%s%s", r->name.c_str(), int(r->v.line), r->v.generic ? " <- " : "",
                              r->v.generic ? (r->owner + (r->v.owner ? " " + Hex(r->v.owner) : "")).c_str() : "");
                std::fprintf(f, "%-56s %10.3f %9u %10.3f %10u  %-10s %s\n", site, r->v.bytes / kMb, r->v.count,
                             r->v.peak / kMb, r->v.allocs, Hex(r->v.ret).c_str(), kSubNames[r->sub]);
            }
            std::fprintf(f, "\n");
        }

        std::vector<process::Allocation> all;
        const PrivateSplit p = SplitPrivate(true, &all, true);
        const uintptr_t crtHeap = *reinterpret_cast<const uintptr_t*>(wxl::offsets::engine::crt::kHeap);
        std::fprintf(f, "== heaps (HeapSummary walks each one under its lock)\n");
        std::fprintf(f, "%-10s %-28s %12s %12s %12s %9s\n", "handle", "owner", "allocated MB", "committed MB", "reserved MB",
                     "walk ms");
        for (const process::Heap& h : p.heaps)
        {
            std::fprintf(f, "%08X   %-28s %12.2f %12.2f %12.2f %9.2f%s\n", unsigned(h.handle), h.owner.c_str(),
                         h.allocated / kMb, h.committed / kMb, h.reserved / kMb, h.walkMs,
                         h.skipped ? (h.walked ? "  (last walk)" : "  (not walked: unserialized)") : "");
            if (h.handle == crtHeap && h.walked && !t.late)
                std::fprintf(f, "           the tracker counts %.2f MB on it (Storm %.2f + tags %.2f + CRT direct %.2f); "
                                "committed beyond allocated: %.2f MB\n",
                             (t.stormBytes + t.tagBytes + t.directBytes) / kMb, t.stormBytes / kMb, t.tagBytes / kMb,
                             t.directBytes / kMb, (double(h.committed) - double(h.allocated)) / kMb);
        }

        std::fprintf(f, "\n== VirtualAlloc reservations by caller module (committed / reserved MB)\n");
        for (const auto& [module, sizes] : p.vaByModule)
            std::fprintf(f, "%-40s %10.2f %10.2f\n", module.c_str(), sizes.first / kMb, sizes.second / kMb);
        std::fprintf(f, "stacks %.2f / %.2f MB in %u threads; gpu mappings %.2f MB\n", p.stackCommit / kMb,
                     p.stackReserve / kMb, p.stacks, p.gpuCommit / kMb);

        std::map<std::string, std::pair<uint64_t, uint64_t>> byLabel;
        for (const process::Allocation& a : all)
        {
            auto& slot = byLabel[(a.type == MEM_IMAGE ? "image " : a.type == MEM_MAPPED ? "mapped " : "private ") + a.label];
            slot.first += a.committed;
            slot.second += a.reserved;
        }
        std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> labels(byLabel.begin(), byLabel.end());
        std::sort(labels.begin(), labels.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
        std::fprintf(f, "\n== address space by owner (committed / reserved MB)\n");
        for (const auto& [label, sizes] : labels)
            std::fprintf(f, "%-56s %10.2f %10.2f\n", label.c_str(), sizes.first / kMb, sizes.second / kMb);

        std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.reserved > b.reserved; });
        std::fprintf(f, "\n== largest reservations\n");
        for (size_t i = 0; i < all.size() && i < 150; ++i)
            std::fprintf(f, "%08X %10.2f MB reserved %10.2f committed  %-8s %s%s\n", unsigned(all[i].base),
                         all[i].reserved / kMb, all[i].committed / kMb,
                         all[i].type == MEM_IMAGE ? "image" : all[i].type == MEM_MAPPED ? "mapped" : "private",
                         all[i].label.c_str(), all[i].creator ? (" (caller " + Hex(all[i].creator) + ")").c_str() : "");

        const std::vector<process::Module> mods = process::Modules();
        std::fprintf(f, "\n== modules (%zu)\n", mods.size());
        for (const process::Module& m : mods)
            std::fprintf(f, "%08X %8.2f MB  %-8s %-28s %s\n", unsigned(m.base), m.size / kMb, process::GroupName(m.group),
                         m.name.c_str(), m.path.c_str());

        std::fprintf(f, "\n== peak windows (loading screens and zone changes, last %zu)\n", kHistory);
        for (const std::string& h : g_s->history) std::fprintf(f, "%s\n", h.c_str());
        if (g_window.kind != Kind::None) std::fprintf(f, "(a window is open now)\n");

        std::fclose(f);
        WLOG_INFO("memory: map written to Logs\\memory-map.txt in %llu ms",
                  static_cast<unsigned long long>(GetTickCount64() - started));
        return true;
    }

    void Summary(char* out, size_t cap)
    {
        std::lock_guard<std::mutex> lock(g_summaryMutex);
        strncpy_s(out, cap, g_summary.c_str(), _TRUNCATE);
    }
}
