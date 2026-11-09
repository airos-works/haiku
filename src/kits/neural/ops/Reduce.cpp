/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	MEAN: the average over some of the dimensions of a tensor. Quantized,
	the sums are brought to the scale of the output in fixed point with
	the division folded into the factor, as the TensorFlow Lite reference
	kernel does it.
*/


#pragma GCC optimize("O3", "fp-contract=off")


#include "Kernels.h"


namespace BPrivate {


namespace {


struct MeanData {
	int32	rank;
	int32	size[B_NEURAL_MAX_RANK];
	//! elements to step in the output per dimension; 0: averaged over
	int32	output_step[B_NEURAL_MAX_RANK];
	int32	output_elements;
	//! elements that go into each average
	int32	count;

	int32	input_zero_point;
	int32	output_zero_point;
	int32	multiplier;
	int		shift;
	int32	minimum;
	int32	maximum;
};


/*!	Adds every element of the input to the sum it belongs to. The last
	dimension is the inner loop: either all of it goes to one sum, or each
	element to the next one. */
template<typename Type, typename Sum>
static void
sum_up(const MeanData& data, const Type* input, Sum* sums)
{
	int32 rank = data.rank;
	int32 index[B_NEURAL_MAX_RANK] = {};
	int32 count = data.size[rank - 1];
	bool reduceLast = data.output_step[rank - 1] == 0;
	int32 offset = 0;

	while (true) {
		Sum* target = sums + offset;
		if (reduceLast) {
			Sum sum = target[0];
			for (int32 i = 0; i < count; i++)
				sum += input[i];
			target[0] = sum;
		} else {
			for (int32 i = 0; i < count; i++)
				target[i] += input[i];
		}
		input += count;

		int32 dimension = rank - 2;
		for (; dimension >= 0; dimension--) {
			offset += data.output_step[dimension];
			if (++index[dimension] < data.size[dimension])
				break;
			offset -= index[dimension] * data.output_step[dimension];
			index[dimension] = 0;
		}
		if (dimension < 0)
			break;
	}
}


template<typename Type>
static status_t
run_quantized(const NeuralOpContext& context)
{
	const MeanData& data = *(const MeanData*)context.prepared;
	int32* sums = (int32*)calloc(data.output_elements, sizeof(int32));
	if (sums == NULL)
		return B_NO_MEMORY;

	sum_up(data, context.Input<Type>(0), sums);

	Type* output = context.Output<Type>(0);
	int32 zeroSum = data.input_zero_point * data.count;
	for (int32 i = 0; i < data.output_elements; i++) {
		int32 value = neural_multiply_by_quantized_multiplier(
			sums[i] - zeroSum, data.multiplier, data.shift)
			+ data.output_zero_point;
		output[i] = (Type)neural_clamp(value, data.minimum, data.maximum);
	}

	free(sums);
	return B_OK;
}


static status_t
run_mean(const NeuralOpContext& context)
{
	const MeanData& data = *(const MeanData*)context.prepared;
	switch (context.InputInfo(0).type) {
		case B_NEURAL_UINT8:
			return run_quantized<uint8>(context);
		case B_NEURAL_INT8:
			return run_quantized<int8>(context);
		default:
			break;
	}

	float* output = context.Output<float>(0);
	for (int32 i = 0; i < data.output_elements; i++)
		output[i] = 0;
	sum_up(data, context.Input<float>(0), output);
	for (int32 i = 0; i < data.output_elements; i++)
		output[i] = output[i] / (float)data.count;
	return B_OK;
}


static status_t
prepare_mean(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	if (op->input_count != 2 || !neural_has_inputs(op, 2)
		|| op->output_count != 1 || op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_reduce_params& params
		= *(const neural_reduce_params*)op->params;
	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& axes = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);
	int32 rank = input.rank;
	int32 axisCount = neural_element_count(axes);

	if (input.type != output.type || rank < 1
		|| neural_element_count(input) < 0
		|| neural_element_count(output) < 0
		|| !neural_is_constant(axes) || !neural_is_index_type(axes.type)
		|| axes.rank > 1 || axisCount < 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	// which dimensions go; one may be named more than once
	bool reduced[B_NEURAL_MAX_RANK] = {};
	for (int32 i = 0; i < axisCount; i++) {
		int64 axis = neural_constant_int(axes, i);
		if (axis < 0)
			axis += rank;
		if (axis < 0 || axis >= rank)
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		reduced[axis] = true;
	}

	// the shape the output must have, and how to walk it
	MeanData header;
	memset(&header, 0, sizeof(header));
	header.rank = rank;
	header.output_elements = neural_element_count(output);

	int32 outputRank = 0;
	for (int32 i = 0; i < rank; i++) {
		if (!reduced[i] || params.keep_dimensions)
			outputRank++;
	}
	if (output.rank != outputRank)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	int64 count = 1;
	int32 step = 1;
	int32 outputIndex = outputRank;
	for (int32 i = rank - 1; i >= 0; i--) {
		header.size[i] = input.dimensions[i];
		if (reduced[i]) {
			count *= input.dimensions[i];
			if (params.keep_dimensions && output.dimensions[--outputIndex] != 1)
				return B_NEURAL_UNSUPPORTED_OPERATOR;
		} else {
			if (output.dimensions[--outputIndex] != input.dimensions[i])
				return B_NEURAL_UNSUPPORTED_OPERATOR;
			header.output_step[i] = step;
			step *= input.dimensions[i];
		}
	}
	header.count = (int32)count;

	if (input.type == B_NEURAL_FLOAT32) {
		// nothing more to it
	} else if (neural_is_quantized_8(input) && neural_is_quantized_8(output)
		&& neural_valid_zero_point(input)
		&& neural_valid_zero_point(output)) {
		// the sums are kept in 32 bits
		if (count > (1 << 23))
			return B_NEURAL_UNSUPPORTED_OPERATOR;

		neural_type_range(input.type, &header.minimum, &header.maximum);
		header.input_zero_point = input.zero_point;
		header.output_zero_point = output.zero_point;

		int32 multiplier;
		int shift;
		if (!neural_checked_quantize_multiplier((double)input.scale
				/ (double)output.scale, &multiplier, &shift)) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}

		// 1 / count goes into the factor: as many bits of it as there is
		// room for move from the shift to the factor first, so that the
		// division loses as little as possible.
		int bits = 63 - __builtin_clzll((uint64)count);
		if (bits > 31 + shift)
			bits = 31 + shift;
		header.multiplier = (int32)(((int64)multiplier << bits) / count);
		header.shift = shift - bits;
	} else
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	MeanData* data = (MeanData*)malloc(sizeof(MeanData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


}	// unnamed namespace


const NeuralKernel kMeanKernel = {
	B_NEURAL_OP_MEAN, &prepare_mean, &run_mean
};


}	// namespace BPrivate
