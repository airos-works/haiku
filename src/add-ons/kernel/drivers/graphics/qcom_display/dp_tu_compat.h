/*
 * Copyright 2009 Red Hat Inc.
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
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Dave Airlie
 *          Christian König
 */

/*	What dp_tu.c needs of Linux: the fixed point helpers of
	include/drm/drm_fixed.h (Linux 6.18.52, MIT, unchanged) and the few
	kernel types and macros they and it use. */
#ifndef QCOM_DP_TU_COMPAT_H
#define QCOM_DP_TU_COMPAT_H


#include <stdbool.h>
#include <stdlib.h>

#include <SupportDefs.h>


typedef int64 s64;
typedef uint64 u64;
typedef uint32 u32;
typedef uint16 u16;
typedef uint8 u8;
typedef unsigned int uint;

#define GFP_KERNEL				0
#define kzalloc(size, flags)	calloc(1, (size))
#define kfree(pointer)			free(pointer)
#ifndef min
#define min(a, b)				((a) < (b) ? (a) : (b))
#endif
#define drm_dbg_dp(device, format...)	do {} while (0)
#define pr_info(format...)		do {} while (0)

// only the debug output named it
struct msm_dp_ctrl_private;

static inline s64
div64_s64(s64 dividend, s64 divisor)
{
	return dividend / divisor;
}

static inline u64
div64_u64_rem_(u64 dividend, u64 divisor, u64* remainder)
{
	*remainder = dividend % divisor;
	return dividend / divisor;
}
// dp_tu.c also hands it an s64 remainder
#define div64_u64_rem(dividend, divisor, remainder) 	div64_u64_rem_((dividend), (divisor), (u64*)(remainder))


#define DRM_FIXED_POINT		32
#define DRM_FIXED_ONE		(1ULL << DRM_FIXED_POINT)
#define DRM_FIXED_DECIMAL_MASK	(DRM_FIXED_ONE - 1)
#define DRM_FIXED_DIGITS_MASK	(~DRM_FIXED_DECIMAL_MASK)
#define DRM_FIXED_EPSILON	1LL
#define DRM_FIXED_ALMOST_ONE	(DRM_FIXED_ONE - DRM_FIXED_EPSILON)

static inline s64 drm_int2fixp(int a)
{
	return ((s64)a) << DRM_FIXED_POINT;
}

static inline int drm_fixp2int(s64 a)
{
	return ((s64)a) >> DRM_FIXED_POINT;
}

static inline int drm_fixp2int_round(s64 a)
{
	return drm_fixp2int(a + DRM_FIXED_ONE / 2);
}

static inline int drm_fixp2int_ceil(s64 a)
{
	if (a >= 0)
		return drm_fixp2int(a + DRM_FIXED_ALMOST_ONE);
	else
		return drm_fixp2int(a - DRM_FIXED_ALMOST_ONE);
}

static inline unsigned drm_fixp_msbset(s64 a)
{
	unsigned shift, sign = (a >> 63) & 1;

	for (shift = 62; shift > 0; --shift)
		if (((a >> shift) & 1) != sign)
			return shift;

	return 0;
}

static inline s64 drm_fixp_mul(s64 a, s64 b)
{
	unsigned shift = drm_fixp_msbset(a) + drm_fixp_msbset(b);
	s64 result;

	if (shift > 61) {
		shift = shift - 61;
		a >>= (shift >> 1) + (shift & 1);
		b >>= shift >> 1;
	} else
		shift = 0;

	result = a * b;

	if (shift > DRM_FIXED_POINT)
		return result << (shift - DRM_FIXED_POINT);

	if (shift < DRM_FIXED_POINT)
		return result >> (DRM_FIXED_POINT - shift);

	return result;
}

static inline s64 drm_fixp_div(s64 a, s64 b)
{
	unsigned shift = 62 - drm_fixp_msbset(a);
	s64 result;

	a <<= shift;

	if (shift < DRM_FIXED_POINT)
		b >>= (DRM_FIXED_POINT - shift);

	result = div64_s64(a, b);

	if (shift > DRM_FIXED_POINT)
		return result >> (shift - DRM_FIXED_POINT);

	return result;
}

static inline s64 drm_fixp_from_fraction(s64 a, s64 b)
{
	s64 res;
	bool a_neg = a < 0;
	bool b_neg = b < 0;
	u64 a_abs = a_neg ? -a : a;
	u64 b_abs = b_neg ? -b : b;
	u64 rem;

	/* determine integer part */
	u64 res_abs  = div64_u64_rem(a_abs, b_abs, &rem);

	/* determine fractional part */
	{
		u32 i = DRM_FIXED_POINT;

		do {
			rem <<= 1;
			res_abs <<= 1;
			if (rem >= b_abs) {
				res_abs |= 1;
				rem -= b_abs;
			}
		} while (--i != 0);
	}

	/* round up LSB */
	{
		u64 summand = (rem << 1) >= b_abs;

		res_abs += summand;
	}

	res = (s64) res_abs;
	if (a_neg ^ b_neg)
		res = -res;
	return res;
}


#endif	/* QCOM_DP_TU_COMPAT_H */
