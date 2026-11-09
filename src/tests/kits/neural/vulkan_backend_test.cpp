/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Checks the vulkan back end against the kit's CPU interpreter.

		vulkan_backend_test [-d <device>] [-v] [<filter>]
		vulkan_backend_test [-d <device>] --runs <n> <model>
		vulkan_backend_test [-d <device>] --threads <n> <model>
		vulkan_backend_test [-d <device>] --cycles <n> <model>

	Without a model, small networks made with BNeuralModelBuilder are run
	on random numbers on the device and on the CPU and the outputs
	compared: every operator the back end takes, in the shapes and with
	the parameters that go different ways through its shaders, and some
	it has to leave to the CPU. A filter runs the cases with that text
	in their name only.

	With a model file:
	--runs     runs it n times on the same input; every run must give
	           exactly what the first one gave, and that must be what the
	           CPU gives
	--threads  the same from n sessions at once, a thread each
	--cycles   sets a session up, runs it and deletes it n times; the
	           device memory in use must be the same after each, and the
	           team must not grow

	Exits with 0 when everything passed, with 1 otherwise (also when
	there is no such device, or the CPU cannot run a case).
*/


#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Message.h>
#include <NeuralKit.h>
#include <OS.h>
#include <String.h>


//! Largest difference accepted, as a fraction of the largest output.
static const float kTolerance = 1e-4f;

static BNeuralDevice sDevice;
static bool sVerbose;
static const char* sFilter;
static uint32 sRandom = 0x2545f491;
static int32 sPassed;
static int32 sFailed;


static float
random_float()
{
	sRandom ^= sRandom << 13;
	sRandom ^= sRandom >> 17;
	sRandom ^= sRandom << 5;
	return (int32)(sRandom & 0xffff) / 32768.0f - 1.0f;
}


struct Shape {
	int32	rank;
	int32	dimensions[4];

	Shape(int32 a)
		: rank(1) { Set(a, 0, 0, 0); }
	Shape(int32 a, int32 b)
		: rank(2) { Set(a, b, 0, 0); }
	Shape(int32 a, int32 b, int32 c)
		: rank(3) { Set(a, b, c, 0); }
	Shape(int32 a, int32 b, int32 c, int32 d)
		: rank(4) { Set(a, b, c, d); }

	void Set(int32 a, int32 b, int32 c, int32 d)
	{
		dimensions[0] = a;
		dimensions[1] = b;
		dimensions[2] = c;
		dimensions[3] = d;
	}

	int32 Count() const
	{
		int32 count = 1;
		for (int32 i = 0; i < rank; i++)
			count *= dimensions[i];
		return count;
	}

	BTensorInfo Info(neural_data_type type = B_NEURAL_FLOAT32) const
	{
		return BTensorInfo(type, rank, dimensions);
	}
};


/*!	A network in the making: what BNeuralModelBuilder does, with random
	numbers for the constants. */
struct Network {
	BNeuralModelBuilder	builder;

	int32 Input(const Shape& shape)
	{
		return builder.AddInput(shape.Info());
	}

	int32 Constant(const Shape& shape, float scale = 1)
	{
		int32 count = shape.Count();
		float* values = (float*)malloc(count * sizeof(float));
		for (int32 i = 0; i < count; i++)
			values[i] = random_float() * scale;
		int32 tensor = builder.AddConstant(shape.Info(), values,
			count * sizeof(float));
		free(values);
		return tensor;
	}

	int32 Integers(const Shape& shape, const int32* values)
	{
		return builder.AddConstant(shape.Info(B_NEURAL_INT32), values,
			shape.Count() * sizeof(int32));
	}

	int32 Operator(neural_operator_code code, const Shape& output,
		int32 a, int32 b = -2, int32 c = -2, const void* params = NULL,
		size_t paramsSize = 0)
	{
		int32 inputs[3] = { a, b, c };
		int32 count = b == -2 ? 1 : (c == -2 ? 2 : 3);
		return builder.AddOperator(code, inputs, count, output.Info(),
			params, paramsSize);
	}

	int32 Output(int32 tensor)
	{
		builder.MarkOutput(tensor);
		return tensor;
	}
};


static bool
wanted(const char* name)
{
	return sFilter == NULL || strstr(name, sFilter) != NULL;
}


static void
report(const char* name, bool passed, const char* format, ...)
{
	char details[256];
	va_list args;
	va_start(args, format);
	vsnprintf(details, sizeof(details), format, args);
	va_end(args);

	if (passed)
		sPassed++;
	else
		sFailed++;
	if (!passed || sVerbose)
		printf("%-6s %-44s %s\n", passed ? "ok" : "FAILED", name, details);
}


//! The largest difference of two tensors over the largest value.
static float
difference(const BTensor& a, const BTensor& b)
{
	const float* x = (const float*)a.Data();
	const float* y = (const float*)b.Data();
	int64 count = a.CountElements();
	if (count != b.CountElements())
		return INFINITY;

	float largest = 0;
	float scale = 1e-6f;
	for (int64 i = 0; i < count; i++) {
		if (isnan(x[i]) != isnan(y[i]))
			return INFINITY;
		largest = fmaxf(largest, fabsf(x[i] - y[i]));
		scale = fmaxf(scale, fabsf(y[i]));
	}
	return largest / scale;
}


/*!	Runs the network on the device, all of it, and on the CPU, and
	compares what comes out. \a inputScale makes the inputs large enough
	for activations to cut something off. */
static void
check(const char* name, BNeuralModelBuilder& builder, float inputScale = 1)
{
	if (!wanted(name))
		return;

	BNeuralModel model;
	status_t status = builder.Build(&model);
	if (status != B_OK) {
		report(name, false, "the model cannot be built: %s",
			strerror(status));
		return;
	}

	BNeuralDevice cpu;
	BNeuralRoster::Default()->FindDevice("cpu", &cpu);
	BNeuralSession reference(model, cpu);
	if (reference.InitCheck() != B_OK) {
		report(name, false, "the CPU cannot run it: %s",
			strerror(reference.InitCheck()));
		return;
	}

	int32 supported = sDevice.CountSupportedOperators(model);
	BNeuralSession session(model, sDevice, B_NEURAL_NO_FALLBACK);
	if (session.InitCheck() != B_OK) {
		report(name, false, "the device takes %" B_PRId32 " of %" B_PRId32
			" operators: %s", supported, model.CountOperators(),
			strerror(session.InitCheck()));
		return;
	}

	for (int32 i = 0; i < session.CountInputs(); i++) {
		BTensor* input = session.InputAt(i);
		float* values = (float*)input->Data();
		for (int64 j = 0; j < input->CountElements(); j++)
			values[j] = random_float() * inputScale;
		reference.InputAt(i)->CopyFrom(*input);
	}

	status = session.Run();
	if (status == B_OK)
		status = reference.Run();
	if (status != B_OK) {
		report(name, false, "a run failed: %s", strerror(status));
		return;
	}

	float largest = 0;
	for (int32 i = 0; i < session.CountOutputs(); i++) {
		largest = fmaxf(largest, difference(*session.OutputAt(i),
			*reference.OutputAt(i)));
	}
	report(name, largest <= kTolerance, "difference %g", largest);
}


