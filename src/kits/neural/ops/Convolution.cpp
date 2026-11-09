/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Convolutions and the fully connected layer: the operators nearly all
	the time of a network goes to.

	Quantized, they all come down to sums of products of a weight and an
	input, each less its zero point. Both then fit 16 bits, and a sum of
	products of 16 bit numbers is what compilers turn into the multiply-add
	instructions of SSE2 and NEON by themselves. So prepare() stores the
	weights as 16 bit numbers with the zero point taken off, and run()
	does the same to the inputs a row or a window at a time. A sum is then
	scaled to the output as the TensorFlow Lite reference kernels do it, to
	the bit: in fixed point for the convolutions, in double precision
	floating point for the fully connected layer (which is what the
	reference kernel of LiteRT 2 does there, unlike its optimized one).

	Floating point sums are made with explicit vectors of four numbers,
	since a compiler may not reorder the additions on its own.

	The pictures are split by output rows over the thread pool.
*/


// The loops below are written for the vectoriser, which -O2 holds back.
// Their starts are put at multiples of 32 bytes: where the few instructions
// that make up nearly all of the time lie relative to that changes their
// speed by a tenth on some processors (measured on a Xeon E5 v4), and
// otherwise depends on whatever code happens to come before.
#pragma GCC optimize("O3", "align-loops=32")


#include "Kernels.h"

#include <algorithm>


namespace BPrivate {


namespace {


struct ConvData {
	neural_window	window;
	int32			filter_height;
	int32			filter_width;
	int32			stride_height;
	int32			stride_width;
	int32			dilation_height;
	int32			dilation_width;
	//! convolution in groups: each filter sees this many input channels
	int32			groups;
	int32			filter_channels;
	//! numbers under a window: what one output channel sums over
	int32			patch_size;
	//! the same as the weights of an output channel are stored (padded)
	int32			row_length;
	//! depthwise: output channels per input channel
	int32			depth_multiplier;
	//! a 1x1 window: the input row itself is the list of patches
	bool			direct;
	bool			quantized;
	//! output positions whose sums are made together
	int32			tile;

	int32			input_offset;
	int32			output_offset;
	int32			activation_min;
	int32			activation_max;
	float			float_min;
	float			float_max;

