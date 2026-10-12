/*
 * Copyright 2014 Advanced Micro Devices, Inc.
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

// Linux 6.18.52 gfx_v8_0_tiling_mode_table_init, CHIP_POLARIS10.
// Values are evaluated from that source and gfx_8_0 register definitions.
#ifndef AMDGPU_POLARIS_TILING_H
#define AMDGPU_POLARIS_TILING_H

#include <stdint.h>

static const uint32_t kPolaris10TileMode[32] = {
	0x00800310, 0x00800b10, 0x00801310, 0x00801b10,
	0x00802b10, 0x00802b08, 0x00802b14, 0x00802954,
	0x00000304, 0x02000308, 0x02000310, 0x06000314,
	0x06000154, 0x02400308, 0x02400310, 0x02400330,
	0x06400314, 0x06400154, 0x0040030c, 0x0100030c,
	0x0100031c, 0x01000334, 0x01000324, 0x01000164,
	0x0040031c, 0x01000320, 0x01000338, 0x02c00308,
	0x02c00310, 0x06c00314, 0x06c00154, 0x00000000,
};

static const uint32_t kPolaris10MacrotileMode[16] = {
	0x000000e8, 0x000000e8, 0x000000e8, 0x000000e8,
	0x000000d4, 0x000000c0, 0x000000c0, 0x00000000,
	0x000000ec, 0x000000e8, 0x000000d4, 0x000000d0,
	0x00000080, 0x00000040, 0x00000040, 0x00000000,
};

// GB_MACROTILE_MODE7 is reserved: Linux intentionally leaves it untouched.
#endif
