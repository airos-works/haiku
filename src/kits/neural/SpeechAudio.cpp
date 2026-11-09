/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Audio on its way into a speech model: raw audio of any usual format
	to one channel at the model's rate (SpeechAudioConverter), and those
	samples to the log-mel spectrogram the model's encoder takes
	(SpeechFrontEnd).

	The spectrogram is Whisper's (log_mel_spectrogram() in audio.py of
	github.com/openai/whisper, MIT): the definition is taken from there,
	the code is not; it is checked against a NumPy version of the
	definition in the host tests.
*/


#pragma GCC optimize("O3")


#include <SpeechCore.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <NeuralInterpreter.h>


namespace BPrivate {


// #pragma mark - SpeechSamples


SpeechSamples::SpeechSamples()
	:
	fSamples(NULL),
	fCount(0),
	fCapacity(0)
{
}


SpeechSamples::~SpeechSamples()
{
	free(fSamples);
}


float*
SpeechSamples::Grow(size_t count)
{
	if (count > fCapacity - fCount) {
		size_t capacity = fCapacity > 0 ? fCapacity : 16384;
		while (capacity - fCount < count) {
			if (capacity > ((size_t)-1) / (2 * sizeof(float)))
				return NULL;
			capacity *= 2;
		}
		float* samples = (float*)realloc(fSamples, capacity * sizeof(float));
		if (samples == NULL)
			return NULL;
		fSamples = samples;
		fCapacity = capacity;
	}

	float* result = fSamples + fCount;
	fCount += count;
	return result;
}


status_t
SpeechSamples::Append(const float* samples, size_t count)
{
	if (count == 0)
		return B_OK;
	float* target = Grow(count);
	if (target == NULL)
		return B_NO_MEMORY;
	memcpy(target, samples, count * sizeof(float));
	return B_OK;
}


void
SpeechSamples::RemoveFirst(size_t count)
{
	if (count >= fCount) {
		fCount = 0;
		return;
	}
	memmove(fSamples, fSamples + count, (fCount - count) * sizeof(float));
	fCount -= count;
}


void
SpeechSamples::MakeEmpty()
{
	fCount = 0;
}


// #pragma mark - SpeechAudioConverter


namespace {


// The filter that changes the rate: sin(pi x) / (pi x) under a Kaiser
// window, kept as a table of its right half with this many entries per
// zero crossing; values between entries are on a straight line.
static const int32 kZeroCrossings = 16;
static const int32 kFilterSteps = 512;
static const double kKaiserBeta = 9.0;
/*!	What is passed, as a share of what the lower of the two rates can
	carry: the filter needs some room to fall off before that. */
static const double kPassband = 0.95;


//! The modified Bessel function of order zero, by its series.
static double
bessel_i0(double x)
{
	double sum = 1;
	double term = 1;
	for (int k = 1; k < 64; k++) {
		term *= (x / 2) / k;
		sum += term * term;
		if (term * term < sum * 1e-17)
			break;
	}
	return sum;
}


static inline uint16
swap_16(uint16 value)
{
	return (value >> 8) | (value << 8);
}


static inline uint32
swap_32(uint32 value)
{
	return __builtin_bswap32(value);
}


/*!	One sample of the given format as a number from -1 to 1. */
static inline float
read_sample(const uint8* data, uint32 format, bool swap)
{
	switch (format) {
		case B_SPEECH_SAMPLE_UINT8:
			return ((int32)data[0] - 128) / 128.0f;
		case B_SPEECH_SAMPLE_INT8:
			return (int8)data[0] / 128.0f;
		case B_SPEECH_SAMPLE_INT16:
		{
			uint16 value;
			memcpy(&value, data, sizeof(value));
			if (swap)
				value = swap_16(value);
			return (int16)value / 32768.0f;
		}
		case B_SPEECH_SAMPLE_INT32:
		{
			uint32 value;
			memcpy(&value, data, sizeof(value));
			if (swap)
				value = swap_32(value);
			return (float)((int32)value / 2147483648.0);
		}
		case B_SPEECH_SAMPLE_FLOAT:
		{
			uint32 value;
			memcpy(&value, data, sizeof(value));
			if (swap)
				value = swap_32(value);
			float sample;
			memcpy(&sample, &value, sizeof(sample));
			// what is not a number would spoil everything after it
			return isfinite(sample) ? sample : 0;
		}
		default:
		{
			uint64 value;
			memcpy(&value, data, sizeof(value));
			if (swap)
				value = __builtin_bswap64(value);
			double sample;
			memcpy(&sample, &value, sizeof(sample));
			return isfinite(sample) ? (float)sample : 0;
		}
	}
}


}	// unnamed namespace


SpeechAudioConverter::SpeechAudioConverter()
	:
	fFormat(0),
	fChannels(0),
	fSampleSize(0),
	fSwapBytes(false),
	fRatio(1),
	fScale(1),
	fHalfWidth(0),
	fFilter(NULL),
	fPartialSize(0),
	fInputStart(0),
	fOutputCount(0)
{
}


SpeechAudioConverter::~SpeechAudioConverter()
{
	free(fFilter);
}


status_t
SpeechAudioConverter::SetFormat(uint32 format, int32 channels,
	double frameRate, bool swapBytes, int32 targetRate)
{
	int32 sampleSize;
	switch (format) {
		case B_SPEECH_SAMPLE_INT8:
		case B_SPEECH_SAMPLE_UINT8:
			sampleSize = 1;
			break;
		case B_SPEECH_SAMPLE_INT16:
			sampleSize = 2;
			break;
		case B_SPEECH_SAMPLE_INT32:
		case B_SPEECH_SAMPLE_FLOAT:
			sampleSize = 4;
			break;
		case B_SPEECH_SAMPLE_DOUBLE:
			sampleSize = 8;
			break;
		default:
			return B_BAD_VALUE;
	}

	// a frame must fit fPartial
	if (channels < 1 || channels > 8 || targetRate < 1
		|| !(frameRate >= 1000) || !(frameRate <= 768000)) {
		return B_BAD_VALUE;
	}

	if (fFilter == NULL) {
		int32 entries = kZeroCrossings * kFilterSteps + 2;
		fFilter = (float*)malloc(entries * sizeof(float));
		if (fFilter == NULL)
			return B_NO_MEMORY;

		double windowScale = 1 / bessel_i0(kKaiserBeta);
		for (int32 i = 0; i < entries; i++) {
			double x = (double)i / kFilterSteps;
			double position = x / kZeroCrossings;
			if (position >= 1) {
				fFilter[i] = 0;
				continue;
			}
			double sinc = i == 0 ? 1 : sin(M_PI * x) / (M_PI * x);
			fFilter[i] = (float)(sinc * windowScale * bessel_i0(kKaiserBeta
				* sqrt(1 - position * position)));
		}
	}

	fFormat = format;
	fChannels = channels;
	fSampleSize = sampleSize;
	fSwapBytes = swapBytes;
	fRatio = frameRate / targetRate;
	if (fabs(fRatio - 1) < 1e-9)
		fRatio = 1;
	// the filter is as wide as the lower rate needs it
	fScale = kPassband * (fRatio > 1 ? 1 / fRatio : 1);
	fHalfWidth = (int32)ceil(kZeroCrossings / fScale);
	Reset();
	return B_OK;
}


void
SpeechAudioConverter::Reset()
{
	fPartialSize = 0;
	fInput.MakeEmpty();
	fInputStart = 0;
	fOutputCount = 0;
}


status_t
SpeechAudioConverter::Convert(const void* _data, size_t size,
	SpeechSamples& output)
{
	if (fChannels == 0)
		return B_NO_INIT;

	const uint8* data = (const uint8*)_data;
	size_t frameSize = (size_t)fSampleSize * fChannels;
	float gain = 1.0f / fChannels;
	float mono[1024];

	// the frame the buffer before left unfinished
	if (fPartialSize > 0) {
		size_t missing = frameSize - fPartialSize;
		if (size < missing) {
			memcpy(fPartial + fPartialSize, data, size);
			fPartialSize += size;
			return B_OK;
		}
		memcpy(fPartial + fPartialSize, data, missing);
		data += missing;
		size -= missing;
		fPartialSize = 0;

		float sum = 0;
		for (int32 channel = 0; channel < fChannels; channel++) {
			sum += read_sample(fPartial + channel * fSampleSize, fFormat,
				fSwapBytes);
		}
		mono[0] = sum * gain;
		status_t status = _AddMono(mono, 1, output);
		if (status != B_OK)
			return status;
	}

	size_t frames = size / frameSize;
	while (frames > 0) {
		size_t count = frames < 1024 ? frames : 1024;
		for (size_t i = 0; i < count; i++) {
			float sum = 0;
			for (int32 channel = 0; channel < fChannels; channel++) {
				sum += read_sample(data + channel * fSampleSize, fFormat,
					fSwapBytes);
			}
			mono[i] = sum * gain;
			data += frameSize;
		}
		status_t status = _AddMono(mono, count, output);
		if (status != B_OK)
			return status;
		frames -= count;
	}

	fPartialSize = size % frameSize;
	memcpy(fPartial, data, fPartialSize);
	return B_OK;
}


status_t
SpeechAudioConverter::Flush(SpeechSamples& output)
{
	if (fChannels == 0)
		return B_NO_INIT;
	fPartialSize = 0;
	if (fRatio == 1)
		return B_OK;
	return _Resample(true, output);
}


status_t
SpeechAudioConverter::_AddMono(const float* samples, size_t count,
	SpeechSamples& output)
{
	if (fRatio == 1)
		return output.Append(samples, count);

	status_t status = fInput.Append(samples, count);
	if (status != B_OK)
		return status;
	return _Resample(false, output);
}


/*!	Makes the output samples the input so far allows: output sample n
	lies at input position n * fRatio and is the sum of the input samples
	around it, each times the filter at its distance. With \a flush the
	input is taken to be followed by silence. */
status_t
SpeechAudioConverter::_Resample(bool flush, SpeechSamples& output)
{
	const float* input = fInput.Samples();
	int64 inputEnd = fInputStart + (int64)fInput.Count();
	float gain = (float)fScale;
	double filterStep = fScale * kFilterSteps;

	while (true) {
		double position = fOutputCount * fRatio;
		int64 first = (int64)ceil(position - fHalfWidth);
		int64 last = (int64)floor(position + fHalfWidth);
		if (flush) {
			// as many samples as the input is long at the new rate
			if (position >= (double)inputEnd)
				break;
		} else if (last >= inputEnd)
			break;

		if (first < fInputStart)
			first = fInputStart;
		if (last >= inputEnd)
			last = inputEnd - 1;

		float sum = 0;
		for (int64 k = first; k <= last; k++) {
			double distance = fabs((double)k - position) * filterStep;
			int32 index = (int32)distance;
			if (index >= kZeroCrossings * kFilterSteps)
				continue;
			float fraction = (float)(distance - index);
			sum += input[k - fInputStart] * (fFilter[index]
				+ fraction * (fFilter[index + 1] - fFilter[index]));
		}

		float* sample = output.Grow(1);
		if (sample == NULL)
			return B_NO_MEMORY;
		*sample = sum * gain;
		fOutputCount++;
	}

	// input the samples still to come do not reach back to
	int64 needed = (int64)ceil(fOutputCount * fRatio - fHalfWidth);
	if (needed > inputEnd)
		needed = inputEnd;
	if (needed > fInputStart + 65536 || (flush && needed > fInputStart)) {
		fInput.RemoveFirst(needed - fInputStart);
		fInputStart = needed;
	}
	return B_OK;
}


// #pragma mark - WAVE files


namespace {


static inline uint32
read_le32(const uint8* bytes)
{
	return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16)
		| ((uint32)bytes[3] << 24);
}


static inline uint16
read_le16(const uint8* bytes)
{
	return bytes[0] | (bytes[1] << 8);
}


}	// unnamed namespace


