// wxl-forever: the order the suite's render features draw in after the world pass.
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

#include <cstdint>

struct IDirect3DDevice9;
struct IDirect3DTexture9;

// A feature adds a pass with an order; after the world pass drew, every pass that wants to run
// draws over the scene in rising order (surface lighting before the fog, so the fog veils lit
// surfaces). The world's depth is redirected to a texture whenever any pass wants to run, and
// nothing is redirected when none does. Each feature stays usable with every other one off.
//
// When a feature drew the world into an HDR target (BeginFrame::colorOverride), the passes run on
// that target (it is render target 0 while they draw) and the last one resolves it to backBuffer.
namespace wxl::forever::passes
{
    /// What every pass of one frame shares.
    struct Frame
    {
        IDirect3DDevice9*  device;
        IDirect3DTexture9* depth;            // the world's INTZ depth (core/SceneDepth)
        unsigned           width, height;
        float              rangeMin, rangeMax;  // depth range the world pass drew into
        float              eye[3];
        float              viewProjRel[16];  // camera-relative view * projection, row vectors
        uint32_t           index;            // frames the passes have run in
        void*              normals;          // IDirect3DSurface9*, the world's G-buffer normals, or null
        void*              sceneColor;       // IDirect3DSurface9*, the world drawn in HDR, or null
        int*               colorResolved;    // set to 1 once a pass resolved sceneColor to backBuffer
        void*              backBuffer;       // IDirect3DSurface9*, where the finished image goes
    };

    /// What a feature may do before the world pass draws: supply targets for it and set what the
    /// engine's own shaders read during it.
    struct BeginFrame
    {
        IDirect3DDevice9* device;
        void*  sceneDepth;        // IDirect3DSurface9*, the engine's depth surface (its size)
        void** normalTarget;      // write an A8R8G8B8 surface here to receive the G-buffer normals
        void** colorOverride;     // write an A16B16G16R16F surface here to draw the world in HDR
        float  eye[3];
        float  viewRel[16];       // camera-relative view: [r, 1] * it = the engine's view space
        float  viewProjRel[16];
        uint32_t index;           // the index the coming frame will have
    };

    using BeginFn = void (*)(const BeginFrame& frame);

    /// Runs before every world pass while any pass wants to run.
    void AddBegin(BeginFn begin);

    using WantsFn = bool (*)();
    using DrawFn  = void (*)(const Frame& frame);

    /// Order: lower draws first. Surface lighting uses 100, the fog 200.
    void Add(int order, const char* name, WantsFn wants, DrawFn draw);

    /// Frames the passes have run in; a service can do its work once per frame with it.
    uint32_t FrameIndex();

    /// Hooks the world pass. Called once at load, before the features.
    void Install();
}
