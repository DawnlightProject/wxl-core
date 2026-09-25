// wxl-forever terrain: baked horizon maps and the terrain as a sun shadow caster.
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

#include "../core/ExtensionApi.hpp"
#include "Terrain.hpp"
#include "Horizon.hpp"
#include "Caster.hpp"

#include "../core/Matrix.hpp"
#include "../core/Panel.hpp"
#include "../core/Passes.hpp"
#include "../core/RenderUtil.hpp"
#include "../core/ShaderLibrary.hpp"

#include "wxl/EventScript.hpp"
#include "game/Camera.hpp"
#include "game/Shadows.hpp"
#include "game/Sky.hpp"

#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    namespace ev     = wxl::events;
    namespace cam    = wxl::game::camera;
    namespace sky    = wxl::game::sky;
    namespace sh     = wxl::game::shadows;
    namespace hz     = wxl::forever::terrain::horizon;
    namespace cs     = wxl::forever::terrain::caster;
    namespace ps     = wxl::forever::passes;
    namespace render = wxl::forever::render;
    namespace mx     = wxl::forever::matrix;
    namespace ui     = wxl::forever::ui;

    int g_view = 0;   // debug view, 0 off (shaders/debug.ps.hlsl)

    /// The world pass ended: stream the horizon tiles and keep the caster in step.
    class Ticker final : public wxl::ext::EventScript
    {
    public:
        Ticker()
        {
            on<&Ticker::OnWorldSceneEnd>(ev::Event::OnWorldSceneEnd);
            on<&Ticker::OnDeviceLost>(ev::Event::OnDeviceLost);
        }

        void OnWorldSceneEnd(const ev::WorldSceneEndArgs& a)
        {
            auto* dev = static_cast<IDirect3DDevice9*>(a.device);
            if (!dev) return;
            float eye[3];
            cam::GetPosition(eye);
            hz::Update(dev, eye, ps::FrameIndex());
            cs::Frame(dev);
        }

        // Everything here is MANAGED; nothing to drop, the reset keeps it.
        void OnDeviceLost(const ev::DeviceResetArgs&) {}
    };

    bool DebugWants() { return g_view != 0 && hz::Get().enabled != 0; }

    /// Draws the chosen view over the scene, one full-screen pass.
    void DebugDraw(const ps::Frame& f)
    {
        IDirect3DDevice9* d = f.device;
        IDirect3DVertexShader9* vs = wxl::forever::shaders::Vertex(d, "core.fullscreen");
        IDirect3DPixelShader9* p = wxl::forever::shaders::Pixel(d, "terrain.debug");
        if (!vs || !p || !f.depth) return;
        float inv[16];
        if (!mx::Invert4(f.viewProjRel, inv)) return;

        float c[14][4] = {};
        mx::Columns(inv, &c[0]);
        c[4][0] = f.eye[0]; c[4][1] = f.eye[1]; c[4][2] = f.eye[2]; c[4][3] = float(g_view);
        c[5][0] = f.rangeMin; c[5][1] = 1.0f / std::max(f.rangeMax - f.rangeMin, 1e-6f); c[5][2] = f.rangeMax; c[5][3] = 1.0f / float(f.width);
        sky::CelestialLight light{};
        if (sky::GetCelestialLight(light)) std::memcpy(&c[6][0], light.toSun, sizeof(float) * 3);
        else c[6][2] = 1.0f;
        c[6][3] = 1.0f / float(f.height);
        sh::Snapshot snap;
        sh::Get(snap);
        const sh::Slot slots[4] = { sh::Slot::Main, sh::Slot::Band0, sh::Slot::Band1, sh::Slot::Band2 };
        for (int k = 0; k < 4; ++k)
        {
            const sh::Map& m = snap.maps[size_t(slots[k])];
            c[7 + k][0] = m.centre[0]; c[7 + k][1] = m.centre[1]; c[7 + k][2] = m.halfExtent;
            c[7 + k][3] = snap.valid && m.present ? 1.0f : 0.0f;
        }
        int ox, oy;
        float zmin, zmax;
        hz::Block(ox, oy, zmin, zmax);
        c[11][0] = zmin; c[11][1] = 1.0f / std::max(zmax - zmin, 1.0f);
        hz::Constants(c[12], c[13], true);

        render::StateGuard guard(d, 14);
        render::PlainState(d);
        render::Sampler(d, 0, f.depth, false);
        hz::Bind(d, 1, 2, 3);
        d->SetVertexShader(vs);
        d->SetPixelShader(p);
        d->SetPixelShaderConstantF(0, &c[0][0], 14);
        render::Quad(d, f.width, f.height);
    }
}

