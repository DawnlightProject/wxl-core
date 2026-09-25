// wxl::gfx::render: what every pass that draws after the engine needs -- the engine's device state
// saved and put back, sampler setup, a clip-space quad and the plain state such a pass draws with.
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

// Header-only. The engine caches the D3D9 state it set and does not re-set a value it believes is
// current, so a pass that changes device state behind its back must put every value back exactly: a
// forgotten sampler or render state shows up as a corrupted draw several frames later, somewhere
// else. StateGuard reads the state back on construction (the proxy strips D3DCREATE_PUREDEVICE so the
// Get* calls answer) and restores it on destruction.
namespace wxl::gfx::render
{
    class StateGuard
    {
    public:
        static constexpr DWORD kMaxStages      = 16;
        static constexpr UINT  kMaxPsConstants = 224;
        static constexpr UINT  kMaxVsConstants = 256;
        static constexpr DWORD kTargets        = 4;

        /**
         * @param d            the device.
         * @param psConstants  how many of the first pixel shader float registers to save (<= 224).
         * @param stages       how many of the first samplers (texture + sampler states) to save (<= 16).
         * @param vsConstants  how many of the first vertex shader float registers to save (<= 256).
         */
        explicit StateGuard(IDirect3DDevice9* d, UINT psConstants = 0, DWORD stages = 8, UINT vsConstants = 0)
            : d_(d),
              psConstants_(psConstants < kMaxPsConstants ? psConstants : kMaxPsConstants),
              vsConstants_(vsConstants < kMaxVsConstants ? vsConstants : kMaxVsConstants),
              stages_(stages < kMaxStages ? stages : kMaxStages)
        {
            for (DWORD t = 0; t < kTargets; ++t)
                if (FAILED(d_->GetRenderTarget(t, &rt_[t]))) rt_[t] = nullptr;
            if (FAILED(d_->GetDepthStencilSurface(&ds_))) ds_ = nullptr;
            d_->GetViewport(&vp_);
            d_->GetScissorRect(&scissor_);
            d_->GetVertexShader(&vs_);
            d_->GetPixelShader(&ps_);
            d_->GetVertexDeclaration(&decl_);
            d_->GetFVF(&fvf_);
            d_->GetStreamSource(0, &vb_, &vbOffset_, &vbStride_);
            d_->GetIndices(&ib_);
            for (DWORD s = 0; s < stages_; ++s)
            {
                d_->GetTexture(s, &tex_[s]);
                for (size_t i = 0; i < kSamplerCount; ++i) d_->GetSamplerState(s, kSamplers[i], &samp_[s][i]);
            }
            for (size_t i = 0; i < kStateCount; ++i) d_->GetRenderState(kStates[i], &states_[i]);
            if (psConstants_) d_->GetPixelShaderConstantF(0, &psConsts_[0][0], psConstants_);
            if (vsConstants_) d_->GetVertexShaderConstantF(0, &vsConsts_[0][0], vsConstants_);
        }

        ~StateGuard()
        {
            // Targets first: binding render target 0 resets the viewport to its size. Target 0 can never
            // be unbound; the others are put back exactly, null included.
            for (DWORD t = 0; t < kTargets; ++t)
                if (t > 0 || rt_[0]) d_->SetRenderTarget(t, rt_[t]);
            d_->SetDepthStencilSurface(ds_);
            d_->SetViewport(&vp_);
            d_->SetScissorRect(&scissor_);
            if (psConstants_) d_->SetPixelShaderConstantF(0, &psConsts_[0][0], psConstants_);
            if (vsConstants_) d_->SetVertexShaderConstantF(0, &vsConsts_[0][0], vsConstants_);
            for (size_t i = 0; i < kStateCount; ++i) d_->SetRenderState(kStates[i], states_[i]);
            for (DWORD s = 0; s < stages_; ++s)
            {
                for (size_t i = 0; i < kSamplerCount; ++i) d_->SetSamplerState(s, kSamplers[i], samp_[s][i]);
                d_->SetTexture(s, tex_[s]);
            }
            d_->SetVertexShader(vs_);
            d_->SetPixelShader(ps_);
            if (decl_) d_->SetVertexDeclaration(decl_);
            else       d_->SetFVF(fvf_);
            // An UP draw unbinds stream 0 and the indices; the engine does not expect to rebind them.
            d_->SetStreamSource(0, vb_, vbOffset_, vbStride_);
            d_->SetIndices(ib_);

            for (DWORD t = 0; t < kTargets; ++t) if (rt_[t]) rt_[t]->Release();
            for (DWORD s = 0; s < stages_; ++s) if (tex_[s]) tex_[s]->Release();
            if (ds_)   ds_->Release();
            if (vs_)   vs_->Release();
            if (ps_)   ps_->Release();
            if (decl_) decl_->Release();
            if (vb_)   vb_->Release();
            if (ib_)   ib_->Release();
        }

        StateGuard(const StateGuard&) = delete;
        StateGuard& operator=(const StateGuard&) = delete;

