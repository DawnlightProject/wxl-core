// wxl-graphics-extend: the pass-through vertex shader of the full-screen quad (DrawFullscreen).
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

// The quad's vertices arrive in clip space already (D3DFVF_XYZW, w = 1). The service normally
// uses a hand-assembled token stream for this and compiles this file only when a device refuses it.
float4 main(float4 p : POSITION) : POSITION
{
    return p;
}
