/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Checks the amdxdna back end (the NPU of AMD's Ryzen AI processors)
	with a design bundle and the network the bundle is for.

		amdxdna_backend_test [-v] [--cycles <n>] [--runs <n>]
			[--no-hardware] <bundle dir>

	<bundle dir> is a bundle as make-bundle.py (linux-drm repository,
	haiku/tests/xdna) writes it: the design, bundle.txt, and model.txt with
	weights, an input and the output the kit's CPU computes for it. The
	test builds that network with BNeuralModelBuilder, lets the back end
	find the bundle (NEURAL_AMDXDNA_BUNDLES is set to the directory the
	bundle is in) and then does one of two things, and says which:

	(a) Without the NPU - no /dev/npu/amdxdna/0, or the caller is not
	    root, or the NPU cannot be started: the back end lists the device
	    exactly when the node exists, claims the convolution exactly when
	    the bundle is there and is for these weights, and a session on
	    the device computes on the CPU, with the expected output.

	(b) With the NPU: the same checks, and the session's outputs are the
	    NPU's: compared with the CPU's for the bundle's input and --runs
	    (10) inputs of noise. The design computes what the kit computes
	    but for one thing that only the hardware can tell: whether it
	    rounds a sum that is an exact half away from zero (as the kit) or
	    up. Outputs that differ by one step at negative exact halves are
	    counted and reported, and do not fail the test; anything else
	    does.

	--cycles <n> sets a session up on the device and deletes it n times
	before anything else: programs made and deleted, or failing to be
	made, must leave nothing behind (look at the driver's buffer memory
	before and after, "xdna_test buffers").

	--no-hardware is for the driver's device without hardware (its
	"haiku_selftest 2", in a virtual machine): there a program is made as
	on the NPU - contexts, buffers, weights - and its first command is
	refused. The test then passes if the session went on on the CPU with
	the expected output, which on a real NPU is a failure.

	Exits with 0 when everything it checked held.
*/


#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <NeuralKit.h>
#include <Path.h>
#include <String.h>


static const char* kNode = "/dev/npu/amdxdna/0";

static bool sVerbose;
static bool sNoHardware;
static int32 sFailures;


static void
check(bool condition, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	if (!condition)
		sFailures++;
	if (!condition || sVerbose) {
		printf(condition ? "ok: " : "FAIL: ");
		vprintf(format, args);
		printf("\n");
	}
	va_end(args);
}


struct Description {
	int32	height, width, inputChannels, outputChannels;
	float	inputScale, weightScale, outputScale;
	BString	weightsFile, inputFile, expectedFile;
};


static void*
read_file(const char* directory, const char* name, size_t expectedSize)
{
	BString path(directory);
	path << "/" << name;
	FILE* file = fopen(path.String(), "rb");
	void* data = malloc(expectedSize + 1);
	if (file == NULL || data == NULL
		|| fread(data, 1, expectedSize + 1, file) != expectedSize) {
		fprintf(stderr, "%s: cannot read %zu bytes\n", path.String(),
			expectedSize);
		exit(1);
	}
	fclose(file);
	return data;
}


static void
read_description(const char* directory, Description& description)
{
	BString path(directory);
	path << "/model.txt";
	FILE* file = fopen(path.String(), "r");
	if (file == NULL) {
		fprintf(stderr, "%s: %s\n", path.String(), strerror(errno));
		exit(1);
	}

	char line[512];
	int found = 0;
	while (fgets(line, sizeof(line), file) != NULL) {
		char name[256];
		line[strcspn(line, "\r\n")] = '\0';
		if (strcmp(line, "operator=CONV_2D") == 0)
			found |= 1;
		else if (sscanf(line, "input=%d,%d,%d,%f", &description.height,
				&description.width, &description.inputChannels,
				&description.inputScale) == 4) {
			found |= 2;
		} else if (sscanf(line, "weights=%d,%*d,%f,%255s",
				&description.outputChannels, &description.weightScale,
				name) == 3) {
			description.weightsFile = name;
			found |= 4;
		} else if (sscanf(line, "output=%*d,%*d,%*d,%f",
				&description.outputScale) == 1) {
			found |= 8;
		} else if (sscanf(line, "test_input=%255s", name) == 1) {
			description.inputFile = name;
			found |= 16;
		} else if (sscanf(line, "test_expected=%255s", name) == 1) {
			description.expectedFile = name;
			found |= 32;
		}
	}
	fclose(file);
	if (found != 63) {
		fprintf(stderr, "%s: not the description of a 1x1 convolution\n",
			path.String());
		exit(1);
	}
}


