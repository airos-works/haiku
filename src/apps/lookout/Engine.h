/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef ENGINE_H
#define ENGINE_H


#include <Entry.h>
#include <Locker.h>
#include <Looper.h>
#include <Messenger.h>
#include <NeuralKit.h>
#include <String.h>
#include <StringList.h>

#include "Snapshot.h"


//! What the engine tells its target.
enum {
	/*!	The devices that run networks, whenever they change: "device"
		(identifiers), "name", "kind" (int32, neural_device_kind), one of
		each per device. */
	MSG_ENGINE_DEVICES			= 'LeDv',
	/*!	"state" (int32, engine_state). ENGINE_LOADING: "model" (ref),
		"model name". ENGINE_READY also: "kind" (int32, model_kind),
		"device", "device name", "device kind", "on device" and "on cpu"
		(int32, operators), "failures" (int32, runs the device failed and
		the CPU did), "choice" (the device asked for, empty: automatic),
		"choice missing" (bool), "labels" (int32).
		ENGINE_FAILED: "error". */
	MSG_ENGINE_STATE			= 'LeSt',
	/*!	One run is over: "source" (int32, the frame's), "kind", "duration"
		(int64, microseconds), "rate" (float, runs per second, 0 when the
		runs do not follow each other), "on device", "on cpu", "failures",
		and per
		thing found "label", "score" (float), "class" (int32) and, from
		detection models, "box" (rect, in the picture's pixels). */
	MSG_ENGINE_RESULT			= 'LeRs'
};

enum engine_state {
	ENGINE_IDLE = 0,
	ENGINE_LOADING,
	ENGINE_READY,
	ENGINE_FAILED
};

enum model_kind {
	MODEL_CLASSIFICATION = 0,
	MODEL_DETECTION
};


/*!	Runs the model. The engine is a looper of its own and the only one to
	touch the session, its tensors and the roster: setting a session up
	takes seconds on a graphics processor, taking one down has to wait for
	the run that is going on, and neither may hold up the window. The
	window says what it wants (a model, a device, a frame) and returns at
	once; what comes of it arrives as messages.

	Runs are asynchronous too (BNeuralSession::RunAsync() with the engine
	as the target), so that the engine keeps answering while a network
	runs: frames that arrive in the meantime replace each other in a slot
	and the newest one is taken when the run is over.
*/
class Engine : public BLooper {
public:
								Engine();
	virtual						~Engine();

	//! Who gets the messages above; before Run().
			void				SetTarget(const BMessenger& target);

	// from any thread
	/*!	The model to run and the device to run it on; NULL leaves the one
		or the other as it is, and an empty device identifier leaves the
		choice to the Neural Kit. Both at once make one session, not two
		of which the first is thrown away. */
			void				Configure(const entry_ref* model,
									const char* device);
	//! Frames and results of other sources are dropped from now on.
			void				SetSource(int32 source);
			void				SetFrame(Snapshot* frame);
	//! Run again and again on a picture that does not change.
			void				SetContinuous(bool continuous);
	/*!	Ends the session and returns when that is done (a run that is
		going on is waited for). For a looper that runs; Quit() is next. */
			void				Shutdown();

	/*!	What BNeuralUtils::IsDetector() says of a session, for a model
		that has none yet: the window picks a model to start with. */
	static	bool				IsDetectionModel(const BNeuralModel& model);

protected:
	virtual	void				MessageReceived(BMessage* message);

private:
			void				_Configure(bool force);
			void				_DropSession();
			status_t			_FindLabels(const entry_ref& model);
			void				_Next(bool again);
			void				_StartRun(Snapshot* frame);
			void				_RunDone(BMessage* message);
			void				_SendDevices();
			void				_SendState(engine_state state,
									const char* error = NULL);
			void				_SendResult(Snapshot* frame,
									bigtime_t duration);
			void				_AddRunInfo(BMessage& message) const;
			float				_Rate();
			void				_Send(BMessage& message);

private:
			BMessenger			fTarget;

	// what is wanted, from any thread
			BLocker				fLock;
			entry_ref			fWantedModel;
			BString				fWantedDevice;
			bool				fConfigurePending;
			bool				fShutDown;
			SnapshotSlot	fSlot;
			int32				fSource;
			int32				fContinuous;

	// what there is, the looper's own
			entry_ref			fModelRef;
			BString				fDeviceChoice;
			bool				fChoiceMissing;
			BNeuralModel		fModel;
			BNeuralSession*		fSession;
			model_kind			fKind;
			BStringList			fLabels;
			int32				fLabelOffset;

			int32				fGeneration;
			bool				fRunning;
			SnapshotReference	fRunFrame;
			SnapshotReference	fFrame;
			neural_image_mapping fMapping;

	// the last result that was passed on (the frame is only compared)
			const Snapshot*	fSentFrame;
			int32				fSentGeneration;
			bigtime_t			fSentTime;

	static	const int32			kRateSamples = 32;
			bigtime_t			fDone[kRateSamples];
			int32				fDoneCount;
};


#endif	// ENGINE_H
