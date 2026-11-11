/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Thumbnailer.h"

#include <Autolock.h>
#include <Bitmap.h>
#include <Entry.h>
#include <MediaFile.h>
#include <MediaTrack.h>
#include <Message.h>
#include <String.h>
#include <View.h>

#include <string.h>
#include <syslog.h>

#include <algorithm>

#include <QuickLookDefs.h>

#include "AddOnManager.h"


static const int32 kMaxQueued = 400;


static BBitmap*
ScaleDown(BBitmap* source, int32 size)
{
	// a plain B_RGBA32 bitmap, no larger than size on a side
	BRect bounds(source->Bounds());
	float width = bounds.Width() + 1;
	float height = bounds.Height() + 1;
	float scale = std::min(1.0f, std::min(size / width, size / height));
	BRect target(0, 0, std::max(1.0f, floorf(width * scale)) - 1,
		std::max(1.0f, floorf(height * scale)) - 1);

	BBitmap* scaled = new BBitmap(target, B_RGBA32, true);
	BView* view = new BView(target, "scale", 0, 0);
	scaled->AddChild(view);
	if (!scaled->Lock()) {
		delete scaled;
		return NULL;
	}
	view->DrawBitmap(source, bounds, target, B_FILTER_BITMAP_BILINEAR);
	view->Sync();
	scaled->RemoveChild(view);
	delete view;
	scaled->Unlock();

	BBitmap* plain = new BBitmap(target, B_RGBA32);
	plain->ImportBits(scaled);
	delete scaled;
	return plain;
}


static BBitmap*
VideoFrame(const entry_ref& ref)
{
	// a frame a tenth of the way in, decoded by the Media Kit
	BMediaFile file(&ref);
	if (file.InitCheck() != B_OK)
		return NULL;

	for (int32 i = 0; i < file.CountTracks(); i++) {
		BMediaTrack* track = file.TrackAt(i);
		if (track == NULL)
			continue;

		media_format format;
		if (track->EncodedFormat(&format) != B_OK
			|| format.type != B_MEDIA_ENCODED_VIDEO) {
			file.ReleaseTrack(track);
			continue;
		}

		media_format decoded;
		decoded.type = B_MEDIA_RAW_VIDEO;
		decoded.u.raw_video.display.format = B_RGB32;
		if (track->DecodedFormat(&decoded) != B_OK
			|| decoded.u.raw_video.display.format != B_RGB32) {
			file.ReleaseTrack(track);
			return NULL;
		}

		int32 width = decoded.u.raw_video.display.line_width;
		int32 height = decoded.u.raw_video.display.line_count;
		int32 bytesPerRow = decoded.u.raw_video.display.bytes_per_row;
		if (width <= 0 || height <= 0) {
			file.ReleaseTrack(track);
			return NULL;
		}

		bigtime_t when = track->Duration() / 10;
		if (when > 0)
			track->SeekToTime(&when, B_MEDIA_SEEK_CLOSEST_BACKWARD);

		BBitmap* frame = new BBitmap(BRect(0, 0, width - 1, height - 1),
			B_RGB32);
		if (bytesPerRow <= 0)
			bytesPerRow = frame->BytesPerRow();
		uint8* buffer = new uint8[bytesPerRow * height + 4096];
		int64 count = 0;
		media_header header;
		status_t status = track->ReadFrames(buffer, &count, &header);
		if (status == B_OK && count > 0) {
			for (int32 y = 0; y < height; y++) {
				memcpy((uint8*)frame->Bits() + y * frame->BytesPerRow(),
					buffer + y * bytesPerRow,
					std::min((int32)frame->BytesPerRow(), bytesPerRow));
			}
		} else {
			delete frame;
			frame = NULL;
		}
		delete[] buffer;
		file.ReleaseTrack(track);
		return frame;
	}

	return NULL;
}


Thumbnailer::Thumbnailer(AddOnManager* addOns)
	:
	fAddOns(addOns),
	fLock("thumbnailer"),
	fSem(create_sem(0, "thumbnail requests")),
	fThread(-1),
	fWorking(false),
	fQuit(false)
{
	fThread = spawn_thread(_ThreadEntry, "thumbnails", B_LOW_PRIORITY, this);
	if (fThread >= 0)
		resume_thread(fThread);
}


Thumbnailer::~Thumbnailer()
{
	{
		BAutolock locker(fLock);
		fQuit = true;
	}
	delete_sem(fSem);
	status_t result;
	if (fThread >= 0)
		wait_for_thread(fThread, &result);

	while (!fQueue.empty()) {
		delete fQueue.front();
		fQueue.pop_front();
	}
}


void
Thumbnailer::Add(BMessage* request)
{
	BAutolock locker(fLock);
	if ((int32)fQueue.size() >= kMaxQueued) {
		// Tracker asks again when the item is shown again
		BMessage reply(B_REPLY);
		reply.AddInt32("error", B_BUSY);
		request->SendReply(&reply);
		delete request;
		return;
	}
	fQueue.push_back(request);
	release_sem(fSem);
}


bool
Thumbnailer::IsBusy()
{
	BAutolock locker(fLock);
	return fWorking || !fQueue.empty();
}


status_t
Thumbnailer::_ThreadEntry(void* data)
{
	((Thumbnailer*)data)->_Run();
	return B_OK;
}


void
Thumbnailer::_Run()
{
	while (acquire_sem(fSem) == B_OK) {
		BMessage* request;
		{
			BAutolock locker(fLock);
			if (fQuit)
				return;
			if (fQueue.empty())
				continue;
			// the newest first: it is what is on screen now
			request = fQueue.back();
			fQueue.pop_back();
			fWorking = true;
		}

		_Answer(request);
		delete request;

		BAutolock locker(fLock);
		fWorking = false;
	}
}


void
Thumbnailer::_Answer(BMessage* request)
{
	BMessage reply(B_REPLY);
	entry_ref ref;
	int32 size = request->GetInt32("size", 128);
	if (request->FindRef("ref", &ref) != B_OK || size <= 0 || size > 1024) {
		reply.AddInt32("error", B_BAD_VALUE);
		request->SendReply(&reply);
		return;
	}

	BString mimeType;
	BBitmap* bitmap = NULL;
	status_t status = fAddOns->CreateThumbnail(ref, size, &mimeType, &bitmap);
	if (status != B_OK && mimeType.StartsWith("video/"))
		bitmap = VideoFrame(ref);

	if (bitmap != NULL) {
		BBitmap* scaled = ScaleDown(bitmap, size);
		delete bitmap;
		bitmap = scaled;
	}

	BMessage archive;
	if (bitmap != NULL && bitmap->Archive(&archive) == B_OK)
		reply.AddMessage("bitmap", &archive);
	else {
		reply.AddInt32("error", status == B_OK ? B_ERROR : status);
		if (status != B_NOT_SUPPORTED || mimeType.StartsWith("video/")) {
			syslog(LOG_INFO, "quick_look: no thumbnail for %s (%s): %s",
				ref.name, mimeType.String(), strerror(status));
		}
	}
	delete bitmap;

	request->SendReply(&reply);
}
