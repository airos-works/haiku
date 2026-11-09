/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SPEECH_CORE_H
#define _SPEECH_CORE_H


/*!	What BSpeechRecognizer is made of: the description of a speech model
	and its vocabulary, the front end that turns samples into the
	spectrogram a model wants, the conversion of raw audio to the samples
	the front end wants, and the engine that cuts a recording into windows
	and has them decoded. Like the graph and the interpreter, all of this
	depends on nothing but the C library, so that it can be built and
	tested on the build host against a reference (src/tests/kits/neural/
	host); messages, Media Kit formats and the search for models are
	BSpeechRecognizer's.

	What is particular to a family of models (how its decoder is driven,
	what its tokens mean) is behind SpeechDecoder; the one family so far
	is Whisper (SpeechWhisper.cpp).
*/


#include <neural/NeuralDefs.h>


namespace BPrivate {


// #pragma mark - the model's description


//! speech.conf, see tools/airos/neural/whisper_convert.py
struct SpeechModelInfo {
	char	family[32];
	char	name[64];
	// file names, relative to the model's directory
	char	encoder[64];
	char	decoder[64];
	char	vocabulary[64];
	char	mel_filters[64];

	// the front end
	int32	sample_rate;
	int32	fft_size;
	int32	hop;
	int32	mel_bins;
	int32	window_frames;

	// the decoder
	int32	text_context;
	int32	max_tokens;
	int32	timestamp_ms;
	int32	vocabulary_size;
	int32	token_eot;
	int32	token_sot;
	int32	token_translate;
	int32	token_transcribe;
	int32	token_no_speech;
	int32	token_no_timestamps;
	int32	token_timestamp_begin;
	int32	token_blank;
	bool	multilingual;
};


struct SpeechLanguage {
	char	code[8];
	int32	token;
};


/*!	A model directory read and checked: the description, the vocabulary
	and the filter bank. The networks themselves are loaded by whoever
	runs them. */
class SpeechModel {
public:
								SpeechModel();
								~SpeechModel();

			status_t			Load(const char* directory);
			void				Unset();
	//! What was wrong with the directory, for developers.
			const char*			Error() const { return fError; }

			const SpeechModelInfo& Info() const { return fInfo; }
			const char*			Directory() const { return fDirectory; }
	//! The path of one of the model's files; false when it is too long.
			bool				GetPath(const char* file, char* path,
									size_t size) const;

	//! [mel_bins][fft_size / 2 + 1]
			const float*		MelFilters() const { return fFilters; }

			int32				CountSuppressed() const
									{ return fSuppressCount; }
			int32				SuppressedAt(int32 index) const
									{ return fSuppress[index]; }

			int32				CountLanguages() const
									{ return fLanguageCount; }
			const SpeechLanguage& LanguageAt(int32 index) const
									{ return fLanguages[index]; }
	//! The token of a language, -1 when the model has no such language.
			int32				LanguageToken(const char* code) const;

	/*!	The text of a sequence of tokens: the bytes of the text tokens
		joined (special tokens and time stamps are skipped) and made
		valid UTF-8, since a character may be cut by the end of a
		sequence. The caller free()s the string. */
			status_t			DecodeTokens(const int32* tokens,
									int32 count, char** _text) const;

private:
			status_t			_LoadDescription(const char* path);
			status_t			_LoadVocabulary(const char* path);
			status_t			_LoadFilters(const char* path);
			status_t			_Check();
			void				_SetError(const char* format, ...);

			SpeechModelInfo		fInfo;
			char				fDirectory[1024];
			char				fError[160];
			float*				fFilters;
			int32*				fSuppress;
			int32				fSuppressCount;
			SpeechLanguage*		fLanguages;
			int32				fLanguageCount;
	//! all tokens' bytes one after the other, and where each starts
			uint8*				fTokenBytes;
			uint32*				fTokenOffsets;
			int32				fTokenCount;
};


/*!	Appends \a length bytes to a string that is to be valid UTF-8: every
	stretch that is not is replaced by U+FFFD, the way Unicode recommends
	it (one replacement per longest stretch that could have begun a
	character). \a output must have room for 3 * length + 1 bytes; returns
	the bytes written, without the terminating zero. */
size_t speech_sanitize_utf8(const uint8* bytes, size_t length, char* output);


// #pragma mark - audio


//! A growing array of samples.
class SpeechSamples {
public:
								SpeechSamples();
								~SpeechSamples();

			status_t			Append(const float* samples, size_t count);
	//! Room for \a count more samples at the end; NULL without memory.
			float*				Grow(size_t count);
	//! Removes the first \a count samples.
			void				RemoveFirst(size_t count);
			void				MakeEmpty();

