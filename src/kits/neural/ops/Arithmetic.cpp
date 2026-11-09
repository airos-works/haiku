/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Operators that combine two tensors element by element: sums,
	differences, products, quotients, the larger and the smaller. The
	tensors may have different shapes that go together NumPy style.

	Quantized sums and differences bring both inputs to a common scale
	with 20 more bits before adding, products multiply the stored values
	and scale once; both exactly as the TensorFlow Lite reference kernels.
*/


#pragma GCC optimize("O3")


#include "Kernels.h"


namespace BPrivate {


namespace {


struct BinaryData {
	neural_broadcast	broadcast;

	// quantized
	int32				offset1;
	int32				offset2;
	int32				output_offset;
	int32				multiplier1;
	int					shift1;
	int32				multiplier2;
	int					shift2;
	int32				output_multiplier;
	int					output_shift;
	int32				activation_min;
	int32				activation_max;

	float				float_min;
	float				float_max;
};


// What is done to a pair of elements. Each is called with the values as
// stored and has copies of the numbers it needs: as an object of its own
// the compiler can keep them in registers over a loop.

struct QuantizedAdd {
	neural_scaling	scaling1;
	neural_scaling	scaling2;
	neural_scaling	output_scaling;
	int32			offset1;
	int32			offset2;
	int32			output_offset;
	int32			min;
	int32			max;
	//! 1 to add, -1 to subtract
	int32			sign;

	QuantizedAdd(const BinaryData& data, int32 sign)
		:
		offset1(data.offset1),
		offset2(data.offset2),
		output_offset(data.output_offset),
		min(data.activation_min),
		max(data.activation_max),
		sign(sign)
	{
		// 20 more bits for the inputs on their way to the common scale
		neural_set_scaling(scaling1, data.multiplier1, data.shift1);
		neural_set_scaling(scaling2, data.multiplier2, data.shift2);
		neural_set_scaling(output_scaling, data.output_multiplier,
			data.output_shift);
		scaling1.left = 20;
		scaling2.left = 20;
	}

	inline int32 operator()(int32 a, int32 b) const
	{
		int32 scaled1 = neural_scale(a + offset1, scaling1);
		int32 scaled2 = neural_scale(b + offset2, scaling2);
		return neural_clamp(neural_scale(scaled1 + sign * scaled2,
			output_scaling) + output_offset, min, max);
	}
};


struct QuantizedMul {
	neural_scaling	scaling;
	int32			offset1;
	int32			offset2;
	int32			output_offset;
	int32			min;
	int32			max;

	QuantizedMul(const BinaryData& data)
		:
		offset1(data.offset1),
		offset2(data.offset2),
		output_offset(data.output_offset),
		min(data.activation_min),
		max(data.activation_max)
	{
		neural_set_scaling(scaling, data.output_multiplier,
			data.output_shift);
	}

	inline int32 operator()(int32 a, int32 b) const
	{
		return neural_clamp(neural_scale((a + offset1) * (b + offset2),
			scaling) + output_offset, min, max);
	}
};


struct FloatAdd {
	float	min;
	float	max;

	inline float operator()(float a, float b) const
	{
		return neural_clamp(a + b, min, max);
	}
};


struct FloatSub {
	float	min;
	float	max;

	inline float operator()(float a, float b) const
	{
		return neural_clamp(a - b, min, max);
	}
};


struct FloatMul {
	float	min;
	float	max;

	inline float operator()(float a, float b) const
	{
		return neural_clamp(a * b, min, max);
	}
};


struct FloatDiv {
	float	min;
	float	max;

