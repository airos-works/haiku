/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Runs a model with the Neural Kit's CPU interpreter on the build host,
	for comparing against a reference implementation (see compare.py).

		neural_host info <model>
		neural_host run <model> <inputs> <outputs> [--dump <directory>]
			[--repeat <n>] [--profile]

	<inputs> holds the raw bytes of the model's inputs one after the
	other; the outputs are written the same way. "random" instead of a
	file fills the inputs with arbitrary bytes (the same ones every time):
	for running models whose results are not looked at. With --dump every
	tensor that is not a constant is written to <directory>/<index>.bin.

	The exit status is 0 when the model ran, 1 when it could not be loaded
	or run (which is the right answer for a damaged model), 2 for a wrong
	command line.
*/


#include <NeuralInterpreter.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>


using namespace BPrivate;


static void
print_tensor(const NeuralGraphData* graph, int32 index)
{
	const neural_graph_tensor& tensor = graph->Tensor(index);
	printf("  %4d %-8s [", (int)index, neural_data_type_name(tensor.type));
	for (int32 i = 0; i < tensor.rank; i++)
		printf("%s%d", i > 0 ? ", " : "", (int)tensor.dimensions[i]);
	printf("]");
	if (tensor.quantized) {
		printf(" scale %g zero %d", tensor.scale, (int)tensor.zero_point);
		if (tensor.channel_count > 0)
			printf(" (%d channel scales)", (int)tensor.channel_count);
	}
	printf(" %s%s\n", tensor.name != NULL ? tensor.name : "",
		tensor.data != NULL ? " (constant)" : "");
}


static bool
read_file(const char* path, void* buffer, size_t size)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return false;
	bool ok = fread(buffer, 1, size, file) == size && fgetc(file) == EOF;
	fclose(file);
	return ok;
}


/*!	What a run allocates, freed whichever way the run ends: tools that
	look for leaks are then left with those of the kit. */
struct RunMemory {
	const NeuralGraphData*	graph;
	void**					tensors;
	uint8*					inputs;
	int32*					order;
	bigtime_t*				durations;

	RunMemory(const NeuralGraphData* graph)
		:
		graph(graph),
		tensors(NULL),
		inputs(NULL),
		order(NULL),
		durations(NULL)
	{
	}

	~RunMemory()
	{
		if (tensors != NULL) {
			for (int32 i = 0; i < graph->CountTensors(); i++) {
				if (graph->Tensor(i).data == NULL)
					free(tensors[i]);
			}
		}
		free(tensors);
		free(inputs);
		free(order);
		free(durations);
	}
};


static int
print_info(const NeuralGraphData* graph)
{
	printf("model \"%s\": %d tensors, %d operators, %lld bytes of "
		"constants\n", graph->Name() != NULL ? graph->Name() : "",
		(int)graph->CountTensors(), (int)graph->CountOperators(),
		(long long)graph->ConstantBytes());
	printf("inputs:\n");
	for (int32 i = 0; i < graph->CountInputs(); i++)
		print_tensor(graph, graph->InputAt(i));
	printf("outputs:\n");
	for (int32 i = 0; i < graph->CountOutputs(); i++)
		print_tensor(graph, graph->OutputAt(i));
	printf("operators:\n");
	int unsupported = 0;
	for (int32 i = 0; i < graph->CountOperators(); i++) {
		const neural_graph_operator& op = graph->Operator(i);
		bool supported = NeuralInterpreter::Supports(graph, i);
		if (!supported)
			unsupported++;
		printf("  %4d %-22s%s\n", (int)i, neural_operator_name(op.code),
			supported ? "" : "  NOT SUPPORTED");
	}
	printf("%d of %d operators not supported by the CPU\n", unsupported,
		(int)graph->CountOperators());
	return unsupported == 0 ? 0 : 3;
}