static void
check(const char* name, Network& network, float inputScale = 1)
{
	check(name, network.builder, inputScale);
}


//! The network is one the device must leave alone, all of it.
static void
check_refused(const char* name, BNeuralModelBuilder& builder)
{
	if (!wanted(name))
		return;

	BNeuralModel model;
	status_t status = builder.Build(&model);
	if (status != B_OK) {
		report(name, false, "the model cannot be built: %s",
			strerror(status));
		return;
	}

	int32 supported = sDevice.CountSupportedOperators(model);
	if (supported != 0) {
		report(name, false, "the device takes %" B_PRId32 " operators",
			supported);
		return;
	}

	// and a session is one of the CPU then
	BNeuralSession session(model, sDevice);
	neural_run_info info;
	memset(&info, 0, sizeof(info));
	if (session.InitCheck() == B_OK)
		session.GetRunInfo(&info);
	report(name, info.device_operators == 0, "refused");
}


static void
check_refused(const char* name, Network& network)
{
	check_refused(name, network.builder);
}


// #pragma mark - cases


static int32
output_size(neural_padding padding, int32 size, int32 filter, int32 stride,
	int32 dilation)
{
	if (padding == B_NEURAL_PADDING_SAME)
		return (size + stride - 1) / stride;
	return (size + stride - ((filter - 1) * dilation + 1)) / stride;
}


struct Convolution {
	int32				batches, height, width, in, out;
	int32				kernelHeight, kernelWidth;
	int32				strideHeight, strideWidth;
	int32				dilationHeight, dilationWidth;
	neural_padding		padding;
	neural_activation	activation;
	bool				bias;
	bool				depthwise;
};


static void
check_convolution(const char* name, const Convolution& c)
{
	neural_conv_params params;
	memset(&params, 0, sizeof(params));
	params.padding = c.padding;
	params.stride_width = c.strideWidth;
	params.stride_height = c.strideHeight;
	params.dilation_width = c.dilationWidth;
	params.dilation_height = c.dilationHeight;
	params.depth_multiplier = c.depthwise ? c.out / c.in : 0;
	params.activation = c.activation;

	Network network;
	int32 input = network.Input(Shape(c.batches, c.height, c.width, c.in));
	int32 weights = network.Constant(c.depthwise
		? Shape(1, c.kernelHeight, c.kernelWidth, c.out)
		: Shape(c.out, c.kernelHeight, c.kernelWidth, c.in));
	int32 bias = c.bias ? network.Constant(Shape(c.out)) : -1;

	Shape output(c.batches,
		output_size(c.padding, c.height, c.kernelHeight, c.strideHeight,
			c.dilationHeight),
		output_size(c.padding, c.width, c.kernelWidth, c.strideWidth,
			c.dilationWidth), c.out);
	network.Output(network.Operator(c.depthwise
			? B_NEURAL_OP_DEPTHWISE_CONV_2D : B_NEURAL_OP_CONV_2D,
		output, input, weights, bias, &params, sizeof(params)));
	check(name, network, 2);
}


static void
test_convolutions()
{
	const neural_padding kSame = B_NEURAL_PADDING_SAME;
	const neural_padding kValid = B_NEURAL_PADDING_VALID;
	const neural_activation kNone = B_NEURAL_ACTIVATION_NONE;
	const neural_activation kRelu = B_NEURAL_ACTIVATION_RELU;
	const neural_activation kRelu6 = B_NEURAL_ACTIVATION_RELU6;
	const neural_activation kRelu1 = B_NEURAL_ACTIVATION_RELU_N1_TO_1;

	static const struct {
		const char*	name;
		Convolution	c;
	} kCases[] = {
		// batches, height, width, in, out, kernel, stride, dilation
		{ "conv 3x3 same 8->16 relu",
			{ 1, 9, 11, 8, 16, 3, 3, 1, 1, 1, 1, kSame, kRelu, true } },
		{ "conv 3x3 stride 2 same 3->8 relu6",
			{ 1, 15, 15, 3, 8, 3, 3, 2, 2, 1, 1, kSame, kRelu6, true } },
		{ "conv 3x3 stride 2 same even size",
			{ 1, 16, 12, 4, 4, 3, 3, 2, 2, 1, 1, kSame, kNone, true } },
		{ "conv 3x3 stride 2 valid 5->7",
			{ 1, 16, 13, 5, 7, 3, 3, 2, 2, 1, 1, kValid, kNone, true } },
		{ "conv 5x5 dilation 2 same 4->6",
			{ 1, 12, 12, 4, 6, 5, 5, 1, 1, 2, 2, kSame, kRelu, true } },
		{ "conv 3x3 dilation 3x2 valid",
			{ 1, 14, 10, 6, 5, 3, 3, 1, 1, 3, 2, kValid, kNone, true } },
		{ "conv 1x7 stride 1x2 same",
			{ 1, 8, 17, 4, 12, 1, 7, 1, 2, 1, 1, kSame, kRelu1, true } },
		{ "conv 7x1 stride 2x1 valid no bias",
			{ 1, 17, 8, 4, 12, 7, 1, 2, 1, 1, 1, kValid, kNone, false } },
		{ "conv 3x3 batch 3",
			{ 3, 6, 7, 5, 9, 3, 3, 1, 1, 1, 1, kSame, kRelu, true } },
		{ "conv 3x3 1->1",
			{ 1, 8, 8, 1, 1, 3, 3, 1, 1, 1, 1, kSame, kNone, true } },
		{ "conv 3x3 same 32->48",
			{ 1, 20, 20, 32, 48, 3, 3, 1, 1, 1, 1, kSame, kRelu6, true } },
		{ "conv 1x1 12->20",
			{ 1, 9, 9, 12, 20, 1, 1, 1, 1, 1, 1, kSame, kRelu6, true } },
		{ "conv 1x1 stride 2 valid 6->10",
			{ 1, 9, 10, 6, 10, 1, 1, 2, 2, 1, 1, kValid, kNone, true } },
		{ "conv 1x1 stride 2 same 7->3 batch 2",
			{ 2, 7, 8, 7, 3, 1, 1, 2, 2, 1, 1, kSame, kRelu, false } },
		{ "conv 1x1 96->24",
			{ 1, 14, 14, 96, 24, 1, 1, 1, 1, 1, 1, kSame, kNone, true } },
		{ "conv 1x1 16->1",
			{ 1, 5, 5, 16, 1, 1, 1, 1, 1, 1, 1, kSame, kNone, true } },
		{ "conv 2x2 stride 2 valid",
			{ 1, 8, 8, 4, 8, 2, 2, 2, 2, 1, 1, kValid, kNone, true } },
		{ "conv 4x4 same (uneven padding)",
			{ 1, 9, 9, 3, 5, 4, 4, 1, 1, 1, 1, kSame, kNone, true } },

		{ "depthwise 3x3 same 8",
			{ 1, 9, 11, 8, 8, 3, 3, 1, 1, 1, 1, kSame, kRelu6, true, true } },
		{ "depthwise 3x3 stride 2 same 6",
			{ 1, 15, 14, 6, 6, 3, 3, 2, 2, 1, 1, kSame, kRelu, true, true } },
		{ "depthwise 5x5 dilation 2 valid 5",
			{ 1, 16, 16, 5, 5, 5, 5, 1, 1, 2, 2, kValid, kNone, true,
				true } },
		{ "depthwise 3x3 no bias batch 2",
			{ 2, 7, 7, 4, 4, 3, 3, 1, 1, 1, 1, kSame, kNone, false, true } },
		{ "depthwise 3x3 multiplier 2 (3->6)",
			{ 1, 8, 8, 3, 6, 3, 3, 1, 1, 1, 1, kSame, kNone, true, true } },
		{ "depthwise 3x3 multiplier 3 (4->12)",
			{ 1, 8, 9, 4, 12, 3, 3, 2, 2, 1, 1, kSame, kRelu, true, true } },
		{ "depthwise 3x3 multiplier 8 (1->8)",
			{ 1, 10, 10, 1, 8, 3, 3, 1, 1, 1, 1, kValid, kNone, true,
				true } },
		{ "depthwise 3x3 multiplier 2 (7->14)",
			{ 2, 6, 6, 7, 14, 3, 3, 1, 1, 1, 1, kSame, kRelu6, true,
				true } },
		{ "depthwise 1x1 96",
			{ 1, 7, 7, 96, 96, 1, 1, 1, 1, 1, 1, kSame, kNone, true, true } }
	};

	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++)
		check_convolution(kCases[i].name, kCases[i].c);
}


