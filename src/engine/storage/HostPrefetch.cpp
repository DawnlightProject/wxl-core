// Tells wxl-host where terrain streams around and where it is headed, so it can read ahead of the client.
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

// Four times a second, one small fire-and-forget message: the streaming focus (the point CMap loads tiles
// around, offsets/game/World.hpp), its velocity over the last interval, the view distance and the map
// folder. The host turns it into the tiles ahead (host/Prefetch.cpp). WXL_HOST_PREFETCH=0 sends nothing.

#include "common/Config.hpp"
#include "engine/events/EventScript.hpp"
#include "ipc/Protocol.hpp"
#include "offsets/game/World.hpp"
#include "runtime/host/HostClient.hpp"

#include <windows.h>

#include <cmath>
#include <cstring>

namespace
{
    namespace woff = wxl::offsets::game::world;
    namespace h = wxl::host;

    constexpr ULONGLONG kPeriodMs = 250;
    constexpr float kMaxSpeed = 120.0f;   // yards per second; a larger jump is a teleport, not travel

    bool Enabled()
    {
        static const bool on = wxl::config::Env("WXL_HOST_PREFETCH", true);
        return on;
    }

    float ReadFloat(uintptr_t address)
    {
        return *reinterpret_cast<const float*>(address);
    }

    uint64_t Pack(float lo, float hi)
    {
        uint32_t a, b;
        std::memcpy(&a, &lo, 4);
        std::memcpy(&b, &hi, 4);
        return uint64_t(a) | (uint64_t(b) << 32);
    }

    class HostPrefetchHints final : public wxl::events::EventScript
    {
    public:
        HostPrefetchHints() { on<&HostPrefetchHints::OnUpdate>(wxl::events::Event::OnUpdate); }

        void OnUpdate(const wxl::events::UpdateArgs&)
        {
            if (!Enabled()) return;
            const ULONGLONG now = GetTickCount64();
            if (now - m_sent < kPeriodMs) return;
            if (!h::CacheEnabled() || *reinterpret_cast<const int32_t*>(woff::kCurrentMapId) < 0) return;

            char folder[wxl::ipc::kNameMax];
            const char* live = reinterpret_cast<const char*>(woff::kMapDirStr);
            size_t n = 0;
            while (n + 1 < sizeof folder && live[n]) { folder[n] = live[n]; ++n; }
            folder[n] = '\0';
            if (!n) return;

            const float x = ReadFloat(woff::kFocusPosX), y = ReadFloat(woff::kFocusPosY);
            if (!std::isfinite(x) || !std::isfinite(y)) return;
            float vx = 0.0f, vy = 0.0f;
            const bool sameMap = m_sent && std::strcmp(folder, m_folder) == 0;
            if (sameMap)
            {
                const float dt = float(now - m_sent) / 1000.0f;
                vx = (x - m_x) / dt;
                vy = (y - m_y) / dt;
                if (std::sqrt(vx * vx + vy * vy) > kMaxSpeed) vx = vy = 0.0f;
            }
            m_sent = now;
            m_x = x;
            m_y = y;
            std::memcpy(m_folder, folder, n + 1);

            h::Params p;
            p.op = wxl::ipc::Op::Hint;
            p.name = folder;
            p.a0 = Pack(x, y);
            p.a1 = Pack(vx, vy);
            p.a2 = Pack(ReadFloat(woff::kFarClip), 0.0f);
            p.a3 = uint64_t(uint32_t(*reinterpret_cast<const int32_t*>(woff::kCurrentMapId)));
            h::Post(p, [](void*, const h::Reply&) {}, nullptr);
        }

    private:
        ULONGLONG m_sent = 0;
        float     m_x = 0.0f, m_y = 0.0f;
        char      m_folder[wxl::ipc::kNameMax] = {};
    };

    HostPrefetchHints g_hostPrefetchHints;
}
