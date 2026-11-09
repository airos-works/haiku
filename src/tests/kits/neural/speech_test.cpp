/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Tests the speech recognizer of the Neural Kit.

		speech_test [<model> [<recording.wav>]]

	Without arguments nothing but the kit is needed: the front end is
	checked against reference values, the audio converter against tones
	whose result is known, the tokenizer and the reading of model
	directories against a small model made up here, and the decoding
	(the rules for tokens, the phrases and their times, what is kept from
	step to step) against networks made up here that follow a script.

	BSpeechRecognizer itself needs a real model: the one that is
	installed, or <model> (a name or a directory). Its messages, the
	results that are not final, cancelling, deleting it in the middle of
	its work and Transcribe() are tried on the recording if one is given
	(it must have speech in it), else on a tone, which only shows that
	everything comes to an end. Without a model that part is skipped.

	Exits with 0 when every check passed.
*/


#include <math.h>
#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Looper.h>
#include <Message.h>
#include <NeuralKit.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>

#include <SpeechCore.h>


using namespace BPrivate;


static int sFailures;


static void
check(bool ok, const char* format, ...)
{
	char what[256];
	va_list args;
	va_start(args, format);
	vsnprintf(what, sizeof(what), format, args);
	va_end(args);

	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	if (!ok)
		sFailures++;
}


// #pragma mark - the front end


// Whisper's front end, as every model so far has it
static const int32 kSampleRate = 16000;
static const int32 kFFTSize = 400;
static const int32 kHop = 160;
static const int32 kMelBins = 80;
static const int32 kFrames = 3000;
static const int32 kBins = kFFTSize / 2 + 1;


/*!	The mel filter bank the models were trained with: triangles evenly
	spaced on Slaney's mel scale, each of unit area (librosa's
	filters.mel(sr = 16000, n_fft = 400, n_mels = 80)). */
static void
make_filters(float* filters)
{
	const double kLinearStep = 200.0 / 3;
	const double kLogStep = log(6.4) / 27;

	double top = 15 + log(kSampleRate / 2 / 1000.0) / kLogStep;
	double edges[kMelBins + 2];
	for (int32 i = 0; i < kMelBins + 2; i++) {
		double mel = top * i / (kMelBins + 1);
		edges[i] = mel >= 15 ? 1000 * exp(kLogStep * (mel - 15))
			: mel * kLinearStep;
	}

	for (int32 i = 0; i < kMelBins; i++) {
		double norm = 2 / (edges[i + 2] - edges[i]);
		for (int32 j = 0; j < kBins; j++) {
			double frequency = (double)j * kSampleRate / kFFTSize;
			double rising = (frequency - edges[i]) / (edges[i + 1] - edges[i]);
			double falling = (edges[i + 2] - frequency)
				/ (edges[i + 2] - edges[i + 1]);
			double weight = rising < falling ? rising : falling;
			filters[i * kBins + j] = (float)(weight > 0 ? weight * norm : 0);
		}
	}
}


/*!	Three seconds of two tones and some noise; the reference made the
	same. */
static void
make_signal(float* samples, int32 count)
{
	uint32 state = 12345;
	for (int32 n = 0; n < count; n++) {
		state = state * 1664525 + 1013904223;
		double noise = ((state >> 16) / 32768.0 - 1.0) * 0.02;
		samples[n] = (float)(0.4 * sin(2 * M_PI * 440.0 * n / kSampleRate)
			+ 0.2 * sin(2 * M_PI * 1875.0 * n / kSampleRate + 0.5) + noise);
	}
}


// What a NumPy version of Whisper's log_mel_spectrogram() gives for that
// signal, and what Whisper's own mel_filters.npz has: printed by
//	src/tests/kits/neural/host/speech_reference.py --test-vectors
// with the model whisper-tiny.en.

static const struct {
	int32	first;
	int32	filter;
	int32	frame;
	float	value;
} kSpectrogram[] = {
	{ 0, 0, 0, 0.9604530f },
	{ 0, 0, 1, 0.4950397f },
	{ 0, 0, 57, 0.2754994f },
	{ 0, 0, 150, 0.2477030f },
	{ 0, 0, 299, 0.2844014f },
	{ 0, 0, 301, 0.2969806f },
	{ 0, 0, 2999, -0.6081343f },
	{ 0, 9, 0, 1.2643716f },
	{ 0, 9, 1, 1.1170585f },
	{ 0, 9, 57, 1.1097512f },
	{ 0, 9, 150, 1.1082586f },
	{ 0, 9, 299, 1.1135033f },
	{ 0, 9, 301, 0.3255042f },
	{ 0, 9, 2999, -0.6081343f },
	{ 0, 24, 0, 0.5460581f },
	{ 0, 24, 1, 0.1277741f },
	{ 0, 24, 57, 0.0044917f },
	{ 0, 24, 150, 0.1445119f },
	{ 0, 24, 299, 0.2994997f },
	{ 0, 24, 301, -0.0831084f },
	{ 0, 24, 2999, -0.6081343f },
	{ 0, 33, 0, 0.3311044f },
	{ 0, 33, 1, 0.1713693f },
	{ 0, 33, 57, 0.1473386f },
	{ 0, 33, 150, 0.2890126f },
	{ 0, 33, 299, 0.1625856f },
	{ 0, 33, 301, -0.1335569f },
	{ 0, 33, 2999, -0.6081343f },
	{ 0, 50, 0, 0.5875318f },
	{ 0, 50, 1, 0.3027864f },
	{ 0, 50, 57, 0.1520329f },
	{ 0, 50, 150, 0.0479609f },
	{ 0, 50, 299, 0.1557674f },
	{ 0, 50, 301, -0.1279311f },
	{ 0, 50, 2999, -0.6081343f },
	{ 0, 79, 0, -0.0354978f },
	{ 0, 79, 1, 0.1539098f },
	{ 0, 79, 57, 0.1576120f },
	{ 0, 79, 150, 0.1316478f },
	{ 0, 79, 299, 0.1568192f },
	{ 0, 79, 301, -0.5981941f },
	{ 0, 79, 2999, -0.6081343f },
	{ 8000, 0, 0, 0.1233121f },
	{ 8000, 0, 1, 0.2411791f },
	{ 8000, 0, 57, 0.1610403f },
	{ 8000, 0, 150, 0.1225860f },
	{ 8000, 0, 299, -0.6081343f },
	{ 8000, 0, 301, -0.6081343f },
	{ 8000, 0, 2999, -0.6081343f },
	{ 8000, 9, 0, 1.1122494f },
	{ 8000, 9, 1, 1.1104070f },
	{ 8000, 9, 57, 1.1112419f },
	{ 8000, 9, 150, 1.1105367f },
	{ 8000, 9, 299, -0.6081343f },
	{ 8000, 9, 301, -0.6081343f },
	{ 8000, 9, 2999, -0.6081343f },
	{ 8000, 24, 0, 0.2251805f },
	{ 8000, 24, 1, 0.2059012f },
	{ 8000, 24, 57, 0.2255795f },
	{ 8000, 24, 150, 0.1285716f },
	{ 8000, 24, 299, -0.6081343f },
	{ 8000, 24, 301, -0.6081343f },
	{ 8000, 24, 2999, -0.6081343f },
	{ 8000, 33, 0, 0.1239128f },
	{ 8000, 33, 1, 0.0963950f },
	{ 8000, 33, 57, 0.1598582f },
	{ 8000, 33, 150, 0.3291030f },
	{ 8000, 33, 299, -0.6081343f },
	{ 8000, 33, 301, -0.6081343f },
	{ 8000, 33, 2999, -0.6081343f },
	{ 8000, 50, 0, 0.0989025f },
	{ 8000, 50, 1, 0.1423489f },
	{ 8000, 50, 57, 0.2110135f },
	{ 8000, 50, 150, 0.0012846f },
	{ 8000, 50, 299, -0.6081343f },
	{ 8000, 50, 301, -0.6081343f },
	{ 8000, 50, 2999, -0.6081343f },
	{ 8000, 79, 0, 0.1294589f },
	{ 8000, 79, 1, 0.1162115f },
	{ 8000, 79, 57, 0.1518770f },
	{ 8000, 79, 150, 0.2067661f },
	{ 8000, 79, 299, -0.6081343f },
	{ 8000, 79, 301, -0.6081343f },
	{ 8000, 79, 2999, -0.6081343f },
};

