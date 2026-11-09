/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Operators that put every element through a function, and SOFTMAX.

	A quantized element has one of 256 values, so its function is a table
	that prepare() fills in; what goes into the table is computed the way
	the TensorFlow Lite reference kernels compute each element (in fixed
	point for the rectifiers and HARD_SWISH, through single precision
	floating point for LOGISTIC and TANH), so that the results agree to
	the bit. SOFTMAX follows the reference's fixed-point arithmetic, which
	is that of the gemmlowp library.
*/


#pragma GCC optimize("O3")


#include "Kernels.h"

#include <pthread.h>


namespace BPrivate {


namespace {


struct UnaryData {
	int32	elements;
	//! quantized: output value by input value (as an unsigned byte)
	uint8	table[256];
	//! LEAKY_RELU
	float	alpha;
	//! GELU: see gelu_table()
	const float* shares;
};


struct UnaryRun {
	const UnaryData*	data;
	const void*			input;
	void*				output;
};


static void
table_slice(void* cookie, int32 first, int32 end)
{
	const UnaryRun& run = *(const UnaryRun*)cookie;
	const uint8* table = run.data->table;
	const uint8* input = (const uint8*)run.input;
	uint8* output = (uint8*)run.output;
	for (int32 i = first; i < end; i++)
		output[i] = table[input[i]];
}


// The functions on floating point numbers; kGrain is how many elements
// are worth a thread of their own (a few dozen microseconds of work).

struct Relu {
	static const int32 kGrain = 65536;

	static inline float Apply(float value, const UnaryData&)
	{
		return value > 0 ? value : 0;
	}
};


struct Relu6 {
	static const int32 kGrain = 65536;

	static inline float Apply(float value, const UnaryData&)
	{
		return neural_clamp(value, 0.0f, 6.0f);
	}
};


struct Logistic {
	static const int32 kGrain = 4096;

	static inline float Apply(float value, const UnaryData&)
	{
		// beyond these the result is 1, or the exponential alone, in
		// single precision
		if (value > 16.619047164916992188f)
			return 1;
		if (value < -9)
			return expf(value);
		return 1 / (1 + expf(-value));
	}
};


struct Tanh {
	static const int32 kGrain = 4096;

	static inline float Apply(float value, const UnaryData&)
	{
		return tanhf(value);
	}
};


struct HardSwish {
	static const int32 kGrain = 65536;

	static inline float Apply(float value, const UnaryData&)
	{
		return value * neural_clamp(value + 3, 0.0f, 6.0f) / 6;
	}
};


struct LeakyRelu {
	static const int32 kGrain = 65536;

	static inline float Apply(float value, const UnaryData& data)
	{
		return value > 0 ? value : value * data.alpha;
	}
};


struct Rsqrt {
	static const int32 kGrain = 32768;

	static inline float Apply(float value, const UnaryData&)
	{
		return 1 / sqrtf(value);
	}
};


// GELU is x times the share of a normal distribution that lies below x.
// The share comes from a table with a straight line between neighbours:
// the error function it is made of costs ten times as much, and a
// transformer puts millions of numbers through this for every input. With
// 256 steps to the unit the result is off by less than 1e-6 of x.

static const int32 kGeluSteps = 256;
//! beyond +-kGeluRange the share is 1 or 0 to all the digits there are
static const int32 kGeluRange = 8;
static const int32 kGeluEntries = 2 * kGeluRange * kGeluSteps + 2;


struct Gelu {
	static const int32 kGrain = 16384;

