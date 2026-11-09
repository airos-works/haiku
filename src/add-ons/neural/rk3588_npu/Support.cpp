/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Support.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>


// What the registers have room for.
static const uint32 kMaxInputSize = 2047;
static const uint32 kMaxOutputSize = 8191;
static const uint32 kMaxChannels = 8191;
static const uint32 kMaxKernels = 16382;
static const uint32 kMaxBytesPerKernel = 0x7ffff;
static const uint32 kMaxPadding = 15;

// What has been tried on the hardware, see rk3588_npu_backend_test.
static const uint32 kMaxKernelSize = 7;
static const uint32 kMaxStride = 2;

// The fewest pixels an addend may have: below, the vendor's driver sets
// the readers up in a way Mesa could only copy for the sizes it saw.
static const uint32 kMinAdditionPixels = 12;


// #pragma mark - GraphUse


GraphUse::GraphUse()
	:
	fProducers(NULL),
	fConsumers(NULL),
	fOutputs(NULL)
{
}


GraphUse::~GraphUse()
{
	free(fProducers);
	free(fConsumers);
	free(fOutputs);
}


status_t
GraphUse::Init(const BNeuralGraph& graph)
{
	int32 tensorCount = graph.CountTensors();
	size_t count = tensorCount > 0 ? tensorCount : 1;

	fProducers = (int32*)malloc(count * sizeof(int32));
	fConsumers = (int32*)calloc(count, sizeof(int32));
	fOutputs = (bool*)calloc(count, sizeof(bool));
	if (fProducers == NULL || fConsumers == NULL || fOutputs == NULL)
		return B_NO_MEMORY;

	for (int32 i = 0; i < tensorCount; i++)
		fProducers[i] = -1;

	for (int32 i = 0; i < graph.CountOperators(); i++) {
		const neural_graph_operator* op = graph.OperatorAt(i);
		for (int32 j = 0; j < op->output_count; j++) {
			int32 tensor = op->outputs[j];
			if (tensor >= 0 && tensor < tensorCount)
				fProducers[tensor] = i;
		}
		for (int32 j = 0; j < op->input_count; j++) {
			int32 tensor = op->inputs[j];
			if (tensor >= 0 && tensor < tensorCount)
				fConsumers[tensor]++;
		}
	}

	for (int32 i = 0; i < graph.CountOutputs(); i++) {
		int32 tensor = graph.OutputAt(i);
		if (tensor >= 0 && tensor < tensorCount)
			fOutputs[tensor] = true;
	}

	return B_OK;
}


// #pragma mark - tensors


static inline bool
is_byte_type(neural_data_type type)
{
	return type == B_NEURAL_UINT8 || type == B_NEURAL_INT8;
}


/*!	A tensor of activations the processor can hold: one picture of
	quantized bytes with one scale. */
static bool
is_feature_tensor(const neural_graph_tensor* tensor)
{
	if (tensor == NULL || tensor->data != NULL || !is_byte_type(tensor->type)
		|| !tensor->quantized || tensor->channel_count != 0
		|| tensor->rank != 4 || tensor->dimensions[0] != 1) {
		return false;
	}
	for (int32 i = 1; i < 4; i++) {
		if (tensor->dimensions[i] < 1)
			return false;
	}
	return tensor->scale > 0 && isfinite(tensor->scale);
}


//! The zero point of a tensor as the processor stores it.
static inline int32
device_zero_point(const neural_graph_tensor* tensor)
{
	return tensor->zero_point - (tensor->type == B_NEURAL_UINT8 ? 0x80 : 0);
}


static inline bool
zero_point_valid(const neural_graph_tensor* tensor)
{
	int32 zeroPoint = device_zero_point(tensor);
	return zeroPoint >= -128 && zeroPoint <= 127;
}


/*!	The processor ends every operation by clamping to what a byte holds
	and knows no other activation (as this back end uses it). That is the
	activation asked for when the range of the output tensor ends where
	the activation's does, which is how quantized networks are made. */
