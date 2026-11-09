/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Engine.h"

#include <new>
#include <string.h>

#include <Autolock.h>
#include <Catalog.h>
#include <Directory.h>
#include <Path.h>


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Engine"


enum {
	kMsgInit		= 'LeIn',
	kMsgConfigure	= 'LeCf',
	kMsgFrame		= 'LeFr',
	kMsgRunDone		= 'LeRd',
	kMsgShutdown	= 'LeSd'
};

//! Detections the model is less sure of than this are not shown.
static const float kMinimumScore = 0.5f;
static const int32 kTopClasses = 5;
static const int32 kMaxDetections = 64;
//! Results of runs on one and the same picture are passed on this often.
static const bigtime_t kResultInterval = 40000;
static const bigtime_t kRateSpan = 2000000;
static const bigtime_t kSendTimeout = 1000000;


static bool
is_image_tensor(const BTensorInfo& info)
{
	int32 rank = info.Rank();
	if (rank != 3 && rank != 4)
		return false;
	if (rank == 4 && info.DimensionAt(0) != 1)
		return false;
	int32 channels = info.DimensionAt(rank - 1);
	return (channels == 3 || channels == 1) && info.DimensionAt(rank - 2) > 1
		&& (info.Type() == B_NEURAL_UINT8 || info.Type() == B_NEURAL_INT8
			|| info.Type() == B_NEURAL_FLOAT32);
}


Engine::Engine()
	:
	BLooper("engine"),
	fLock("engine wishes"),
	fConfigurePending(false),
	fShutDown(false),
	fSource(0),
	fContinuous(0),
	fChoiceMissing(false),
	fSession(NULL),
	fKind(MODEL_CLASSIFICATION),
	fLabelOffset(0),
	fGeneration(0),
	fRunning(false),
	fSentFrame(NULL),
	fSentGeneration(-1),
	fSentTime(0),
	fDoneCount(0)
{
	// The first question to the roster loads the back ends, which look
	// for their hardware: on the engine's own time.
	PostMessage(kMsgInit);
}


Engine::~Engine()
{
	BNeuralRoster::Default()->StopWatching(BMessenger(this));
	_DropSession();
}


void
Engine::SetTarget(const BMessenger& target)
{
	fTarget = target;
}


void
Engine::Configure(const entry_ref* model, const char* device)
{
	BAutolock locker(fLock);
	if (model != NULL)
		fWantedModel = *model;
	if (device != NULL)
		fWantedDevice = device;

	// one message, however often the wishes change before it is read
	if (!fConfigurePending) {
		fConfigurePending = true;
		PostMessage(kMsgConfigure);
	}
}


void
Engine::SetSource(int32 source)
{
	atomic_set(&fSource, source);
}


void
Engine::SetFrame(Snapshot* frame)
{
	if (frame == NULL || frame->Source() != atomic_get(&fSource))
		return;

	// One message per filled slot, however many frames pass through it.
	if (fSlot.Put(frame))
		PostMessage(kMsgFrame);
}


void
Engine::SetContinuous(bool continuous)
{
	atomic_set(&fContinuous, continuous ? 1 : 0);
	PostMessage(kMsgFrame);
}


void
Engine::Shutdown()
{
	// The session is not left to the destructor, which runs while the
	// thread that quits this looper waits for it: the wait for a run can
	// take as long as a run, and is better done before anybody waits.
	BMessage reply;
	BMessenger(this).SendMessage(kMsgShutdown, &reply);
}


/*static*/ bool
Engine::IsDetectionModel(const BNeuralModel& model)
{
	// What TensorFlow Lite's detection post-processing gives: boxes
	// [1, n, 4], classes, scores and how many of them count.
	if (model.CountOutputs() != 4)
		return false;

	BTensorInfo boxes;
	BTensorInfo count;
	if (model.GetOutputInfo(0, &boxes) != B_OK
		|| model.GetOutputInfo(3, &count) != B_OK) {
		return false;
	}
	return boxes.Rank() == 3 && boxes.DimensionAt(2) == 4
		&& boxes.Type() == B_NEURAL_FLOAT32 && count.CountElements() == 1;
}


