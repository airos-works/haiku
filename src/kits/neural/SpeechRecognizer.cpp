/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	BSpeechRecognizer: the speech engine (SpeechCore.h) with what an
	application needs around it: models found in the data directories and
	run by sessions, Media Kit formats, a thread that does the work and
	messages that carry the results.

	The thread is the only one that touches the engine. What the caller's
	threads leave for it (samples, "the recording is over", "another
	recording") is kept under a lock that is never held while a network
	runs, so that no call here waits for the model.
*/


#include <neural/SpeechRecognizer.h>

#include <errno.h>
#include <new>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include <ByteOrder.h>
#include <Directory.h>
#include <Entry.h>
#include <List.h>
#include <Message.h>
#include <Path.h>
#include <PathFinder.h>
#include <StringList.h>
#include <neural/NeuralModel.h>
#include <neural/NeuralSession.h>

#include <SpeechCore.h>


namespace BPrivate {


// A target whose port is full gets some time, not forever: deleting the
// recognizer waits for the thread that sends.
static const bigtime_t kSendTimeout = 2000000;
// what Feed() lets pile up before it says that the machine does not
// keep up: ten minutes
static const int64 kMaxPendingSeconds = 600;


/*!	A session as the engine sees a network. */
class SessionNetwork : public SpeechNetwork {
public:
	status_t SetTo(const char* path, const BNeuralDevice* device,
		uint32 flags, BString& error)
	{
		status_t status = fModel.SetTo(path);
		if (status != B_OK) {
			error.SetToFormat("%s: %s", path, fModel.ErrorDetails()[0] != '\0'
				? fModel.ErrorDetails() : neural_error_string(status));
			return status;
		}

		if (device != NULL)
			status = fSession.SetTo(fModel, *device, flags);
		else
			status = fSession.SetTo(fModel, flags);
		if (status != B_OK) {
			error.SetToFormat("%s: no session: %s", path,
				neural_error_string(status));
		}
		return status;
	}

	const BNeuralDevice& Device() const
	{
		return fSession.Device();
	}

	virtual int32 CountInputs() const
	{
		return fSession.CountInputs();
	}

	virtual status_t GetInput(int32 index, speech_tensor* _tensor)
	{
		BTensor* tensor = fSession.InputAt(index);
		if (tensor == NULL)
			return B_BAD_INDEX;
		_Describe(*tensor, tensor->Data(), _tensor);
		return B_OK;
	}

	virtual int32 CountOutputs() const
	{
		return fSession.CountOutputs();
	}

	virtual status_t GetOutput(int32 index, speech_tensor* _tensor)
	{
		const BTensor* tensor = fSession.OutputAt(index);
		if (tensor == NULL)
			return B_BAD_INDEX;
		// (the engine reads outputs and writes inputs; one structure
		// describes both)
		_Describe(*tensor, const_cast<void*>(tensor->Data()), _tensor);
		return B_OK;
	}

	virtual status_t Run()
	{
		return fSession.Run();
	}

private:
	static void _Describe(const BTensor& tensor, void* data,
		speech_tensor* _tensor)
	{
		_tensor->name = tensor.Info().Name();
		_tensor->type = tensor.Type();
		_tensor->rank = tensor.Rank();
		for (int32 i = 0; i < B_NEURAL_MAX_RANK; i++)
			_tensor->dimensions[i] = tensor.DimensionAt(i);
		_tensor->data = data;
		_tensor->size = tensor.ByteSize();
	}

	BNeuralModel	fModel;
	BNeuralSession	fSession;
};


class SpeechRecognizerData : public SpeechListener {
public:
								SpeechRecognizerData(
									BSpeechRecognizer* owner);
	virtual						~SpeechRecognizerData();

			status_t			Init(const char* directory,
									const BNeuralDevice* device,
									uint32 flags, BString& error);
			void				Quit();

			const SpeechModel&	Model() const { return fModel; }
			const BNeuralDevice& Device() const
									{ return fEncoder.Device(); }

			status_t			SetLanguage(const char* code);
			const char*			Language() const { return fLanguage; }
			void				SetTimestamps(bool enabled);
			bool				Timestamps() const { return fTimestamps; }
			void				SetTarget(const BMessenger& target,
									const BMessage* message);
			void				SetInterimInterval(bigtime_t interval);
			bigtime_t			InterimInterval() const
									{ return fInterimInterval; }

			status_t			Start(const media_raw_audio_format* format,
									bool quiet, int32* _token);
			status_t			Feed(const void* buffer, size_t size);
			status_t			Finish();
			void				Cancel();
			status_t			WaitUntilFinished(bigtime_t timeout);
			bool				IsRecognizing();

			int32				CountSegments();
			status_t			GetSegment(int32 index,
									speech_segment* _segment);
			status_t			GetText(BString* _text);
			void				GetInfo(speech_recognizer_info* info);

	// SpeechListener, on the recognizer's thread
	virtual	void				SegmentRecognized(bigtime_t start,
									bigtime_t end, const char* text,
									bool final);

private:
	static	status_t			_ThreadEntry(void* data);
			void				_Work();
			void				_BeginRecording();
			status_t			_Recognize(bool finish, bool interim);
			void				_Send(bool finished, status_t status,
									const speech_segment* segment,
									bool final);
			void				_UpdateInfo();
			void				_ClearSegments();

			BSpeechRecognizer*	fOwner;
			SpeechModel			fModel;
			SessionNetwork		fEncoder;
			SessionNetwork		fDecoder;
			SpeechEngine		fEngine;
			thread_id			fThread;

	// everything below is guarded by fLock, except where it says that
	// only the recognizer's thread uses it
			pthread_mutex_t		fLock;
			pthread_cond_t		fWork;
				// something for the thread to do
			pthread_cond_t		fDone;
				// a recording is done with
			bool				fQuit;

	// settings
			char				fLanguage[8];
			bool				fTimestamps;
			BMessenger			fTarget;
			BMessage			fMessage;
			bool				fOwnMessage;
			bigtime_t			fInterimInterval;

	// what the callers leave for the thread
			SpeechAudioConverter fConverter;
			SpeechSamples		fPending;
			int32				fToken;
			bool				fRestart;
			bool				fFinish;
			bool				fQuiet;
	//! set to make the engine stop what it is doing; read without the lock
			int32				fCancel;

	// the state of the recording
			bool				fActive;
			int32				fActiveToken;
			status_t			fResult;
			BList				fSegments;
			speech_recognizer_info fInfo;
	//! samples the engine has that are not recognized for good yet
			int64				fBacklog;

	// used by the recognizer's thread only
			bool				fQuietNow;
			int64				fSamplesTaken;
			int64				fSamplesAtInterim;
			bigtime_t			fLastInterim;
			bigtime_t			fProcessingTime;
			BString				fInterimText;
			bigtime_t			fInterimStart;
			bigtime_t			fInterimEnd;
};


class Locker {
public:
	Locker(pthread_mutex_t& lock)
		:
		fLock(lock)
	{
		pthread_mutex_lock(&fLock);
	}

	~Locker()
	{
		pthread_mutex_unlock(&fLock);
	}

private:
	pthread_mutex_t&	fLock;
};


SpeechRecognizerData::SpeechRecognizerData(BSpeechRecognizer* owner)
	:
	fOwner(owner),
	fThread(-1),
	fQuit(false),
	fTimestamps(true),
	fOwnMessage(false),
	fInterimInterval(0),
	fToken(0),
	fRestart(false),
	fFinish(false),
	fQuiet(false),
	fCancel(0),
	fActive(false),
	fActiveToken(0),
	fResult(B_OK),
	fBacklog(0),
	fQuietNow(false),
	fSamplesTaken(0),
	fSamplesAtInterim(0),
	fLastInterim(0),
	fProcessingTime(0),
	fInterimStart(0),
	fInterimEnd(0)
{
	pthread_mutex_init(&fLock, NULL);
	pthread_cond_init(&fWork, NULL);
	pthread_cond_init(&fDone, NULL);
	strcpy(fLanguage, "en");
	memset(&fInfo, 0, sizeof(fInfo));
}


SpeechRecognizerData::~SpeechRecognizerData()
{
	Quit();
	_ClearSegments();
	pthread_cond_destroy(&fWork);
	pthread_cond_destroy(&fDone);
	pthread_mutex_destroy(&fLock);
}


status_t
SpeechRecognizerData::Init(const char* directory,
	const BNeuralDevice* device, uint32 flags, BString& error)
{
	status_t status = fModel.Load(directory);
	if (status != B_OK) {
		error.SetToFormat("%s: %s", directory, fModel.Error());
		return status;
	}

	const SpeechModelInfo& info = fModel.Info();
	char path[B_PATH_NAME_LENGTH + 64];
	if (!fModel.GetPath(info.encoder, path, sizeof(path)))
		return B_NAME_TOO_LONG;
	status = fEncoder.SetTo(path, device, flags, error);
	if (status != B_OK)
		return status;

	if (!fModel.GetPath(info.decoder, path, sizeof(path)))
		return B_NAME_TOO_LONG;
	// The decoder goes where the encoder went, so that a device that
	// takes part of the model has all of it to itself.
	BNeuralDevice encoderDevice(fEncoder.Device());
	status = fDecoder.SetTo(path, &encoderDevice, flags, error);
	if (status != B_OK)
		return status;

	status = fEngine.Init(&fModel, &fEncoder, &fDecoder);
	if (status != B_OK) {
		error.SetToFormat("%s: %s", directory, fEngine.Error());
		return status;
	}
	fEngine.SetListener(this);
	fEngine.SetCancelFlag(&fCancel);

	fThread = spawn_thread(&_ThreadEntry, "speech recognizer",
		B_NORMAL_PRIORITY, this);
	if (fThread < 0)
		return fThread;
	resume_thread(fThread);
	return B_OK;
}


/*!	Ends the thread and waits for it: it is out of the engine after the
	decoder's current step, or after the encoder's run, which cannot be
	cut short. */
void
SpeechRecognizerData::Quit()
{
	if (fThread < 0)
		return;

	pthread_mutex_lock(&fLock);
	fQuit = true;
	atomic_set(&fCancel, 1);
	pthread_cond_broadcast(&fWork);
	pthread_mutex_unlock(&fLock);

	// (a signal must not make this return before the thread is gone:
	// it uses this object)
	status_t result;
	while (wait_for_thread(fThread, &result) == B_INTERRUPTED)
		;
	fThread = -1;
}


status_t
SpeechRecognizerData::SetLanguage(const char* code)
{
	if (code == NULL || strlen(code) >= sizeof(fLanguage))
		return B_BAD_VALUE;
	if (fModel.Info().multilingual ? fModel.LanguageToken(code) < 0
			: strcmp(code, "en") != 0) {
		return B_NAME_NOT_FOUND;
	}

	Locker locker(fLock);
	strcpy(fLanguage, code);
	return B_OK;
}


void
SpeechRecognizerData::SetTimestamps(bool enabled)
{
	Locker locker(fLock);
	fTimestamps = enabled;
}


void
SpeechRecognizerData::SetTarget(const BMessenger& target,
	const BMessage* message)
{
	Locker locker(fLock);
	fTarget = target;
	fOwnMessage = message != NULL;
	if (message != NULL)
		fMessage = *message;
	else
		fMessage.MakeEmpty();
}


void
SpeechRecognizerData::SetInterimInterval(bigtime_t interval)
{
	Locker locker(fLock);
	fInterimInterval = interval > 0 ? interval : 0;
	pthread_cond_broadcast(&fWork);
}


status_t
SpeechRecognizerData::Start(const media_raw_audio_format* format, bool quiet,
	int32* _token)
{
	const SpeechModelInfo& info = fModel.Info();
	Locker locker(fLock);

	status_t status;
	if (format == NULL) {
		status = fConverter.SetFormat(B_SPEECH_SAMPLE_FLOAT, 1,
			info.sample_rate, false, info.sample_rate);
	} else {
		// the Media Kit's sample formats are the converter's
		status = fConverter.SetFormat(format->format, format->channel_count,
			format->frame_rate, format->byte_order != B_MEDIA_HOST_ENDIAN,
			info.sample_rate);
	}
	if (status != B_OK)
		return status;

	// whatever the thread is doing for the recording before is of no
	// use any more
	fPending.MakeEmpty();
	fRestart = true;
	fFinish = false;
	fQuiet = quiet;
	fActive = true;
	fResult = B_OK;
	fToken++;
	if (_token != NULL)
		*_token = fToken;
	atomic_set(&fCancel, 1);
	pthread_cond_broadcast(&fWork);
	return B_OK;
}


status_t
SpeechRecognizerData::Feed(const void* buffer, size_t size)
{
	Locker locker(fLock);
	if (!fActive || fFinish)
		return B_NOT_ALLOWED;
	// what waits here, and what the engine has and is not done with
	if ((int64)fPending.Count() + fBacklog
			> kMaxPendingSeconds * fModel.Info().sample_rate) {
		return B_BUSY;
	}

	status_t status = fConverter.Convert(buffer, size, fPending);
	pthread_cond_broadcast(&fWork);
	return status;
}


status_t
SpeechRecognizerData::Finish()
{
	Locker locker(fLock);
	if (!fActive)
		return B_NOT_ALLOWED;
	if (fFinish)
		return B_OK;

	status_t status = fConverter.Flush(fPending);
	fFinish = true;
	pthread_cond_broadcast(&fWork);
	return status;
}


void
SpeechRecognizerData::Cancel()
{
	Locker locker(fLock);
	if (!fActive)
		return;

	// A recording of nothing takes the place of this one: the thread
	// drops what it has, and results on their way are of another token.
	fPending.MakeEmpty();
	fRestart = true;
	fFinish = false;
	fActive = false;
	fResult = B_CANCELED;
	fToken++;
	atomic_set(&fCancel, 1);
	pthread_cond_broadcast(&fWork);
	pthread_cond_broadcast(&fDone);
}


status_t
SpeechRecognizerData::WaitUntilFinished(bigtime_t timeout)
{
	struct timespec end;
	if (timeout != B_INFINITE_TIMEOUT) {
		clock_gettime(CLOCK_REALTIME, &end);
		end.tv_sec += timeout / 1000000;
		end.tv_nsec += (timeout % 1000000) * 1000;
		if (end.tv_nsec >= 1000000000) {
			end.tv_sec++;
			end.tv_nsec -= 1000000000;
		}
	}

	Locker locker(fLock);
	if (fActive && !fFinish)
		return B_NOT_ALLOWED;
	while (fActive) {
		if (timeout == B_INFINITE_TIMEOUT) {
			pthread_cond_wait(&fDone, &fLock);
		} else if (pthread_cond_timedwait(&fDone, &fLock, &end)
				== ETIMEDOUT) {
			if (fActive)
				return B_TIMED_OUT;
		}
	}
	return fResult;
}


bool
SpeechRecognizerData::IsRecognizing()
{
	Locker locker(fLock);
	return fActive;
}


int32
SpeechRecognizerData::CountSegments()
{
	Locker locker(fLock);
	return fSegments.CountItems();
}


status_t
SpeechRecognizerData::GetSegment(int32 index, speech_segment* _segment)
{
	Locker locker(fLock);
	speech_segment* segment = (speech_segment*)fSegments.ItemAt(index);
	if (segment == NULL)
		return B_BAD_INDEX;
	*_segment = *segment;
	return B_OK;
}


//! The phrases with a space between them.
status_t
SpeechRecognizerData::GetText(BString* _text)
{
	Locker locker(fLock);
	_text->Truncate(0);
	for (int32 i = 0; i < fSegments.CountItems(); i++) {
		if (i > 0)
			*_text << ' ';
		*_text << ((speech_segment*)fSegments.ItemAt(i))->text;
	}
	return B_OK;
}


void
SpeechRecognizerData::GetInfo(speech_recognizer_info* info)
{
	Locker locker(fLock);
	*info = fInfo;
}


void
SpeechRecognizerData::_ClearSegments()
{
	for (int32 i = 0; i < fSegments.CountItems(); i++)
		delete (speech_segment*)fSegments.ItemAt(i);
	fSegments.MakeEmpty();
}


/*static*/ status_t
SpeechRecognizerData::_ThreadEntry(void* data)
{
	((SpeechRecognizerData*)data)->_Work();
	return B_OK;
}


/*!	Called with the lock held: the engine and the thread's own state are
	made ready for the recording Start() announced. */
void
SpeechRecognizerData::_BeginRecording()
{
	fRestart = false;
	atomic_set(&fCancel, 0);
	fActiveToken = fToken;
	fQuietNow = fQuiet;
	_ClearSegments();
	memset(&fInfo, 0, sizeof(fInfo));
	fBacklog = 0;

	fEngine.Reset();
	fEngine.SetLanguage(fLanguage);
	fEngine.SetTimestamps(fTimestamps);
	fSamplesTaken = 0;
	fSamplesAtInterim = 0;
	fLastInterim = system_time();
	fProcessingTime = 0;
}


void
SpeechRecognizerData::_Work()
{
	pthread_mutex_lock(&fLock);
	while (!fQuit) {
		if (fRestart)
			_BeginRecording();

		// what there is to do: new samples, the end of the recording,
		// or a look at what is not final yet
		bool finish = fActive && fFinish;
		bool interim = false;
		if (fActive && !finish && fInterimInterval > 0
			&& fSamplesTaken + (int64)fPending.Count() > fSamplesAtInterim) {
			bigtime_t due = fLastInterim + fInterimInterval;
			bigtime_t now = system_time();
			if (now >= due)
				interim = true;
			else if (fPending.Count() == 0) {
				// nothing but that to wait for
				struct timespec end;
				clock_gettime(CLOCK_REALTIME, &end);
				bigtime_t wait = due - now;
				end.tv_sec += wait / 1000000;
				end.tv_nsec += (wait % 1000000) * 1000;
				if (end.tv_nsec >= 1000000000) {
					end.tv_sec++;
					end.tv_nsec -= 1000000000;
				}
				pthread_cond_timedwait(&fWork, &fLock, &end);
				continue;
			}
		}

		if (!fActive || (fPending.Count() == 0 && !finish && !interim)) {
			pthread_cond_wait(&fWork, &fLock);
			continue;
		}

		// the samples change hands, and the model runs without the lock
		status_t status = fEngine.AddSamples(fPending.Samples(),
			fPending.Count());
		fSamplesTaken += fPending.Count();
		fPending.MakeEmpty();
		pthread_mutex_unlock(&fLock);

		if (status == B_OK)
			status = _Recognize(finish, interim);

		pthread_mutex_lock(&fLock);
		_UpdateInfo();
		if (fRestart || fQuit) {
			// the recording is not wanted any more, whatever became of it
			continue;
		}

		if (finish || status != B_OK) {
			fResult = status;
			fActive = false;
			fFinish = false;
			pthread_cond_broadcast(&fDone);
			pthread_mutex_unlock(&fLock);
			_Send(true, status, NULL, false);
			pthread_mutex_lock(&fLock);
		}
	}
	pthread_mutex_unlock(&fLock);
}


//! On the recognizer's thread, without the lock.
status_t
SpeechRecognizerData::_Recognize(bool finish, bool interim)
{
	bigtime_t start = system_time();
	status_t status = fEngine.Process(finish);

	if (status == B_OK && interim && !finish
		&& fEngine.PendingSeconds() > 0) {
		// one result for all that is not final: the phrases of this
		// reading, joined
		fInterimText.Truncate(0);
		fInterimStart = -1;
		status = fEngine.ProcessInterim();
		if (status == B_OK && fInterimText.Length() > 0) {
			speech_segment segment;
			segment.start = fInterimStart;
			segment.end = fInterimEnd;
			segment.text = fInterimText;
			_Send(false, B_OK, &segment, false);
		}
	}
	if (interim) {
		fLastInterim = system_time();
		fSamplesAtInterim = fSamplesTaken;
	}

	fProcessingTime += system_time() - start;
	return status;
}


//! Called with the lock held.
void
SpeechRecognizerData::_UpdateInfo()
{
	const speech_engine_info& info = fEngine.Info();
	double pending = fEngine.PendingSeconds();
	fBacklog = (int64)(pending * fModel.Info().sample_rate);
	fInfo.audio_duration = fSamplesTaken * 1000000LL
		/ fModel.Info().sample_rate - (bigtime_t)(pending * 1e6);
	fInfo.processing_time = fProcessingTime;
	fInfo.front_end_time = info.front_end_time;
	fInfo.encoder_time = info.encoder_time;
	fInfo.decoder_time = info.decoder_time;
	fInfo.windows = info.windows;
	fInfo.tokens = info.tokens;
}


void
SpeechRecognizerData::SegmentRecognized(bigtime_t start, bigtime_t end,
	const char* text, bool final)
{
	// without the spaces the model begins its phrases with
	BString trimmed(text);
	trimmed.Trim();

	if (!final) {
		if (fInterimStart < 0)
			fInterimStart = start;
		fInterimEnd = end;
		if (fInterimText.Length() > 0)
			fInterimText << ' ';
		fInterimText << trimmed;
		return;
	}

	speech_segment* segment = new(std::nothrow) speech_segment;
	if (segment == NULL)
		return;
	segment->start = start;
	segment->end = end;
	segment->text = trimmed;

	pthread_mutex_lock(&fLock);
	// (results of a recording that was dropped meanwhile go nowhere)
	bool wanted = !fRestart && !fQuit;
	if (!wanted || !fSegments.AddItem(segment)) {
		pthread_mutex_unlock(&fLock);
		delete segment;
		return;
	}
	speech_segment copy(*segment);
	pthread_mutex_unlock(&fLock);

	_Send(false, B_OK, &copy, true);
}


/*!	A result or the end of the recording, to the target. On the
	recognizer's thread, without the lock. */
void
SpeechRecognizerData::_Send(bool finished, status_t status,
	const speech_segment* segment, bool final)
{
	pthread_mutex_lock(&fLock);
	bool wanted = !fQuietNow && !fRestart && !fQuit && fTarget.IsValid();
	BMessenger target(fTarget);
	BMessage message(fMessage);
	bool ownMessage = fOwnMessage;
	speech_recognizer_info info = fInfo;
	pthread_mutex_unlock(&fLock);
	if (!wanted)
		return;

	if (!ownMessage)
		message.what = finished ? B_SPEECH_FINISHED : B_SPEECH_RESULT;
	else
		message.AddBool("speech:finished", finished);
	message.AddInt32("speech:token", fActiveToken);
	message.AddPointer("speech:recognizer", fOwner);
	if (finished) {
		message.AddInt32("speech:status", status);
		message.AddInt64("speech:audio", info.audio_duration);
		message.AddInt64("speech:duration", info.processing_time);
	} else {
		message.AddString("speech:text", segment->text);
		message.AddBool("speech:final", final);
		message.AddInt64("speech:start", segment->start);
		message.AddInt64("speech:end", segment->end);
	}

	target.SendMessage(&message, (BHandler*)NULL, kSendTimeout);
}


}	// namespace BPrivate


using BPrivate::SpeechRecognizerData;


speech_segment::speech_segment()
	:
	start(0),
	end(0)
{
	memset(_reserved, 0, sizeof(_reserved));
}


// #pragma mark - BSpeechRecognizer


BSpeechRecognizer::BSpeechRecognizer()
	:
	fData(NULL),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
}


