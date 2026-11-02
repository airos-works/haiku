/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <MediaSession.h>

#include <algorithm>
#include <math.h>

#include <Application.h>
#include <Bitmap.h>
#include <Handler.h>
#include <Looper.h>
#include <View.h>


namespace BPrivate {


static const int32 kMaxArtworkSize = 256;
static const bigtime_t kSendTimeout = 100000;

static int32 sNextSessionNumber = 1;


/*!	Copies \a source into an archive, scaled down to fit kMaxArtworkSize.
*/
static status_t
archive_artwork(const BBitmap* source, BMessage& archive)
{
	BRect bounds = source->Bounds();
	float width = bounds.Width() + 1;
	float height = bounds.Height() + 1;
	if (width <= kMaxArtworkSize && height <= kMaxArtworkSize
		&& (source->ColorSpace() == B_RGBA32 || source->ColorSpace() == B_RGB32)) {
		BBitmap copy(source);
		return copy.Archive(&archive);
	}

	float scale = std::min(kMaxArtworkSize / width, kMaxArtworkSize / height);
	scale = std::min(scale, 1.0f);
	BRect scaledBounds(0, 0, std::max(1.0f, roundf(width * scale)) - 1,
		std::max(1.0f, roundf(height * scale)) - 1);

	BBitmap scaled(scaledBounds, B_BITMAP_ACCEPTS_VIEWS, B_RGBA32);
	status_t status = scaled.InitCheck();
	if (status != B_OK)
		return status;

	BView* view = new BView(scaledBounds, "scaler", B_FOLLOW_NONE, 0);
	scaled.AddChild(view);
	if (scaled.Lock()) {
		view->SetHighColor(0, 0, 0, 0);
		view->FillRect(scaledBounds);
		view->SetDrawingMode(B_OP_ALPHA);
		view->DrawBitmap(source, bounds, scaledBounds,
			B_FILTER_BITMAP_BILINEAR);
		view->Sync();
		scaled.RemoveChild(view);
		scaled.Unlock();
	}
	delete view;

	return scaled.Archive(&archive);
}


//	#pragma mark -


MediaSession::MediaSession()
	:
	fID(-1),
	fRegistered(false),
	fHasMetadata(false),
	fState(B_MEDIA_SESSION_STOPPED),
	fPosition(-1),
	fPositionTime(0),
	fDuration(-1),
	fRate(1.0f),
	fActions(B_MEDIA_DEFAULT_ACTIONS)
{
}


MediaSession::~MediaSession()
{
	Unregister();
}


status_t
MediaSession::Register(const BMessenger& target, const char* name)
{
	if (!target.IsValid())
		return B_BAD_VALUE;
	if (be_app == NULL)
		return B_NO_INIT;

	if (fRegistered)
		Unregister();

	if (fID < 0) {
		fID = ((int64)be_app->Team() << 32)
			| (uint32)atomic_add(&sNextSessionNumber, 1);
	}

	fTarget = target;
	fName = name;
	fServer = BMessenger(kMediaControlsServerSignature);
	fRegistered = true;

	return _SendRegister();
}


status_t
MediaSession::Register(const BHandler* target, const char* name)
{
	if (target == NULL || target->Looper() == NULL)
		return B_BAD_VALUE;

	return Register(BMessenger(target), name);
}


void
MediaSession::Unregister()
{
	if (!fRegistered)
		return;

	BMessage message(kMsgMediaSessionUnregister);
	message.AddInt64("session", fID);
	fServer.SendMessage(&message, (BHandler*)NULL, kSendTimeout);
	fRegistered = false;
}


status_t
MediaSession::SetMetadata(const char* title, const char* artist,
	const char* album, const BBitmap* artwork)
{
	fTitle = title;
	fArtist = artist;
	fAlbum = album;
	fArtwork.MakeEmpty();
	if (artwork != NULL && artwork->IsValid())
		archive_artwork(artwork, fArtwork);
	fHasMetadata = true;

	if (!fRegistered)
		return B_OK;

	BMessage message(kMsgMediaSessionUpdate);
	message.AddInt64("session", fID);
	_AddMetadata(message);
	return _Send(message);
}


status_t
MediaSession::SetPlaybackState(media_session_state state, bigtime_t position,
	bigtime_t duration, float rate)
{
	fState = state;
	fPosition = position;
	fPositionTime = system_time();
	fDuration = duration;
	fRate = rate;

	if (!fRegistered)
		return B_OK;

	BMessage message(kMsgMediaSessionUpdate);
	message.AddInt64("session", fID);
	_AddPlayback(message);
	return _Send(message);
}


status_t
MediaSession::SetSupportedActions(uint32 actions)
{
	fActions = actions;

	if (!fRegistered)
		return B_OK;

	BMessage message(kMsgMediaSessionUpdate);
	message.AddInt64("session", fID);
	message.AddUInt32("actions", fActions);
	return _Send(message);
}


status_t
MediaSession::ParseAction(const BMessage* message,
	media_session_action* _action, bigtime_t* _position) const
{
	if (message == NULL || message->what != kMsgMediaSessionAction
		|| message->GetInt64("session", -1) != fID)
		return B_BAD_VALUE;

	int32 action = message->GetInt32("action", -1);
	if (action < 0 || action >= B_MEDIA_ACTION_COUNT)
		return B_BAD_VALUE;

	if (_action != NULL)
		*_action = (media_session_action)action;
	if (_position != NULL)
		*_position = message->GetInt64("position", -1);
	return B_OK;
}


void
MediaSession::_AddMetadata(BMessage& message) const
{
	message.AddString("title", fTitle);
	if (fArtist.Length() > 0)
		message.AddString("artist", fArtist);
	if (fAlbum.Length() > 0)
		message.AddString("album", fAlbum);
	if (!fArtwork.IsEmpty())
		message.AddMessage("artwork", &fArtwork);
}


void
MediaSession::_AddPlayback(BMessage& message) const
{
	message.AddInt32("state", fState);
	if (fPosition >= 0) {
		message.AddInt64("position", fPosition);
		message.AddInt64("position_time", fPositionTime);
	}
	if (fDuration > 0)
		message.AddInt64("duration", fDuration);
	message.AddFloat("rate", fRate);
}


status_t
MediaSession::_Send(BMessage& message)
{
	status_t status = fServer.SendMessage(&message, (BHandler*)NULL,
		kSendTimeout);
	if (status == B_BAD_PORT_ID) {
		// The server was restarted: it does not know us anymore.
		fServer = BMessenger(kMediaControlsServerSignature);
		status = _SendRegister();
	}
	return status;
}


status_t
MediaSession::_SendRegister()
{
	BMessage message(kMsgMediaSessionRegister);
	message.AddInt64("session", fID);
	message.AddMessenger("target", fTarget);
	if (fName.Length() > 0)
		message.AddString("name", fName);
	if (fHasMetadata)
		_AddMetadata(message);
	_AddPlayback(message);
	message.AddUInt32("actions", fActions);

	return fServer.SendMessage(&message, (BHandler*)NULL, kSendTimeout);
}


}	// namespace BPrivate