status_t
speech_read_wave_file(const char* path, int32 targetRate,
	SpeechSamples& output)
{
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return B_NAME_NOT_FOUND;

	uint8 header[12];
	if (fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) != 0
		|| memcmp(header + 8, "WAVE", 4) != 0) {
		fclose(file);
		return B_BAD_DATA;
	}

	// chunks: four letters, a size, the bytes (padded to an even count)
	SpeechAudioConverter converter;
	bool haveFormat = false;
	int32 bits = 0;
	status_t status = B_BAD_DATA;
	uint8 chunk[8];
	while (fread(chunk, 1, 8, file) == 8) {
		uint32 size = read_le32(chunk + 4);

		if (memcmp(chunk, "fmt ", 4) == 0) {
			uint8 format[40];
			if (size < 16 || size > sizeof(format)
				|| fread(format, 1, size, file) != size) {
				break;
			}
			uint16 tag = read_le16(format);
			int32 channels = read_le16(format + 2);
			uint32 rate = read_le32(format + 4);
			bits = read_le16(format + 14);
			// "extensible": the real tag is further on
			if (tag == 0xfffe && size >= 26)
				tag = read_le16(format + 24);

			uint32 sampleFormat = 0;
			if (tag == 1 && bits == 8)
				sampleFormat = B_SPEECH_SAMPLE_UINT8;
			else if (tag == 1 && bits == 16)
				sampleFormat = B_SPEECH_SAMPLE_INT16;
			else if (tag == 1 && (bits == 24 || bits == 32))
				sampleFormat = B_SPEECH_SAMPLE_INT32;
			else if (tag == 3 && bits == 32)
				sampleFormat = B_SPEECH_SAMPLE_FLOAT;
			else if (tag == 3 && bits == 64)
				sampleFormat = B_SPEECH_SAMPLE_DOUBLE;
			else
				break;

			// (files are little endian, as the machines the kit runs on)
			if (converter.SetFormat(sampleFormat, channels, rate, false,
					targetRate) != B_OK) {
				break;
			}
			haveFormat = true;
			if ((size & 1) != 0)
				fgetc(file);
			continue;
		}

		if (memcmp(chunk, "data", 4) != 0) {
			if (fseek(file, (long)size + (size & 1), SEEK_CUR) != 0)
				break;
			continue;
		}

		if (!haveFormat)
			break;

		// the samples, a piece at a time; a size that reaches past the
		// end of the file (recorders that were cut off leave such) means
		// "to the end"
		status = B_OK;
		uint8 buffer[3 * 4096];
		uint8 widened[4 * 4096];
		uint64 left = size;
		while (status == B_OK && left > 0) {
			size_t wanted = left < sizeof(buffer) ? left : sizeof(buffer);
			size_t count = fread(buffer, 1, wanted, file);
			if (count == 0)
				break;
			left -= count;

			if (bits == 24) {
				// three bytes become the upper three of four
				size_t samples = count / 3;
				for (size_t i = 0; i < samples; i++) {
					widened[4 * i] = 0;
					memcpy(widened + 4 * i + 1, buffer + 3 * i, 3);
				}
				status = converter.Convert(widened, 4 * samples, output);
			} else
				status = converter.Convert(buffer, count, output);
		}
		if (status == B_OK)
			status = converter.Flush(output);
		break;
	}

	fclose(file);
	return status;
}