BSpeechRecognizer::BSpeechRecognizer(const char* model, uint32 flags)
	:
	fData(NULL),
	fStatus(B_NO_INIT)
{
	memset(_reserved, 0, sizeof(_reserved));
	SetTo(model, flags);
}


BSpeechRecognizer::~BSpeechRecognizer()
{
	Unset();
}


static status_t
set_recognizer(const char* model, const BNeuralDevice* device, uint32 flags,
	BSpeechRecognizer* owner, SpeechRecognizerData** _data, BString& error)
{
	BPath directory;
	status_t status = BSpeechRecognizer::FindModel(model, &directory);
	if (status != B_OK) {
		error = model != NULL ? "no such speech model is installed"
			: "no speech model is installed";
		return status;
	}

	SpeechRecognizerData* data = new(std::nothrow) SpeechRecognizerData(
		owner);
	if (data == NULL)
		return B_NO_MEMORY;

	status = data->Init(directory.Path(), device, flags, error);
	if (status != B_OK) {
		delete data;
		return status;
	}
	*_data = data;
	return B_OK;
}


status_t
BSpeechRecognizer::SetTo(const char* model, uint32 flags)
{
	Unset();
	fStatus = set_recognizer(model, NULL, flags, this, &fData, fError);
	if (fStatus == B_OK)
		fDevice = fData->Device();
	return fStatus;
}


