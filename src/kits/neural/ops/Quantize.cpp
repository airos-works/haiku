/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	QUANTIZE and DEQUANTIZE: between real numbers and stored values, and
	from one quantization to another.
*/


// Rounding real numbers to stored values must give the same on every
// processor: no merging into multiply-add instructions.
#pragma GCC optimize("O3", "fp-contract=off")


#include "Kernels.h"


namespace BPrivate {


namespace {


struct QuantizeData {
	int32	elements;
	float	scale;
	int32	zero_point;
	int32	minimum;
	int32	maximum;
	//! from 8 bits to 8 bits: output value by input value
	uint8	table[256];
};


//! Elements of the operator: input and output have the same shape.
static int32
common_elements(const NeuralGraphData* graph, const neural_graph_operator* op)
{
	if (op->input_count != 1 || op->inputs[0] < 0 || op->output_count != 1)
		return -1;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);
	if (!neural_same_shape(input, output))
		return -1;
	return neural_element_count(input);
}


static bool
is_quantized(const neural_graph_tensor& tensor)
{
	// one scale for the whole tensor
	return (neural_is_quantized_8(tensor) || (tensor.type == B_NEURAL_INT16
			&& tensor.quantized && tensor.scale > 0
			&& isfinite(tensor.scale)))
		&& tensor.channel_count == 0 && neural_valid_zero_point(tensor);
}


// #pragma mark - QUANTIZE


template<typename Type>
static void
quantize(const QuantizeData& data, const float* input, Type* output)
{
	for (int32 i = 0; i < data.elements; i++) {
		// in single precision, as the reference does; looked at before
		// it is made an integer, since it may be anything
		float scaled = roundf(input[i] / data.scale);
		int32 value;
		if (!(scaled > -1e9f))
			value = data.minimum;
		else if (scaled > 1e9f)
			value = data.maximum;
		else {
			value = neural_clamp((int32)scaled + data.zero_point,
				data.minimum, data.maximum);
		}
		output[i] = (Type)value;
	}
}


static status_t
prepare_quantize(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	QuantizeData header;
	memset(&header, 0, sizeof(header));
	header.elements = common_elements(graph, op);
	if (header.elements < 0)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);
	if (!is_quantized(output))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	header.scale = output.scale;
	header.zero_point = output.zero_point;
	neural_type_range(output.type, &header.minimum, &header.maximum);

	if (input.type == B_NEURAL_FLOAT32) {
		// any quantized output
	} else if (neural_is_quantized_8(input) && is_quantized(input)
		&& output.type != B_NEURAL_INT16) {
		// from one quantization to another: a table
		int32 multiplier;
		int shift;
		if (!neural_checked_quantize_multiplier((double)input.scale
				/ (double)output.scale, &multiplier, &shift)) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}

		int32 min, max;
		neural_type_range(input.type, &min, &max);
		for (int32 stored = min; stored <= max; stored++) {
			int32 value = neural_multiply_by_quantized_multiplier(
				stored - input.zero_point, multiplier, shift);
			header.table[(uint8)stored] = (uint8)neural_clamp(
				(int32)((uint32)value + (uint32)output.zero_point),
				header.minimum, header.maximum);
		}
	} else
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	QuantizeData* data = (QuantizeData*)malloc(sizeof(QuantizeData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


static status_t
run_quantize(const NeuralOpContext& context)
{
	const QuantizeData& data = *(const QuantizeData*)context.prepared;

	if (context.InputInfo(0).type != B_NEURAL_FLOAT32) {
		const uint8* input = context.Input<uint8>(0);
		uint8* output = context.Output<uint8>(0);
		for (int32 i = 0; i < data.elements; i++)
			output[i] = data.table[input[i]];
		return B_OK;
	}

	const float* input = context.Input<float>(0);
	switch (context.OutputInfo(0).type) {
		case B_NEURAL_UINT8:
			quantize(data, input, context.Output<uint8>(0));
			break;
		case B_NEURAL_INT8:
			quantize(data, input, context.Output<int8>(0));
			break;
		default:
			quantize(data, input, context.Output<int16>(0));
			break;
	}
	return B_OK;
}


// #pragma mark - DEQUANTIZE


template<typename Type>
static void
dequantize(const QuantizeData& data, const Type* input, float* output)
{
	// the product in double precision, as the reference has it
	double scale = data.scale;
	for (int32 i = 0; i < data.elements; i++)
		output[i] = (float)(scale * (input[i] - data.zero_point));
}


//! A half precision number (IEEE 754 binary16) as single precision.
static inline float
half_to_float(uint16 half)
{
	uint32 sign = (uint32)(half & 0x8000) << 16;
	uint32 exponent = (half >> 10) & 0x1f;
	uint32 mantissa = half & 0x3ff;

	uint32 bits;
	if (exponent == 0x1f) {
		// infinity, not a number
		bits = 0x7f800000 | (mantissa << 13);
	} else if (exponent != 0) {
		bits = ((exponent + 112) << 23) | (mantissa << 13);
	} else if (mantissa != 0) {
		// a subnormal half is a normal float: shift the leading bit out
		int shift = __builtin_clz(mantissa) - 21;
		bits = ((uint32)(113 - shift) << 23)
			| ((mantissa << (shift + 13)) & 0x7fffff);
	} else
		bits = 0;

	bits |= sign;
	float value;
	memcpy(&value, &bits, sizeof(value));
	return value;
}


static status_t
prepare_dequantize(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	QuantizeData header;
	memset(&header, 0, sizeof(header));
	header.elements = common_elements(graph, op);
	if (header.elements < 0)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);
	if (output.type != B_NEURAL_FLOAT32
		|| (input.type != B_NEURAL_FLOAT16 && !is_quantized(input))) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	header.scale = input.scale;
	header.zero_point = input.zero_point;

	QuantizeData* data = (QuantizeData*)malloc(sizeof(QuantizeData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


static status_t
run_dequantize(const NeuralOpContext& context)
{
	const QuantizeData& data = *(const QuantizeData*)context.prepared;
	float* output = context.Output<float>(0);

	switch (context.InputInfo(0).type) {
		case B_NEURAL_UINT8:
			dequantize(data, context.Input<uint8>(0), output);
			break;
		case B_NEURAL_INT8:
			dequantize(data, context.Input<int8>(0), output);
			break;
		case B_NEURAL_INT16:
			dequantize(data, context.Input<int16>(0), output);
			break;
		default:
		{
			const uint16* input = context.Input<uint16>(0);
			for (int32 i = 0; i < data.elements; i++)
				output[i] = half_to_float(input[i]);
			break;
		}
	}
	return B_OK;
}


}	// unnamed namespace


const NeuralKernel kQuantizeKernel = {
	B_NEURAL_OP_QUANTIZE, &prepare_quantize, &run_quantize
};

const NeuralKernel kDequantizeKernel = {
	B_NEURAL_OP_DEQUANTIZE, &prepare_dequantize, &run_dequantize
};


}	// namespace BPrivate