static void
check_fully_connected(const char* name, const Shape& input,
	const Shape& output, int32 depth, bool keepDimensions, bool bias,
	neural_activation activation)
{
	neural_fully_connected_params params;
	memset(&params, 0, sizeof(params));
	params.activation = activation;
	params.keep_dimensions = keepDimensions;

	int32 units = output.dimensions[output.rank - 1];
	Network network;
	int32 in = network.Input(input);
	int32 weights = network.Constant(Shape(units, depth));
	int32 biases = bias ? network.Constant(Shape(units)) : -1;
	network.Output(network.Operator(B_NEURAL_OP_FULLY_CONNECTED, output, in,
		weights, biases, &params, sizeof(params)));
	check(name, network, 2);
}


static void
test_fully_connected()
{
	check_fully_connected("fully connected 32->10", Shape(1, 32),
		Shape(1, 10), 32, false, true, B_NEURAL_ACTIVATION_NONE);
	check_fully_connected("fully connected 3 rows 17->5 relu", Shape(3, 17),
		Shape(3, 5), 17, false, true, B_NEURAL_ACTIVATION_RELU);
	check_fully_connected("fully connected of a picture (2,3,3,5)",
		Shape(2, 3, 3, 5), Shape(2, 7), 45, false, true,
		B_NEURAL_ACTIVATION_NONE);
	check_fully_connected("fully connected of a picture (1,4,4,8)",
		Shape(1, 4, 4, 8), Shape(1, 12), 128, false, false,
		B_NEURAL_ACTIVATION_RELU6);
	check_fully_connected("fully connected keeping dimensions",
		Shape(2, 5, 9), Shape(2, 5, 6), 9, true, true,
		B_NEURAL_ACTIVATION_NONE);
	check_fully_connected("fully connected 1280->1001",
		Shape(1, 1, 1, 1280), Shape(1, 1001), 1280, false, true,
		B_NEURAL_ACTIVATION_NONE);
	check_fully_connected("fully connected 6->1", Shape(4, 6), Shape(4, 1),
		6, false, true, B_NEURAL_ACTIVATION_NONE);
}


static void
check_pool(const char* name, bool average, const Shape& input,
	int32 filterHeight, int32 filterWidth, int32 strideHeight,
	int32 strideWidth, neural_padding padding,
	neural_activation activation = B_NEURAL_ACTIVATION_NONE)
{
	neural_pool_params params;
	memset(&params, 0, sizeof(params));
	params.padding = padding;
	params.stride_width = strideWidth;
	params.stride_height = strideHeight;
	params.filter_width = filterWidth;
	params.filter_height = filterHeight;
	params.activation = activation;

	Network network;
	int32 in = network.Input(input);
	Shape output(input.dimensions[0],
		output_size(padding, input.dimensions[1], filterHeight, strideHeight,
			1),
		output_size(padding, input.dimensions[2], filterWidth, strideWidth,
			1), input.dimensions[3]);
	network.Output(network.Operator(average ? B_NEURAL_OP_AVERAGE_POOL_2D
			: B_NEURAL_OP_MAX_POOL_2D,
		output, in, -2, -2, &params, sizeof(params)));
	check(name, network, 3);
}


static void
test_pools()
{
	const neural_padding kSame = B_NEURAL_PADDING_SAME;
	const neural_padding kValid = B_NEURAL_PADDING_VALID;

	check_pool("average pool 2x2 stride 2 valid", true, Shape(1, 8, 8, 8),
		2, 2, 2, 2, kValid);
	check_pool("average pool 3x3 same (edges)", true, Shape(1, 7, 9, 5),
		3, 3, 1, 1, kSame);
	check_pool("average pool 3x3 stride 2 same batch 2", true,
		Shape(2, 9, 8, 6), 3, 3, 2, 2, kSame, B_NEURAL_ACTIVATION_RELU);
	check_pool("average pool 7x7 (all of it)", true, Shape(1, 7, 7, 32),
		7, 7, 1, 1, kValid);
	check_pool("average pool 2x3 stride 1x2 same", true, Shape(1, 6, 10, 3),
		2, 3, 1, 2, kSame);
	check_pool("max pool 2x2 stride 2 valid", false, Shape(1, 8, 8, 5),
		2, 2, 2, 2, kValid);
	check_pool("max pool 3x3 stride 2 same", false, Shape(1, 9, 11, 8),
		3, 3, 2, 2, kSame);
	check_pool("max pool 3x3 same relu6 batch 2", false, Shape(2, 5, 5, 3),
		3, 3, 1, 1, kSame, B_NEURAL_ACTIVATION_RELU6);
	check_pool("max pool 5x5 larger than the picture", false,
		Shape(1, 3, 4, 4), 5, 5, 1, 1, kSame);
}