	static inline float Apply(float value, const UnaryData& data)
	{
		// (written so that something that is not a number gets a place
		// in the table too; it stays what it is in the product)
		float clamped = value > -kGeluRange
			? (value < kGeluRange ? value : (float)kGeluRange)
			: (float)-kGeluRange;
		float position = (clamped + kGeluRange) * kGeluSteps;
		int32 index = (int32)position;
		float fraction = position - (float)index;
		const float* shares = data.shares;
		return value * (shares[index]
			+ fraction * (shares[index + 1] - shares[index]));
	}
};


template<typename Function>
static void
float_slice(void* cookie, int32 first, int32 end)
{
	NeuralFlushDenormals flush;
	const UnaryRun& run = *(const UnaryRun*)cookie;
	const float* __restrict input = (const float*)run.input;
	float* __restrict output = (float*)run.output;
	const UnaryData& data = *run.data;
	for (int32 i = first; i < end; i++)
		output[i] = Function::Apply(input[i], data);
}


template<typename Function>
static status_t
run_unary(const NeuralOpContext& context)
{
	const UnaryData* data = (const UnaryData*)context.prepared;
	UnaryRun run = { data, context.Input<void>(0), context.Output<void>(0) };

	if (context.InputInfo(0).type == B_NEURAL_FLOAT32) {
		context.pool->Run(data->elements, Function::kGrain,
			&float_slice<Function>, &run);
	} else
		context.pool->Run(data->elements, 65536, &table_slice, &run);
	return B_OK;
}


/*!	What all of these ask for: one input and an output of its type and
	shape; quantized: both with a scale. */
static status_t
check_unary(const NeuralGraphData* graph, const neural_graph_operator* op,
	UnaryData* data)
{
	if (op->input_count != 1 || op->inputs[0] < 0 || op->output_count != 1)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);