void
Engine::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgInit:
			BNeuralRoster::Default()->StartWatching(BMessenger(this));
			_SendDevices();
			break;

		case kMsgConfigure:
			if (!fShutDown)
				_Configure(false);
			break;

		case kMsgFrame:
			_Next(false);
			break;

		case kMsgShutdown:
			fShutDown = true;
			_DropSession();
			message->SendReply(B_REPLY);
			break;

		case kMsgRunDone:
			_RunDone(message);
			break;

		case B_NEURAL_DEVICE_ADDED:
		case B_NEURAL_DEVICE_REMOVED:
		{
			_SendDevices();

			// A session whose device went away already runs on the CPU;
			// setting it up anew is how the choice is made again, and how
			// a device that came (back) gets used.
			const char* id = message->GetString("device", "");
			bool concerned;
			if (message->what == B_NEURAL_DEVICE_ADDED)
				concerned = fDeviceChoice.IsEmpty() || fDeviceChoice == id;
			else {
				concerned = fSession != NULL
					&& strcmp(fSession->Device().ID(), id) == 0;
			}
			if (concerned && !fShutDown)
				_Configure(true);
			break;
		}

		default:
			BLooper::MessageReceived(message);
			break;
	}
}


/*!	Makes the session what is wanted now. This is the slow part, and the
	reason the engine has a thread of its own. */
void
Engine::_Configure(bool force)
{
	entry_ref ref;
	BString choice;
	{
		BAutolock locker(fLock);
		ref = fWantedModel;
		choice = fWantedDevice;
		fConfigurePending = false;
	}

	if (ref.name == NULL)
		return;

	bool newModel = ref != fModelRef || fModel.InitCheck() != B_OK;
	if (!force && !newModel && choice == fDeviceChoice && fSession != NULL)
		return;

	_DropSession();
	fModelRef = ref;
	fDeviceChoice = choice;
	fChoiceMissing = false;
	_SendState(ENGINE_LOADING);

	if (newModel) {
		status_t status = fModel.SetTo(&ref);
		if (status != B_OK) {
			BString error(neural_error_string(status));
			if (fModel.ErrorDetails()[0] != '\0')
				error << " (" << fModel.ErrorDetails() << ")";
			fModel.Unset();
			_SendState(ENGINE_FAILED, error);
			return;
		}
	}

	BTensorInfo input;
	if (fModel.CountInputs() < 1 || fModel.GetInputInfo(0, &input) != B_OK
		|| !is_image_tensor(input)) {
		_SendState(ENGINE_FAILED,
			B_TRANSLATE("This model does not take a picture"));
		return;
	}

	BNeuralSession* session = new(std::nothrow) BNeuralSession();
	if (session == NULL) {
		_SendState(ENGINE_FAILED, strerror(B_NO_MEMORY));
		return;
	}

	status_t status;
	BNeuralDevice device;
	if (!choice.IsEmpty()
		&& BNeuralRoster::Default()->FindDevice(choice, &device) == B_OK
		&& device.IsAvailable()) {
		status = session->SetTo(fModel, device);
	} else {
		// the kit's choice, also when the user's is not there today
		fChoiceMissing = !choice.IsEmpty();
		status = session->SetTo(fModel);
	}
	if (status != B_OK) {
		delete session;
		_SendState(ENGINE_FAILED, neural_error_string(status));
		return;
	}

	fSession = session;
	fKind = BNeuralUtils::IsDetector(*fSession)
		? MODEL_DETECTION : MODEL_CLASSIFICATION;
	_FindLabels(ref);
	_SendState(ENGINE_READY);

	// the picture that is on show, with the new model or device
	_Next(true);
}


/*!	Ends the session. A run that is going on is waited for, which is why
	nobody else may hold the session: when this returns, nothing of it is
	in use any more. The message of that last run may still be in the
	queue; its generation is the old one. */
void
Engine::_DropSession()
{
	// (the session's destructor waits for a run that is under way)
	delete fSession;
	fSession = NULL;
	fRunning = false;
	fRunFrame.Unset();
	fGeneration++;
	fDoneCount = 0;
	fLabels.MakeEmpty();
	fLabelOffset = 0;
}


/*!	Looks for the names of the model's classes next to it: a file named
	like the model, or one of the names such files usually have. In a
	directory with several models and several label files, the one whose
	length fits the model is taken. */