			const float*		Samples() const { return fSamples; }
			size_t				Count() const { return fCount; }

private:
			float*				fSamples;
			size_t				fCount;
			size_t				fCapacity;
};


//! How samples are stored; the values are the Media Kit's.
enum speech_sample_format {
	B_SPEECH_SAMPLE_INT8 = 0x1,
	B_SPEECH_SAMPLE_UINT8 = 0x11,
	B_SPEECH_SAMPLE_INT16 = 0x2,
	B_SPEECH_SAMPLE_INT32 = 0x4,
	B_SPEECH_SAMPLE_FLOAT = 0x24,
	B_SPEECH_SAMPLE_DOUBLE = 0x28
};


/*!	Turns raw audio (any of the formats above, any number of channels,
	any rate) into what a speech model takes: one channel of floating
	point samples at the model's rate. Works on a stream: buffers may end
	anywhere, also in the middle of a frame.

	The rate is changed with a windowed sinc filter (16 zero crossings to
	each side, Kaiser window), which also removes what the lower rate
	cannot carry.
*/
class SpeechAudioConverter {
public:
								SpeechAudioConverter();
								~SpeechAudioConverter();

			status_t			SetFormat(uint32 format, int32 channels,
									double frameRate, bool swapBytes,
									int32 targetRate);
			void				Reset();

	//! Appends what the bytes give to \a output.
			status_t			Convert(const void* data, size_t size,
									SpeechSamples& output);
	/*!	The samples that were waiting for audio that is not coming any
		more. */
			status_t			Flush(SpeechSamples& output);

private:
			status_t			_AddMono(const float* samples, size_t count,
									SpeechSamples& output);
			status_t			_Resample(bool flush, SpeechSamples& output);

			uint32				fFormat;
			int32				fChannels;
			int32				fSampleSize;
			bool				fSwapBytes;
			double				fRatio;
				// input samples per output sample; 1: nothing to do
			double				fScale;
			int32				fHalfWidth;
			float*				fFilter;
	//! a frame cut by the end of a buffer
			uint8				fPartial[64];
			size_t				fPartialSize;
	//! input not used up yet, and the number of its first sample
			SpeechSamples		fInput;
			int64				fInputStart;
			int64				fOutputCount;
};


/*!	Reads a RIFF/WAVE file (PCM of 8, 16, 24 as 32, or 32 bits, or
	floating point; any number of channels up to 8, any rate) and appends
	it, converted to one channel at \a targetRate, to \a output.
	B_BAD_DATA for a file that is not such a file. */
status_t speech_read_wave_file(const char* path, int32 targetRate,
	SpeechSamples& output);


/*!	The front end of a speech model: the log-mel spectrogram of a window
	of audio, as Whisper defines it (a Hann window of fft_size samples
	every hop samples, the power spectrum through the mel filter bank,
	the logarithm to base 10, nothing quieter than 8 below the loudest,
	scaled). */
class SpeechFrontEnd {
public:
								SpeechFrontEnd();
								~SpeechFrontEnd();

			status_t			Init(int32 fftSize, int32 hop, int32 melBins,
									int32 frames, const float* filters);

	/*!	The spectrogram ([mel_bins][frames], a row per filter) of the
		window that begins at sample \a first of a recording.
		\a audio are \a count samples of the recording from its sample
		\a audioStart on. The transforms reach half their size to both
		sides of the window: before the beginning of the recording the
		beginning is taken mirrored, and what is not in \a audio counts
		as silence. */
			status_t			Compute(const float* audio, int64 count,
									int64 audioStart, int64 first,
									float* mel) const;

private:
			struct Plan;
	static	void				_Frames(void* cookie, int32 first, int32 end);

			int32				fFFTSize;
			int32				fHop;
			int32				fMelBins;
			int32				fFrames;
			int32				fBins;
			Plan*				fPlan;
			double*				fWindow;
	//! per filter: the first bin it takes, how many, and their weights
			int32*				fFilterFirst;
			int32*				fFilterCount;
			float*				fFilters;
};


// #pragma mark - networks


struct speech_tensor {
	const char*			name;
	neural_data_type	type;
	int32				rank;
	int32				dimensions[B_NEURAL_MAX_RANK];
	//! the tensor's memory, which stays where it is between runs
	void*				data;
	size_t				size;
};


/*!	A network that can be run: a BNeuralSession in the kit, the bare
	interpreter in the host tests. */
class SpeechNetwork {
public:
	virtual						~SpeechNetwork();

	virtual	int32				CountInputs() const = 0;
	virtual	status_t			GetInput(int32 index,
									speech_tensor* _tensor) = 0;
	virtual	int32				CountOutputs() const = 0;
	virtual	status_t			GetOutput(int32 index,
									speech_tensor* _tensor) = 0;
	virtual	status_t			Run() = 0;

