/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "LiveViews.h"

#include <algorithm>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <Autolock.h>
#include <Bitmap.h>
#include <Catalog.h>
#include <GradientLinear.h>
#include <LayoutUtils.h>
#include <MediaRecorder.h>
#include <MediaRoster.h>
#include <MessageRunner.h>
#include <TimeSource.h>

#include "Cards.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Live views"


static const uint32 kMsgFrame = 'lvFr';
static const uint32 kMsgStatus = 'lvSt';
static const uint32 kMsgPulse = 'lvPu';

static const bigtime_t kPulse = 50000;


static float
ui_scale()
{
	return std::max(1.0f, be_plain_font->Size() / 12.0f);
}


static void
post_status(BMessenger& messenger, const char* text)
{
	BMessage message(kMsgStatus);
	message.AddString("text", text);
	messenger.SendMessage(&message, (BHandler*)NULL, 0);
}


/*!	An application that went away without disconnecting leaves the output
	it recorded from connected to nothing, and a sound card does not notice:
	disconnect what is connected to a port that is gone. Returns the free
	output, if there is one then.
*/
static bool
find_free_output(BMediaRoster* roster, const media_node& node,
	media_type type, media_output& output)
{
	int32 count = 0;
	if (roster->GetFreeOutputsFor(node, &output, 1, &count, type) == B_OK
		&& count > 0) {
		return true;
	}

	media_output outputs[16];
	if (roster->GetConnectedOutputsFor(node, outputs, 16, &count) != B_OK)
		return false;

	bool cleared = false;
	for (int32 i = 0; i < count; i++) {
		port_info portInfo;
		if (get_port_info(outputs[i].destination.port, &portInfo) == B_OK)
			continue;
		roster->Disconnect(node.node, outputs[i].source, node.node,
			outputs[i].destination);
		cleared = true;
	}

	count = 0;
	return cleared && roster->GetFreeOutputsFor(node, &output, 1, &count,
			type) == B_OK && count > 0;
}


// #pragma mark - CameraPreview


CameraPreview::CameraPreview(const media_node& node)
	:
	BView("camera preview", B_WILL_DRAW | B_FRAME_EVENTS
		| B_FULL_UPDATE_ON_RESIZE),
	fNode(node),
	fRecorder(NULL),
	fStartThread(-1),
	fLock("camera preview"),
	fFrame(NULL),
	fFramePending(0),
	fAspect(16.0f / 9.0f),
	fRunning(false)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
}


CameraPreview::~CameraPreview()
{
	Stop();
	delete fFrame;
}


void
CameraPreview::AttachedToWindow()
{
	BView::AttachedToWindow();
	fMessenger = BMessenger(this);
	Start();
}


void
CameraPreview::DetachedFromWindow()
{
	Stop();
	BView::DetachedFromWindow();
}


BSize
CameraPreview::MinSize()
{
	float scale = ui_scale();
	return BLayoutUtils::ComposeSize(ExplicitMinSize(),
		BSize(160 * scale, 200 * scale));
}


BSize
CameraPreview::MaxSize()
{
	return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
		BSize(B_SIZE_UNLIMITED, 200 * ui_scale()));
}


BSize
CameraPreview::PreferredSize()
{
	float scale = ui_scale();
	return BLayoutUtils::ComposeSize(ExplicitPreferredSize(),
		BSize(356 * scale, 200 * scale));
}


bool
CameraPreview::HasHeightForWidth()
{
	return false;
}


void
CameraPreview::GetHeightForWidth(float width, float* min, float* max,
	float* preferred)
{
	BView::GetHeightForWidth(width, min, max, preferred);
}


void
CameraPreview::Start()
{
	if (fRunning || fStartThread >= 0 || !fMessenger.IsValid())
		return;

	_SetStatus(B_TRANSLATE("Starting the camera…"));
	fStartThread = spawn_thread(&_StartThread, "camera preview",
		B_NORMAL_PRIORITY, this);
	if (fStartThread < 0) {
		_SetStatus(B_TRANSLATE("The camera could not be started."));
		return;
	}
	resume_thread(fStartThread);
}