static bool
activation_is_clamp(neural_activation activation,
	const neural_graph_tensor* output)
{
	double minimum = output->type == B_NEURAL_UINT8 ? 0 : -128;
	double maximum = output->type == B_NEURAL_UINT8 ? 255 : 127;
	double zeroPoint = output->zero_point;
	double scale = output->scale;

	switch (activation) {
		case B_NEURAL_ACTIVATION_NONE:
			return true;
		case B_NEURAL_ACTIVATION_RELU:
			return zeroPoint <= minimum;
		case B_NEURAL_ACTIVATION_RELU6:
			return zeroPoint <= minimum
				&& zeroPoint + round(6.0 / scale) >= maximum;
		case B_NEURAL_ACTIVATION_RELU_N1_TO_1:
			return zeroPoint + round(-1.0 / scale) <= minimum
				&& zeroPoint + round(1.0 / scale) >= maximum;
		default:
			return false;
	}
}


// #pragma mark - operators


bool
tasks_possible(const Operation& operation)
{
	Operation copy = operation;
	copy.tasks = NULL;
	copy.task_count = 0;

	status_t status = split_tasks(copy);
	free(copy.tasks);
	return status == B_OK;
}


status_t
describe_convolution(const BNeuralGraph& graph, int32 index,
	Operation& operation)
{
	const neural_graph_operator* op = graph.OperatorAt(index);
	if (op == NULL || (op->code != B_NEURAL_OP_CONV_2D
			&& op->code != B_NEURAL_OP_DEPTHWISE_CONV_2D)
		|| op->input_count < 2 || op->output_count != 1
		|| op->params == NULL) {
		return B_NOT_SUPPORTED;
	}

	const neural_conv_params* params = (const neural_conv_params*)op->params;
	const neural_graph_tensor* input = graph.TensorAt(op->inputs[0]);
	const neural_graph_tensor* weights = graph.TensorAt(op->inputs[1]);
	const neural_graph_tensor* output = graph.TensorAt(op->outputs[0]);
	int32 biasIndex = op->input_count > 2 ? op->inputs[2] : -1;

	if (!is_feature_tensor(input) || !is_feature_tensor(output)
		|| input->type != output->type || !zero_point_valid(input)
		|| !zero_point_valid(output)) {
		return B_NOT_SUPPORTED;
	}

	// The weights: constant bytes with one scale. Weights with a scale
	// per channel would need a multiplier per channel after the sums.
	if (weights == NULL || weights->data == NULL
		|| !is_byte_type(weights->type) || !weights->quantized
		|| weights->rank != 4 || !zero_point_valid(weights)) {
		return B_NOT_SUPPORTED;
	}
	float weightsScale = weights->scale;
	if (weights->channel_count > 0) {
		if (weights->channel_scales == NULL || weights->zero_point != 0)
			return B_NOT_SUPPORTED;
		weightsScale = weights->channel_scales[0];
		for (int32 i = 1; i < weights->channel_count; i++) {
			if (weights->channel_scales[i] != weightsScale)
				return B_NOT_SUPPORTED;
		}
	}
	if (!(weightsScale > 0) || !isfinite(weightsScale))
		return B_NOT_SUPPORTED;

	memset(&operation, 0, sizeof(operation));
	operation.add_tensor = -1;
	operation.addend = -1;
	operation.input = -1;
	operation.output = -1;

	operation.depthwise = op->code == B_NEURAL_OP_DEPTHWISE_CONV_2D;
	operation.padding_same = params->padding == B_NEURAL_PADDING_SAME;
	operation.stride = params->stride_width;

	operation.input_tensor = op->inputs[0];
	operation.output_tensor = op->outputs[0];
	operation.weights_tensor = op->inputs[1];
	operation.bias_tensor = biasIndex;

	operation.input_height = input->dimensions[1];
	operation.input_width = input->dimensions[2];
	operation.input_channels = input->dimensions[3];
	operation.input_zero_point = device_zero_point(input);
	operation.input_scale = input->scale;

	operation.output_height = output->dimensions[1];
	operation.output_width = output->dimensions[2];
	operation.output_channels = output->dimensions[3];
	operation.output_zero_point = device_zero_point(output);
	operation.output_scale = output->scale;

	operation.weights_height = weights->dimensions[1];
	operation.weights_width = weights->dimensions[2];
	operation.weights_signed = weights->type == B_NEURAL_INT8;
	operation.weights_zero_point = device_zero_point(weights);
	operation.weights_scale = weightsScale;

	if (params->padding != B_NEURAL_PADDING_SAME
		&& params->padding != B_NEURAL_PADDING_VALID) {
		return B_NOT_SUPPORTED;
	}
	if (params->dilation_width != 1 || params->dilation_height != 1
		|| params->stride_width != params->stride_height
		|| operation.stride < 1 || operation.stride > kMaxStride) {
		return B_NOT_SUPPORTED;
	}
	if (operation.weights_width != operation.weights_height
		|| operation.weights_width < 1
		|| operation.weights_width > kMaxKernelSize) {
		return B_NOT_SUPPORTED;
	}

	// The channels. A single input channel the processor wants in a
	// layout of its own, which this back end does not make.
	if (operation.input_channels < 2
		|| operation.input_channels > kMaxChannels
		|| operation.output_channels > kMaxChannels) {
		return B_NOT_SUPPORTED;
	}
	if (operation.depthwise) {
		if (params->depth_multiplier != 1 || weights->dimensions[0] != 1
			|| (uint32)weights->dimensions[3] != operation.input_channels
			|| operation.output_channels != operation.input_channels) {
			return B_NOT_SUPPORTED;
		}
	} else {
		if ((uint32)weights->dimensions[0] != operation.output_channels
			|| (uint32)weights->dimensions[3] != operation.input_channels
			|| operation.output_channels > kMaxKernels) {
			return B_NOT_SUPPORTED;
		}
	}
	if (operation.weights_width * operation.weights_height
			* round_up(operation.input_channels, FEATURE_ATOMIC_SIZE)
			> kMaxBytesPerKernel) {
		return B_NOT_SUPPORTED;
	}

	// the bias: one number of 32 bits per output channel
	if (biasIndex >= 0) {
		const neural_graph_tensor* bias = graph.TensorAt(biasIndex);
		if (bias == NULL || bias->data == NULL
			|| bias->type != B_NEURAL_INT32
			|| bias->size != operation.output_channels * sizeof(int32)) {
			return B_NOT_SUPPORTED;
		}
	}

	// The sizes, and that the output is as large as the padding makes it.
	if (operation.input_width > kMaxInputSize
		|| operation.input_height > kMaxInputSize
		|| operation.output_width > kMaxOutputSize
		|| operation.output_height > kMaxOutputSize) {
		return B_NOT_SUPPORTED;
	}
	uint32 expectedWidth;
	uint32 expectedHeight;
	if (operation.padding_same) {
		expectedWidth = divide_up(operation.input_width, operation.stride);
		expectedHeight = divide_up(operation.input_height, operation.stride);

		int32 padding = (int32)((expectedWidth - 1) * operation.stride
			+ operation.weights_width) - (int32)operation.input_width;
		int32 paddingHeight = (int32)((expectedHeight - 1) * operation.stride
			+ operation.weights_height) - (int32)operation.input_height;
		if (paddingHeight > padding)
			padding = paddingHeight;
		if (padding > 0 && ((uint32)(padding + 1) / 2 > kMaxPadding
				|| (uint32)(padding + 1) / 2 >= operation.weights_width)) {
			return B_NOT_SUPPORTED;
		}
	} else {
		if (operation.input_width < operation.weights_width
			|| operation.input_height < operation.weights_height) {
			return B_NOT_SUPPORTED;
		}
		expectedWidth = (operation.input_width - operation.weights_width)
			/ operation.stride + 1;
		expectedHeight = (operation.input_height - operation.weights_height)
			/ operation.stride + 1;
	}
	if (operation.output_width != expectedWidth
		|| operation.output_height != expectedHeight) {
		return B_NOT_SUPPORTED;
	}

	if (!activation_is_clamp(params->activation, output))
		return B_NOT_SUPPORTED;

	uint32 scale;
	uint32 shift;
	if (!output_conversion((double)operation.input_scale
			* operation.weights_scale / operation.output_scale, &scale,
			&shift)) {
		return B_NOT_SUPPORTED;
	}

	if (!tasks_possible(operation))
		return B_NOT_SUPPORTED;

	return B_OK;
}


