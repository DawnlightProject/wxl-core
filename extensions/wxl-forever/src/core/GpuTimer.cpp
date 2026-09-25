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

#include "GpuTimer.hpp"

#include <windows.h>
#include <d3d9.h>

namespace wxl::forever
{
    bool GpuTimer::Create(IDirect3DDevice9* dev)
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

    void GpuTimer::Poll()
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

    void GpuTimer::Begin(IDirect3DDevice9* dev)
    {
        open_ = -1;
        if (!dev || !Create(dev)) return;
        Poll();
        Set& s = sets_[next_];
        if (s.pending) return;   // still in flight; skip this frame rather than wait
        s.disjoint->Issue(D3DISSUE_BEGIN);
        s.stamps[0]->Issue(D3DISSUE_END);
        s.marked[0] = -1;
        s.count = 1;
        open_ = next_;
        next_ = (next_ + 1) % kRing;
    }

    void GpuTimer::Mark(int span)
    {
        if (open_ < 0 || span < 0 || span >= kSpans) return;
        Set& s = sets_[open_];
        if (s.count > kSpans) return;
        s.stamps[s.count]->Issue(D3DISSUE_END);
        s.marked[s.count] = span;
        ++s.count;
    }

    void GpuTimer::End()
    {
        if (open_ < 0) return;
        Set& s = sets_[open_];
        s.freq->Issue(D3DISSUE_END);
        s.disjoint->Issue(D3DISSUE_END);
        s.pending = true;
        open_ = -1;
    }

    void GpuTimer::Release()
    {
        for (Set& s : sets_)
        {
            if (s.disjoint) s.disjoint->Release();
            if (s.freq)     s.freq->Release();
            for (IDirect3DQuery9*& q : s.stamps)
                if (q) { q->Release(); q = nullptr; }
            s = Set{};
        }
        created_ = false;
        open_ = -1;
        next_ = 0;
    }
}