status_t
BSpeechRecognizer::SetTo(const char* model, const BNeuralDevice& device,
	uint32 flags)
{
	Unset();
	fStatus = set_recognizer(model, &device, flags, this, &fData, fError);
	if (fStatus == B_OK)
		fDevice = fData->Device();
	return fStatus;
}


void
BSpeechRecognizer::Unset()
{
	delete fData;
	fData = NULL;
	fStatus = B_NO_INIT;
	fError = "";
	fDevice = BNeuralDevice();
}


status_t
BSpeechRecognizer::InitCheck() const
{
	return fStatus;
}


const char*
BSpeechRecognizer::ErrorDetails() const
{
	return fError.String();
}


//! Whether the directory is that of a speech model.
static bool
is_model_directory(const char* path)
{
	BString description(path);
	description << "/speech.conf";
	struct stat stat;
	return ::stat(description.String(), &stat) == 0 && S_ISREG(stat.st_mode);
}


/*static*/ status_t
BSpeechRecognizer::FindModel(const char* name, BPath* _directory)
{
	if (_directory == NULL)
		return B_BAD_VALUE;

	// a path is taken as it is
	if (name != NULL && strchr(name, '/') != NULL) {
		if (!is_model_directory(name))
			return B_ENTRY_NOT_FOUND;
		return _directory->SetTo(name);
	}

	BStringList directories;
	status_t status = BPathFinder::FindPaths(B_FIND_PATH_DATA_DIRECTORY,
		"neural/models", B_FIND_PATH_EXISTING_ONLY, directories);
	if (status != B_OK)
		return status;

	// The user's directories come first, so that a model of the user's
	// shadows one of the system of the same name.
	for (int32 i = 0; i < directories.CountStrings(); i++) {
		if (name != NULL) {
			BPath path(directories.StringAt(i), name);
			if (path.InitCheck() == B_OK && is_model_directory(path.Path())) {
				*_directory = path;
				return B_OK;
			}
			continue;
		}

		// any model: the first by name
		BDirectory directory(directories.StringAt(i));
		BString best;
		BEntry entry;
		while (directory.GetNextEntry(&entry, true) == B_OK) {
			BPath path;
			if (entry.GetPath(&path) == B_OK
				&& is_model_directory(path.Path())
				&& (best.Length() == 0 || best.Compare(path.Path()) > 0)) {
				best = path.Path();
			}
		}
		if (best.Length() > 0)
			return _directory->SetTo(best.String());
	}
	return B_ENTRY_NOT_FOUND;
}


