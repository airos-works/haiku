/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Pictures made larger or smaller: by taking the nearest element, or by
	weighing the four around a position. Where a position of the output
	lies in the input depends on two parameters: with "align corners" the
	corner elements of both lie on each other, with "half pixel centers"
	the centres of the elements are what is compared instead of their
	upper left corners.

	Each kind of element is computed the way the TensorFlow Lite reference
	does it: unsigned and floating point elements in single precision in
	one fixed order of operations, signed ones with 10 bit fractions.
*/


// The order of the floating point operations is part of the result: no
// merging into multiply-add instructions.
#pragma GCC optimize("O3", "fp-contract=off")


#include "Kernels.h"


namespace BPrivate {


namespace {


//! Where an output row or column comes from.
struct ResizeSource {
	//! the two elements weighed (nearest: the first is the one)
	int32	lower;
	int32	upper;
	//! weight of the upper one: floating point, and of 1024
	float	fraction;
	int32	fixed_fraction;
};


struct ResizeData {
	int32			batches;
	int32			input_height;
	int32			input_width;
	int32			output_height;
	int32			output_width;
	int32			channels;
	int32			element_size;
	ResizeSource*	rows;
	ResizeSource*	columns;
};


struct ResizeRun {
	const ResizeData*	data;
	const void*			input;
	void*				output;
};


static void
nearest_source(int32 inputSize, int32 outputSize,
	const neural_resize_params& params, ResizeSource* sources)
{
	float scale = params.align_corners && outputSize > 1
		? (inputSize - 1) / (float)(outputSize - 1)
		: inputSize / (float)outputSize;
	float offset = params.half_pixel_centers ? 0.5f : 0.0f;

	for (int32 i = 0; i < outputSize; i++) {
		float position = (i + offset) * scale;
		position = params.align_corners ? roundf(position) : floorf(position);
		// compared before it is made an integer: it may be large
		int32 source = position >= inputSize - 1 ? inputSize - 1
			: (position > 0 ? (int32)position : 0);
		sources[i].lower = sources[i].upper = source;
	}
}


static void
bilinear_source(int32 inputSize, int32 outputSize,
	const neural_resize_params& params, ResizeSource* sources)
{
	float scale = (float)inputSize / outputSize;
	if (params.align_corners && outputSize > 1)
		scale = (float)(inputSize - 1) / (outputSize - 1);

	for (int32 i = 0; i < outputSize; i++) {
		float position = params.half_pixel_centers
			? (i + 0.5f) * scale - 0.5f : i * scale;
		float lower = floorf(position);
		float upper = ceilf(position);
		sources[i].lower = lower > 0 ? (lower < inputSize - 1
			? (int32)lower : inputSize - 1) : 0;
		sources[i].upper = upper < inputSize - 1
			? (upper > 0 ? (int32)upper : 0) : inputSize - 1;
		// may be negative at the first position; the reference lets it
		sources[i].fraction = position - sources[i].lower;
	}
}


//! The same in whole numbers: positions with 10 bits after the point.
static void
bilinear_fixed_source(int32 inputSize, int32 outputSize,
	const neural_resize_params& params, ResizeSource* sources)
{
	int32 scale = ((1 << 10) * inputSize + outputSize / 2) / outputSize;
	if (params.align_corners && outputSize > 1) {
		scale = ((1 << 10) * (inputSize - 1) + (outputSize - 1) / 2)
			/ (outputSize - 1);
	}

	for (int32 i = 0; i < outputSize; i++) {
		int32 position = i * scale;
		if (params.half_pixel_centers)
			position += scale / 2 - (1 << 9);
		int32 lower = position / (1 << 10);
		int32 upper = (position + (1 << 10) - 1) / (1 << 10);
		sources[i].lower = neural_clamp(lower, 0, inputSize - 1);
		sources[i].upper = neural_clamp(upper, 0, inputSize - 1);
		sources[i].fixed_fraction = position - (1 << 10) * sources[i].lower;
	}
}


static void
nearest_rows(void* cookie, int32 first, int32 end)
{
	const ResizeRun& run = *(const ResizeRun*)cookie;
	const ResizeData& data = *run.data;
	size_t pixelSize = (size_t)data.channels * data.element_size;
	size_t inputRowSize = data.input_width * pixelSize;
	size_t outputRowSize = data.output_width * pixelSize;

	for (int32 index = first; index < end; index++) {
		int32 batch = index / data.output_height;
		int32 y = index % data.output_height;
		const uint8* input = (const uint8*)run.input
			+ ((size_t)batch * data.input_height + data.rows[y].lower)
				* inputRowSize;
		uint8* output = (uint8*)run.output + index * outputRowSize;

		for (int32 x = 0; x < data.output_width; x++) {
			memcpy(output + x * pixelSize,
				input + data.columns[x].lower * pixelSize, pixelSize);
		}
	}
}


// A weighted element from the sum the reference makes, in its order.

static inline float
weigh(float a, float b, float c, float d, float fractionY, float fractionX,
	float*)
{
	return a * (1 - fractionY) * (1 - fractionX) + b * fractionY
		* (1 - fractionX) + c * (1 - fractionY) * fractionX
		+ d * fractionY * fractionX;
}


static inline uint8
weigh(float a, float b, float c, float d, float fractionY, float fractionX,
	uint8*)
{
	float value = a * (1 - fractionY) * (1 - fractionX) + b * fractionY
		* (1 - fractionX) + c * (1 - fractionY) * fractionX
		+ d * fractionY * fractionX + 0.5f;
	return !(value > 0) ? 0 : (value >= 255 ? 255 : (uint8)value);
}


template<typename Type>
static void
bilinear_rows(void* cookie, int32 first, int32 end)
{
	const ResizeRun& run = *(const ResizeRun*)cookie;
	const ResizeData& data = *run.data;
	int32 channels = data.channels;
	size_t inputRowSize = (size_t)data.input_width * channels;
	size_t outputRowSize = (size_t)data.output_width * channels;

	for (int32 index = first; index < end; index++) {
		int32 batch = index / data.output_height;
		const ResizeSource& row = data.rows[index % data.output_height];
		const Type* input = (const Type*)run.input
			+ (size_t)batch * data.input_height * inputRowSize;
		const Type* upperRow = input + row.lower * inputRowSize;
		const Type* lowerRow = input + row.upper * inputRowSize;
		Type* output = (Type*)run.output + index * outputRowSize;

		for (int32 x = 0; x < data.output_width; x++) {
			const ResizeSource& column = data.columns[x];
			const Type* a = upperRow + (size_t)column.lower * channels;
			const Type* b = lowerRow + (size_t)column.lower * channels;
			const Type* c = upperRow + (size_t)column.upper * channels;
			const Type* d = lowerRow + (size_t)column.upper * channels;
			for (int32 i = 0; i < channels; i++) {
				output[i] = weigh(a[i], b[i], c[i], d[i], row.fraction,
					column.fraction, (Type*)NULL);
			}
			output += channels;
		}
	}
}


static void
bilinear_fixed_rows(void* cookie, int32 first, int32 end)
{
	const ResizeRun& run = *(const ResizeRun*)cookie;
	const ResizeData& data = *run.data;
	int32 channels = data.channels;
	size_t inputRowSize = (size_t)data.input_width * channels;
	size_t outputRowSize = (size_t)data.output_width * channels;

	for (int32 index = first; index < end; index++) {
		int32 batch = index / data.output_height;
		const ResizeSource& row = data.rows[index % data.output_height];
		const int8* input = (const int8*)run.input
			+ (size_t)batch * data.input_height * inputRowSize;
		const int8* upperRow = input + row.lower * inputRowSize;
		const int8* lowerRow = input + row.upper * inputRowSize;
		int8* output = (int8*)run.output + index * outputRowSize;
		int32 fractionY = row.fixed_fraction;

		for (int32 x = 0; x < data.output_width; x++) {
			const ResizeSource& column = data.columns[x];
			const int8* a = upperRow + (size_t)column.lower * channels;
			const int8* b = lowerRow + (size_t)column.lower * channels;
			const int8* c = upperRow + (size_t)column.upper * channels;
			const int8* d = lowerRow + (size_t)column.upper * channels;
			int32 fractionX = column.fixed_fraction;

			// weights with 20 bits after the point; they add up to one
			int64 weightA = (int64)((1 << 10) - fractionY)
				* ((1 << 10) - fractionX);
			int64 weightB = (int64)fractionY * ((1 << 10) - fractionX);
			int64 weightC = (int64)((1 << 10) - fractionY) * fractionX;
			int64 weightD = (int64)fractionY * fractionX;
			for (int32 i = 0; i < channels; i++) {
				int64 sum = a[i] * weightA + b[i] * weightB + c[i] * weightC
					+ d[i] * weightD;
				// rounded half away from zero
				sum += sum > 0 ? (1 << 19) : -(1 << 19);
				output[i] = (int8)(sum / (1 << 20));
			}
			output += channels;
		}
	}
}


template<bool kBilinear>
static status_t
prepare_resize(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	if (op->input_count != 2 || !neural_has_inputs(op, 2)
		|| op->output_count != 1 || op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_resize_params& params
		= *(const neural_resize_params*)op->params;
	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& size = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	// Weighing takes one of the two ways to place the elements at most;
	// sizes small enough for positions with 10 more bits to fit an int32.
	if (input.type != output.type || input.rank != 4 || output.rank != 4
		|| input.dimensions[0] != output.dimensions[0]
		|| input.dimensions[3] != output.dimensions[3]
		|| neural_element_count(input) < 0
		|| neural_element_count(output) < 0
		|| (kBilinear && params.align_corners && params.half_pixel_centers)
		|| size.type != B_NEURAL_INT32 || size.rank != 1
		|| size.dimensions[0] != 2) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}
	for (int32 i = 1; i <= 2; i++) {
		if (input.dimensions[i] >= (1 << 20)
			|| output.dimensions[i] >= (1 << 20)) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	}

	// the size asked for is the size of the output, where it can be seen
	if (neural_is_constant(size)) {
		const int32* asked = (const int32*)size.data;
		if (asked[0] != output.dimensions[1]
			|| asked[1] != output.dimensions[2]) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	}

	if (kBilinear) {
		// The stored values are weighed: they must mean the same in input
		// and output.
		if (input.type != B_NEURAL_FLOAT32 && (!neural_is_quantized_8(input)
				|| !neural_same_quantization(input, output))) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	}

	NeuralBlock block(sizeof(ResizeData));
	size_t rowsOffset = block.Reserve(output.dimensions[1],
		sizeof(ResizeSource));
	size_t columnsOffset = block.Reserve(output.dimensions[2],
		sizeof(ResizeSource));
	ResizeData* data = (ResizeData*)block.Allocate();
	if (data == NULL)
		return B_NO_MEMORY;

	data->batches = input.dimensions[0];
	data->input_height = input.dimensions[1];
	data->input_width = input.dimensions[2];
	data->output_height = output.dimensions[1];
	data->output_width = output.dimensions[2];
	data->channels = input.dimensions[3];
	data->element_size = NeuralGraphData::TypeSize(input.type);
	data->rows = block.At<ResizeSource>(rowsOffset);
	data->columns = block.At<ResizeSource>(columnsOffset);

	if (!kBilinear) {
		nearest_source(data->input_height, data->output_height, params,
			data->rows);
		nearest_source(data->input_width, data->output_width, params,
			data->columns);
	} else if (input.type == B_NEURAL_INT8) {
		bilinear_fixed_source(data->input_height, data->output_height,
			params, data->rows);
		bilinear_fixed_source(data->input_width, data->output_width, params,
			data->columns);
	} else {
		bilinear_source(data->input_height, data->output_height, params,
			data->rows);
		bilinear_source(data->input_width, data->output_width, params,
			data->columns);
	}

	*_prepared = data;
	return B_OK;
}


template<bool kBilinear>
static status_t
run_resize(const NeuralOpContext& context)
{
	const ResizeData* data = (const ResizeData*)context.prepared;
	ResizeRun run = { data, context.Input<void>(0),
		context.Output<void>(0) };

	NeuralThreadPool::Function function = &nearest_rows;
	if (kBilinear) {
		switch (context.InputInfo(0).type) {
			case B_NEURAL_FLOAT32:
				function = &bilinear_rows<float>;
				break;
			case B_NEURAL_UINT8:
				function = &bilinear_rows<uint8>;
				break;
			default:
				function = &bilinear_fixed_rows;
				break;
		}
	}

	int64 rowWork = (int64)data->output_width * data->channels;
	int64 grain = 32768 / (rowWork > 0 ? rowWork : 1);
	context.pool->Run(data->batches * data->output_height,
		grain < 1 ? 1 : (int32)grain, function, &run);
	return B_OK;
}


}	// unnamed namespace


const NeuralKernel kResizeBilinearKernel = {
	B_NEURAL_OP_RESIZE_BILINEAR, &prepare_resize<true>, &run_resize<true>
};

const NeuralKernel kResizeNearestKernel = {
	B_NEURAL_OP_RESIZE_NEAREST, &prepare_resize<false>, &run_resize<false>
};


}	// namespace BPrivate