void
CameraPreview::Stop()
{
	_WaitForStart();

	BMediaRecorder* recorder;
	{
		BAutolock _(fLock);
		recorder = fRecorder;
		fRecorder = NULL;
		fRunning = false;
	}
	if (recorder == NULL)
		return;

	// Stop the camera, so its light goes out, then let go of it.
	recorder->Stop();
	BMediaRoster* roster = BMediaRoster::CurrentRoster();
	if (roster != NULL)
		roster->StopNode(fNode, 0, true);
	recorder->Disconnect();
	delete recorder;

	_SetStatus(B_TRANSLATE("Preview paused"));
}


void
CameraPreview::_WaitForStart()
{
	if (fStartThread >= 0) {
		status_t result;
		wait_for_thread(fStartThread, &result);
		fStartThread = -1;
	}
}


status_t
CameraPreview::_StartThread(void* data)
{
	CameraPreview* self = (CameraPreview*)data;
	BMediaRoster* roster = BMediaRoster::Roster();

	media_output output;
	if (!find_free_output(roster, self->fNode, B_MEDIA_RAW_VIDEO, output)) {
		post_status(self->fMessenger, B_TRANSLATE("The camera is being used "
			"by another application. Changes still apply."));
		return B_BUSY;
	}

	BMediaRecorder* recorder = new BMediaRecorder("Media preferences preview",
		B_MEDIA_RAW_VIDEO);
	status_t status = recorder->InitCheck();
	if (status == B_OK) {
		recorder->SetHooks(&_ProcessFrame, NULL, self);

		media_format format;
		format.type = B_MEDIA_RAW_VIDEO;
		format.u.raw_video = media_raw_video_format::wildcard;
		format.u.raw_video.display.format = B_RGB32;
		format.u.raw_video.display.line_width = 640;
		format.u.raw_video.display.line_count = 360;
		status = recorder->Connect(self->fNode, &output, &format);
		if (status != B_OK) {
			// whatever size the camera likes
			format.u.raw_video.display.line_width = 0;
			format.u.raw_video.display.line_count = 0;
			status = recorder->Connect(self->fNode, &output, &format);
		}
		if (status == B_OK)
			status = recorder->Start();
	}

	if (status != B_OK) {
		if (recorder->IsConnected())
			recorder->Disconnect();
		delete recorder;

		BString text(B_TRANSLATE("The camera could not be started: %error%."));
		text.ReplaceFirst("%error%", strerror(status));
		post_status(self->fMessenger, text);
		return status;
	}

	BAutolock _(self->fLock);
	self->fRecorder = recorder;
	self->fRunning = true;
	return B_OK;
}


void
CameraPreview::_ProcessFrame(void* cookie, bigtime_t timestamp, void* data,
	size_t size, const media_format& format)
{
	((CameraPreview*)cookie)->_Frame(data, size, format);
}


void
CameraPreview::_Frame(const void* data, size_t size,
	const media_format& format)
{
	const media_video_display_info& display = format.u.raw_video.display;
	uint32 width = display.line_width;
	uint32 height = display.line_count;
	uint32 bytesPerRow = display.bytes_per_row != 0
		? display.bytes_per_row : width * 4;
	if (width == 0 || height == 0
		|| (display.format != B_RGB32 && display.format != B_RGBA32)
		|| size < (size_t)bytesPerRow * height) {
		return;
	}

	{
		BAutolock _(fLock);
		if (fFrame == NULL || fFrame->Bounds().IntegerWidth() + 1
				!= (int32)width
			|| fFrame->Bounds().IntegerHeight() + 1 != (int32)height) {
			delete fFrame;
			fFrame = new BBitmap(BRect(0, 0, width - 1, height - 1), B_RGB32);
		}

		uint8* bits = (uint8*)fFrame->Bits();
		int32 targetBytesPerRow = fFrame->BytesPerRow();
		const uint8* source = (const uint8*)data;
		if ((uint32)targetBytesPerRow == bytesPerRow)
			memcpy(bits, source, (size_t)bytesPerRow * height);
		else {
			for (uint32 y = 0; y < height; y++) {
				memcpy(bits + y * targetBytesPerRow, source + y * bytesPerRow,
					std::min((uint32)targetBytesPerRow, width * 4));
			}
		}
		fAspect = (float)width / height;
	}

	if (atomic_or(&fFramePending, 1) == 0) {
		BMessage message(kMsgFrame);
		if (fMessenger.SendMessage(&message, (BHandler*)NULL, 0) != B_OK)
			atomic_and(&fFramePending, 0);
	}
}