status_t
find_addition_target(const BNeuralGraph& graph, const GraphUse& use,
	int32 index, const bool* inProgram, int32* _convolution, int32* _addend)
{
	const neural_graph_operator* op = graph.OperatorAt(index);
	if (op == NULL || op->code != B_NEURAL_OP_ADD || op->input_count != 2
		|| op->output_count != 1) {
		return B_NOT_SUPPORTED;
	}

	int32 a = op->inputs[0];
	int32 b = op->inputs[1];
	if (a < 0 || b < 0 || a == b)
		return B_NOT_SUPPORTED;

	// The convolution has to be the later of the two makers: the other
	// operand must be there when it runs.
	int32 sum = use.Producer(a) > use.Producer(b) ? a : b;
	int32 addend = sum == a ? b : a;
	int32 convolution = use.Producer(sum);
	if (convolution < 0 || convolution >= index
		|| use.Producer(addend) >= convolution) {
		return B_NOT_SUPPORTED;
	}

	const neural_graph_operator* target = graph.OperatorAt(convolution);
	if ((target->code != B_NEURAL_OP_CONV_2D
			&& target->code != B_NEURAL_OP_DEPTHWISE_CONV_2D)
		|| target->params == NULL
		|| ((const neural_conv_params*)target->params)->activation
			!= B_NEURAL_ACTIVATION_NONE) {
		return B_NOT_SUPPORTED;
	}

	// Its output is not made any more, so nobody else may want it ...
	if (use.CountConsumers(sum) != 1 || use.IsGraphOutput(sum))
		return B_NOT_SUPPORTED;

	// ... and both have to end up in one program.
	for (int32 i = convolution; i < index; i++) {
		if (!inProgram[i])
			return B_NOT_SUPPORTED;
	}

	*_convolution = convolution;
	*_addend = addend;
	return B_OK;
}


