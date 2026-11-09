/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Checks the rk3588_npu back end against the kit's CPU interpreter:
	networks of a single convolution (with an addition after it, for
	some), made with BNeuralModelBuilder from random weights, are run on
	random inputs on the processor and on the CPU, and the outputs
	compared.

		rk3588_npu_backend_test [-v] [--tolerance <n>] [--case <spec>]...

	Without --case the built-in list is run: the shapes the back end says
	it supports, and some it has to refuse. A case is a list of
	name=value pairs separated by commas, for trying things out:

		w, h      input size (default 16)
		ic, oc    channels (default 16)
		k         kernel size (default 1)
		s         stride (default 1)
		same      1: "same" padding, 0: "valid" (default 1)
		dw        1: depthwise
		add       1: an addition follows, 2: with the operands swapped
		int8      1: int8 tensors, 2: and weights with a scale per channel
		          (all the same), 3: (all different)
		act       0 none, 1 ReLU, 2 ReLU6, 3 -1 to 1 (the output range is
		          made to fit unless ozp is given)
		izp, wzp, ozp  zero points (default: random)
		bias      0: no bias
		seed      for the random numbers
		refuse    1: the case passes if the processor does not take it
		wide      1: an output range so wide that no sum is beyond it

	Exits with 0 when every case passed. The largest difference accepted
	is one step of the output (--tolerance): the processor rounds its
	sums to outputs a little differently than the interpreter does.

		rk3588_npu_backend_test [-v] --model <file> [--input <file>]

	checks the operators of a model one by one instead: the model is run
	on the CPU, and then every operator the processor takes is run on it
	alone, on the very numbers the CPU had at that point (noise by
	default, or a picture, or a file of raw input bytes). This tells
	which operators the processor computes differently, however small the
	difference is at the end of the model. An addition is run together
	with the convolution it becomes part of.

		rk3588_npu_backend_test --model <file> --repeat <n> [--input <file>]

	runs the whole model n times on the processor with the same input
	and checks that the outputs are the same every time.
*/


#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include <Application.h>
#include <Bitmap.h>
#include <File.h>
#include <NeuralKit.h>
#include <String.h>
#include <TranslationUtils.h>

#include <NeuralRosterPrivate.h>


struct Case {
	int32	w, h, ic, oc, k, s;
	int32	same, dw, add, int8, act, bias, seed, refuse, wide;
	int32	izp, wzp, ozp;
		// -1000: random
};


static bool sVerbose;
static int32 sTolerance = 1;
static uint32 sRandom;


static uint32
next_random()
{
	sRandom = sRandom * 1664525 + 1013904223;
	return sRandom >> 8;
}


static int32
random_between(int32 low, int32 high)
{
	return low + (int32)(next_random() % (uint32)(high - low + 1));
}


static void
set_defaults(Case& test)
{
	memset(&test, 0, sizeof(test));
	test.w = test.h = 16;
	test.ic = test.oc = 16;
	test.k = 1;
	test.s = 1;
	test.same = 1;
	test.bias = 1;
	test.seed = 1;
	test.izp = test.wzp = test.ozp = -1000;
}


static bool
parse_case(const char* spec, Case& test)
{
	set_defaults(test);

	char buffer[256];
	strlcpy(buffer, spec, sizeof(buffer));

	bool sawOutputChannels = false;
	char* state;
	for (char* pair = strtok_r(buffer, ",", &state); pair != NULL;
			pair = strtok_r(NULL, ",", &state)) {
		char* equals = strchr(pair, '=');
		if (equals == NULL)
			return false;
		*equals = '\0';
		int32 value = atoi(equals + 1);

		struct {
			const char*	name;
			int32*		field;
		} fields[] = {
			{ "w", &test.w }, { "h", &test.h }, { "ic", &test.ic },
			{ "oc", &test.oc }, { "k", &test.k }, { "s", &test.s },
			{ "same", &test.same }, { "dw", &test.dw }, { "add", &test.add },
			{ "int8", &test.int8 }, { "act", &test.act },
			{ "bias", &test.bias }, { "seed", &test.seed },
			{ "refuse", &test.refuse }, { "wide", &test.wide },
			{ "izp", &test.izp },
			{ "wzp", &test.wzp }, { "ozp", &test.ozp }
		};

		bool found = false;
		for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
			if (strcmp(pair, fields[i].name) == 0) {
				*fields[i].field = value;
				found = true;
			}
		}
		if (!found)
			return false;
		if (strcmp(pair, "oc") == 0)
			sawOutputChannels = true;
	}

	if (test.dw != 0 && !sawOutputChannels)
		test.oc = test.ic;
	return test.w > 0 && test.h > 0 && test.ic > 0 && test.oc > 0
		&& test.k > 0 && test.s > 0;
}