status_t
Engine::_FindLabels(const entry_ref& model)
{
	fLabels.MakeEmpty();
	fLabelOffset = 0;

	BEntry entry(&model, true);
	BDirectory directory;
	status_t status = entry.GetParent(&directory);
	if (status != B_OK)
		return status;

	int64 classes = 0;
	if (fKind == MODEL_CLASSIFICATION && fSession->OutputAt(0) != NULL)
		classes = fSession->OutputAt(0)->CountElements();

	BStringList candidates;
	BString name(model.name);
	candidates.Add(BString(name) << ".txt");
	int32 dot = name.FindLast('.');
	if (dot > 0) {
		name.Truncate(dot);
		candidates.Add(BString(name) << ".txt");
	}
	int32 named = candidates.CountStrings();

	if (fKind == MODEL_DETECTION)
		candidates.Add("labelmap.txt");
	candidates.Add("labels.txt");

	BStringList others;
	char leaf[B_FILE_NAME_LENGTH];
	directory.Rewind();
	while (directory.GetNextEntry(&entry) == B_OK) {
		if (entry.GetName(leaf) != B_OK)
			continue;
		BString other(leaf);
		if (other.StartsWith("labels_") && other.EndsWith(".txt"))
			others.Add(other);
	}
	others.Sort();
	candidates.Add(others);

	if (fKind != MODEL_DETECTION)
		candidates.Add("labelmap.txt");

	for (int32 i = 0; i < candidates.CountStrings(); i++) {
		BPath path(&directory, candidates.StringAt(i));
		BStringList labels;
		if (path.InitCheck() != B_OK
			|| BNeuralUtils::GetLabels(path.Path(), &labels) != B_OK
			|| labels.IsEmpty()) {
			continue;
		}

		int32 count = labels.CountStrings();
		int32 offset = 0;
		if (fKind == MODEL_DETECTION) {
			// Label files of detection models start with a "???" line for
			// the background, which the model's classes do not count.
			if (labels.StringAt(0) == "???")
				offset = 1;
		} else if (count == classes - 1) {
			// no line for the model's background class
			offset = -1;
		} else if (count != classes && i >= named) {
			// the labels of another model
			continue;
		}

		fLabels = labels;
		fLabelOffset = offset;
		return B_OK;
	}

	return B_ENTRY_NOT_FOUND;
}


/*!	Starts a run if there is something to run on: the newest frame that
	came in, or (\a again, or when running continuously) the picture that
	was run on last. */
void
Engine::_Next(bool again)
{
	if (fSession == NULL || fRunning)
		return;

	int32 source = atomic_get(&fSource);

	SnapshotReference next = fSlot.Take();
	if (next.IsSet() && next->Source() == source) {
		fFrame = next;
		_StartRun(next.Get());
		return;
	}

	if (!fFrame.IsSet())
		return;
	if (fFrame->Source() != source) {
		fFrame.Unset();
		return;
	}

	if (again || (atomic_get(&fContinuous) != 0 && !fFrame->IsLive()))
		_StartRun(fFrame.Get());
}


void
Engine::_StartRun(Snapshot* frame)
{
	// Networks are trained on pictures squeezed into their input, and
	// "neural run" does the same.
	neural_image_options options;
	options.fit = B_NEURAL_FIT_STRETCH;

	status_t status = BNeuralUtils::SetTensorFromBitmap(fSession->InputAt(0),
		frame->Bitmap(), NULL, &options, &fMapping);
	if (status == B_OK) {
		BMessage done(kMsgRunDone);
		done.AddInt32("generation", fGeneration);
		status = fSession->RunAsync(BMessenger(this), &done);
	}
	if (status != B_OK) {
		BString error(B_TRANSLATE("The model could not be run: %error%"));
		error.ReplaceFirst("%error%", neural_error_string(status));
		_DropSession();
		_SendState(ENGINE_FAILED, error);
		return;
	}

	fRunning = true;
	fRunFrame = frame;
}


void
Engine::_RunDone(BMessage* message)
{
	// Of a session that is no more? The kit says which session a run was
	// of, but by its address, which the next session may get, and counts
	// runs per session: the generation is what tells the run of a session
	// before from the first one of this.
	if (!fRunning || message->GetInt32("generation", -1) != fGeneration
		|| message->GetPointer("neural:session", (const void*)NULL)
			!= fSession) {
		return;
	}

	SnapshotReference frame = fRunFrame;
	fRunFrame.Unset();
	fRunning = false;

	status_t status = message->GetInt32("neural:status", B_ERROR);
	if (status != B_OK) {
		BString error(B_TRANSLATE("The model could not be run: %error%"));
		error.ReplaceFirst("%error%", neural_error_string(status));
		_DropSession();
		_SendState(ENGINE_FAILED, error);
		return;
	}

	if (frame->Source() == atomic_get(&fSource))
		_SendResult(frame.Get(), message->GetInt64("neural:duration", 0));

	_Next(false);
}


void
Engine::_SendDevices()
{
	BMessage message(MSG_ENGINE_DEVICES);

	BNeuralRoster* roster = BNeuralRoster::Default();
	int32 count = roster->CountDevices();
	for (int32 i = 0; i < count; i++) {
		BNeuralDevice device;
		if (roster->GetDeviceAt(i, &device) != B_OK)
			continue;
		message.AddString("device", device.ID());
		message.AddString("name", device.Name());
		message.AddInt32("kind", device.Kind());
	}
	_Send(message);
}