	memset(data, 0, sizeof(UnaryData));
	data->elements = neural_element_count(input);
	if (input.type != output.type || !neural_same_shape(input, output)
		|| data->elements < 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	if (input.type == B_NEURAL_FLOAT32)
		return B_OK;

	if (!neural_is_quantized_8(input) || !neural_is_quantized_8(output)
		|| !neural_valid_zero_point(input)
		|| !neural_valid_zero_point(output)) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}
	return B_OK;
}


static status_t
finish_unary(const UnaryData& header, void** _prepared)
{
	UnaryData* data = (UnaryData*)malloc(sizeof(UnaryData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


// #pragma mark - RELU, RELU6, LEAKY_RELU


/*!	The rectifiers also convert between the scales of input and output.
	\a alphaFactor is what negative inputs are multiplied with on the way,
	0 to cut them off. */
static status_t
fill_relu_table(const neural_graph_tensor& input,
	const neural_graph_tensor& output, neural_activation activation,
	bool leaky, float alphaFactor, uint8* table)
{
	int32 min, max;
	neural_type_range(input.type, &min, &max);

	int32 activationMin = min;
	int32 activationMax = max;
	if (!leaky && neural_quantized_activation_range(activation, output,
			&activationMin, &activationMax) != B_OK) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	// in single precision, as the reference does
	float factor = input.scale / output.scale;
	int32 multiplier;
	int shift;
	if (!neural_checked_quantize_multiplier(factor, &multiplier, &shift))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	// a negative slope is applied as a positive one and the sign put back
	int32 alphaMultiplier = 0;
	int alphaShift = 0;
	if (leaky && !neural_checked_quantize_multiplier(fabsf(alphaFactor),
			&alphaMultiplier, &alphaShift)) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	for (int32 stored = min; stored <= max; stored++) {
		int32 value = stored - input.zero_point;
		int32 result;
		if (value >= 0 || !leaky) {
			result = neural_multiply_by_quantized_multiplier(value,
				multiplier, shift);
		} else {
			result = neural_multiply_by_quantized_multiplier(value,
				alphaMultiplier, alphaShift);
			if (alphaFactor < 0)
				result = -result;
		}
		table[(uint8)stored] = (uint8)neural_clamp(
			(int32)((uint32)result + (uint32)output.zero_point),
			activationMin, activationMax);
	}
	return B_OK;
}


template<neural_activation kActivation>
static status_t
prepare_relu(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	UnaryData data;
	status_t status = check_unary(graph, op, &data);
	if (status != B_OK)
		return status;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	if (input.type != B_NEURAL_FLOAT32) {
		status = fill_relu_table(input, graph->Tensor(op->outputs[0]),
			kActivation, false, 0, data.table);
		if (status != B_OK)
			return status;
	}
	return finish_unary(data, _prepared);
}


static status_t
prepare_leaky_relu(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	UnaryData data;
	status_t status = check_unary(graph, op, &data);
	if (status != B_OK)
		return status;
	if (op->params == NULL)
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);
	data.alpha = ((const neural_leaky_relu_params*)op->params)->alpha;
	if (!isfinite(data.alpha))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	if (input.type != B_NEURAL_FLOAT32) {
		status = fill_relu_table(input, output, B_NEURAL_ACTIVATION_NONE,
			true, input.scale * data.alpha / output.scale, data.table);
		if (status != B_OK)
			return status;
	}
	return finish_unary(data, _prepared);
}


// #pragma mark - RSQRT, GELU


//! For the functions that are there for floating point numbers only.
static status_t
prepare_float_only(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	UnaryData data;
	status_t status = check_unary(graph, op, &data);
	if (status != B_OK)
		return status;
	if (graph->Tensor(op->inputs[0]).type != B_NEURAL_FLOAT32)
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	return finish_unary(data, _prepared);
}


static float sGeluShares[2][kGeluEntries];
static pthread_once_t sGeluOnce = PTHREAD_ONCE_INIT;


static void
fill_gelu_tables()
{
	for (int32 i = 0; i < kGeluEntries; i++) {
		double x = (double)i / kGeluSteps - kGeluRange;
		sGeluShares[0][i] = (float)(0.5 * (1 + erf(x * M_SQRT1_2)));
		// the approximation of Hendrycks and Gimpel
		sGeluShares[1][i] = (float)(0.5 * (1 + tanh(sqrt(2 / M_PI)
			* (x + 0.044715 * x * x * x))));
	}
}


static status_t
prepare_gelu(const NeuralGraphData* graph, const neural_graph_operator* op,
	void** _prepared)
{
	UnaryData data;
	status_t status = check_unary(graph, op, &data);
	if (status != B_OK)
		return status;
	if (op->params == NULL
		|| graph->Tensor(op->inputs[0]).type != B_NEURAL_FLOAT32) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	pthread_once(&sGeluOnce, &fill_gelu_tables);
	data.shares = sGeluShares[
		((const neural_gelu_params*)op->params)->approximate ? 1 : 0];
	return finish_unary(data, _prepared);
}


// #pragma mark - LOGISTIC, TANH


static float
logistic(float value)
{
	return 1.0f / (1.0f + expf(-value));
}


/*!	The reference fills its table through single precision floating
	point; with the same steps in the same order this table is the same
	(as long as the C library's expf() and tanhf() agree, which they do
	on all but a few arguments). */
template<float (*kFunction)(float)>
static status_t
prepare_float_table(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	UnaryData data;
	status_t status = check_unary(graph, op, &data);
	if (status != B_OK)
		return status;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);
	if (input.type != B_NEURAL_FLOAT32) {
		int32 min, max;
		neural_type_range(input.type, &min, &max);

		float inverseScale = 1 / output.scale;
		for (int32 stored = min; stored <= max; stored++) {
			float real = input.scale * (stored - input.zero_point);
			float scaled = roundf(kFunction(real) * inverseScale)
				+ output.zero_point;
			// compared before it is made an integer: it may be anything
			int32 result = !(scaled > min) ? min
				: (scaled > max ? max : (int32)scaled);
			data.table[(uint8)stored] = (uint8)result;
		}
	}
	return finish_unary(data, _prepared);
}


// #pragma mark - HARD_SWISH


static inline int16
saturating_left_shift_16(int16 value, int amount)
{
	int64 result = (int64)value * ((int64)1 << amount);
	return (int16)(result > SHRT_MAX ? SHRT_MAX
		: (result < SHRT_MIN ? SHRT_MIN : result));
}


static inline int16
rounding_doubling_high_mul_16(int16 a, int16 b)
{
	if (a == b && a == SHRT_MIN)
		return SHRT_MAX;
	int32 product = (int32)a * b;
	int32 nudge = product >= 0 ? (1 << 14) : (1 - (1 << 14));
	return (int16)((product + nudge) / (1 << 15));
}


static inline int16
doubling_high_mul_16(int16 a, int16 b)
{
	if (a == b && a == SHRT_MIN)
		return SHRT_MAX;
	return (int16)(((int32)a * b) / (1 << 15));
}


//! Q31 factor to Q15, rounded.
static inline int16
narrow_multiplier(int32 multiplier)
{
	if (multiplier >= INT_MAX - (1 << 15))
		return SHRT_MAX;
	return (int16)((multiplier + (1 << 15)) >> 16);
}


/*!	x * relu6(x + 3) / 6 in 16 bit fixed point: the input on a finer
	scale, the "relu-ish" factor as a number from 0 to 1 in 15 bits. */
static status_t
prepare_hard_swish(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	UnaryData data;
	status_t status = check_unary(graph, op, &data);
	if (status != B_OK)
		return status;

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);
	if (input.type == B_NEURAL_FLOAT32)
		return finish_unary(data, _prepared);

	// all in single precision, as the reference does
	float fineScale = (1.0f / 128.0f) * input.scale;
	float reluishScale = 3.0f / 32768.0f;
	float outputFactor = fineScale / output.scale;
	float reluishFactor = fineScale / reluishScale;

	int32 multiplier;
	int outputExponent;
	int reluishExponent;
	if (!neural_checked_quantize_multiplier(outputFactor, &multiplier,
			&outputExponent) || outputExponent > 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}
	int16 outputMultiplier = narrow_multiplier(multiplier);

	if (!neural_checked_quantize_multiplier(reluishFactor, &multiplier,
			&reluishExponent) || reluishExponent > 15) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}
	int16 reluishMultiplier = narrow_multiplier(multiplier);

