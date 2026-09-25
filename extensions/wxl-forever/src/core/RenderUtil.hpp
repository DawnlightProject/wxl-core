// wxl-forever: what every full-screen pass needs: the engine's state saved and put back, samplers,
// a quad, and the plain state such a pass draws with.
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

namespace wxl::forever::render
{
    /// Everything a pass touches, read back on construction and put back on destruction: render
    /// target 0, viewport, shaders, vertex input, the first kStages samplers, the states a pass
    /// changes and the first `constants` pixel shader registers.
    class StateGuard
    {
    public:
        static constexpr DWORD kStages = 11;
        static constexpr UINT  kMaxConstants = 224;

        StateGuard(IDirect3DDevice9* d, UINT constants) : d_(d), constants_(constants < kMaxConstants ? constants : kMaxConstants)
        {
            d_->GetRenderTarget(0, &rt_);
            d_->GetViewport(&vp_);
            d_->GetVertexShader(&vs_);
            d_->GetPixelShader(&ps_);
            d_->GetVertexDeclaration(&decl_);
            d_->GetFVF(&fvf_);
            d_->GetStreamSource(0, &vb_, &vbOffset_, &vbStride_);
            for (DWORD s = 0; s < kStages; ++s)
            {
                d_->GetTexture(s, &tex_[s]);
                for (size_t i = 0; i < _countof(kSamplers); ++i) d_->GetSamplerState(s, kSamplers[i], &samp_[s][i]);
            }
            for (size_t i = 0; i < _countof(kStates); ++i) d_->GetRenderState(kStates[i], &states_[i]);
            if (constants_) d_->GetPixelShaderConstantF(0, &consts_[0][0], constants_);
        }

        ~StateGuard()
        {
            d_->SetRenderTarget(0, rt_);
            d_->SetViewport(&vp_);
            if (constants_) d_->SetPixelShaderConstantF(0, &consts_[0][0], constants_);
            for (size_t i = 0; i < _countof(kStates); ++i) d_->SetRenderState(kStates[i], states_[i]);
            for (DWORD s = 0; s < kStages; ++s)
            {
                for (size_t i = 0; i < _countof(kSamplers); ++i) d_->SetSamplerState(s, kSamplers[i], samp_[s][i]);
                d_->SetTexture(s, tex_[s]);
                if (tex_[s]) tex_[s]->Release();
            }
            d_->SetVertexShader(vs_);
            d_->SetPixelShader(ps_);
            if (decl_) d_->SetVertexDeclaration(decl_);
            else       d_->SetFVF(fvf_);
            // DrawPrimitiveUP unbinds stream 0; the engine does not expect to rebind it.
            d_->SetStreamSource(0, vb_, vbOffset_, vbStride_);
            if (rt_)   rt_->Release();
            if (vs_)   vs_->Release();
            if (ps_)   ps_->Release();
            if (decl_) decl_->Release();
            if (vb_)   vb_->Release();
        }

        StateGuard(const StateGuard&) = delete;
        StateGuard& operator=(const StateGuard&) = delete;

    private:
        static constexpr D3DRENDERSTATETYPE kStates[] = {
            D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE,
            D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_SEPARATEALPHABLENDENABLE,
            D3DRS_CULLMODE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_COLORWRITEENABLE,
            D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
        };
        static constexpr D3DSAMPLERSTATETYPE kSamplers[] = {
            D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_MAGFILTER, D3DSAMP_MINFILTER,
            D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE,
        };

        IDirect3DDevice9*            d_;
        UINT                         constants_;
        IDirect3DSurface9*           rt_ = nullptr;
        D3DVIEWPORT9                 vp_{};
        IDirect3DVertexShader9*      vs_ = nullptr;
        IDirect3DPixelShader9*       ps_ = nullptr;
        IDirect3DVertexDeclaration9* decl_ = nullptr;
        DWORD                        fvf_ = 0;
        IDirect3DVertexBuffer9*      vb_ = nullptr;
        UINT                         vbOffset_ = 0, vbStride_ = 0;
        IDirect3DBaseTexture9*       tex_[kStages] = {};
        DWORD                        samp_[kStages][_countof(kSamplers)] = {};
        DWORD                        states_[_countof(kStates)] = {};
        float                        consts_[kMaxConstants][4] = {};
    };

    inline void Sampler(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* tex, bool linear, bool wrap = false,
                        bool mips = false)
    {
        const DWORD address = wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
        d->SetTexture(stage, tex);
        d->SetSamplerState(stage, D3DSAMP_ADDRESSU, address);
        d->SetSamplerState(stage, D3DSAMP_ADDRESSV, address);
        d->SetSamplerState(stage, D3DSAMP_ADDRESSW, address);
        d->SetSamplerState(stage, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        d->SetSamplerState(stage, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        d->SetSamplerState(stage, D3DSAMP_MIPFILTER, mips ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        d->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    /// A quad over a w x h viewport; the pass-through vertex shader takes it in clip space.
    inline void Quad(IDirect3DDevice9* d, UINT w, UINT h)
    {
        const D3DVIEWPORT9 vp{ 0, 0, w, h, 0.0f, 1.0f };
        d->SetViewport(&vp);
        const float quad[4][4] = {
            { -1.0f,  1.0f, 0.0f, 1.0f },
            {  1.0f,  1.0f, 0.0f, 1.0f },
            { -1.0f, -1.0f, 0.0f, 1.0f },
            {  1.0f, -1.0f, 0.0f, 1.0f },
        };
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof quad[0]);
    }

    /// The plain state a full-screen pass draws with: no depth, no blend, no fog, all channels.
    inline void PlainState(IDirect3DDevice9* d)
    {
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        d->SetFVF(D3DFVF_XYZW);
    }
}
