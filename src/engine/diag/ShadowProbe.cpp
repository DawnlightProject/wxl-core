// Shadow probe: logs what wxl::game::shadows reads, behind WXL_DIAG_SHADOWS (off by default).
// Copyright (C) 2026 WarcraftXL
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "common/Config.hpp"
#include "common/Log.hpp"
#include "engine/events/Event.hpp"
#include "engine/hook/Registry.hpp"
#include "game/Shadows.hpp"

#include <windows.h>

namespace
{
    namespace ev = wxl::events;
    namespace sh = wxl::game::shadows;

    struct Seen
    {
        int      mode = -1;
        bool     valid = false;
        bool     hwPcf = false;
        uint32_t size = 0;
        uint32_t generation = 0;
        uint32_t matricesLogged = 0xFFFFFFFF;  // generation whose matrices were written
    };

    Seen               g_seen;
    bool               g_chained = false;
    uint32_t           g_renderCalls = 0;
    uint32_t           g_frames = 0, g_validFrames = 0;
    unsigned long long g_nextBeat = 0;

    void __cdecl OnRenderAfter(const sh::CallbackArgs&, void*) { ++g_renderCalls; }

    const char* SlotName(int i)
    {
        static const char* const kNames[] = { "main", "interior", "band0", "band1", "band2" };
        return kNames[i];
    }

    void LogMatrices(const sh::Snapshot& s)
    {
        WLOG_INFO("shadows: light dir (%.3f, %.3f, %.3f), view-space (%.3f, %.3f, %.3f), camera (%.1f, %.1f, %.1f),"
                  " view translation (%.3f, %.3f, %.3f)",
                  s.lightDir[0], s.lightDir[1], s.lightDir[2], s.lightDirView[0], s.lightDirView[1], s.lightDirView[2],
                  s.cameraPos[0], s.cameraPos[1], s.cameraPos[2], s.view[12], s.view[13], s.view[14]);
        for (int i = 0; i < int(sh::Slot::Count); ++i)
        {
            const sh::Map& m = s.maps[i];
            if (!m.present) { WLOG_INFO("shadows:   %-8s absent", SlotName(i)); continue; }
            WLOG_INFO("shadows:   %-8s tex %p extent %.1f bias %.2f centre (%.1f, %.1f, %.1f)", SlotName(i), m.texture,
                      m.halfExtent, m.depthBias, m.centre[0], m.centre[1], m.centre[2]);
            for (int r = 0; r < 3; ++r)
                WLOG_INFO("shadows:     row%d (%.5f, %.5f, %.5f, %.5f)", r, m.rows[r][0], m.rows[r][1], m.rows[r][2], m.rows[r][3]);
        }
    }

    void __cdecl OnWorldSceneEnd(void*, const void*)
    {
        if (!g_chained) g_chained = sh::ChainAfter(sh::Callback::Render, &OnRenderAfter, nullptr);

        sh::Snapshot s;
        sh::Get(s);
        ++g_frames;
        if (s.valid) ++g_validFrames;

        if (s.mode != g_seen.mode || s.valid != g_seen.valid || s.hwPcf != g_seen.hwPcf || s.size != g_seen.size ||
            s.generation != g_seen.generation)
        {
            WLOG_INFO("shadows: mode %d tier %d valid %d hwPCF %d size %u generation %u", s.mode, s.tier, s.valid,
                      s.hwPcf, s.size, s.generation);
            g_seen.mode = s.mode; g_seen.valid = s.valid; g_seen.hwPcf = s.hwPcf;
            g_seen.size = s.size; g_seen.generation = s.generation;
        }
        if (s.valid && g_seen.matricesLogged != s.generation)
        {
            g_seen.matricesLogged = s.generation;
            LogMatrices(s);
        }

        const unsigned long long now = GetTickCount64();
        if (now >= g_nextBeat)
        {
            if (g_nextBeat)
                WLOG_INFO("shadows: last 10 s: %u frames, %u valid, %u render callbacks (chained %d)", g_frames,
                          g_validFrames, g_renderCalls, g_chained);
            g_nextBeat = now + 10000;
            g_frames = g_validFrames = g_renderCalls = 0;
        }
    }

    bool InstallShadowProbe()
    {
        if (!wxl::config::Env("WXL_DIAG_SHADOWS", false)) return true;
        ev::Subscribe(ev::Event::OnWorldSceneEnd, &OnWorldSceneEnd, nullptr);
        WLOG_INFO("shadows: probe on (WXL_DIAG_SHADOWS)");
        return true;
    }
}

WXL_REGISTER_FEATURE("shadow-probe", true, InstallShadowProbe)
