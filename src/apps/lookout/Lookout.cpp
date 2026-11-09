/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	Lookout shows a picture or what a camera sees, and marks what a neural
	network finds in it: boxes around the things a detection model knows,
	or the classes a classification model thinks most likely. It is the
	Neural Kit's demonstration: a model file and a BBitmap in, names out,
	on whichever processor the machine has for it.

	Three loopers share the work. The window draws and takes the user's
	wishes. The engine (Engine.h) owns the session and runs the model. The
	camera (Camera.h) owns the Media Kit connection. They talk in messages
	and hand frames around by reference, so that none waits for another:
	the window stays live while a model is set up on a graphics processor
	or a run is under way.
*/


#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <Catalog.h>
#include <Entry.h>
#include <Path.h>
#include <TranslationUtils.h>

#include "Camera.h"
#include "Engine.h"
#include "Snapshot.h"
#include "LookoutWindow.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Lookout"


static const char* kSignature = "application/x-vnd.airos-Lookout";

static const char* kUsage =
	"usage: Lookout [options] [<picture>] [<model>]\n"
	"\n"
	"Shows a picture or a camera and what a neural network finds in it.\n"
	"Given to a Lookout that is already running, the options and files\n"
	"change what that one shows.\n"
	"\n"
	"  --source picture|camera  what to look at\n"
	"  --device <id>|auto       the device to run the model on (see\n"
	"                           \"neural devices\"); auto: the system's choice\n"
	"  --continuous on|off      run again and again on a picture\n"
	"  --verbose                print what happens: states, results, times\n"
	"  -h, --help               this text\n";


class LookoutApp : public BApplication {
public:
								LookoutApp();
	virtual						~LookoutApp();

	virtual	void				ArgvReceived(int32 argc, char** argv);
	virtual	void				RefsReceived(BMessage* message);
	virtual	void				ReadyToRun();
	virtual	bool				QuitRequested();

private:
			void				_Open(const entry_ref& ref, bool restore,
									BMessage* command = NULL);

private:
			BMessage			fSettings;
			LookoutWindow*		fWindow;
			BMessenger			fWindowMessenger;
			Engine*				fEngine;
			Camera*				fCamera;
			bool				fStarted;
			bool				fPictureGiven;
			bool				fSourceGiven;
};


LookoutApp::LookoutApp()
	:
	BApplication(kSignature),
	fWindow(NULL),
	fEngine(NULL),
	fCamera(NULL),
	fStarted(false),
	fPictureGiven(false),
	fSourceGiven(false)
{
	LookoutWindow::LoadSettings(fSettings);

	// The engine and the camera send to the window, and the window calls
	// them: they are made before it and outlive it (QuitRequested()).
	// None of the three runs before ReadyToRun(); until then their ports
	// collect what the settings and the command line have to say.
	fEngine = new Engine();
	fCamera = new Camera(fEngine);
	fWindow = new LookoutWindow(fEngine, fCamera, fSettings);
	fWindowMessenger = BMessenger(fWindow);

	fEngine->SetTarget(fWindowMessenger);
	fCamera->SetTarget(fWindowMessenger);
}


LookoutApp::~LookoutApp()
{
}


void
LookoutApp::ArgvReceived(int32 argc, char** argv)
{
	// relative names are relative to where the command was given, which
	// is not where a Lookout that runs already is
	const char* cwd = NULL;
	if (CurrentMessage() != NULL)
		cwd = CurrentMessage()->GetString("cwd", NULL);

	BMessage command(MSG_COMMAND);
	for (int32 i = 1; i < argc; i++) {
		const char* arg = argv[i];
		const char* value = i + 1 < argc ? argv[i + 1] : NULL;

		if (strcmp(arg, "--verbose") == 0)
			command.AddBool("verbose", true);
		else if (strcmp(arg, "--source") == 0 && value != NULL) {
			command.AddString("source", value);
			fSourceGiven = true;
			i++;
		} else if (strcmp(arg, "--device") == 0 && value != NULL) {
			command.AddString("device", value);
			i++;
		} else if (strcmp(arg, "--continuous") == 0 && value != NULL) {
			command.AddBool("continuous", strcmp(value, "off") != 0);
			i++;
		} else if (strcmp(arg, "--test-source") == 0 && value != NULL) {
			// Not in the usage text: for testing where there is no
			// camera. The picture stands in for one (Camera.h), from the
			// start of the program on.
			BPath path(cwd != NULL && value[0] != '/' ? cwd : value,
				cwd != NULL && value[0] != '/' ? value : NULL);
			BBitmap* picture = fStarted
				? NULL : BTranslationUtils::GetBitmap(path.Path());
			if (picture != NULL && (picture->ColorSpace() == B_RGB32
					|| picture->ColorSpace() == B_RGBA32)) {
				fCamera->SetTestPicture(picture);
				command.AddString("source", "camera");
				fSourceGiven = true;
			} else {
				delete picture;
				fprintf(stderr, "Lookout: --test-source needs a picture, "
					"when the program is started\n");
			}
			i++;
		} else if (arg[0] != '-') {
			BPath path(cwd != NULL && arg[0] != '/' ? cwd : arg,
				cwd != NULL && arg[0] != '/' ? arg : NULL);
			entry_ref ref;
			if (get_ref_for_path(path.Path(), &ref) == B_OK)
				_Open(ref, false, &command);
		} else
			fprintf(stderr, "Lookout: unknown option %s\n", arg);
	}

	if (command.IsEmpty())
		return;

	// At the start the engine hears of the model and the device at once:
	// it has not begun to set up the ones of last time, and need not.
	if (!fStarted) {
		entry_ref model;
		bool hasModel = command.FindRef("model", &model) == B_OK;
		const char* device = command.GetString("device", NULL);
		if (device != NULL && strcmp(device, "auto") == 0)
			device = "";
		if (hasModel || device != NULL)
			fEngine->Configure(hasModel ? &model : NULL, device);
	}

	// after the pictures: a picture switches to "picture", and a source
	// that was asked for has the last word
	fWindowMessenger.SendMessage(&command);
}