static const struct {
	int32	filter;
	int32	bin;
	float	weight;
} kFilterWeights[] = {
	{ 0, 0, -0.000000000f },
	{ 0, 1, 0.024862595f },
	{ 0, 2, 0.000000000f },
	{ 5, 5, 0.009954110f },
	{ 5, 6, 0.014908484f },
	{ 5, 7, 0.000000000f },
	{ 20, 19, 0.010972200f },
	{ 20, 20, 0.013890394f },
	{ 20, 21, 0.000000000f },
	{ 40, 42, 0.005411105f },
	{ 40, 43, 0.014735566f },
	{ 40, 44, 0.006518190f },
	{ 60, 92, 0.005447904f },
	{ 60, 93, 0.006591093f },
	{ 60, 94, 0.004660012f },
	{ 79, 191, 0.002698383f },
	{ 79, 192, 0.003164712f },
	{ 79, 193, 0.003141313f },
};


static void
test_front_end()
{
	printf("-- front end\n");

	float* filters = (float*)malloc(kMelBins * kBins * sizeof(float));
	make_filters(filters);
	float worst = 0;
	for (size_t i = 0; i < B_COUNT_OF(kFilterWeights); i++) {
		float difference = fabsf(filters[kFilterWeights[i].filter * kBins
			+ kFilterWeights[i].bin] - kFilterWeights[i].weight);
		worst = difference > worst ? difference : worst;
	}
	check(worst < 1e-7f, "the filter bank is Whisper's (off by %.1e)", worst);

	int32 count = 3 * kSampleRate;
	float* samples = (float*)malloc(count * sizeof(float));
	make_signal(samples, count);

	SpeechFrontEnd frontEnd;
	float* mel = (float*)malloc((size_t)kMelBins * kFrames * sizeof(float));
	check(frontEnd.Init(kFFTSize, kHop, kMelBins, kFrames, filters) == B_OK,
		"front end set up");

	for (int32 pass = 0; pass < 2; pass++) {
		int32 first = pass == 0 ? 0 : 8000;
		status_t status = frontEnd.Compute(samples, count, 0, first, mel);
		worst = 0;
		for (size_t i = 0; i < B_COUNT_OF(kSpectrogram); i++) {
			if (kSpectrogram[i].first != first)
				continue;
			float difference = fabsf(mel[(size_t)kSpectrogram[i].filter
				* kFrames + kSpectrogram[i].frame] - kSpectrogram[i].value);
			worst = difference > worst ? difference : worst;
		}
		check(status == B_OK && worst < 2e-4f, "spectrogram of the window "
			"from sample %d agrees with the reference (off by %.1e)",
			(int)first, worst);
	}

	// The same window from audio of which the beginning is gone, as the
	// engine keeps it: only what the transforms reach back to is needed.
	float* later = (float*)malloc((size_t)kMelBins * kFrames * sizeof(float));
	frontEnd.Compute(samples + 8000 - kFFTSize, count - 8000 + kFFTSize,
		8000 - kFFTSize, 8000, later);
	check(memcmp(mel, later, (size_t)kMelBins * kFrames * sizeof(float)) == 0,
		"a window does not need the audio before it");

	check(frontEnd.Compute(samples, count, 0, -1, mel) == B_BAD_VALUE,
		"a window before the recording is refused");
	SpeechFrontEnd odd;
	check(odd.Init(401, kHop, kMelBins, kFrames, filters) != B_OK
		&& odd.Init(kFFTSize, 0, kMelBins, kFrames, filters) != B_OK,
		"sizes that make no sense are refused");

	free(later);
	free(mel);
	free(samples);
	free(filters);
}


// #pragma mark - the converter


/*!	Tones a 16 kHz recording can hold and, at rates that can, one it
	cannot, put through the converter: the first must come out as they
	are, the other not at all. */