static BString
case_name(const Case& test)
{
	BString name;
	name.SetToFormat("%s%" B_PRId32 "x%" B_PRId32 " stride %" B_PRId32
		" %s, %" B_PRId32 "x%" B_PRId32 "x%" B_PRId32 " -> %" B_PRId32
		" channels, %s%s%s%s%s", test.dw != 0 ? "depthwise " : "", test.k,
		test.k, test.s, test.same != 0 ? "same" : "valid", test.w, test.h,
		test.ic, test.oc,
		test.int8 == 0 ? "uint8" : test.int8 == 1 ? "int8"
			: test.int8 == 2 ? "int8 per channel (equal)"
			: "int8 per channel",
		test.add == 1 ? ", addition" : test.add == 2
			? ", addition (swapped)" : "",
		test.act == 1 ? ", ReLU" : test.act == 2 ? ", ReLU6"
			: test.act == 3 ? ", -1 to 1" : "",
		test.bias == 0 ? ", no bias" : "",
		test.refuse != 0 ? " (to be refused)" : "");
	return name;
}


static BTensorInfo
tensor_info(neural_data_type type, int32 n, int32 h, int32 w, int32 c,
	const char* name, float scale, int32 zeroPoint)
{
	int32 dimensions[4] = { n, h, w, c };
	BTensorInfo info(type, 4, dimensions, name);
	info.SetQuantization(scale, zeroPoint);
	return info;
}


/*!	Makes the model of a case. The scales are chosen so that the outputs
	use the whole range of a byte, with some of them beyond it. */
static status_t
build_model(const Case& test, BNeuralModel& model)
{
	sRandom = (uint32)test.seed * 2654435761u + 12345;

	bool isSigned = test.int8 != 0;
	neural_data_type type = isSigned ? B_NEURAL_INT8 : B_NEURAL_UINT8;
	int32 minimum = isSigned ? -128 : 0;
	int32 maximum = isSigned ? 127 : 255;

	int32 outputWidth;
	int32 outputHeight;
	if (test.same != 0) {
		outputWidth = (test.w + test.s - 1) / test.s;
		outputHeight = (test.h + test.s - 1) / test.s;
	} else {
		outputWidth = (test.w - test.k) / test.s + 1;
		outputHeight = (test.h - test.k) / test.s + 1;
	}
	if (outputWidth < 1 || outputHeight < 1)
		return B_BAD_VALUE;

	// (without a bias to make up for it, the zero points are where random
	// numbers have theirs)
	int32 inputZeroPoint = test.izp != -1000 ? test.izp
		: test.bias == 0 ? (isSigned ? 0 : 128)
		: random_between(minimum, maximum);
	// int8 weights have a zero point of 0 by convention
	int32 weightsZeroPoint = test.wzp != -1000 ? test.wzp
		: isSigned ? 0 : test.bias == 0 ? 128 : random_between(64, 192);

	float inputScale = 0.02f + (next_random() % 1000) * 0.00002f;
	float weightsScale = 0.002f + (next_random() % 1000) * 0.00001f;

	// How large the sums get: every term is about 74 * 74 (two numbers
	// spread evenly over a byte), and there are this many of them.
	int32 terms = test.k * test.k * (test.dw != 0 ? 1 : test.ic);
	double deviation = sqrt((double)terms) * 74.0 * 74.0;
	float outputScale = (float)(inputScale * weightsScale
		* (test.wide != 0 ? 8.0 : 3.0) * deviation / 127.0);

	int32 outputZeroPoint = test.ozp != -1000 ? test.ozp
		: random_between(minimum + 96, maximum - 96);
	if (test.act != 0) {
		// Most networks have the range of the output end where the
		// activation's does: nothing below 0, and for ReLU6 nothing above
		// 6. With a zero point given, the range is wider than that and
		// the activation has to be computed.
		if (test.ozp == -1000)
			outputZeroPoint = minimum;
		if (test.act >= 2) {
			outputScale = (test.act == 3 ? 4.0f : test.ozp == -1000 ? 6.0f
				: 10.0f) / 255.0f;
			weightsScale = (float)(outputScale * 127.0
				/ (3.0 * deviation * inputScale));
		}
	}

	BNeuralModelBuilder builder("rk3588_npu_backend_test");

	int32 input = builder.AddInput(tensor_info(type, 1, test.h, test.w,
		test.ic, "input", inputScale, inputZeroPoint));

	// weights
	int32 weightsCount = test.dw != 0 ? test.k * test.k * test.oc
		: test.oc * test.k * test.k * test.ic;
	uint8* weightsData = (uint8*)malloc(weightsCount);
	if (weightsData == NULL)
		return B_NO_MEMORY;
	for (int32 i = 0; i < weightsCount; i++) {
		weightsData[i] = (uint8)(isSigned ? random_between(-127, 127)
			: random_between(0, 255));
	}

	int32 weights = builder.AddConstant(
		test.dw != 0
			? tensor_info(type, 1, test.k, test.k, test.oc, "weights",
				weightsScale, weightsZeroPoint)
			: tensor_info(type, test.oc, test.k, test.k, test.ic, "weights",
				weightsScale, weightsZeroPoint),
		weightsData, weightsCount);
	free(weightsData);

	if (test.int8 >= 2) {
		float* scales = (float*)malloc(test.oc * sizeof(float));
		if (scales == NULL)
			return B_NO_MEMORY;
		for (int32 i = 0; i < test.oc; i++) {
			scales[i] = test.int8 == 2 ? weightsScale
				: weightsScale * (1.0f + (i % 7) * 0.1f);
		}
		builder.SetChannelQuantization(weights, scales, test.oc,
			test.dw != 0 ? 3 : 0);
		free(scales);
	}

	// bias, in units of the sums
	int32 bias = -1;
	if (test.bias != 0) {
		int32* biasData = (int32*)malloc(test.oc * sizeof(int32));
		if (biasData == NULL)
			return B_NO_MEMORY;
		// The zero points are anywhere, so the sums of random numbers
		// are far from zero on average; the bias takes that away, or
		// most outputs would be at an end of the range.
		double mean = terms * ((minimum + maximum) / 2.0 - inputZeroPoint)
			* ((isSigned ? 0 : 127.5) - weightsZeroPoint);
		for (int32 i = 0; i < test.oc; i++) {
			biasData[i] = (int32)-mean + random_between(-(int32)deviation,
				(int32)deviation);
		}
		int32 dimensions[1] = { test.oc };
		BTensorInfo info(B_NEURAL_INT32, 1, dimensions, "bias");
		info.SetQuantization(inputScale * weightsScale, 0);
		bias = builder.AddConstant(info, biasData,
			test.oc * sizeof(int32));
		free(biasData);
	}

	neural_conv_params params;
	memset(&params, 0, sizeof(params));
	params.padding = test.same != 0 ? B_NEURAL_PADDING_SAME
		: B_NEURAL_PADDING_VALID;
	params.stride_width = params.stride_height = test.s;
	params.dilation_width = params.dilation_height = 1;
	params.depth_multiplier = 1;
	params.activation = test.act == 1 ? B_NEURAL_ACTIVATION_RELU
		: test.act == 2 ? B_NEURAL_ACTIVATION_RELU6
		: test.act == 3 ? B_NEURAL_ACTIVATION_RELU_N1_TO_1
		: B_NEURAL_ACTIVATION_NONE;

	int32 inputs[3] = { input, weights, bias };
	int32 output = builder.AddOperator(
		test.dw != 0 ? B_NEURAL_OP_DEPTHWISE_CONV_2D : B_NEURAL_OP_CONV_2D,
		inputs, 3, tensor_info(type, 1, outputHeight, outputWidth, test.oc,
			test.add != 0 ? "sum" : "output", outputScale, outputZeroPoint),
		&params, sizeof(params));

	if (test.add != 0) {
		float addendScale = outputScale * (0.6f + (next_random() % 100)
			* 0.008f);
		int32 addend = builder.AddInput(tensor_info(type, 1, outputHeight,
			outputWidth, test.oc, "addend", addendScale,
			random_between(minimum + 96, maximum - 96)));

		neural_binary_params addParams;
		memset(&addParams, 0, sizeof(addParams));
		addParams.activation = B_NEURAL_ACTIVATION_NONE;

		int32 addInputs[2] = { output, addend };
		if (test.add == 2) {
			addInputs[0] = addend;
			addInputs[1] = output;
		}
		output = builder.AddOperator(B_NEURAL_OP_ADD, addInputs, 2,
			tensor_info(type, 1, outputHeight, outputWidth, test.oc,
				"output", (outputScale + addendScale) * 0.75f,
				random_between(minimum + 96, maximum - 96)),
			&addParams, sizeof(addParams));
	}

	builder.MarkOutput(output);
	return builder.Build(&model);
}