static void
check_binary(const char* name, neural_operator_code code, const Shape& a,
	bool constantA, const Shape& b, bool constantB, const Shape& output,
	neural_activation activation = B_NEURAL_ACTIVATION_NONE)
{
	neural_binary_params params;
	params.activation = activation;

	Network network;
	int32 first = constantA ? network.Constant(a, 2) : network.Input(a);
	int32 second = constantB ? network.Constant(b, 2) : network.Input(b);
	network.Output(network.Operator(code, output, first, second, -2, &params,
		sizeof(params)));
	check(name, network, 4);
}


static void
test_binaries()
{
	static const struct {
		neural_operator_code	code;
		const char*				name;
	} kOperators[] = {
		{ B_NEURAL_OP_ADD, "add" },
		{ B_NEURAL_OP_SUB, "sub" },
		{ B_NEURAL_OP_MUL, "mul" }
	};

	for (int32 i = 0; i < 3; i++) {
		neural_operator_code code = kOperators[i].code;
		BString name;

		name.SetToFormat("%s same shape", kOperators[i].name);
		check_binary(name, code, Shape(1, 6, 7, 8), false, Shape(1, 6, 7, 8),
			false, Shape(1, 6, 7, 8));
		name.SetToFormat("%s same shape 5 channels relu6",
			kOperators[i].name);
		check_binary(name, code, Shape(2, 3, 4, 5), false, Shape(2, 3, 4, 5),
			false, Shape(2, 3, 4, 5), B_NEURAL_ACTIVATION_RELU6);
		name.SetToFormat("%s constant per channel", kOperators[i].name);
		check_binary(name, code, Shape(1, 5, 5, 6), false, Shape(6), true,
			Shape(1, 5, 5, 6));
		name.SetToFormat("%s constant per channel first",
			kOperators[i].name);
		check_binary(name, code, Shape(1, 1, 1, 7), true, Shape(2, 4, 4, 7),
			false, Shape(2, 4, 4, 7), B_NEURAL_ACTIVATION_RELU);
		name.SetToFormat("%s constant scalar", kOperators[i].name);
		check_binary(name, code, Shape(1, 4, 6, 3), false, Shape(1), true,
			Shape(1, 4, 6, 3));
		name.SetToFormat("%s constant scalar first", kOperators[i].name);
		check_binary(name, code, Shape(1, 1, 1, 1), true, Shape(1, 4, 6, 9),
			false, Shape(1, 4, 6, 9), B_NEURAL_ACTIVATION_RELU_N1_TO_1);
		name.SetToFormat("%s input per channel", kOperators[i].name);
		check_binary(name, code, Shape(3, 5, 10), false, Shape(10), false,
			Shape(3, 5, 10));
		name.SetToFormat("%s input scalar", kOperators[i].name);
		check_binary(name, code, Shape(4, 6), false, Shape(1, 1), false,
			Shape(4, 6));
		name.SetToFormat("%s constant picture for a batch",
			kOperators[i].name);
		check_binary(name, code, Shape(3, 4, 4, 5), false, Shape(1, 4, 4, 5),
			true, Shape(3, 4, 4, 5));
		name.SetToFormat("%s vectors", kOperators[i].name);
		check_binary(name, code, Shape(13), false, Shape(13), false,
			Shape(13));
	}
}


static void
test_unaries()
{
	static const struct {
		neural_operator_code	code;
		const char*				name;
		float					scale;
	} kOperators[] = {
		{ B_NEURAL_OP_RELU, "relu", 2 },
		{ B_NEURAL_OP_RELU6, "relu6", 10 },
		{ B_NEURAL_OP_LOGISTIC, "logistic", 8 },
		{ B_NEURAL_OP_TANH, "tanh", 4 },
		{ B_NEURAL_OP_HARD_SWISH, "hard swish", 6 }
	};
	static const Shape kShapes[] = {
		Shape(1, 6, 7, 8), Shape(2, 3, 5, 5), Shape(4, 10), Shape(7)
	};

	for (int32 i = 0; i < 5; i++) {
		for (int32 j = 0; j < 4; j++) {
			BString name;
			name.SetToFormat("%s rank %" B_PRId32, kOperators[i].name,
				kShapes[j].rank);
			if (j == 1)
				name << " 5 channels";

			Network network;
			int32 input = network.Input(kShapes[j]);
			network.Output(network.Operator(kOperators[i].code, kShapes[j],
				input));
			check(name, network, kOperators[i].scale);
		}
	}

	// numbers a tanh() made of exponentials would choke on
	Network network;
	int32 input = network.Input(Shape(1, 8));
	network.Output(network.Operator(B_NEURAL_OP_TANH, Shape(1, 8), input));
	check("tanh large numbers", network, 500);

	Network network2;
	input = network2.Input(Shape(1, 8));
	network2.Output(network2.Operator(B_NEURAL_OP_LOGISTIC, Shape(1, 8),
		input));
	check("logistic large numbers", network2, 500);
}


static void
test_softmax()
{
	static const struct {
		const char*	name;
		Shape		shape;
		float		beta;
		float		scale;
	} kCases[] = {
		{ "softmax 1001", Shape(1, 1001), 1, 6 },
		{ "softmax rows of 10", Shape(4, 10), 1, 3 },
		{ "softmax pictures of 5 channels beta 0.5", Shape(2, 3, 4, 5),
			0.5f, 4 },
		{ "softmax beta 3 large numbers", Shape(3, 21), 3, 60 },
		{ "softmax of one", Shape(5, 1), 1, 1 },
		{ "softmax vector", Shape(8), 1, 2 }
	};

	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		neural_softmax_params params;
		params.beta = kCases[i].beta;

		Network network;
		int32 input = network.Input(kCases[i].shape);
		network.Output(network.Operator(B_NEURAL_OP_SOFTMAX, kCases[i].shape,
			input, -2, -2, &params, sizeof(params)));
		check(kCases[i].name, network, kCases[i].scale);
	}
}


/*!	A new shape is nothing by itself for the device: the networks have
	an operator before and after it. */
static void
check_reshape(const char* name, const Shape& from, const Shape& to,
	bool squeeze = false, bool withShapeInput = false)
{
	Network network;
	int32 input = network.Input(from);
	int32 first = network.Operator(B_NEURAL_OP_RELU, from, input);

	int32 reshaped;
	if (squeeze) {
		neural_squeeze_params params;
		memset(&params, 0, sizeof(params));
		reshaped = network.Operator(B_NEURAL_OP_SQUEEZE, to, first, -2, -2,
			&params, sizeof(params));
	} else {
		neural_reshape_params params;
		memset(&params, 0, sizeof(params));
		params.rank = to.rank;
		memcpy(params.shape, to.dimensions, sizeof(to.dimensions));
		int32 shape = withShapeInput
			? network.Integers(Shape(to.rank), to.dimensions) : -2;
		reshaped = network.Operator(B_NEURAL_OP_RESHAPE, to, first, shape,
			-2, &params, sizeof(params));
	}

	// both the reshaped tensor and something made of it come out
	network.Output(reshaped);
	network.Output(network.Operator(B_NEURAL_OP_TANH, to, reshaped));
	check(name, network, 2);
}