namespace wxl::forever::terrain
{
    void Install()
    {
        horizon::Install();
        caster::Install();
        g_view = int(wxl_forever::ConfigFloat("WXL_FOREVER_TERRAIN_VIEW", 0.0f, 0.0f, 6.0f));
        static Ticker ticker;
        passes::Add(250, "terrain.debug", &DebugWants, &DebugDraw);
        WLOG_INFO("terrain: horizon maps %s, terrain shadows %s, low sun %s", horizon::Get().enabled ? "on" : "off",
                  caster::Get().enabled ? "on" : "off", caster::Get().lowSun ? "on" : "off");
    }

    void UiOverview()
    {
        ui::Scope scope("terrain.overview");
        ui::Check("Horizon maps", &horizon::Get().enabled,
                  "Baked terrain horizons around the camera: the fog and surfaces know where hills shadow the sun and hide the sky, on and off screen.");
        ui::Check("Terrain shadows", &caster::Get().enabled,
                  "The terrain is drawn into the engine's sun shadow maps, so hills and cliffs cast shadows like buildings do. Needs extShadowQuality above 0.");
        ui::Check("Low sun", &caster::Get().lowSun,
                  "Lets the shadow light follow the real sun lower at dawn and dusk. The engine lifts it to at least 50 degrees, which keeps every shadow short.");
        ui::Text(horizon::Status());
        ui::Text(caster::Status());
    }

    void UiSettings()
    {
        ui::Scope scope("terrain");
        horizon::Settings& h = horizon::Get();
        caster::Settings& c = caster::Get();
        ui::Text("Horizon maps");
        ui::Check("Enabled", &h.enabled, "Streams the baked horizon tiles around the camera (a 4 x 4 block, about 5 MB) for the shaders to read.");
        ui::Slider("Strength", &h.strength, 0.0f, 1.0f, "How much of the baked terrain shadowing the consumers apply. 0 turns the effect off while the tiles stay resident.");
        ui::Slider("Penumbra (deg)", &h.penumbra, 0.1f, 10.0f,
                   "Degrees of sun elevation the terrain's shadow edge fades over. The sun is half a degree wide; wider hides the 22.5 degree azimuth steps of the bake.");
        ui::Slider("Occluder distance (yd)", &h.occluder, 10.0f, 400.0f,
                   "How far away the terrain forming the horizon is assumed to be. A point that high above the ground sees the sun over it; fog high above a valley is not shadowed by its walls.");
        ui::Text(horizon::Status());
        ui::Separator();
        ui::Text("Terrain shadows");
        ui::Check("Terrain casts", &c.enabled, "Draws the resident terrain tiles depth-only into the engine's sun shadow maps after its own casters.");
        ui::Slider("Cascades", &c.cascades, 1, 4,
                   "How many of the engine's maps receive terrain: 1 the 40 yd main map only, 2 adds the 80 yd band, 3 the 320 yd band, 4 the 1280 yd band. Fewer is cheaper.");
        ui::Slider("Depth bias (yd)", &c.bias, 0.0f, 2.0f,
                   "Yards the terrain is pushed away from the light before it is drawn. Raise it if slopes show stripes of self-shadow; lower it if shadows detach from the ground.");
        ui::Slider("Detail", &c.detail, -1, 1,
                   "Mesh step per cascade: 0 draws 4 yd cells in the near maps and 8 and 17 yd cells in the far ones; -1 halves the step (more triangles), 1 doubles it.");
        ui::Text(caster::Status());
        ui::Separator();
        ui::Text("Low sun");
        ui::Check("Adjust the shadow light", &c.lowSun,
                  "Replaces the engine's shadow light direction (the sun lifted five times, never under 50 degrees) with the real sun, lifted by the scale below and floored at the minimum elevation.");
        ui::Slider("Sun height scale", &c.heightScale, 0.5f, 5.0f,
                   "Multiplier on the sun's height before the shadows are rendered. 1 is the true sun; 5 is what the engine does. Lower makes longer evening shadows.");
        ui::Slider("Minimum elevation (deg)", &c.minElevation, 2.0f, 60.0f,
                   "The shadow light never drops below this elevation. Very low values make shadows longer than the maps can hold, so they cut off at the map's edge.");
    }

    void UiDebug()
    {
        ui::Scope scope("terrain.debug");
        static const char* const kViews[] = {
            "Off", "Sun visibility", "Sky occlusion", "Horizon at the sun's azimuth", "Cascade coverage", "Terrain height", "Resident tiles",
        };
        ui::Combo("View", &g_view, kViews, 7,
                  "Over the scene: how much sun the terrain lets through at each pixel, how much sky it sees, the baked horizon elevation towards the sun (black flat to red steep), which shadow cascade holds the pixel (red main, green, blue, yellow bands, grey none), the terrain height, or the block of resident tiles. Red tint: outside the resident block.");
    }
}