/*!	Runs a case; returns whether it passed. */
static bool
run_case(const Case& test, const BNeuralDevice& device,
	const BNeuralDevice& cpu)
{
	BString name = case_name(test);

	BNeuralModel model;
	status_t status = build_model(test, model);
	if (status != B_OK) {
		printf("FAILED: %s: cannot make the model: %s %s\n", name.String(),
			strerror(status), model.ErrorDetails());
		return false;
	}

	BNeuralSession deviceSession(model, device, 0);
	BNeuralSession cpuSession(model, cpu, 0);
	if (deviceSession.InitCheck() != B_OK || cpuSession.InitCheck() != B_OK) {
		printf("FAILED: %s: no session: %s\n", name.String(),
			strerror(deviceSession.InitCheck() != B_OK
				? deviceSession.InitCheck() : cpuSession.InitCheck()));
		return false;
	}

	neural_run_info info;
	deviceSession.GetRunInfo(&info);
	bool taken = info.cpu_operators == 0 && info.device_operators > 0;
	if (test.refuse != 0) {
		printf("%s: %s%s\n", taken ? "FAILED" : "ok", name.String(),
			taken ? ": the processor took it" : "");
		return !taken;
	}
	if (!taken) {
		printf("FAILED: %s: the processor runs %" B_PRId32 " of %" B_PRId32
			" operators\n", name.String(), info.device_operators,
			info.device_operators + info.cpu_operators);
		return false;
	}

	int64 different = 0;
	int64 total = 0;
	int32 largest = 0;
	int64 saturated = 0;
	int64 signedSum = 0;

	// two inputs: random numbers, and the same again with other numbers
	for (int32 round = 0; round < 2; round++) {
		for (int32 i = 0; i < deviceSession.CountInputs(); i++) {
			BTensor* tensor = deviceSession.InputAt(i);
			uint8* data = (uint8*)tensor->Data();
			for (size_t j = 0; j < tensor->ByteSize(); j++)
				data[j] = (uint8)next_random();
			cpuSession.InputAt(i)->CopyFrom(*tensor);
		}

		status = deviceSession.Run();
		if (status == B_OK)
			status = cpuSession.Run();
		if (status != B_OK) {
			printf("FAILED: %s: run: %s\n", name.String(), strerror(status));
			return false;
		}

		const BTensor* a = deviceSession.OutputAt(0);
		const BTensor* b = cpuSession.OutputAt(0);
		const uint8* x = (const uint8*)a->Data();
		const uint8* y = (const uint8*)b->Data();
		bool isSigned = a->Type() == B_NEURAL_INT8;

		for (size_t j = 0; j < a->ByteSize(); j++) {
			int32 p = isSigned ? (int32)(int8)x[j] : (int32)x[j];
			int32 q = isSigned ? (int32)(int8)y[j] : (int32)y[j];
			int32 difference = abs(p - q);
			signedSum += p - q;
			if (difference != 0)
				different++;
			if (difference > largest)
				largest = difference;
			if (q == (isSigned ? -128 : 0) || q == (isSigned ? 127 : 255))
				saturated++;

			if (sVerbose && difference > sTolerance && different < 20) {
				printf("    [%" B_PRIuSIZE "] processor %" B_PRId32 ", CPU %"
					B_PRId32 "\n", j, p, q);
			}
		}
		total += a->ByteSize();
	}

	bool ok = largest <= sTolerance;
	printf("%s: %s: %" B_PRId64 " of %" B_PRId64 " differ, largest "
		"difference %" B_PRId32, ok ? "ok" : "FAILED", name.String(),
		different, total, largest);
	if (sVerbose) {
		printf(" (%" B_PRId64 " at the ends of the range, mean %.4f)",
			saturated, (double)signedSum / total);
	}
	printf("\n");
	return ok;
}


