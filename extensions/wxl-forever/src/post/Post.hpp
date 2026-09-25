// wxl-forever post: what happens to the finished image, last in the chain: bloom first.
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

struct IDirect3DTexture9;

// Then the eye: the image's average luminance (a log-luminance pyramid down to one texel) eased
// over time, faster going brighter than going darker, gives a clamped exposure, and a filmic curve
// (an ACES fit) takes the exposed light to the screen, once, at the end of the suite's chain. Until
// the world draws into an HDR target the LDR scene is opened up by a gentle inverse curve first.
//
// Bloom makes light sources glow: a bright pass (the image's own bright pixels with a soft knee,
// averaged so one hot pixel cannot flicker, plus the peaks of surface lighting's light buffer when
// it ran this frame), an FP16 pyramid of up to six levels down and back up with a dual filter whose
// fine levels fade with the softness, then the glow added over the image with a tint and an
// intensity. Runs after every other feature (order 300) and works with any of them off.
namespace wxl::forever::post
{
    struct Settings
    {
        int   bloom      = 1;
        float threshold  = 0.85f;   // linear luminance where glow starts
        float knee       = 0.25f;   // how gradually it starts
        float intensity  = 0.25f;
        float softness   = 0.7f;    // 0 every level equal (a tight halo), 1 only the widest levels (a haze)
        int   levels     = 5;       // pyramid depth, 3..6: each level doubles the glow's radius
        int   stable     = 1;       // the bright pass averages its four source pixels by brightness (no fireflies)
        float tint[4]    = { 1.0f, 0.95f, 0.9f, 1.0f };
        float lightShare = 0.3f;    // how much the light buffer's peaks glow
        int   view       = 0;       // 0 image, 1 bloom only, 2 luminance
        int   tonemap    = 1;       // the filmic curve at the end of the chain
        float filmic     = 1.0f;    // 0 the image as it was .. 1 fully filmic
        int   curve      = 1;       // 0 ACES (contrasty, crushes darks), 1 gentle (a shoulder, no toe)
        float white      = 2.5f;    // gentle curve: the exposed light that reaches white
        float saturation = 0.9f;    // colour after the curve: 1 as is, below 1 muted
        int   adaptation = 1;       // the eye adapts to the image's brightness
        float key        = 0.18f;   // the average luminance the eye brings the image to
        float minExposure = 0.6f;   // how far the eye may darken a bright image
        float maxExposure = 2.5f;   // how far it may brighten a dark one
        float adaptUp    = 3.0f;    // speed of adapting to a brighter image, per second
        float adaptDown  = 0.6f;    // speed of adapting to a darker one (a dark building opens slowly)
        float adaptStrength = 0.5f; // share of the brightness change the eye makes up (1 all of it)
        int   centreMetering = 1;   // the eye weighs the centre of the view more than its edges
        float unclip     = 0.0f;    // how strongly the LDR scene's brights are opened up (off until HDR)
        int   hdrWorld   = 0;       // the world draws into an FP16 target; this feature resolves it
    };

    Settings& Config();

    /// Reads the config, registers the pass and the panel. Called once at load.
    void Install();

    /// The feature's parts of the Forever window (core/ForeverUi.cpp): its line in Overview, its own
    /// tab, and its group in Debug.
    void UiOverview();
    void UiSettings();
    void UiDebug();
}
