// wxl-graphics-extend: the overlay panel -- device facts, what the passes asked for and got, per-pass
// GPU time, render targets, shader cache.
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

#include "Panel.hpp"
#include "../assets/Assets.hpp"
#include "../core/Extension.hpp"
#include "../frame/Scheduler.hpp"
#include "../shaders/Compiler.hpp"
#include "../targets/TargetPool.hpp"
#include "../textures/Neutral.hpp"
#include "../vulkan/Vulkan.hpp"

#include <cstdarg>
#include <cstddef>
#include <cstdio>

// Read-only, apart from one button: the panel reports, the .cfg configures. Only the base Ui*
// functions of WXL_Api are used, so it draws on every core that has an overlay at all.
namespace
{
    namespace frame    = wxl::gfx::frame;
    namespace shaders  = wxl::gfx::shaders;
    namespace targets  = wxl::gfx::targets;
    namespace textures = wxl::gfx::textures;

    const WXL_Api* Api() { return wxl::gfx::g_api; }

    void Text(const char* text) { Api()->UiText(text); }

    /// A formatted line; the formatting happens here, no varargs cross the ABI.
    void Textf(const char* fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, args);
        va_end(args);
        Api()->UiText(buf);
    }

    void Heading(const char* text)
    {
        Api()->UiSeparator();
        Text(text);
    }

    const char* YesNo(bool value) { return value ? "yes" : "no"; }

    /// WXL_GFX_NEED_* as words: "depth normals hdr run", or "none".
    const char* Needs(uint32_t bits, char* buf, size_t cap)
    {
        static const struct { uint32_t bit; const char* word; } kWords[] = {
            { WXL_GFX_NEED_DEPTH, "depth" }, { WXL_GFX_NEED_NORMALS, "normals" },
            { WXL_GFX_NEED_ALBEDO, "albedo" }, { WXL_GFX_NEED_HDR, "hdr" }, { WXL_GFX_NEED_RUN, "run" },
        };
        size_t n = 0;
        buf[0] = '\0';
        for (const auto& w : kWords)
        {
            if (!(bits & w.bit) || n >= cap) continue;
            const int wrote = std::snprintf(buf + n, cap - n, "%s%s", n ? " " : "", w.word);
            if (wrote > 0) n += size_t(wrote);
        }
        if (!buf[0]) std::snprintf(buf, cap, "none");
        return buf;
    }

    void __cdecl Draw(void*)
    {
        WXL_GfxStatus s{};
        s.structSize = sizeof s;
        frame::FillStatus(&s);

        if (!(s.caps & WXL_GFX_CAP_PROBED))
            Text("Device: not probed yet (enter the world)");
        else
            Textf("Device: INTZ %s | MRT %s | FP16 target %s | FP16 blend %s | MSAA %s | pure %s",
                  YesNo(s.caps & WXL_GFX_CAP_INTZ), YesNo(s.caps & WXL_GFX_CAP_MRT),
                  YesNo(s.caps & WXL_GFX_CAP_FP16_TARGET), YesNo(s.caps & WXL_GFX_CAP_FP16_BLEND),
                  YesNo(s.caps & WXL_GFX_CAP_MSAA), YesNo(s.caps & WXL_GFX_CAP_PURE));
        Textf("Depth: %s", s.depthStatus ? s.depthStatus : "?");
        uint32_t w = 0, h = 0;
        if (frame::WorldSize(w, h)) Textf("World target: %ux%u", w, h);
        char requested[48], available[48];
        Textf("Frame %u | requested: %s | available: %s", s.frameIndex,
              Needs(s.lastRequested, requested, sizeof requested), Needs(s.lastAvailable, available, sizeof available));

        Heading("Passes");
        const size_t passCount = frame::PassCount();
        if (!passCount) Text("None registered.");
        for (size_t i = 0; i < passCount; ++i)
        {
            frame::PassInfo p{};
            if (!frame::GetPassInfo(i, p)) break;
            char wants[48], gpu[24];
            if (p.gpuMs >= 0.0f) std::snprintf(gpu, sizeof gpu, "%.2f ms", p.gpuMs);
            else                 std::snprintf(gpu, sizeof gpu, "-");
            Textf("[%d] %s | wants %s | %s | %s", p.order, p.name ? p.name : "?",
                  Needs(p.lastWants, wants, sizeof wants), p.ran ? "ran" : "idle", gpu);
        }
        const float total = frame::TotalGpuMs();
        if (!frame::ProfilingEnabled()) Text("GPU timing off (WXL_GFX_PROFILE=0)");
        else if (total >= 0.0f)         Textf("Total GPU: %.2f ms", total);
        else                            Text("Total GPU: not measured yet");

        Heading("Render targets");
        uint32_t count = 0, bytes = 0;
        targets::Stats(count, bytes);
        Textf("%u alive, %.1f MB", count, double(bytes) / (1024.0 * 1024.0));
        const size_t targetCount = targets::Count();
        for (size_t i = 0; i < targetCount; ++i)
        {
            char line[256];
            if (targets::Describe(i, line, sizeof line)) Text(line);
        }

        Heading("Shaders");
        const char* status = shaders::Status();   // includes the compile / cache-hit counts
        if (status && status[0]) Text(status);
        if (Api()->UiButton("Clear shader cache")) shaders::ClearCache();

        Heading("Vulkan (DXVK)");
        wxl::gfx::vulkan::PanelSection();

        Heading("Baked assets");
        Text(wxl::gfx::assets::Status());

        Heading("Textures");
        uint32_t bakeMs = 0;
        if (textures::BlueNoiseReady(bakeMs)) Textf("Blue noise: ready (baked in %u ms)", bakeMs);
        else                                  Text("Blue noise: not ready (baked on first request)");
    }
}

namespace wxl::gfx::ui
{
    void Install()
    {
        g_api->UiAddPanel("Graphics Extend", &Draw, nullptr);
    }
}