static const char* const kCases[] = {
	// 1x1
	"w=16,h=16,ic=16,oc=16,k=1",
	"w=56,h=56,ic=24,oc=144,k=1",
	"w=14,h=14,ic=384,oc=64,k=1",
	"w=7,h=7,ic=320,oc=1280,k=1",
	"w=1,h=1,ic=1024,oc=1001,k=1",
	"w=19,h=19,ic=384,oc=273,k=1",
	"w=75,h=75,ic=48,oc=96,k=1",
	"w=17,h=17,ic=176,oc=48,k=1",
	// 3x3, with and without padding, both strides
	"w=224,h=224,ic=3,oc=32,k=3,s=2",
	"w=32,h=32,ic=16,oc=32,k=3,s=1",
	"w=32,h=32,ic=16,oc=32,k=3,s=1,same=0",
	"w=32,h=32,ic=16,oc=32,k=3,s=2,same=0",
	"w=10,h=10,ic=192,oc=384,k=3,s=2",
	"w=3,h=3,ic=96,oc=192,k=3,s=2",
	"w=33,h=20,ic=36,oc=20,k=3",
	// other kernels
	"w=20,h=20,ic=32,oc=32,k=2,s=2,same=0",
	"w=20,h=20,ic=32,oc=32,k=5,s=1",
	"w=224,h=224,ic=3,oc=32,k=7,s=2",
	// depthwise
	"w=112,h=112,ic=32,k=3,s=1,dw=1",
	"w=112,h=112,ic=96,k=3,s=2,dw=1",
	"w=14,h=14,ic=576,k=3,s=1,dw=1,same=0",
	"w=7,h=7,ic=1024,k=3,s=1,dw=1",
	"w=150,h=150,ic=48,k=3,s=2,dw=1",
	"w=20,h=20,ic=24,k=5,dw=1",
	// inputs and weights that do not fit the convolution buffer at once
	"w=300,h=300,ic=3,oc=24,k=3,s=2",
	"w=160,h=160,ic=64,oc=32,k=1",
	"w=40,h=40,ic=512,oc=512,k=3",
	"w=10,h=10,ic=1024,oc=512,k=3",
	"w=100,h=100,ic=17,oc=1000,k=3",
	// activations: as the range of the output, and computed
	"w=28,h=28,ic=32,oc=64,k=1,act=1",
	"w=28,h=28,ic=32,oc=64,k=3,act=2",
	"w=28,h=28,ic=32,oc=64,k=1,act=1,ozp=100",
	"w=28,h=28,ic=32,oc=64,k=3,act=2,ozp=60",
	"w=28,h=28,ic=32,oc=64,k=3,act=3,ozp=128",
	"w=28,h=28,ic=64,k=3,dw=1,act=2,ozp=60",
	// without bias
	"w=28,h=28,ic=32,oc=64,k=3,bias=0",
	// additions
	"w=56,h=56,ic=144,oc=24,k=1,add=1",
	"w=14,h=14,ic=384,oc=64,k=1,add=2",
	"w=7,h=7,ic=960,oc=160,k=1,add=1",
	"w=16,h=16,ic=32,oc=64,k=3,s=2,add=1",
	"w=200,h=200,ic=16,oc=16,k=3,add=1",
	"w=3,h=3,ic=32,oc=100,k=1,add=1",
	"w=28,h=28,ic=32,oc=32,k=1,add=1,act=2,ozp=60",
	// int8
	"w=16,h=16,ic=16,oc=16,k=1,int8=1",
	"w=32,h=32,ic=16,oc=32,k=3,s=2,int8=1",
	"w=28,h=28,ic=64,k=3,dw=1,int8=1",
	"w=28,h=28,ic=32,oc=64,k=1,int8=2",
	"w=28,h=28,ic=32,oc=32,k=1,int8=1,add=1",
	"w=20,h=20,ic=32,oc=32,k=3,int8=1,act=2,ozp=-60",
	// what the processor cannot do and has to leave to the CPU
	"w=28,h=28,ic=32,oc=64,k=1,int8=3,refuse=1",
	"w=28,h=28,ic=1,oc=16,k=3,refuse=1",
	"w=16,h=16,ic=64,k=3,dw=1,add=1,refuse=1",
	"w=24,h=24,ic=1000,oc=40,k=5,refuse=1",
};