	int32 min, max;
	neural_type_range(input.type, &min, &max);

	for (int32 stored = min; stored <= max; stored++) {
		int16 fine = (int16)((stored - input.zero_point) * (1 << 7));
		int16 scaled = rounding_doubling_high_mul_16(fine, outputMultiplier);

		int16 reluish = fine;
		if (reluishExponent > 0) {
			reluish = saturating_left_shift_16(reluish,
				reluishExponent - 1);
		}
		reluish = rounding_doubling_high_mul_16(reluish, reluishMultiplier);
		if (reluishExponent > 0)
			reluish = saturating_left_shift_16(reluish, 1);
		if (reluishExponent < 0) {
			reluish = (int16)neural_rounding_divide_by_pot(reluish,
				-reluishExponent);
		}
		reluish = (int16)((reluish + (1 << 15)) >> 1);

		// The sum with the zero point wraps in 16 bits in the reference;
		// kept, so that the same models give the same numbers.
		int16 result = (int16)neural_rounding_divide_by_pot(
			doubling_high_mul_16(reluish, scaled), -outputExponent);
		result = (int16)(result + output.zero_point);
		data.table[(uint8)stored] = (uint8)neural_clamp((int32)result, min,
			max);
	}

	return finish_unary(data, _prepared);
}


// #pragma mark - SOFTMAX


/*!	Fixed-point numbers as gemmlowp has them: an int32 with a number of
	integer bits (Q0.31 holds -1 .. 1, Q5.26 -32 .. 32, ...). The
	functions below do what the functions of that library the reference
	calls do, step by step.
*/

static inline int32
fixed_multiply(int32 a, int32 b)
{
	return neural_saturating_rounding_doubling_high_mul(a, b);
}


static inline int32
saturating_left_shift(int32 value, int amount)
{
	int32 threshold = (int32)(((int64)1 << (31 - amount)) - 1);
	if (value > threshold)
		return INT_MAX;
	if (value < -threshold)
		return INT_MIN;
	return value * (1 << amount);
}


/*!	e^a for a Q5.26 number a <= 0, as Q0.31: a is split into a part
	between -1/4 and 0, whose exponential is a polynomial around -1/8, and
	a sum of powers of two, each of which is a factor. */
static int32
exp_on_negative_values(int32 a)
{
	if (a == 0)
		return INT_MAX;

	const int32 kQuarter = 1 << 24;
	int32 rest = (a & (kQuarter - 1)) - kQuarter;

	// exp(x - 1/8) with x = rest + 1/8, in Q0.31
	const int32 kConstantTerm = 1895147668;
		// exp(-1/8)
	const int32 kOneThird = 715827883;
	int32 x = saturating_left_shift(rest, 5) + (1 << 28);
	int32 x2 = fixed_multiply(x, x);
	int32 x3 = fixed_multiply(x2, x);
	int32 x4 = fixed_multiply(x2, x2);
	int32 x4Quarter = neural_rounding_divide_by_pot(x4, 2);
	int32 series = neural_rounding_divide_by_pot(
		fixed_multiply(x4Quarter + x3, kOneThird) + x2, 1);
	int32 result = kConstantTerm + fixed_multiply(kConstantTerm, x + series);

	// exp(-1/4), exp(-1/2), exp(-1), ... exp(-16)
	static const int32 kFactors[7] = { 1672461947, 1302514674, 790015084,
		290630308, 39332535, 720401, 242 };
	int32 remainder = rest - a;
	for (int i = 0; i < 7; i++) {
		if ((remainder & (1 << (24 + i))) != 0)
			result = fixed_multiply(result, kFactors[i]);
	}
	return result;
}


