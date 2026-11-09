/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _SPEECH_RECOGNIZER_H
#define _SPEECH_RECOGNIZER_H


#include <MediaDefs.h>
#include <Messenger.h>
#include <String.h>
#include <neural/NeuralDefs.h>
#include <neural/NeuralRoster.h>

class BPath;
class BStringList;

namespace BPrivate {
	class SpeechRecognizerData;
}


//! One phrase of a transcript.
struct speech_segment {
								speech_segment();

	//! from the beginning of the recording, in microseconds
	bigtime_t			start;
	bigtime_t			end;
	BString				text;
	uint32				_reserved[4];
};


//! BSpeechRecognizer::GetInfo()
struct speech_recognizer_info {
	//! audio of the recording recognized so far, in microseconds
	bigtime_t			audio_duration;
	//! time spent recognizing it: all of it, and by part
	bigtime_t			processing_time;
	bigtime_t			front_end_time;
	bigtime_t			encoder_time;
	bigtime_t			decoder_time;
	//! windows of audio the model was run on, and tokens it wrote
	int32				windows;
	int32				tokens;
	uint32				_reserved[8];
};


/*!	Turns speech into text with a speech model (a Whisper model in the
	kit's form: see tools/airos/neural/fetch-models.sh) on this machine.

	Audio goes in as it is recorded, in any raw format of the Media Kit;
	text comes out as messages, without the caller ever waiting for the
	model:

		BSpeechRecognizer recognizer;
		recognizer.SetTo();                     // the model that is installed
		recognizer.SetTarget(BMessenger(this));
		recognizer.Start(&format);              // a media_raw_audio_format
		// for every buffer the recording gives:
		recognizer.Feed(buffer, size);
		// when it is over:
		recognizer.Finish();

		void MyWindow::MessageReceived(BMessage* message)
		{
			if (message->what == B_SPEECH_RESULT)
				// message->GetString("speech:text"), "speech:final", ...

	or all at once, for a recording that is there already:

		BString text;
		recognizer.Transcribe(samples, count, &text);

	The model works on windows of 30 seconds and tells where the phrases
	in a window end; a phrase is final when the model has seen its end.
	With SetInterimInterval() the recognizer also reads the window that
	is still filling every so often and sends that as results that are
	not final.

	A recognizer has a thread of its own, which does the work; its
	methods may be called from any thread (the one that records can
	Feed() while another one sets things and deletes it). Deleting it is
	safe at any time and waits for the model to finish the step it is in.
*/
class BSpeechRecognizer {
public:
								BSpeechRecognizer();
								BSpeechRecognizer(const char* model,
									uint32 flags = 0);
	virtual						~BSpeechRecognizer();

	/*!	Loads a model: \a model is the name of one ("whisper-tiny.en":
		a directory under neural/models of the data directories), the
		path of a model directory, or NULL for the first one FindModel()
		finds. The device is chosen as for a BNeuralSession; \a flags
		are B_NEURAL_CPU_ONLY and the others of BNeuralSession. */
			status_t			SetTo(const char* model = NULL,
									uint32 flags = 0);
			status_t			SetTo(const char* model,
									const BNeuralDevice& device,
									uint32 flags = 0);
			void				Unset();
			status_t			InitCheck() const;
	//! More about why SetTo() failed, for developers.
			const char*			ErrorDetails() const;

	/*!	The directory of an installed model; NULL finds the first there
		is. B_ENTRY_NOT_FOUND when none is installed. */
	static	status_t			FindModel(const char* name,
									BPath* _directory);
	//! The names of the installed models.
	static	status_t			GetModels(BStringList* names);

	//! "tiny.en": what the model calls itself.
			const char*			ModelName() const;
	//! The device the model's encoder, the larger part, was put on.
			const BNeuralDevice& Device() const;

	/*!	The languages the model knows, as ISO 639-1 codes; a model for
		one language has just that one. */
			status_t			GetLanguages(BStringList* codes) const;
	/*!	The language that is spoken; "en" to begin with. Takes effect
		with the next Start(). B_NAME_NOT_FOUND when the model does not
		know it. */
			status_t			SetLanguage(const char* code);
			const char*			Language() const;

	/*!	Whether the model is asked for the times of its phrases (the
		default). Without them a window is one phrase, and a recording
		longer than a window is cut where the windows end, not between
		phrases. Takes effect with the next Start(). */
			void				SetTimestamps(bool enabled);
			bool				Timestamps() const;

	/*!	Where results go: B_SPEECH_RESULT and B_SPEECH_FINISHED (see
		NeuralDefs.h), or \a message with their fields added and
		"speech:finished" (bool) to tell the two apart. */
			status_t			SetTarget(const BMessenger& target,
									const BMessage* message = NULL);
	/*!	How often, at most, the audio that is not final yet is read for
		a result that is not final; 0 (the default): never. A reading
		takes as long as a whole window does (see GetInfo()). */
			void				SetInterimInterval(bigtime_t interval);
			bigtime_t			InterimInterval() const;

	// a recording, as it comes in

	/*!	Begins a recording in the given format (NULL: 16000 samples per
		second, one channel, float, as Transcribe() takes them). One
		that was going on is dropped. \a _token gets a number that comes
		back in the messages of this recording. */
			status_t			Start(const media_raw_audio_format* format
										= NULL,
									int32* _token = NULL);
	/*!	More audio of the recording; returns at once. B_BUSY when ten
		minutes of audio are waiting for the model: the machine does not
		keep up. */
			status_t			Feed(const void* buffer, size_t size);
	/*!	The recording is over: what is left of it is recognized, then
		B_SPEECH_FINISHED is sent. Returns at once. */
			status_t			Finish();
	//! Drops the recording; no more results of it are sent.
			void				Cancel();
	/*!	Waits until a recording that was Finish()ed is done with and
		returns how it went. */
			status_t			WaitUntilFinished(
									bigtime_t timeout = B_INFINITE_TIMEOUT);
	//! Whether there is a recording that is not done with.
			bool				IsRecognizing() const;

	// a recording that is complete

	/*!	Recognizes a whole recording and returns when that is done:
		samples at 16000 per second, one channel. No messages are sent.
		B_BUSY while a recording is going on. */
			status_t			Transcribe(const float* samples,
									size_t count, BString* _text);
	//! The same for audio of any raw format.
			status_t			Transcribe(
									const media_raw_audio_format& format,
									const void* buffer, size_t size,
									BString* _text);

	/*!	The final phrases of the recording so far (of the last
		Transcribe(), or since Start()). */
			int32				CountSegments() const;
			status_t			GetSegment(int32 index,
									speech_segment* _segment) const;

			status_t			GetInfo(speech_recognizer_info* info) const;

private:
								BSpeechRecognizer(const BSpeechRecognizer&);
			BSpeechRecognizer&	operator=(const BSpeechRecognizer&);

	virtual	void				_ReservedSpeechRecognizer1();
	virtual	void				_ReservedSpeechRecognizer2();
	virtual	void				_ReservedSpeechRecognizer3();
	virtual	void				_ReservedSpeechRecognizer4();

			BPrivate::SpeechRecognizerData* fData;
			status_t			fStatus;
			BString				fError;
			BNeuralDevice		fDevice;
			uint32				_reserved[6];
};


#endif	// _SPEECH_RECOGNIZER_H