static void
test_reshapes()
{
	check_reshape("reshape (1,1,1,10) -> (1,10)", Shape(1, 1, 1, 10),
		Shape(1, 10));
	check_reshape("reshape (1,4,4,8) -> (1,128)", Shape(1, 4, 4, 8),
		Shape(1, 128), false, true);
	check_reshape("reshape (2,64) -> (2,4,4,4)", Shape(2, 64),
		Shape(2, 4, 4, 4));
	check_reshape("reshape (1,3,3,5) -> (1,45)", Shape(1, 3, 3, 5),
		Shape(1, 45));
	check_reshape("reshape (1,45) -> (3,15)", Shape(1, 45), Shape(3, 15),
		false, true);
	check_reshape("reshape (1,6,6,3) -> (1,12,9)", Shape(1, 6, 6, 3),
		Shape(1, 12, 9));
	check_reshape("reshape (2,8) -> (16)", Shape(2, 8), Shape(16));
	check_reshape("reshape (2,7) -> (14)", Shape(2, 7), Shape(14));
	check_reshape("reshape (1,2,3,4) -> (1,24,1,1)", Shape(1, 2, 3, 4),
		Shape(1, 24, 1, 1));
	check_reshape("squeeze (1,1,1,1001) -> (1,1001)", Shape(1, 1, 1, 1001),
		Shape(1, 1001), true);
	check_reshape("squeeze (1,5,1,6) -> (5,6)", Shape(1, 5, 1, 6),
		Shape(5, 6), true);
	check_reshape("squeeze (3,4,1) -> (3,4)", Shape(3, 4, 1), Shape(3, 4),
		true);
}


static void
check_concatenation(const char* name, int32 rank, const int32* leading,
	const int32* channels, int32 count, int32 axis)
{
	neural_concat_params params;
	params.axis = axis;
	params.activation = B_NEURAL_ACTIVATION_NONE;

	BNeuralModelBuilder builder;
	int32 inputs[8];
	int32 dimensions[4];
	memcpy(dimensions, leading, (rank - 1) * sizeof(int32));
	int32 total = 0;
	for (int32 i = 0; i < count; i++) {
		dimensions[rank - 1] = channels[i];
		total += channels[i];
		// the second one is a constant, the others come in
		BTensorInfo info(B_NEURAL_FLOAT32, rank, dimensions);
		if (i == 1 && count > 2) {
			int64 elements = info.CountElements();
			float* values = (float*)malloc(elements * sizeof(float));
			for (int64 j = 0; j < elements; j++)
				values[j] = random_float();
			inputs[i] = builder.AddConstant(info, values,
				elements * sizeof(float));
			free(values);
		} else
			inputs[i] = builder.AddInput(info);
	}

	dimensions[rank - 1] = total;
	int32 output = builder.AddOperator(B_NEURAL_OP_CONCATENATION, inputs,
		count, BTensorInfo(B_NEURAL_FLOAT32, rank, dimensions), &params,
		sizeof(params));
	builder.MarkOutput(output);

	check(name, builder, 2);
}


static void
test_concatenations()
{
	static const int32 kPicture[] = { 1, 5, 6 };
	static const int32 kBatch[] = { 2, 3, 3 };
	static const int32 kRows[] = { 3 };

	static const int32 kAligned[] = { 4, 8 };
	check_concatenation("concatenation 4+8", 4, kPicture, kAligned, 2, 3);
	static const int32 kUneven[] = { 3, 5 };
	check_concatenation("concatenation 3+5", 4, kPicture, kUneven, 2, -1);
	static const int32 kThree[] = { 1, 2, 3 };
	check_concatenation("concatenation 1+2+3", 4, kBatch, kThree, 3, 3);
	static const int32 kFive[] = { 5, 7, 4, 1, 6 };
	check_concatenation("concatenation 5+7+4+1+6", 4, kBatch, kFive, 5, -1);
	static const int32 kWide[] = { 24, 13, 32 };
	check_concatenation("concatenation 24+13+32", 4, kPicture, kWide, 3, 3);
	static const int32 kPair[] = { 3, 6 };
	check_concatenation("concatenation rows 3+6", 2, kRows, kPair, 2, 1);
	static const int32 kOne[] = { 6 };
	check_concatenation("concatenation of one", 4, kPicture, kOne, 1, 3);
}


static void
check_mean(const char* name, const Shape& input, const int32* axes,
	int32 axisCount, bool keepDimensions)
{
	neural_reduce_params params;
	params.keep_dimensions = keepDimensions;

	Network network;
	int32 in = network.Input(input);
	int32 axesTensor = network.Integers(Shape(axisCount), axes);
	Shape output = keepDimensions
		? Shape(input.dimensions[0], 1, 1, input.dimensions[3])
		: Shape(input.dimensions[0], input.dimensions[3]);
	network.Output(network.Operator(B_NEURAL_OP_MEAN, output, in, axesTensor,
		-2, &params, sizeof(params)));
	check(name, network, 2);
}


static void
test_means()
{
	static const int32 kAxes[] = { 1, 2 };
	static const int32 kSwapped[] = { 2, 1 };
	static const int32 kNegative[] = { -3, -2 };
	static const int32 kTwice[] = { 1, 2, 1 };

	check_mean("mean 7x7x16 keeping dimensions", Shape(1, 7, 7, 16), kAxes,
		2, true);
	check_mean("mean 5x6x5 batch 2", Shape(2, 5, 6, 5), kSwapped, 2, false);
	check_mean("mean negative axes", Shape(1, 4, 9, 3), kNegative, 2, false);
	check_mean("mean an axis named twice", Shape(3, 3, 3, 8), kTwice, 3,
		true);
	check_mean("mean 40x40x4", Shape(1, 40, 40, 4), kAxes, 2, false);
}


static void
check_pad(const char* name, const Shape& input, int32 top, int32 bottom,
	int32 left, int32 right, const float* value)
{
	const int32 paddings[8] = { 0, 0, top, bottom, left, right, 0, 0 };

	Network network;
	int32 in = network.Input(input);
	int32 paddingsTensor = network.Integers(Shape(4, 2), paddings);
	int32 valueTensor = -2;
	if (value != NULL) {
		valueTensor = network.builder.AddConstant(
			Shape(1).Info(), value, sizeof(float));
	}
	Shape output(input.dimensions[0], input.dimensions[1] + top + bottom,
		input.dimensions[2] + left + right, input.dimensions[3]);
	network.Output(network.Operator(B_NEURAL_OP_PAD, output, in,
		paddingsTensor, valueTensor));
	check(name, network, 2);
}