void
CameraPreview::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgFrame:
			atomic_and(&fFramePending, 0);
			Invalidate();
			break;

		case kMsgStatus:
		{
			const char* text;
			if (message->FindString("text", &text) == B_OK)
				_SetStatus(text);
			break;
		}

		default:
			BView::MessageReceived(message);
			break;
	}
}


void
CameraPreview::_SetStatus(const char* status)
{
	{
		BAutolock _(fLock);
		fStatus = status;
	}
	if (Window() != NULL)
		Invalidate();
}


void
CameraPreview::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	float radius = 6 * ui_scale();

	SetDrawingMode(B_OP_OVER);
	SetHighColor(22, 23, 26);
	FillRoundRect(bounds, radius, radius);
	SetDrawingMode(B_OP_COPY);

	BAutolock _(fLock);
	if (fRunning && fFrame != NULL) {
		BRect target = bounds;
		float width = bounds.Height() * fAspect;
		if (width <= bounds.Width()) {
			target.left = floorf(bounds.left + (bounds.Width() - width) / 2);
			target.right = target.left + floorf(width);
		} else {
			float height = bounds.Width() / fAspect;
			target.top = floorf(bounds.top + (bounds.Height() - height) / 2);
			target.bottom = target.top + floorf(height);
		}
		DrawBitmap(fFrame, fFrame->Bounds(), target,
			B_FILTER_BITMAP_BILINEAR);
		return;
	}

	BString text(fStatus);
	BFont font(be_plain_font);
	SetFont(&font);
	TruncateString(&text, B_TRUNCATE_END, bounds.Width() - 16);
	font_height height;
	font.GetHeight(&height);
	SetHighColor(210, 212, 216);
	SetLowColor(22, 23, 26);
	DrawString(text, BPoint(
		floorf(bounds.left + (bounds.Width() - StringWidth(text)) / 2),
		floorf(bounds.top + (bounds.Height() + height.ascent
			- height.descent) / 2)));
}


void
CameraPreview::FrameResized(float width, float height)
{
	BView::FrameResized(width, height);
	Invalidate();
}


// #pragma mark - LevelMeter


LevelMeter::LevelMeter()
	:
	BView("level meter", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE),
	fRecorder(NULL),
	fHasInput(false),
	fStartThread(-1),
	fRunner(NULL),
	fLock("level meter"),
	fPeak(0),
	fLevel(0),
	fHold(0),
	fHoldTime(0)
{
	SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
}


LevelMeter::~LevelMeter()
{
	_Stop();
}


BSize
LevelMeter::MinSize()
{
	font_height height;
	be_plain_font->GetHeight(&height);
	return BLayoutUtils::ComposeSize(ExplicitMinSize(),
		BSize(80 * ui_scale(), ceilf(height.ascent + height.descent) + 4));
}


BSize
LevelMeter::MaxSize()
{
	return BLayoutUtils::ComposeSize(ExplicitMaxSize(),
		BSize(B_SIZE_UNLIMITED, MinSize().height));
}


BSize
LevelMeter::PreferredSize()
{
	return BLayoutUtils::ComposeSize(ExplicitPreferredSize(),
		BSize(300 * ui_scale(), MinSize().height));
}


void
LevelMeter::AttachedToWindow()
{
	BView::AttachedToWindow();
	fMessenger = BMessenger(this);

	_SetStatus(B_TRANSLATE("Listening…"));
	fStartThread = spawn_thread(&_StartThread, "level meter",
		B_NORMAL_PRIORITY, this);
	if (fStartThread >= 0)
		resume_thread(fStartThread);

	BMessage pulse(kMsgPulse);
	fRunner = new BMessageRunner(fMessenger, &pulse, kPulse);
}


