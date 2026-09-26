// wxl::gfx::ui: the panel helpers every graphics extension draws its settings with -- tabs, scoped
// ids and a "(?)" marker explaining each setting.
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

#include "wxl/PluginApi.h"

#include <cstdarg>
#include <cstddef>
#include <cstdio>

// Header-only (wxl-forever's core/Panel.hpp, generalised). Call Bind(api) once from WXL_Load; every
// helper is then valid inside a WXL_PanelFn only, like the Ui* functions it wraps. On a core too old
// for tabs and tooltips the tabs become collapsing headers and the markers are left out.
namespace wxl::gfx::ui
{
    inline const WXL_Api*& ApiSlot()
    {
        static const WXL_Api* api = nullptr;
        return api;
    }

    inline void Bind(const WXL_Api* api) { ApiSlot() = api; }
    inline const WXL_Api* Api() { return ApiSlot(); }

    inline bool Has(size_t fieldEnd) { return Api() && Api()->structSize >= fieldEnd; }
    inline bool HasTabs() { return Has(offsetof(WXL_Api, UiEndTabItem) + sizeof(void*)); }
    inline bool HasTooltips() { return Has(offsetof(WXL_Api, UiTextDisabled) + sizeof(void*)); }

    // Two controls with the same label in one window share an ImGui id. A scope set per section makes
    // each label "label##scope": the text shown is unchanged, the id is the section's own.
    inline const char*& CurrentScope()
    {
        static const char* scope = nullptr;
        return scope;
    }

    struct Scope
    {
        const char* saved;
        explicit Scope(const char* name) : saved(CurrentScope()) { CurrentScope() = name; }
        ~Scope() { CurrentScope() = saved; }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };

    /// The label with the current scope as its hidden id part (rotating buffers, each used at once).
    inline const char* Id(const char* label)
    {
        const char* scope = CurrentScope();
        if (!scope || !label) return label;
        static char buffers[4][192];
        static int next = 0;
        char* out = buffers[next++ & 3];
        std::snprintf(out, sizeof buffers[0], "%s##%s", label, scope);
        return out;
    }

    inline bool Section(const char* label) { return Api()->UiCollapsingHeader(Id(label)) != 0; }

    inline bool BeginTabs(const char* id) { return HasTabs() ? Api()->UiBeginTabBar(id) != 0 : true; }
    inline void EndTabs() { if (HasTabs()) Api()->UiEndTabBar(); }

    /// True while the tab is selected (or its header open); call EndTab only then.
    inline bool BeginTab(const char* label)
    {
        return HasTabs() ? Api()->UiBeginTabItem(Id(label)) != 0 : Api()->UiCollapsingHeader(Id(label)) != 0;
    }
    inline void EndTab() { if (HasTabs()) Api()->UiEndTabItem(); }

    /// The "(?)" marker after the last control, explaining it on hover.
    inline void Help(const char* text)
    {
        if (!text || !HasTooltips()) return;
        Api()->UiSameLine();
        Api()->UiTextDisabled("(?)");
        Api()->UiItemTooltip(text);
    }

    inline void Text(const char* text) { Api()->UiText(text); }
    inline void Separator() { Api()->UiSeparator(); }

    inline void Textf(const char* fmt, ...)
    {
        char buf[512];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(buf, sizeof buf, fmt, args);
        va_end(args);
        Api()->UiText(buf);
    }

    inline bool Button(const char* label, const char* help = nullptr)
    {
        const bool pressed = Api()->UiButton(Id(label)) != 0;
        Help(help);
        return pressed;
    }

    inline bool Check(const char* label, int* value, const char* help = nullptr)
    {
        const bool changed = Api()->UiCheckbox(Id(label), value) != 0;
        Help(help);
        return changed;
    }

    inline bool Slider(const char* label, float* value, float lo, float hi, const char* help = nullptr)
    {
        const bool changed = Api()->UiSliderFloat(Id(label), value, lo, hi) != 0;
        Help(help);
        return changed;
    }

    inline bool Slider(const char* label, int* value, int lo, int hi, const char* help = nullptr)
    {
        const bool changed = Api()->UiSliderInt(Id(label), value, lo, hi) != 0;
        Help(help);
        return changed;
    }

    inline bool Combo(const char* label, int* index, const char* const* items, int count, const char* help = nullptr)
    {
        const bool changed = Api()->UiCombo(Id(label), index, items, count) != 0;
        Help(help);
        return changed;
    }

    inline bool Color(const char* label, float rgba[4], const char* help = nullptr)
    {
        const bool changed = Api()->UiColorEdit(Id(label), rgba) != 0;
        Help(help);
        return changed;
    }
}