// #pragma mark - SpeechFrontEnd


namespace {


struct complex_t {
	double	re;
	double	im;
};


}	// unnamed namespace


/*!	A discrete Fourier transform of a size that is a product of small
	numbers (400 = 2 * 2 * 2 * 2 * 5 * 5 for Whisper), done the usual way:
	a transform of size n = p * m is p transforms of size m, of every
	p-th sample each, put together with n * p multiplications (n for
	p = 2).
*/
struct SpeechFrontEnd::Plan {
	int32	size;
	int32	factor_count;
	int32	factors[32];
	//! e^(-2 pi i k / size): cosine and sine of each k
	double*	twiddles;

	void Transform(int32 level, const double* input, int32 inputStep,
		complex_t* output, int32 size) const;
};


void
SpeechFrontEnd::Plan::Transform(int32 level, const double* input,
	int32 inputStep, complex_t* output, int32 size) const
{
	if (size == 1) {
		output[0].re = input[0];
		output[0].im = 0;
		return;
	}

	int32 p = factors[level];
	int32 m = size / p;
	for (int32 q = 0; q < p; q++) {
		Transform(level + 1, input + q * inputStep, inputStep * p,
			output + q * m, m);
	}

	// Element k + r * m of the result is the sum over q of the q-th
	// part's element k times e^(-2 pi i q (k + r m) / size).
	int32 twiddleStep = this->size / size;

	if (p == 2) {
		for (int32 k = 0; k < m; k++) {
			const double* w = twiddles + 2 * (size_t)(k * twiddleStep);
			complex_t a = output[k];
			complex_t b = output[k + m];
			double re = b.re * w[0] - b.im * w[1];
			double im = b.re * w[1] + b.im * w[0];
			output[k].re = a.re + re;
			output[k].im = a.im + im;
			output[k + m].re = a.re - re;
			output[k + m].im = a.im - im;
		}
		return;
	}

	complex_t parts[32];
	for (int32 k = 0; k < m; k++) {
		for (int32 q = 0; q < p; q++)
			parts[q] = output[k + q * m];
		for (int32 r = 0; r < p; r++) {
			int32 index = k + r * m;
			double re = parts[0].re;
			double im = parts[0].im;
			int32 turn = 0;
			for (int32 q = 1; q < p; q++) {
				turn += index;
				if (turn >= size)
					turn -= size;
				const double* w = twiddles + 2 * (size_t)(turn * twiddleStep);
				re += parts[q].re * w[0] - parts[q].im * w[1];
				im += parts[q].re * w[1] + parts[q].im * w[0];
			}
			output[index].re = re;
			output[index].im = im;
		}
	}
}