/*static*/ status_t
BSpeechRecognizer::GetModels(BStringList* names)
{
	if (names == NULL)
		return B_BAD_VALUE;
	names->MakeEmpty();

	BStringList directories;
	status_t status = BPathFinder::FindPaths(B_FIND_PATH_DATA_DIRECTORY,
		"neural/models", B_FIND_PATH_EXISTING_ONLY, directories);
	if (status != B_OK)
		return status;

	for (int32 i = 0; i < directories.CountStrings(); i++) {
		BDirectory directory(directories.StringAt(i));
		BEntry entry;
		while (directory.GetNextEntry(&entry, true) == B_OK) {
			BPath path;
			if (entry.GetPath(&path) == B_OK
				&& is_model_directory(path.Path())
				&& !names->HasString(path.Leaf())) {
				names->Add(path.Leaf());
			}
		}
	}
	names->Sort();
	return B_OK;
}


const char*
BSpeechRecognizer::ModelName() const
{
	return fData != NULL ? fData->Model().Info().name : "";
}


const BNeuralDevice&
BSpeechRecognizer::Device() const
{
	return fDevice;
}


status_t
BSpeechRecognizer::GetLanguages(BStringList* codes) const
{
	if (codes == NULL)
		return B_BAD_VALUE;
	if (fData == NULL)
		return B_NO_INIT;

	codes->MakeEmpty();
	const BPrivate::SpeechModel& model = fData->Model();
	if (!model.Info().multilingual)
		codes->Add("en");
	for (int32 i = 0; i < model.CountLanguages(); i++)
		codes->Add(model.LanguageAt(i).code);
	return B_OK;
}