/*!	1 / (1 + a) for a Q0.31 number a from 0 to 1, as Q0.31: three steps
	of Newton's method on half the denominator. */
static int32
one_over_one_plus(int32 a)
{
	int64 sum = (int64)a + INT_MAX;
	int32 halfDenominator = (int32)((sum + (sum >= 0 ? 1 : -1)) / 2);

	// Q2.29 from here
	const int32 k48Over17 = 1515870810;
	const int32 kMinus32Over17 = -1010580540;
	const int32 kOne = 1 << 29;
	int32 x = k48Over17 + fixed_multiply(halfDenominator, kMinus32Over17);
	for (int i = 0; i < 3; i++) {
		int32 product = fixed_multiply(halfDenominator, x);
		x = x + saturating_left_shift(fixed_multiply(x, kOne - product), 2);
	}
	return saturating_left_shift(x, 1);
}


struct SoftmaxData {
	int32	rows;
	int32	depth;
	float	beta;
	//! the lowest stored value of the type: where the results start
	int32	minimum;
	//! e^(scaled difference to the largest element) by difference
	int32	exponentials[256];
	//! differences from this one on count as "no chance": result 0
	int32	cutoff;
};


struct SoftmaxRun {
	const SoftmaxData*	data;
	const void*			input;
	void*				output;
};


template<typename Type>
static void
softmax_rows(void* cookie, int32 first, int32 end)
{
	const SoftmaxRun& run = *(const SoftmaxRun*)cookie;
	const SoftmaxData& data = *run.data;
	int32 depth = data.depth;

	for (int32 row = first; row < end; row++) {
		const Type* input = (const Type*)run.input + (size_t)row * depth;
		Type* output = (Type*)run.output + (size_t)row * depth;

		int32 largest = input[0];
		for (int32 i = 1; i < depth; i++)
			largest = input[i] > largest ? (int32)input[i] : largest;

		// the sum of the exponentials in Q12.19; like the reference's it
		// wraps when there are thousands of nearly equal elements
		uint32 sum = 0;
		for (int32 i = 0; i < depth; i++) {
			int32 difference = largest - input[i];
			if (difference < data.cutoff) {
				sum += (uint32)neural_rounding_divide_by_pot(
					data.exponentials[difference], 12);
			}
		}

		// 1 / sum: the sum shifted to 1 .. 2, its reciprocal, and the
		// shift on the result
		int headroom = sum != 0 ? __builtin_clz(sum) : 31;
		int32 scale = one_over_one_plus(
			(int32)((sum << headroom) - ((uint32)1 << 31)));
		int exponent = 12 - headroom + 31 - 8;

		for (int32 i = 0; i < depth; i++) {
			int32 difference = largest - input[i];
			int32 value = 0;
			if (difference < data.cutoff && exponent <= 31) {
				value = neural_rounding_divide_by_pot(fixed_multiply(scale,
					data.exponentials[difference]), exponent);
			}
			output[i] = (Type)(value > 255 ? data.minimum + 255
				: data.minimum + value);
		}
	}
}


static void
softmax_float_rows(void* cookie, int32 first, int32 end)
{
	NeuralFlushDenormals flush;
	const SoftmaxRun& run = *(const SoftmaxRun*)cookie;
	const SoftmaxData& data = *run.data;
	int32 depth = data.depth;

	for (int32 row = first; row < end; row++) {
		const float* input = (const float*)run.input + (size_t)row * depth;
		float* output = (float*)run.output + (size_t)row * depth;

		float largest = input[0];
		for (int32 i = 1; i < depth; i++)
			largest = input[i] > largest ? input[i] : largest;

		float sum = 0;
		for (int32 i = 0; i < depth; i++) {
			output[i] = expf((input[i] - largest) * data.beta);
			sum += output[i];
		}
		for (int32 i = 0; i < depth; i++)
			output[i] = output[i] / sum;
	}
}