namespace {


struct FrameRun {
	const SpeechFrontEnd*	front_end;
	const float*			audio;
	int64					count;
	int64					audio_start;
	int64					first;
	float*					mel;
	int32					status;
};


}	// unnamed namespace


SpeechFrontEnd::SpeechFrontEnd()
	:
	fFFTSize(0),
	fHop(0),
	fMelBins(0),
	fFrames(0),
	fBins(0),
	fPlan(NULL),
	fWindow(NULL),
	fFilterFirst(NULL),
	fFilterCount(NULL),
	fFilters(NULL)
{
}


SpeechFrontEnd::~SpeechFrontEnd()
{
	if (fPlan != NULL)
		free(fPlan->twiddles);
	free(fPlan);
	free(fWindow);
	free(fFilterFirst);
	free(fFilterCount);
	free(fFilters);
}


status_t
SpeechFrontEnd::Init(int32 fftSize, int32 hop, int32 melBins, int32 frames,
	const float* filters)
{
	if (fPlan != NULL)
		return B_NOT_ALLOWED;
	if (fftSize < 2 || fftSize > 65536 || fftSize % 2 != 0 || hop < 1
		|| hop > fftSize || melBins < 1 || frames < 1 || filters == NULL) {
		return B_BAD_VALUE;
	}

	fFFTSize = fftSize;
	fHop = hop;
	fMelBins = melBins;
	fFrames = frames;
	fBins = fftSize / 2 + 1;

	fPlan = (Plan*)calloc(1, sizeof(Plan));
	fWindow = (double*)malloc(fftSize * sizeof(double));
	fFilterFirst = (int32*)calloc(melBins, sizeof(int32));
	fFilterCount = (int32*)calloc(melBins, sizeof(int32));
	fFilters = (float*)malloc((size_t)melBins * fBins * sizeof(float));
	if (fPlan == NULL || fWindow == NULL || fFilterFirst == NULL
		|| fFilterCount == NULL || fFilters == NULL) {
		return B_NO_MEMORY;
	}
	fPlan->twiddles = (double*)malloc(2 * (size_t)fftSize * sizeof(double));
	if (fPlan->twiddles == NULL)
		return B_NO_MEMORY;

	// the size as a product of small primes
	fPlan->size = fftSize;
	int32 rest = fftSize;
	for (int32 factor = 2; factor < 32; factor++) {
		while (rest % factor == 0) {
			fPlan->factors[fPlan->factor_count++] = factor;
			rest /= factor;
		}
	}
	if (rest != 1) {
		// a size with a large prime in it; no model has one
		return B_NOT_SUPPORTED;
	}

	for (int32 k = 0; k < fftSize; k++) {
		double angle = -2 * M_PI * k / fftSize;
		fPlan->twiddles[2 * k] = cos(angle);
		fPlan->twiddles[2 * k + 1] = sin(angle);
		// the Hann window in the form that repeats with the transform
		fWindow[k] = 0.5 - 0.5 * cos(2 * M_PI * k / fftSize);
	}

	// Each filter is a narrow triangle: only the bins it takes are kept.
	memcpy(fFilters, filters, (size_t)melBins * fBins * sizeof(float));
	for (int32 i = 0; i < melBins; i++) {
		const float* row = filters + (size_t)i * fBins;
		int32 first = 0;
		while (first < fBins && row[first] == 0)
			first++;
		int32 end = fBins;
		while (end > first && row[end - 1] == 0)
			end--;
		fFilterFirst[i] = first;
		fFilterCount[i] = end - first;
	}
	return B_OK;
}


