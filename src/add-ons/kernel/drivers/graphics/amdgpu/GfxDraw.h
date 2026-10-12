/*
 * Copyright 2014, 2022 Advanced Micro Devices, Inc.
 * Copyright 2026, air/OS.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// Fixed GFX8 offscreen rasterization fixture. Register definitions are from
// gfx_8_0_{d,sh_mask}.h; state setup follows Mesa GFX6-8 and libdrm's draw test.
// No vertex buffers, descriptors, scratch, depth, MSAA, metadata or scanout.
#ifndef AMDGPU_GFX_DRAW_H
#define AMDGPU_GFX_DRAW_H

// gfx803: vertex ID in v0 selects (0,0), (32.5,0), (0,32.5).
// Window coordinates and half-integer pixel centers avoid edge ties.
// Assembly sources: src/tests/add-ons/kernel/drivers/amdgpu/shaders/.
static const uint32 kTriangleVS[] = {
	0x7d940081, 0x7e020280, 0x7e0402ff, 0x42020000,
	0x00020501, 0x7d940082, 0x7e040280, 0x7e0602ff,
	0x42020000, 0x00040702, 0x7e060280, 0x7e0802f2,
	0xc40008cf, 0x04030201, 0xbf810000,
};

static const uint32 kColorPS[] = {
	0x7e000200, 0x7e020201, 0x7e040202, 0x7e060203,
	0xc400180f, 0x03020100, 0xbf810000,
};

// All entries are GFX8 context registers. Shaders and framebuffer addresses
// are supplied separately by the kernel from its private 1 MiB allocation.
static const uint32 kDrawContext[][2] = {
	{0xa000, 0}, // DB_RENDER_CONTROL
	{0xa003, 0x2a}, // DB_RENDER_OVERRIDE: disable hierarchical depth/stencil
	{0xa004, 0},
	{0xa00c, 0}, // screen scissor (0,0)..(32,32)
	{0xa00d, 32 << 16 | 32},
	{0xa010, 0}, // no depth/stencil surface
	{0xa011, 0},
	{0xa080, 0}, // window offset
	{0xa081, 0x80000000},
	{0xa082, 32 << 16 | 32},
	{0xa083, 0xffff}, // clip rectangle rule
	{0xa08c, 0xaa99aaaa}, // PA_SC_EDGERULE, standard top-left coverage
	{0xa08d, 0},
	{0xa08e, 0xf}, // CB_TARGET_MASK, only MRT0
	{0xa08f, 0xf}, // CB_SHADER_MASK
	{0xa090, 0x80000000}, // generic scissor
	{0xa091, 32 << 16 | 32},
	{0xa094, 0x80000000}, // viewport scissor
	{0xa095, 32 << 16 | 32},
	{0xa0b4, 0}, // viewport depth range
	{0xa0b5, 0x3f800000},
	{0xa0da, 0}, // CP_VMID: direct kernel rendering
	{0xa103, 0}, // primitive restart index (restart disabled)
	{0xa109, 0}, // CB_DCC_CONTROL: no compression
	{0xa1b1, 0}, // SPI_VS_OUT_CONFIG, no interpolated attributes
	{0xa1b3, 2}, // PS requires at least one enabled barycentric input
	{0xa1b4, 2},
	{0xa1b5, 0},
	{0xa1b6, 0},
	{0xa1b8, 0},
	{0xa1c3, 4}, // POS0: four 32-bit floats
	{0xa1c4, 0}, // no depth export
	{0xa1c5, 9}, // MRT0: four 32-bit floats
	{0xa1e0, 0}, // CB_BLEND0_CONTROL: disabled
	{0xa200, 0}, // DB_DEPTH_CONTROL: disabled
	{0xa201, 0},
	{0xa202, 0xcc0010}, // CB_NORMAL, ROP copy
	{0xa203, 0x10}, // DB_SHADER_CONTROL: late Z, no depth/kill export
	{0xa204, 0x90000}, // clipping disabled for bounded window coordinates
	{0xa205, 0}, // no face culling
	{0xa206, 0x300}, // pretransformed XY/Z, no viewport transform
	{0xa207, 0},
	{0xa208, 0},
	{0xa290, 0}, // GS off
	{0xa292, 0x20},
	{0xa293, 0x060201b8},
	{0xa2a1, 0}, // primitive ID off
	{0xa2ad, 0},
	{0xa2ae, 0},
	{0xa2d5, 0}, // VS only, no tessellation or GS
	{0xa2dc, 0}, // DB_ALPHA_TO_MASK: disabled
	{0xa2e5, 0}, // streamout off
	{0xa2e6, 0},
	{0xa2f5, 0}, // centroid priorities
	{0xa2f6, 0},
	{0xa2f8, 0}, // no multisampling
	{0xa2f9, 5}, // half-integer centers, round to even, 1/16 precision
	{0xa2fa, 0x3f800000},
	{0xa2fb, 0x3f800000},
	{0xa2fc, 0x3f800000},
	{0xa2fd, 0x3f800000},
	{0xa30e, 0xffffffff}, // sample masks
	{0xa30f, 0xffffffff},
};
#endif
