// wxl-forever: the panel framework every feature draws its settings with.
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

#include "ExtensionApi.hpp"

#include <cstddef>
#include <cstdio>

// Tabs, and a "(?)" marker after every setting whose tooltip says what it changes. On a core too old
// for tabs and tooltips the tabs become collapsing headers and the markers are left out.
namespace wxl::forever::ui
{
    inline const WXL_Api* Api() { return wxl_forever::g_api; }

    inline bool Has(size_t fieldEnd) { return Api() && Api()->structSize >= fieldEnd; }

    inline bool HasTabs()
    {
        return Has(offsetof(WXL_Api, UiEndTabItem) + sizeof(void*));
    }

    inline bool HasTooltips()
    {
        return Has(offsetof(WXL_Api, UiTextDisabled) + sizeof(void*));
    }

    // Every feature draws into the one Forever window, where two controls with the same label would
    // share an ImGui id. A scope set per feature section makes each label "label##scope": the text
    // shown is unchanged, the id is the feature's own.
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
    };

    /// The label with the current scope as its hidden id part (a few rotating buffers, each used at once).
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

    /// A section folded until opened: the Debug tab's per-feature groups.
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

    inline bool Button(const char* label, const char* help)
    {
        const bool pressed = Api()->UiButton(Id(label)) != 0;
        Help(help);
        return pressed;
    }

    inline void Text(const char* text) { Api()->UiText(text); }
    inline void Separator() { Api()->UiSeparator(); }

    inline bool Check(const char* label, int* value, const char* help)
    {
        const bool changed = Api()->UiCheckbox(Id(label), value) != 0;
        Help(help);
        return changed;
    }

    inline bool Slider(const char* label, float* value, float lo, float hi, const char* help)
    {
        const bool changed = Api()->UiSliderFloat(Id(label), value, lo, hi) != 0;
        Help(help);
        return changed;
    }

    inline bool Slider(const char* label, int* value, int lo, int hi, const char* help)
    {
        const bool changed = Api()->UiSliderInt(Id(label), value, lo, hi) != 0;
        Help(help);
        return changed;
    }

    inline bool Combo(const char* label, int* index, const char* const* items, int count, const char* help)
    {
        const bool changed = Api()->UiCombo(Id(label), index, items, count) != 0;
        Help(help);
        return changed;
    }

    inline bool Color(const char* label, float rgba[4], const char* help)
    {
        const bool changed = Api()->UiColorEdit(Id(label), rgba) != 0;
        Help(help);
        return changed;
    }
}
