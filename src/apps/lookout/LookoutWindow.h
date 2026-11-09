/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef LOOKOUT_WINDOW_H
#define LOOKOUT_WINDOW_H


#include <Entry.h>
#include <Message.h>
#include <String.h>
#include <StringList.h>
#include <Window.h>

#include "Camera.h"
#include "Engine.h"
#include "Snapshot.h"


class BFilePanel;
class BMenu;
class BMenuItem;
class BRefFilter;
class BStringView;
class PictureView;
class ResultsView;


//! What the application tells the window.
enum {
	/*!	A picture was read: "frame" (pointer to a Snapshot, with a reference
		that becomes the window's), "ref", and "restore" (bool) when it is
		the picture of last time, which does not change the source. */
	MSG_SET_PICTURE				= 'LwPc',
	//! A file was neither a picture nor a model: "ref".
	MSG_PICTURE_FAILED			= 'LwPf',
	//! "ref": a model to use.
	MSG_SET_MODEL				= 'LwMd',
	/*!	From the command line: "model" (ref), "device" (an identifier, or
		"auto"), "source" ("picture", "camera"), "continuous" (bool),
		"verbose" (bool); all optional. */
	MSG_COMMAND					= 'LwCm'
};

enum {
	SOURCE_PICTURE = 0,
	SOURCE_CAMERA
};


class LookoutWindow : public BWindow {
public:
								LookoutWindow(Engine* engine, Camera* camera,
									const BMessage& settings);
	virtual						~LookoutWindow();

	virtual	void				MessageReceived(BMessage* message);
	virtual	void				MenusBeginning();
	virtual	bool				QuitRequested();

	static	status_t			LoadSettings(BMessage& settings);

private:
			void				_SetSource(int32 source, bool force = false);
			void				_SetPicture(BMessage* message);
			void				_Configure(const entry_ref* model, bool byHand,
									const char* device);
			void				_SetContinuous(bool continuous);
			void				_HandleCommand(BMessage* message);

			void				_EngineStateChanged(BMessage* message);
			void				_ResultArrived(BMessage* message);
			void				_CameraStateChanged(BMessage* message);
			void				_CameraFrameArrived();
			void				_ClearResult();

			void				_BuildSourceMenu();
			void				_BuildDeviceMenu();
			void				_BuildModelMenu();
			void				_FindModels(BStringList& paths) const;
			bool				_ChooseDefaultModel();

			void				_UpdateNote();
			void				_UpdateStatus();
			void				_UpdateTitle();
			BString				_StatusText() const;
			bool				_IsStream() const;
			void				_Log(const char* format, ...) const;
			status_t			_SaveSettings() const;

private:
			Engine*				fEngine;
			Camera*				fCamera;

			PictureView*		fPictureView;
			ResultsView*		fResultsView;
			BStringView*		fStatusView;
			BMenu*				fSourceMenu;
			BMenu*				fDeviceMenu;
			BMenu*				fModelMenu;
			BMenuItem*			fContinuousItem;
			BFilePanel*			fPicturePanel;
			BFilePanel*			fModelPanel;
			BRefFilter*			fPictureFilter;
			BRefFilter*			fModelFilter;

	// the source
			int32				fSource;
			int32				fSourceNumber;
			SnapshotReference	fPicture;
			entry_ref			fPictureRef;
			BString				fPictureError;
			BString				fCameraChoice;
			camera_state		fCameraState;
			BString				fCameraName;
			BString				fCameraError;
			bool				fCameraShown;
			BStringList			fInputs;
			BString				fDefaultInput;

	// the model and the device
			entry_ref			fModelRef;
			entry_ref			fOpenedModel;
			BString				fDeviceChoice;
			BMessage			fDevices;
			BMessage			fEngineState;
			BMessage			fResult;
			bool				fHasResult;
			bool				fContinuous;

			bool				fVerbose;
	mutable	bigtime_t			fLastLog;
	mutable	int32				fLoggedFrames;
};


#endif	// LOOKOUT_WINDOW_H