static void
test_conversion(const char* name, uint32 format, int32 channels, int32 rate,
	float tolerance)
{
	const double kTones[3][2] = { { 300, 0.25 }, { 1000, 0.2 },
		{ 3100, 0.15 } };
	int32 frames = rate * 3 / 2;
	size_t sampleSize = format & 0xf;
	uint8* raw = (uint8*)malloc((size_t)frames * channels * sampleSize);

	for (int32 n = 0; n < frames; n++) {
		double value = 0;
		for (int32 i = 0; i < 3; i++)
			value += kTones[i][1] * sin(2 * M_PI * kTones[i][0] * n / rate);
		if (rate > 24000)
			value += 0.1 * sin(2 * M_PI * 11000.0 * n / rate);

		for (int32 channel = 0; channel < channels; channel++) {
			// the channels differ, and their mean is the signal
			double sample = value;
			if (channel < (channels & ~1))
				sample *= (channel & 1) != 0 ? 0.7 : 1.3;
			uint8* target = raw + ((size_t)n * channels + channel)
				* sampleSize;
			switch (format) {
				case B_SPEECH_SAMPLE_UINT8:
					*target = (uint8)lround(sample * 127 + 128);
					break;
				case B_SPEECH_SAMPLE_INT16:
				{
					int16 stored = (int16)lround(sample * 32767);
					memcpy(target, &stored, sizeof(stored));
					break;
				}
				case B_SPEECH_SAMPLE_INT32:
				{
					int32 stored = (int32)lround(sample * 2147483000.0);
					memcpy(target, &stored, sizeof(stored));
					break;
				}
				default:
				{
					float stored = (float)sample;
					memcpy(target, &stored, sizeof(stored));
					break;
				}
			}
		}
	}

	size_t size = (size_t)frames * channels * sampleSize;
	SpeechSamples whole;
	SpeechSamples pieces;
	SpeechAudioConverter converter;
	status_t status = converter.SetFormat(format, channels, rate, false,
		kSampleRate);
	if (status == B_OK)
		status = converter.Convert(raw, size, whole);
	if (status == B_OK)
		status = converter.Flush(whole);

	// and in pieces that end in the middle of frames
	converter.Reset();
	for (size_t at = 0; status == B_OK && at < size; at += 7) {
		status = converter.Convert(raw + at, size - at < 7 ? size - at : 7,
			pieces);
	}
	if (status == B_OK)
		status = converter.Flush(pieces);

	// the ends, where the filter runs into silence, are left out
	double worst = 0;
	int32 count = whole.Count();
	for (int32 n = 1600; n < count - 1600; n++) {
		double expected = 0;
		for (int32 i = 0; i < 3; i++) {
			expected += kTones[i][1]
				* sin(2 * M_PI * kTones[i][0] * n / kSampleRate);
		}
		double difference = fabs(whole.Samples()[n] - expected);
		worst = difference > worst ? difference : worst;
	}

	int64 expectedCount = (int64)frames * kSampleRate / rate;
	check(status == B_OK && llabs(count - expectedCount) <= 1
		&& worst < tolerance, "%s: %d samples, off by %.1e", name,
		(int)count, worst);
	check(pieces.Count() == whole.Count() && memcmp(pieces.Samples(),
			whole.Samples(), whole.Count() * sizeof(float)) == 0,
		"%s: the same in pieces of 7 bytes", name);
	free(raw);
}


static void
test_converter()
{
	printf("-- audio converter\n");
	test_conversion("float, 1 channel, 16000 Hz", B_SPEECH_SAMPLE_FLOAT, 1,
		16000, 1e-6f);
	test_conversion("16 bit, 2 channels, 44100 Hz", B_SPEECH_SAMPLE_INT16, 2,
		44100, 2e-3f);
	test_conversion("float, 2 channels, 48000 Hz", B_SPEECH_SAMPLE_FLOAT, 2,
		48000, 2e-3f);
	test_conversion("16 bit, 1 channel, 8000 Hz", B_SPEECH_SAMPLE_INT16, 1,
		8000, 2e-3f);
	test_conversion("8 bit, 1 channel, 22050 Hz", B_SPEECH_SAMPLE_UINT8, 1,
		22050, 1.2e-2f);
	test_conversion("32 bit, 6 channels, 48000 Hz", B_SPEECH_SAMPLE_INT32, 6,
		48000, 2e-3f);

	SpeechAudioConverter converter;
	check(converter.SetFormat(0x3, 1, 44100, false, kSampleRate) != B_OK
		&& converter.SetFormat(B_SPEECH_SAMPLE_INT16, 0, 44100, false,
			kSampleRate) != B_OK
		&& converter.SetFormat(B_SPEECH_SAMPLE_INT16, 2, 0, false,
			kSampleRate) != B_OK,
		"formats that make no sense are refused");

	// bytes swapped
	int16 swapped[4] = { 0x0040, (int16)0x00c0, 0x0040, (int16)0x00c0 };
	SpeechSamples samples;
	converter.SetFormat(B_SPEECH_SAMPLE_INT16, 1, kSampleRate, true,
		kSampleRate);
	converter.Convert(swapped, sizeof(swapped), samples);
	check(samples.Count() == 4 && samples.Samples()[0] == 0.5f
		&& samples.Samples()[1] == -0.5f, "samples of the other byte order");
}


// #pragma mark - a model made up here


// The text tokens of the made-up model; the special ones follow.
static const char* const kTokens[] = {
	" one", " two", " three", ",", ".", " ", "(", "\xc3", "\xa9",
	"\xe2\x80", "\x9c", " caf"
};
enum {
	kOne = 0, kTwo, kThree, kComma, kStop, kBlank, kBracket, kLead, kTail,
	kQuoteLead, kQuoteTail, kCaf,
	kTextTokens,

	kEot = kTextTokens,
	kSot,
	kTranslate,
	kTranscribe,
	kNoSpeech,
	kNoTimestamps,
	kFirstStamp,
	kVocabulary = kFirstStamp + 1501
};
static const int32 kContext = 32;


static bool
write_file(const char* directory, const char* name, const void* data,
	size_t size)
{
	BString path(directory);
	path << "/" << name;
	FILE* file = fopen(path.String(), "wb");
	if (file == NULL)
		return false;
	bool ok = fwrite(data, 1, size, file) == size;
	fclose(file);
	return ok;
}


/*!	Writes the description of the made-up model; \a change replaces the
	line that begins with its first word (or is added). */
static void
write_description(const char* directory, const char* change = NULL)
{
	BString text;
	text.SetToFormat(
		"# a model made up by speech_test\n"
		"family whisper\n"
		"name test\n"
		"encoder encoder.tflite\n"
		"decoder decoder.tflite\n"
		"vocabulary vocabulary.bin\n"
		"mel_filters mel_filters.bin\n"
		"sample_rate %d\nfft_size %d\nhop %d\nmel_bins %d\n"
		"window_frames %d\n"
		"text_context %d\nmax_tokens 20\ntimestamp_ms 20\n"
		"vocabulary_size %d\ntoken_eot %d\ntoken_sot %d\n"
		"token_translate %d\ntoken_transcribe %d\ntoken_no_speech %d\n"
		"token_no_timestamps %d\ntoken_timestamp_begin %d\n"
		"token_blank %d\nmultilingual 0\n"
		"suppress %d %d   # a bracket, and what belongs into the prompt\n"
		"suppress %d %d %d\n",
		(int)kSampleRate, (int)kFFTSize, (int)kHop, (int)kMelBins,
		(int)kFrames, (int)kContext, (int)kVocabulary, (int)kEot, (int)kSot,
		(int)kTranslate, (int)kTranscribe, (int)kNoSpeech,
		(int)kNoTimestamps, (int)kFirstStamp, (int)kBlank, (int)kBracket,
		(int)kSot, (int)kTranslate, (int)kTranscribe, (int)kNoSpeech);

	if (change != NULL) {
		BString key(change);
		key.Truncate(key.FindFirst(' ') + 1);
		int32 at = text.FindFirst(BString("\n") << key);
		if (at >= 0) {
			int32 end = text.FindFirst('\n', at + 1);
			text.Remove(at + 1, end - at);
		}
		text << change << "\n";
	}
	write_file(directory, "speech.conf", text.String(), text.Length());
}