static void
test_pads()
{
	const float kValue = 1.5f;
	check_pad("pad 1,1,2,0", Shape(1, 5, 5, 4), 1, 1, 2, 0, NULL);
	check_pad("pad with a value, 3 channels", Shape(1, 4, 6, 3), 0, 2, 1, 3,
		&kValue);
	check_pad("pad batch 2, 5 channels", Shape(2, 3, 3, 5), 2, 2, 2, 2,
		&kValue);
	check_pad("pad nothing", Shape(1, 3, 3, 8), 0, 0, 0, 0, NULL);
}


static void
check_resize(const char* name, const Shape& input, int32 height, int32 width,
	bool alignCorners, bool halfPixelCenters)
{
	neural_resize_params params;
	params.align_corners = alignCorners;
	params.half_pixel_centers = halfPixelCenters;
	const int32 size[2] = { height, width };

	Network network;
	int32 in = network.Input(input);
	int32 sizeTensor = network.Integers(Shape(2), size);
	network.Output(network.Operator(B_NEURAL_OP_RESIZE_BILINEAR,
		Shape(input.dimensions[0], height, width, input.dimensions[3]), in,
		sizeTensor, -2, &params, sizeof(params)));
	check(name, network, 2);
}


static void
test_resizes()
{
	check_resize("resize 4x4 -> 8x8", Shape(1, 4, 4, 8), 8, 8, false, false);
	check_resize("resize 5x7 -> 9x13 aligned corners", Shape(1, 5, 7, 4), 9,
		13, true, false);
	check_resize("resize 6x6 -> 9x15 half pixel centers", Shape(1, 6, 6, 3),
		9, 15, false, true);
	check_resize("resize 8x8 -> 3x5", Shape(2, 8, 8, 5), 3, 5, false, false);
	check_resize("resize 8x8 -> 3x5 half pixel centers", Shape(1, 8, 8, 4),
		3, 5, false, true);
	check_resize("resize 3x3 -> 1x1 aligned corners", Shape(1, 3, 3, 4), 1,
		1, true, false);
	check_resize("resize 1x1 -> 4x4", Shape(1, 1, 1, 6), 4, 4, false, true);
	check_resize("resize 7x5 -> 7x5", Shape(1, 7, 5, 2), 7, 5, false, false);
}


/*!	Several operators in a row, tensors read by more than one of them,
	two results: what tells whether tensors that share memory are kept
	apart. */
static void
test_networks()
{
	if (!wanted("network"))
		return;

	neural_conv_params conv;
	memset(&conv, 0, sizeof(conv));
	conv.padding = B_NEURAL_PADDING_SAME;
	conv.stride_width = conv.stride_height = 1;
	conv.dilation_width = conv.dilation_height = 1;
	conv.activation = B_NEURAL_ACTIVATION_RELU6;
	neural_binary_params binary;
	binary.activation = B_NEURAL_ACTIVATION_NONE;
	neural_concat_params concat;
	concat.axis = 3;
	concat.activation = B_NEURAL_ACTIVATION_NONE;
	neural_reduce_params reduce;
	reduce.keep_dimensions = false;
	neural_fully_connected_params connected;
	memset(&connected, 0, sizeof(connected));
	neural_softmax_params softmax;
	softmax.beta = 1;
	static const int32 kAxes[] = { 1, 2 };

	{
		// a block with a way around it, as residual networks have many
		Network network;
		Shape picture(1, 12, 12, 6);
		Shape wide(1, 12, 12, 20);
		int32 input = network.Input(picture);
		int32 x = network.Operator(B_NEURAL_OP_CONV_2D, wide, input,
			network.Constant(Shape(20, 1, 1, 6)), network.Constant(Shape(20)),
			&conv, sizeof(conv));
		conv.depth_multiplier = 1;
		x = network.Operator(B_NEURAL_OP_DEPTHWISE_CONV_2D, wide, x,
			network.Constant(Shape(1, 3, 3, 20)), network.Constant(Shape(20)),
			&conv, sizeof(conv));
		conv.activation = B_NEURAL_ACTIVATION_NONE;
		x = network.Operator(B_NEURAL_OP_CONV_2D, picture, x,
			network.Constant(Shape(6, 1, 1, 20)), network.Constant(Shape(6)),
			&conv, sizeof(conv));
		int32 sum = network.Operator(B_NEURAL_OP_ADD, picture, x, input, -2,
			&binary, sizeof(binary));
		int32 both[2] = { sum, input };
		int32 joined = network.builder.AddOperator(B_NEURAL_OP_CONCATENATION,
			both, 2, Shape(1, 12, 12, 12).Info(), &concat, sizeof(concat));
		int32 mean = network.Operator(B_NEURAL_OP_MEAN, Shape(1, 12), joined,
			network.Integers(Shape(2), kAxes), -2, &reduce, sizeof(reduce));
		int32 scores = network.Operator(B_NEURAL_OP_FULLY_CONNECTED,
			Shape(1, 9), mean, network.Constant(Shape(9, 12)),
			network.Constant(Shape(9)), &connected, sizeof(connected));
		network.Output(network.Operator(B_NEURAL_OP_SOFTMAX, Shape(1, 9),
			scores, -2, -2, &softmax, sizeof(softmax)));
		// something from the middle comes out too
		network.Output(sum);
		check("network with a way around", network, 1);
	}

	{
		// many layers one after the other: their tensors take turns in
		// the same memory
		Network network;
		Shape shape(1, 10, 10, 8);
		int32 x = network.Input(shape);
		int32 first = x;
		conv.activation = B_NEURAL_ACTIVATION_RELU_N1_TO_1;
		for (int32 i = 0; i < 12; i++) {
			x = network.Operator(B_NEURAL_OP_CONV_2D, shape, x,
				network.Constant(Shape(8, 3, 3, 8), 0.3f),
				network.Constant(Shape(8)), &conv, sizeof(conv));
			if (i % 4 == 3) {
				x = network.Operator(B_NEURAL_OP_ADD, shape, x, first, -2,
					&binary, sizeof(binary));
			}
		}
		network.Output(x);
		check("network of twelve layers", network, 1);
	}

	{
		// two inputs and two outputs that have nothing to do with each
		// other
		Network network;
		int32 a = network.Input(Shape(1, 8, 8, 3));
		int32 b = network.Input(Shape(2, 5));
		network.Output(network.Operator(B_NEURAL_OP_LOGISTIC,
			Shape(1, 8, 8, 3), a));
		network.Output(network.Operator(B_NEURAL_OP_HARD_SWISH, Shape(2, 5),
			b));
		check("network of two halves", network, 3);
	}
}