// #pragma mark - the operators of a model


/*!	A back end that runs nothing: it is how this program gets to see the
	graph of a model, which only back ends are shown. */
class SpyBackend : public BNeuralBackend {
public:
	SpyBackend()
		:
		BNeuralBackend(-1),
		fGraph(NULL)
	{
	}

	virtual const char* Name() const { return "spy"; }

	virtual int32 CountDevices() { return 1; }

	virtual status_t GetDeviceInfo(int32 device, neural_device_info* info,
		BMessage* details)
	{
		if (device != 0)
			return B_BAD_INDEX;
		strlcpy(info->id, "0", sizeof(info->id));
		strlcpy(info->name, "Spy", sizeof(info->name));
		strlcpy(info->vendor, "air/OS", sizeof(info->vendor));
		info->kind = B_NEURAL_DEVICE_GPU;
		info->flags = B_NEURAL_DEVICE_QUANTIZED | B_NEURAL_DEVICE_FLOAT;
		info->priority = 0;
		return B_OK;
	}

	virtual status_t GetSupportedOperators(int32 device,
		const BNeuralGraph& graph, bool* supported)
	{
		fGraph = &graph;
		for (int32 i = 0; i < graph.CountOperators(); i++)
			supported[i] = false;
		return B_OK;
	}

	virtual status_t CreateProgram(int32 device, const BNeuralGraph& graph,
		const neural_partition& partition, uint32 flags,
		BNeuralProgram** _program)
	{
		return B_NOT_SUPPORTED;
	}

	const BNeuralGraph*	fGraph;
};


/*!	Some operators of a graph as a model of their own. */
struct SubModel {
	BNeuralModel	model;
	int32*			inputs;
		// the graph's tensors that are its inputs and outputs, in order
	int32			input_count;
	int32*			outputs;
	int32			output_count;

	SubModel()
		:
		inputs(NULL),
		input_count(0),
		outputs(NULL),
		output_count(0)
	{
	}

	~SubModel()
	{
		free(inputs);
		free(outputs);
	}
};


static BTensorInfo
tensor_info(const neural_graph_tensor* tensor)
{
	BTensorInfo info(tensor->type, tensor->rank, tensor->dimensions,
		tensor->name);
	if (tensor->quantized)
		info.SetQuantization(tensor->scale, tensor->zero_point);
	return info;
}


/*!	Makes a model of the given operators of a graph. What they read and
	do not make themselves becomes an input; \a output is the tensor that
	is to come out, or -1 for everything the operators make. */
static status_t
build_sub_model(const BNeuralGraph& graph, const int32* operators,
	int32 operatorCount, int32 output, SubModel& sub)
{
	int32 tensorCount = graph.CountTensors();
	int32* newIndex = (int32*)malloc(tensorCount * sizeof(int32));
	sub.inputs = (int32*)malloc(tensorCount * sizeof(int32));
	sub.outputs = (int32*)malloc(tensorCount * sizeof(int32));
	if (newIndex == NULL || sub.inputs == NULL || sub.outputs == NULL) {
		free(newIndex);
		return B_NO_MEMORY;
	}
	for (int32 i = 0; i < tensorCount; i++)
		newIndex[i] = -1;

	BNeuralModelBuilder builder(graph.Name());

	for (int32 i = 0; i < operatorCount; i++) {
		const neural_graph_operator* op = graph.OperatorAt(operators[i]);

		int32 inputs[16];
		BTensorInfo outputs[8];
		if (op->input_count > 16 || op->output_count > 8) {
			free(newIndex);
			return B_NOT_SUPPORTED;
		}

		for (int32 j = 0; j < op->input_count; j++) {
			int32 index = op->inputs[j];
			if (index >= 0 && newIndex[index] < 0) {
				const neural_graph_tensor* tensor = graph.TensorAt(index);
				if (tensor->data != NULL) {
					newIndex[index] = builder.AddConstant(tensor_info(tensor),
						tensor->data, tensor->size);
					if (tensor->channel_count > 0) {
						builder.SetChannelQuantization(newIndex[index],
							tensor->channel_scales, tensor->channel_count,
							tensor->channel_axis);
					}
				} else {
					newIndex[index] = builder.AddInput(tensor_info(tensor));
					sub.inputs[sub.input_count++] = index;
				}
			}
			inputs[j] = index >= 0 ? newIndex[index] : -1;
		}

		for (int32 j = 0; j < op->output_count; j++)
			outputs[j] = tensor_info(graph.TensorAt(op->outputs[j]));

		int32 first = builder.AddOperator(op->code, inputs, op->input_count,
			outputs, op->output_count, op->params,
			neural_operator_params_size(op->code));
		if (first < 0) {
			free(newIndex);
			return first;
		}

		for (int32 j = 0; j < op->output_count; j++) {
			newIndex[op->outputs[j]] = first + j;
			if (output < 0 || output == op->outputs[j]) {
				builder.MarkOutput(first + j);
				sub.outputs[sub.output_count++] = op->outputs[j];
			}
		}
	}

	free(newIndex);
	return builder.Build(&sub.model);
}