static void
write_model(const char* directory)
{
	write_description(directory);

	BString vocabulary;
	for (int32 i = 0; i < kTextTokens; i++) {
		vocabulary.Append((char)strlen(kTokens[i]), 1);
		vocabulary << kTokens[i];
	}
	write_file(directory, "vocabulary.bin", vocabulary.String(),
		vocabulary.Length());

	float* filters = (float*)malloc(kMelBins * kBins * sizeof(float));
	make_filters(filters);
	write_file(directory, "mel_filters.bin", filters,
		kMelBins * kBins * sizeof(float));
	free(filters);
}


static void
test_model_directory(const char* directory)
{
	printf("-- model directory, tokenizer\n");

	SpeechModel model;
	status_t status = model.Load(directory);
	check(status == B_OK, "the made-up model loads (%s)", model.Error());
	if (status != B_OK)
		return;
	check(model.Info().token_eot == kEot && model.Info().hop == kHop
		&& model.CountSuppressed() == 5 && model.SuppressedAt(0) == kBracket
		&& !model.Info().multilingual && model.LanguageToken("en") == -1,
		"its description is read");

	static const struct {
		int32		tokens[8];
		int32		count;
		const char*	text;
	} kTexts[] = {
		{ { kOne, kComma, kTwo, kStop }, 4, " one, two." },
		// special tokens and time stamps are no text
		{ { kSot, kFirstStamp, kThree, kFirstStamp + 400, kEot }, 5,
			" three" },
		// a character of two bytes, whole and cut at either end
		{ { kCaf, kLead, kTail }, 3, " caf\xc3\xa9" },
		{ { kCaf, kLead }, 2, " caf\xef\xbf\xbd" },
		{ { kTail, kOne }, 2, "\xef\xbf\xbd one" },
		// one of three bytes; cut after two it is one replacement
		{ { kQuoteLead, kQuoteTail, kStop, kQuoteLead }, 4,
			"\xe2\x80\x9c.\xef\xbf\xbd" },
		{ { kQuoteLead, kQuoteLead, kQuoteTail }, 3,
			"\xef\xbf\xbd\xe2\x80\x9c" },
		{ { -5, kVocabulary + 7 }, 2, "" }
	};
	for (size_t i = 0; i < B_COUNT_OF(kTexts); i++) {
		char* text = NULL;
		status = model.DecodeTokens(kTexts[i].tokens, kTexts[i].count, &text);
		check(status == B_OK && strcmp(text, kTexts[i].text) == 0,
			"%d tokens give \"%s\"", (int)kTexts[i].count, kTexts[i].text);
		free(text);
	}

	// descriptions and files that must not be taken
	static const char* const kBadLines[] = {
		"hop 0", "hop 401", "fft_size 401", "sample_rate 16000000",
		"mel_bins x", "token_eot 5000", "token_timestamp_begin 3",
		"max_tokens 31", "vocabulary ../vocabulary.bin",
		"encoder /boot/home/encoder.tflite", "suppress 99999",
		"token_blank 500", "vocabulary_size 15"
	};
	bool refused = true;
	for (size_t i = 0; i < B_COUNT_OF(kBadLines); i++) {
		write_description(directory, kBadLines[i]);
		SpeechModel bad;
		if (bad.Load(directory) == B_OK) {
			printf("   taken: %s\n", kBadLines[i]);
			refused = false;
		}
	}
	check(refused, "descriptions with numbers that do not go together are "
		"refused");

	write_description(directory, "token_eot 11");
		// one token less than the vocabulary file has
	SpeechModel shorter;
	bool vocabularyRefused = shorter.Load(directory) != B_OK;
	write_description(directory);
	write_file(directory, "mel_filters.bin", "short", 5);
	SpeechModel noFilters;
	check(vocabularyRefused && noFilters.Load(directory) != B_OK,
		"a vocabulary or filter bank of the wrong size is refused");

	SpeechModel nowhere;
	check(nowhere.Load("/nonexistent/model") != B_OK,
		"a directory that is not there is refused");

	write_model(directory);
}


// #pragma mark - networks that follow a script


/*!	An encoder that computes nothing much: its one output says how loud
	the spectrogram is, so that the decoder can tell windows apart. */
class ScriptedEncoder : public SpeechNetwork {
public:
	ScriptedEncoder()
		:
		fRuns(0)
	{
		fMel = (float*)calloc((size_t)kMelBins * kFrames, sizeof(float));
	}

	virtual ~ScriptedEncoder()
	{
		free(fMel);
	}

	virtual int32 CountInputs() const { return 1; }
	virtual int32 CountOutputs() const { return 1; }

	virtual status_t GetInput(int32 index, speech_tensor* tensor)
	{
		memset(tensor, 0, sizeof(*tensor));
		tensor->name = "mel";
		tensor->type = B_NEURAL_FLOAT32;
		tensor->rank = 3;
		tensor->dimensions[0] = 1;
		tensor->dimensions[1] = kMelBins;
		tensor->dimensions[2] = kFrames;
		tensor->data = fMel;
		tensor->size = (size_t)kMelBins * kFrames * sizeof(float);
		return B_OK;
	}

	virtual status_t GetOutput(int32 index, speech_tensor* tensor)
	{
		memset(tensor, 0, sizeof(*tensor));
		tensor->name = "audio";
		tensor->type = B_NEURAL_FLOAT32;
		tensor->rank = 1;
		tensor->dimensions[0] = 4;
		tensor->data = fAudio;
		tensor->size = sizeof(fAudio);
		return B_OK;
	}

	virtual status_t Run()
	{
		fRuns++;
		double sum = 0;
		for (int32 i = 0; i < kMelBins * kFrames; i++)
			sum += fMel[i];
		fAudio[0] = (float)(sum / (kMelBins * kFrames));
		return B_OK;
	}

	float*	fMel;
	float	fAudio[4];
	int32	fRuns;
};


/*!	A decoder that wants to say what its script says, token by token:
	the scripted token gets the highest score, a token that the rules
	must not let through (if the script names one for the step) a higher
	one still. It also checks what the real decoder relies on: that each
	step finds what the steps before left, at their positions. */