	//! per output channel: weights less their zero point
	int16*			weights;
	int32*			bias;
	int32*			multipliers;
	int32*			shifts;
	/*!	The same factors as finish() wants them when every one of them
		scales down and no sum is near the end of 32 bits (which is always,
		but for models made to be odd): see there. */
	bool			fast_scaling;
	//! the shift to the right when all channels have the same, or -1
	int32			common_shift;
	uint32*			post_multipliers;
	int32*			rounding;
	int32*			sign_masks;
	/*!	Depthwise, one output channel per input channel: the weights of
		each tap repeated to \c wide_length numbers, so that the products
		of a tap for a stretch of positions are one loop over two arrays.
		Elements as the sums are made of (int16 or float). */
	void*			wide_weights;
	int32			wide_length;
	//! fully connected: real factor from sum to output
	double*			scales;
	//! floating point: zeros, for an operator without bias
	float*			zero_bias;
};


struct ConvRun {
	const ConvData*	data;
	const void*		input;
	const void*		weights;
	const void*		bias;
	void*			output;
	//! what the pieces of work leave when they cannot get memory
	int32			status;
};


typedef float float4 __attribute__((vector_size(16)));
typedef int32 int4 __attribute__((vector_size(16)));


static inline float4
load4(const float* values)
{
	float4 vector;
	memcpy(&vector, values, sizeof(vector));
	return vector;
}


// The sums of products are made for a block of output channels at a time,
// which share the input: eight where the compiler makes the loop (the
// work around a sum, mostly adding up the lanes of its vector at the end,
// then weighs less), four where the vectors are spelled out.

static inline int32
block_size(const int16*)
{
	return 8;
}


static inline int32
block_size(const float*)
{
	return 4;
}


/*!	Eight sums of products at once. The compiler makes multiply-add
	instructions of this loop (pmaddwd, smlal).
	\a kPadded says that the count is a multiple of eight, as that of
	weights stored by prepare() is; the compiler then need not provide for
	a rest, which is a good part of the time of a short sum. */
template<bool kPadded>
static inline void
dot_block(const int16* x, const int16* const* rows, int32 count, int32* sums)
{
	if (kPadded)
		count &= ~7;

	const int16* w0 = rows[0];
	const int16* w1 = rows[1];
	const int16* w2 = rows[2];
	const int16* w3 = rows[3];
	const int16* w4 = rows[4];
	const int16* w5 = rows[5];
	const int16* w6 = rows[6];
	const int16* w7 = rows[7];
	int32 sum0 = 0;
	int32 sum1 = 0;
	int32 sum2 = 0;
	int32 sum3 = 0;
	int32 sum4 = 0;
	int32 sum5 = 0;
	int32 sum6 = 0;
	int32 sum7 = 0;
	for (int32 i = 0; i < count; i++) {
		int32 value = x[i];
		sum0 += value * w0[i];
		sum1 += value * w1[i];
		sum2 += value * w2[i];
		sum3 += value * w3[i];
		sum4 += value * w4[i];
		sum5 += value * w5[i];
		sum6 += value * w6[i];
		sum7 += value * w7[i];
	}
	sums[0] = sum0;
	sums[1] = sum1;
	sums[2] = sum2;
	sums[3] = sum3;
	sums[4] = sum4;
	sums[5] = sum5;
	sums[6] = sum6;
	sums[7] = sum7;
}


/*!	Four sums of floating point products at once. Without leave to
	reorder additions the compiler would add one product at a time. Four
	partial sums per output are made instead, eight where the sums are
	long (processors that can start two additions at a time then do), and
	the partial sums of all four outputs are added up together at the end.
*/
template<bool kPadded>
static inline void
dot_block(const float* x, const float* const* rows, int32 count,
	float* sums)
{
	const float* w0 = rows[0];
	const float* w1 = rows[1];
	const float* w2 = rows[2];
	const float* w3 = rows[3];
	float4 a0 = { 0, 0, 0, 0 };
	float4 a1 = a0, a2 = a0, a3 = a0;

	int32 i = 0;
	if (count >= 64) {
		float4 b0 = a0, b1 = a0, b2 = a0, b3 = a0;
		for (; i + 8 <= count; i += 8) {
			float4 low = load4(x + i);
			float4 high = load4(x + i + 4);
			a0 += low * load4(w0 + i);
			b0 += high * load4(w0 + i + 4);
			a1 += low * load4(w1 + i);
			b1 += high * load4(w1 + i + 4);
			a2 += low * load4(w2 + i);
			b2 += high * load4(w2 + i + 4);
			a3 += low * load4(w3 + i);
			b3 += high * load4(w3 + i + 4);
		}
		a0 += b0;
		a1 += b1;
		a2 += b2;
		a3 += b3;
	}
	for (; i + 4 <= count; i += 4) {
		float4 values = load4(x + i);
		a0 += values * load4(w0 + i);
		a1 += values * load4(w1 + i);
		a2 += values * load4(w2 + i);
		a3 += values * load4(w3 + i);
	}

	// The four partial sums of each output are a row; add the columns of
	// the transposed rows and each output's sum is an element.
	float4 low01 = __builtin_shuffle(a0, a1, (int4){ 0, 4, 1, 5 });
	float4 high01 = __builtin_shuffle(a0, a1, (int4){ 2, 6, 3, 7 });
	float4 low23 = __builtin_shuffle(a2, a3, (int4){ 0, 4, 1, 5 });
	float4 high23 = __builtin_shuffle(a2, a3, (int4){ 2, 6, 3, 7 });
	float4 sum01 = low01 + high01;
	float4 sum23 = low23 + high23;
	float4 total = __builtin_shuffle(sum01, sum23, (int4){ 0, 1, 4, 5 })
		+ __builtin_shuffle(sum01, sum23, (int4){ 2, 3, 6, 7 });

	float sum0 = total[0];
	float sum1 = total[1];
	float sum2 = total[2];
	float sum3 = total[3];
	for (; i < count; i++) {
		sum0 += x[i] * w0[i];
		sum1 += x[i] * w1[i];
		sum2 += x[i] * w2[i];
		sum3 += x[i] * w3[i];
	}
	sums[0] = sum0;
	sums[1] = sum1;
	sums[2] = sum2;
	sums[3] = sum3;
}


/*!	The sums of \a count output channels for several inputs, \a inputStep
	apart: \a rows holds the weights of the channels one after the other,
	\a stride apart. All inputs are done with a block of output channels,
	then with the next block. The weights of many channels do not fit the
	processor's caches; this way they pass through them once, not once per
	input. The sums of an input are \a sumsStep apart.
*/
template<bool kPadded, typename Element, typename Sum>
static inline void
dot_tile(const Element* inputs, size_t inputStep, int32 inputCount,
	const Element* rows, size_t stride, int32 length, int32 count, Sum* sums,
	size_t sumsStep)
{
	const int32 kBlock = block_size(rows);

	for (int32 i = 0; i < count; i += kBlock) {
		// the last ones: the same again for the channels missing to a
		// block
		const Element* row[8];
		for (int32 j = 0; j < kBlock; j++) {
			int32 channel = i + j < count ? i + j : count - 1;
			row[j] = rows + channel * stride;
		}
		int32 valid = count - i < kBlock ? count - i : kBlock;

		for (int32 input = 0; input < inputCount; input++) {
			Sum result[8];
			dot_block<kPadded>(inputs + input * inputStep, row, length,
				result);
			Sum* target = sums + input * sumsStep + i;
			for (int32 j = 0; j < valid; j++)
				target[j] = result[j];
		}
	}
}


//! The same for one input.
template<bool kPadded, typename Element, typename Sum>
static inline void
dot_rows(const Element* x, const Element* rows, size_t stride, int32 length,
	int32 count, Sum* sums)
{
	dot_tile<kPadded>(x, 0, 1, rows, stride, length, count, sums, 0);
}


//! Input as the numbers the sums are made of: less the zero point.
template<typename Source, typename Target>
static inline void
convert_row(const Source* source, int32 offset, Target* target, int32 count)
{
	for (int32 i = 0; i < count; i++)
		target[i] = (Target)(source[i] + offset);
}


/*!	Sums to output values: bias, scaling by the factor of the channel
	and the output's zero point, exactly as the reference does.

	The reference multiplies by a 31 bit factor, rounds, shifts right by a
	number of bits that differs from channel to channel, and rounds again.
	Written down like that the compiler has to do one element at a time.
	The loop below gives the same results from steps that exist as vector
	instructions everywhere:
	- A product of signed numbers rounded to the nearest (halves up) is
	  floor((x * m + 2^30) / 2^31). With x + 2^31, which is not negative,
	  in place of x the product is one of unsigned numbers and comes out
	  larger by exactly m.
	- A shift to the right by n bits is a multiplication with 2^(31 - n)
	  followed by one by 31 bits, the same for all channels; for negative
	  numbers again with 2^31 added first. The reference rounds halves away
	  from zero here: add half, less one for negative numbers.
*/
template<typename Type>
static inline void
finish(const ConvData& data, const void* bias, const int32* __restrict sums,
	int32 count, Type* __restrict output)
{
	const int32* __restrict channelBias = data.bias;
	const int32* __restrict multipliers = data.multipliers;
	int32 offset = data.output_offset;
	int32 min = data.activation_min;
	int32 max = data.activation_max;

	if (!data.fast_scaling) {
		const int32* shifts = data.shifts;
		for (int32 i = 0; i < count; i++) {
			int32 value = neural_multiply_by_quantized_multiplier(
				sums[i] + channelBias[i], multipliers[i], shifts[i]) + offset;
			output[i] = (Type)neural_clamp(value, min, max);
		}
		return;
	}

	const uint32 kSign = 0x80000000;

	if (data.common_shift >= 0) {
		// all channels are shifted alike (always, when the weights have
		// one scale): the second step is a shift after all
		int32 shift = data.common_shift;
		int32 half = shift > 0 ? 1 << (shift - 1) : 0;
		int32 signMask = shift > 0 ? -1 : 0;
		for (int32 i = 0; i < count; i++) {
			uint32 multiplier = (uint32)multipliers[i];
			uint32 sum = (uint32)(sums[i] + channelBias[i]) ^ kSign;
			int32 scaled = (int32)((uint32)(((uint64)sum * multiplier
				+ ((uint32)1 << 30)) >> 31) - multiplier);
			int32 value = ((scaled + half + ((scaled >> 31) & signMask))
				>> shift) + offset;

			value = value < min ? min : value;
			value = value > max ? max : value;
			output[i] = (Type)value;
		}
		return;
	}

	const uint32* __restrict postMultipliers = data.post_multipliers;
	const int32* __restrict rounding = data.rounding;
	const int32* __restrict signMasks = data.sign_masks;

	for (int32 i = 0; i < count; i++) {
		uint32 multiplier = (uint32)multipliers[i];
		uint32 sum = (uint32)(sums[i] + channelBias[i]) ^ kSign;
		int32 scaled = (int32)((uint32)(((uint64)sum * multiplier
			+ ((uint32)1 << 30)) >> 31) - multiplier);

		uint32 postMultiplier = postMultipliers[i];
		uint32 rounded = (uint32)(scaled + rounding[i]
			+ ((scaled >> 31) & signMasks[i])) ^ kSign;
		int32 value = (int32)((uint32)(((uint64)rounded * postMultiplier)
			>> 31) - postMultiplier) + offset;

		value = value < min ? min : value;
		value = value > max ? max : value;
		output[i] = (Type)value;
	}
}


static inline void
finish(const ConvData& data, const void* bias, const float* __restrict sums,
	int32 count, float* __restrict output)
{
	const float* __restrict channelBias = (const float*)bias;
	float min = data.float_min;
	float max = data.float_max;
	for (int32 i = 0; i < count; i++)
		output[i] = neural_clamp(sums[i] + channelBias[i], min, max);
}


/*!	The numbers under the window at an output position, for one group of
	input channels, with zeros where the window is over the edge. */
template<typename Type, typename Element>
static inline void
build_patch(const ConvData& data, const Type* input, int32 top, int32 left,
	int32 group, Element* patch)
{
	const neural_window& window = data.window;
	int32 channels = data.filter_channels;
	int32 rowSize = data.filter_width * channels;

	for (int32 y = 0; y < data.filter_height; y++, patch += rowSize) {
		int32 inputY = top + y * data.dilation_height;
		if (inputY < 0 || inputY >= window.input_height) {
			memset(patch, 0, rowSize * sizeof(Element));
			continue;
		}

		const Type* row = input + ((size_t)inputY * window.input_width)
			* window.input_channels + group * channels;

		if (data.dilation_width == 1 && data.groups == 1) {
			// a stretch of the input row, clipped at both edges
			int32 first = neural_clamp(-left, 0, data.filter_width);
			int32 end = neural_clamp(window.input_width - left, first,
				data.filter_width);

			memset(patch, 0, first * channels * sizeof(Element));
			if (end > first) {
				convert_row(row + (size_t)(left + first) * channels,
					data.input_offset, patch + first * channels,
					(end - first) * channels);
			}
			memset(patch + end * channels, 0,
				(data.filter_width - end) * channels * sizeof(Element));
			continue;
		}

		for (int32 x = 0; x < data.filter_width; x++) {
			int32 inputX = left + x * data.dilation_width;
			Element* target = patch + x * channels;
			if (inputX < 0 || inputX >= window.input_width) {
				memset(target, 0, channels * sizeof(Element));
			} else {
				convert_row(row + (size_t)inputX * window.input_channels,
					data.input_offset, target, channels);
			}
		}
	}
}


// A floating point row needs no conversion: it is used where it is.

static inline const float*
row_view(const float* row, int32 offset, float* scratch, int32 count)
{
	return row;
}


template<typename Type>
static inline const int16*
row_view(const Type* row, int32 offset, int16* scratch, int32 count)
{
	convert_row(row, offset, scratch, count);
	return scratch;
}


static inline void
set_failed(ConvRun* run)
{
	__atomic_store_n(&run->status, (int32)B_NO_MEMORY, __ATOMIC_RELAXED);
}


// #pragma mark - CONV_2D


/*!	Output rows \a first to \a end (counted through all batches) of a
	convolution. \a Element is what the sums are made of: int16 or float;
	\a Sum what they add up to. The positions of a row are taken a tile at
	a time, see dot_tile(). */
template<typename Type, typename Element, typename Sum>
static void
conv_rows(void* cookie, int32 first, int32 end)
{
	NeuralFlushDenormals flush;
	ConvRun* run = (ConvRun*)cookie;
	const ConvData& data = *run->data;
	const neural_window& window = data.window;
	int32 channels = window.output_channels;
	int32 length = data.row_length;
	int32 tile = data.tile;

	// one block: the sums of a tile, its patches (padded like the
	// weights, the padding staying zero) or a converted input row
	int32 rowElements = window.input_width * window.input_channels;
	size_t sumsSize = (size_t)tile * channels * sizeof(Sum);
	size_t patchSize = data.direct
		? ((size_t)rowElements + 16) * sizeof(Element)
		: ((size_t)tile * length + 16) * sizeof(Element);
	NeuralScratch scratch;
	size_t sumsOffset = scratch.Reserve(sumsSize, 1);
	size_t patchOffset = scratch.Reserve(patchSize, 1);
	if (!scratch.Allocate()) {
		set_failed(run);
		return;
	}
	Sum* sums = scratch.At<Sum>(sumsOffset);
	Element* patches = scratch.At<Element>(patchOffset);

	const Element* weights = data.quantized
		? (const Element*)data.weights : (const Element*)run->weights;
	int32 groupOutputs = channels / data.groups;
	size_t inputSize = (size_t)window.input_height * rowElements;
	size_t outputRowSize = (size_t)window.output_width * channels;
	size_t directStep = (size_t)data.stride_width * window.input_channels;

	for (int32 index = first; index < end; index++) {
		int32 batch = index / window.output_height;
		int32 y = index % window.output_height;
		const Type* input = (const Type*)run->input + batch * inputSize;
		Type* output = (Type*)run->output + index * outputRowSize;

		const Element* row = NULL;
		if (data.direct) {
			row = row_view(input + (size_t)y * data.stride_height
				* rowElements, data.input_offset, patches, rowElements);
		}

		int32 top = y * data.stride_height - window.pad_top;
		for (int32 x = 0; x < window.output_width; x += tile) {
			int32 count = std::min(tile, window.output_width - x);

			// (quantized, the weights are padded to a multiple of eight)
			if (data.direct) {
				dot_tile<true>(row + x * directStep, directStep, count,
					weights, length, length, channels, sums, channels);
			} else {
				for (int32 group = 0; group < data.groups; group++) {
					for (int32 i = 0; i < count; i++) {
						build_patch(data, input, top, (x + i)
							* data.stride_width - window.pad_left, group,
							patches + (size_t)i * length);
					}
					dot_tile<true>((const Element*)patches, length, count,
						weights + (size_t)group * groupOutputs * length,
						length, length, groupOutputs,
						sums + group * groupOutputs, channels);
				}
			}

			for (int32 i = 0; i < count; i++) {
				finish(data, run->bias, sums + (size_t)i * channels,
					channels, output + (size_t)(x + i) * channels);
			}
		}
	}
}


/*!	The weights of a quantized operator as the run wants them, the bias
	and the factors from sums to outputs. \a channelAxis is the dimension
	of the weights that counts output channels, \a rowLength what
	ConvData::row_length is to be; the weights of one output channel are
	stored in the order the remaining dimensions give.
*/
struct QuantizedSetup {
	const neural_graph_tensor*	input;
	const neural_graph_tensor*	weights;
	const neural_graph_tensor*	bias;
	const neural_graph_tensor*	output;
	neural_activation			activation;
	int32						channels;
	int32						channel_axis;
	int32						patch_size;
	int32						row_length;
	//! the reference scales fully connected sums in floating point
	bool						real_scales;
};


static bool
check_quantized(const QuantizedSetup& setup)
{
	const neural_graph_tensor& input = *setup.input;
	const neural_graph_tensor& weights = *setup.weights;
	const neural_graph_tensor& output = *setup.output;

	if (!neural_is_quantized_8(input) || !neural_is_quantized_8(output)
		|| !neural_is_quantized_8(weights) || input.type != output.type
		|| weights.type != input.type || !neural_is_constant(weights)
		|| !neural_valid_zero_point(input)
		|| !neural_valid_zero_point(output)
		|| !neural_valid_zero_point(weights)) {
		return false;
	}

	// Signed weights are symmetric, except those of the fully connected
	// layer, which may have a zero point.
	if (input.type == B_NEURAL_INT8 && weights.zero_point != 0
		&& !setup.real_scales) {
		return false;
	}

	if (weights.channel_count > 0) {
		// one scale per output channel: signed weights only
		if (input.type != B_NEURAL_INT8
			|| weights.channel_count != setup.channels
			|| weights.channel_axis != setup.channel_axis) {
			return false;
		}
		for (int32 i = 0; i < weights.channel_count; i++) {
			if (!(weights.channel_scales[i] > 0)
				|| !isfinite(weights.channel_scales[i])) {
				return false;
			}
		}
	}

	if (setup.bias != NULL) {
		const neural_graph_tensor& bias = *setup.bias;
		if (bias.type != B_NEURAL_INT32 || !neural_is_constant(bias)
			|| neural_element_count(bias) != setup.channels) {
			return false;
		}
	}

	return true;
}


static void
reserve_quantized(const QuantizedSetup& setup, NeuralBlock& block,
	size_t* offsets)
{
	offsets[0] = block.Reserve((size_t)setup.channels * setup.row_length + 16,
		sizeof(int16));
	offsets[1] = block.Reserve(setup.channels, sizeof(int32));
	offsets[2] = block.Reserve(setup.channels, sizeof(int32));
	offsets[3] = block.Reserve(setup.channels, sizeof(int32));
	offsets[4] = block.Reserve(setup.real_scales ? setup.channels : 0,
		sizeof(double));
	offsets[5] = block.Reserve(setup.channels, sizeof(uint32));
	offsets[6] = block.Reserve(setup.channels, sizeof(int32));
	offsets[7] = block.Reserve(setup.channels, sizeof(int32));
}


static status_t
fill_quantized(const QuantizedSetup& setup, const NeuralBlock& block,
	const size_t* offsets, ConvData* data)
{
	const neural_graph_tensor& input = *setup.input;
	const neural_graph_tensor& weights = *setup.weights;
	const neural_graph_tensor& output = *setup.output;

	data->quantized = true;
	data->row_length = setup.row_length;
	data->patch_size = setup.patch_size;
	data->weights = block.At<int16>(offsets[0]);
	data->bias = block.At<int32>(offsets[1]);
	data->multipliers = block.At<int32>(offsets[2]);
	data->shifts = block.At<int32>(offsets[3]);
	data->scales = setup.real_scales ? block.At<double>(offsets[4]) : NULL;
	data->post_multipliers = block.At<uint32>(offsets[5]);
	data->rounding = block.At<int32>(offsets[6]);
	data->sign_masks = block.At<int32>(offsets[7]);
	data->fast_scaling = !setup.real_scales;
	data->common_shift = -1;
	data->input_offset = -input.zero_point;
	data->output_offset = output.zero_point;

	status_t status = neural_quantized_activation_range(setup.activation,
		output, &data->activation_min, &data->activation_max);
	if (status != B_OK)
		return status;

	// The weights: channel by channel, whatever dimension counts them.
	int32 axisSize = weights.dimensions[setup.channel_axis];
	int32 inner = 1;
	for (int32 i = setup.channel_axis + 1; i < weights.rank; i++)
		inner *= weights.dimensions[i];
	int32 elements = neural_element_count(weights);
	int32 weightOffset = -weights.zero_point;

	for (int32 i = 0; i < elements; i++) {
		int32 channel = (i / inner) % axisSize;
		int32 position = (i / inner / axisSize) * inner + i % inner;
		int32 value = weights.type == B_NEURAL_UINT8
			? ((const uint8*)weights.data)[i]
			: ((const int8*)weights.data)[i];
		data->weights[(size_t)channel * setup.row_length + position]
			= (int16)(value + weightOffset);
	}

	int32 inputMin = 0;
	int32 inputMax = 0;
	neural_type_range(input.type, &inputMin, &inputMax);
	int64 largestInput = (int64)inputMax - input.zero_point;
	if ((int64)input.zero_point - inputMin > largestInput)
		largestInput = (int64)input.zero_point - inputMin;

	for (int32 channel = 0; channel < setup.channels; channel++) {
		const int16* row = data->weights
			+ (size_t)channel * setup.row_length;
		int64 magnitude = 0;
		for (int32 i = 0; i < setup.patch_size; i++)
			magnitude += abs(row[i]);

		int32 bias = setup.bias != NULL
			? ((const int32*)setup.bias->data)[channel] : 0;
		data->bias[channel] = bias;

		// A sum that could leave 32 bits is not one a model made in
		// earnest has; refusing it keeps the arithmetic defined.
		int64 largestSum = magnitude * largestInput
			+ (bias < 0 ? -(int64)bias : bias);
		if (largestSum > INT_MAX)
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		if (largestSum >= (1 << 30))
			data->fast_scaling = false;

		// The factor is computed as the reference does: for unsigned
		// models the product of the scales is rounded to single
		// precision first.
		double scale;
		if (setup.real_scales || input.type == B_NEURAL_INT8) {
			float weightScale = weights.channel_count > 0
				? weights.channel_scales[channel] : weights.scale;
			scale = (double)input.scale * (double)weightScale
				/ (double)output.scale;
		} else {
			scale = (double)(input.scale * weights.scale)
				/ (double)output.scale;
		}

		if (setup.real_scales) {
			if (!isfinite(scale))
				return B_NEURAL_UNSUPPORTED_OPERATOR;
			data->scales[channel] = scale;
		} else {
			int shift;
			if (!neural_checked_quantize_multiplier(scale,
					&data->multipliers[channel], &shift)) {
				return B_NEURAL_UNSUPPORTED_OPERATOR;
			}
			data->shifts[channel] = shift;

			// the shift to the right as finish() does it
			if (shift > 0) {
				data->fast_scaling = false;
			} else {
				data->post_multipliers[channel] = (uint32)1 << (31 + shift);
				data->rounding[channel] = shift < 0 ? 1 << (-shift - 1) : 0;
				data->sign_masks[channel] = shift < 0 ? -1 : 0;
			}
		}
	}

	if (data->fast_scaling) {
		data->common_shift = -data->shifts[0];
		for (int32 channel = 1; channel < setup.channels; channel++) {
			if (data->shifts[channel] != data->shifts[0])
				data->common_shift = -1;
		}
	}

	return B_OK;
}


static bool
check_float(const neural_graph_tensor& input,
	const neural_graph_tensor& weights, const neural_graph_tensor* bias,
	const neural_graph_tensor& output, int32 channels)
{
	if (input.type != B_NEURAL_FLOAT32 || output.type != B_NEURAL_FLOAT32
		|| weights.type != B_NEURAL_FLOAT32) {
		return false;
	}
	return bias == NULL || (bias->type == B_NEURAL_FLOAT32
		&& neural_element_count(*bias) == channels);
}


static inline int32
round_up(int32 value, int32 unit)
{
	return (value + unit - 1) / unit * unit;
}


static status_t
prepare_conv(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	if (!neural_has_inputs(op, 2) || op->input_count > 3
		|| op->output_count != 1 || op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_conv_params& params = *(const neural_conv_params*)op->params;
	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& weights = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor* bias = neural_has_input(op, 2)
		? &graph->Tensor(op->inputs[2]) : NULL;
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	if (weights.rank != 4 || neural_element_count(weights) < 0)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	ConvData header;
	memset(&header, 0, sizeof(header));
	header.filter_height = weights.dimensions[1];
	header.filter_width = weights.dimensions[2];
	header.filter_channels = weights.dimensions[3];
	header.stride_height = params.stride_height;
	header.stride_width = params.stride_width;
	header.dilation_height = params.dilation_height;
	header.dilation_width = params.dilation_width;

	neural_window& window = header.window;
	if (!neural_window_geometry(input, output, params.padding,
			header.filter_height, header.filter_width, params.stride_height,
			params.stride_width, params.dilation_height,
			params.dilation_width, &window)
		|| window.output_channels != weights.dimensions[0]
		|| window.input_channels % header.filter_channels != 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	header.groups = window.input_channels / header.filter_channels;
	if (window.output_channels % header.groups != 0)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	header.patch_size = neural_element_count(weights)
		/ window.output_channels;
	header.direct = header.filter_height == 1 && header.filter_width == 1
		&& header.groups == 1;

	NeuralBlock block(sizeof(ConvData));
	size_t offsets[8];
	size_t zeroBias = 0;
	QuantizedSetup setup = { &input, &weights, bias, &output,
		params.activation, window.output_channels, 0, header.patch_size,
		round_up(header.patch_size, 8), false };

	if (input.type == B_NEURAL_FLOAT32) {
		if (!check_float(input, weights, bias, output,
				window.output_channels)
			|| neural_float_activation_range(params.activation,
				&header.float_min, &header.float_max) != B_OK) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
		header.row_length = header.patch_size;
		zeroBias = block.Reserve(window.output_channels, sizeof(float));
	} else {
		if (!check_quantized(setup))
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		reserve_quantized(setup, block, offsets);
	}

	if (input.type != B_NEURAL_FLOAT32)
		header.row_length = setup.row_length;

	// Positions per tile: what fits the first level cache with the weights
	// of a block of channels; of large patches at least a few, which then
	// stay in the second level. The sums of a tile are to stay small, too.
	size_t patchBytes = (size_t)header.row_length
		* (input.type == B_NEURAL_FLOAT32 ? sizeof(float) : sizeof(int16));
	size_t tile = 16384 / patchBytes;
	if (tile < 8)
		tile = std::min((size_t)8, 131072 / patchBytes);
	tile = std::min(tile, 65536 / ((size_t)window.output_channels * 4));
	header.tile = (int32)std::max((size_t)1, std::min(tile,
		(size_t)window.output_width));

	ConvData* data = (ConvData*)block.Allocate();
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;

	if (input.type == B_NEURAL_FLOAT32) {
		data->zero_bias = block.At<float>(zeroBias);
	} else {
		status_t status = fill_quantized(setup, block, offsets, data);
		if (status != B_OK) {
			free(data);
			return status;
		}
	}

	*_prepared = data;
	return B_OK;
}


/*!	How many rows are worth a thread of their own: a few dozen
	microseconds of work (measured: with less the threads mostly wake
	each other, with more the last ones have nothing to do).
	\a workPerRow is in products of a convolution. */
static int32
row_grain(int64 workPerRow)
{
	int64 grain = 250000 / (workPerRow > 0 ? workPerRow : 1);
	return grain < 1 ? 1 : (int32)grain;
}


static status_t
run_conv(const NeuralOpContext& context)
{
	const ConvData* data = (const ConvData*)context.prepared;
	const neural_window& window = data->window;

	ConvRun run;
	run.data = data;
	run.input = context.Input<void>(0);
	run.weights = context.Input<void>(1);
	run.bias = context.HasInput(2)
		? context.Input<void>(2) : (const void*)data->zero_bias;
	run.output = context.Output<void>(0);
	run.status = B_OK;

	NeuralThreadPool::Function function;
	if (!data->quantized)
		function = &conv_rows<float, float, float>;
	else if (context.InputInfo(0).type == B_NEURAL_UINT8)
		function = &conv_rows<uint8, int16, int32>;
	else
		function = &conv_rows<int8, int16, int32>;

	// (bringing a sum to the output costs like some more products)
	context.pool->Run(window.batches * window.output_height,
		row_grain((int64)window.output_width * window.output_channels
			* ((int64)data->patch_size + 16)), function, &run);
	return run.status;
}


// #pragma mark - DEPTHWISE_CONV_2D


// Products of one, two or three taps added to the sums of a stretch of
// positions. With more taps per walk over the sums they are read and
// written less often; three is what the registers of an x86 take.

template<typename Element, typename Sum>
static inline void
add_products(Sum* __restrict sums, const Element* __restrict values,
	const Element* __restrict weights, int32 count)
{
	for (int32 i = 0; i < count; i++)
		sums[i] += (Sum)values[i] * (Sum)weights[i];
}


template<typename Element, typename Sum>
static inline void
add_products(Sum* __restrict sums, const Element* __restrict values0,
	const Element* __restrict weights0, const Element* __restrict values1,
	const Element* __restrict weights1, int32 count)
{
	for (int32 i = 0; i < count; i++) {
		sums[i] += (Sum)values0[i] * (Sum)weights0[i]
			+ (Sum)values1[i] * (Sum)weights1[i];
	}
}


template<typename Element, typename Sum>
static inline void
add_products(Sum* __restrict sums, const Element* __restrict values0,
	const Element* __restrict weights0, const Element* __restrict values1,
	const Element* __restrict weights1, const Element* __restrict values2,
	const Element* __restrict weights2, int32 count)
{
	for (int32 i = 0; i < count; i++) {
		sums[i] += (Sum)values0[i] * (Sum)weights0[i]
			+ (Sum)values1[i] * (Sum)weights1[i]
			+ (Sum)values2[i] * (Sum)weights2[i];
	}
}


/*!	Output rows of a depthwise convolution. The input rows a window
	touches are kept converted and padded with zeros left and right, so
	that the window can walk over them without looking where it is; a row
	stays until no later output row needs it.

	With one output channel per input channel (nearly always) a tap is done
	for the whole output row at once: its inputs times its weights,
	repeated along the row, added to the sums of the row. A loop over
	a few channels at every position would be mostly getting started.
*/
template<typename Type, typename Element, typename Sum>
static void
depthwise_rows(void* cookie, int32 first, int32 end)
{
	NeuralFlushDenormals flush;
	ConvRun* run = (ConvRun*)cookie;
	const ConvData& data = *run->data;
	const neural_window& window = data.window;

	int32 channels = window.input_channels;
	int32 outputChannels = window.output_channels;
	int32 multiplier = data.depth_multiplier;

	// the width the windows of an output row cover, padding included
	int64 paddedWidth = (int64)(window.output_width - 1) * data.stride_width
		+ (int64)(data.filter_width - 1) * data.dilation_width + 1;
	int32 slots = std::min(data.filter_height, window.input_height);
	size_t rowSize = (size_t)paddedWidth * channels * sizeof(Element);
	size_t outputRowSize = (size_t)window.output_width * outputChannels;
	bool wide = data.wide_weights != NULL;
	// sums of a whole row, and room for the inputs of a tap side by side
	size_t sumsSize = (wide ? outputRowSize : outputChannels) * sizeof(Sum);
	size_t gatherSize = wide && data.stride_width != 1
		? 3 * outputRowSize * sizeof(Element) : 0;
	size_t tableSize = (size_t)data.filter_height * sizeof(Element*)
		+ (size_t)slots * sizeof(int32);

	NeuralScratch scratch;
	size_t tableOffset = scratch.Reserve(tableSize, 1);
	size_t sumsOffset = scratch.Reserve(sumsSize, 1);
	size_t gatherOffset = scratch.Reserve(gatherSize, 1);
	size_t rowsOffset = scratch.Reserve(slots, rowSize);
	if (!scratch.Allocate()) {
		set_failed(run);
		return;
	}
	const Element** tapRows = scratch.At<const Element*>(tableOffset);
	int32* slotRows = (int32*)(tapRows + data.filter_height);
	Sum* sums = scratch.At<Sum>(sumsOffset);
	Element* gathered = scratch.At<Element>(gatherOffset);
	Element* rows = scratch.At<Element>(rowsOffset);
	for (int32 i = 0; i < slots; i++)
		slotRows[i] = -1;

	const Element* weights = data.quantized
		? (const Element*)data.weights : (const Element*)run->weights;
	size_t inputRowSize = (size_t)window.input_width * channels;
	size_t inputSize = (size_t)window.input_height * inputRowSize;

	// the input columns that fall under some window: 0 to this one
	int64 endColumn = paddedWidth - window.pad_left;
	if (endColumn > window.input_width)
		endColumn = window.input_width;

	int32 lastBatch = -1;
	for (int32 index = first; index < end; index++) {
		int32 batch = index / window.output_height;
		int32 y = index % window.output_height;
		const Type* input = (const Type*)run->input + batch * inputSize;
		Type* output = (Type*)run->output + index * outputRowSize;

		if (batch != lastBatch) {
			for (int32 i = 0; i < slots; i++)
				slotRows[i] = -1;
			lastBatch = batch;
		}

		// Find or make the rows under the window. A row is kept in the
		// slot its number picks; the rows of one window differ in that.
		int32 top = y * data.stride_height - window.pad_top;
		for (int32 tap = 0; tap < data.filter_height; tap++) {
			int64 inputY = top + (int64)tap * data.dilation_height;
			if (inputY < 0 || inputY >= window.input_height) {
				tapRows[tap] = NULL;
				continue;
			}

			int32 slot = -1;
			for (int32 i = 0; i < slots; i++) {
				if (slotRows[i] == inputY) {
					slot = i;
					break;
				}
			}
			if (slot < 0) {
				// Take a slot no tap of this window uses; there is one,
				// as there are as many slots as rows under a window.
				slot = 0;
				for (int32 i = 0; i < slots; i++) {
					int64 distance = slotRows[i] - (int64)top;
					if (slotRows[i] < 0 || distance < 0
						|| distance % data.dilation_height != 0
						|| distance / data.dilation_height
							>= data.filter_height) {
						slot = i;
						break;
					}
				}

				Element* row = rows + (size_t)slot * paddedWidth * channels;
				memset(row, 0, (size_t)window.pad_left * channels
					* sizeof(Element));
				convert_row(input + (size_t)inputY * inputRowSize,
					data.input_offset,
					row + (size_t)window.pad_left * channels,
					(int32)endColumn * channels);
				int64 filled = window.pad_left + endColumn;
				memset(row + filled * channels, 0, (size_t)(paddedWidth
					- filled) * channels * sizeof(Element));
				slotRows[slot] = (int32)inputY;
			}

			tapRows[tap] = rows + (size_t)slot * paddedWidth * channels;
		}

		if (wide) {
			memset(sums, 0, sumsSize);

			for (int32 tapY = 0; tapY < data.filter_height; tapY++) {
				if (tapRows[tapY] == NULL)
					continue;

				for (int32 tapX = 0; tapX < data.filter_width; tapX += 3) {
					int32 taps = std::min(data.filter_width - tapX, 3);

					// the inputs of these taps for the positions of the
					// row, and their weights
					const Element* sources[3];
					const Element* tapWeights[3];
					for (int32 tap = 0; tap < taps; tap++) {
						const Element* source = tapRows[tapY] + (size_t)(tapX
							+ tap) * data.dilation_width * channels;
						if (data.stride_width != 1) {
							// side by side first
							Element* target = gathered + tap * outputRowSize;
							size_t step = (size_t)data.stride_width
								* channels;
							for (int32 x = 0; x < window.output_width; x++) {
								memcpy(target + (size_t)x * channels,
									source + x * step,
									channels * sizeof(Element));
							}
							source = target;
						}
						sources[tap] = source;
						tapWeights[tap] = (const Element*)data.wide_weights
							+ ((size_t)tapY * data.filter_width + tapX + tap)
								* data.wide_length;
					}

					for (size_t offset = 0; offset < outputRowSize;
							offset += data.wide_length) {
						int32 count = (int32)std::min(
							(size_t)data.wide_length,
							outputRowSize - offset);
						if (taps == 3) {
							add_products(sums + offset, sources[0] + offset,
								tapWeights[0], sources[1] + offset,
								tapWeights[1], sources[2] + offset,
								tapWeights[2], count);
						} else if (taps == 2) {
							add_products(sums + offset, sources[0] + offset,
								tapWeights[0], sources[1] + offset,
								tapWeights[1], count);
						} else {
							add_products(sums + offset, sources[0] + offset,
								tapWeights[0], count);
						}
					}
				}
			}

			for (int32 x = 0; x < window.output_width; x++) {
				finish(data, run->bias, sums + (size_t)x * channels,
					channels, output + (size_t)x * channels);
			}
			continue;
		}

		for (int32 x = 0; x < window.output_width; x++) {
			for (int32 i = 0; i < outputChannels; i++)
				sums[i] = 0;

			size_t column = (size_t)x * data.stride_width * channels;
			for (int32 tapY = 0; tapY < data.filter_height; tapY++) {
				if (tapRows[tapY] == NULL)
					continue;

				const Element* source = tapRows[tapY] + column;
				const Element* tapWeights = weights
					+ (size_t)tapY * data.filter_width * outputChannels;
				for (int32 tapX = 0; tapX < data.filter_width; tapX++) {
					const Element* values = source
						+ (size_t)tapX * data.dilation_width * channels;
					if (multiplier == 1) {
						for (int32 i = 0; i < channels; i++)
							sums[i] += (Sum)values[i] * (Sum)tapWeights[i];
					} else {
						for (int32 i = 0; i < channels; i++) {
							Sum value = values[i];
							for (int32 j = 0; j < multiplier; j++) {
								sums[i * multiplier + j] += value
									* (Sum)tapWeights[i * multiplier + j];
							}
						}
					}
					tapWeights += outputChannels;
				}
			}

			finish(data, run->bias, sums, outputChannels,
				output + (size_t)x * outputChannels);
		}
	}
}


static status_t
prepare_depthwise(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	if (!neural_has_inputs(op, 2) || op->input_count > 3
		|| op->output_count != 1 || op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_conv_params& params = *(const neural_conv_params*)op->params;
	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& weights = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor* bias = neural_has_input(op, 2)
		? &graph->Tensor(op->inputs[2]) : NULL;
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	if (weights.rank != 4 || weights.dimensions[0] != 1
		|| neural_element_count(weights) < 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	ConvData header;
	memset(&header, 0, sizeof(header));
	header.filter_height = weights.dimensions[1];
	header.filter_width = weights.dimensions[2];
	header.stride_height = params.stride_height;
	header.stride_width = params.stride_width;
	header.dilation_height = params.dilation_height;
	header.dilation_width = params.dilation_width;
	header.groups = 1;

	// The multiplier is what the shapes say; the parameter of the same
	// name is not always filled in.
	neural_window& window = header.window;
	if (!neural_window_geometry(input, output, params.padding,
			header.filter_height, header.filter_width, params.stride_height,
			params.stride_width, params.dilation_height,
			params.dilation_width, &window)
		|| window.output_channels != weights.dimensions[3]
		|| window.output_channels % window.input_channels != 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	header.depth_multiplier = window.output_channels / window.input_channels;
	header.patch_size = header.filter_height * header.filter_width;

	// the rows a piece of work keeps must be of a size that can be asked
	// for
	int64 paddedWidth = (int64)(window.output_width - 1) * params.stride_width
		+ (int64)(header.filter_width - 1) * params.dilation_width + 1;
	if (paddedWidth * window.input_channels > kNeuralMaxElements)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	NeuralBlock block(sizeof(ConvData));
	size_t offsets[8];
	size_t zeroBias = 0;
	QuantizedSetup setup = { &input, &weights, bias, &output,
		params.activation, window.output_channels, 3, header.patch_size,
		header.patch_size, false };

	if (input.type == B_NEURAL_FLOAT32) {
		if (!check_float(input, weights, bias, output,
				window.output_channels)
			|| neural_float_activation_range(params.activation,
				&header.float_min, &header.float_max) != B_OK) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
		zeroBias = block.Reserve(window.output_channels, sizeof(float));
	} else {
		if (!check_quantized(setup))
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		reserve_quantized(setup, block, offsets);
	}

	// The weights of each tap, repeated to some hundred numbers; this
	// needs them now, which is the rule.
	int32 channels = window.output_channels;
	int32 taps = header.patch_size;
	size_t wideOffset = 0;
	size_t elementSize = input.type == B_NEURAL_FLOAT32
		? sizeof(float) : sizeof(int16);
	if (header.depth_multiplier == 1 && neural_is_constant(weights)) {
		header.wide_length = channels * ((256 + channels - 1) / channels);
		wideOffset = block.Reserve((size_t)taps * header.wide_length,
			elementSize);
	}

	ConvData* data = (ConvData*)block.Allocate();
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;

	if (input.type == B_NEURAL_FLOAT32) {
		data->zero_bias = block.At<float>(zeroBias);
		if (wideOffset != 0) {
			float* wide = block.At<float>(wideOffset);
			data->wide_weights = wide;
			for (int32 tap = 0; tap < taps; tap++) {
				for (int32 i = 0; i < data->wide_length; i++) {
					wide[(size_t)tap * data->wide_length + i]
						= ((const float*)weights.data)[(size_t)tap * channels
							+ i % channels];
				}
			}
		}
	} else {
		status_t status = fill_quantized(setup, block, offsets, data);
		if (status == B_OK) {
			// fill_quantized() stored the taps of each channel together;
			// the run wants the channels of each tap together, which is
			// the order of the file.
			int16* scratch = (int16*)malloc((size_t)taps * channels
				* sizeof(int16));
			if (scratch == NULL) {
				status = B_NO_MEMORY;
			} else {
				for (int32 tap = 0; tap < taps; tap++) {
					for (int32 i = 0; i < channels; i++) {
						scratch[(size_t)tap * channels + i]
							= data->weights[(size_t)i * taps + tap];
					}
				}
				memcpy(data->weights, scratch, (size_t)taps * channels
					* sizeof(int16));
				free(scratch);
			}
		}
		if (status == B_OK && wideOffset != 0) {
			int16* wide = block.At<int16>(wideOffset);
			data->wide_weights = wide;
			for (int32 tap = 0; tap < taps; tap++) {
				for (int32 i = 0; i < data->wide_length; i++) {
					wide[(size_t)tap * data->wide_length + i]
						= data->weights[(size_t)tap * channels + i % channels];
				}
			}
		}
		if (status != B_OK) {
			free(data);
			return status;
		}
	}

	*_prepared = data;
	return B_OK;
}


static status_t
run_depthwise(const NeuralOpContext& context)
{
	const ConvData* data = (const ConvData*)context.prepared;
	const neural_window& window = data->window;

	ConvRun run;
	run.data = data;
	run.input = context.Input<void>(0);
	run.weights = context.Input<void>(1);
	run.bias = context.HasInput(2)
		? context.Input<void>(2) : (const void*)data->zero_bias;
	run.output = context.Output<void>(0);
	run.status = B_OK;

	NeuralThreadPool::Function function;
	if (!data->quantized)
		function = &depthwise_rows<float, float, float>;
	else if (context.InputInfo(0).type == B_NEURAL_UINT8)
		function = &depthwise_rows<uint8, int16, int32>;
	else
		function = &depthwise_rows<int8, int16, int32>;

	// (a product here takes several times as long as in a convolution)
	context.pool->Run(window.batches * window.output_height,
		row_grain((int64)window.output_width * window.output_channels
			* (4 * (int64)data->patch_size + 16)), function, &run);
	return run.status;
}


// #pragma mark - FULLY_CONNECTED


struct FullyConnectedRun {
	const ConvData*	data;
	//! floating point: the input; quantized: converted, padded rows
	const void*		input;
	const void*		weights;
	const void*		bias;
	void*			output;
	int32			batches;
	int32			units;
};


/*!	Output units \a first to \a end of all batches. The reference scales
	the sums of this operator in double precision floating point instead
	of fixed point; so does this. */
template<typename Type>
static void
fully_connected_units(void* cookie, int32 first, int32 end)
{
	FullyConnectedRun* run = (FullyConnectedRun*)cookie;
	const ConvData& data = *run->data;
	int32 length = data.row_length;

	int32 sums[64];
	for (int32 batch = 0; batch < run->batches; batch++) {
		const int16* input = (const int16*)run->input
			+ (size_t)batch * length;
		Type* output = (Type*)run->output + (size_t)batch * run->units;

		for (int32 unit = first; unit < end; unit += 64) {
			int32 count = std::min(end - unit, (int32)64);
			dot_rows<true>(input, data.weights + (size_t)unit * length,
				length, length, count, sums);

			for (int32 i = 0; i < count; i++) {
				double value = round((double)(sums[i] + data.bias[unit + i])
					* data.scales[unit + i]) + data.output_offset;
				// clamped before it is made an integer: it may be
				// anything
				if (!(value > data.activation_min))
					value = data.activation_min;
				if (value > data.activation_max)
					value = data.activation_max;
				output[unit + i] = (Type)(int32)value;
			}
		}
	}
}


static void
fully_connected_float_units(void* cookie, int32 first, int32 end)
{
	NeuralFlushDenormals flush;
	FullyConnectedRun* run = (FullyConnectedRun*)cookie;
	const ConvData& data = *run->data;
	int32 length = data.row_length;
	const float* weights = (const float*)run->weights;
	const float* bias = (const float*)run->bias;

	float sums[64];
	for (int32 batch = 0; batch < run->batches; batch++) {
		const float* input = (const float*)run->input
			+ (size_t)batch * length;
		float* output = (float*)run->output + (size_t)batch * run->units;

		for (int32 unit = first; unit < end; unit += 64) {
			int32 count = std::min(end - unit, (int32)64);
			dot_rows<false>(input, weights + (size_t)unit * length, length,
				length, count, sums);
			finish(data, bias + unit, sums, count, output + unit);
		}
	}
}


static status_t
prepare_fully_connected(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	if (!neural_has_inputs(op, 2) || op->input_count > 3
		|| op->output_count != 1 || op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_fully_connected_params& params
		= *(const neural_fully_connected_params*)op->params;
	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& weights = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor* bias = neural_has_input(op, 2)
		? &graph->Tensor(op->inputs[2]) : NULL;
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	int32 inputElements = neural_element_count(input);
	int32 outputElements = neural_element_count(output);
	if (weights.rank != 2 || neural_element_count(weights) < 0
		|| inputElements < 0 || outputElements < 0 || output.rank < 1) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	// The input is taken as rows of as many numbers as a unit has
	// weights, whatever its shape.
	int32 units = weights.dimensions[0];
	int32 depth = weights.dimensions[1];
	if (inputElements % depth != 0
		|| outputElements / units != inputElements / depth
		|| outputElements % units != 0
		|| output.dimensions[output.rank - 1] != units) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	if (params.keep_dimensions) {
		if (input.rank != output.rank || input.rank < 1
			|| input.dimensions[input.rank - 1] != depth) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	} else if (output.rank != 2)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	ConvData header;
	memset(&header, 0, sizeof(header));
	header.patch_size = depth;
	header.window.batches = inputElements / depth;
	header.window.output_channels = units;

	NeuralBlock block(sizeof(ConvData));
	size_t offsets[8];
	size_t zeroBias = 0;
	QuantizedSetup setup = { &input, &weights, bias, &output,
		params.activation, units, 0, depth, round_up(depth, 8), true };

	if (input.type == B_NEURAL_FLOAT32) {
		if (!check_float(input, weights, bias, output, units)
			|| neural_float_activation_range(params.activation,
				&header.float_min, &header.float_max) != B_OK) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
		header.row_length = depth;
		zeroBias = block.Reserve(units, sizeof(float));
	} else {
		// Rows of converted input are made for every run; they must be
		// of a size that can be asked for.
		if (!check_quantized(setup) || (int64)header.window.batches
				* setup.row_length > kNeuralMaxElements) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
		reserve_quantized(setup, block, offsets);
	}

	ConvData* data = (ConvData*)block.Allocate();
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;

	if (input.type == B_NEURAL_FLOAT32) {
		data->zero_bias = block.At<float>(zeroBias);
	} else {
		status_t status = fill_quantized(setup, block, offsets, data);
		if (status != B_OK) {
			free(data);
			return status;
		}
	}

	*_prepared = data;
	return B_OK;
}


static status_t
run_fully_connected(const NeuralOpContext& context)
{
	const ConvData* data = (const ConvData*)context.prepared;

	FullyConnectedRun run;
	run.data = data;
	run.input = context.Input<void>(0);
	run.weights = context.Input<void>(1);
	run.bias = context.HasInput(2)
		? context.Input<void>(2) : (const void*)data->zero_bias;
	run.output = context.Output<void>(0);
	run.batches = data->window.batches;
	run.units = data->window.output_channels;

	int32 grain = row_grain((int64)run.batches * (data->patch_size + 16));
	if (!data->quantized) {
		context.pool->Run(run.units, grain, &fully_connected_float_units,
			&run);
		return B_OK;
	}

	// the input less its zero point, in rows padded like the weights
	int32 depth = data->patch_size;
	int16* converted = (int16*)calloc((size_t)run.batches * data->row_length
		+ 16, sizeof(int16));
	if (converted == NULL)
		return B_NO_MEMORY;

	bool isUnsigned = context.InputInfo(0).type == B_NEURAL_UINT8;
	for (int32 batch = 0; batch < run.batches; batch++) {
		int16* row = converted + (size_t)batch * data->row_length;
		if (isUnsigned) {
			convert_row(context.Input<uint8>(0) + (size_t)batch * depth,
				data->input_offset, row, depth);
		} else {
			convert_row(context.Input<int8>(0) + (size_t)batch * depth,
				data->input_offset, row, depth);
		}
	}

	run.input = converted;
	context.pool->Run(run.units, grain, isUnsigned
		? &fully_connected_units<uint8> : &fully_connected_units<int8>,
		&run);

	free(converted);
	return B_OK;
}


// #pragma mark - TRANSPOSE_CONV


/*!	Output rows of a transposed convolution: every input position adds
	its value times the filter to a window of the output. Seen from an
	output position those are the input positions a step apart whose
	windows reach it; with them a row can be made without the others.
*/
template<typename Type, typename Element, typename Sum>
static void
transpose_conv_rows(void* cookie, int32 first, int32 end)
{
	NeuralFlushDenormals flush;
	ConvRun* run = (ConvRun*)cookie;
	const ConvData& data = *run->data;
	const neural_window& window = data.window;
		// the window of the convolution this is the reverse of: its
		// "input" is our output

	int32 inputChannels = window.output_channels;
	int32 outputChannels = window.input_channels;
	int32 inputHeight = window.output_height;
	int32 inputWidth = window.output_width;
	int32 outputHeight = window.input_height;
	int32 outputWidth = window.input_width;

	Sum* sums = (Sum*)malloc(2 * (size_t)outputChannels * sizeof(Sum));
	if (sums == NULL) {
		set_failed(run);
		return;
	}
	Sum* partial = sums + outputChannels;

	const Element* weights = data.quantized
		? (const Element*)data.weights : (const Element*)run->weights;
	size_t inputRowSize = (size_t)inputWidth * inputChannels;
	size_t inputSize = (size_t)inputHeight * inputRowSize;
	size_t outputRowSize = (size_t)outputWidth * outputChannels;
	// weights of one output channel, and of one of its taps
	size_t channelStride = (size_t)data.filter_height * data.filter_width
		* inputChannels;

	for (int32 index = first; index < end; index++) {
		int32 batch = index / outputHeight;
		int32 y = index % outputHeight;
		const Element* input = (const Element*)run->input
			+ batch * inputSize;
		Type* output = (Type*)run->output + index * outputRowSize;

		for (int32 x = 0; x < outputWidth; x++) {
			for (int32 i = 0; i < outputChannels; i++)
				sums[i] = 0;

			for (int32 tapY = 0; tapY < data.filter_height; tapY++) {
				int32 distanceY = y + window.pad_top - tapY;
				if (distanceY < 0 || distanceY % data.stride_height != 0
					|| distanceY / data.stride_height >= inputHeight) {
					continue;
				}
				int32 inputY = distanceY / data.stride_height;

				for (int32 tapX = 0; tapX < data.filter_width; tapX++) {
					int32 distanceX = x + window.pad_left - tapX;
					if (distanceX < 0 || distanceX % data.stride_width != 0
						|| distanceX / data.stride_width >= inputWidth) {
						continue;
					}
					int32 inputX = distanceX / data.stride_width;

					dot_rows<false>(input + inputY * inputRowSize
							+ (size_t)inputX * inputChannels,
						weights + ((size_t)tapY * data.filter_width + tapX)
							* inputChannels,
						channelStride, inputChannels, outputChannels,
						partial);
					for (int32 i = 0; i < outputChannels; i++)
						sums[i] += partial[i];
				}
			}

			finish(data, run->bias, sums, outputChannels,
				output + (size_t)x * outputChannels);
		}
	}

	free(sums);
}


static status_t
prepare_transpose_conv(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	if (!neural_has_inputs(op, 3) || op->input_count > 4
		|| op->output_count != 1 || op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_conv_params& params = *(const neural_conv_params*)op->params;
	const neural_graph_tensor& shape = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& weights = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& input = graph->Tensor(op->inputs[2]);
	const neural_graph_tensor* bias = neural_has_input(op, 3)
		? &graph->Tensor(op->inputs[3]) : NULL;
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	if (weights.rank != 4 || neural_element_count(weights) < 0
		|| params.dilation_height != 1 || params.dilation_width != 1) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	// The shape input must say what the output tensor says, where it
	// can be looked at.
	if (neural_is_constant(shape)) {
		if (shape.type != B_NEURAL_INT32 || shape.rank != 1
			|| shape.dimensions[0] != 4 || output.rank != 4) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
		for (int32 i = 0; i < 4; i++) {
			if (((const int32*)shape.data)[i] != output.dimensions[i])
				return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	}

	ConvData header;
	memset(&header, 0, sizeof(header));
	header.filter_height = weights.dimensions[1];
	header.filter_width = weights.dimensions[2];
	header.filter_channels = weights.dimensions[3];
	header.stride_height = params.stride_height;
	header.stride_width = params.stride_width;
	header.dilation_height = 1;
	header.dilation_width = 1;
	header.groups = 1;

	// The operator undoes the shapes of a convolution from its output to
	// its input: that convolution must come out at our input's size.
	neural_window& window = header.window;
	if (!neural_window_geometry(output, input, params.padding,
			header.filter_height, header.filter_width, params.stride_height,
			params.stride_width, 1, 1, &window)
		|| window.input_channels != weights.dimensions[0]
		|| window.output_channels != weights.dimensions[3]) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	int32 outputChannels = window.input_channels;
	header.patch_size = neural_element_count(weights) / outputChannels;

	NeuralBlock block(sizeof(ConvData));
	size_t offsets[8];
	size_t zeroBias = 0;
	QuantizedSetup setup = { &input, &weights, bias, &output,
		params.activation, outputChannels, 0, header.patch_size,
		header.patch_size, false };

	if (input.type == B_NEURAL_FLOAT32) {
		if (!check_float(input, weights, bias, output, outputChannels)
			|| neural_float_activation_range(params.activation,
				&header.float_min, &header.float_max) != B_OK) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
		zeroBias = block.Reserve(outputChannels, sizeof(float));
	} else {
		if (!check_quantized(setup))
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		reserve_quantized(setup, block, offsets);
	}

	ConvData* data = (ConvData*)block.Allocate();
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;

	if (input.type == B_NEURAL_FLOAT32) {
		data->zero_bias = block.At<float>(zeroBias);
	} else {
		status_t status = fill_quantized(setup, block, offsets, data);
		if (status != B_OK) {
			free(data);
			return status;
		}
	}

	*_prepared = data;
	return B_OK;
}


static status_t
run_transpose_conv(const NeuralOpContext& context)
{
	const ConvData* data = (const ConvData*)context.prepared;
	const neural_window& window = data->window;

	ConvRun run;
	run.data = data;
	run.input = context.Input<void>(2);
	run.weights = context.Input<void>(1);
	run.bias = context.HasInput(3)
		? context.Input<void>(3) : (const void*)data->zero_bias;
	run.output = context.Output<void>(0);
	run.status = B_OK;

	int32 rows = window.batches * window.input_height;
	int32 grain = row_grain((int64)window.input_width * window.input_channels
		* ((int64)data->patch_size + 16));

	if (!data->quantized) {
		context.pool->Run(rows, grain,
			&transpose_conv_rows<float, float, float>, &run);
		return run.status;
	}

	// the whole input less its zero point
	const neural_graph_tensor& input = context.InputInfo(2);
	int32 elements = neural_element_count(input);
	int16* converted = (int16*)malloc((size_t)elements * sizeof(int16));
	if (converted == NULL)
		return B_NO_MEMORY;

	bool isUnsigned = input.type == B_NEURAL_UINT8;
	if (isUnsigned) {
		convert_row(context.Input<uint8>(2), data->input_offset, converted,
			elements);
	} else {
		convert_row(context.Input<int8>(2), data->input_offset, converted,
			elements);
	}

	run.input = converted;
	context.pool->Run(rows, grain, isUnsigned
		? &transpose_conv_rows<uint8, int16, int32>
		: &transpose_conv_rows<int8, int16, int32>, &run);

	free(converted);
	return run.status;
}


}	// unnamed namespace


const NeuralKernel kConvKernel = {
	B_NEURAL_OP_CONV_2D, &prepare_conv, &run_conv
};

const NeuralKernel kDepthwiseConvKernel = {
	B_NEURAL_OP_DEPTHWISE_CONV_2D, &prepare_depthwise, &run_depthwise
};

const NeuralKernel kFullyConnectedKernel = {
	B_NEURAL_OP_FULLY_CONNECTED, &prepare_fully_connected,
	&run_fully_connected
};

const NeuralKernel kTransposeConvKernel = {
	B_NEURAL_OP_TRANSPOSE_CONV, &prepare_transpose_conv, &run_transpose_conv
};


}	// namespace BPrivate