void
LevelMeter::DetachedFromWindow()
{
	_Stop();
	BView::DetachedFromWindow();
}


status_t
LevelMeter::_StartThread(void* data)
{
	LevelMeter* self = (LevelMeter*)data;
	BMediaRoster* roster = BMediaRoster::Roster();

	media_node input;
	if (roster->GetAudioInput(&input) != B_OK) {
		post_status(self->fMessenger, B_TRANSLATE("There is no sound input."));
		return B_ERROR;
	}

	media_output output;
	if (!find_free_output(roster, input, B_MEDIA_RAW_AUDIO, output)) {
		roster->ReleaseNode(input);
		post_status(self->fMessenger, B_TRANSLATE("The input is being used by "
			"another application."));
		return B_BUSY;
	}

	BMediaRecorder* recorder = new BMediaRecorder("Media preferences level",
		B_MEDIA_RAW_AUDIO);
	status_t status = recorder->InitCheck();
	if (status == B_OK) {
		recorder->SetHooks(&_ProcessBuffer, NULL, self);
		media_format format;
		format.type = B_MEDIA_RAW_AUDIO;
		format.u.raw_audio = media_raw_audio_format::wildcard;
		status = recorder->Connect(input, &output, &format);
		if (status == B_OK)
			status = recorder->Start();
		if (status == B_OK) {
			// A sound input is its own time source, which is all
			// BMediaRecorder starts; an input nothing plays to (a USB
			// microphone) has to be started as a node to record.
			BTimeSource* timeSource = roster->MakeTimeSourceFor(input);
			if (timeSource != NULL) {
				roster->StartNode(input, timeSource->Now());
				timeSource->Release();
			}
		}
	}

	if (status != B_OK) {
		if (recorder->IsConnected())
			recorder->Disconnect();
		delete recorder;
		roster->ReleaseNode(input);
		post_status(self->fMessenger,
			B_TRANSLATE("The input level cannot be shown."));
		return status;
	}

	BAutolock _(self->fLock);
	self->fRecorder = recorder;
	self->fInput = input;
	self->fHasInput = true;
	self->fStatus = "";
	return B_OK;
}


void
LevelMeter::_Stop()
{
	delete fRunner;
	fRunner = NULL;

	if (fStartThread >= 0) {
		status_t result;
		wait_for_thread(fStartThread, &result);
		fStartThread = -1;
	}

	BMediaRecorder* recorder;
	{
		BAutolock _(fLock);
		recorder = fRecorder;
		fRecorder = NULL;
	}
	if (recorder != NULL) {
		// Only our own node stops: the input is often the output, too.
		recorder->Stop();
		recorder->Disconnect();
		delete recorder;
	}
	if (fHasInput) {
		BMediaRoster* roster = BMediaRoster::CurrentRoster();
		if (roster != NULL) {
			// The input stops again when nothing else uses it.
			media_output outputs[4];
			media_input inputs[4];
			int32 outputCount = 0;
			int32 inputCount = 0;
			if (recorder != NULL
				&& roster->GetConnectedOutputsFor(fInput, outputs, 4,
					&outputCount) == B_OK
				&& roster->GetConnectedInputsFor(fInput, inputs, 4,
					&inputCount) == B_OK
				&& outputCount == 0 && inputCount == 0) {
				roster->StopNode(fInput, 0, true);
			}
			roster->ReleaseNode(fInput);
		}
		fHasInput = false;
	}
}


