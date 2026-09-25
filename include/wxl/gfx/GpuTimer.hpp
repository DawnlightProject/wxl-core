// wxl::gfx::GpuTimer: GPU time of consecutive spans of draws, from D3D9 timestamp queries read a few
// frames late, never waiting on the GPU.
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

#include <windows.h>
#include <d3d9.h>

// Header-only. Begin, then Mark(i) after span i, then End, once per frame. Mark(-1) closes a span
// that is timed but attributed to nothing, so work between two measured spans does not inflate the
// second. Each span's time is smoothed separately; -1 until a reading arrives. A result not ready yet is picked up on a later
// frame (a ring of kRing query sets); a frame whose set is still in flight is skipped rather than
// waited for. A span not marked in a frame reads zero for that frame. Call Release from OnDeviceLost:
// queries do not survive a reset.
namespace wxl::gfx
{
    class GpuTimer
    {
    public:
        static constexpr int kSpans = 16;

        void Begin(IDirect3DDevice9* dev)
        {
            open_ = -1;
            if (!dev || !Create(dev)) return;
            Poll();
            Set& s = sets_[next_];
            if (s.pending) return;
            s.disjoint->Issue(D3DISSUE_BEGIN);
            s.stamps[0]->Issue(D3DISSUE_END);
            s.marked[0] = -1;
            s.count = 1;
            open_ = next_;
            next_ = (next_ + 1) % kRing;
        }

        void Mark(int span)
        {
            if (open_ < 0 || span < -1 || span >= kSpans) return;
            Set& s = sets_[open_];
            if (s.count > kSpans) return;
            s.stamps[s.count]->Issue(D3DISSUE_END);
            s.marked[s.count] = span;
            ++s.count;
        }

        void End()
        {
            if (open_ < 0) return;
            Set& s = sets_[open_];
            s.freq->Issue(D3DISSUE_END);
            s.disjoint->Issue(D3DISSUE_END);
            s.pending = true;
            open_ = -1;
        }

        void Release()
        {
            for (Set& s : sets_)
            {
                if (s.disjoint) s.disjoint->Release();
                if (s.freq)     s.freq->Release();
                for (IDirect3DQuery9* q : s.stamps)
                    if (q) q->Release();
                s = Set{};
            }
            created_ = false;
            open_ = -1;
            next_ = 0;
        }

        /// Smoothed milliseconds of one span, or of every span together with span -1; -1 when unknown.
        float Milliseconds(int span = -1) const
        {
            if (span < 0) return total_;
            return span < kSpans ? spans_[span] : -1.0f;
        }

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

        bool Create(IDirect3DDevice9* dev)
        {
            if (created_) return true;
            if (unsupported_) return false;
            for (Set& s : sets_)
            {
                bool ok = SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.disjoint))
                       && SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &s.freq));
                for (int i = 0; ok && i <= kSpans; ++i)
                    ok = SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.stamps[i]));
                if (!ok)
                {
                    Release();
                    unsupported_ = true;
                    return false;
                }
            }
            created_ = true;
            return true;
        }

        void Poll()
        {
            for (Set& s : sets_)
            {
                if (!s.pending) continue;
                BOOL disjoint = TRUE;
                UINT64 freq = 0;
                UINT64 stamps[kSpans + 1] = {};
                if (s.disjoint->GetData(&disjoint, sizeof disjoint, 0) != S_OK) continue;
                if (s.freq->GetData(&freq, sizeof freq, 0) != S_OK) continue;
                bool ready = true;
                for (int i = 0; i < s.count && ready; ++i)
                    ready = s.stamps[i]->GetData(&stamps[i], sizeof stamps[i], 0) == S_OK;
                if (!ready) continue;
                s.pending = false;
                if (disjoint || freq == 0) continue;

                float frame[kSpans] = {};
                for (int i = 1; i < s.count; ++i)
                    if (s.marked[i] >= 0 && stamps[i] >= stamps[i - 1])
                        frame[s.marked[i]] += float(double(stamps[i] - stamps[i - 1]) * 1000.0 / double(freq));
                float total = 0.0f;
                for (int i = 0; i < kSpans; ++i)
                {
                    spans_[i] = spans_[i] < 0.0f ? frame[i] : spans_[i] * 0.9f + frame[i] * 0.1f;
                    total += spans_[i];
                }
                total_ = total;
            }
        }

        Set   sets_[kRing];
        int   next_ = 0;
        int   open_ = -1;
        bool  created_ = false;
        bool  unsupported_ = false;
        float spans_[kSpans] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
        float total_ = -1.0f;
    };
}