/*!	The network of the bundle: one CONV_2D, 1x1, int8, zero points 0, no
	bias, in the order make-bundle.py assumes nothing about (the graph
	key does not depend on it). */
static status_t
build_model(const Description& description, const int8* weights,
	float outputScale, BNeuralModel& model)
{
	BNeuralModelBuilder builder("conv2d 1x1");

	int32 inputDims[] = { 1, description.height, description.width,
		description.inputChannels };
	BTensorInfo inputInfo(B_NEURAL_INT8, 4, inputDims, "input");
	inputInfo.SetQuantization(description.inputScale, 0);
	int32 input = builder.AddInput(inputInfo);

	int32 weightDims[] = { description.outputChannels, 1, 1,
		description.inputChannels };
	BTensorInfo weightInfo(B_NEURAL_INT8, 4, weightDims, "weights");
	weightInfo.SetQuantization(description.weightScale, 0);
	int32 weightsTensor = builder.AddConstant(weightInfo, weights,
		(size_t)description.outputChannels * description.inputChannels);

	int32 outputDims[] = { 1, description.height, description.width,
		description.outputChannels };
	BTensorInfo outputInfo(B_NEURAL_INT8, 4, outputDims, "output");
	outputInfo.SetQuantization(outputScale, 0);

	neural_conv_params params;
	memset(&params, 0, sizeof(params));
	params.padding = B_NEURAL_PADDING_VALID;
	params.stride_width = params.stride_height = 1;
	params.dilation_width = params.dilation_height = 1;
	params.activation = B_NEURAL_ACTIVATION_NONE;

	int32 inputs[] = { input, weightsTensor, -1 };
	int32 output = builder.AddOperator(B_NEURAL_OP_CONV_2D, inputs, 3,
		outputInfo, &params, sizeof(params));
	builder.MarkOutput(output);
	return builder.Build(&model);
}


static uint32 sRandom = 12345;

static int8
noise()
{
	sRandom = sRandom * 1664525 + 1013904223;
	return (int8)(sRandom >> 24);
}


/*!	Compares what the device's session made of \a input with the CPU's.
	Returns the number of outputs that differ in a way the rounding of
	exact halves does not explain; those it does explain are counted in
	\a _halves. */
static int32
compare(const Description& description, const int8* weights,
	const int8* input, const int8* device, const int8* cpu, int32* _halves)
{
	int32 wrong = 0;
	size_t pixels = (size_t)description.height * description.width;
	for (size_t pixel = 0; pixel < pixels; pixel++) {
		const int8* in = input + pixel * description.inputChannels;
		for (int32 o = 0; o < description.outputChannels; o++) {
			size_t index = pixel * description.outputChannels + o;
			if (device[index] == cpu[index])
				continue;

			// the sum behind this output: a negative exact half is where
			// "halves up" gives one more than "halves away from zero"
			int32 sum = 0;
			for (int32 i = 0; i < description.inputChannels; i++) {
				sum += (int32)in[i]
					* weights[(size_t)o * description.inputChannels + i];
			}
			if (sum < 0 && (sum & 1023) == 512
				&& device[index] == cpu[index] + 1) {
				++*_halves;
				continue;
			}
			if (wrong++ < 8) {
				printf("  output %zu: NPU %d, CPU %d (sum %" B_PRId32 ")\n",
					index, device[index], cpu[index], sum);
			}
		}
	}
	return wrong;
}