static int
run_model(const NeuralGraphData* graph, const char* inputPath,
	const char* outputPath, const char* dumpDirectory, int repeat,
	bool profile)
{
	RunMemory memory(graph);

	// The operators are prepared first: what a kernel refuses needs no
	// memory.
	int32 opCount = graph->CountOperators();
	int32* order = memory.order = (int32*)malloc((opCount + 1)
		* sizeof(int32));
	for (int32 i = 0; i < opCount; i++)
		order[i] = i;

	NeuralInterpreter interpreter(graph);
	status_t status = interpreter.Prepare(order, opCount);
	if (status != B_OK) {
		fprintf(stderr, "prepare failed: %#x\n", (unsigned)status);
		return 1;
	}

	// Memory for every tensor, each in a block of its own (and of its
	// exact size, so that tools that watch memory see every step beyond
	// it). A model that asks for more than a test could want is turned
	// down rather than taking the machine's memory.
	int32 tensorCount = graph->CountTensors();
	uint64 total = 0;
	for (int32 i = 0; i < tensorCount; i++) {
		if (graph->Tensor(i).data == NULL)
			total += graph->Tensor(i).size;
	}
	if (total > ((uint64)1 << 31)) {
		fprintf(stderr, "the tensors take %llu bytes: too much for this "
			"tool\n", (unsigned long long)total);
		return 1;
	}

	void** tensors = memory.tensors = (void**)calloc(tensorCount + 1,
		sizeof(void*));
	for (int32 i = 0; i < tensorCount; i++) {
		const neural_graph_tensor& tensor = graph->Tensor(i);
		if (tensor.data != NULL) {
			tensors[i] = (void*)tensor.data;
		} else if (posix_memalign(&tensors[i], 64, tensor.size) != 0) {
			tensors[i] = NULL;
			fprintf(stderr, "out of memory\n");
			return 1;
		} else
			memset(tensors[i], 0, tensor.size);
	}

	size_t inputSize = 0;
	for (int32 i = 0; i < graph->CountInputs(); i++)
		inputSize += graph->Tensor(graph->InputAt(i)).size;
	uint8* inputs = memory.inputs = (uint8*)malloc(inputSize + 1);
	if (strcmp(inputPath, "random") == 0) {
		uint32 state = 12345;
		for (size_t i = 0; i < inputSize; i++) {
			state = state * 1664525 + 1013904223;
			inputs[i] = state >> 24;
		}
	} else if (!read_file(inputPath, inputs, inputSize)) {
		fprintf(stderr, "%s: cannot read %zu bytes of inputs\n", inputPath,
			inputSize);
		return 1;
	}
	size_t offset = 0;
	for (int32 i = 0; i < graph->CountInputs(); i++) {
		const neural_graph_tensor& tensor = graph->Tensor(graph->InputAt(i));
		memcpy(tensors[graph->InputAt(i)], inputs + offset, tensor.size);
		offset += tensor.size;
	}

	// the times of the fastest run, and of each operator in it
	bigtime_t* durations = memory.durations = (bigtime_t*)calloc(
		2 * (opCount + 1), sizeof(bigtime_t));
	bigtime_t* bestDurations = durations + opCount + 1;
	bigtime_t best = -1;
	for (int i = 0; i < repeat; i++) {
		bigtime_t start = system_time();
		status = interpreter.Run(tensors, profile ? durations : NULL);
		bigtime_t duration = system_time() - start;
		if (status != B_OK) {
			fprintf(stderr, "run failed: %#x\n", (unsigned)status);
			return 1;
		}
		if (best < 0 || duration < best) {
			best = duration;
			memcpy(bestDurations, durations, opCount * sizeof(bigtime_t));
		}
	}
	fprintf(stderr, "run: %lld us (best of %d, %d threads)\n",
		(long long)best, repeat,
		(int)NeuralThreadPool::Default()->CountThreads());

	if (profile) {
		// per operator, then per kind of operator
		bigtime_t byCode[B_NEURAL_OP_COUNT] = {};
		for (int32 i = 0; i < opCount; i++) {
			fprintf(stderr, "  %4d %-22s %8lld us\n", (int)i,
				neural_operator_name(graph->Operator(i).code),
				(long long)bestDurations[i]);
			byCode[graph->Operator(i).code] += bestDurations[i];
		}
		for (int32 code = 0; code < B_NEURAL_OP_COUNT; code++) {
			if (byCode[code] != 0) {
				fprintf(stderr, "  all  %-22s %8lld us\n",
					neural_operator_name((neural_operator_code)code),
					(long long)byCode[code]);
			}
		}
	}

	FILE* output = fopen(outputPath, "wb");
	if (output == NULL) {
		fprintf(stderr, "%s: cannot write\n", outputPath);
		return 1;
	}
	for (int32 i = 0; i < graph->CountOutputs(); i++) {
		const neural_graph_tensor& tensor
			= graph->Tensor(graph->OutputAt(i));
		fwrite(tensors[graph->OutputAt(i)], 1, tensor.size, output);
	}
	fclose(output);

	if (dumpDirectory != NULL) {
		for (int32 i = 0; i < tensorCount; i++) {
			const neural_graph_tensor& tensor = graph->Tensor(i);
			if (tensor.data != NULL)
				continue;
			char path[1024];
			snprintf(path, sizeof(path), "%s/%d.bin", dumpDirectory, (int)i);
			FILE* dump = fopen(path, "wb");
			if (dump != NULL) {
				fwrite(tensors[i], 1, tensor.size, dump);
				fclose(dump);
			}
		}
	}

	return 0;
}


int
main(int argc, char** argv)
{
	bool info = argc >= 3 && strcmp(argv[1], "info") == 0;
	if (argc < 3 || (!info && (strcmp(argv[1], "run") != 0 || argc < 5))) {
		fprintf(stderr, "usage: %s info <model>\n"
			"       %s run <model> <inputs> <outputs> [--dump <directory>]"
			" [--repeat <n>] [--profile]\n", argv[0], argv[0]);
		return 2;
	}

	const char* dumpDirectory = NULL;
	int repeat = 1;
	bool profile = false;
	for (int i = 5; i < argc; i++) {
		if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
			dumpDirectory = argv[++i];
		else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc)
			repeat = atoi(argv[++i]);
		else if (strcmp(argv[i], "--profile") == 0)
			profile = true;
	}

	int fd = open(argv[2], O_RDONLY);
	struct stat stat;
	if (fd < 0 || fstat(fd, &stat) != 0) {
		fprintf(stderr, "%s: cannot open\n", argv[2]);
		if (fd >= 0)
			close(fd);
		return 1;
	}

	void* file = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (file == MAP_FAILED) {
		fprintf(stderr, "%s: cannot map\n", argv[2]);
		return 1;
	}

	// the graph unmaps the file when it goes
	NeuralGraphData* graph = new NeuralGraphData();
	graph->AdoptBuffer(file, stat.st_size, true);

	int result;
	status_t status = neural_load_tflite(file, stat.st_size, graph);
	if (status != B_OK) {
		fprintf(stderr, "%s: load failed: %s (%#x)\n", argv[2],
			status == B_NEURAL_UNSUPPORTED_OPERATOR ? "unsupported operator"
				: status == B_NEURAL_BAD_MODEL ? "bad model"
				: status == B_NEURAL_UNKNOWN_FORMAT ? "unknown format"
				: "error", (unsigned)status);
		result = 1;
	} else if (info)
		result = print_info(graph);
	else {
		result = run_model(graph, argv[3], argv[4], dumpDirectory, repeat,
			profile);
	}

	graph->ReleaseReference();
	return result;
}
