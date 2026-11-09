/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Everything the kit knows about Whisper as a family of speech models
	is in this file: how its decoder is run token by token, which tokens
	may follow which, and how phrases and their times are read from what
	it wrote. (The numbers of a particular model, its special tokens and
	the sizes of its front end, are in the model's speech.conf.) Another
	family would be another SpeechDecoder beside this one.

	The rules are those of OpenAI's implementation (decoding.py and
	transcribe.py of github.com/openai/whisper, MIT, (c) 2022 OpenAI):
	greedy decoding with its filters (SuppressBlank, SuppressTokens,
	ApplyTimestampRules), the test for silence and the way segments and
	the start of the next window are taken from the time stamp tokens.
	They are written anew here from that description. Not taken over: the
	second tries at higher temperatures when a window's text looks wrong,
	beam search, and giving the decoder the text before the window.

	The networks (see tools/airos/neural/whisper_convert.py):
	- the encoder takes the spectrogram ("mel") and gives, for every
	  layer of the decoder, what that layer needs of the audio;
	- the decoder does one step: inputs "token" and "position", the
	  encoder's outputs under their names, and for every input
	  "self_<x>" (what earlier steps computed; one of its dimensions
	  counts the positions) an output "new_<x>" of the same shape with
	  one position: this step's part, which goes into the input at the
	  step's position before the next step. Output "logits": how well
	  each token fits next.
*/


#include <SpeechCore.h>

#include <math.h>
#include <new>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>


namespace BPrivate {


namespace {


//! The window is taken as silence when the model says so this firmly ...
static const float kNoSpeechThreshold = 0.6f;
//! ... and is this unsure of what it wrote instead.
static const float kLogProbabilityThreshold = -1.0f;
//! A window's first time stamp lies within this of its beginning.
static const int32 kMaxInitialTimestampMs = 1000;


class WhisperDecoder : public SpeechDecoder {
public:
								WhisperDecoder();
	virtual						~WhisperDecoder();

	virtual	status_t			Init(const SpeechModel& model,
									SpeechNetwork* encoder,
									SpeechNetwork* decoder);
	virtual	const char*			Error() const { return fError; }

	virtual	status_t			DecodeWindow(const float* mel,
									const speech_decode_options& options,
									speech_window_result* _result);

private:
			//! what one step leaves for the steps after it
			struct State {
				speech_tensor	all;
				speech_tensor	fresh;
				/*!	\c all is \c outer times: a stretch of \c inner
					bytes for each position */
				int32			outer;
				size_t			inner;
			};

			//! what the encoder computes for the decoder
			struct Link {
				speech_tensor	from;
				speech_tensor	to;
			};

			status_t			_Bind();
			status_t			_Step(int32 position, int32 token);
			double				_Probability(int32 token) const;
			void				_ApplyRules(const int32* text, int32 count,
									bool timestamps);
			void				_LogSoftmax();
			void				_AddSegment(int32 startMs, int32 endMs,
									int32 first, int32 count);
			void				_MakeSegments(int32 count,
									int32 contentFrames,
									speech_window_result* result);
			void				_SetError(const char* format, ...);

			const SpeechModel*	fModel;
			SpeechNetwork*		fEncoder;
			SpeechNetwork*		fDecoder;
			speech_tensor		fMel;
			speech_tensor		fToken;
			speech_tensor		fPosition;
			speech_tensor		fLogits;
			State*				fStates;
			int32				fStateCount;
			Link*				fLinks;
			int32				fLinkCount;