void
Engine::_AddRunInfo(BMessage& message) const
{
	// asked after every run: a device that fails hands its part of the
	// model to the CPU, and the user is to see that
	neural_run_info info;
	if (fSession == NULL || fSession->GetRunInfo(&info) != B_OK)
		return;

	message.AddInt32("on device", info.device_operators);
	message.AddInt32("on cpu", info.cpu_operators);
	message.AddInt32("failures", info.device_failures);
}


void
Engine::_SendState(engine_state state, const char* error)
{
	BMessage message(MSG_ENGINE_STATE);
	message.AddInt32("state", state);
	message.AddRef("model", &fModelRef);
	message.AddString("choice", fDeviceChoice);
	if (error != NULL)
		message.AddString("error", error);

	if (state == ENGINE_READY) {
		const BNeuralDevice& device = fSession->Device();
		message.AddString("model name", fModel.Name());
		message.AddInt32("kind", fKind);
		message.AddString("device", device.ID());
		message.AddString("device name", device.Name());
		message.AddInt32("device kind", device.Kind());
		message.AddBool("choice missing", fChoiceMissing);
		message.AddInt32("labels", fLabels.CountStrings());
		_AddRunInfo(message);
	}
	_Send(message);
}


void
Engine::_SendResult(Snapshot* frame, bigtime_t duration)
{
	float rate = _Rate();

	// A picture that does not change gives the same answer every time;
	// only the times are news, and not five hundred times per second.
	bigtime_t now = system_time();
	if (!frame->IsLive() && frame == fSentFrame
		&& fGeneration == fSentGeneration
		&& now - fSentTime < kResultInterval) {
		return;
	}
	fSentFrame = frame;
	fSentGeneration = fGeneration;
	fSentTime = now;

	BMessage message(MSG_ENGINE_RESULT);
	message.AddInt32("source", frame->Source());
	message.AddInt32("kind", fKind);
	message.AddInt64("duration", duration);
	message.AddFloat("rate", rate);
	_AddRunInfo(message);

	if (fKind == MODEL_DETECTION) {
		// best first, as the kit hands them out; boxes are fractions of
		// the model's input
		neural_detection found[kMaxDetections];
		int32 count = BNeuralUtils::GetDetections(*fSession, found,
			kMaxDetections);
		for (int32 i = 0; i < count; i++) {
			if (found[i].score < kMinimumScore)
				continue;

			int32 index = found[i].class_index;
			BString label = fLabels.StringAt(index + fLabelOffset);
			if (label.IsEmpty()) {
				label.SetToFormat(B_TRANSLATE_COMMENT("class %" B_PRId32,
					"A class the model has no name for"), index);
			}
			message.AddRect("box", fMapping.ToBitmap(found[i].left,
				found[i].top, found[i].right, found[i].bottom));
			message.AddString("label", label);
			message.AddFloat("score", found[i].score);
			message.AddInt32("class", index);
		}
	} else {
		int64 indices[kTopClasses];
		float values[kTopClasses];
		int32 found = BNeuralUtils::GetTopValues(*fSession->OutputAt(0),
			kTopClasses, indices, values);
		for (int32 i = 0; i < found; i++) {
			BString label = fLabels.StringAt(indices[i] + fLabelOffset);
			if (label.IsEmpty()) {
				label.SetToFormat(B_TRANSLATE_COMMENT("class %" B_PRId32,
					"A class the model has no name for"), (int32)indices[i]);
			}
			message.AddString("label", label);
			message.AddFloat("score", values[i]);
			message.AddInt32("class", (int32)indices[i]);
		}
	}

	_Send(message);
}


/*!	Runs per second over the last moments, 0 when the runs did not follow
	each other closely enough to say. Called once per run. */
float
Engine::_Rate()
{
	bigtime_t now = system_time();

	int32 kept = 0;
	for (int32 i = 0; i < fDoneCount; i++) {
		if (now - fDone[i] <= kRateSpan
			&& (fDoneCount - i) < kRateSamples) {
			fDone[kept++] = fDone[i];
		}
	}
	fDone[kept++] = now;
	fDoneCount = kept;

	if (fDoneCount < 3 || fDone[fDoneCount - 1] <= fDone[0])
		return 0;
	return (fDoneCount - 1) * 1e6f / (fDone[fDoneCount - 1] - fDone[0]);
}


void
Engine::_Send(BMessage& message)
{
	// Not for ever: the window may be waiting for this looper to quit.
	fTarget.SendMessage(&message, (BHandler*)NULL, kSendTimeout);
}