class ScriptedDecoder : public SpeechNetwork {
public:
	ScriptedDecoder()
		:
		fScriptCount(0),
		fWindow(-1),
		fPrompt(1),
		fSteps(0),
		fStateErrors(0),
		fNoSpeech(false),
		fDelay(0)
	{
		fLogits = (float*)calloc(kVocabulary, sizeof(float));
		memset(fState, 0, sizeof(fState));
	}

	virtual ~ScriptedDecoder()
	{
		free(fLogits);
	}

	/*!	\a script: what follows the prompt of \a prompt tokens;
		\a tempting: per step a token to score even higher, or -1. Each
		call adds the script of one more window; the last one is that of
		all further windows too. */
	void AddScript(const int32* script, const int32* tempting, int32 length,
		int32 prompt)
	{
		fScripts[fScriptCount] = script;
		fTemptings[fScriptCount] = tempting;
		fLengths[fScriptCount++] = length;
		fPrompt = prompt;
	}

	void ClearScripts()
	{
		fScriptCount = 0;
		fWindow = -1;
	}

	virtual int32 CountInputs() const { return 4; }
	virtual int32 CountOutputs() const { return 2; }

	virtual status_t GetInput(int32 index, speech_tensor* tensor)
	{
		memset(tensor, 0, sizeof(*tensor));
		tensor->type = B_NEURAL_FLOAT32;
		switch (index) {
			case 0:
				tensor->name = "token";
				tensor->type = B_NEURAL_INT32;
				tensor->rank = 1;
				tensor->dimensions[0] = 1;
				tensor->data = &fToken;
				tensor->size = sizeof(fToken);
				break;
			case 1:
				tensor->name = "position";
				tensor->type = B_NEURAL_INT32;
				tensor->rank = 1;
				tensor->dimensions[0] = 1;
				tensor->data = &fPosition;
				tensor->size = sizeof(fPosition);
				break;
			case 2:
				// two numbers per position, the positions in the middle
				tensor->name = "self_state";
				tensor->rank = 3;
				tensor->dimensions[0] = 2;
				tensor->dimensions[1] = kContext;
				tensor->dimensions[2] = 1;
				tensor->data = fState;
				tensor->size = sizeof(fState);
				break;
			default:
				tensor->name = "audio";
				tensor->rank = 1;
				tensor->dimensions[0] = 4;
				tensor->data = fAudio;
				tensor->size = sizeof(fAudio);
				break;
		}
		return B_OK;
	}

	virtual status_t GetOutput(int32 index, speech_tensor* tensor)
	{
		memset(tensor, 0, sizeof(*tensor));
		tensor->type = B_NEURAL_FLOAT32;
		if (index == 0) {
			tensor->name = "logits";
			tensor->rank = 2;
			tensor->dimensions[0] = 1;
			tensor->dimensions[1] = kVocabulary;
			tensor->data = fLogits;
			tensor->size = kVocabulary * sizeof(float);
		} else {
			tensor->name = "new_state";
			tensor->rank = 3;
			tensor->dimensions[0] = 2;
			tensor->dimensions[1] = 1;
			tensor->dimensions[2] = 1;
			tensor->data = fFresh;
			tensor->size = sizeof(fFresh);
		}
		return B_OK;
	}

	virtual status_t Run()
	{
		fSteps++;
		if (fDelay > 0)
			snooze(fDelay);
		if (fPosition < 0 || fPosition >= kContext || fScriptCount == 0)
			return B_BAD_INDEX;

		// a window begins at the first position
		if (fPosition == 0)
			fWindow++;
		int32 window = fWindow < fScriptCount ? fWindow : fScriptCount - 1;
		const int32* script = fScripts[window];
		const int32* tempting = fTemptings[window];

		// what the steps before left: their token, and their position
		for (int32 i = 0; i < fPosition; i++) {
			if (fState[0][i] != (float)fSeen[i]
				|| fState[1][i] != (float)(i + 100)) {
				fStateErrors++;
			}
		}
		fSeen[fPosition] = fToken;
		fFresh[0] = (float)fToken;
		fFresh[1] = (float)(fPosition + 100);

		for (int32 i = 0; i < kVocabulary; i++)
			fLogits[i] = -8;
		if (fToken == kSot)
			fLogits[kNoSpeech] = fNoSpeech ? 8 : -20;

		int32 step = fPosition - fPrompt + 1;
		if (step >= 0 && step < fLengths[window]) {
			// (the scores of a model that is sure of the script, or, for
			// silence, of one that hardly prefers it to anything else)
			fLogits[script[step]] = fNoSpeech ? -7.9f : 5;
			if (tempting != NULL && tempting[step] >= 0)
				fLogits[tempting[step]] = 9;
		} else
			fLogits[kEot] = 5;
		return B_OK;
	}

	int32			fToken;
	int32			fPosition;
	float			fState[2][kContext];
	float			fFresh[2];
	float			fAudio[4];
	float*			fLogits;
	int32			fSeen[kContext];
	const int32*	fScripts[4];
	const int32*	fTemptings[4];
	int32			fLengths[4];
	int32			fScriptCount;
	int32			fWindow;
	int32			fPrompt;
	int32			fSteps;
	int32			fStateErrors;
	bool			fNoSpeech;
	bigtime_t		fDelay;
};


class Collector : public SpeechListener {
public:
	virtual void SegmentRecognized(bigtime_t start, bigtime_t end,
		const char* text, bool final)
	{
		BString line;
		line.SetToFormat("%s[%.2f-%.2f]%s", final ? "" : "~", start / 1e6,
			end / 1e6, text);
		fLines.Add(line);
	}

	BString Joined() const
	{
		return fLines.Join(" ");
	}

	BStringList	fLines;
};


