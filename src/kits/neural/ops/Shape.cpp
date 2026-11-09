/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


//!	Operators that change the shape of a tensor and leave its numbers alone.


#include "Kernels.h"


namespace BPrivate {


static status_t
prepare_copy(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	if (op->input_count < 1 || op->inputs[0] < 0 || op->output_count != 1)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);
	if (input.type != output.type || input.size != output.size)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	return B_OK;
}


static status_t
run_copy(const NeuralOpContext& context)
{
	memcpy(context.Output<uint8>(0), context.Input<uint8>(0),
		context.OutputInfo(0).size);
	return B_OK;
}


const NeuralKernel kReshapeKernel = {
	B_NEURAL_OP_RESHAPE, &prepare_copy, &run_copy
};

const NeuralKernel kSqueezeKernel = {
	B_NEURAL_OP_SQUEEZE, &prepare_copy, &run_copy
};


}	// namespace BPrivate