	//! B_NAME_NOT_FOUND when there is no such tensor.
			status_t			FindInput(const char* name,
									speech_tensor* _tensor);
			status_t			FindOutput(const char* name,
									speech_tensor* _tensor);
};


// #pragma mark - decoding


struct speech_decode_options {
	//! the language's token (multilingual models), or -1
	int32				language_token;
	//! have the model say where its phrases begin and end
	bool				timestamps;
	//! frames of the window that hold audio (the rest is padding)
	int32				content_frames;
	//! set by another thread to make the decoder stop; may be NULL
	const int32*		cancel;
};


struct speech_window_segment {
	//! from the beginning of the window
	int32				start_ms;
	int32				end_ms;
	//! which of the window's tokens are its text
	int32				first_token;
	int32				token_count;
};


//! What a decoder made of a window; its memory is the decoder's.
struct speech_window_result {
	const int32*		tokens;
	int32				token_count;
	const speech_window_segment* segments;
	int32				segment_count;
	//! where the next window is to begin, in frames from this one's start
	int32				advance_frames;
	//! the model's own estimate that there is no speech in the window
	float				no_speech;
	//! mean of the logarithms of the tokens' probabilities
	float				mean_log_probability;
	//! the window was taken as silence: no segments
	bool				silent;
	bigtime_t			encoder_time;
	bigtime_t			decoder_time;
};


/*!	The part that knows a family of models: runs encoder and decoder on
	a spectrogram and says what was said in it. */
class SpeechDecoder {
public:
	virtual						~SpeechDecoder();

	/*!	Checks that the networks are what the description says (their
		inputs and outputs by name, type and size): a model directory is
		input from anywhere. The model and the networks must outlive the
		decoder. */
	virtual	status_t			Init(const SpeechModel& model,
									SpeechNetwork* encoder,
									SpeechNetwork* decoder) = 0;
	virtual	const char*			Error() const = 0;

	/*!	B_CANCELED when the cancel flag was set. */
	virtual	status_t			DecodeWindow(const float* mel,
									const speech_decode_options& options,
									speech_window_result* _result) = 0;
};


//! NULL for a family the kit does not know.
SpeechDecoder* speech_create_decoder(const char* family);


class SpeechListener {
public:
	virtual						~SpeechListener();

	/*!	A phrase was recognized. Times are microseconds from the start of
		the recording. A phrase that is not final is the engine's look at
		audio it will go over again when more has come. */
	virtual	void				SegmentRecognized(bigtime_t start,
									bigtime_t end, const char* text,
									bool final) = 0;
};


struct speech_engine_info {
	int32				windows;
	int32				tokens;
	bigtime_t			front_end_time;
	bigtime_t			encoder_time;
	bigtime_t			decoder_time;
};


/*!	Holds the audio of a recording as it comes in, cuts it into windows
	and hands what the decoder finds to a listener. Not thread safe: one
	thread feeds and runs it. */
class SpeechEngine {
public:
								SpeechEngine();
								~SpeechEngine();

	//! The model and the networks must outlive the engine.
			status_t			Init(const SpeechModel* model,
									SpeechNetwork* encoder,
									SpeechNetwork* decoder);
			const char*			Error() const { return fError; }

			void				SetListener(SpeechListener* listener)
									{ fListener = listener; }
			void				SetCancelFlag(const int32* flag)
									{ fCancel = flag; }
	//! B_NAME_NOT_FOUND for a language the model does not have.
			status_t			SetLanguage(const char* code);
			void				SetTimestamps(bool timestamps)
									{ fTimestamps = timestamps; }

	//! Forgets the recording: the next samples begin a new one.
			void				Reset();
	//! Samples at the model's rate, one channel.
			status_t			AddSamples(const float* samples,
									size_t count);
	//! Seconds of audio that are not recognized for good yet.
			double				PendingSeconds() const;

	/*!	Recognizes the windows that are complete; with \a finish all that
		is left, the recording being over. Each phrase goes to the
		listener as a final one. */
			status_t			Process(bool finish);
	/*!	Recognizes what is there of the window that is still filling
		and hands it to the listener as phrases that are not final. */
			status_t			ProcessInterim();

			const speech_engine_info& Info() const { return fInfo; }

private:
			status_t			_DecodeWindow(int32 contentFrames,
									speech_window_result* _result);
			status_t			_Deliver(const speech_window_result& result,
									bool final);

			const SpeechModel*	fModel;
			SpeechDecoder*		fDecoder;
			SpeechFrontEnd		fFrontEnd;
			SpeechListener*		fListener;
			const int32*		fCancel;
			int32				fLanguageToken;
			bool				fTimestamps;
			char				fError[160];

			SpeechSamples		fAudio;
	//! number (in the recording) of the first sample kept
			int64				fAudioStart;
	//! samples of the recording so far
			int64				fAudioEnd;
	//! frame at which the next window begins
			int64				fSeek;
			float*				fMel;
			speech_engine_info	fInfo;
};


}	// namespace BPrivate


#endif	// _SPEECH_CORE_H