static void
test_decoding(const char* directory)
{
	printf("-- decoding\n");

	SpeechModel model;
	if (model.Load(directory) != B_OK) {
		check(false, "the made-up model loads");
		return;
	}

	// 12 seconds of something
	int32 count = 12 * kSampleRate;
	float* samples = (float*)malloc(count * sizeof(float));
	make_signal(samples, count);

	// Two phrases with their times, and the beginning of a third, which
	// the end of the window cut off: the model would hear it again from
	// 6.00 on.
	static const int32 kScript[] = {
		kFirstStamp, kOne, kComma, kTwo, kFirstStamp + 150,
		kFirstStamp + 150, kThree, kStop, kFirstStamp + 300,
		kFirstStamp + 300, kOne, kEot
	};
	// what the model would rather say, and must not: text before the
	// first time stamp, a suppressed token, a time stamp that goes back,
	// text right after a single time stamp, three time stamps in a row
	static const int32 kTempting[] = {
		kOne, kBracket, -1, kSot, kFirstStamp, kThree, -1, kNoTimestamps,
		-1, kOne, kFirstStamp + 400, -1
	};

	{
		// what the window after that says, which begins at 6.00: one
		// phrase without a pair of time stamps
		static const int32 kSecond[] = { kFirstStamp + 10, kTwo, kStop,
			kFirstStamp + 40, kEot };

		ScriptedEncoder encoder;
		ScriptedDecoder decoder;
		// (the first window is read twice: as it fills, and for good)
		decoder.AddScript(kScript, kTempting, B_COUNT_OF(kScript), 1);
		decoder.AddScript(kScript, kTempting, B_COUNT_OF(kScript), 1);
		decoder.AddScript(kSecond, NULL, B_COUNT_OF(kSecond), 1);
		SpeechEngine engine;
		Collector collector;
		status_t status = engine.Init(&model, &encoder, &decoder);
		check(status == B_OK, "engine set up with scripted networks (%s)",
			engine.Error());
		engine.SetListener(&collector);

		// one window only: as if the recording went on
		engine.AddSamples(samples, 7 * kSampleRate);
		status = engine.ProcessInterim();
		check(status == B_OK && collector.Joined()
				== "~[0.00-3.00] one, two ~[3.00-6.00] three.",
			"phrases and times from the time stamps, the rules kept: %s",
			collector.Joined().String());
		check(decoder.fStateErrors == 0 && decoder.fSteps == 12,
			"each step finds what the steps before left (%d steps)",
			(int)decoder.fSteps);
		check(fabs(engine.PendingSeconds() - 7) < 0.01,
			"a look at the window does not use it up");

		// The recording ends: the second window begins where the last
		// whole phrase ended, at 6 seconds.
		collector.fLines.MakeEmpty();
		status = engine.Process(false);
		check(status == B_OK && collector.fLines.IsEmpty(),
			"a window that is not full waits for more audio");
		status = engine.Process(true);
		check(status == B_OK && engine.Info().windows == 3
			&& collector.Joined() == "[0.00-3.00] one, two "
				"[3.00-6.00] three. [6.00-6.80] two.",
			"the next window begins where the last whole phrase ended: %s",
			collector.Joined().String());
		check(engine.PendingSeconds() == 0 && decoder.fStateErrors == 0,
			"nothing is left at the end");
	}

	{
		// without time stamps a window is one phrase; fed bit by bit
		static const int32 kPlain[] = { kOne, kTwo, kStop, kEot };
		static const int32 kTemptingStamps[] = { kFirstStamp + 5, kNoSpeech,
			kFirstStamp + 100, -1 };
		ScriptedEncoder encoder;
		ScriptedDecoder decoder;
		decoder.AddScript(kPlain, kTemptingStamps, B_COUNT_OF(kPlain), 2);
		SpeechEngine engine;
		Collector collector;
		engine.Init(&model, &encoder, &decoder);
		engine.SetListener(&collector);
		engine.SetTimestamps(false);

		status_t status = B_OK;
		for (int32 at = 0; status == B_OK && at < count; at += 3000) {
			status = engine.AddSamples(samples + at,
				count - at < 3000 ? count - at : 3000);
			if (status == B_OK)
				status = engine.Process(false);
		}
		if (status == B_OK)
			status = engine.Process(true);
		check(status == B_OK
				&& collector.Joined() == "[0.00-12.00] one two.",
			"without time stamps: %s", collector.Joined().String());

		// " " may not begin a window (the first token that may takes its
		// place: " one"), and may be in it
		static const int32 kBlanks[] = { kBlank, kTwo, kBlank, kEot };
		decoder.ClearScripts();
		decoder.AddScript(kBlanks, NULL, B_COUNT_OF(kBlanks), 2);
		collector.fLines.MakeEmpty();
		engine.Reset();
		engine.AddSamples(samples, count);
		status = engine.Process(true);
		check(status == B_OK
				&& collector.Joined() == "[0.00-12.00] one two ",
			"no space at the beginning of a window: \"%s\"",
			collector.Joined().String());
	}

	{
		// silence: the model says so and is unsure of its text
		ScriptedEncoder encoder;
		ScriptedDecoder decoder;
		decoder.AddScript(kScript, NULL, B_COUNT_OF(kScript), 1);
		decoder.fNoSpeech = true;
		SpeechEngine engine;
		Collector collector;
		engine.Init(&model, &encoder, &decoder);
		engine.SetListener(&collector);
		engine.AddSamples(samples, count);
		status_t status = engine.Process(true);
		check(status == B_OK && collector.fLines.IsEmpty()
			&& engine.Info().windows == 1,
			"a window the model takes for silence gives no text");

		// cancelled from another thread, here: before it began
		int32 cancel = 1;
		engine.SetCancelFlag(&cancel);
		engine.Reset();
		engine.AddSamples(samples, count);
		check(engine.Process(true) == B_CANCELED, "the cancel flag stops it");
	}

	{
		// networks that are not what the description says
		struct WrongLogits : ScriptedDecoder {
			virtual status_t GetOutput(int32 index, speech_tensor* tensor)
			{
				status_t status = ScriptedDecoder::GetOutput(index, tensor);
				if (index == 0)
					tensor->size -= sizeof(float);
				return status;
			}
		};
		struct WrongState : ScriptedDecoder {
			virtual status_t GetInput(int32 index, speech_tensor* tensor)
			{
				status_t status = ScriptedDecoder::GetInput(index, tensor);
				if (index == 2)
					tensor->dimensions[1] = kContext - 1;
				return status;
			}
		};
		struct WrongLink : ScriptedDecoder {
			virtual status_t GetInput(int32 index, speech_tensor* tensor)
			{
				status_t status = ScriptedDecoder::GetInput(index, tensor);
				if (index == 3)
					tensor->name = "something";
				return status;
			}
		};

		ScriptedEncoder encoder;
		WrongLogits wrongLogits;
		WrongState wrongState;
		WrongLink wrongLink;
		SpeechEngine first;
		SpeechEngine second;
		SpeechEngine third;
		check(first.Init(&model, &encoder, &wrongLogits) != B_OK
			&& second.Init(&model, &encoder, &wrongState) != B_OK
			&& third.Init(&model, &encoder, &wrongLink) != B_OK,
			"networks that do not fit the description are refused (%s)",
			third.Error());
	}

	free(samples);
}


// #pragma mark - BSpeechRecognizer


class Receiver : public BLooper {
public:
	Receiver()
		:
		BLooper("speech results"),
		fDone(create_sem(0, "finished")),
		fFinals(0),
		fInterims(0),
		fFinished(0),
		fStatus(B_ERROR),
		fToken(-1),
		fWrongToken(0),
		fOwn(0)
	{
	}

