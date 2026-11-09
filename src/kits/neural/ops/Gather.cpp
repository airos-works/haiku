/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	GATHER: slices of a tensor picked by number, the way a network looks
	up the vector of a word or of a position in a table. The elements are
	moved as bytes, whatever their type.

	The numbers come with the input of the network, so they are checked
	on every run: one that is out of range fails the run (as it does in
	TensorFlow Lite) instead of reading what is not there.
*/


#pragma GCC optimize("O3")


#include "Kernels.h"


namespace BPrivate {


namespace {


struct GatherData {
	//! slices are picked this many times: once per index before the axis
	int32	outer_count;
	//! slices there are to pick from
	int32	axis_size;
	//! bytes of a slice
	size_t	slice_size;
	int32	index_count;
	bool	wide_indices;
};


static inline int64
index_at(const void* indices, bool wide, int32 at)
{
	if (wide)
		return ((const int64*)indices)[at];
	return ((const int32*)indices)[at];
}


static status_t
prepare_gather(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	if (op->input_count != 2 || !neural_has_inputs(op, 2)
		|| op->output_count != 1 || op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_gather_params& params
		= *(const neural_gather_params*)op->params;
	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& indices = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	int32 rank = input.rank;
	int32 axis = params.axis < 0 ? params.axis + rank : params.axis;
	int32 indexCount = neural_element_count(indices);
	size_t elementSize = NeuralGraphData::TypeSize(input.type);

	// Indices that are paired with leading dimensions of the input
	// ("batch dimensions") are not implemented.
	if (params.batch_dimensions != 0 || rank < 1 || axis < 0 || axis >= rank
		|| input.type != output.type || elementSize == 0
		|| !neural_is_index_type(indices.type) || indexCount < 0
		|| neural_element_count(input) < 0
		|| neural_element_count(output) < 0
		|| output.rank != rank - 1 + indices.rank) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	// stored values are passed on as they are: they must mean the same
	if ((input.quantized || output.quantized)
		&& !neural_same_quantization(input, output)) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	// the shape of the input with the shape of the indices in place of
	// the axis
	GatherData header;
	memset(&header, 0, sizeof(header));
	header.outer_count = 1;
	header.axis_size = input.dimensions[axis];
	header.slice_size = elementSize;
	header.index_count = indexCount;
	header.wide_indices = indices.type == B_NEURAL_INT64;

	int32 at = 0;
	for (int32 i = 0; i < axis; i++) {
		if (output.dimensions[at++] != input.dimensions[i])
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		header.outer_count *= input.dimensions[i];
	}
	for (int32 i = 0; i < indices.rank; i++) {
		if (output.dimensions[at++] != indices.dimensions[i])
			return B_NEURAL_UNSUPPORTED_OPERATOR;
	}
	for (int32 i = axis + 1; i < rank; i++) {
		if (output.dimensions[at++] != input.dimensions[i])
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		header.slice_size *= input.dimensions[i];
	}

	// indices that are part of the model are checked once, here
	if (neural_is_constant(indices)) {
		for (int32 i = 0; i < indexCount; i++) {
			int64 index = index_at(indices.data, header.wide_indices, i);
			if (index < 0 || index >= header.axis_size)
				return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
	}

	GatherData* data = (GatherData*)malloc(sizeof(GatherData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


static status_t
run_gather(const NeuralOpContext& context)
{
	const GatherData& data = *(const GatherData*)context.prepared;
	const uint8* input = context.Input<uint8>(0);
	const void* indices = context.Input<void>(1);
	uint8* output = context.Output<uint8>(0);
	size_t sliceSize = data.slice_size;

	for (int32 outer = 0; outer < data.outer_count; outer++) {
		const uint8* slices = input
			+ (size_t)outer * data.axis_size * sliceSize;
		for (int32 i = 0; i < data.index_count; i++) {
			int64 index = index_at(indices, data.wide_indices, i);
			if (index < 0 || index >= data.axis_size)
				return B_BAD_INDEX;
			memcpy(output, slices + (size_t)index * sliceSize, sliceSize);
			output += sliceSize;
		}
	}
	return B_OK;
}


}	// unnamed namespace


const NeuralKernel kGatherKernel = {
	B_NEURAL_OP_GATHER, &prepare_gather, &run_gather
};


}	// namespace BPrivate