status_t
fuse_addition(const BNeuralGraph& graph, int32 index, int32 addendIndex,
	Operation& operation)
{
	const neural_graph_operator* op = graph.OperatorAt(index);
	if (op == NULL || op->code != B_NEURAL_OP_ADD || op->output_count != 1
		|| operation.add_tensor >= 0) {
		return B_NOT_SUPPORTED;
	}

	const neural_graph_tensor* sum = graph.TensorAt(operation.output_tensor);
	const neural_graph_tensor* addend = graph.TensorAt(addendIndex);
	const neural_graph_tensor* output = graph.TensorAt(op->outputs[0]);

	if (!is_feature_tensor(addend) || !is_feature_tensor(output)
		|| addend->type != sum->type || output->type != sum->type
		|| !zero_point_valid(addend) || !zero_point_valid(output)) {
		return B_NOT_SUPPORTED;
	}
	// no stretching of one operand to the shape of the other
	for (int32 i = 1; i < 4; i++) {
		if (addend->dimensions[i] != sum->dimensions[i]
			|| output->dimensions[i] != sum->dimensions[i]) {
			return B_NOT_SUPPORTED;
		}
	}
	if (operation.output_width * operation.output_height
			< kMinAdditionPixels) {
		return B_NOT_SUPPORTED;
	}

	neural_activation activation = op->params != NULL
		? ((const neural_binary_params*)op->params)->activation
		: B_NEURAL_ACTIVATION_NONE;
	if (!activation_is_clamp(activation, output))
		return B_NOT_SUPPORTED;

	double sumScale = (double)operation.input_scale * operation.weights_scale;
	uint32 scale;
	uint32 shift;
	if (!output_conversion(sumScale / output->scale, &scale, &shift)
		|| !addend_conversion(addend->scale / sumScale, &scale, &shift)) {
		return B_NOT_SUPPORTED;
	}

	operation.add_tensor = addendIndex;
	operation.addend_zero_point = device_zero_point(addend);
	operation.addend_scale = addend->scale;

	operation.output_tensor = op->outputs[0];
	operation.output_zero_point = device_zero_point(output);
	operation.output_scale = output->scale;

	return B_OK;
}
