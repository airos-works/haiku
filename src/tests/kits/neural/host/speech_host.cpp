/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The Neural Kit's speech recognizer on the build host: its core (the
	model directory, the front end, the decoding, the engine) with the
	CPU interpreter under it, for comparing against a reference (see
	speech_reference.py and speech_tests.sh).

		speech_host transcribe <model directory> [--no-timestamps]
			[--language <code>] [--chunk <samples>] [--interim <seconds>]
			[--times] <audio.wav>...
		speech_host mel <model directory> <audio.wav> <first sample>
			<output>
		speech_host convert <format> <channels> <rate> <target rate>
			<chunk bytes> <input> <output>
		speech_host text <model directory> <token>...

	"transcribe" prints a line per phrase, "[start --> end] text", as the
	reference does. With --chunk the audio is fed that many samples at a
	time and recognized as it comes in (the result must be the same);
	with --interim a look at the window that is still filling is taken
	every so many seconds of audio and printed with a "~". --times adds
	where the time went.

	"mel" writes the spectrogram of one window (float32 [bins][frames]),
	"convert" puts raw audio (<format>: uint8, int16, int32, float32,
	float64; little endian) through the converter and writes float32
	samples, "text" prints the text of tokens followed by a line feed.
*/


#include <NeuralInterpreter.h>
#include <SpeechCore.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>


using namespace BPrivate;


/*!	A model file run by the interpreter. */
class HostNetwork : public SpeechNetwork {
public:
	HostNetwork()
		:
		fGraph(NULL),
		fInterpreter(NULL),
		fTensors(NULL)
	{
	}

	virtual ~HostNetwork()
	{
		delete fInterpreter;
		if (fTensors != NULL) {
			for (int32 i = 0; i < fGraph->CountTensors(); i++) {
				if (fGraph->Tensor(i).data == NULL)
					free(fTensors[i]);
			}
		}
		free(fTensors);
		if (fGraph != NULL)
			fGraph->ReleaseReference();
	}