/*static*/ void
SpeechFrontEnd::_Frames(void* cookie, int32 first, int32 end)
{
	FrameRun& run = *(FrameRun*)cookie;
	const SpeechFrontEnd& self = *run.front_end;
	int32 size = self.fFFTSize;
	int32 bins = self.fBins;

	double* samples = (double*)malloc(size * sizeof(double));
	complex_t* spectrum = (complex_t*)malloc(size * sizeof(complex_t));
	double* power = (double*)malloc(bins * sizeof(double));
	if (samples == NULL || spectrum == NULL || power == NULL) {
		free(samples);
		free(spectrum);
		free(power);
		run.status = B_NO_MEMORY;
		return;
	}

	int64 audioEnd = run.audio_start + run.count;
	for (int32 frame = first; frame < end; frame++) {
		// the samples around the frame's place, under the window
		int64 start = run.first + (int64)frame * self.fHop - size / 2;
		for (int32 i = 0; i < size; i++) {
			int64 index = start + i;
			if (index < 0)
				index = -index;
			double sample = index >= run.audio_start && index < audioEnd
				? run.audio[index - run.audio_start] : 0;
			samples[i] = sample * self.fWindow[i];
		}

		self.fPlan->Transform(0, samples, 1, spectrum, size);
		for (int32 i = 0; i < bins; i++) {
			power[i] = spectrum[i].re * spectrum[i].re
				+ spectrum[i].im * spectrum[i].im;
		}

		for (int32 i = 0; i < self.fMelBins; i++) {
			const float* weights = self.fFilters + (size_t)i * bins
				+ self.fFilterFirst[i];
			const double* values = power + self.fFilterFirst[i];
			double sum = 0;
			for (int32 j = 0; j < self.fFilterCount[i]; j++)
				sum += weights[j] * values[j];
			run.mel[(size_t)i * self.fFrames + frame]
				= (float)log10(sum > 1e-10 ? sum : 1e-10);
		}
	}

	free(samples);
	free(spectrum);
	free(power);
}


status_t
SpeechFrontEnd::Compute(const float* audio, int64 count, int64 audioStart,
	int64 first, float* mel) const
{
	if (fPlan == NULL)
		return B_NO_INIT;
	if (count < 0 || audioStart < 0 || first < 0)
		return B_BAD_VALUE;

	FrameRun run = { this, audio, count, audioStart, first, mel, B_OK };
	NeuralThreadPool::Default()->Run(fFrames, 64, &_Frames, &run);
	if (run.status != B_OK)
		return run.status;

	// nothing more than 8 (80 dB) below the loudest, then to a range of
	// about -1 to 1
	size_t elements = (size_t)fMelBins * fFrames;
	float largest = mel[0];
	for (size_t i = 1; i < elements; i++)
		largest = mel[i] > largest ? mel[i] : largest;
	float floor = largest - 8;
	for (size_t i = 0; i < elements; i++)
		mel[i] = ((mel[i] > floor ? mel[i] : floor) + 4) / 4;
	return B_OK;
}


}	// namespace BPrivate