/*!	What the back end must say no to. (The kit itself says no to models
	its CPU kernels cannot run, a convolution whose output has the wrong
	size for one: those never get as far as a back end.) */
static void
test_refused()
{
	neural_conv_params conv;
	memset(&conv, 0, sizeof(conv));
	conv.padding = B_NEURAL_PADDING_SAME;
	conv.stride_width = conv.stride_height = 1;
	conv.dilation_width = conv.dilation_height = 1;

	{
		// numbers of 8 bits
		BNeuralModelBuilder builder;
		BTensorInfo info = Shape(1, 4, 4, 4).Info(B_NEURAL_UINT8);
		info.SetQuantization(0.05f, 128);
		int32 input = builder.AddInput(info);
		int32 output = builder.AddOperator(B_NEURAL_OP_RELU, &input, 1, info);
		builder.MarkOutput(output);
		check_refused("refused: quantized", builder);
	}
	{
		// a convolution in groups
		Network network;
		int32 input = network.Input(Shape(1, 6, 6, 8));
		network.Output(network.Operator(B_NEURAL_OP_CONV_2D,
			Shape(1, 6, 6, 8), input, network.Constant(Shape(8, 3, 3, 4)),
			network.Constant(Shape(8)), &conv, sizeof(conv)));
		check_refused("refused: convolution in groups", network);
	}
	{
		neural_concat_params params;
		params.axis = 1;
		params.activation = B_NEURAL_ACTIVATION_NONE;
		Network network;
		int32 inputs[2];
		inputs[0] = network.Input(Shape(1, 4, 4, 4));
		inputs[1] = network.Input(Shape(1, 2, 4, 4));
		network.Output(network.builder.AddOperator(B_NEURAL_OP_CONCATENATION,
			inputs, 2, Shape(1, 6, 4, 4).Info(), &params, sizeof(params)));
		check_refused("refused: concatenation along the height", network);
	}
	{
		neural_reduce_params params;
		params.keep_dimensions = false;
		static const int32 kAxis[] = { 3 };
		Network network;
		int32 input = network.Input(Shape(1, 4, 4, 4));
		network.Output(network.Operator(B_NEURAL_OP_MEAN, Shape(1, 4, 4),
			input, network.Integers(Shape(1), kAxis), -2, &params,
			sizeof(params)));
		check_refused("refused: mean over the channels", network);
	}
	{
		static const int32 kPaddings[8] = { 0, 0, 1, 1, 1, 1, 0, 2 };
		Network network;
		int32 input = network.Input(Shape(1, 4, 4, 4));
		network.Output(network.Operator(B_NEURAL_OP_PAD, Shape(1, 6, 6, 6),
			input, network.Integers(Shape(4, 2), kPaddings)));
		check_refused("refused: padding of the channels", network);
	}
	{
		// a value per column is not the end of the other shape
		neural_binary_params params;
		params.activation = B_NEURAL_ACTIVATION_NONE;
		Network network;
		int32 a = network.Input(Shape(1, 4, 4, 3));
		int32 b = network.Input(Shape(1, 4, 1, 3));
		network.Output(network.Operator(B_NEURAL_OP_ADD, Shape(1, 4, 4, 3),
			a, b, -2, &params, sizeof(params)));
		check_refused("refused: addition repeating in the middle", network);
	}
	{
		neural_binary_params params;
		params.activation = B_NEURAL_ACTIVATION_NONE;
		Network network;
		int32 a = network.Input(Shape(1, 4, 4, 3));
		int32 b = network.Input(Shape(1, 4, 4, 3));
		network.Output(network.Operator(B_NEURAL_OP_MAXIMUM,
			Shape(1, 4, 4, 3), a, b, -2, &params, sizeof(params)));
		check_refused("refused: maximum", network);
	}
	{
		neural_softmax_params params;
		params.beta = -1;
		Network network;
		int32 input = network.Input(Shape(1, 10));
		network.Output(network.Operator(B_NEURAL_OP_SOFTMAX, Shape(1, 10),
			input, -2, -2, &params, sizeof(params)));
		check_refused("refused: softmax with a factor below zero", network);
	}
	{
		// a new shape and nothing else
		neural_reshape_params params;
		memset(&params, 0, sizeof(params));
		params.rank = 2;
		params.shape[0] = 1;
		params.shape[1] = 48;
		Network network;
		int32 input = network.Input(Shape(1, 4, 4, 3));
		network.Output(network.Operator(B_NEURAL_OP_RESHAPE, Shape(1, 48),
			input, -2, -2, &params, sizeof(params)));
		check_refused("refused: nothing but a new shape", network);
	}
}


// #pragma mark - models


static int64
device_memory()
{
	BMessage details;
	sDevice.GetInfo(&details);
	return details.GetInt64("memory used", -1);
}


static int64
team_memory()
{
	int64 total = 0;
	area_info info;
	ssize_t cookie = 0;
	while (get_next_area_info(B_CURRENT_TEAM, &cookie, &info) == B_OK)
		total += info.ram_size;
	return total;
}


static void
fill_inputs(BNeuralSession& session)
{
	uint32 saved = sRandom;
	sRandom = 0x2545f491;
	for (int32 i = 0; i < session.CountInputs(); i++) {
		BTensor* input = session.InputAt(i);
		if (input->Type() == B_NEURAL_FLOAT32) {
			float* values = (float*)input->Data();
			for (int64 j = 0; j < input->CountElements(); j++)
				values[j] = random_float();
		} else {
			for (int64 j = 0; j < input->CountElements(); j++)
				input->SetValueAt(j, random_float());
		}
	}
	sRandom = saved;
}


struct RunsJob {
	BNeuralModel*	model;
	int32			runs;
	int32			number;
	bool			passed;
};


/*!	Runs the model over and over on the same input: the device has no
	reason to give anything but the same every time. */