	inline float operator()(float a, float b) const
	{
		return neural_clamp(a / b, min, max);
	}
};


struct FloatSquaredDifference {
	inline float operator()(float a, float b) const
	{
		float difference = a - b;
		return difference * difference;
	}
};


template<typename Type>
struct Larger {
	inline Type operator()(Type a, Type b) const
	{
		return a > b ? a : b;
	}
};


template<typename Type>
struct Smaller {
	inline Type operator()(Type a, Type b) const
	{
		return a < b ? a : b;
	}
};


template<typename Type, typename Operation>
struct BinaryRun {
	const neural_broadcast*	broadcast;
	const Type*				input1;
	const Type*				input2;
	Type*					output;
	Operation				operation;
};


/*!	A row: \a step1 and \a step2 are 1, or 0 for an input that is
	repeated. Inlined with the steps known, each case becomes a loop of its
	own. */
template<typename Type, typename Operation>
static inline __attribute__((always_inline)) void
binary_row(const Operation& operation, const Type* __restrict input1,
	const Type* __restrict input2, Type* __restrict output, int32 count,
	int32 step1, int32 step2)
{
	// (a copy: nothing the loop stores can change it)
	const Operation local = operation;
	for (int32 i = 0; i < count; i++)
		output[i] = (Type)local(input1[i * step1], input2[i * step2]);
}


template<typename Type, typename Operation>
static inline void
binary_rows(const BinaryRun<Type, Operation>& run, const Type* input1,
	const Type* input2, Type* output, int32 count)
{
	const neural_broadcast& broadcast = *run.broadcast;
	int32 last = broadcast.rank - 1;
	if (broadcast.step1[last] != 0 && broadcast.step2[last] != 0)
		binary_row(run.operation, input1, input2, output, count, 1, 1);
	else if (broadcast.step1[last] != 0)
		binary_row(run.operation, input1, input2, output, count, 1, 0);
	else
		binary_row(run.operation, input1, input2, output, count, 0, 1);
}


/*!	With one (merged) dimension the pool splits that one: \a first and
	\a end are elements; with more they are rows of the last dimension. */
template<typename Type, typename Operation>
static void
binary_slice(void* cookie, int32 first, int32 end)
{
	NeuralFlushDenormals flush;
	const BinaryRun<Type, Operation>& run
		= *(const BinaryRun<Type, Operation>*)cookie;
	const neural_broadcast& broadcast = *run.broadcast;
	int32 last = broadcast.rank - 1;

	if (broadcast.rank == 1) {
		binary_rows(run, run.input1 + first * broadcast.step1[0],
			run.input2 + first * broadcast.step2[0], run.output + first,
			end - first);
		return;
	}

	int32 count = broadcast.size[last];
	for (int32 row = first; row < end; row++) {
		int32 offset1, offset2;
		neural_broadcast_offsets(broadcast, row, &offset1, &offset2);
		binary_rows(run, run.input1 + offset1, run.input2 + offset2,
			run.output + (size_t)row * count, count);
	}
}


/*!	\a grain: the elements worth a thread of their own, a few dozen
	microseconds of work. */
template<typename Type, typename Operation>
static void
run_binary(const NeuralOpContext& context, const Operation& operation,
	int32 grain)
{
	const BinaryData* data = (const BinaryData*)context.prepared;
	const neural_broadcast& broadcast = data->broadcast;

	BinaryRun<Type, Operation> run = { &broadcast, context.Input<Type>(0),
		context.Input<Type>(1), context.Output<Type>(0), operation };

	if (broadcast.rank == 1) {
		context.pool->Run(broadcast.size[0], grain,
			&binary_slice<Type, Operation>, &run);
	} else {
		int32 rowGrain = grain / broadcast.size[broadcast.rank - 1];
		context.pool->Run(broadcast.outer_count, rowGrain < 1 ? 1 : rowGrain,
			&binary_slice<Type, Operation>, &run);
	}
}


static bool
common_checks(const NeuralGraphData* graph, const neural_graph_operator* op,
	BinaryData* data)
{
	if (op->input_count != 2 || !neural_has_inputs(op, 2)
		|| op->output_count != 1) {
		return false;
	}

	const neural_graph_tensor& input1 = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& input2 = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	memset(data, 0, sizeof(BinaryData));
	if (input1.type != input2.type || input1.type != output.type
		|| !neural_prepare_broadcast(input1, input2, output,
			&data->broadcast)) {
		return false;
	}

	if (input1.type == B_NEURAL_FLOAT32)
		return true;

	return neural_is_quantized_8(input1) && neural_is_quantized_8(input2)
		&& neural_is_quantized_8(output) && neural_valid_zero_point(input1)
		&& neural_valid_zero_point(input2)
		&& neural_valid_zero_point(output);
}


static status_t
finish_prepare(const BinaryData& header, void** _prepared)
{
	BinaryData* data = (BinaryData*)malloc(sizeof(BinaryData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


static neural_activation
activation_of(const neural_graph_operator* op)
{
	if (op->params == NULL)
		return B_NEURAL_ACTIVATION_NONE;
	return ((const neural_binary_params*)op->params)->activation;
}


// #pragma mark - ADD, SUB


static status_t
prepare_add(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	BinaryData data;
	if (!common_checks(graph, op, &data))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	const neural_graph_tensor& input1 = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& input2 = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	if (input1.type == B_NEURAL_FLOAT32) {
		if (neural_float_activation_range(activation_of(op), &data.float_min,
				&data.float_max) != B_OK) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
		return finish_prepare(data, _prepared);
	}

	data.offset1 = -input1.zero_point;
	data.offset2 = -input2.zero_point;
	data.output_offset = output.zero_point;

	// The factors as the reference computes them, single precision
	// where it uses single precision.
	float twiceLargest = 2 * (input1.scale > input2.scale
		? input1.scale : input2.scale);
	double factor1 = input1.scale / (double)twiceLargest;
	double factor2 = input2.scale / (double)twiceLargest;
	double outputFactor = (double)twiceLargest
		/ (double)((1 << 20) * output.scale);

	// All three are to scale down; the fixed-point multiplication the
	// reference uses here cannot do anything else.
	if (!neural_checked_quantize_multiplier(factor1, &data.multiplier1,
			&data.shift1)
		|| !neural_checked_quantize_multiplier(factor2, &data.multiplier2,
			&data.shift2)
		|| !neural_checked_quantize_multiplier(outputFactor,
			&data.output_multiplier, &data.output_shift)
		|| data.shift1 > 0 || data.shift2 > 0 || data.output_shift > 0
		|| neural_quantized_activation_range(activation_of(op), output,
			&data.activation_min, &data.activation_max) != B_OK) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	return finish_prepare(data, _prepared);
}


template<int32 kSign>
static status_t
run_add(const NeuralOpContext& context)
{
	const BinaryData& data = *(const BinaryData*)context.prepared;
	QuantizedAdd quantized(data, kSign);

	switch (context.InputInfo(0).type) {
		case B_NEURAL_FLOAT32:
			if (kSign > 0) {
				FloatAdd operation = { data.float_min, data.float_max };
				run_binary<float, FloatAdd>(context, operation, 65536);
			} else {
				FloatSub operation = { data.float_min, data.float_max };
				run_binary<float, FloatSub>(context, operation, 65536);
			}
			break;
		case B_NEURAL_UINT8:
			run_binary<uint8, QuantizedAdd>(context, quantized, 32768);
			break;
		default:
			run_binary<int8, QuantizedAdd>(context, quantized, 32768);
			break;
	}
	return B_OK;
}


// #pragma mark - MUL, DIV


static status_t
prepare_mul(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	BinaryData data;
	if (!common_checks(graph, op, &data))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	const neural_graph_tensor& input1 = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& input2 = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	if (input1.type == B_NEURAL_FLOAT32) {
		if (neural_float_activation_range(activation_of(op), &data.float_min,
				&data.float_max) != B_OK) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}
		return finish_prepare(data, _prepared);
	}

	// quantized quotients are not implemented
	if (op->code != B_NEURAL_OP_MUL)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	data.offset1 = -input1.zero_point;
	data.offset2 = -input2.zero_point;
	data.output_offset = output.zero_point;

	// in single precision, as the reference does
	float factor = input1.scale * input2.scale / output.scale;
	if (!neural_checked_quantize_multiplier(factor, &data.output_multiplier,
			&data.output_shift)
		|| neural_quantized_activation_range(activation_of(op), output,
			&data.activation_min, &data.activation_max) != B_OK) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	return finish_prepare(data, _prepared);
}


static status_t
run_mul(const NeuralOpContext& context)
{
	const BinaryData& data = *(const BinaryData*)context.prepared;
	QuantizedMul quantized(data);
	FloatMul operation = { data.float_min, data.float_max };

	switch (context.InputInfo(0).type) {
		case B_NEURAL_FLOAT32:
			run_binary<float, FloatMul>(context, operation, 65536);
			break;
		case B_NEURAL_UINT8:
			run_binary<uint8, QuantizedMul>(context, quantized, 32768);
			break;
		default:
			run_binary<int8, QuantizedMul>(context, quantized, 32768);
			break;
	}
	return B_OK;
}


static status_t
run_div(const NeuralOpContext& context)
{
	const BinaryData& data = *(const BinaryData*)context.prepared;
	FloatDiv operation = { data.float_min, data.float_max };
	run_binary<float, FloatDiv>(context, operation, 32768);
	return B_OK;
}


// #pragma mark - SQUARED_DIFFERENCE


static status_t
prepare_squared_difference(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	BinaryData data;
	if (!common_checks(graph, op, &data))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	// what layer normalisation is made of: floating point only
	if (graph->Tensor(op->inputs[0]).type != B_NEURAL_FLOAT32)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	return finish_prepare(data, _prepared);
}


static status_t
run_squared_difference(const NeuralOpContext& context)
{
	run_binary<float, FloatSquaredDifference>(context, {}, 65536);
	return B_OK;
}


// #pragma mark - MAXIMUM, MINIMUM


static status_t
prepare_compare(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	BinaryData data;
	if (!common_checks(graph, op, &data))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	const neural_graph_tensor& input1 = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& input2 = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	// The stored values are compared: they must mean the same in all
	// three tensors. There is no activation to these operators.
	if (activation_of(op) != B_NEURAL_ACTIVATION_NONE
		|| (input1.type != B_NEURAL_FLOAT32
			&& (!neural_same_quantization(input1, input2)
				|| !neural_same_quantization(input1, output)))) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	return finish_prepare(data, _prepared);
}


template<bool kLarger>
static status_t
run_compare(const NeuralOpContext& context)
{
	switch (context.InputInfo(0).type) {
		case B_NEURAL_FLOAT32:
			if (kLarger)
				run_binary<float, Larger<float> >(context, {}, 65536);
			else
				run_binary<float, Smaller<float> >(context, {}, 65536);
			break;
		case B_NEURAL_UINT8:
			if (kLarger)
				run_binary<uint8, Larger<uint8> >(context, {}, 131072);
			else
				run_binary<uint8, Smaller<uint8> >(context, {}, 131072);
			break;
		default:
			if (kLarger)
				run_binary<int8, Larger<int8> >(context, {}, 131072);
			else
				run_binary<int8, Smaller<int8> >(context, {}, 131072);
			break;
	}
	return B_OK;
}


// #pragma mark - PRELU


/*!	A rectifier whose slope for negative values is a tensor (usually one
	number per channel): an operator on two tensors like the others here.
*/
struct QuantizedPrelu {
	neural_scaling	scaling;
	neural_scaling	slope_scaling;
	int32			offset;
	int32			slope_offset;
	int32			output_offset;
	int32			min;
	int32			max;

	QuantizedPrelu(const BinaryData& data)
		:
		offset(data.offset1),
		slope_offset(data.offset2),
		output_offset(data.output_offset),
		min(data.activation_min),
		max(data.activation_max)
	{
		neural_set_scaling(scaling, data.multiplier1, data.shift1);
		neural_set_scaling(slope_scaling, data.output_multiplier,
			data.output_shift);
	}

	inline int32 operator()(int32 a, int32 slope) const
	{
		int32 value = a + offset;
		int32 positive = neural_scale(value, scaling);
		int32 negative = neural_scale(value * (slope + slope_offset),
			slope_scaling);
		return neural_clamp((value >= 0 ? positive : negative)
			+ output_offset, min, max);
	}
};


struct FloatPrelu {
	inline float operator()(float a, float slope) const
	{
		return a >= 0 ? a : a * slope;
	}
};


static status_t
prepare_prelu(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	BinaryData data;
	if (!common_checks(graph, op, &data))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& slope = graph->Tensor(op->inputs[1]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	// the slopes may be repeated over the input, not the other way round
	if (!neural_same_shape(input, output))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	if (input.type == B_NEURAL_FLOAT32)
		return finish_prepare(data, _prepared);

	data.offset1 = -input.zero_point;
	data.offset2 = -slope.zero_point;
	data.output_offset = output.zero_point;
	neural_type_range(output.type, &data.activation_min,
		&data.activation_max);

	// in single precision, as the reference does
	float factor = input.scale / output.scale;
	float slopeFactor = input.scale * slope.scale / output.scale;
	if (!neural_checked_quantize_multiplier(factor, &data.multiplier1,
			&data.shift1)
		|| !neural_checked_quantize_multiplier(slopeFactor,
			&data.output_multiplier, &data.output_shift)) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	return finish_prepare(data, _prepared);
}


static status_t
run_prelu(const NeuralOpContext& context)
{
	QuantizedPrelu quantized(*(const BinaryData*)context.prepared);

	switch (context.InputInfo(0).type) {
		case B_NEURAL_FLOAT32:
			run_binary<float, FloatPrelu>(context, {}, 65536);
			break;
		case B_NEURAL_UINT8:
			run_binary<uint8, QuantizedPrelu>(context, quantized, 32768);
			break;
		default:
			run_binary<int8, QuantizedPrelu>(context, quantized, 32768);
			break;
	}
	return B_OK;
}


}	// unnamed namespace


const NeuralKernel kAddKernel = {
	B_NEURAL_OP_ADD, &prepare_add, &run_add<1>
};

const NeuralKernel kSubKernel = {
	B_NEURAL_OP_SUB, &prepare_add, &run_add<-1>
};

const NeuralKernel kMulKernel = {
	B_NEURAL_OP_MUL, &prepare_mul, &run_mul
};

const NeuralKernel kDivKernel = {
	B_NEURAL_OP_DIV, &prepare_mul, &run_div
};

const NeuralKernel kMaximumKernel = {
	B_NEURAL_OP_MAXIMUM, &prepare_compare, &run_compare<true>
};

const NeuralKernel kMinimumKernel = {
	B_NEURAL_OP_MINIMUM, &prepare_compare, &run_compare<false>
};

const NeuralKernel kSquaredDifferenceKernel = {
	B_NEURAL_OP_SQUARED_DIFFERENCE, &prepare_squared_difference,
	&run_squared_difference
};

const NeuralKernel kPreluKernel = {
	B_NEURAL_OP_PRELU, &prepare_prelu, &run_prelu
};


}	// namespace BPrivate