static status_t
fill_model_inputs(BNeuralSession& session, const char* path)
{
	if (path != NULL) {
		BTensor* first = session.InputAt(0);
		BBitmap* bitmap = BTranslationUtils::GetBitmap(path);
		if (bitmap != NULL) {
			status_t status = BNeuralUtils::SetTensorFromBitmap(first,
				bitmap);
			delete bitmap;
			return status;
		}

		BFile file(path, B_READ_ONLY);
		status_t status = file.InitCheck();
		for (int32 i = 0; status == B_OK && i < session.CountInputs(); i++) {
			BTensor* tensor = session.InputAt(i);
			if (file.Read(tensor->Data(), tensor->ByteSize())
					!= (ssize_t)tensor->ByteSize()) {
				status = B_BAD_DATA;
			}
		}
		return status;
	}

	sRandom = 0x2545f491;
	for (int32 i = 0; i < session.CountInputs(); i++) {
		BTensor* tensor = session.InputAt(i);
		uint8* data = (uint8*)tensor->Data();
		for (size_t j = 0; j < tensor->ByteSize(); j++)
			data[j] = (uint8)next_random();
	}
	return B_OK;
}


static int
check_model(const char* path, const char* inputPath,
	const BNeuralDevice& device, const BNeuralDevice& cpu)
{
	BNeuralModel model(path);
	if (model.InitCheck() != B_OK) {
		printf("FAILED: %s: %s %s\n", path, strerror(model.InitCheck()),
			model.ErrorDetails());
		return 1;
	}

	// the graph, by way of a back end of our own
	SpyBackend* spy = new(std::nothrow) SpyBackend();
	BNeuralDevice spyDevice;
	if (spy == NULL
		|| BPrivate::NeuralRosterData::Default()->AddBackend(spy) != B_OK
		|| BNeuralRoster::Default()->FindDevice("spy/0", &spyDevice) != B_OK
		|| spyDevice.CountSupportedOperators(model) < 0
		|| spy->fGraph == NULL) {
		printf("FAILED: cannot get at the graph of %s\n", path);
		return 1;
	}
	const BNeuralGraph& graph = *spy->fGraph;
	int32 operatorCount = graph.CountOperators();
	int32 tensorCount = graph.CountTensors();

	// everything the model computes, on the CPU
	int32* all = (int32*)malloc(operatorCount * sizeof(int32));
	const uint8** reference = (const uint8**)calloc(tensorCount,
		sizeof(uint8*));
	int32* producer = (int32*)malloc(tensorCount * sizeof(int32));
	int32* consumers = (int32*)calloc(tensorCount, sizeof(int32));
	if (all == NULL || reference == NULL || producer == NULL
		|| consumers == NULL) {
		return 1;
	}
	for (int32 i = 0; i < operatorCount; i++)
		all[i] = i;

	SubModel whole;
	status_t status = build_sub_model(graph, all, operatorCount, -1, whole);
	BNeuralSession wholeSession;
	if (status == B_OK)
		status = wholeSession.SetTo(whole.model, cpu, 0);
	if (status == B_OK)
		status = fill_model_inputs(wholeSession, inputPath);
	if (status == B_OK)
		status = wholeSession.Run();
	if (status != B_OK) {
		printf("FAILED: cannot run %s on the CPU: %s %s\n", path,
			strerror(status), whole.model.ErrorDetails());
		return 1;
	}

	for (int32 i = 0; i < whole.input_count; i++) {
		reference[whole.inputs[i]]
			= (const uint8*)wholeSession.InputAt(i)->Data();
	}
	for (int32 i = 0; i < whole.output_count; i++) {
		reference[whole.outputs[i]]
			= (const uint8*)wholeSession.OutputAt(i)->Data();
	}

	for (int32 i = 0; i < tensorCount; i++)
		producer[i] = -1;
	for (int32 i = 0; i < operatorCount; i++) {
		const neural_graph_operator* op = graph.OperatorAt(i);
		for (int32 j = 0; j < op->output_count; j++)
			producer[op->outputs[j]] = i;
		for (int32 j = 0; j < op->input_count; j++) {
			if (op->inputs[j] >= 0)
				consumers[op->inputs[j]]++;
		}
	}

	// With -v, the whole model on the processor as well: how far its
	// tensors are from the CPU's when every operator works on what the
	// ones before it made. (All tensors come out of this model, so no
	// addition becomes part of a convolution here.)
	if (sVerbose) {
		BNeuralSession deviceSession;
		status = deviceSession.SetTo(whole.model, device, 0);
		for (int32 i = 0; status == B_OK && i < whole.input_count; i++) {
			deviceSession.InputAt(i)->CopyFrom(*wholeSession.InputAt(i));
		}
		if (status == B_OK)
			status = deviceSession.Run();
		for (int32 i = 0; status == B_OK && i < whole.output_count; i++) {
			const BTensor* a = deviceSession.OutputAt(i);
			if (a->Type() != B_NEURAL_UINT8 && a->Type() != B_NEURAL_INT8)
				continue;
			const uint8* x = (const uint8*)a->Data();
			const uint8* y = reference[whole.outputs[i]];
			bool isSigned = a->Type() == B_NEURAL_INT8;
			int64 different = 0;
			int32 largest = 0;
			for (size_t j = 0; j < a->ByteSize(); j++) {
				int32 difference = abs(isSigned
					? (int32)(int8)x[j] - (int32)(int8)y[j]
					: (int32)x[j] - (int32)y[j]);
				if (difference != 0)
					different++;
				if (difference > largest)
					largest = difference;
			}
			printf("  in the whole model: %-40.40s %" B_PRId64 " of %"
				B_PRIuSIZE " differ, largest difference %" B_PRId32 "\n",
				a->Info().Name(), different, a->ByteSize(), largest);
		}
	}

	int32 failures = 0;
	int32 checked = 0;
	int32 withDifferences = 0;

	for (int32 i = 0; i < operatorCount; i++) {
		const neural_graph_operator* op = graph.OperatorAt(i);
		if (op->code != B_NEURAL_OP_CONV_2D
			&& op->code != B_NEURAL_OP_DEPTHWISE_CONV_2D
			&& op->code != B_NEURAL_OP_ADD) {
			continue;
		}

		// an addition goes with the convolution before it
		int32 operators[2] = { i, -1 };
		int32 count = 1;
		if (op->code == B_NEURAL_OP_ADD && op->input_count == 2) {
			int32 a = producer[op->inputs[0]];
			int32 b = producer[op->inputs[1]];
			int32 convolution = a > b ? a : b;
			if (convolution >= 0) {
				operators[0] = convolution;
				operators[1] = i;
				count = 2;
			}
		}

		SubModel sub;
		BNeuralSession session;
		status = build_sub_model(graph, operators, count, op->outputs[0],
			sub);
		if (status == B_OK)
			status = session.SetTo(sub.model, device, 0);
		if (status != B_OK) {
			printf("FAILED: operator %" B_PRId32 ": no session: %s\n", i,
				strerror(status));
			failures++;
			continue;
		}

		const neural_graph_tensor* output = graph.TensorAt(op->outputs[0]);
		char shape[64];
		snprintf(shape, sizeof(shape), "%" B_PRId32 "x%" B_PRId32 "x%"
			B_PRId32, output->rank == 4 ? output->dimensions[2] : 0,
			output->rank == 4 ? output->dimensions[1] : 0,
			output->rank == 4 ? output->dimensions[3] : 0);

		neural_run_info info;
		session.GetRunInfo(&info);
		if (info.cpu_operators != 0) {
			if (sVerbose) {
				printf("  operator %3" B_PRId32 " %-18s -> %-12s on the "
					"CPU\n", i, neural_operator_name(op->code), shape);
			}
			continue;
		}

		for (int32 j = 0; j < sub.input_count; j++) {
			BTensor* tensor = session.InputAt(j);
			memcpy(tensor->Data(), reference[sub.inputs[j]],
				tensor->ByteSize());
		}
		status = session.Run();
		if (status != B_OK) {
			printf("FAILED: operator %" B_PRId32 ": run: %s\n", i,
				strerror(status));
			failures++;
			continue;
		}

		const BTensor* result = session.OutputAt(0);
		const uint8* x = (const uint8*)result->Data();
		const uint8* y = reference[op->outputs[0]];
		bool isSigned = result->Type() == B_NEURAL_INT8;
		int64 different = 0;
		int32 largest = 0;
		for (size_t j = 0; j < result->ByteSize(); j++) {
			int32 p = isSigned ? (int32)(int8)x[j] : (int32)x[j];
			int32 q = isSigned ? (int32)(int8)y[j] : (int32)y[j];
			int32 difference = abs(p - q);
			if (difference != 0)
				different++;
			if (difference > largest)
				largest = difference;
		}

		checked++;
		if (different > 0)
			withDifferences++;
		bool ok = largest <= sTolerance;
		if (!ok)
			failures++;
		if (!ok || sVerbose) {
			printf("%s operator %3" B_PRId32 " %-18s -> %-12s %" B_PRId64
				" of %" B_PRIuSIZE " differ, largest difference %" B_PRId32
				"\n", ok ? "ok:    " : "FAILED:", i,
				count == 2 ? "convolution + ADD"
					: neural_operator_name(op->code), shape, different,
				result->ByteSize(), largest);
		}
	}

	printf("%s: %" B_PRId32 " operators (additions with their convolution) "
		"checked on the processor, %" B_PRId32 " with differences, %"
		B_PRId32 " beyond the tolerance of %" B_PRId32 "\n", path, checked,
		withDifferences, failures, sTolerance);

	free(all);
	free(reference);
	free(producer);
	free(consumers);
	return failures == 0 ? 0 : 1;
}


