/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "VulkanPlan.h"

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>


static const status_t kUnsupported = B_NEURAL_UNSUPPORTED_OPERATOR;

/*!	The most vec4 a value may have and the most invocations of a step:
	the shaders compute element numbers in 32 bits. */
static const uint64 kMaxVectors = 1 << 28;
static const uint64 kMaxInvocations = 1 << 30;

// the "operation" of binary.comp and unary.comp
enum {
	kBinaryAdd = 0,
	kBinarySub,
	kBinaryMul
};

enum {
	kUnaryRelu = 0,
	kUnaryRelu6,
	kUnaryLogistic,
	kUnaryTanh,
	kUnaryHardSwish
};


static inline uint32
groups_of(uint32 channels)
{
	return (channels + 3) / 4;
}


static inline uint64
align_up(uint64 value, uint64 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


static int64
element_count(const neural_graph_tensor& tensor)
{
	int64 count = 1;
	for (int32 i = 0; i < tensor.rank; i++)
		count *= tensor.dimensions[i];
	return count;
}


static bool
same_shape(const neural_graph_tensor& a, const neural_graph_tensor& b)
{
	if (a.rank != b.rank)
		return false;
	for (int32 i = 0; i < a.rank; i++) {
		if (a.dimensions[i] != b.dimensions[i])
			return false;
	}
	return true;
}


/*!	The limits a fused activation puts on a result. The largest numbers
	stand for "none": the shaders clamp always. */
static bool
activation_range(neural_activation activation, float* _minimum,
	float* _maximum)
{
	switch (activation) {
		case B_NEURAL_ACTIVATION_NONE:
			*_minimum = -FLT_MAX;
			*_maximum = FLT_MAX;
			return true;
		case B_NEURAL_ACTIVATION_RELU:
			*_minimum = 0;
			*_maximum = FLT_MAX;
			return true;
		case B_NEURAL_ACTIVATION_RELU_N1_TO_1:
			*_minimum = -1;
			*_maximum = 1;
			return true;
		case B_NEURAL_ACTIVATION_RELU6:
			*_minimum = 0;
			*_maximum = 6;
			return true;
		default:
			return false;
	}
}


/*!	Output size of a window that slides over \a inputSize elements, and
	the padding before the first one: what TensorFlow Lite computes, and
	the kit's CPU kernels with it. 0 for what cannot be. */
static int32
window_output_size(neural_padding padding, int32 inputSize, int32 filterSize,
	int32 stride, int32 dilation)
{
	int64 effective = (int64)(filterSize - 1) * dilation + 1;
	int64 size;
	if (padding == B_NEURAL_PADDING_SAME)
		size = ((int64)inputSize + stride - 1) / stride;
	else
		size = ((int64)inputSize + stride - effective) / stride;
	return size > 0 ? (int32)size : 0;
}


static int32
window_padding(int32 stride, int32 dilation, int32 inputSize,
	int32 filterSize, int32 outputSize)
{
	int64 effective = (int64)(filterSize - 1) * dilation + 1;
	int64 total = (int64)(outputSize - 1) * stride + effective - inputSize;
	return total > 0 ? (int32)(total / 2) : 0;
}


//! What a window does to a picture, checked.
struct Window {
	int32	batches;
	int32	in_height;
	int32	in_width;
	int32	out_height;
	int32	out_width;
	int32	pad_top;
	int32	pad_left;

	bool SetTo(const neural_graph_tensor& input,
		const neural_graph_tensor& output, neural_padding padding,
		int32 filterHeight, int32 filterWidth, int32 strideHeight,
		int32 strideWidth, int32 dilationHeight, int32 dilationWidth)
	{
		// sizes the shaders multiply in 32 bits without a thought
		const int32 kMost = 1 << 15;
		if (input.dimensions[0] != output.dimensions[0]
			|| (padding != B_NEURAL_PADDING_SAME
				&& padding != B_NEURAL_PADDING_VALID)
			|| filterHeight < 1 || filterHeight >= kMost
			|| filterWidth < 1 || filterWidth >= kMost
			|| strideHeight < 1 || strideHeight >= kMost
			|| strideWidth < 1 || strideWidth >= kMost
			|| dilationHeight < 1 || dilationHeight >= kMost
			|| dilationWidth < 1 || dilationWidth >= kMost
			|| (int64)filterHeight * filterWidth >= kMost) {
			return false;
		}

		batches = input.dimensions[0];
		in_height = input.dimensions[1];
		in_width = input.dimensions[2];
		out_height = output.dimensions[1];
		out_width = output.dimensions[2];
		if (in_height >= kMost || in_width >= kMost
			|| out_height != window_output_size(padding, in_height,
				filterHeight, strideHeight, dilationHeight)
			|| out_width != window_output_size(padding, in_width,
				filterWidth, strideWidth, dilationWidth)) {
			return false;
		}

		pad_top = window_padding(strideHeight, dilationHeight, in_height,
			filterHeight, out_height);
		pad_left = window_padding(strideWidth, dilationWidth, in_width,
			filterWidth, out_width);
		return true;
	}
};


//! The push constants of a step, after the two words every shader has.
class VulkanPlan::Params {
public:
	Params()
		:
		fCount(2)
	{
		memset(fWords, 0, sizeof(fWords));
	}

	Params& U(uint32 value)
	{
		if (fCount < VULKAN_PUSH_CONSTANT_SIZE / 4)
			fWords[fCount++].u = value;
		return *this;
	}

	Params& I(int32 value)
	{
		if (fCount < VULKAN_PUSH_CONSTANT_SIZE / 4)
			fWords[fCount++].i = value;
		return *this;
	}

	Params& F(float value)
	{
		if (fCount < VULKAN_PUSH_CONSTANT_SIZE / 4)
			fWords[fCount++].f = value;
		return *this;
	}

	uint32 Count() const { return fCount; }
	const PlanWord* Words() const { return fWords; }

private:
	PlanWord	fWords[VULKAN_PUSH_CONSTANT_SIZE / 4];
	uint32		fCount;
};


// #pragma mark - PlanConstant


void
PlanConstant::Fill(const BNeuralGraph& graph, float* target) const
{
	memset(target, 0, size);
	if (tensor < 0)
		return;

	const float* source = (const float*)graph.TensorAt(tensor)->data;

	switch (kind) {
		case PLAN_CONSTANT_TENSOR:
		{
			// outputs: positions, inputs: channels
			uint32 groups = groups_of(inputs);
			if (inputs % 4 == 0) {
				memcpy(target, source, size);
				break;
			}
			for (uint32 i = 0; i < outputs; i++) {
				memcpy(target + (size_t)i * groups * 4,
					source + (size_t)i * inputs, inputs * sizeof(float));
			}
			break;
		}

		case PLAN_CONSTANT_CONV_WEIGHTS:
		{
			// From output, row, column, input to a mat4 per four inputs
			// and four outputs, in the order of the shader's loops:
			// output group, row, column, input group. A column of the
			// matrix is an input channel.
			uint32 inGroups = groups_of(inputs);
			for (uint32 o = 0; o < outputs; o++) {
				size_t group = o / 4;
				for (uint32 ky = 0; ky < kernel_height; ky++) {
					for (uint32 kx = 0; kx < kernel_width; kx++) {
						size_t matrix = ((group * kernel_height + ky)
							* kernel_width + kx) * inGroups;
						float* to = target + matrix * 16 + o % 4;
						for (uint32 i = 0; i < inputs; i++)
							to[i * 4] = *source++;
					}
				}
			}
			break;
		}

		case PLAN_CONSTANT_DEPTHWISE_WEIGHTS:
		{
			// from row, column, output to output group, row, column
			size_t kernel = (size_t)kernel_height * kernel_width;
			for (size_t k = 0; k < kernel; k++) {
				for (uint32 o = 0; o < outputs; o++)
					target[((o / 4) * kernel + k) * 4 + o % 4] = *source++;
			}
			break;
		}

		case PLAN_CONSTANT_BIAS:
			memcpy(target, source, outputs * sizeof(float));
			break;
	}
}


// #pragma mark - VulkanPlan


VulkanPlan::VulkanPlan(const BNeuralGraph& graph, const VulkanLimits& limits)
	:
	fGraph(graph),
	fLimits(limits),
	fStatus(B_OK),
	fOperator(-1),
	fTensorValues(NULL),
	fTensorMemory(0),
	fConstantMemory(0),
	fTransferMemory(0)
{
	int32 count = graph.CountTensors();
	fTensorValues = (int32*)malloc((count > 0 ? count : 1) * sizeof(int32));
	if (fTensorValues == NULL) {
		fStatus = B_NO_MEMORY;
		return;
	}
	for (int32 i = 0; i < count; i++)
		fTensorValues[i] = -1;
}


VulkanPlan::~VulkanPlan()
{
	free(fTensorValues);
}


const neural_graph_tensor*
VulkanPlan::_Tensor(int32 index) const
{
	if (index < 0 || index >= fGraph.CountTensors())
		return NULL;
	return fGraph.TensorAt(index);
}


//! A tensor the device can hold: 32 bit floating point numbers.
const neural_graph_tensor*
VulkanPlan::_FloatTensor(int32 index) const
{
	const neural_graph_tensor* tensor = _Tensor(index);
	if (tensor == NULL || tensor->type != B_NEURAL_FLOAT32
		|| tensor->quantized
		|| (tensor->data != NULL
			&& tensor->size != (size_t)element_count(*tensor) * 4)) {
		return NULL;
	}
	return tensor;
}


//! A tensor of batch, height, width and channels.
const neural_graph_tensor*
VulkanPlan::_Picture(int32 index) const
{
	const neural_graph_tensor* tensor = _FloatTensor(index);
	return tensor != NULL && tensor->rank == 4 ? tensor : NULL;
}


//! Reads a constant tensor of \a count whole numbers.
bool
VulkanPlan::_ReadIntegers(int32 index, int32 count, int64* values) const
{
	const neural_graph_tensor* tensor = _Tensor(index);
	if (tensor == NULL || tensor->data == NULL
		|| element_count(*tensor) != count) {
		return false;
	}

	if (tensor->type == B_NEURAL_INT32
		&& tensor->size == count * sizeof(int32)) {
		for (int32 i = 0; i < count; i++)
			values[i] = ((const int32*)tensor->data)[i];
		return true;
	}
	if (tensor->type == B_NEURAL_INT64
		&& tensor->size == count * sizeof(int64)) {
		for (int32 i = 0; i < count; i++)
			values[i] = ((const int64*)tensor->data)[i];
		return true;
	}
	return false;
}


status_t
VulkanPlan::_NewValue(uint32 positions, uint32 channels, int32* _value)
{
	PlanValue value;
	memset(&value, 0, sizeof(value));
	value.positions = positions;
	value.channels = channels;
	value.groups = groups_of(channels);
	value.constant = -1;
	value.first_step = -1;
	value.last_step = -1;

	if (positions == 0 || channels == 0
		|| (uint64)positions * value.groups > kMaxVectors
		|| value.Bytes() > fLimits.max_range) {
		return kUnsupported;
	}

	value.root = fValues.Count();
	*_value = fValues.Add(value);
	return *_value >= 0 ? B_OK : B_NO_MEMORY;
}


/*!	The value of a tensor an operator reads: made by an earlier step,
	or new: something that comes from outside, or a constant. */
status_t
VulkanPlan::_InputValue(int32 index, int32* _value)
{
	const neural_graph_tensor* tensor = _FloatTensor(index);
	if (tensor == NULL)
		return kUnsupported;

	if (fTensorValues[index] >= 0) {
		*_value = fTensorValues[index];
		return B_OK;
	}

	int64 elements = element_count(*tensor);
	uint32 channels = tensor->rank > 0
		? tensor->dimensions[tensor->rank - 1] : 1;
	if (elements <= 0 || elements / channels > (int64)kMaxVectors)
		return kUnsupported;

	status_t status = _NewValue(elements / channels, channels, _value);
	if (status != B_OK)
		return status;

	if (tensor->data != NULL) {
		PlanConstant constant;
		memset(&constant, 0, sizeof(constant));
		constant.kind = PLAN_CONSTANT_TENSOR;
		constant.tensor = index;
		constant.outputs = fValues[*_value].positions;
		constant.inputs = channels;
		constant.size = fValues[*_value].Bytes();

		int32 constantIndex;
		status = _AddConstant(constant, &constantIndex);
		if (status != B_OK)
			return status;
		fValues[*_value].constant = constantIndex;
	}

	fTensorValues[index] = *_value;
	return B_OK;
}


//! A new value for the tensor an operator makes.
status_t
VulkanPlan::_OutputValue(int32 index, int32* _value)
{
	const neural_graph_tensor* tensor = _FloatTensor(index);
	if (tensor == NULL || tensor->data != NULL)
		return kUnsupported;

	int64 elements = element_count(*tensor);
	uint32 channels = tensor->rank > 0
		? tensor->dimensions[tensor->rank - 1] : 1;
	if (elements <= 0 || elements / channels > (int64)kMaxVectors)
		return kUnsupported;

	status_t status = _NewValue(elements / channels, channels, _value);
	if (status == B_OK)
		fTensorValues[index] = *_value;
	return status;
}


/*!	The numbers of a value as so many positions of so many channels: the
	value itself, a second look at its memory when the two layouts are
	the same (the channels do not change, or neither has padding), or a
	copy made by a step of its own. */
status_t
VulkanPlan::_View(int32 index, uint32 positions, uint32 channels,
	int32* _view)
{
	// by value: the list may move when it grows
	PlanValue value = fValues[index];
	if ((uint64)value.positions * value.channels
			!= (uint64)positions * channels) {
		return kUnsupported;
	}
	if (value.positions == positions) {
		*_view = index;
		return B_OK;
	}

	status_t status = _NewValue(positions, channels, _view);
	if (status != B_OK)
		return status;

	if (value.channels % 4 == 0 && channels % 4 == 0) {
		fValues[*_view].root = value.root;
		return B_OK;
	}

	const PlanValue& view = fValues[*_view];
	return _AddStep(VULKAN_SHADER_REPACK, positions * view.groups,
		Params().U(view.groups).U(channels).U(value.groups)
			.U(value.channels),
		*_view, index);
}


status_t
VulkanPlan::_AddConstant(const PlanConstant& constant, int32* _index)
{
	if (constant.size == 0 || constant.size > fLimits.max_range)
		return kUnsupported;

	*_index = fConstants.Add(constant);
	return *_index >= 0 ? B_OK : B_NO_MEMORY;
}


/*!	The bias of a convolution: the tensor (one number per output), or
	zeros without one. */
status_t
VulkanPlan::_AddBias(int32 tensorIndex, uint32 outputs, int32* _index)
{
	if (tensorIndex >= 0) {
		const neural_graph_tensor* tensor = _FloatTensor(tensorIndex);
		if (tensor == NULL || tensor->data == NULL
			|| element_count(*tensor) != outputs) {
			return kUnsupported;
		}
	}

	PlanConstant constant;
	memset(&constant, 0, sizeof(constant));
	constant.kind = PLAN_CONSTANT_BIAS;
	constant.tensor = tensorIndex;
	constant.outputs = outputs;
	constant.size = groups_of(outputs) * 16;
	return _AddConstant(constant, _index);
}


/*!	A dispatch of \a total invocations numbered in a row: as many
	workgroups along x as there may be, and rows of them along y for the
	rest. */
bool
VulkanPlan::_SetVariant(PlanVariant& variant, uint32 shader,
	uint32 total) const
{
	if (total == 0 || total > kMaxInvocations)
		return false;

	uint32 groups = (total + VULKAN_LOCAL_SIZE - 1) / VULKAN_LOCAL_SIZE;
	variant.shader = shader;
	variant.groups[0] = groups < fLimits.max_groups[0]
		? groups : fLimits.max_groups[0];
	variant.groups[1] = (groups + variant.groups[0] - 1) / variant.groups[0];
	variant.groups[2] = 1;
	variant.total = total;
	variant.row_size = variant.groups[0] * VULKAN_LOCAL_SIZE;
	return variant.groups[1] <= fLimits.max_groups[1];
}


status_t
VulkanPlan::_AddStep(uint32 shader, uint32 total, const Params& params,
	int32 output, int32 input, int32 second, int32 weights, int32 bias)
{
	PlanStep step;
	memset(&step, 0, sizeof(step));
	step.op = fOperator;
	step.variant_count = 1;
	if (!_SetVariant(step.variants[0], shader, total))
		return kUnsupported;

	step.word_count = params.Count();
	memcpy(step.words, params.Words(), sizeof(step.words));

	step.bindings[0].kind = PLAN_BINDING_VALUE;
	step.bindings[0].index = output;
	step.bindings[1].kind = PLAN_BINDING_VALUE;
	step.bindings[1].index = input;
	if (second >= 0) {
		step.bindings[2].kind = PLAN_BINDING_VALUE;
		step.bindings[2].index = second;
	} else if (weights >= 0) {
		step.bindings[2].kind = PLAN_BINDING_CONSTANT;
		step.bindings[2].index = weights;
	}
	if (bias >= 0) {
		step.bindings[3].kind = PLAN_BINDING_CONSTANT;
		step.bindings[3].index = bias;
	}

	return fSteps.Add(step) >= 0 ? B_OK : B_NO_MEMORY;
}


/*!	Another way to do the step added last. One that does not fit the
	device is no loss: there is the first. */
status_t
VulkanPlan::_AddVariant(uint32 shader, uint32 total)
{
	PlanStep& step = fSteps[fSteps.Count() - 1];
	if (step.variant_count < PLAN_MAX_VARIANTS
		&& _SetVariant(step.variants[step.variant_count], shader, total)) {
		step.variant_count++;
	}
	return B_OK;
}


/*!	The same for a shader whose invocations are laid out as the output
	is (GRID in common.glsl): \a width by \a height, \a depth deep. */
status_t
VulkanPlan::_AddGridVariant(uint32 shader, uint32 width, uint32 height,
	uint32 depth)
{
	const uint32 kGridSize = 8;
	PlanStep& step = fSteps[fSteps.Count() - 1];
	PlanVariant variant;
	variant.shader = shader;
	variant.groups[0] = (width + kGridSize - 1) / kGridSize;
	variant.groups[1] = (height + kGridSize - 1) / kGridSize;
	variant.groups[2] = depth;
	variant.total = 0;
	variant.row_size = 0;

	if (step.variant_count < PLAN_MAX_VARIANTS
		&& variant.groups[0] <= fLimits.max_groups[0]
		&& variant.groups[1] <= fLimits.max_groups[1]
		&& variant.groups[2] <= fLimits.max_groups[2]) {
		step.variants[step.variant_count++] = variant;
	}
	return B_OK;
}


status_t
VulkanPlan::AddOperator(int32 index)
{
	if (fStatus != B_OK)
		return fStatus;

	const neural_graph_operator* op = fGraph.OperatorAt(index);
	if (op == NULL)
		return B_BAD_INDEX;
	if (op->output_count != 1 || op->input_count < 1)
		return kUnsupported;

	fOperator = index;

	switch (op->code) {
		case B_NEURAL_OP_CONV_2D:
			return _Convolution(*op, false);
		case B_NEURAL_OP_DEPTHWISE_CONV_2D:
			return _Convolution(*op, true);
		case B_NEURAL_OP_FULLY_CONNECTED:
			return _FullyConnected(*op);
		case B_NEURAL_OP_AVERAGE_POOL_2D:
			return _Pool(*op, true);
		case B_NEURAL_OP_MAX_POOL_2D:
			return _Pool(*op, false);
		case B_NEURAL_OP_ADD:
			return _Binary(*op, kBinaryAdd);
		case B_NEURAL_OP_SUB:
			return _Binary(*op, kBinarySub);
		case B_NEURAL_OP_MUL:
			return _Binary(*op, kBinaryMul);
		case B_NEURAL_OP_RELU:
			return _Unary(*op, kUnaryRelu);
		case B_NEURAL_OP_RELU6:
			return _Unary(*op, kUnaryRelu6);
		case B_NEURAL_OP_LOGISTIC:
			return _Unary(*op, kUnaryLogistic);
		case B_NEURAL_OP_TANH:
			return _Unary(*op, kUnaryTanh);
		case B_NEURAL_OP_HARD_SWISH:
			return _Unary(*op, kUnaryHardSwish);
		case B_NEURAL_OP_SOFTMAX:
			return _Softmax(*op);
		case B_NEURAL_OP_RESHAPE:
		case B_NEURAL_OP_SQUEEZE:
			return _Reshape(*op);
		case B_NEURAL_OP_CONCATENATION:
			return _Concatenation(*op);
		case B_NEURAL_OP_MEAN:
			return _Mean(*op);
		case B_NEURAL_OP_PAD:
			return _Pad(*op);
		case B_NEURAL_OP_RESIZE_BILINEAR:
			return _Resize(*op);
		default:
			return kUnsupported;
	}
}


// #pragma mark - operators


status_t
VulkanPlan::_Convolution(const neural_graph_operator& op, bool depthwise)
{
	if (op.input_count < 2 || op.input_count > 3 || op.params == NULL)
		return kUnsupported;

	const neural_conv_params& params = *(const neural_conv_params*)op.params;
	const neural_graph_tensor* input = _Picture(op.inputs[0]);
	const neural_graph_tensor* weights = _Picture(op.inputs[1]);
	const neural_graph_tensor* output = _Picture(op.outputs[0]);
	int32 biasTensor = op.input_count > 2 ? op.inputs[2] : -1;
	if (input == NULL || weights == NULL || weights->data == NULL
		|| output == NULL) {
		return kUnsupported;
	}

	int32 inChannels = input->dimensions[3];
	int32 outChannels = output->dimensions[3];
	int32 kernelHeight = weights->dimensions[1];
	int32 kernelWidth = weights->dimensions[2];
	uint32 multiplier = 1;
	if (depthwise) {
		// one or more outputs for each input, each from that input only
		if (weights->dimensions[0] != 1
			|| weights->dimensions[3] != outChannels
			|| outChannels % inChannels != 0) {
			return kUnsupported;
		}
		multiplier = outChannels / inChannels;
	} else if (weights->dimensions[0] != outChannels
		|| weights->dimensions[3] != inChannels) {
		// which also leaves out convolutions in groups
		return kUnsupported;
	}

	Window window;
	float minimum, maximum;
	if (!window.SetTo(*input, *output, params.padding, kernelHeight,
			kernelWidth, params.stride_height, params.stride_width,
			params.dilation_height, params.dilation_width)
		|| !activation_range(params.activation, &minimum, &maximum)) {
		return kUnsupported;
	}

	int32 inValue, outValue;
	status_t status = _InputValue(op.inputs[0], &inValue);
	if (status == B_OK)
		status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	uint32 inGroups = groups_of(inChannels);
	uint32 outGroups = groups_of(outChannels);
	uint32 positions = fValues[outValue].positions;
	bool pointwise = kernelHeight == 1 && kernelWidth == 1;

	PlanConstant constant;
	memset(&constant, 0, sizeof(constant));
	constant.tensor = op.inputs[1];
	constant.outputs = outChannels;
	constant.kernel_height = kernelHeight;
	constant.kernel_width = kernelWidth;
	constant.inputs = inChannels;
	uint64 kernel = (uint64)kernelHeight * kernelWidth;
	if (depthwise) {
		constant.kind = PLAN_CONSTANT_DEPTHWISE_WEIGHTS;
		constant.size = outGroups * kernel * 16;
	} else {
		constant.kind = PLAN_CONSTANT_CONV_WEIGHTS;
		constant.size = outGroups * kernel * inGroups * 64;
	}

	int32 weightsIndex, biasIndex;
	status = _AddConstant(constant, &weightsIndex);
	if (status == B_OK)
		status = _AddBias(biasTensor, outChannels, &biasIndex);
	if (status != B_OK)
		return status;

	if ((uint64)outGroups * positions > kMaxInvocations)
		return kUnsupported;
	uint32 total = outGroups * positions;

	Params constants;
	constants.U(positions).I(window.in_width).I(window.in_height)
		.U(inGroups).I(window.out_width).I(window.out_height).U(outGroups)
		.I(kernelWidth).I(kernelHeight).I(params.stride_width)
		.I(params.stride_height).I(params.dilation_width)
		.I(params.dilation_height).I(window.pad_left).I(window.pad_top)
		.F(minimum).F(maximum);

	if (depthwise) {
		constants.U(multiplier);
		if (multiplier != 1) {
			return _AddStep(VULKAN_SHADER_DEPTHWISE_MULTIPLIER, total,
				constants, outValue, inValue, -1, weightsIndex, biasIndex);
		}

		status = _AddStep(VULKAN_SHADER_DEPTHWISE, total, constants,
			outValue, inValue, -1, weightsIndex, biasIndex);
		if (status == B_OK) {
			status = _AddGridVariant(VULKAN_SHADER_DEPTHWISE_GRID,
				window.out_width, window.out_height,
				window.batches * outGroups);
		}
		return status;
	}

	if (!pointwise) {
		return _AddStep(VULKAN_SHADER_CONV, total, constants, outValue,
			inValue, -1, weightsIndex, biasIndex);
	}

	status = _AddStep(VULKAN_SHADER_CONV_POINTWISE, total, constants,
		outValue, inValue, -1, weightsIndex, biasIndex);
	if (status == B_OK && params.stride_width == 1
		&& params.stride_height == 1 && positions >= 4) {
		status = _AddVariant(VULKAN_SHADER_CONV_QUAD,
			outGroups * ((positions + 3) / 4));
	}
	return status;
}


/*!	A fully connected layer is a 1 x 1 convolution of a picture that has
	one pixel for each row of the input. */
status_t
VulkanPlan::_FullyConnected(const neural_graph_operator& op)
{
	if (op.input_count < 2 || op.input_count > 3 || op.params == NULL)
		return kUnsupported;

	const neural_fully_connected_params& params
		= *(const neural_fully_connected_params*)op.params;
	const neural_graph_tensor* input = _FloatTensor(op.inputs[0]);
	const neural_graph_tensor* weights = _FloatTensor(op.inputs[1]);
	const neural_graph_tensor* output = _FloatTensor(op.outputs[0]);
	int32 biasTensor = op.input_count > 2 ? op.inputs[2] : -1;
	float minimum, maximum;
	if (input == NULL || weights == NULL || weights->data == NULL
		|| weights->rank != 2 || output == NULL || output->rank < 1
		|| input->rank < 1
		|| !activation_range(params.activation, &minimum, &maximum)) {
		return kUnsupported;
	}

	int32 units = weights->dimensions[0];
	int32 depth = weights->dimensions[1];
	int64 inElements = element_count(*input);
	int64 outElements = element_count(*output);
	if (inElements % depth != 0 || inElements / depth > INT32_MAX
		|| output->dimensions[output->rank - 1] != units) {
		return kUnsupported;
	}
	int32 rows = inElements / depth;
	if (outElements != (int64)rows * units)
		return kUnsupported;
	if (params.keep_dimensions) {
		if (input->rank != output->rank
			|| input->dimensions[input->rank - 1] != depth) {
			return kUnsupported;
		}
	} else if (output->rank != 2)
		return kUnsupported;

	int32 inValue, outValue;
	status_t status = _InputValue(op.inputs[0], &inValue);
	if (status == B_OK)
		status = _View(inValue, rows, depth, &inValue);
	if (status == B_OK)
		status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	uint32 inGroups = groups_of(depth);
	uint32 outGroups = groups_of(units);

	PlanConstant constant;
	memset(&constant, 0, sizeof(constant));
	constant.kind = PLAN_CONSTANT_CONV_WEIGHTS;
	constant.tensor = op.inputs[1];
	constant.outputs = units;
	constant.kernel_height = 1;
	constant.kernel_width = 1;
	constant.inputs = depth;
	constant.size = (uint64)outGroups * inGroups * 64;

	int32 weightsIndex, biasIndex;
	status = _AddConstant(constant, &weightsIndex);
	if (status == B_OK)
		status = _AddBias(biasTensor, units, &biasIndex);
	if (status != B_OK)
		return status;

	if ((uint64)outGroups * rows > kMaxInvocations)
		return kUnsupported;

	// a picture of rows x 1 x 1: the batch is what counts the rows
	Params constants;
	constants.U(rows).I(1).I(1).U(inGroups).I(1).I(1).U(outGroups).I(1).I(1)
		.I(1).I(1).I(1).I(1).I(0).I(0).F(minimum).F(maximum);

	status = _AddStep(VULKAN_SHADER_CONV_POINTWISE, outGroups * rows,
		constants, outValue, inValue, -1, weightsIndex, biasIndex);
	if (status == B_OK && rows >= 4) {
		status = _AddVariant(VULKAN_SHADER_CONV_QUAD,
			outGroups * ((rows + 3) / 4));
	}
	return status;
}


status_t
VulkanPlan::_Pool(const neural_graph_operator& op, bool average)
{
	if (op.input_count != 1 || op.params == NULL)
		return kUnsupported;

	const neural_pool_params& params = *(const neural_pool_params*)op.params;
	const neural_graph_tensor* input = _Picture(op.inputs[0]);
	const neural_graph_tensor* output = _Picture(op.outputs[0]);
	Window window;
	float minimum, maximum;
	if (input == NULL || output == NULL
		|| input->dimensions[3] != output->dimensions[3]
		|| !window.SetTo(*input, *output, params.padding,
			params.filter_height, params.filter_width, params.stride_height,
			params.stride_width, 1, 1)
		|| !activation_range(params.activation, &minimum, &maximum)) {
		return kUnsupported;
	}

	int32 inValue, outValue;
	status_t status = _InputValue(op.inputs[0], &inValue);
	if (status == B_OK)
		status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	const PlanValue& value = fValues[outValue];
	return _AddStep(average ? VULKAN_SHADER_POOL_AVERAGE
			: VULKAN_SHADER_POOL_MAXIMUM,
		value.positions * value.groups,
		Params().U(value.groups).I(window.in_width).I(window.in_height)
			.I(window.out_width).I(window.out_height)
			.I(params.filter_width).I(params.filter_height)
			.I(params.stride_width).I(params.stride_height)
			.I(window.pad_left).I(window.pad_top).F(minimum).F(maximum),
		outValue, inValue);
}


/*!	How an input of a binary operator is read for an output of the shape
	\a output: 0 for a single number, else the number of vec4 it has,
	which the shader goes through again and again. That works when the
	shape of the input is the end of the output's shape (ones before it
	do not count). */
static bool
broadcast_count(const neural_graph_tensor& input,
	const neural_graph_tensor& output, uint32* _count)
{
	if (element_count(input) == 1) {
		*_count = 0;
		return true;
	}
	if (input.rank > output.rank || output.rank < 1)
		return false;

	int32 shift = output.rank - input.rank;
	bool repeated = false;
	uint64 positions = 1;
	for (int32 i = output.rank - 1; i >= 0; i--) {
		int32 size = i >= shift ? input.dimensions[i - shift] : 1;
		if (!repeated && size != output.dimensions[i])
			repeated = true;
		if (repeated && size != 1)
			return false;
		if (!repeated && i < output.rank - 1)
			positions *= size;
	}

	*_count = positions * groups_of(output.dimensions[output.rank - 1]);
	return true;
}


status_t
VulkanPlan::_Binary(const neural_graph_operator& op, uint32 operation)
{
	if (op.input_count != 2 || op.params == NULL)
		return kUnsupported;

	const neural_binary_params& params
		= *(const neural_binary_params*)op.params;
	const neural_graph_tensor* a = _FloatTensor(op.inputs[0]);
	const neural_graph_tensor* b = _FloatTensor(op.inputs[1]);
	const neural_graph_tensor* output = _FloatTensor(op.outputs[0]);
	float minimum, maximum;
	uint32 countA, countB;
	if (a == NULL || b == NULL || output == NULL
		|| !broadcast_count(*a, *output, &countA)
		|| !broadcast_count(*b, *output, &countB)
		|| !activation_range(params.activation, &minimum, &maximum)) {
		return kUnsupported;
	}

	// The output must be as large as the larger input in each dimension:
	// that is so when one of them is not repeated at all.
	if (!same_shape(*a, *output) && !same_shape(*b, *output))
		return kUnsupported;

	int32 valueA, valueB, outValue;
	status_t status = _InputValue(op.inputs[0], &valueA);
	if (status == B_OK)
		status = _InputValue(op.inputs[1], &valueB);
	if (status == B_OK)
		status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	const PlanValue& value = fValues[outValue];
	return _AddStep(VULKAN_SHADER_BINARY, value.positions * value.groups,
		Params().U(value.groups).U(value.channels).U(operation).U(countA)
			.U(countB).F(minimum).F(maximum),
		outValue, valueA, valueB);
}


status_t
VulkanPlan::_Unary(const neural_graph_operator& op, uint32 operation)
{
	const neural_graph_tensor* input = _FloatTensor(op.inputs[0]);
	const neural_graph_tensor* output = _FloatTensor(op.outputs[0]);
	if (op.input_count != 1 || input == NULL || output == NULL
		|| !same_shape(*input, *output)) {
		return kUnsupported;
	}

	int32 inValue, outValue;
	status_t status = _InputValue(op.inputs[0], &inValue);
	if (status == B_OK)
		status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	const PlanValue& value = fValues[outValue];
	return _AddStep(VULKAN_SHADER_UNARY, value.positions * value.groups,
		Params().U(value.groups).U(value.channels).U(operation),
		outValue, inValue);
}


status_t
VulkanPlan::_Softmax(const neural_graph_operator& op)
{
	const neural_graph_tensor* input = _FloatTensor(op.inputs[0]);
	const neural_graph_tensor* output = _FloatTensor(op.outputs[0]);
	if (op.input_count != 1 || op.params == NULL || input == NULL
		|| output == NULL || input->rank < 1
		|| !same_shape(*input, *output)) {
		return kUnsupported;
	}

	// The shader takes the largest input first, which keeps the
	// exponentials small only for a factor above zero.
	float beta = ((const neural_softmax_params*)op.params)->beta;
	if (!isfinite(beta) || beta <= 0)
		return kUnsupported;

	int32 inValue, outValue;
	status_t status = _InputValue(op.inputs[0], &inValue);
	if (status == B_OK)
		status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	const PlanValue& value = fValues[outValue];
	return _AddStep(VULKAN_SHADER_SOFTMAX, value.positions,
		Params().U(value.groups).U(value.channels).F(beta),
		outValue, inValue);
}


/*!	RESHAPE and SQUEEZE: the same numbers in the same order. Nothing is
	done on the device unless the last dimension changes to or from one
	that is not a multiple of four. */
status_t
VulkanPlan::_Reshape(const neural_graph_operator& op)
{
	const neural_graph_tensor* input = _FloatTensor(op.inputs[0]);
	const neural_graph_tensor* output = _FloatTensor(op.outputs[0]);
	if (input == NULL || output == NULL || output->data != NULL
		|| element_count(*input) != element_count(*output)) {
		return kUnsupported;
	}

	if (op.code == B_NEURAL_OP_SQUEEZE) {
		// only dimensions of one may go
		int32 j = 0;
		for (int32 i = 0; i < input->rank; i++) {
			if (j < output->rank
				&& input->dimensions[i] == output->dimensions[j]) {
				j++;
			} else if (input->dimensions[i] != 1)
				return kUnsupported;
		}
		if (j != output->rank)
			return kUnsupported;
	}

	uint32 channels = output->rank > 0
		? output->dimensions[output->rank - 1] : 1;
	int64 positions = element_count(*output) / channels;
	if (positions > (int64)kMaxVectors)
		return kUnsupported;

	int32 inValue, outValue;
	status_t status = _InputValue(op.inputs[0], &inValue);
	if (status == B_OK)
		status = _View(inValue, positions, channels, &outValue);
	if (status != B_OK)
		return status;

	fTensorValues[op.outputs[0]] = outValue;
	return B_OK;
}


status_t
VulkanPlan::_Concatenation(const neural_graph_operator& op)
{
	const neural_graph_tensor* output = _FloatTensor(op.outputs[0]);
	if (op.params == NULL || output == NULL || output->rank < 1)
		return kUnsupported;

	// along the channels only, where the inputs interleave
	const neural_concat_params& params
		= *(const neural_concat_params*)op.params;
	int32 rank = output->rank;
	int32 axis = params.axis < 0 ? params.axis + rank : params.axis;
	if (axis != rank - 1 || params.activation != B_NEURAL_ACTIVATION_NONE)
		return kUnsupported;

	int64 channels = 0;
	for (int32 i = 0; i < op.input_count; i++) {
		const neural_graph_tensor* input = _FloatTensor(op.inputs[i]);
		if (input == NULL || input->rank != rank)
			return kUnsupported;
		for (int32 j = 0; j < rank - 1; j++) {
			if (input->dimensions[j] != output->dimensions[j])
				return kUnsupported;
		}
		channels += input->dimensions[rank - 1];
	}
	if (channels != output->dimensions[rank - 1])
		return kUnsupported;

	int32 outValue;
	status_t status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	uint32 offset = 0;
	for (int32 i = 0; i < op.input_count; i++) {
		int32 inValue;
		status = _InputValue(op.inputs[i], &inValue);
		if (status != B_OK)
			return status;

		PlanValue input = fValues[inValue];
		PlanValue value = fValues[outValue];
		uint32 firstGroup = offset / 4;
		uint32 spanGroups = (offset + input.channels + 3) / 4 - firstGroup;
		if ((uint64)value.positions * spanGroups > kMaxInvocations)
			return kUnsupported;

		status = _AddStep(VULKAN_SHADER_CONCAT, value.positions * spanGroups,
			Params().U(value.groups).U(input.groups).U(firstGroup)
				.U(spanGroups).U(offset).U(input.channels),
			outValue, inValue);
		if (status != B_OK)
			return status;

		offset += input.channels;
	}
	return B_OK;
}


//! MEAN over height and width of pictures, which leaves the channels.
status_t
VulkanPlan::_Mean(const neural_graph_operator& op)
{
	if (op.input_count != 2 || op.params == NULL)
		return kUnsupported;

	const neural_reduce_params& params
		= *(const neural_reduce_params*)op.params;
	const neural_graph_tensor* input = _Picture(op.inputs[0]);
	const neural_graph_tensor* axes = _Tensor(op.inputs[1]);
	const neural_graph_tensor* output = _FloatTensor(op.outputs[0]);
	if (input == NULL || axes == NULL || output == NULL || axes->rank > 1)
		return kUnsupported;

	int64 axisCount = element_count(*axes);
	int64 axisValues[8];
	if (axisCount < 2 || axisCount > 8
		|| !_ReadIntegers(op.inputs[1], axisCount, axisValues)) {
		return kUnsupported;
	}
	bool reduced[4] = {};
	for (int32 i = 0; i < axisCount; i++) {
		int64 axis = axisValues[i] < 0 ? axisValues[i] + 4 : axisValues[i];
		if (axis < 0 || axis > 3)
			return kUnsupported;
		reduced[axis] = true;
	}
	if (reduced[0] || !reduced[1] || !reduced[2] || reduced[3])
		return kUnsupported;

	int32 batches = input->dimensions[0];
	int32 channels = input->dimensions[3];
	if (params.keep_dimensions) {
		if (output->rank != 4 || output->dimensions[0] != batches
			|| output->dimensions[1] != 1 || output->dimensions[2] != 1
			|| output->dimensions[3] != channels) {
			return kUnsupported;
		}
	} else if (output->rank != 2 || output->dimensions[0] != batches
		|| output->dimensions[1] != channels) {
		return kUnsupported;
	}

	int32 inValue, outValue;
	status_t status = _InputValue(op.inputs[0], &inValue);
	if (status == B_OK)
		status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	const PlanValue& value = fValues[outValue];
	uint32 area = input->dimensions[1] * input->dimensions[2];
	return _AddStep(VULKAN_SHADER_MEAN, value.positions * value.groups,
		Params().U(value.groups).U(area), outValue, inValue);
}


//! PAD of pictures at the top, bottom, left and right.
status_t
VulkanPlan::_Pad(const neural_graph_operator& op)
{
	if (op.input_count < 2 || op.input_count > 3)
		return kUnsupported;

	const neural_graph_tensor* input = _Picture(op.inputs[0]);
	const neural_graph_tensor* output = _Picture(op.outputs[0]);
	int64 paddings[8];
	if (input == NULL || output == NULL
		|| !_ReadIntegers(op.inputs[1], 8, paddings)) {
		return kUnsupported;
	}

	if (paddings[0] != 0 || paddings[1] != 0 || paddings[6] != 0
		|| paddings[7] != 0) {
		return kUnsupported;
	}
	for (int32 i = 0; i < 4; i++) {
		if (paddings[2 * i] < 0 || paddings[2 * i + 1] < 0
			|| paddings[2 * i] >= (1 << 15)
			|| output->dimensions[i] != input->dimensions[i]
				+ paddings[2 * i] + paddings[2 * i + 1]) {
			return kUnsupported;
		}
	}
	if (output->dimensions[1] >= (1 << 15)
		|| output->dimensions[2] >= (1 << 15)) {
		return kUnsupported;
	}

	float padValue = 0;
	if (op.input_count > 2 && op.inputs[2] >= 0) {
		const neural_graph_tensor* constant = _FloatTensor(op.inputs[2]);
		if (constant == NULL || constant->data == NULL
			|| element_count(*constant) != 1) {
			return kUnsupported;
		}
		padValue = *(const float*)constant->data;
	}

	int32 inValue, outValue;
	status_t status = _InputValue(op.inputs[0], &inValue);
	if (status == B_OK)
		status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	const PlanValue& value = fValues[outValue];
	return _AddStep(VULKAN_SHADER_PAD, value.positions * value.groups,
		Params().U(value.groups).U(value.channels)
			.I(input->dimensions[2]).I(input->dimensions[1])
			.I(output->dimensions[2]).I(output->dimensions[1])
			.I(paddings[4]).I(paddings[2]).F(padValue),
		outValue, inValue);
}


status_t
VulkanPlan::_Resize(const neural_graph_operator& op)
{
	if (op.input_count != 2 || op.params == NULL)
		return kUnsupported;

	const neural_resize_params& params
		= *(const neural_resize_params*)op.params;
	const neural_graph_tensor* input = _Picture(op.inputs[0]);
	const neural_graph_tensor* size = _Tensor(op.inputs[1]);
	const neural_graph_tensor* output = _Picture(op.outputs[0]);
	if (input == NULL || output == NULL || size == NULL
		|| input->dimensions[0] != output->dimensions[0]
		|| input->dimensions[3] != output->dimensions[3]
		|| (params.align_corners && params.half_pixel_centers)) {
		return kUnsupported;
	}

	int32 inHeight = input->dimensions[1];
	int32 inWidth = input->dimensions[2];
	int32 outHeight = output->dimensions[1];
	int32 outWidth = output->dimensions[2];
	if (inHeight >= (1 << 15) || inWidth >= (1 << 15)
		|| outHeight >= (1 << 15) || outWidth >= (1 << 15)) {
		return kUnsupported;
	}

	// the size asked for is the size of the output
	int64 asked[2];
	if (!_ReadIntegers(op.inputs[1], 2, asked) || asked[0] != outHeight
		|| asked[1] != outWidth) {
		return kUnsupported;
	}

	float scaleY = (float)inHeight / outHeight;
	if (params.align_corners && outHeight > 1)
		scaleY = (float)(inHeight - 1) / (outHeight - 1);
	float scaleX = (float)inWidth / outWidth;
	if (params.align_corners && outWidth > 1)
		scaleX = (float)(inWidth - 1) / (outWidth - 1);

	int32 inValue, outValue;
	status_t status = _InputValue(op.inputs[0], &inValue);
	if (status == B_OK)
		status = _OutputValue(op.outputs[0], &outValue);
	if (status != B_OK)
		return status;

	const PlanValue& value = fValues[outValue];
	return _AddStep(VULKAN_SHADER_RESIZE, value.positions * value.groups,
		Params().U(value.groups).I(inWidth).I(inHeight).I(outWidth)
			.I(outHeight).F(scaleX).F(scaleY)
			.F(params.half_pixel_centers ? 0.5f : 0.0f),
		outValue, inValue);
}


// #pragma mark - memory


/*!	Says what goes in and out, and gives everything its place in memory.
	Tensors between the steps share memory when one is no longer needed
	by the time the other is made: a network of a hundred layers takes
	the memory of its few largest. */
status_t
VulkanPlan::Finish(const neural_partition& partition)
{
	if (fStatus != B_OK)
		return fStatus;
	// only views: nothing a device is faster at
	if (fSteps.Count() == 0)
		return kUnsupported;

	uint64 alignment = fLimits.alignment;

	// the constants, one after the other
	for (int32 i = 0; i < fConstants.Count(); i++) {
		fConstants[i].offset = fConstantMemory;
		fConstantMemory += align_up(fConstants[i].size, alignment);
	}

	// what is copied in and out
	for (int32 i = 0; i < partition.input_count + partition.output_count;
			i++) {
		bool isInput = i < partition.input_count;
		PlanTransfer transfer;
		transfer.tensor = isInput ? partition.inputs[i]
			: partition.outputs[i - partition.input_count];
		if (transfer.tensor < 0 || transfer.tensor >= fGraph.CountTensors())
			return B_BAD_VALUE;

		transfer.value = fTensorValues[transfer.tensor];
		if (transfer.value < 0) {
			// an input no step reads, as the shape of a RESHAPE
			if (isInput)
				continue;
			return B_BAD_VALUE;
		}

		PlanValue& root = fValues[fValues[transfer.value].root];
		if (root.constant >= 0)
			return B_BAD_VALUE;
		if (isInput)
			root.is_input = true;
		else
			root.is_output = true;

		transfer.offset = fTransferMemory;
		fTransferMemory += align_up(fValues[transfer.value].Bytes(),
			alignment);
		if ((isInput ? fInputs : fOutputs).Add(transfer) < 0)
			return B_NO_MEMORY;
	}

	// when each piece of memory is needed
	for (int32 i = 0; i < fSteps.Count(); i++) {
		const PlanStep& step = fSteps[i];
		for (int32 j = 0; j < VULKAN_BINDING_COUNT; j++) {
			if (step.bindings[j].kind != PLAN_BINDING_VALUE)
				continue;
			PlanValue& root = fValues[fValues[step.bindings[j].index].root];
			if (j == 0 && root.first_step < 0 && !root.is_input)
				root.first_step = i;
			root.last_step = i;
		}
	}

	struct Slot {
		uint64	size;
		bool	free;
	};
	PlanList<Slot> slots;
	int32* slotOf = (int32*)malloc(fValues.Count() * sizeof(int32));
	if (slotOf == NULL)
		return B_NO_MEMORY;
	for (int32 i = 0; i < fValues.Count(); i++)
		slotOf[i] = -1;

	status_t status = B_OK;

	// step -1 is the copying in; a slot is taken for a value when it is
	// first written and given back after the step that uses it last
	for (int32 i = -1; i < fSteps.Count() && status == B_OK; i++) {
		for (int32 j = 0; j < fValues.Count(); j++) {
			PlanValue& value = fValues[j];
			if (value.root != j || value.constant >= 0 || slotOf[j] >= 0)
				continue;
			if (i < 0 ? !value.is_input : value.first_step != i)
				continue;

			// the smallest free slot that is large enough, or else the
			// largest free one made larger
			uint64 size = align_up(value.Bytes(), alignment);
			int32 best = -1;
			for (int32 k = 0; k < slots.Count(); k++) {
				if (!slots[k].free)
					continue;
				if (best < 0) {
					best = k;
					continue;
				}
				bool fits = slots[k].size >= size;
				bool bestFits = slots[best].size >= size;
				if (fits != bestFits ? fits
						: (fits ? slots[k].size < slots[best].size
							: slots[k].size > slots[best].size)) {
					best = k;
				}
			}

			if (best < 0) {
				Slot slot = { size, false };
				best = slots.Add(slot);
				if (best < 0) {
					status = B_NO_MEMORY;
					break;
				}
			} else {
				if (slots[best].size < size)
					slots[best].size = size;
				slots[best].free = false;
			}
			slotOf[j] = best;
		}

		for (int32 j = 0; j < fValues.Count(); j++) {
			const PlanValue& value = fValues[j];
			if (slotOf[j] >= 0 && value.root == j && !value.is_output
				&& value.last_step == i) {
				slots[slotOf[j]].free = true;
			}
		}
	}

	if (status == B_OK) {
		// A slot has grown to its final size by now.
		uint64 offset = 0;
		for (int32 i = 0; i < slots.Count(); i++) {
			uint64 size = slots[i].size;
			slots[i].size = offset;
			offset += size;
		}
		fTensorMemory = offset;

		for (int32 i = 0; i < fValues.Count(); i++) {
			PlanValue& value = fValues[i];
			if (value.root != i || value.constant >= 0)
				continue;
			if (slotOf[i] < 0) {
				// a value no step writes and nothing brings in: a hole
				// in the partition
				status = B_BAD_VALUE;
				break;
			}
			value.offset = slots[slotOf[i]].size;
		}
	}

	free(slotOf);
	return status;
}
