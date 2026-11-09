/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Pooling: the average or the largest of the values under a window,
	channel by channel. Padding does not count: at the edges the window is
	what is left of it within the picture.
*/


#pragma GCC optimize("O3")


#include "Kernels.h"

#include <algorithm>


namespace BPrivate {


namespace {


struct PoolData {
	neural_window	window;
	int32			filter_height;
	int32			filter_width;
	int32			stride_height;
	int32			stride_width;
	int32			activation_min;
	int32			activation_max;
	float			float_min;
	float			float_max;
};


struct PoolRun {
	const PoolData*	data;
	const void*		input;
	void*			output;
	int32			status;
};


// The average of a window from the sum of its values, rounded as the
// reference does: unsigned values half up, signed ones half away from
// zero.

static inline uint8
average(const PoolData& data, int32 sum, int32 count, uint8*)
{
	return (uint8)neural_clamp((sum + count / 2) / count,
		data.activation_min, data.activation_max);
}


static inline int8
average(const PoolData& data, int32 sum, int32 count, int8*)
{
	int32 value = sum > 0 ? (sum + count / 2) / count
		: (sum - count / 2) / count;
	return (int8)neural_clamp(value, data.activation_min,
		data.activation_max);
}


static inline float
average(const PoolData& data, float sum, int32 count, float*)
{
	return neural_clamp(sum / (float)count, data.float_min, data.float_max);
}


static inline uint8
limit(const PoolData& data, uint8 value)
{
	return (uint8)neural_clamp((int32)value, data.activation_min,
		data.activation_max);
}


static inline int8
limit(const PoolData& data, int8 value)
{
	return (int8)neural_clamp((int32)value, data.activation_min,
		data.activation_max);
}


static inline float
limit(const PoolData& data, float value)
{
	return neural_clamp(value, data.float_min, data.float_max);
}


/*!	Output rows \a first to \a end, counted through all batches. \a Sum
	is what the values of a window add up in; for the largest value it is
	the type itself. */
template<typename Type, typename Sum, bool kAverage>
static void
pool_rows(void* cookie, int32 first, int32 end)
{
	NeuralFlushDenormals flush;
	PoolRun* run = (PoolRun*)cookie;
	const PoolData& data = *run->data;
	const neural_window& window = data.window;
	int32 channels = window.input_channels;

	Sum* sums = (Sum*)malloc((size_t)channels * sizeof(Sum));
	if (sums == NULL) {
		__atomic_store_n(&run->status, (int32)B_NO_MEMORY,
			__ATOMIC_RELAXED);
		return;
	}

	size_t inputRowSize = (size_t)window.input_width * channels;
	size_t inputSize = (size_t)window.input_height * inputRowSize;
	size_t outputRowSize = (size_t)window.output_width * channels;

	for (int32 index = first; index < end; index++) {
		int32 batch = index / window.output_height;
		int32 y = index % window.output_height;
		const Type* input = (const Type*)run->input + batch * inputSize;
		Type* output = (Type*)run->output + index * outputRowSize;

		// 64 bits: a window may be as large as an int32 holds
		int64 top = (int64)y * data.stride_height - window.pad_top;
		int32 firstY = top > 0 ? (int32)top : 0;
		int32 endY = (int32)std::min(top + data.filter_height,
			(int64)window.input_height);

		for (int32 x = 0; x < window.output_width; x++, output += channels) {
			int64 left = (int64)x * data.stride_width - window.pad_left;
			int32 firstX = left > 0 ? (int32)left : 0;
			int32 endX = (int32)std::min(left + data.filter_width,
				(int64)window.input_width);

			int32 count = 0;
			if (endY > firstY && endX > firstX)
				count = (endY - firstY) * (endX - firstX);
			if (count == 0) {
				// a window that is all padding: cannot be, but if it is
				for (int32 i = 0; i < channels; i++)
					output[i] = limit(data, (Type)0);
				continue;
			}

			if (kAverage) {
				for (int32 i = 0; i < channels; i++)
					sums[i] = 0;
			} else {
				const Type* values = input + firstY * inputRowSize
					+ (size_t)firstX * channels;
				for (int32 i = 0; i < channels; i++)
					sums[i] = values[i];
			}

			for (int32 inputY = firstY; inputY < endY; inputY++) {
				const Type* values = input + inputY * inputRowSize
					+ (size_t)firstX * channels;
				for (int32 inputX = firstX; inputX < endX; inputX++,
						values += channels) {
					if (kAverage) {
						for (int32 i = 0; i < channels; i++)
							sums[i] += values[i];
					} else {
						for (int32 i = 0; i < channels; i++) {
							sums[i] = values[i] > sums[i]
								? (Sum)values[i] : sums[i];
						}
					}
				}
			}

			if (kAverage) {
				for (int32 i = 0; i < channels; i++)
					output[i] = average(data, sums[i], count, (Type*)NULL);
			} else {
				for (int32 i = 0; i < channels; i++)
					output[i] = limit(data, (Type)sums[i]);
			}
		}
	}

	free(sums);
}


static status_t
prepare_pool(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	if (op->input_count != 1 || op->inputs[0] < 0 || op->output_count != 1
		|| op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_pool_params& params = *(const neural_pool_params*)op->params;
	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	PoolData header;
	memset(&header, 0, sizeof(header));
	header.filter_height = params.filter_height;
	header.filter_width = params.filter_width;
	header.stride_height = params.stride_height;
	header.stride_width = params.stride_width;

	const neural_window& window = header.window;
	if (input.type != output.type
		|| !neural_window_geometry(input, output, params.padding,
			params.filter_height, params.filter_width, params.stride_height,
			params.stride_width, 1, 1, &header.window)
		|| window.input_channels != window.output_channels) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	// what the values under a window add up to has to fit 32 bits
	int64 area = (int64)std::min(params.filter_height, window.input_height)
		* std::min(params.filter_width, window.input_width);
	if (area > (1 << 23))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	if (input.type == B_NEURAL_FLOAT32) {
		if (neural_float_activation_range(params.activation,
				&header.float_min, &header.float_max) != B_OK) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	} else {
		// The values go through as they are: input and output must be
		// quantized the same way.
		if (!neural_is_quantized_8(input)
			|| !neural_same_quantization(input, output)
			|| neural_quantized_activation_range(params.activation, output,
				&header.activation_min, &header.activation_max) != B_OK) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	}

	PoolData* data = (PoolData*)malloc(sizeof(PoolData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


template<bool kAverage>
static status_t
run_pool(const NeuralOpContext& context)
{
	const PoolData* data = (const PoolData*)context.prepared;
	const neural_window& window = data->window;

	PoolRun run;
	run.data = data;
	run.input = context.Input<void>(0);
	run.output = context.Output<void>(0);
	run.status = B_OK;

	NeuralThreadPool::Function function;
	switch (context.InputInfo(0).type) {
		case B_NEURAL_FLOAT32:
			function = kAverage ? &pool_rows<float, float, true>
				: &pool_rows<float, float, false>;
			break;
		case B_NEURAL_UINT8:
			function = kAverage ? &pool_rows<uint8, int32, true>
				: &pool_rows<uint8, uint8, false>;
			break;
		default:
			function = kAverage ? &pool_rows<int8, int32, true>
				: &pool_rows<int8, int8, false>;
			break;
	}

	int64 work = (int64)window.output_width * window.output_channels
		* std::min(data->filter_height, window.input_height)
		* std::min(data->filter_width, window.input_width);
	int64 grain = 400000 / (work > 0 ? work : 1);
	context.pool->Run(window.batches * window.output_height,
		grain < 1 ? 1 : (int32)grain, function, &run);
	return run.status;
}


}	// unnamed namespace


const NeuralKernel kAveragePoolKernel = {
	B_NEURAL_OP_AVERAGE_POOL_2D, &prepare_pool, &run_pool<true>
};

const NeuralKernel kMaxPoolKernel = {
	B_NEURAL_OP_MAX_POOL_2D, &prepare_pool, &run_pool<false>
};


}	// namespace BPrivate