status_t
BSpeechRecognizer::SetLanguage(const char* code)
{
	if (fData == NULL)
		return B_NO_INIT;
	return fData->SetLanguage(code);
}


const char*
BSpeechRecognizer::Language() const
{
	return fData != NULL ? fData->Language() : "";
}


void
BSpeechRecognizer::SetTimestamps(bool enabled)
{
	if (fData != NULL)
		fData->SetTimestamps(enabled);
}


bool
BSpeechRecognizer::Timestamps() const
{
	return fData != NULL && fData->Timestamps();
}


status_t
BSpeechRecognizer::SetTarget(const BMessenger& target,
	const BMessage* message)
{
	if (fData == NULL)
		return B_NO_INIT;
	fData->SetTarget(target, message);
	return B_OK;
}


void
BSpeechRecognizer::SetInterimInterval(bigtime_t interval)
{
	if (fData != NULL)
		fData->SetInterimInterval(interval);
}


bigtime_t
BSpeechRecognizer::InterimInterval() const
{
	return fData != NULL ? fData->InterimInterval() : 0;
}


status_t
BSpeechRecognizer::Start(const media_raw_audio_format* format, int32* _token)
{
	if (fData == NULL)
		return B_NO_INIT;
	return fData->Start(format, false, _token);
}


status_t
BSpeechRecognizer::Feed(const void* buffer, size_t size)
{
	if (fData == NULL)
		return B_NO_INIT;
	if (buffer == NULL && size > 0)
		return B_BAD_VALUE;
	return fData->Feed(buffer, size);
}