    private:
        static constexpr D3DRENDERSTATETYPE kStates[] = {
            D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHATESTENABLE, D3DRS_ALPHAREF,
            D3DRS_ALPHAFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP,
            D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA,
            D3DRS_BLENDFACTOR, D3DRS_CULLMODE, D3DRS_FILLMODE, D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE,
            D3DRS_COLORWRITEENABLE, D3DRS_COLORWRITEENABLE1, D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3,
            D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
        };
        static constexpr D3DSAMPLERSTATETYPE kSamplers[] = {
            D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_ADDRESSW, D3DSAMP_BORDERCOLOR, D3DSAMP_MAGFILTER,
            D3DSAMP_MINFILTER, D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL,
            D3DSAMP_MAXANISOTROPY, D3DSAMP_SRGBTEXTURE,
        };
        static constexpr size_t kStateCount   = sizeof(kStates) / sizeof(kStates[0]);
        static constexpr size_t kSamplerCount = sizeof(kSamplers) / sizeof(kSamplers[0]);

        IDirect3DDevice9*            d_;
        UINT                         psConstants_, vsConstants_;
        DWORD                        stages_;
        IDirect3DSurface9*           rt_[kTargets] = {};
        IDirect3DSurface9*           ds_ = nullptr;
        D3DVIEWPORT9                 vp_{};
        RECT                         scissor_{};
        IDirect3DVertexShader9*      vs_ = nullptr;
        IDirect3DPixelShader9*       ps_ = nullptr;
        IDirect3DVertexDeclaration9* decl_ = nullptr;
        DWORD                        fvf_ = 0;
        IDirect3DVertexBuffer9*      vb_ = nullptr;
        UINT                         vbOffset_ = 0, vbStride_ = 0;
        IDirect3DIndexBuffer9*       ib_ = nullptr;
        IDirect3DBaseTexture9*       tex_[kMaxStages] = {};
        DWORD                        samp_[kMaxStages][kSamplerCount] = {};
        DWORD                        states_[kStateCount] = {};
        float                        psConsts_[kMaxPsConstants][4] = {};
        float                        vsConsts_[kMaxVsConstants][4] = {};
    };

    /// Binds tex on stage with clamp (or wrap) addressing, point or bilinear filtering, optional mips.
    inline void Sampler(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* tex, bool linear, bool wrap = false,
                        bool mips = false)
    {
        const DWORD address = wrap ? D3DTADDRESS_WRAP : D3DTADDRESS_CLAMP;
        const DWORD filter  = linear ? D3DTEXF_LINEAR : D3DTEXF_POINT;
        d->SetTexture(stage, tex);
        d->SetSamplerState(stage, D3DSAMP_ADDRESSU, address);
        d->SetSamplerState(stage, D3DSAMP_ADDRESSV, address);
        d->SetSamplerState(stage, D3DSAMP_ADDRESSW, address);
        d->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
        d->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
        d->SetSamplerState(stage, D3DSAMP_MIPFILTER, mips ? D3DTEXF_LINEAR : D3DTEXF_NONE);
        d->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, 0);
        d->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, 0);
        d->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
    }

    /// A clip-space quad over a w x h viewport, for a vertex shader that passes POSITION through (FVF
    /// XYZW must be set, as PlainState does). The service's DrawFullscreen does the same and binds
    /// its own pass-through shader.
    inline void Quad(IDirect3DDevice9* d, UINT w, UINT h)
    {
        const D3DVIEWPORT9 vp{ 0, 0, w, h, 0.0f, 1.0f };
        d->SetViewport(&vp);
        static const float kQuad[4][4] = {
            { -1.0f,  1.0f, 0.0f, 1.0f },
            {  1.0f,  1.0f, 0.0f, 1.0f },
            { -1.0f, -1.0f, 0.0f, 1.0f },
            {  1.0f, -1.0f, 0.0f, 1.0f },
        };
        d->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, kQuad, sizeof kQuad[0]);
    }

    /// The plain state a full-screen pass draws with: no depth, stencil, blend, fog or clipping, every
    /// channel written, FVF XYZW for Quad / DrawFullscreen.
    inline void PlainState(IDirect3DDevice9* d)
    {
        d->SetRenderState(D3DRS_ZENABLE, FALSE);
        d->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        d->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        d->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        d->SetRenderState(D3DRS_STENCILENABLE, FALSE);
        d->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        d->SetRenderState(D3DRS_FOGENABLE, FALSE);
        d->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        d->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        d->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
        d->SetFVF(D3DFVF_XYZW);
    }

    /// Additive blending (dest += src) on top of PlainState, for passes that add light.
    inline void AdditiveBlend(IDirect3DDevice9* d)
    {
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    }

    /// Premultiplied-alpha blending (dest = src + dest * (1 - src.a)) on top of PlainState.
    inline void PremultipliedBlend(IDirect3DDevice9* d)
    {
        d->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        d->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        d->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        d->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    }
}