/*!	The same input has to give the same output, however often. */
static int
repeat_model(const char* path, const char* inputPath, int32 runs,
	const BNeuralDevice& device)
{
	BNeuralModel model(path);
	BNeuralSession session(model, device, 0);
	status_t status = session.InitCheck();
	if (status == B_OK)
		status = fill_model_inputs(session, inputPath);
	if (status != B_OK) {
		printf("FAILED: %s: %s\n", path, strerror(status));
		return 1;
	}

	neural_run_info info;
	session.GetRunInfo(&info);

	size_t size = 0;
	for (int32 i = 0; i < session.CountOutputs(); i++)
		size += session.OutputAt(i)->ByteSize();
	uint8* first = (uint8*)malloc(size);
	if (first == NULL)
		return 1;

	int32 different = 0;
	bigtime_t start = system_time();
	for (int32 run = 0; run < runs; run++) {
		status = session.Run();
		if (status != B_OK) {
			printf("FAILED: %s: run %" B_PRId32 ": %s\n", path, run,
				strerror(status));
			free(first);
			return 1;
		}

		size_t offset = 0;
		bool same = true;
		for (int32 i = 0; i < session.CountOutputs(); i++) {
			const BTensor* output = session.OutputAt(i);
			if (run == 0)
				memcpy(first + offset, output->Data(), output->ByteSize());
			else if (memcmp(first + offset, output->Data(),
					output->ByteSize()) != 0) {
				same = false;
			}
			offset += output->ByteSize();
		}
		if (!same)
			different++;
	}
	bigtime_t duration = system_time() - start;
	free(first);

	printf("%s: %s: %" B_PRId32 " runs on %s (%" B_PRId32 " of %" B_PRId32
		" operators on the processor), %.2f ms each, %" B_PRId32
		" with outputs other than the first run's\n",
		different == 0 ? "ok" : "FAILED", path, runs, device.ID(),
		info.device_operators, info.device_operators + info.cpu_operators,
		duration / 1000.0 / runs, different);
	return different == 0 ? 0 : 1;
}


