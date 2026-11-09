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
			*_min = 0;
			*_max = 0;
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

	// A scale so small that the bounds do not fit an int32 is refused, as
	// the reference does.
	float quantizedLow = roundf(low / output.scale);
	if (!(fabsf(quantizedLow) < 1e9f))
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	int64 lowest = (int64)output.zero_point + (int32)quantizedLow;
	*_min = lowest > min ? (lowest < max ? (int32)lowest : max) : min;
	if (isinf(high)) {
		*_max = max;
	} else {
		float quantizedHigh = roundf(high / output.scale);
		if (!(fabsf(quantizedHigh) < 1e9f))
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		int64 highest = (int64)output.zero_point + (int32)quantizedHigh;
		*_max = highest < max ? (highest > min ? (int32)highest : min) : max;
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
	if (stride <= 0 || dilation <= 0 || filterSize <= 0 || inputSize <= 0)
		return 0;
	int64 effective = (int64)(filterSize - 1) * dilation + 1;
	int64 size;
	if (padding == B_NEURAL_PADDING_SAME)
		size = ((int64)inputSize + stride - 1) / stride;
	else
		size = ((int64)inputSize + stride - effective) / stride;
	return size > 0 ? (int32)size : 0;
}


static inline int32
neural_padding_before(int32 stride, int32 dilation, int32 inputSize,
	int32 filterSize, int32 outputSize)
{
	int64 effective = (int64)(filterSize - 1) * dilation + 1;
	int64 total = (int64)(outputSize - 1) * stride + effective - inputSize;
	return total > 0 ? (int32)(total / 2) : 0;
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


// #pragma mark - what every prepare() checks


//! The most elements of a tensor the kernels index with an int32.
static const int32 kNeuralMaxElements = 1 << 30;


/*!	Number of elements of a tensor, or -1 for more than the kernels can
	index. */
static inline int32
neural_element_count(const neural_graph_tensor& tensor)
{
	int64 count = NeuralGraphData::CountElements(tensor);
	if (count < 0 || count > kNeuralMaxElements)
		return -1;
	return (int32)count;
}


//! Whether the operator has its first \a count inputs.
static inline bool
neural_has_inputs(const neural_graph_operator* op, int32 count)
{
	if (op->input_count < count)
		return false;
	for (int32 i = 0; i < count; i++) {
		if (op->inputs[i] < 0)
			return false;
	}
	return true;
}


static inline bool
neural_has_input(const neural_graph_operator* op, int32 index)
{
	return index < op->input_count && op->inputs[index] >= 0;
}


static inline bool
neural_is_constant(const neural_graph_tensor& tensor)
{
	return tensor.data != NULL;
}


static inline bool
neural_is_quantized_8(const neural_graph_tensor& tensor)
{
	return (tensor.type == B_NEURAL_UINT8 || tensor.type == B_NEURAL_INT8)
		&& tensor.quantized && tensor.scale > 0 && isfinite(tensor.scale);
}


/*!	Whether the zero point is a value the type can hold; a quantized
	tensor for which it is not makes no sense and would take the
	arithmetic out of the ranges the kernels count on. */
static inline bool
neural_valid_zero_point(const neural_graph_tensor& tensor)
{
	int32 min, max;
	if (!neural_type_range(tensor.type, &min, &max))
		return false;
	return tensor.zero_point >= min && tensor.zero_point <= max;
}


/*!	Whether the stored values of two tensors mean the same, for operators
	that pass them on as they are. Scales that differ in their last digits
	count as the same: converters leave such pairs behind, and the
	reference does not look at all. */
static inline bool
neural_same_quantization(const neural_graph_tensor& a,
	const neural_graph_tensor& b)
{
	if (a.type != b.type || a.quantized != b.quantized
		|| a.zero_point != b.zero_point) {
		return false;
	}
	float difference = fabsf(a.scale - b.scale);
	return difference <= 1e-6f
		|| difference <= 1e-5f * (a.scale > b.scale ? a.scale : b.scale);
}


/*!	Reads element \a index of a constant tensor of whole numbers (int32
	or int64). */
static inline int64
neural_constant_int(const neural_graph_tensor& tensor, int32 index)
{
	if (tensor.type == B_NEURAL_INT64)
		return ((const int64*)tensor.data)[index];
	return ((const int32*)tensor.data)[index];
}


static inline bool
neural_is_index_type(neural_data_type type)
{
	return type == B_NEURAL_INT32 || type == B_NEURAL_INT64;
}


// #pragma mark - prepared data


/*!	Lays out the single block a prepare() returns: a structure followed
	by arrays, each aligned to 16 bytes. Reserve() everything, Allocate()
	once, then ask for the arrays by the offsets Reserve() gave. */
class NeuralBlock {
public:
	NeuralBlock(size_t headerSize)
		:
		fSize(headerSize),
		fBlock(NULL)
	{
	}

	size_t Reserve(size_t count, size_t elementSize)
	{
		fSize = (fSize + 15) & ~(size_t)15;
		size_t offset = fSize;
		fSize += count * elementSize;
		return offset;
	}

	//! Zeroed; NULL when there is no memory.
	void* Allocate()
	{
		fBlock = (uint8*)calloc(fSize > 0 ? fSize : 1, 1);
		return fBlock;
	}

	template<typename Type> Type* At(size_t offset) const
	{
		return (Type*)(fBlock + offset);
	}

private:
	size_t	fSize;
	uint8*	fBlock;
};


/*!	The memory a run needs for the time being: several arrays in one
	block, each starting at a multiple of 64 bytes (what a processor moves
	between memory and its caches at a time; an array that starts in the
	middle of that is slower to walk). Reserve() everything, Allocate()
	once, then ask for the arrays by the offsets Reserve() gave. The block
	goes with the object. */
class NeuralScratch {
public:
	NeuralScratch()
		:
		fSize(0),
		fBlock(NULL),
		fBase(NULL)
	{
	}

	~NeuralScratch()
	{
		free(fBlock);
	}

	size_t Reserve(size_t count, size_t elementSize)
	{
		size_t offset = fSize;
		fSize += (count * elementSize + 63) & ~(size_t)63;
		return offset;
	}

	//! Zeroed; false when there is no memory.
	bool Allocate()
	{
		fBlock = (uint8*)calloc(fSize + 64, 1);
		if (fBlock == NULL)
			return false;
		fBase = fBlock + (64 - ((addr_t)fBlock & 63)) % 64;
		return true;
	}

	template<typename Type> Type* At(size_t offset) const
	{
		return (Type*)(fBase + offset);
	}

private:
	size_t	fSize;
	uint8*	fBlock;
	uint8*	fBase;
};


// #pragma mark - arithmetic


// Written as two steps, each of which is one instruction without a jump
// (a jump on data as random as the signs of activations is guessed wrong
// half of the time). Something that is not a number stays what it is.

static inline int32
neural_clamp(int32 value, int32 min, int32 max)
{
	value = value < min ? min : value;
	return value > max ? max : value;
}


static inline float
neural_clamp(float value, float min, float max)
{
	value = value < min ? min : value;
	return value > max ? max : value;
}


/*!	neural_multiply_by_quantized_multiplier() for a factor and a shift
	that stay the same over a loop, taken apart beforehand and done in
	steps that compilers make vector instructions of (the function above
	they do one number at a time). The results are the same, provided that
	the factor is not negative and that the value, shifted left, stays
	below 2^30 in magnitude (or is not shifted right at all).

	The steps: a product of signed numbers rounded to the nearest (halves
	up) is floor((x * m + 2^30) / 2^31); with x + 2^31, which is not
	negative, in place of x that is a product of unsigned numbers which
	comes out larger by exactly m. The shift to the right rounds halves
	away from zero: add half, less one for negative numbers.
*/
struct neural_scaling {
	uint32	multiplier;
	int32	left;
	int32	right;
	int32	half;
	int32	sign_mask;
};


static inline void
neural_set_scaling(neural_scaling& scaling, int32 multiplier, int shift)
{
	scaling.multiplier = (uint32)multiplier;
	scaling.left = shift > 0 ? shift : 0;
	scaling.right = shift > 0 ? 0 : -shift;
	scaling.half = scaling.right > 0 ? 1 << (scaling.right - 1) : 0;
	scaling.sign_mask = scaling.right > 0 ? -1 : 0;
}


static inline int32
neural_scale(int32 value, const neural_scaling& scaling)
{
	const uint32 kSign = 0x80000000;
	uint32 shifted = ((uint32)value << scaling.left) ^ kSign;
	int32 scaled = (int32)((uint32)(((uint64)shifted * scaling.multiplier
		+ ((uint32)1 << 30)) >> 31) - scaling.multiplier);
	return (int32)((uint32)scaled + (uint32)scaling.half
		+ (uint32)((scaled >> 31) & scaling.sign_mask)) >> scaling.right;
}


/*!	A real multiplier as fixed point, refused when it is not one the
	fixed-point helpers above can take (negative, not a number, or too
	large to shift). */
static inline bool
neural_checked_quantize_multiplier(double multiplier, int32* _factor,
	int* _shift)
{
	if (!(multiplier >= 0) || !isfinite(multiplier))
		return false;
	neural_quantize_multiplier(multiplier, _factor, _shift);
	return *_shift <= 30 && *_shift >= -31;
}


// #pragma mark - windows (convolution, pooling)


//! What a window that slides over a picture works on, checked.
struct neural_window {
	int32	batches;
	int32	input_height;
	int32	input_width;
	int32	input_channels;
	int32	output_height;
	int32	output_width;
	int32	output_channels;
	int32	pad_top;
	int32	pad_left;
};


/*!	Checks that input and output are pictures (batch, height, width,
	channels) and that the output has the size the window, its steps and
	the padding give. */
static inline bool
neural_window_geometry(const neural_graph_tensor& input,
	const neural_graph_tensor& output, neural_padding padding,
	int32 filterHeight, int32 filterWidth, int32 strideHeight,
	int32 strideWidth, int32 dilationHeight, int32 dilationWidth,
	neural_window* window)
{
	if (input.rank != 4 || output.rank != 4
		|| input.dimensions[0] != output.dimensions[0]
		|| (padding != B_NEURAL_PADDING_SAME
			&& padding != B_NEURAL_PADDING_VALID)
		|| filterHeight < 1 || filterWidth < 1
		|| strideHeight < 1 || strideHeight > SHRT_MAX
		|| strideWidth < 1 || strideWidth > SHRT_MAX
		|| dilationHeight < 1 || dilationHeight > SHRT_MAX
		|| dilationWidth < 1 || dilationWidth > SHRT_MAX
		|| neural_element_count(input) < 0
		|| neural_element_count(output) < 0) {
		return false;
	}

	window->batches = input.dimensions[0];
	window->input_height = input.dimensions[1];
	window->input_width = input.dimensions[2];
	window->input_channels = input.dimensions[3];
	window->output_height = output.dimensions[1];
	window->output_width = output.dimensions[2];
	window->output_channels = output.dimensions[3];

	if (window->output_height != neural_output_size(padding,
			window->input_height, filterHeight, strideHeight,
			dilationHeight)
		|| window->output_width != neural_output_size(padding,
			window->input_width, filterWidth, strideWidth, dilationWidth)) {
		return false;
	}

	window->pad_top = neural_padding_before(strideHeight, dilationHeight,
		window->input_height, filterHeight, window->output_height);
	window->pad_left = neural_padding_before(strideWidth, dilationWidth,
		window->input_width, filterWidth, window->output_width);
	return true;
}


// #pragma mark - broadcasting


/*!	How two tensors of different shapes are walked together, NumPy style:
	a dimension of size 1 is repeated to the size of the other tensor.
	Neighbouring dimensions that behave alike are taken together, so that
	the last one, which the kernels loop over, is as long as can be.
*/
struct neural_broadcast {
	int32	rank;
	//! size of the output in each (merged) dimension
	int32	size[B_NEURAL_MAX_RANK];
	//! elements to step in each input when the index of a dimension grows
	int32	step1[B_NEURAL_MAX_RANK];
	int32	step2[B_NEURAL_MAX_RANK];
	//! product of all sizes but the last
	int32	outer_count;
};


/*!	Fills in \a broadcast for the two inputs, and checks that \a output
	has the shape they give together. */
static inline bool
neural_prepare_broadcast(const neural_graph_tensor& input1,
	const neural_graph_tensor& input2, const neural_graph_tensor& output,
	neural_broadcast* broadcast)
{
	int32 rank = input1.rank > input2.rank ? input1.rank : input2.rank;
	if (output.rank != rank || neural_element_count(input1) < 0
		|| neural_element_count(input2) < 0
		|| neural_element_count(output) < 0) {
		return false;
	}

	// per merged dimension: the size of each input in it
	int32 size1[B_NEURAL_MAX_RANK];
	int32 size2[B_NEURAL_MAX_RANK];
	int32 merged = 0;
	int kind = 0;
		// of the dimension being merged into: 0 no input repeated (or
		// both of size 1), 1 the first repeated, 2 the second

	for (int32 i = 0; i < rank; i++) {
		int32 index1 = i - (rank - input1.rank);
		int32 index2 = i - (rank - input2.rank);
		int32 dimension1 = index1 >= 0 ? input1.dimensions[index1] : 1;
		int32 dimension2 = index2 >= 0 ? input2.dimensions[index2] : 1;
		if (dimension1 != dimension2 && dimension1 != 1 && dimension2 != 1)
			return false;

		int32 size = dimension1 > dimension2 ? dimension1 : dimension2;
		if (output.dimensions[i] != size)
			return false;
		if (size == 1)
			continue;

		int thisKind = dimension1 == dimension2 ? 0 : (dimension1 == 1 ? 1 : 2);
		if (merged > 0 && thisKind == kind) {
			size1[merged - 1] *= dimension1;
			size2[merged - 1] *= dimension2;
		} else {
			size1[merged] = dimension1;
			size2[merged] = dimension2;
			merged++;
			kind = thisKind;
		}
	}

	if (merged == 0) {
		size1[0] = size2[0] = 1;
		merged = 1;
	}

	broadcast->rank = merged;
	broadcast->outer_count = 1;
	int32 step1 = 1;
	int32 step2 = 1;
	for (int32 i = merged - 1; i >= 0; i--) {
		broadcast->size[i] = size1[i] > size2[i] ? size1[i] : size2[i];
		broadcast->step1[i] = size1[i] == broadcast->size[i] ? step1 : 0;
		broadcast->step2[i] = size2[i] == broadcast->size[i] ? step2 : 0;
		step1 *= size1[i];
		step2 *= size2[i];
		if (i < merged - 1)
			broadcast->outer_count *= broadcast->size[i];
	}
	return true;
}


/*!	Where the row of the last dimension with number \a outer starts in
	each input. */
static inline void
neural_broadcast_offsets(const neural_broadcast& broadcast, int32 outer,
	int32* _offset1, int32* _offset2)
{
	int32 offset1 = 0;
	int32 offset2 = 0;
	for (int32 i = broadcast.rank - 2; i >= 0; i--) {
		int32 index = outer % broadcast.size[i];
		outer /= broadcast.size[i];
		offset1 += index * broadcast.step1[i];
		offset2 += index * broadcast.step2[i];
	}
	*_offset1 = offset1;
	*_offset2 = offset2;
}


// #pragma mark - floating point


/*!	While one of these exists in a thread, numbers too small to be stored
	normally ("denormal", below about 1e-38) are taken as zero in it.
	Networks are full of weights and activations that have decayed to next
	to nothing, and x86 processors compute with such numbers a hundred
	times slower than with others: a layer can take ten times as long for
	some inputs. The results differ by less than can be told from zero.
	(This is the one place where the kernels do something only for one
	kind of processor; others handle these numbers at speed.)
*/
class NeuralFlushDenormals {
public:
	NeuralFlushDenormals()
	{
#if defined(__SSE2__)
		// "flush to zero" for results and "denormals are zero" for
		// operands, in the control register of the SSE unit
		fSaved = __builtin_ia32_stmxcsr();
		__builtin_ia32_ldmxcsr(fSaved | 0x8040);
#endif
	}

	~NeuralFlushDenormals()
	{
#if defined(__SSE2__)
		__builtin_ia32_ldmxcsr(fSaved);
#endif
	}

private:
#if defined(__SSE2__)
	unsigned int	fSaved;
#endif
};


}	// namespace BPrivate


#endif	// _NEURAL_KERNEL_UTILS_H
