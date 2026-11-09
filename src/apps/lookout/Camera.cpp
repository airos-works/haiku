/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Camera.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include <Autolock.h>
#include <Buffer.h>
#include <BufferConsumer.h>
#include <Catalog.h>
#include <MediaEventLooper.h>
#include <MediaRoster.h>
#include <TimeSource.h>

#include "Engine.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Camera"


enum {
	kMsgInit		= 'LcIi',
	kMsgApply		= 'LcAp',
	kMsgShutdown	= 'LcSd'
};

//! The size asked of the camera: plenty for networks that look at 300 x 300.
static const uint32 kWidth = 640;
static const uint32 kHeight = 480;
static const int32 kMaxInputs = 16;


/*!	The node the camera's node sends its buffers to. Every buffer is
	copied into a frame at once and given back: the camera gets its
	buffers back at its own pace, however long a network takes. */
class FrameConsumer : public BMediaEventLooper, public BBufferConsumer {
public:
								FrameConsumer(Camera* camera,
									SnapshotPool* pool, int32 source);
	virtual						~FrameConsumer();

			void				SetSource(int32 source)
									{ atomic_set(&fSource, source); }
			const media_raw_video_format& Format() const
									{ return fInput.format.u.raw_video; }

	// BMediaNode
	virtual	BMediaAddOn*		AddOn(int32* _id) const;
	virtual	void				NodeRegistered();
	virtual	status_t			HandleMessage(int32 message,
									const void* data, size_t size);

	// BMediaEventLooper
	virtual	void				HandleEvent(const media_timed_event* event,
									bigtime_t lateness, bool realTimeEvent);

	// BBufferConsumer
	virtual	status_t			AcceptFormat(
									const media_destination& destination,
									media_format* format);
	virtual	status_t			GetNextInput(int32* cookie,
									media_input* _input);
	virtual	void				DisposeInputCookie(int32 cookie);
	virtual	void				BufferReceived(BBuffer* buffer);
	virtual	void				ProducerDataStatus(
									const media_destination& destination,
									int32 status, bigtime_t when);
	virtual	status_t			GetLatencyFor(
									const media_destination& destination,
									bigtime_t* _latency,
									media_node_id* _timeSource);
	virtual	status_t			Connected(const media_source& source,
									const media_destination& destination,
									const media_format& format,
									media_input* _input);
	virtual	void				Disconnected(const media_source& source,
									const media_destination& destination);
	virtual	status_t			FormatChanged(const media_source& source,
									const media_destination& destination,
									int32 tag, const media_format& format);

private:
			Camera*				fCamera;
			SnapshotPool*			fPool;
			int32				fSource;
			media_input			fInput;
};


FrameConsumer::FrameConsumer(Camera* camera, SnapshotPool* pool, int32 source)
	:
	BMediaNode("Lookout"),
	BMediaEventLooper(),
	BBufferConsumer(B_MEDIA_RAW_VIDEO),
	fCamera(camera),
	fPool(pool),
	fSource(source)
{
	fInput.destination.port = -1;
}


FrameConsumer::~FrameConsumer()
{
	// ends the node's thread: no BufferReceived() after this
	Quit();
}


BMediaAddOn*
FrameConsumer::AddOn(int32* _id) const
{
	return NULL;
}


void
FrameConsumer::NodeRegistered()
{
	fInput.destination.port = ControlPort();
	fInput.destination.id = 0;
	fInput.source = media_source::null;
	fInput.node = Node();
	fInput.format.type = B_MEDIA_RAW_VIDEO;
	fInput.format.u.raw_video = media_raw_video_format::wildcard;
	strlcpy(fInput.name, "Lookout input", sizeof(fInput.name));

	Run();
}


status_t
FrameConsumer::HandleMessage(int32 message, const void* data, size_t size)
{
	return B_ERROR;
}


void
FrameConsumer::HandleEvent(const media_timed_event* event, bigtime_t lateness,
	bool realTimeEvent)
{
	// Buffers are not queued for their time: the newest picture is the
	// one to look at, as soon as it is there.
}