int
main(int argc, char** argv)
{
	const char* specs[64];
	int32 specCount = 0;
	const char* modelPath = NULL;
	const char* inputPath = NULL;
	int32 repeat = 0;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0)
			sVerbose = true;
		else if (strcmp(argv[i], "--tolerance") == 0 && i + 1 < argc)
			sTolerance = atoi(argv[++i]);
		else if (strcmp(argv[i], "--case") == 0 && i + 1 < argc
			&& specCount < 64) {
			specs[specCount++] = argv[++i];
		} else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc)
			modelPath = argv[++i];
		else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc)
			inputPath = argv[++i];
		else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
			repeat = atoi(argv[++i]);
		else {
			fprintf(stderr, "usage: %s [-v] [--tolerance <n>] "
				"[--case <spec>]...\n"
				"       %s [-v] [--tolerance <n>] --model <file> "
				"[--repeat <n>] [--input <file>]\n", argv[0], argv[0]);
			return 2;
		}
	}

	// pictures are read by translators, which want an application
	BApplication application("application/x-vnd.airos-rk3588-npu-test");

	BNeuralRoster* roster = BNeuralRoster::Default();
	BNeuralDevice device;
	BNeuralDevice cpu;
	bool found = false;
	for (int32 i = 0; i < roster->CountDevices(); i++) {
		BNeuralDevice candidate;
		if (roster->GetDeviceAt(i, &candidate) != B_OK)
			continue;
		if (strstr(candidate.ID(), "rk3588_npu") != NULL) {
			device = candidate;
			found = true;
		}
	}
	if (!found || roster->FindDevice("cpu", &cpu) != B_OK) {
		printf("FAILED: no rk3588_npu device\n");
		return 1;
	}

	if (modelPath != NULL && repeat > 0)
		return repeat_model(modelPath, inputPath, repeat, device);
	if (modelPath != NULL)
		return check_model(modelPath, inputPath, device, cpu);

	int32 failures = 0;
	int32 count = specCount > 0 ? specCount
		: (int32)(sizeof(kCases) / sizeof(kCases[0]));
	for (int32 i = 0; i < count; i++) {
		const char* spec = specCount > 0 ? specs[i] : kCases[i];
		Case test;
		if (!parse_case(spec, test)) {
			printf("FAILED: cannot read \"%s\"\n", spec);
			failures++;
			continue;
		}
		if (!run_case(test, device, cpu))
			failures++;
	}

	printf("%" B_PRId32 " of %" B_PRId32 " cases failed\n", failures, count);
	return failures == 0 ? 0 : 1;
}
