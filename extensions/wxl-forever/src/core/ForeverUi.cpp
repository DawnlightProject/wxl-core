// wxl-forever: the one Forever window, its features as top-level tabs.
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

#include "ForeverUi.hpp"
#include "Panel.hpp"
#include "../fog/Fog.hpp"
#include "../lights/Lights.hpp"
#include "../post/Post.hpp"
#include "../surface/Surface.hpp"
#include "../terrain/Terrain.hpp"

// Overview: each feature's switch, state and GPU time. Then one tab per feature with its settings,
// and Debug: every view and isolate, one folded section per feature.
namespace
{
    namespace ui = wxl::forever::ui;
    namespace fv = wxl::forever;

    void Heading(const char* text)
    {
        ui::Separator();
        ui::Text(text);
    }

    void __cdecl Panel(void*)
    {
        if (!ui::BeginTabs("forever")) return;
        if (ui::BeginTab("Overview"))
        {
            ui::Text("Each feature works on its own. The fog Quality below is the one preset; every other setting is on its tab.");
            Heading("Fog");
            fv::fog::UiOverview();
            Heading("Lights");
            fv::lights::UiOverview();
            Heading("Surface");
            fv::surface::UiOverview();
            Heading("Post");
            fv::post::UiOverview();
            Heading("Terrain");
            fv::terrain::UiOverview();
            ui::EndTab();
        }
        if (ui::BeginTab("Fog"))     { fv::fog::UiSettings();     ui::EndTab(); }
        if (ui::BeginTab("Lights"))  { fv::lights::UiSettings();  ui::EndTab(); }
        if (ui::BeginTab("Surface")) { fv::surface::UiSettings(); ui::EndTab(); }
        if (ui::BeginTab("Post"))    { fv::post::UiSettings();    ui::EndTab(); }
        if (ui::BeginTab("Terrain")) { fv::terrain::UiSettings(); ui::EndTab(); }
        if (ui::BeginTab("Debug"))
        {
            ui::Text("Views and isolates, one section per feature. An isolate removes one ingredient, to find which one causes an artefact.");
            if (ui::Section("Fog"))     fv::fog::UiDebug();
            if (ui::Section("Lights"))  fv::lights::UiDebug();
            if (ui::Section("Surface")) fv::surface::UiDebug();
            if (ui::Section("Post"))    fv::post::UiDebug();
            if (ui::Section("Terrain")) fv::terrain::UiDebug();
            ui::EndTab();
        }
        ui::EndTabs();
    }
}

namespace wxl::forever::ui
{
    void InstallWindow()
    {
        wxl_forever::g_api->UiAddPanel("Forever", &Panel, nullptr);
    }
}
