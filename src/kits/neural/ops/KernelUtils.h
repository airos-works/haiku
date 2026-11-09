/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _NEURAL_KERNEL_UTILS_H
#define _NEURAL_KERNEL_UTILS_H


/*!	What the kernels share: the fixed-point arithmetic of quantized
	networks (the same as TensorFlow Lite's reference kernels and gemmlowp
	use, so that results agree to the bit), activation ranges and padding.
*/


#include <NeuralInterpreter.h>

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


namespace BPrivate {


//! Rounds half away from zero, as round() does.
static inline int32
neural_round(double value)
{
	return (int32)round(value);
}


/*!	Splits a positive real multiplier into a Q31 factor and a power of
	two: multiplier = factor / 2^31 * 2^shift. */
static inline void
neural_quantize_multiplier(double multiplier, int32* _factor, int* _shift)
{
	if (multiplier == 0) {
		*_factor = 0;
		*_shift = 0;
		return;
	}

	double fraction = frexp(multiplier, _shift);
	int64 fixed = (int64)round(fraction * (double)((int64)1 << 31));
	if (fixed == ((int64)1 << 31)) {
		fixed /= 2;
		++*_shift;
	}
	if (*_shift < -31) {
		*_shift = 0;
		fixed = 0;
	}
	*_factor = (int32)fixed;
}


static inline int32
neural_saturating_rounding_doubling_high_mul(int32 a, int32 b)
{
	bool overflow = a == b && a == INT_MIN;
	int64 product = (int64)a * (int64)b;
	int32 nudge = product >= 0 ? (1 << 30) : (1 - (1 << 30));
	int32 result = (int32)((product + nudge) / ((int64)1 << 31));
	return overflow ? INT_MAX : result;
}


static inline int32
neural_rounding_divide_by_pot(int32 value, int exponent)
{
	int32 mask = (int32)(((int64)1 << exponent) - 1);
	int32 remainder = value & mask;
	int32 threshold = (mask >> 1) + (value < 0 ? 1 : 0);
	return (value >> exponent) + (remainder > threshold ? 1 : 0);
}


//! value * factor / 2^31 * 2^shift, rounded as the reference does.
static inline int32
neural_multiply_by_quantized_multiplier(int32 value, int32 factor, int shift)
{
	int leftShift = shift > 0 ? shift : 0;
	int rightShift = shift > 0 ? 0 : -shift;
	return neural_rounding_divide_by_pot(
		neural_saturating_rounding_doubling_high_mul(
			(int32)((uint32)value << leftShift), factor), rightShift);
}


//! The stored values a quantized type can hold.
static inline bool
neural_type_range(neural_data_type type, int32* _min, int32* _max)
{
	switch (type) {
		case B_NEURAL_UINT8:
			*_min = 0;
			*_max = 255;
			return true;
		case B_NEURAL_INT8:
			*_min = -128;
			*_max = 127;
			return true;
		case B_NEURAL_INT16:
			*_min = -32768;
			*_max = 32767;
			return true;
		default:
			return false;
	}
}


/*!	The range of stored values an activation leaves, for a quantized
	output tensor. */
static inline status_t
neural_quantized_activation_range(neural_activation activation,
	const neural_graph_tensor& output, int32* _min, int32* _max)
{
	int32 min, max;
	if (!neural_type_range(output.type, &min, &max) || !output.quantized
		|| output.scale <= 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	float low, high;
	switch (activation) {
		case B_NEURAL_ACTIVATION_NONE:
			*_min = min;
			*_max = max;
			return B_OK;
		case B_NEURAL_ACTIVATION_RELU:
			low = 0;
			high = INFINITY;
			break;
		case B_NEURAL_ACTIVATION_RELU6:
			low = 0;
			high = 6;
			break;
		case B_NEURAL_ACTIVATION_RELU_N1_TO_1:
			low = -1;
			high = 1;
			break;
		default:
			return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	int32 quantizedLow = output.zero_point
		+ neural_round(low / output.scale);
	*_min = quantizedLow > min ? quantizedLow : min;
	if (isinf(high)) {
		*_max = max;
	} else {
		int32 quantizedHigh = output.zero_point
			+ neural_round(high / output.scale);
		*_max = quantizedHigh < max ? quantizedHigh : max;
	}
	return B_OK;
}


static inline status_t
neural_float_activation_range(neural_activation activation, float* _min,
	float* _max)
{
	switch (activation) {
		case B_NEURAL_ACTIVATION_NONE:
			*_min = -INFINITY;
			*_max = INFINITY;
			return B_OK;
		case B_NEURAL_ACTIVATION_RELU:
			*_min = 0;
			*_max = INFINITY;
			return B_OK;
		case B_NEURAL_ACTIVATION_RELU6:
			*_min = 0;
			*_max = 6;
			return B_OK;
		case B_NEURAL_ACTIVATION_RELU_N1_TO_1:
			*_min = -1;
			*_max = 1;
			return B_OK;
		default:
			return B_NEURAL_UNSUPPORTED_OPERATOR;
	}
}


/*!	Output size of a windowed operator along one dimension and the
	padding before the first element. */
static inline int32
neural_output_size(neural_padding padding, int32 inputSize, int32 filterSize,
	int32 stride, int32 dilation)
{
	if (stride <= 0)
		return 0;
	int32 effective = (filterSize - 1) * dilation + 1;
	if (padding == B_NEURAL_PADDING_SAME)
		return (inputSize + stride - 1) / stride;
	return (inputSize + stride - effective) / stride;
}


static inline int32
neural_padding_before(int32 stride, int32 dilation, int32 inputSize,
	int32 filterSize, int32 outputSize)
{
	int32 effective = (filterSize - 1) * dilation + 1;
	int32 total = (outputSize - 1) * stride + effective - inputSize;
	return total > 0 ? total / 2 : 0;
}


static inline bool
neural_same_shape(const neural_graph_tensor& a, const neural_graph_tensor& b)
{
	if (a.rank != b.rank)
		return false;
	for (int32 i = 0; i < a.rank; i++) {
		if (a.dimensions[i] != b.dimensions[i])
			return false;
	}
	return true;
}


}	// namespace BPrivate


#endif	// _NEURAL_KERNEL_UTILS_H