status_t
BSpeechRecognizer::Finish()
{
	if (fData == NULL)
		return B_NO_INIT;
	return fData->Finish();
}


void
BSpeechRecognizer::Cancel()
{
	if (fData != NULL)
		fData->Cancel();
}


status_t
BSpeechRecognizer::WaitUntilFinished(bigtime_t timeout)
{
	if (fData == NULL)
		return B_NO_INIT;
	return fData->WaitUntilFinished(timeout);
}


bool
BSpeechRecognizer::IsRecognizing() const
{
	return fData != NULL && fData->IsRecognizing();
}


status_t
BSpeechRecognizer::Transcribe(const float* samples, size_t count,
	BString* _text)
{
	if (fData == NULL)
		return B_NO_INIT;
	if (samples == NULL && count > 0)
		return B_BAD_VALUE;
	if (fData->IsRecognizing())
		return B_BUSY;

	status_t status = fData->Start(NULL, true, NULL);
	if (status == B_OK)
		status = fData->Feed(samples, count * sizeof(float));
	if (status == B_OK)
		status = fData->Finish();
	if (status == B_OK)
		status = fData->WaitUntilFinished(B_INFINITE_TIMEOUT);
	else
		fData->Cancel();
	if (status == B_OK && _text != NULL)
		status = fData->GetText(_text);
	return status;
}