	virtual void MessageReceived(BMessage* message)
	{
		if (message->what == 'mine') {
			fOwn++;
			message->what = message->GetBool("speech:finished", false)
				? B_SPEECH_FINISHED : B_SPEECH_RESULT;
		}

		switch (message->what) {
			case B_SPEECH_RESULT:
				if (message->GetInt32("speech:token", -1) != fToken)
					fWrongToken++;
				if (message->GetBool("speech:final", false)) {
					fFinals++;
					if (fText.Length() > 0)
						fText << ' ';
					fText << message->GetString("speech:text", "");
					fLastEnd = message->GetInt64("speech:end", -1);
				} else {
					fInterims++;
					fLastInterim = message->GetString("speech:text", "");
				}
				break;
			case B_SPEECH_FINISHED:
				if (message->GetInt32("speech:token", -1) != fToken)
					fWrongToken++;
				fFinished++;
				fStatus = message->GetInt32("speech:status", B_ERROR);
				fAudio = message->GetInt64("speech:audio", -1);
				release_sem(fDone);
				break;
			default:
				BLooper::MessageReceived(message);
		}
	}

	void Reset(int32 token)
	{
		Lock();
		fFinals = fInterims = fFinished = fWrongToken = fOwn = 0;
		fText = "";
		fLastInterim = "";
		fStatus = B_ERROR;
		fToken = token;
		Unlock();
	}

	sem_id		fDone;
	int32		fFinals;
	int32		fInterims;
	int32		fFinished;
	status_t	fStatus;
	int32		fToken;
	int32		fWrongToken;
	int32		fOwn;
	BString		fText;
	BString		fLastInterim;
	bigtime_t	fLastEnd;
	bigtime_t	fAudio;
};


