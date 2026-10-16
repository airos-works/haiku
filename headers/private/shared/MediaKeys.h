/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _MEDIA_KEYS_H
#define _MEDIA_KEYS_H


#include <Message.h>


/*!	The media keys of a keyboard (play/pause, next and previous track, stop)
	reach the applications through the mediakey command, which the Shortcuts
	preferences bind to the consumer-page key codes by default.

	The command first asks every running application what it could do with
	the key (a query), then sends the key to one of them: an application that
	is playing, else the active application, else the one started last.

	Both messages have kMsgMediaKey as their what code:

	query:		"key" int32 (media_key), "query" true
		An application that can act on this key replies with the same what
		code and "can_handle" true, plus "playing" true when it is playing
		right now. No reply, or any other reply, means it cannot.
	key:		"key" int32 (media_key)
		The application acts on the key and replies "handled" bool.
*/


enum {
	kMsgMediaKey = 'mkey'
};


enum media_key {
	B_MEDIA_KEY_PLAY_PAUSE = 0,
	B_MEDIA_KEY_PLAY,
	B_MEDIA_KEY_PAUSE,
	B_MEDIA_KEY_STOP,
	B_MEDIA_KEY_NEXT,
	B_MEDIA_KEY_PREVIOUS
};


static inline const char*
media_key_name(int32 key)
{
	switch (key) {
		case B_MEDIA_KEY_PLAY_PAUSE:
			return "play-pause";
		case B_MEDIA_KEY_PLAY:
			return "play";
		case B_MEDIA_KEY_PAUSE:
			return "pause";
		case B_MEDIA_KEY_STOP:
			return "stop";
		case B_MEDIA_KEY_NEXT:
			return "next";
		case B_MEDIA_KEY_PREVIOUS:
			return "previous";
	}
	return "?";
}


//! Answers a query from the mediakey command; for an application's use.
static inline void
reply_media_key_query(BMessage* message, bool canHandle, bool playing)
{
	BMessage reply(kMsgMediaKey);
	reply.AddBool("can_handle", canHandle);
	reply.AddBool("playing", playing);
	message->SendReply(&reply);
}


static inline void
reply_media_key_handled(BMessage* message, bool handled)
{
	BMessage reply(kMsgMediaKey);
	reply.AddBool("handled", handled);
	message->SendReply(&reply);
}


#endif	// _MEDIA_KEYS_H