	status_t Load(const char* path)
	{
		int fd = open(path, O_RDONLY);
		struct stat stat;
		if (fd < 0 || fstat(fd, &stat) != 0) {
			if (fd >= 0)
				close(fd);
			return B_NAME_NOT_FOUND;
		}
		void* file = mmap(NULL, stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
		close(fd);
		if (file == MAP_FAILED)
			return B_NO_MEMORY;

		fGraph = new NeuralGraphData();
		status_t status = fGraph->AdoptBuffer(file, stat.st_size, true);
		if (status != B_OK) {
			munmap(file, stat.st_size);
			return status;
		}
		status = neural_load_tflite(file, stat.st_size, fGraph);
		if (status != B_OK) {
			fprintf(stderr, "%s: %s\n", path, fGraph->Error());
			return status;
		}

		int32 opCount = fGraph->CountOperators();
		int32* order = (int32*)malloc((opCount + 1) * sizeof(int32));
		for (int32 i = 0; i < opCount; i++)
			order[i] = i;
		fInterpreter = new NeuralInterpreter(fGraph);
		status = fInterpreter->Prepare(order, opCount);
		free(order);
		if (status != B_OK)
			return status;

		int32 tensorCount = fGraph->CountTensors();
		fTensors = (void**)calloc(tensorCount + 1, sizeof(void*));
		for (int32 i = 0; i < tensorCount; i++) {
			const neural_graph_tensor& tensor = fGraph->Tensor(i);
			if (tensor.data != NULL) {
				fTensors[i] = (void*)tensor.data;
			} else if (posix_memalign(&fTensors[i], 64,
					tensor.size > 0 ? tensor.size : 1) != 0) {
				fTensors[i] = NULL;
				return B_NO_MEMORY;
			} else
				memset(fTensors[i], 0, tensor.size);
		}
		return B_OK;
	}

	virtual int32 CountInputs() const
	{
		return fGraph->CountInputs();
	}

	virtual status_t GetInput(int32 index, speech_tensor* _tensor)
	{
		if (index < 0 || index >= fGraph->CountInputs())
			return B_BAD_INDEX;
		_Describe(fGraph->InputAt(index), _tensor);
		return B_OK;
	}

	virtual int32 CountOutputs() const
	{
		return fGraph->CountOutputs();
	}

	virtual status_t GetOutput(int32 index, speech_tensor* _tensor)
	{
		if (index < 0 || index >= fGraph->CountOutputs())
			return B_BAD_INDEX;
		_Describe(fGraph->OutputAt(index), _tensor);
		return B_OK;
	}

	virtual status_t Run()
	{
		return fInterpreter->Run(fTensors);
	}

private:
	void _Describe(int32 index, speech_tensor* _tensor)
	{
		const neural_graph_tensor& tensor = fGraph->Tensor(index);
		_tensor->name = tensor.name;
		_tensor->type = tensor.type;
		_tensor->rank = tensor.rank;
		for (int32 i = 0; i < B_NEURAL_MAX_RANK; i++)
			_tensor->dimensions[i] = tensor.dimensions[i];
		_tensor->data = fTensors[index];
		_tensor->size = tensor.size;
	}

	NeuralGraphData*	fGraph;
	NeuralInterpreter*	fInterpreter;
	void**				fTensors;
};


class Printer : public SpeechListener {
public:
	virtual void SegmentRecognized(bigtime_t start, bigtime_t end,
		const char* text, bool final)
	{
		printf("%s[%7.2f --> %7.2f] %s\n", final ? "" : "~", start / 1e6,
			end / 1e6, text);
		fflush(stdout);
	}
};


static bool
load_model(const char* directory, SpeechModel& model)
{
	status_t status = model.Load(directory);
	if (status != B_OK) {
		fprintf(stderr, "%s: %s (%#x)\n", directory, model.Error(),
			(unsigned)status);
		return false;
	}
	return true;
}


static int
transcribe(int argc, char** argv)
{
	SpeechModel model;
	if (!load_model(argv[2], model))
		return 1;
	const SpeechModelInfo& info = model.Info();

	bool timestamps = true;
	bool times = false;
	const char* language = NULL;
	long chunk = 0;
	double interim = 0;
	int first = 3;
	for (; first < argc && argv[first][0] == '-'; first++) {
		if (strcmp(argv[first], "--no-timestamps") == 0)
			timestamps = false;
		else if (strcmp(argv[first], "--times") == 0)
			times = true;
		else if (strcmp(argv[first], "--language") == 0 && first + 1 < argc)
			language = argv[++first];
		else if (strcmp(argv[first], "--chunk") == 0 && first + 1 < argc)
			chunk = atol(argv[++first]);
		else if (strcmp(argv[first], "--interim") == 0 && first + 1 < argc)
			interim = atof(argv[++first]);
		else
			return 2;
	}

	char path[1280];
	HostNetwork encoder;
	HostNetwork decoder;
	model.GetPath(info.encoder, path, sizeof(path));
	status_t status = encoder.Load(path);
	if (status == B_OK) {
		model.GetPath(info.decoder, path, sizeof(path));
		status = decoder.Load(path);
	}
	if (status != B_OK) {
		fprintf(stderr, "%s: cannot load: %#x\n", path, (unsigned)status);
		return 1;
	}

	Printer printer;
	SpeechEngine engine;
	status = engine.Init(&model, &encoder, &decoder);
	if (status != B_OK) {
		fprintf(stderr, "%s: %s (%#x)\n", argv[2], engine.Error(),
			(unsigned)status);
		return 1;
	}
	engine.SetListener(&printer);
	engine.SetTimestamps(timestamps);
	if (language != NULL && engine.SetLanguage(language) != B_OK) {
		fprintf(stderr, "the model has no language \"%s\"\n", language);
		return 1;
	}

	for (int i = first; i < argc; i++) {
		SpeechSamples audio;
		status = speech_read_wave_file(argv[i], info.sample_rate, audio);
		if (status != B_OK) {
			fprintf(stderr, "%s: cannot read: %#x\n", argv[i],
				(unsigned)status);
			return 1;
		}
		double seconds = (double)audio.Count() / info.sample_rate;
		printf("%s: %.2f s\n", argv[i], seconds);

		bigtime_t start = system_time();
		engine.Reset();
		if (chunk <= 0) {
			status = engine.AddSamples(audio.Samples(), audio.Count());
		} else {
			// as a recorder would hand it over
			double sinceInterim = 0;
			for (size_t at = 0; status == B_OK && at < audio.Count();
					at += chunk) {
				size_t count = audio.Count() - at < (size_t)chunk
					? audio.Count() - at : (size_t)chunk;
				status = engine.AddSamples(audio.Samples() + at, count);
				if (status == B_OK)
					status = engine.Process(false);
				sinceInterim += (double)count / info.sample_rate;
				if (status == B_OK && interim > 0
					&& sinceInterim >= interim) {
					status = engine.ProcessInterim();
					sinceInterim = 0;
				}
			}
		}
		if (status == B_OK)
			status = engine.Process(true);
		if (status != B_OK) {
			fprintf(stderr, "%s: recognition failed: %#x\n", argv[i],
				(unsigned)status);
			return 1;
		}

		if (times) {
			double took = (system_time() - start) / 1e6;
			const speech_engine_info& stats = engine.Info();
			fprintf(stderr, "  %.2f s for %.2f s of audio (%.3f of real "
				"time): %d windows, %d tokens; front end %.3f, encoder "
				"%.3f, decoder %.3f s; %d threads\n", took, seconds,
				took / seconds, (int)stats.windows, (int)stats.tokens,
				stats.front_end_time / 1e6, stats.encoder_time / 1e6,
				stats.decoder_time / 1e6,
				(int)NeuralThreadPool::Default()->CountThreads());
		}
	}
	return 0;
}


static int
write_mel(int argc, char** argv)
{
	if (argc != 6)
		return 2;

	SpeechModel model;
	if (!load_model(argv[2], model))
		return 1;
	const SpeechModelInfo& info = model.Info();

	SpeechSamples audio;
	if (speech_read_wave_file(argv[3], info.sample_rate, audio) != B_OK) {
		fprintf(stderr, "%s: cannot read\n", argv[3]);
		return 1;
	}

	SpeechFrontEnd frontEnd;
	size_t elements = (size_t)info.mel_bins * info.window_frames;
	float* mel = (float*)malloc(elements * sizeof(float));
	if (frontEnd.Init(info.fft_size, info.hop, info.mel_bins,
			info.window_frames, model.MelFilters()) != B_OK
		|| frontEnd.Compute(audio.Samples(), audio.Count(), 0,
			atoll(argv[4]), mel) != B_OK) {
		fprintf(stderr, "the front end failed\n");
		return 1;
	}

	FILE* file = fopen(argv[5], "wb");
	bool ok = file != NULL
		&& fwrite(mel, sizeof(float), elements, file) == elements;
	if (file != NULL)
		fclose(file);
	free(mel);
	return ok ? 0 : 1;
}


static int
convert(int argc, char** argv)
{
	if (argc != 9)
		return 2;

	uint32 format;
	if (strcmp(argv[2], "uint8") == 0)
		format = B_SPEECH_SAMPLE_UINT8;
	else if (strcmp(argv[2], "int16") == 0)
		format = B_SPEECH_SAMPLE_INT16;
	else if (strcmp(argv[2], "int32") == 0)
		format = B_SPEECH_SAMPLE_INT32;
	else if (strcmp(argv[2], "float32") == 0)
		format = B_SPEECH_SAMPLE_FLOAT;
	else if (strcmp(argv[2], "float64") == 0)
		format = B_SPEECH_SAMPLE_DOUBLE;
	else
		return 2;

	SpeechAudioConverter converter;
	if (converter.SetFormat(format, atoi(argv[3]), atof(argv[4]), false,
			atoi(argv[5])) != B_OK) {
		fprintf(stderr, "the converter does not take this format\n");
		return 1;
	}

	size_t chunk = atol(argv[6]);
	FILE* input = fopen(argv[7], "rb");
	FILE* output = fopen(argv[8], "wb");
	if (chunk == 0 || input == NULL || output == NULL)
		return 1;

	uint8* buffer = (uint8*)malloc(chunk);
	SpeechSamples samples;
	size_t count;
	status_t status = B_OK;
	while (status == B_OK && (count = fread(buffer, 1, chunk, input)) > 0)
		status = converter.Convert(buffer, count, samples);
	if (status == B_OK)
		status = converter.Flush(samples);
	if (status == B_OK) {
		fwrite(samples.Samples(), sizeof(float), samples.Count(), output);
	}
	free(buffer);
	fclose(input);
	fclose(output);
	return status == B_OK ? 0 : 1;
}


static int
print_text(int argc, char** argv)
{
	SpeechModel model;
	if (!load_model(argv[2], model))
		return 1;

	int32 count = argc - 3;
	int32* tokens = (int32*)malloc((count + 1) * sizeof(int32));
	for (int32 i = 0; i < count; i++)
		tokens[i] = atoi(argv[3 + i]);
	char* text;
	if (model.DecodeTokens(tokens, count, &text) != B_OK)
		return 1;
	printf("%s\n", text);
	free(text);
	free(tokens);
	return 0;
}


int
main(int argc, char** argv)
{
	int result = 2;
	if (argc >= 4 && strcmp(argv[1], "transcribe") == 0)
		result = transcribe(argc, argv);
	else if (argc >= 3 && strcmp(argv[1], "mel") == 0)
		result = write_mel(argc, argv);
	else if (argc >= 2 && strcmp(argv[1], "convert") == 0)
		result = convert(argc, argv);
	else if (argc >= 3 && strcmp(argv[1], "text") == 0)
		result = print_text(argc, argv);

	if (result == 2) {
		fprintf(stderr, "usage: %s transcribe <model directory> "
			"[--no-timestamps] [--language <code>] [--chunk <samples>] "
			"[--interim <seconds>] [--times] <audio.wav>...\n"
			"       %s mel <model directory> <audio.wav> <first sample> "
			"<output>\n"
			"       %s convert <format> <channels> <rate> <target rate> "
			"<chunk bytes> <input> <output>\n"
			"       %s text <model directory> <token>...\n", argv[0],
			argv[0], argv[0], argv[0]);
	}
	return result;
}