static void
test_recognizer(const char* modelName, const char* recording)
{
	printf("-- BSpeechRecognizer\n");

	BSpeechRecognizer none;
	check(none.InitCheck() == B_NO_INIT && none.Start() == B_NO_INIT
		&& none.Feed("", 0) == B_NO_INIT && none.CountSegments() == 0
		&& none.SetTo("/nonexistent/model") != B_OK
		&& none.SetTo("no-such-model") == B_ENTRY_NOT_FOUND,
		"a recognizer without a model does nothing, politely");

	BPath directory;
	if (BSpeechRecognizer::FindModel(modelName, &directory) != B_OK) {
		printf("skipped: no speech model is installed (give one as the "
			"first argument)\n");
		return;
	}
	printf("   model: %s\n", directory.Path());

	// the audio: the recording, or 6 seconds of a tone at 44.1 kHz, two
	// channels of 16 bits, which goes through the converter
	SpeechSamples samples;
	media_raw_audio_format format;
	memset(&format, 0, sizeof(format));
	int16* tone = NULL;
	size_t toneSize = 0;
	bool speech = recording != NULL;
	if (speech) {
		if (speech_read_wave_file(recording, 16000, samples) != B_OK) {
			check(false, "%s can be read", recording);
			return;
		}
	} else {
		format.frame_rate = 44100;
		format.channel_count = 2;
		format.format = media_raw_audio_format::B_AUDIO_SHORT;
		format.byte_order = B_MEDIA_HOST_ENDIAN;
		int32 frames = 6 * 44100;
		toneSize = (size_t)frames * 2 * sizeof(int16);
		tone = (int16*)malloc(toneSize);
		for (int32 n = 0; n < frames; n++) {
			tone[2 * n] = tone[2 * n + 1]
				= (int16)(8000 * sin(2 * M_PI * 440.0 * n / 44100));
		}
	}

	Receiver* receiver = new Receiver();
	receiver->Run();

	BSpeechRecognizer* recognizer = new BSpeechRecognizer();
	bigtime_t start = system_time();
	status_t status = recognizer->SetTo(directory.Path());
	check(status == B_OK, "set up in %.2f s on %s (%s)",
		(system_time() - start) / 1e6, recognizer->Device().ID(),
		recognizer->ErrorDetails());
	if (status != B_OK) {
		delete recognizer;
		receiver->Lock();
		receiver->Quit();
		free(tone);
		return;
	}

	BStringList languages;
	check(recognizer->GetLanguages(&languages) == B_OK
		&& languages.HasString("en")
		&& recognizer->SetLanguage("en") == B_OK
		&& recognizer->SetLanguage("xx") == B_NAME_NOT_FOUND
		&& recognizer->Feed("", 0) == B_NOT_ALLOWED
		&& recognizer->Finish() == B_NOT_ALLOWED,
		"languages; no audio without a recording");

	// 1. a whole recording, synchronously
	BString text;
	start = system_time();
	if (speech) {
		status = recognizer->Transcribe(samples.Samples(), samples.Count(),
			&text);
	} else
		status = recognizer->Transcribe(format, tone, toneSize, &text);
	bigtime_t took = system_time() - start;
	speech_recognizer_info info;
	recognizer->GetInfo(&info);
	check(status == B_OK && info.windows >= 1 && !recognizer->IsRecognizing()
		&& (!speech || (text.Length() > 0
			&& recognizer->CountSegments() > 0)),
		"Transcribe(): %.2f s of audio in %.2f s, %d window(s), %d "
		"phrase(s)", info.audio_duration / 1e6, took / 1e6,
		(int)info.windows, (int)recognizer->CountSegments());
	printf("   \"%s\"\n", text.String());

	speech_segment segment;
	bool ordered = true;
	bigtime_t last = 0;
	for (int32 i = 0; i < recognizer->CountSegments(); i++) {
		if (recognizer->GetSegment(i, &segment) != B_OK
			|| segment.start < last || segment.end <= segment.start
			|| segment.text.Length() == 0) {
			ordered = false;
		}
		last = segment.start;
	}
	check(ordered && recognizer->GetSegment(recognizer->CountSegments(),
			&segment) == B_BAD_INDEX, "the phrases are in order");

	// 2. the same as it comes in, with messages
	int32 token = 0;
	recognizer->SetTarget(BMessenger(receiver));
	if (speech)
		status = recognizer->Start(NULL, &token);
	else
		status = recognizer->Start(&format, &token);
	receiver->Reset(token);
	if (speech) {
		for (size_t at = 0; status == B_OK && at < samples.Count();
				at += 4096) {
			size_t count = samples.Count() - at < 4096
				? samples.Count() - at : 4096;
			status = recognizer->Feed(samples.Samples() + at,
				count * sizeof(float));
		}
	} else {
		for (size_t at = 0; status == B_OK && at < toneSize; at += 4098) {
			status = recognizer->Feed((uint8*)tone + at,
				toneSize - at < 4098 ? toneSize - at : 4098);
		}
	}
	check(status == B_OK && recognizer->IsRecognizing()
		&& recognizer->WaitUntilFinished(1000) == B_NOT_ALLOWED,
		"audio is taken at once; a recording that goes on cannot be "
		"waited for");
	status = recognizer->Finish();
	check(status == B_OK
		&& recognizer->Feed(tone, 0) == B_NOT_ALLOWED,
		"Finish(); no audio after it");

	bool arrived = acquire_sem_etc(receiver->fDone, 1, B_RELATIVE_TIMEOUT,
		300000000LL) == B_OK;
	status = recognizer->WaitUntilFinished();
	receiver->Lock();
	check(arrived && status == B_OK && receiver->fFinished == 1
		&& receiver->fStatus == B_OK && receiver->fWrongToken == 0
		&& receiver->fFinals == recognizer->CountSegments()
		&& receiver->fText == text && receiver->fAudio > 0,
		"messages: %d final result(s), the text of Transcribe(), then "
		"B_SPEECH_FINISHED", (int)receiver->fFinals);
	receiver->Unlock();

	// 3. results that are not final, and the caller's own message
	BMessage own('mine');
	own.AddString("mine", "yes");
	recognizer->SetTarget(BMessenger(receiver), &own);
	recognizer->SetInterimInterval(1);
		// as often as the model can
	if (speech)
		status = recognizer->Start(NULL, &token);
	else
		status = recognizer->Start(&format, &token);
	receiver->Reset(token);
	// two thirds of the audio, then a pause in which a reading that is
	// not final must come, then the rest
	size_t firstPart;
	if (speech) {
		firstPart = samples.Count() * 2 / 3;
		recognizer->Feed(samples.Samples(), firstPart * sizeof(float));
	} else {
		firstPart = toneSize * 2 / 3 / 4 * 4;
		recognizer->Feed(tone, firstPart);
	}
	bigtime_t deadline = system_time() + 120000000LL;
	int32 interims = 0;
	while (system_time() < deadline) {
		receiver->Lock();
		interims = receiver->fInterims;
		receiver->Unlock();
		recognizer->GetInfo(&info);
		// (a tone gives no text; then a window read is all there is)
		if (interims > 0 || (!speech && info.windows > 0))
			break;
		snooze(50000);
	}
	if (speech) {
		recognizer->Feed(samples.Samples() + firstPart,
			(samples.Count() - firstPart) * sizeof(float));
	} else
		recognizer->Feed((uint8*)tone + firstPart, toneSize - firstPart);
	recognizer->Finish();
	status = recognizer->WaitUntilFinished();
	acquire_sem_etc(receiver->fDone, 1, B_RELATIVE_TIMEOUT, 10000000);
	receiver->Lock();
	check(status == B_OK && (interims > 0 || !speech)
		&& receiver->fFinished == 1 && receiver->fText == text
		&& receiver->fOwn == receiver->fFinals + receiver->fInterims + 1,
		"%d result(s) that were not final (\"%s\"), then the same text; "
		"all in the caller's message", (int)receiver->fInterims,
		receiver->fLastInterim.String());
	receiver->Unlock();
	recognizer->SetInterimInterval(0);
	recognizer->SetTarget(BMessenger(receiver));

	// 4. cancelled in the middle: no results after that
	if (speech)
		status = recognizer->Start(NULL, &token);
	else
		status = recognizer->Start(&format, &token);
	receiver->Reset(token);
	if (speech) {
		recognizer->Feed(samples.Samples(),
			samples.Count() * sizeof(float));
	} else
		recognizer->Feed(tone, toneSize);
	recognizer->Finish();
	snooze(100000);
	start = system_time();
	recognizer->Cancel();
	status = recognizer->WaitUntilFinished();
	took = system_time() - start;
	snooze(500000);
	receiver->Lock();
	int32 after = receiver->fFinals + receiver->fFinished;
	receiver->Unlock();
	snooze(3000000);
	receiver->Lock();
	check(status == B_CANCELED && !recognizer->IsRecognizing()
		&& receiver->fFinals + receiver->fFinished == after
		&& receiver->fFinished == 0 && took < 100000,
		"Cancel() returns at once (%lld us) and ends the results",
		(long long)took);
	receiver->Unlock();

	// and the recognizer is as good as new
	if (speech) {
		status = recognizer->Transcribe(samples.Samples(), samples.Count(),
			&text);
	} else
		status = recognizer->Transcribe(format, tone, toneSize, &text);
	receiver->Lock();
	check(status == B_OK && receiver->fText == ""
		&& (!speech || text.Length() > 0),
		"after that it works as before; Transcribe() sends no messages");
	receiver->Unlock();

	// 5. deleted in the middle of its work
	if (speech)
		recognizer->Start(NULL, &token);
	else
		recognizer->Start(&format, &token);
	receiver->Reset(token);
	if (speech) {
		recognizer->Feed(samples.Samples(),
			samples.Count() * sizeof(float));
	} else
		recognizer->Feed(tone, toneSize);
	recognizer->Finish();
	snooze(300000);
	start = system_time();
	delete recognizer;
	took = system_time() - start;
	snooze(1000000);
	receiver->Lock();
	check(receiver->fFinished == 0 && took < 30000000LL,
		"deleted while it worked: back after %.2f s, nothing came after",
		took / 1e6);
	receiver->Unlock();

	// 6. made and deleted in a row, with a recording begun
	start = system_time();
	bool allOk = true;
	for (int32 i = 0; i < 3; i++) {
		BSpeechRecognizer again(directory.Path());
		allOk = allOk && again.InitCheck() == B_OK
			&& again.Start() == B_OK;
		float silence[1600] = {};
		allOk = allOk && again.Feed(silence, sizeof(silence)) == B_OK;
		if (i == 1)
			again.Finish();
	}
	check(allOk, "three recognizers made, begun and deleted in %.2f s",
		(system_time() - start) / 1e6);

	receiver->Lock();
	receiver->Quit();
	free(tone);
}


int
main(int argc, char** argv)
{
	char directory[] = "/tmp/speech_test.XXXXXX";
	if (mkdtemp(directory) == NULL) {
		fprintf(stderr, "no temporary directory\n");
		return 1;
	}
	write_model(directory);

	test_front_end();
	test_converter();
	test_model_directory(directory);
	test_decoding(directory);
	test_recognizer(argc > 1 ? argv[1] : NULL, argc > 2 ? argv[2] : NULL);

	static const char* const kFiles[] = { "speech.conf", "vocabulary.bin",
		"mel_filters.bin" };
	for (size_t i = 0; i < B_COUNT_OF(kFiles); i++)
		unlink((BString(directory) << "/" << kFiles[i]).String());
	rmdir(directory);

	printf("%s\n", sFailures == 0 ? "all checks passed" : "CHECKS FAILED");
	return sFailures == 0 ? 0 : 1;
}