	//! the prompt and what was decoded after it
			int32*				fTokens;
			speech_window_segment* fSegments;
			int32				fSegmentCount;
	//! the logits of the last step with the rules applied
			double*				fScores;
	//! and the logarithms of the probabilities they amount to
			double*				fChances;
			char				fError[160];
};


static inline bool
is_float_tensor(const speech_tensor& tensor, int64 elements)
{
	return tensor.type == B_NEURAL_FLOAT32 && tensor.data != NULL
		&& tensor.size == (size_t)elements * sizeof(float);
}


static inline bool
is_index_tensor(const speech_tensor& tensor)
{
	return tensor.type == B_NEURAL_INT32 && tensor.data != NULL
		&& tensor.size == sizeof(int32);
}


WhisperDecoder::WhisperDecoder()
	:
	fModel(NULL),
	fEncoder(NULL),
	fDecoder(NULL),
	fStates(NULL),
	fStateCount(0),
	fLinks(NULL),
	fLinkCount(0),
	fTokens(NULL),
	fSegments(NULL),
	fSegmentCount(0),
	fScores(NULL),
	fChances(NULL)
{
	fError[0] = '\0';
}


WhisperDecoder::~WhisperDecoder()
{
	free(fStates);
	free(fLinks);
	free(fTokens);
	free(fSegments);
	free(fScores);
	free(fChances);
}


void
WhisperDecoder::_SetError(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vsnprintf(fError, sizeof(fError), format, args);
	va_end(args);
}


status_t
WhisperDecoder::Init(const SpeechModel& model, SpeechNetwork* encoder,
	SpeechNetwork* decoder)
{
	if (fModel != NULL)
		return B_NOT_ALLOWED;
	if (encoder == NULL || decoder == NULL)
		return B_BAD_VALUE;

	fModel = &model;
	fEncoder = encoder;
	fDecoder = decoder;

	const SpeechModelInfo& info = model.Info();
	int32 inputs = decoder->CountInputs();
	fStates = (State*)calloc(inputs > 0 ? inputs : 1, sizeof(State));
	fLinks = (Link*)calloc(inputs > 0 ? inputs : 1, sizeof(Link));
	fTokens = (int32*)malloc(info.text_context * sizeof(int32));
	fSegments = (speech_window_segment*)malloc(info.text_context
		* sizeof(speech_window_segment));
	fScores = (double*)malloc(info.vocabulary_size * sizeof(double));
	fChances = (double*)malloc(info.vocabulary_size * sizeof(double));
	if (fStates == NULL || fLinks == NULL || fTokens == NULL
		|| fSegments == NULL || fScores == NULL || fChances == NULL) {
		return B_NO_MEMORY;
	}

	return _Bind();
}


/*!	Finds the networks' inputs and outputs and checks that they are what
	the description of the model promises. */
status_t
WhisperDecoder::_Bind()
{
	const SpeechModelInfo& info = fModel->Info();

	if (fEncoder->CountInputs() != 1
		|| fEncoder->GetInput(0, &fMel) != B_OK
		|| !is_float_tensor(fMel, (int64)info.mel_bins * info.window_frames)) {
		_SetError("the encoder does not take a spectrogram of %d by %d",
			(int)info.mel_bins, (int)info.window_frames);
		return B_NEURAL_SHAPE_MISMATCH;
	}

	if (fDecoder->FindInput("token", &fToken) != B_OK
		|| !is_index_tensor(fToken)
		|| fDecoder->FindInput("position", &fPosition) != B_OK
		|| !is_index_tensor(fPosition)
		|| fDecoder->FindOutput("logits", &fLogits) != B_OK
		|| !is_float_tensor(fLogits, info.vocabulary_size)) {
		_SetError("the decoder lacks \"token\", \"position\" or \"logits\" "
			"for %d tokens", (int)info.vocabulary_size);
		return B_NEURAL_SHAPE_MISMATCH;
	}

	for (int32 i = 0; i < fDecoder->CountInputs(); i++) {
		speech_tensor input;
		if (fDecoder->GetInput(i, &input) != B_OK || input.name == NULL)
			return B_NEURAL_BAD_MODEL;
		if (strcmp(input.name, "token") == 0
			|| strcmp(input.name, "position") == 0) {
			continue;
		}

		if (strncmp(input.name, "self_", 5) == 0) {
			// The parts of earlier steps and this step's part: the same
			// shape, but for the one dimension that counts the positions.
			char name[64];
			State& state = fStates[fStateCount];
			if ((size_t)snprintf(name, sizeof(name), "new_%s", input.name + 5)
					>= sizeof(name)
				|| fDecoder->FindOutput(name, &state.fresh) != B_OK
				|| input.rank < 1 || input.rank != state.fresh.rank) {
				_SetError("the decoder's \"%s\" has no fitting \"new_...\"",
					input.name);
				return B_NEURAL_SHAPE_MISMATCH;
			}

			int32 axis = -1;
			int64 elements = 1;
			int64 freshElements = 1;
			bool fits = true;
			for (int32 j = 0; j < input.rank; j++) {
				int32 size = input.dimensions[j];
				int32 freshSize = state.fresh.dimensions[j];
				if (size < 1 || freshSize < 1)
					fits = false;
				else if (size != freshSize) {
					if (axis >= 0 || size != info.text_context
						|| freshSize != 1) {
						fits = false;
					}
					axis = j;
				}
				elements *= size > 0 ? size : 1;
				freshElements *= freshSize > 0 ? freshSize : 1;
				if (elements > INT32_MAX)
					fits = false;
			}
			if (!fits || axis < 0 || !is_float_tensor(input, elements)
				|| !is_float_tensor(state.fresh, freshElements)) {
				_SetError("the decoder's \"%s\" and \"%s\" do not go "
					"together", input.name, name);
				return B_NEURAL_SHAPE_MISMATCH;
			}

			state.all = input;
			state.outer = 1;
			for (int32 j = 0; j < axis; j++)
				state.outer *= input.dimensions[j];
			state.inner = sizeof(float);
			for (int32 j = axis + 1; j < input.rank; j++)
				state.inner *= input.dimensions[j];
			fStateCount++;
			continue;
		}

		// everything else comes from the encoder
		Link& link = fLinks[fLinkCount];
		if (fEncoder->FindOutput(input.name, &link.from) != B_OK
			|| link.from.type != B_NEURAL_FLOAT32
			|| input.type != B_NEURAL_FLOAT32 || input.data == NULL
			|| link.from.data == NULL || link.from.size != input.size) {
			_SetError("the encoder has no output for the decoder's \"%s\"",
				input.name);
			return B_NEURAL_SHAPE_MISMATCH;
		}
		link.to = input;
		fLinkCount++;
	}

	return B_OK;
}


//! Runs the decoder on one token; its logits are in fLogits afterwards.
status_t
WhisperDecoder::_Step(int32 position, int32 token)
{
	*(int32*)fToken.data = token;
	*(int32*)fPosition.data = position;

	status_t status = fDecoder->Run();
	if (status != B_OK)
		return status;

	// this step's part of what the next steps look back at
	int32 positions = fModel->Info().text_context;
	for (int32 i = 0; i < fStateCount; i++) {
		const State& state = fStates[i];
		const uint8* fresh = (const uint8*)state.fresh.data;
		uint8* all = (uint8*)state.all.data + (size_t)position * state.inner;
		for (int32 j = 0; j < state.outer; j++) {
			memcpy(all, fresh, state.inner);
			fresh += state.inner;
			all += (size_t)positions * state.inner;
		}
	}
	return B_OK;
}


//! The probability the last step gave a token, before any rule.
double
WhisperDecoder::_Probability(int32 token) const
{
	const float* logits = (const float*)fLogits.data;
	int32 count = fModel->Info().vocabulary_size;

	double largest = logits[0];
	for (int32 i = 1; i < count; i++)
		largest = logits[i] > largest ? (double)logits[i] : largest;
	double sum = 0;
	for (int32 i = 0; i < count; i++)
		sum += exp(logits[i] - largest);
	return exp(logits[token] - largest) / sum;
}


//! fChances: the logarithms of the probabilities fScores amount to.
void
WhisperDecoder::_LogSoftmax()
{
	int32 count = fModel->Info().vocabulary_size;

	double largest = fScores[0];
	for (int32 i = 1; i < count; i++)
		largest = fScores[i] > largest ? fScores[i] : largest;
	double sum = 0;
	for (int32 i = 0; i < count; i++)
		sum += exp(fScores[i] - largest);
	double offset = largest + log(sum);
	for (int32 i = 0; i < count; i++)
		fChances[i] = fScores[i] - offset;
}


/*!	Rules out, in fScores, what the next token must not be. \a text is
	what was decoded of the window so far. */
void
WhisperDecoder::_ApplyRules(const int32* text, int32 count, bool timestamps)
{
	const SpeechModelInfo& info = fModel->Info();
	const double kNever = -HUGE_VAL;
	int32 size = info.vocabulary_size;
	int32 firstStamp = info.token_timestamp_begin;

	// a window does not begin with a space or with its end
	if (count == 0) {
		fScores[info.token_blank] = kNever;
		fScores[info.token_eot] = kNever;
	}

	for (int32 i = 0; i < fModel->CountSuppressed(); i++)
		fScores[fModel->SuppressedAt(i)] = kNever;

	if (!timestamps) {
		for (int32 i = firstStamp; i < size; i++)
			fScores[i] = kNever;
		return;
	}

	fScores[info.token_no_timestamps] = kNever;

	// Time stamps come in pairs (the end of a phrase and the beginning
	// of the next), except right before the end of the text.
	bool last = count >= 1 && text[count - 1] >= firstStamp;
	bool before = count < 2 || text[count - 2] >= firstStamp;
	if (last) {
		if (before) {
			for (int32 i = firstStamp; i < size; i++)
				fScores[i] = kNever;
		} else {
			for (int32 i = 0; i < info.token_eot; i++)
				fScores[i] = kNever;
		}
	}

	// time does not go back
	int32 latest = -1;
	for (int32 i = count - 1; i >= 0; i--) {
		if (text[i] >= firstStamp) {
			latest = text[i];
			break;
		}
	}
	if (latest >= 0) {
		int32 limit = last && !before ? latest : latest + 1;
		for (int32 i = firstStamp; i < limit && i < size; i++)
			fScores[i] = kNever;
	}

	if (count == 0) {
		// a window begins with a time stamp, one of its first second
		for (int32 i = 0; i < firstStamp; i++)
			fScores[i] = kNever;
		int32 allowed = firstStamp
			+ kMaxInitialTimestampMs / info.timestamp_ms;
		for (int32 i = allowed + 1; i < size; i++)
			fScores[i] = kNever;
	}

	// If all time stamps together are likelier than any text token, a
	// time stamp it is.
	_LogSoftmax();
	double largest = kNever;
	for (int32 i = firstStamp; i < size; i++)
		largest = fChances[i] > largest ? fChances[i] : largest;
	double together = kNever;
	if (largest > kNever) {
		double sum = 0;
		for (int32 i = firstStamp; i < size; i++) {
			if (fChances[i] > kNever)
				sum += exp(fChances[i] - largest);
		}
		together = largest + log(sum);
	}
	double bestText = kNever;
	for (int32 i = 0; i < firstStamp; i++)
		bestText = fChances[i] > bestText ? fChances[i] : bestText;
	if (together > bestText) {
		for (int32 i = 0; i < firstStamp; i++)
			fScores[i] = kNever;
	}
}


void
WhisperDecoder::_AddSegment(int32 startMs, int32 endMs, int32 first,
	int32 count)
{
	speech_window_segment& segment = fSegments[fSegmentCount++];
	segment.start_ms = startMs;
	segment.end_ms = endMs;
	segment.first_token = first;
	segment.token_count = count;
}


/*!	Phrases from the tokens of a window (fTokens after the prompt: \a
	count of them), and where the next window begins: two time stamps in
	a row end one phrase and begin the next; the window after this one
	begins at the last such place, since what follows it was cut off by
	the end of the window. */
void
WhisperDecoder::_MakeSegments(int32 count, int32 contentFrames,
	speech_window_result* result)
{
	const SpeechModelInfo& info = fModel->Info();
	const int32* tokens = result->tokens;
	int32 firstStamp = info.token_timestamp_begin;
	int32 stampMs = info.timestamp_ms;
	// frames (of the spectrogram) per step of the time stamps
	double stampFrames = (double)stampMs * info.sample_rate
		/ (1000.0 * info.hop);

	fSegmentCount = 0;
	result->advance_frames = contentFrames;

	bool singleEnd = count >= 1 && tokens[count - 1] >= firstStamp
		&& (count < 2 || tokens[count - 2] < firstStamp);
	// (a lone time stamp is no ending of that kind)
	if (count < 2)
		singleEnd = false;

	int32 last = 0;
	bool cut = false;
	for (int32 i = 1; i <= count; i++) {
		bool pair = i < count && tokens[i - 1] >= firstStamp
			&& tokens[i] >= firstStamp;
		bool end = i == count && singleEnd && cut;
		if (!pair && !end)
			continue;

		// tokens last .. i - 1 are a phrase between two time stamps
		_AddSegment((tokens[last] - firstStamp) * stampMs,
			(tokens[i - 1] - firstStamp) * stampMs, last, i - last);
		last = i;
		cut = true;
	}

	if (cut) {
		if (!singleEnd) {
			int32 advance = (int32)lround((tokens[last - 1] - firstStamp)
				* stampFrames);
			// (the rules make this more than nothing; if a model breaks
			// them, the windows must still move on)
			if (advance > 0)
				result->advance_frames = advance;
		}
		return;
	}

	// no pair of time stamps: all of it is one phrase, which ends where
	// the last time stamp says, if there is one
	int32 endMs = (int32)lround((double)contentFrames * info.hop * 1000
		/ info.sample_rate);
	for (int32 i = count - 1; i >= 0; i--) {
		if (tokens[i] >= firstStamp) {
			if (tokens[i] != firstStamp)
				endMs = (tokens[i] - firstStamp) * stampMs;
			break;
		}
	}
	if (count > 0)
		_AddSegment(0, endMs, 0, count);
}


status_t
WhisperDecoder::DecodeWindow(const float* mel,
	const speech_decode_options& options, speech_window_result* result)
{
	if (fModel == NULL)
		return B_NO_INIT;

	const SpeechModelInfo& info = fModel->Info();
	memset(result, 0, sizeof(*result));

	bigtime_t start = system_time();
	memcpy(fMel.data, mel, fMel.size);
	status_t status = fEncoder->Run();
	if (status != B_OK)
		return status;
	result->encoder_time = system_time() - start;

	start = system_time();
	for (int32 i = 0; i < fLinkCount; i++)
		memcpy(fLinks[i].to.data, fLinks[i].from.data, fLinks[i].to.size);
	for (int32 i = 0; i < fStateCount; i++)
		memset(fStates[i].all.data, 0, fStates[i].all.size);

	// what the decoder is told before it writes: start of a transcript,
	// (in which language, to be written down, not translated,) and
	// whether times are wanted
	int32 count = 0;
	fTokens[count++] = info.token_sot;
	if (info.multilingual && options.language_token >= 0) {
		fTokens[count++] = options.language_token;
		fTokens[count++] = info.token_transcribe;
	}
	if (!options.timestamps)
		fTokens[count++] = info.token_no_timestamps;
	int32 begin = count;

	double noSpeech = 0;
	for (int32 position = 0; position < begin - 1; position++) {
		status = _Step(position, fTokens[position]);
		if (status != B_OK)
			return status;
		if (fTokens[position] == info.token_sot)
			noSpeech = _Probability(info.token_no_speech);
	}

	double total = 0;
	int32 steps = 0;
	const float* logits = (const float*)fLogits.data;
	for (int32 i = 0; i < info.max_tokens; i++) {
		if (options.cancel != NULL && *options.cancel != 0)
			return B_CANCELED;

		status = _Step(count - 1, fTokens[count - 1]);
		if (status != B_OK)
			return status;
		if (fTokens[count - 1] == info.token_sot)
			noSpeech = _Probability(info.token_no_speech);

		for (int32 j = 0; j < info.vocabulary_size; j++)
			fScores[j] = logits[j];
		_ApplyRules(fTokens + begin, count - begin, options.timestamps);
		_LogSoftmax();

		// the likeliest token that is left; the first of equals
		int32 best = 0;
		for (int32 j = 1; j < info.vocabulary_size; j++) {
			if (fScores[j] > fScores[best])
				best = j;
		}
		total += fChances[best];
		steps++;

		if (best == info.token_eot)
			break;
		fTokens[count++] = best;
		if (count >= info.text_context)
			break;
	}

	result->tokens = fTokens + begin;
	result->token_count = count - begin;
	result->no_speech = (float)noSpeech;
	result->mean_log_probability = (float)(total / (steps + 1));
	result->advance_frames = options.content_frames;
	result->silent = noSpeech > kNoSpeechThreshold
		&& result->mean_log_probability < kLogProbabilityThreshold;
	if (!result->silent) {
		_MakeSegments(result->token_count, options.content_frames, result);
		result->segments = fSegments;
		result->segment_count = fSegmentCount;
	}
	result->decoder_time = system_time() - start;
	return B_OK;
}


}	// unnamed namespace


SpeechDecoder::~SpeechDecoder()
{
}


SpeechDecoder*
speech_create_decoder(const char* family)
{
	if (family != NULL && strcmp(family, "whisper") == 0)
		return new(std::nothrow) WhisperDecoder();
	return NULL;
}


}	// namespace BPrivate