void
LevelMeter::_ProcessBuffer(void* cookie, bigtime_t timestamp, void* data,
	size_t size, const media_format& format)
{
	LevelMeter* self = (LevelMeter*)cookie;
	float peak = 0;

	switch (format.u.raw_audio.format) {
		case media_raw_audio_format::B_AUDIO_FLOAT:
		{
			const float* samples = (const float*)data;
			for (size_t i = 0; i < size / sizeof(float); i++)
				peak = std::max(peak, fabsf(samples[i]));
			break;
		}
		case media_raw_audio_format::B_AUDIO_INT:
		{
			const int32* samples = (const int32*)data;
			for (size_t i = 0; i < size / sizeof(int32); i++)
				peak = std::max(peak, fabsf(samples[i] / 2147483648.0f));
			break;
		}
		case media_raw_audio_format::B_AUDIO_SHORT:
		{
			const int16* samples = (const int16*)data;
			for (size_t i = 0; i < size / sizeof(int16); i++)
				peak = std::max(peak, fabsf(samples[i] / 32768.0f));
			break;
		}
		case media_raw_audio_format::B_AUDIO_CHAR:
		{
			const int8* samples = (const int8*)data;
			for (size_t i = 0; i < size; i++)
				peak = std::max(peak, fabsf(samples[i] / 128.0f));
			break;
		}
		case media_raw_audio_format::B_AUDIO_UCHAR:
		{
			const uint8* samples = (const uint8*)data;
			for (size_t i = 0; i < size; i++)
				peak = std::max(peak, fabsf((samples[i] - 128) / 128.0f));
			break;
		}
	}

	BAutolock _(self->fLock);
	self->fPeak = std::max(self->fPeak, peak);
}


void
LevelMeter::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgPulse:
		{
			float peak;
			{
				BAutolock _(fLock);
				peak = fPeak;
				fPeak = 0;
			}
			float decibels = 20 * log10f(std::max(peak, 0.00001f));
			float level = std::max(0.0f, std::min(1.0f,
				(decibels + 60) / 60));
			fLevel = std::max(level, fLevel * 0.8f);

			bigtime_t now = system_time();
			if (level >= fHold) {
				fHold = level;
				fHoldTime = now;
			} else if (now - fHoldTime > 1000000)
				fHold = std::max(level, fHold - 0.02f);

			Invalidate();
			break;
		}

		case kMsgStatus:
		{
			const char* text;
			if (message->FindString("text", &text) == B_OK)
				_SetStatus(text);
			break;
		}

		default:
			BView::MessageReceived(message);
			break;
	}
}


void
LevelMeter::_SetStatus(const char* status)
{
	{
		BAutolock _(fLock);
		fStatus = status;
	}
	Invalidate();
}


void
LevelMeter::Draw(BRect updateRect)
{
	BRect bounds = Bounds();
	BString status;
	bool running;
	{
		BAutolock _(fLock);
		status = fStatus;
		running = fRecorder != NULL;
	}

	if (!running) {
		font_height height;
		GetFontHeight(&height);
		SetHighColor(dimmed_text_color());
		TruncateString(&status, B_TRUNCATE_END, bounds.Width());
		DrawString(status, BPoint(bounds.left, floorf(bounds.top
			+ (bounds.Height() + height.ascent - height.descent) / 2)));
		return;
	}

	float barHeight = ceilf(8 * ui_scale());
	BRect track(bounds.left, floorf(bounds.top
			+ (bounds.Height() - barHeight) / 2),
		bounds.right, 0);
	track.bottom = track.top + barHeight;
	float radius = barHeight / 2;

	SetDrawingMode(B_OP_OVER);
	SetHighColor(mix_color(LowColor(), ui_color(B_PANEL_TEXT_COLOR), 40));
	FillRoundRect(track, radius, radius);

	if (fLevel > 0.005f) {
		BGradientLinear gradient(track.LeftTop(), track.RightTop());
		gradient.AddColor(make_color(52, 199, 89), 0);
		gradient.AddColor(make_color(52, 199, 89), 150);
		gradient.AddColor(make_color(255, 204, 0), 210);
		gradient.AddColor(make_color(255, 59, 48), 255);

		BRect level(track);
		level.right = std::max(level.left + barHeight,
			floorf(level.left + track.Width() * fLevel));
		FillRoundRect(level, radius, radius, gradient);
	}

	if (fHold > 0.01f) {
		float x = floorf(track.left + track.Width() * fHold);
		SetHighColor(ui_color(B_PANEL_TEXT_COLOR));
		StrokeLine(BPoint(x, track.top), BPoint(x, track.bottom));
	}
	SetDrawingMode(B_OP_COPY);
}
