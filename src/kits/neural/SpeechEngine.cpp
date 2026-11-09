/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The part of the speech recognizer that is the same for every family
	of models: a recording comes in bit by bit, is cut into the windows
	the model takes (30 seconds for Whisper), and each window's phrases
	go to a listener with their times in the recording.

	Windows do not simply follow one another: the decoder says where the
	last whole phrase of a window ended, and the next window begins
	there, so that a phrase cut by the end of a window is heard again in
	full.
*/


#include <SpeechCore.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>


namespace BPrivate {


SpeechNetwork::~SpeechNetwork()
{
}


status_t
SpeechNetwork::FindInput(const char* name, speech_tensor* _tensor)
{
	for (int32 i = 0; i < CountInputs(); i++) {
		speech_tensor tensor;
		if (GetInput(i, &tensor) == B_OK && tensor.name != NULL
			&& strcmp(tensor.name, name) == 0) {
			*_tensor = tensor;
			return B_OK;
		}
	}
	return B_NAME_NOT_FOUND;
}


status_t
SpeechNetwork::FindOutput(const char* name, speech_tensor* _tensor)
{
	for (int32 i = 0; i < CountOutputs(); i++) {
		speech_tensor tensor;
		if (GetOutput(i, &tensor) == B_OK && tensor.name != NULL
			&& strcmp(tensor.name, name) == 0) {
			*_tensor = tensor;
			return B_OK;
		}
	}
	return B_NAME_NOT_FOUND;
}


SpeechListener::~SpeechListener()
{
}


// #pragma mark - SpeechEngine


SpeechEngine::SpeechEngine()
	:
	fModel(NULL),
	fDecoder(NULL),
	fListener(NULL),
	fCancel(NULL),
	fLanguageToken(-1),
	fTimestamps(true),
	fAudioStart(0),
	fAudioEnd(0),
	fSeek(0),
	fMel(NULL)
{
	fError[0] = '\0';
	memset(&fInfo, 0, sizeof(fInfo));
}


SpeechEngine::~SpeechEngine()
{
	delete fDecoder;
	free(fMel);
}


status_t
SpeechEngine::Init(const SpeechModel* model, SpeechNetwork* encoder,
	SpeechNetwork* decoder)
{
	if (fModel != NULL)
		return B_NOT_ALLOWED;
	if (model == NULL)
		return B_BAD_VALUE;

	const SpeechModelInfo& info = model->Info();
	fDecoder = speech_create_decoder(info.family);
	if (fDecoder == NULL) {
		snprintf(fError, sizeof(fError), "models of the family \"%s\" are "
			"not supported", info.family);
		return B_NOT_SUPPORTED;
	}

	status_t status = fDecoder->Init(*model, encoder, decoder);
	if (status != B_OK) {
		snprintf(fError, sizeof(fError), "%s", fDecoder->Error());
		return status;
	}

	status = fFrontEnd.Init(info.fft_size, info.hop, info.mel_bins,
		info.window_frames, model->MelFilters());
	if (status != B_OK)
		return status;

	fMel = (float*)malloc((size_t)info.mel_bins * info.window_frames
		* sizeof(float));
	if (fMel == NULL)
		return B_NO_MEMORY;

	fModel = model;
	// a model of many languages must be told one
	if (info.multilingual)
		fLanguageToken = model->LanguageToken("en");
	return B_OK;
}


status_t
SpeechEngine::SetLanguage(const char* code)
{
	if (fModel == NULL)
		return B_NO_INIT;
	if (!fModel->Info().multilingual) {
		// the one language such a model has
		return code == NULL || strcmp(code, "en") == 0
			? B_OK : B_NAME_NOT_FOUND;
	}

	int32 token = fModel->LanguageToken(code);
	if (token < 0)
		return B_NAME_NOT_FOUND;
	fLanguageToken = token;
	return B_OK;
}


void
SpeechEngine::Reset()
{
	fAudio.MakeEmpty();
	fAudioStart = 0;
	fAudioEnd = 0;
	fSeek = 0;
	memset(&fInfo, 0, sizeof(fInfo));
}


status_t
SpeechEngine::AddSamples(const float* samples, size_t count)
{
	if (fModel == NULL)
		return B_NO_INIT;
	status_t status = fAudio.Append(samples, count);
	if (status == B_OK)
		fAudioEnd += count;
	return status;
}


double
SpeechEngine::PendingSeconds() const
{
	if (fModel == NULL)
		return 0;
	const SpeechModelInfo& info = fModel->Info();
	int64 pending = fAudioEnd - fSeek * info.hop;
	return pending > 0 ? (double)pending / info.sample_rate : 0;
}


status_t
SpeechEngine::_DecodeWindow(int32 contentFrames,
	speech_window_result* _result)
{
	const SpeechModelInfo& info = fModel->Info();

	bigtime_t start = system_time();
	status_t status = fFrontEnd.Compute(fAudio.Samples(), fAudio.Count(),
		fAudioStart, fSeek * info.hop, fMel);
	if (status != B_OK)
		return status;
	fInfo.front_end_time += system_time() - start;

	speech_decode_options options;
	options.language_token = fLanguageToken;
	options.timestamps = fTimestamps;
	options.content_frames = contentFrames;
	options.cancel = fCancel;
	status = fDecoder->DecodeWindow(fMel, options, _result);
	if (status != B_OK)
		return status;

	fInfo.windows++;
	fInfo.tokens += _result->token_count;
	fInfo.encoder_time += _result->encoder_time;
	fInfo.decoder_time += _result->decoder_time;
	return B_OK;
}


status_t
SpeechEngine::_Deliver(const speech_window_result& result, bool final)
{
	if (fListener == NULL)
		return B_OK;

	const SpeechModelInfo& info = fModel->Info();
	bigtime_t offset = fSeek * info.hop * 1000000LL / info.sample_rate;

	for (int32 i = 0; i < result.segment_count; i++) {
		const speech_window_segment& segment = result.segments[i];
		if (segment.end_ms <= segment.start_ms)
			continue;

		char* text;
		status_t status = fModel->DecodeTokens(
			result.tokens + segment.first_token, segment.token_count, &text);
		if (status != B_OK)
			return status;

		// a phrase of nothing but spaces is none
		bool empty = true;
		for (const char* at = text; *at != '\0'; at++) {
			if (!isspace((unsigned char)*at)) {
				empty = false;
				break;
			}
		}
		if (!empty) {
			fListener->SegmentRecognized(
				offset + (bigtime_t)segment.start_ms * 1000,
				offset + (bigtime_t)segment.end_ms * 1000, text, final);
		}
		free(text);
	}
	return B_OK;
}


status_t
SpeechEngine::Process(bool finish)
{
	if (fModel == NULL)
		return B_NO_INIT;

	const SpeechModelInfo& info = fModel->Info();
	while (true) {
		if (fCancel != NULL && *fCancel != 0)
			return B_CANCELED;

		// frames of the recording so far
		int64 content = fAudioEnd / info.hop;
		if (fSeek >= content)
			break;

		// While the recording goes on, a window is done when its audio is
		// there, and what the last transform reaches beyond it.
		if (!finish && fAudioEnd < (fSeek + info.window_frames) * info.hop
				+ info.fft_size / 2) {
			break;
		}

		int64 left = content - fSeek;
		speech_window_result result;
		status_t status = _DecodeWindow(left < info.window_frames
			? (int32)left : info.window_frames, &result);
		if (status == B_OK)
			status = _Deliver(result, true);
		if (status != B_OK)
			return status;

		fSeek += result.advance_frames;

		// audio before the next window is done with, but for what its
		// first transforms reach back to
		int64 keep = fSeek * info.hop - info.fft_size;
		if (keep > fAudioEnd)
			keep = fAudioEnd;
		if (keep > fAudioStart) {
			fAudio.RemoveFirst(keep - fAudioStart);
			fAudioStart = keep;
		}
	}
	return B_OK;
}


status_t
SpeechEngine::ProcessInterim()
{
	if (fModel == NULL)
		return B_NO_INIT;

	const SpeechModelInfo& info = fModel->Info();
	int64 left = fAudioEnd / info.hop - fSeek;
	if (left <= 0)
		return B_OK;

	speech_window_result result;
	status_t status = _DecodeWindow(left < info.window_frames
		? (int32)left : info.window_frames, &result);
	if (status != B_OK)
		return status;
	return _Deliver(result, false);
}


}	// namespace BPrivate