status_t
BSpeechRecognizer::Transcribe(const media_raw_audio_format& format,
	const void* buffer, size_t size, BString* _text)
{
	if (fData == NULL)
		return B_NO_INIT;
	if (buffer == NULL && size > 0)
		return B_BAD_VALUE;
	if (fData->IsRecognizing())
		return B_BUSY;

	status_t status = fData->Start(&format, true, NULL);
	if (status == B_OK)
		status = fData->Feed(buffer, size);
	if (status == B_OK)
		status = fData->Finish();
	if (status == B_OK)
		status = fData->WaitUntilFinished(B_INFINITE_TIMEOUT);
	else
		fData->Cancel();
	if (status == B_OK && _text != NULL)
		status = fData->GetText(_text);
	return status;
}


int32
BSpeechRecognizer::CountSegments() const
{
	return fData != NULL ? fData->CountSegments() : 0;
}


status_t
BSpeechRecognizer::GetSegment(int32 index, speech_segment* _segment) const
{
	if (_segment == NULL)
		return B_BAD_VALUE;
	if (fData == NULL)
		return B_NO_INIT;
	return fData->GetSegment(index, _segment);
}


status_t
BSpeechRecognizer::GetInfo(speech_recognizer_info* info) const
{
	if (info == NULL)
		return B_BAD_VALUE;
	if (fData == NULL)
		return B_NO_INIT;
	fData->GetInfo(info);
	return B_OK;
}


void BSpeechRecognizer::_ReservedSpeechRecognizer1() {}
void BSpeechRecognizer::_ReservedSpeechRecognizer2() {}
void BSpeechRecognizer::_ReservedSpeechRecognizer3() {}
void BSpeechRecognizer::_ReservedSpeechRecognizer4() {}
