/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef CAMERA_H
#define CAMERA_H


#include <Locker.h>
#include <Looper.h>
#include <MediaDefs.h>
#include <MediaNode.h>
#include <Messenger.h>
#include <String.h>
#include <StringList.h>

#include "Snapshot.h"


class BMediaRoster;
class Engine;
class FrameConsumer;


//! What the camera tells its target.
enum {
	/*!	The video inputs there are, whenever they change: "name" (one per
		input) and "default" (the name of the system's default one). */
	MSG_CAMERA_INPUTS			= 'LcIn',
	/*!	"state" (int32, camera_state). CAMERA_RUNNING: "name", "width",
		"height" (int32). CAMERA_FAILED: "error". */
	MSG_CAMERA_STATE			= 'LcSt',
	//! There is a frame in DisplaySlot().
	MSG_CAMERA_FRAME			= 'LcFr'
};

enum camera_state {
	CAMERA_OFF = 0,
	CAMERA_STARTING,
	CAMERA_RUNNING,
	//! there is no video input
	CAMERA_MISSING,
	CAMERA_FAILED
};


/*!	The video input, through the Media Kit: a consumer node of this
	program's, connected to the camera's node while the camera is wanted
	and to nothing while it is not (which is what switches a camera's
	light off). Frames go to the engine and into a slot for the window.

	All of it happens in this looper's thread: the roster's calls wait for
	other teams, and neither the window nor the engine should.
*/
class Camera : public BLooper {
public:
								Camera(Engine* engine);
	virtual						~Camera();

	//! Who gets the messages above; before Run().
			void				SetTarget(const BMessenger& target);

	// from any thread
	/*!	Starts the input of that name, or the system's default one for an
		empty name. Frames carry \a source. */
			void				Start(const char* name, int32 source);
			void				Stop();
	/*!	Lets go of the camera and returns when that is done. For a looper
		that runs; Quit() is next. */
			void				Shutdown();

			SnapshotSlot*	DisplaySlot()
									{ return &fDisplaySlot; }
	//! Frames since the camera was started.
			int32				CountFrames() const
									{ return atomic_get((int32*)&fFrames); }

protected:
	virtual	void				MessageReceived(BMessage* message);

private:
	friend class FrameConsumer;

			void				_Apply(bool retry);
			status_t			_Connect(const BString& name, int32 source,
									BString& _error);
			status_t			_FindProducer(const BString& name);
			void				_Disconnect();
			bool				_UpdateInputs();
			void				_SendState(camera_state state,
									const char* error = NULL);
			void				_FrameReceived(Snapshot* frame);

private:
			BMessenger			fTarget;
			Engine*				fEngine;
			SnapshotSlot	fDisplaySlot;
			SnapshotPool*	fPool;
			int32				fFrames;

	// what is wanted, from any thread
			BLocker				fLock;
			bool				fWanted;
			BString				fWantedName;
			int32				fWantedSource;
			bool				fApplyPending;

	// what there is, the looper's own
			BMediaRoster*		fRoster;
			bool				fWatching;
			BStringList			fInputs;
			BString				fDefaultInput;

			FrameConsumer*		fConsumer;
			media_node			fProducer;
			media_output		fOutput;
			media_input			fInput;
			bool				fConnected;
			BString				fName;
			camera_state		fState;
};


#endif	// CAMERA_H