int
main(int argc, char** argv)
{
	int32 cycles = 0;
	int32 runs = 10;
	const char* directory = NULL;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0)
			sVerbose = true;
		else if (strcmp(argv[i], "--no-hardware") == 0)
			sNoHardware = true;
		else if (strcmp(argv[i], "--cycles") == 0 && i + 1 < argc)
			cycles = atoi(argv[++i]);
		else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc)
			runs = atoi(argv[++i]);
		else if (argv[i][0] != '-' && directory == NULL)
			directory = argv[i];
		else
			directory = NULL, i = argc;
	}
	if (directory == NULL) {
		fprintf(stderr, "usage: amdxdna_backend_test [-v] [--cycles <n>] "
			"[--runs <n>] [--no-hardware] <bundle dir>\n");
		return 2;
	}

	Description description;
	read_description(directory, description);
	size_t inputSize = (size_t)description.height * description.width
		* description.inputChannels;
	size_t outputSize = (size_t)description.height * description.width
		* description.outputChannels;
	size_t weightsSize = (size_t)description.outputChannels
		* description.inputChannels;
	int8* weights = (int8*)read_file(directory,
		description.weightsFile.String(), weightsSize);
	int8* input = (int8*)read_file(directory, description.inputFile.String(),
		inputSize);
	int8* expected = (int8*)read_file(directory,
		description.expectedFile.String(), outputSize);

	// Bundles are looked for where this one is, and at first nowhere.
	BPath bundles(directory);
	BPath parent;
	bundles.GetParent(&parent);
	char empty[] = "/tmp/amdxdna_backend_test.XXXXXX";
	if (mkdtemp(empty) == NULL) {
		fprintf(stderr, "no temporary directory: %s\n", strerror(errno));
		return 1;
	}

	BNeuralModel model;
	status_t status = build_model(description, weights,
		description.outputScale, model);
	if (status != B_OK) {
		fprintf(stderr, "building the network: %s\n", strerror(status));
		return 1;
	}

	// the CPU: the reference, and what the bundle's maker says it computes
	int8* cpuOutput = (int8*)malloc(outputSize);
	{
		BNeuralSession session(model, B_NEURAL_CPU_ONLY);
		status = session.InitCheck();
		if (status == B_OK) {
			memcpy(session.InputAt(0)->Data(), input, inputSize);
			status = session.Run();
		}
		if (status != B_OK) {
			fprintf(stderr, "the CPU session: %s\n", strerror(status));
			return 1;
		}
		memcpy(cpuOutput, session.OutputAt(0)->Data(), outputSize);
		check(memcmp(cpuOutput, expected, outputSize) == 0,
			"the CPU computes the output the bundle's maker expects");
	}

	// the device: there exactly when the node is, and nothing was opened
	// to find out
	struct stat st;
	bool nodeExists = stat(kNode, &st) == 0;
	BNeuralRoster* roster = BNeuralRoster::Default();
	BNeuralDevice device;
	bool listed = roster->FindDevice("amdxdna/0", &device) == B_OK;
	check(listed == nodeExists, "%s %s, the device is %slisted", kNode,
		nodeExists ? "exists" : "does not exist", listed ? "" : "not ");

	if (!listed) {
		// what is left to check: a session on whatever there is works
		BNeuralSession session(model);
		status = session.InitCheck();
		if (status == B_OK) {
			memcpy(session.InputAt(0)->Data(), input, inputSize);
			status = session.Run();
		}
		check(status == B_OK && memcmp(session.OutputAt(0)->Data(), expected,
				outputSize) == 0,
			"a session without the device computes the expected output");
		rmdir(empty);
		printf("(a) no NPU node: %s\n", sFailures == 0 ? "PASS" : "FAILED");
		return sFailures == 0 ? 0 : 1;
	}

	check(device.Kind() == B_NEURAL_DEVICE_NPU
		&& (device.Flags() & B_NEURAL_DEVICE_QUANTIZED) != 0
		&& strcmp(device.Backend(), "amdxdna") == 0,
		"the device is an NPU for quantized networks (%s, %s)",
		device.Name(), device.Vendor());

	// claimed exactly when there is a bundle for this very network
	setenv("NEURAL_AMDXDNA_BUNDLES", empty, 1);
	int32 without = device.CountSupportedOperators(model);
	setenv("NEURAL_AMDXDNA_BUNDLES", parent.Path(), 1);
	int32 with = device.CountSupportedOperators(model);
	check(without == 0, "no bundle directory with the network's key: "
		"%" B_PRId32 " operators claimed", without);
	check(with == 1, "the bundle is there: %" B_PRId32 " of 1 operators "
		"claimed", with);
	{
		// other weights: the same key, another digest
		int8* other = (int8*)malloc(weightsSize);
		memcpy(other, weights, weightsSize);
		other[weightsSize / 2] ^= 2;
		BNeuralModel otherModel;
		build_model(description, other, description.outputScale, otherModel);
		int32 count = device.CountSupportedOperators(otherModel);
		check(count == 0, "one weight changed: %" B_PRId32 " operators "
			"claimed", count);
		free(other);

		// another quantization: another key
		BNeuralModel scaledModel;
		build_model(description, weights, description.outputScale * 2,
			scaledModel);
		count = device.CountSupportedOperators(scaledModel);
		check(count == 0, "another output scale: %" B_PRId32 " operators "
			"claimed", count);
	}

	// programs made and deleted, or failing to be made
	for (int32 i = 0; i < cycles; i++) {
		BNeuralSession session(model, device);
		if (session.InitCheck() != B_OK) {
			check(false, "session %" B_PRId32 " of %" B_PRId32 ": %s", i,
				cycles, strerror(session.InitCheck()));
			break;
		}
	}
	if (cycles > 0)
		printf("%" B_PRId32 " sessions set up and deleted\n", cycles);

	// a session on the device: the NPU's if a program could be made, the
	// CPU's if not - right either way
	BNeuralSession session(model, device);
	status = session.InitCheck();
	check(status == B_OK, "a session on the device: %s", strerror(status));
	if (status != B_OK)
		return 1;
	neural_run_info info;
	session.GetRunInfo(&info);
	bool onDevice = info.device_operators == 1;

	memcpy(session.InputAt(0)->Data(), input, inputSize);
	status = session.Run();
	check(status == B_OK, "running it: %s", strerror(status));
	if (status != B_OK)
		return 1;
	session.GetRunInfo(&info);
	if (onDevice && info.device_operators == 0) {
		// the program gave up in its first run: the CPU took over
		printf("a program was made, its command failed, the CPU took "
			"over%s\n", sNoHardware ? " (as it has to without hardware)" : "");
		onDevice = false;
		if (!sNoHardware)
			sFailures++;
	} else if (sNoHardware) {
		check(false, "--no-hardware, but %s", onDevice
			? "the device ran the command" : "no program was made");
	}

	int32 halves = 0;
	if (!onDevice) {
		check(memcmp(session.OutputAt(0)->Data(), expected, outputSize) == 0,
			"not computed by the NPU (%s): the session computed the expected "
			"output on the CPU", geteuid() != 0 ? "not root"
				: "see the syslog");
	} else {
		int32 wrong = compare(description, weights, input,
			(const int8*)session.OutputAt(0)->Data(), cpuOutput, &halves);
		check(wrong == 0, "the bundle's input: %" B_PRId32 " outputs of "
			"the NPU differ from the CPU's", wrong);

		BNeuralSession cpuSession(model, B_NEURAL_CPU_ONLY);
		int8* random = (int8*)malloc(inputSize);
		bigtime_t deviceTime = 0;
		for (int32 run = 0; run < runs && cpuSession.InitCheck() == B_OK;
				run++) {
			for (size_t i = 0; i < inputSize; i++)
				random[i] = noise();
			memcpy(session.InputAt(0)->Data(), random, inputSize);
			memcpy(cpuSession.InputAt(0)->Data(), random, inputSize);
			status = session.Run();
			if (status == B_OK)
				status = cpuSession.Run();
			if (status != B_OK) {
				check(false, "run %" B_PRId32 ": %s", run, strerror(status));
				break;
			}
			session.GetRunInfo(&info);
			deviceTime += info.last_device_duration;
			if (info.device_operators != 1) {
				check(false, "run %" B_PRId32 ": the NPU's program failed",
					run);
				break;
			}
			wrong = compare(description, weights, random,
				(const int8*)session.OutputAt(0)->Data(),
				(const int8*)cpuSession.OutputAt(0)->Data(), &halves);
			check(wrong == 0, "noise %" B_PRId32 ": %" B_PRId32 " outputs "
				"of the NPU differ from the CPU's", run, wrong);
		}
		free(random);
		if (runs > 0) {
			printf("%" B_PRId32 " runs on the NPU, %" B_PRId64 " us each\n",
				runs, deviceTime / runs);
		}
		if (halves > 0) {
			printf("the NPU rounds exact halves up, the kit away from zero: "
				"%" B_PRId32 " outputs one step higher at negative halves\n",
				halves);
		} else
			printf("the NPU rounds as the kit does: no output differs\n");
	}

	rmdir(empty);
	printf("(%s) %s: %s\n", onDevice ? "b" : "a",
		onDevice ? "the NPU computed" : "the CPU computed, the NPU was not "
			"used", sFailures == 0 ? "PASS" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