static status_t
prepare_softmax(const NeuralGraphData* graph,
	const neural_graph_operator* op, void** _prepared)
{
	if (op->input_count != 1 || op->inputs[0] < 0 || op->output_count != 1
		|| op->params == NULL) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	const neural_graph_tensor& input = graph->Tensor(op->inputs[0]);
	const neural_graph_tensor& output = graph->Tensor(op->outputs[0]);
	int32 elements = neural_element_count(input);
	if (input.type != output.type || !neural_same_shape(input, output)
		|| input.rank < 1 || elements < 0) {
		return B_NEURAL_UNSUPPORTED_OPERATOR;
	}

	SoftmaxData header;
	memset(&header, 0, sizeof(header));
	header.depth = input.dimensions[input.rank - 1];
	header.rows = elements / header.depth;
	header.beta = ((const neural_softmax_params*)op->params)->beta;
	if (!isfinite(header.beta))
		return B_NEURAL_UNSUPPORTED_OPERATOR;

	if (input.type != B_NEURAL_FLOAT32) {
		// The results are fractions of 256 from the lowest value of the
		// type on; the output must be quantized to match.
		int32 max;
		if (!neural_is_quantized_8(input) || !neural_is_quantized_8(output)
			|| !neural_type_range(output.type, &header.minimum, &max)
			|| output.zero_point != header.minimum
			|| fabsf(output.scale - 1.0f / 256) > 0.001f / 256) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}

		// differences are scaled to Q5.26
		double factor = (double)header.beta * (double)input.scale
			* (1 << 26);
		if (factor > 2147483647.0)
			factor = 2147483647.0;
		int32 multiplier;
		int shift;
		if (!(factor > 1) || !neural_checked_quantize_multiplier(factor,
				&multiplier, &shift)) {
			return B_NEURAL_UNSUPPORTED_OPERATOR;
		}

		// the largest difference that still fits after scaling
		double radius = floor(31.0 * ((int64)1 << 26)
			/ (double)((int64)1 << shift));
		header.cutoff = radius >= 255 ? 256 : (int32)radius + 1;
		for (int32 i = 0; i < header.cutoff; i++) {
			header.exponentials[i] = exp_on_negative_values(
				fixed_multiply(-i * (1 << shift), multiplier));
		}
	}

	SoftmaxData* data = (SoftmaxData*)malloc(sizeof(SoftmaxData));
	if (data == NULL)
		return B_NO_MEMORY;
	*data = header;
	*_prepared = data;
	return B_OK;
}


static status_t
run_softmax(const NeuralOpContext& context)
{
	const SoftmaxData* data = (const SoftmaxData*)context.prepared;
	SoftmaxRun run = { data, context.Input<void>(0),
		context.Output<void>(0) };

	NeuralThreadPool::Function function;
	switch (context.InputInfo(0).type) {
		case B_NEURAL_FLOAT32:
			function = &softmax_float_rows;
			break;
		case B_NEURAL_UINT8:
			function = &softmax_rows<uint8>;
			break;
		default:
			function = &softmax_rows<int8>;
			break;
	}

	int32 grain = 8192 / data->depth;
	context.pool->Run(data->rows, grain < 1 ? 1 : grain, function, &run);
	return B_OK;
}


}	// unnamed namespace


const NeuralKernel kReluKernel = {
	B_NEURAL_OP_RELU, &prepare_relu<B_NEURAL_ACTIVATION_RELU>,
	&run_unary<Relu>
};

const NeuralKernel kRelu6Kernel = {
	B_NEURAL_OP_RELU6, &prepare_relu<B_NEURAL_ACTIVATION_RELU6>,
	&run_unary<Relu6>
};

const NeuralKernel kLogisticKernel = {
	B_NEURAL_OP_LOGISTIC, &prepare_float_table<&logistic>,
	&run_unary<Logistic>
};

const NeuralKernel kTanhKernel = {
	B_NEURAL_OP_TANH, &prepare_float_table<&tanhf>, &run_unary<Tanh>
};

const NeuralKernel kHardSwishKernel = {
	B_NEURAL_OP_HARD_SWISH, &prepare_hard_swish, &run_unary<HardSwish>
};

const NeuralKernel kLeakyReluKernel = {
	B_NEURAL_OP_LEAKY_RELU, &prepare_leaky_relu, &run_unary<LeakyRelu>
};

const NeuralKernel kRsqrtKernel = {
	B_NEURAL_OP_RSQRT, &prepare_float_only, &run_unary<Rsqrt>
};

const NeuralKernel kGeluKernel = {
	B_NEURAL_OP_GELU, &prepare_gelu, &run_unary<Gelu>
};

const NeuralKernel kSoftmaxKernel = {
	B_NEURAL_OP_SOFTMAX, &prepare_softmax, &run_softmax
};


}	// namespace BPrivate
