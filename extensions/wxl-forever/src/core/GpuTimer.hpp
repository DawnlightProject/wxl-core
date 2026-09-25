// wxl-forever: GPU time of consecutive spans of draws, from D3D9 timestamp queries read a few frames late.
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

struct IDirect3DDevice9;
struct IDirect3DQuery9;

namespace wxl::forever
{
    /// Begin, then Mark(i) after span i, then End. Each span's time is smoothed separately; -1 until
    /// a reading arrives. Never waits on the GPU: a result not ready yet is picked up later. A span
    /// skipped in a frame (not marked) reads zero for that frame.
    class GpuTimer
    {
    public:
        static constexpr int kSpans = 8;

        void Begin(IDirect3DDevice9* dev);
        void Mark(int span);
        void End();
        void Release();

        /// Smoothed time of one span, or of all of them together with span -1.
        float Milliseconds(int span = -1) const { return span < 0 ? total_ : spans_[span]; }
        bool Supported() const { return !unsupported_; }

    private:
        struct Set
        {
            IDirect3DQuery9* disjoint = nullptr;
            IDirect3DQuery9* freq = nullptr;
            IDirect3DQuery9* stamps[kSpans + 1] = {};
            int  marked[kSpans + 1] = {};   // which span each stamp closes; -1 for the opening one
            int  count = 0;
            bool pending = false;
        };
        static constexpr int kRing = 4;

        bool Create(IDirect3DDevice9* dev);
        void Poll();

        Set   sets_[kRing];
        int   next_ = 0;
        int   open_ = -1;
        bool  created_ = false;
        bool  unsupported_ = false;
        float spans_[kSpans] = { -1, -1, -1, -1, -1, -1, -1, -1 };
        float total_ = -1.0f;
    };
}