void
LookoutApp::RefsReceived(BMessage* message)
{
	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++)
		_Open(ref, false);
}


void
LookoutApp::ReadyToRun()
{
	// as it was left, unless the command line said otherwise
	const char* picture;
	entry_ref ref;
	if (!fPictureGiven && fSettings.FindString("picture", &picture) == B_OK
		&& get_ref_for_path(picture, &ref) == B_OK && BEntry(&ref).Exists()) {
		_Open(ref, true);
	}

	if (!fPictureGiven && !fSourceGiven) {
		BMessage command(MSG_COMMAND);
		command.AddString("source", fSettings.GetString("source", "picture"));
		fWindowMessenger.SendMessage(&command);
	}

	fStarted = true;
	fEngine->Run();
	fCamera->Run();
	fWindow->Show();
}


bool
LookoutApp::QuitRequested()
{
	if (!BApplication::QuitRequested())
		return false;

	// The window is gone. The camera first, which feeds the engine: it
	// lets go of the camera (disconnects, releases the nodes). Then the
	// engine, which waits for a run that is still going on before it
	// deletes the session. Both do that in their own threads and before
	// they are quit: Shutdown() returns when they are through.
	if (fCamera != NULL) {
		if (fStarted)
			fCamera->Shutdown();
		if (fCamera->Lock())
			fCamera->Quit();
		fCamera = NULL;
	}
	if (fEngine != NULL) {
		if (fStarted)
			fEngine->Shutdown();
		if (fEngine->Lock())
			fEngine->Quit();
		fEngine = NULL;
	}

	return true;
}


/*!	A file the user gave: a model, or a picture. Pictures are read here,
	in the application's thread, and handed to the window as a snapshot.
	A model goes into \a command if there is one, else to the window. */
void
LookoutApp::_Open(const entry_ref& ref, bool restore, BMessage* command)
{
	BEntry entry(&ref, true);
	entry_ref target;
	if (entry.InitCheck() != B_OK || entry.GetRef(&target) != B_OK)
		target = ref;

	if (BNeuralModel::IsModel(&target)) {
		if (command != NULL) {
			// goes with the rest of the command line
			command->RemoveName("model");
			command->AddRef("model", &ref);
			return;
		}
		BMessage message(MSG_SET_MODEL);
		message.AddRef("ref", &ref);
		fWindowMessenger.SendMessage(&message);
		return;
	}

	BBitmap* bitmap = BTranslationUtils::GetBitmap(&target);
	if (bitmap == NULL || bitmap->InitCheck() != B_OK) {
		delete bitmap;
		if (restore)
			return;
		BMessage message(MSG_PICTURE_FAILED);
		message.AddRef("ref", &ref);
		fWindowMessenger.SendMessage(&message);
		return;
	}

	if (!restore)
		fPictureGiven = true;

	// the reference travels with the message
	Snapshot* frame = new Snapshot(bitmap, 0);
	BMessage message(MSG_SET_PICTURE);
	message.AddPointer("frame", frame);
	message.AddRef("ref", &ref);
	message.AddBool("restore", restore);
	if (fWindowMessenger.SendMessage(&message) != B_OK)
		frame->ReleaseReference();
}


int
main(int argc, char** argv)
{
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			fputs(kUsage, stdout);
			return 0;
		}
	}

	LookoutApp app;
	app.Run();
	return 0;
}