static status_t
runs_thread(void* data)
{
	RunsJob* job = (RunsJob*)data;
	job->passed = false;

	BNeuralDevice cpu;
	BNeuralRoster::Default()->FindDevice("cpu", &cpu);
	BNeuralSession reference(*job->model, cpu);
	BNeuralSession session(*job->model, sDevice);
	if (reference.InitCheck() != B_OK || session.InitCheck() != B_OK) {
		printf("session %" B_PRId32 ": cannot set up: %s\n", job->number,
			strerror(session.InitCheck()));
		return B_ERROR;
	}

	neural_run_info info;
	session.GetRunInfo(&info);
	if (info.device_operators == 0) {
		printf("session %" B_PRId32 ": nothing runs on the device\n",
			job->number);
		return B_ERROR;
	}

	fill_inputs(reference);
	fill_inputs(session);
	if (reference.Run() != B_OK)
		return B_ERROR;

	int32 outputs = session.CountOutputs();
	void** first = (void**)calloc(outputs, sizeof(void*));
	bigtime_t start = system_time();
	int32 different = 0;

	for (int32 i = 0; i < job->runs; i++) {
		status_t status = session.Run();
		if (status != B_OK) {
			printf("session %" B_PRId32 ": run %" B_PRId32 " failed: %s\n",
				job->number, i, strerror(status));
			return B_ERROR;
		}

		for (int32 j = 0; j < outputs; j++) {
			const BTensor* output = session.OutputAt(j);
			if (i == 0) {
				first[j] = malloc(output->ByteSize());
				memcpy(first[j], output->Data(), output->ByteSize());

				float largest = difference(*output, *reference.OutputAt(j));
				if (largest > kTolerance) {
					printf("session %" B_PRId32 ": output %" B_PRId32
						" differs from the CPU's by %g\n", job->number, j,
						largest);
					return B_ERROR;
				}
			} else if (memcmp(first[j], output->Data(), output->ByteSize())
					!= 0) {
				different++;
			}
		}
	}

	bigtime_t duration = system_time() - start;
	session.GetRunInfo(&info);
	printf("session %" B_PRId32 ": %" B_PRId32 " runs in %.1f ms (%.2f ms "
		"each), %" B_PRId32 " operators on the device, %" B_PRId32
		" results differ from the first\n", job->number, job->runs,
		duration / 1000.0, duration / 1000.0 / job->runs,
		info.device_operators, different);

	for (int32 j = 0; j < outputs; j++)
		free(first[j]);
	free(first);

	job->passed = different == 0;
	return B_OK;
}


static bool
test_runs(BNeuralModel& model, int32 runs, int32 threads)
{
	RunsJob jobs[16];
	thread_id ids[16];
	if (threads > 16)
		threads = 16;

	for (int32 i = 0; i < threads; i++) {
		jobs[i].model = &model;
		jobs[i].runs = runs;
		jobs[i].number = i;
		ids[i] = spawn_thread(&runs_thread, "runs", B_NORMAL_PRIORITY,
			&jobs[i]);
		resume_thread(ids[i]);
	}

	bool passed = true;
	for (int32 i = 0; i < threads; i++) {
		status_t result;
		wait_for_thread(ids[i], &result);
		passed &= jobs[i].passed;
	}
	return passed;
}


/*!	Sessions come and go; what they had on the device and in the team
	must go with them. */
static bool
test_cycles(BNeuralModel& model, int32 cycles)
{
	int64 deviceBase = -1;
	int64 teamBase = 0;
	bool passed = true;

	for (int32 i = 0; i < cycles; i++) {
		int64 inUse;
		{
			BNeuralSession session(model, sDevice);
			if (session.InitCheck() != B_OK) {
				printf("cycle %" B_PRId32 ": cannot set up: %s\n", i,
					strerror(session.InitCheck()));
				return false;
			}
			fill_inputs(session);
			status_t status = session.Run();
			if (status != B_OK) {
				printf("cycle %" B_PRId32 ": run failed: %s\n", i,
					strerror(status));
				return false;
			}
			inUse = device_memory();
		}

		int64 after = device_memory();
		int64 team = team_memory();
		// the first cycles load the driver and compile the shaders
		if (i == 9) {
			deviceBase = after;
			teamBase = team;
		}
		if (i == 0 || i == 9 || (i + 1) % 50 == 0 || i == cycles - 1) {
			printf("cycle %4" B_PRId32 ": device memory %" B_PRId64
				" KiB with the session, %" B_PRId64 " KiB without; team %"
				B_PRId64 " KiB\n", i + 1, inUse / 1024, after / 1024,
				team / 1024);
		}
		if (i > 9 && after != deviceBase) {
			printf("cycle %" B_PRId32 ": device memory in use changed\n",
				i + 1);
			passed = false;
		}
		if (i > 9 && team > teamBase + 16 * 1024 * 1024) {
			printf("cycle %" B_PRId32 ": the team grew by %" B_PRId64
				" KiB\n", i + 1, (team - teamBase) / 1024);
			return false;
		}
	}

	return passed && deviceBase == 0;
}


int
main(int argc, char** argv)
{
	const char* deviceID = "vulkan/0";
	const char* modelPath = NULL;
	int32 runs = 0;
	int32 threads = 0;
	int32 cycles = 0;

	for (int i = 1; i < argc; i++) {
		const char* value = i + 1 < argc ? argv[i + 1] : NULL;
		if (strcmp(argv[i], "-v") == 0)
			sVerbose = true;
		else if (strcmp(argv[i], "-d") == 0 && value != NULL) {
			deviceID = value;
			i++;
		} else if (strcmp(argv[i], "--runs") == 0 && value != NULL) {
			runs = atoi(value);
			i++;
		} else if (strcmp(argv[i], "--threads") == 0 && value != NULL) {
			threads = atoi(value);
			i++;
		} else if (strcmp(argv[i], "--cycles") == 0 && value != NULL) {
			cycles = atoi(value);
			i++;
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "usage: vulkan_backend_test [-d <device>] [-v] "
				"[<filter>]\n"
				"       vulkan_backend_test [-d <device>] --runs <n> "
				"[--threads <n>] <model>\n"
				"       vulkan_backend_test [-d <device>] --cycles <n> "
				"<model>\n");
			return 1;
		} else if (runs > 0 || threads > 0 || cycles > 0)
			modelPath = argv[i];
		else
			sFilter = argv[i];
	}

	if (BNeuralRoster::Default()->FindDevice(deviceID, &sDevice) != B_OK) {
		printf("no device %s\n", deviceID);
		return 1;
	}
	printf("%s: %s\n", sDevice.ID(), sDevice.Name());

	if (modelPath != NULL) {
		BNeuralModel model(modelPath);
		if (model.InitCheck() != B_OK) {
			printf("%s: %s\n", modelPath, strerror(model.InitCheck()));
			return 1;
		}

		bool passed = true;
		if (cycles > 0)
			passed &= test_cycles(model, cycles);
		if (runs > 0 || threads > 0) {
			passed &= test_runs(model, runs > 0 ? runs : 100,
				threads > 0 ? threads : 1);
		}
		printf("%s\n", passed ? "passed" : "FAILED");
		return passed ? 0 : 1;
	}

	test_convolutions();
	test_fully_connected();
	test_pools();
	test_binaries();
	test_unaries();
	test_softmax();
	test_reshapes();
	test_concatenations();
	test_means();
	test_pads();
	test_resizes();
	test_networks();
	test_refused();

	printf("%" B_PRId32 " passed, %" B_PRId32 " failed\n", sPassed, sFailed);
	return sFailed == 0 && sPassed > 0 ? 0 : 1;
}
