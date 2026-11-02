/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _MEDIA_SESSION_H
#define _MEDIA_SESSION_H


#include <Message.h>
#include <Messenger.h>
#include <String.h>

#include <MediaControls.h>


class BBitmap;
class BHandler;


namespace BPrivate {


/*!	A player as the system media controls see it: what it plays, in which
	state, and what it can do. Register() shows it to the media_controls_server
	(the Deskbar replicant, the media keys); the target handler then gets
	kMsgMediaSessionAction messages (see ParseAction()) for the actions it
	declared with SetSupportedActions().

	All calls are asynchronous and cheap: they never wait for the server.
	The object keeps the whole state and sends it again when the server was
	restarted in between. It is not locked: use it from one thread (the
	target's looper, normally).

	Link with libmedia_controls.a.
*/
class MediaSession {
public:
								MediaSession();
								~MediaSession();
									// unregisters

			status_t			Register(const BMessenger& target,
									const char* name = NULL);
			status_t			Register(const BHandler* target,
									const char* name = NULL);
			void				Unregister();
			bool				IsRegistered() const
									{ return fRegistered; }
			int64				ID() const
									{ return fID; }

			// Each sends an update right away when registered.
			status_t			SetMetadata(const char* title,
									const char* artist = NULL,
									const char* album = NULL,
									const BBitmap* artwork = NULL);
									// artwork is copied, and scaled down to
									// at most 256 x 256
			status_t			SetPlaybackState(media_session_state state,
									bigtime_t position = -1,
									bigtime_t duration = -1,
									float rate = 1.0f);
									// position, duration < 0: unknown
			status_t			SetSupportedActions(uint32 actions);
									// B_MEDIA_ACTION_FLAG()s, default
									// B_MEDIA_DEFAULT_ACTIONS

			media_session_state	PlaybackState() const
									{ return fState; }

	// For the target's MessageReceived(): B_OK when the message is an
	// action for this session.
			status_t			ParseAction(const BMessage* message,
									media_session_action* _action,
									bigtime_t* _position = NULL) const;

private:
			void				_AddMetadata(BMessage& message) const;
			void				_AddPlayback(BMessage& message) const;
			status_t			_Send(BMessage& message);
			status_t			_SendRegister();

private:
			BMessenger			fServer;
			BMessenger			fTarget;
			BString				fName;
			int64				fID;
			bool				fRegistered;

			BString				fTitle;
			BString				fArtist;
			BString				fAlbum;
			BMessage			fArtwork;
			bool				fHasMetadata;

			media_session_state	fState;
			bigtime_t			fPosition;
			bigtime_t			fPositionTime;
			bigtime_t			fDuration;
			float				fRate;

			uint32				fActions;
};


}	// namespace BPrivate


using BPrivate::MediaSession;


#endif	// _MEDIA_SESSION_H