status_t
FrameConsumer::AcceptFormat(const media_destination& destination,
	media_format* format)
{
	if (destination != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;

	if (format->type == B_MEDIA_NO_TYPE)
		format->type = B_MEDIA_RAW_VIDEO;
	if (format->type != B_MEDIA_RAW_VIDEO)
		return B_MEDIA_BAD_FORMAT;

	color_space& space = format->u.raw_video.display.format;
	if (space == media_raw_video_format::wildcard.display.format)
		space = B_RGB32;
	if (space != B_RGB32 && space != B_RGBA32)
		return B_MEDIA_BAD_FORMAT;

	return B_OK;
}


status_t
FrameConsumer::GetNextInput(int32* cookie, media_input* _input)
{
	if (*cookie != 0)
		return B_BAD_INDEX;

	*_input = fInput;
	(*cookie)++;
	return B_OK;
}


void
FrameConsumer::DisposeInputCookie(int32 cookie)
{
}


void
FrameConsumer::BufferReceived(BBuffer* buffer)
{
	const media_raw_video_format& format = fInput.format.u.raw_video;
	int32 width = format.display.line_width;
	int32 height = format.display.line_count;

	Snapshot* frame = NULL;
	if (fInput.source != media_source::null && width > 0 && height > 0
		&& (format.display.format == B_RGB32
			|| format.display.format == B_RGBA32)) {
		frame = fPool->GetFrame(width, height, atomic_get(&fSource));
	}

	if (frame != NULL) {
		BBitmap* bitmap = frame->Bitmap();
		size_t sourceRow = format.display.bytes_per_row;
		if (sourceRow < (size_t)width * 4)
			sourceRow = (size_t)width * 4;
		size_t targetRow = bitmap->BytesPerRow();
		size_t rowBytes = (size_t)width * 4;

		// a buffer that is short (a camera that lost part of a picture)
		// leaves the rest of what the frame showed before
		int32 rows = buffer->SizeUsed() / sourceRow;
		if (rows > height)
			rows = height;

		const uint8* source = (const uint8*)buffer->Data();
		uint8* target = (uint8*)bitmap->Bits();
		if (sourceRow == targetRow)
			memcpy(target, source, rows * sourceRow);
		else {
			for (int32 y = 0; y < rows; y++) {
				memcpy(target, source, rowBytes);
				source += sourceRow;
				target += targetRow;
			}
		}
	}

	buffer->Recycle();

	if (frame != NULL) {
		fCamera->_FrameReceived(frame);
		frame->ReleaseReference();
	}
}


void
FrameConsumer::ProducerDataStatus(const media_destination& destination,
	int32 status, bigtime_t when)
{
}


status_t
FrameConsumer::GetLatencyFor(const media_destination& destination,
	bigtime_t* _latency, media_node_id* _timeSource)
{
	if (destination != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;

	*_latency = 10000;
	*_timeSource = TimeSource()->ID();
	return B_OK;
}


status_t
FrameConsumer::Connected(const media_source& source,
	const media_destination& destination, const media_format& format,
	media_input* _input)
{
	fInput.source = source;
	fInput.format = format;
	*_input = fInput;
	return B_OK;
}


void
FrameConsumer::Disconnected(const media_source& source,
	const media_destination& destination)
{
	fInput.source = media_source::null;
}


status_t
FrameConsumer::FormatChanged(const media_source& source,
	const media_destination& destination, int32 tag,
	const media_format& format)
{
	if (destination != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;
	if (format.type != B_MEDIA_RAW_VIDEO
		|| (format.u.raw_video.display.format != B_RGB32
			&& format.u.raw_video.display.format != B_RGBA32)) {
		return B_MEDIA_BAD_FORMAT;
	}

	fInput.format = format;
	return B_OK;
}


// #pragma mark - Camera


Camera::Camera(Engine* engine)
	:
	BLooper("camera"),
	fEngine(engine),
	fPool(new SnapshotPool()),
	fFrames(0),
	fLock("camera wishes"),
	fWanted(false),
	fWantedSource(0),
	fApplyPending(false),
	fRoster(NULL),
	fWatching(false),
	fConsumer(NULL),
	fProducer(media_node::null),
	fConnected(false),
	fState(CAMERA_OFF)
{
	PostMessage(kMsgInit);
}


Camera::~Camera()
{
	// in the looper's own thread, as everything else that talks to the
	// media roster
	_Disconnect();
	if (fWatching)
		fRoster->StopWatching(BMessenger(this));

	fDisplaySlot.Take();
	fPool->ReleaseReference();
}


void
Camera::SetTarget(const BMessenger& target)
{
	fTarget = target;
}


void
Camera::Start(const char* name, int32 source)
{
	BAutolock locker(fLock);
	fWanted = true;
	fWantedName = name;
	fWantedSource = source;
	if (!fApplyPending) {
		fApplyPending = true;
		PostMessage(kMsgApply);
	}
}


void
Camera::Stop()
{
	BAutolock locker(fLock);
	fWanted = false;
	if (!fApplyPending) {
		fApplyPending = true;
		PostMessage(kMsgApply);
	}
}


void
Camera::Shutdown()
{
	// Not left to the destructor: that runs while the thread that quits
	// this looper waits for it, and a thread that is waited for gets its
	// own waits interrupted. The node's thread is waited for when the
	// node is deleted, and that wait must hold.
	BMessage reply;
	BMessenger(this).SendMessage(kMsgShutdown, &reply);
}


void
Camera::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgShutdown:
		{
			BAutolock locker(fLock);
			fWanted = false;
			locker.Unlock();
			_Disconnect();
			message->SendReply(B_REPLY);
			break;
		}

		case kMsgInit:
		{
			status_t status;
			fRoster = BMediaRoster::Roster(&status);
			if (fRoster != NULL) {
				fWatching = fRoster->StartWatching(BMessenger(this)) == B_OK;
				_UpdateInputs();
			}
			break;
		}

		case kMsgApply:
			_Apply(false);
			break;

		case B_MEDIA_NODE_DELETED:
		{
			// the camera was unplugged?
			int32 id;
			for (int32 i = 0; fConnected
					&& message->FindInt32("media_node_id", i, &id) == B_OK;
					i++) {
				if (id == fProducer.node) {
					_Disconnect();
					_SendState(CAMERA_MISSING);
				}
			}
			// fall through
		}
		case B_MEDIA_NODE_CREATED:
		case B_MEDIA_FLAVORS_CHANGED:
		case B_MEDIA_SERVER_STARTED:
			// A camera that turns up while one is wanted is started; only
			// when the inputs changed, or this program's own node coming
			// and going would make it try for ever.
			if (_UpdateInputs())
				_Apply(true);
			break;

		default:
			BLooper::MessageReceived(message);
			break;
	}
}


/*!	Makes the connection what is wanted now. With \a retry, a camera that
	could not be started is tried again. */
void
Camera::_Apply(bool retry)
{
	bool wanted;
	BString name;
	int32 source;
	{
		BAutolock locker(fLock);
		wanted = fWanted;
		name = fWantedName;
		source = fWantedSource;
		if (!retry)
			fApplyPending = false;
	}

	if (!wanted) {
		_Disconnect();
		if (fState != CAMERA_OFF)
			_SendState(CAMERA_OFF);
		return;
	}

	if (retry && fConnected)
		return;
	if (fConnected && name == fName) {
		// the same camera for a new purpose
		fConsumer->SetSource(source);
		_SendState(CAMERA_RUNNING);
		return;
	}

	_Disconnect();
	_SendState(CAMERA_STARTING);

	BString error;
	status_t status = _Connect(name, source, error);
	if (status == B_OK) {
		fName = name;
		_SendState(CAMERA_RUNNING);
		return;
	}

	_Disconnect();
	if (status == B_ENTRY_NOT_FOUND)
		_SendState(CAMERA_MISSING);
	else {
		error << " (" << strerror(status) << ")";
		_SendState(CAMERA_FAILED, error);
	}
}


/*!	Gets a reference to the node of the input of that name, or to the
	default video input, or to any video input there is. */
status_t
Camera::_FindProducer(const BString& name)
{
	live_node_info nodes[kMaxInputs];
	int32 count = kMaxInputs;
	media_format format;
	format.type = B_MEDIA_RAW_VIDEO;
	if (fRoster->GetLiveNodes(nodes, &count, NULL, &format, NULL,
			B_BUFFER_PRODUCER | B_PHYSICAL_INPUT) != B_OK) {
		count = 0;
	}

	if (!name.IsEmpty()) {
		for (int32 i = 0; i < count; i++) {
			if (name == nodes[i].name
				&& fRoster->GetNodeFor(nodes[i].node.node, &fProducer)
					== B_OK) {
				return B_OK;
			}
		}
	}

	if (fRoster->GetVideoInput(&fProducer) == B_OK)
		return B_OK;

	for (int32 i = 0; i < count; i++) {
		if (fRoster->GetNodeFor(nodes[i].node.node, &fProducer) == B_OK)
			return B_OK;
	}

	fProducer = media_node::null;
	return B_ENTRY_NOT_FOUND;
}


status_t
Camera::_Connect(const BString& name, int32 source, BString& _error)
{
	if (fRoster == NULL) {
		status_t status;
		fRoster = BMediaRoster::Roster(&status);
		if (fRoster == NULL) {
			_error = B_TRANSLATE("The media services are not running");
			return status != B_OK ? status : B_ERROR;
		}
		fWatching = fRoster->StartWatching(BMessenger(this)) == B_OK;
	}

	status_t status = _FindProducer(name);
	if (status != B_OK)
		return status;

	media_node timeSource;
	status = fRoster->GetTimeSource(&timeSource);
	if (status != B_OK) {
		_error = B_TRANSLATE("There is no time source");
		return status;
	}

	fConsumer = new(std::nothrow) FrameConsumer(this, fPool, source);
	if (fConsumer == NULL)
		return B_NO_MEMORY;
	status = fRoster->RegisterNode(fConsumer);
	if (status != B_OK) {
		_error = B_TRANSLATE("The media services did not accept this "
			"program's node");
		return status;
	}

	int32 count = 0;
	status = fRoster->GetFreeOutputsFor(fProducer, &fOutput, 1, &count,
		B_MEDIA_RAW_VIDEO);
	if (status != B_OK || count < 1) {
		_error = B_TRANSLATE("The camera is in use by another application");
		return status != B_OK ? status : B_BUSY;
	}

	count = 0;
	status = fRoster->GetFreeInputsFor(fConsumer->Node(), &fInput, 1, &count,
		B_MEDIA_RAW_VIDEO);
	if (status != B_OK || count < 1) {
		_error = B_TRANSLATE("The camera could not be connected");
		return status != B_OK ? status : B_ERROR;
	}

	// A modest size; a node that does not have it gets to say which it
	// has.
	media_format format;
	format.type = B_MEDIA_RAW_VIDEO;
	format.u.raw_video = media_raw_video_format::wildcard;
	format.u.raw_video.display.format = B_RGB32;
	format.u.raw_video.display.line_width = kWidth;
	format.u.raw_video.display.line_count = kHeight;

	media_output output = fOutput;
	media_input input = fInput;
	status = fRoster->Connect(output.source, input.destination, &format,
		&fOutput, &fInput);
	if (status != B_OK) {
		format.u.raw_video = media_raw_video_format::wildcard;
		format.u.raw_video.display.format = B_RGB32;
		status = fRoster->Connect(output.source, input.destination, &format,
			&fOutput, &fInput);
	}
	if (status != B_OK) {
		_error = B_TRANSLATE("The camera could not be connected");
		return status;
	}
	fConnected = true;
	atomic_set(&fFrames, 0);

	fRoster->SetTimeSourceFor(fProducer.node, timeSource.node);
	fRoster->SetTimeSourceFor(fConsumer->ID(), timeSource.node);

	// Machines without a sound card have a time source that stands still.
	BTimeSource* clock = fRoster->MakeTimeSourceFor(fProducer);
	bigtime_t real = BTimeSource::RealTime();
	if (clock != NULL && !clock->IsRunning()) {
		fRoster->StartTimeSource(timeSource, real);
		fRoster->SeekTimeSource(timeSource, 0, real);
	}
	bigtime_t start = clock != NULL
		? clock->PerformanceTimeFor(real + 50000) : 0;
	if (clock != NULL)
		clock->Release();

	status = fRoster->StartNode(fConsumer->Node(), start);
	if (status == B_OK)
		status = fRoster->StartNode(fProducer, start);
	if (status != B_OK) {
		_error = B_TRANSLATE("The camera could not be started");
		return status;
	}

	return B_OK;
}


/*!	Lets go of the camera altogether: stopped, disconnected, the nodes
	released. A camera that nobody is connected to stops taking pictures.
	Safe to call in any state, also half way through _Connect(). */
void
Camera::_Disconnect()
{
	if (fRoster == NULL)
		return;

	if (fConnected) {
		fRoster->StopNode(fProducer, 0, true);
		fRoster->StopNode(fConsumer->Node(), 0, true);
		fRoster->Disconnect(fOutput.node.node, fOutput.source,
			fInput.node.node, fInput.destination);
		fConnected = false;
	}

	if (fProducer != media_node::null) {
		fRoster->ReleaseNode(fProducer);
		fProducer = media_node::null;
	}

	if (fConsumer != NULL) {
		fRoster->UnregisterNode(fConsumer);
		fConsumer->Release();
			// deletes it, and with it its thread
		fConsumer = NULL;
	}

	fName = "";
	fDisplaySlot.Take();
}


/*!	Looks which video inputs there are and tells the target if they are
	not the ones of last time. Returns whether they changed. */
bool
Camera::_UpdateInputs()
{
	if (fRoster == NULL)
		return false;

	BStringList inputs;
	BString defaultInput;

	live_node_info nodes[kMaxInputs];
	int32 count = kMaxInputs;
	media_format format;
	format.type = B_MEDIA_RAW_VIDEO;
	if (fRoster->GetLiveNodes(nodes, &count, NULL, &format, NULL,
			B_BUFFER_PRODUCER | B_PHYSICAL_INPUT) == B_OK) {
		for (int32 i = 0; i < count; i++)
			inputs.Add(nodes[i].name);
	}

	media_node node;
	if (fRoster->GetVideoInput(&node) == B_OK) {
		live_node_info info;
		if (fRoster->GetLiveNodeInfo(node, &info) == B_OK) {
			defaultInput = info.name;
			if (!inputs.HasString(defaultInput))
				inputs.Add(defaultInput);
		}
		fRoster->ReleaseNode(node);
	}

	if (inputs == fInputs && defaultInput == fDefaultInput)
		return false;

	fInputs = inputs;
	fDefaultInput = defaultInput;

	BMessage message(MSG_CAMERA_INPUTS);
	message.AddStrings("name", fInputs);
	message.AddString("default", fDefaultInput);
	fTarget.SendMessage(&message, (BHandler*)NULL, 1000000);
	return true;
}


void
Camera::_SendState(camera_state state, const char* error)
{
	fState = state;

	BMessage message(MSG_CAMERA_STATE);
	message.AddInt32("state", state);
	if (error != NULL)
		message.AddString("error", error);
	if (state == CAMERA_RUNNING) {
		live_node_info info;
		if (fRoster->GetLiveNodeInfo(fProducer, &info) == B_OK)
			message.AddString("name", info.name);
		const media_raw_video_format& format = fInput.format.u.raw_video;
		message.AddInt32("width", format.display.line_width);
		message.AddInt32("height", format.display.line_count);
	}
	fTarget.SendMessage(&message, (BHandler*)NULL, 1000000);
}


//!	In the consumer node's thread.
void
Camera::_FrameReceived(Snapshot* frame)
{
	atomic_add(&fFrames, 1);

	fEngine->SetFrame(frame);

	if (fDisplaySlot.Put(frame)) {
		BMessage message(MSG_CAMERA_FRAME);
		if (fTarget.SendMessage(&message, (BHandler*)NULL, 0) != B_OK) {
			// not told: the next frame tries again
			fDisplaySlot.Take();
		}
	}
}
