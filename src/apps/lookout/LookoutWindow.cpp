/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "LookoutWindow.h"

#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <Application.h>
#include <Catalog.h>
#include <Directory.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <NodeInfo.h>
#include <Path.h>
#include <PathFinder.h>
#include <Screen.h>
#include <SeparatorView.h>
#include <StringView.h>

#include "PictureView.h"
#include "ResultsView.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "LookoutWindow"


enum {
	kMsgOpenPicture		= 'LwOp',
	kMsgOpenModel		= 'LwOm',
	kMsgSource			= 'LwSr',
	kMsgCameraInput		= 'LwCi',
	kMsgDevice			= 'LwDv',
	kMsgModel			= 'LwMs',
	kMsgContinuous		= 'LwCt'
};

static const char* kSettingsFile = "Lookout_settings";
static const char* kModelType = "application/x-tflite";
static const char* kSeparator = "  \xC2\xB7  ";
static const bigtime_t kLogInterval = 1000000;


//!	Pictures, for the "Open picture" panel.
class PictureFilter : public BRefFilter {
public:
	virtual bool Filter(const entry_ref* ref, BNode* node,
		struct stat_beos* stat, const char* mimeType)
	{
		if (node->IsDirectory() || node->IsSymLink())
			return true;
		return strncmp(mimeType, "image/", 6) == 0;
	}
};


//!	Models, for the "Open model" panel.
class ModelFilter : public BRefFilter {
public:
	virtual bool Filter(const entry_ref* ref, BNode* node,
		struct stat_beos* stat, const char* mimeType)
	{
		if (node->IsDirectory() || node->IsSymLink())
			return true;
		if (strcmp(mimeType, kModelType) == 0)
			return true;
		// a file nobody has told the type of yet
		return BString(ref->name).IEndsWith(".tflite");
	}
};


static BString
model_label(const char* path)
{
	BString label(BPath(path).Leaf());
	if (label.IEndsWith(".tflite"))
		label.Truncate(label.Length() - 7);
	return label;
}


// #pragma mark - LookoutWindow


LookoutWindow::LookoutWindow(Engine* engine, Camera* camera,
	const BMessage& settings)
	:
	BWindow(BRect(0, 0, 1, 1), B_TRANSLATE_SYSTEM_NAME("Lookout"),
		B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS
			| B_QUIT_ON_WINDOW_CLOSE),
	fEngine(engine),
	fCamera(camera),
	fPicturePanel(NULL),
	fModelPanel(NULL),
	fPictureFilter(NULL),
	fModelFilter(NULL),
	fSource(SOURCE_PICTURE),
	fSourceNumber(1),
	fCameraState(CAMERA_OFF),
	fCameraShown(false),
	fHasResult(false),
	fContinuous(false),
	fVerbose(false),
	fLastLog(0),
	fLoggedFrames(0)
{
	BMenuBar* menuBar = new BMenuBar("menu bar");

	BMenu* menu = new BMenu(B_TRANSLATE("File"));
	menu->AddItem(new BMenuItem(B_TRANSLATE("Open picture" B_UTF8_ELLIPSIS),
		new BMessage(kMsgOpenPicture), 'O'));
	menu->AddItem(new BMenuItem(B_TRANSLATE("Open model" B_UTF8_ELLIPSIS),
		new BMessage(kMsgOpenModel), 'M'));
	menu->AddSeparatorItem();
	menu->AddItem(new BMenuItem(B_TRANSLATE("Quit"),
		new BMessage(B_QUIT_REQUESTED), 'Q'));
	menuBar->AddItem(menu);

	fSourceMenu = new BMenu(B_TRANSLATE("Source"));
	menuBar->AddItem(fSourceMenu);
	fDeviceMenu = new BMenu(B_TRANSLATE("Device"));
	menuBar->AddItem(fDeviceMenu);
	fModelMenu = new BMenu(B_TRANSLATE("Model"));
	menuBar->AddItem(fModelMenu);

	fContinuousItem = new BMenuItem(B_TRANSLATE("Run continuously"),
		new BMessage(kMsgContinuous), 'R');

	fPictureView = new PictureView();
	fResultsView = new ResultsView();

	fStatusView = new BStringView("status", "");
	fStatusView->SetTruncation(B_TRUNCATE_END);
	// the text is long and changes all the time: it is not to push the
	// window's sides around
	fStatusView->SetExplicitMinSize(BSize(0, B_SIZE_UNSET));
	fStatusView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.Add(fPictureView, 10)
		.Add(new BSeparatorView(B_HORIZONTAL, B_PLAIN_BORDER))
		.Add(fResultsView, 0)
		.Add(new BSeparatorView(B_HORIZONTAL, B_PLAIN_BORDER))
		.AddGroup(B_HORIZONTAL, 0, 0)
			.SetInsets(B_USE_HALF_ITEM_INSETS, B_USE_SMALL_INSETS,
				B_USE_HALF_ITEM_INSETS, B_USE_SMALL_INSETS)
			.Add(fStatusView)
		.End();

	// what was chosen last time
	fDeviceChoice = settings.GetString("device", "");
	fCameraChoice = settings.GetString("camera", "");
	fEngine->SetSource(fSourceNumber);

	BEntry model(settings.GetString("model", ""), true);
	if (model.InitCheck() != B_OK || !model.IsFile()
		|| model.GetRef(&fModelRef) != B_OK) {
		_ChooseDefaultModel();
	}
	fEngine->Configure(fModelRef.name != NULL ? &fModelRef : NULL,
		fDeviceChoice);

	_BuildSourceMenu();
	_BuildDeviceMenu();
	_BuildModelMenu();
	_UpdateNote();
	_UpdateStatus();
	_UpdateTitle();

	// as large as the layout likes and the screen allows, or as left
	BRect screen = BScreen(this).Frame();
	BRect frame;
	if (settings.FindRect("frame", &frame) == B_OK && frame.IsValid()) {
		MoveTo(frame.LeftTop());
		ResizeTo(frame.Width(), frame.Height());
		MoveOnScreen();
	} else {
		// (a column's preferred height is its minimum one: the room the
		// picture would like is added by hand)
		BSize size = GetLayout()->PreferredSize();
		size.height += fPictureView->PreferredSize().height
			- fPictureView->MinSize().height;
		ResizeTo(std::min(size.width, floorf(screen.Width() * 0.8f)),
			std::min(size.height, floorf(screen.Height() * 0.8f)));
		CenterOnScreen();
	}
}


LookoutWindow::~LookoutWindow()
{
	delete fPicturePanel;
	delete fModelPanel;
	delete fPictureFilter;
	delete fModelFilter;

	// in no menu at the moment?
	if (fContinuousItem->Menu() == NULL)
		delete fContinuousItem;
}


void
LookoutWindow::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgOpenPicture:
			if (fPicturePanel == NULL) {
				fPictureFilter = new PictureFilter();
				fPicturePanel = new BFilePanel(B_OPEN_PANEL, NULL, NULL,
					B_FILE_NODE, false, NULL, fPictureFilter);
				fPicturePanel->Window()->SetTitle(
					B_TRANSLATE("Open picture"));
			}
			fPicturePanel->Show();
			break;

		case kMsgOpenModel:
			if (fModelPanel == NULL) {
				fModelFilter = new ModelFilter();
				fModelPanel = new BFilePanel(B_OPEN_PANEL, NULL, NULL,
					B_FILE_NODE, false, NULL, fModelFilter);
				fModelPanel->Window()->SetTitle(B_TRANSLATE("Open model"));
			}
			fModelPanel->Show();
			break;

		case B_SIMPLE_DATA:
		case B_REFS_RECEIVED:
		{
			// Dropped files: the application finds out what they are and
			// reads pictures in its own thread.
			if (!message->HasRef("refs"))
				break;
			BMessage refs(*message);
			refs.what = B_REFS_RECEIVED;
			be_app->PostMessage(&refs);
			break;
		}

		case MSG_SET_PICTURE:
			_SetPicture(message);
			break;

		case MSG_PICTURE_FAILED:
		{
			entry_ref ref;
			if (message->FindRef("ref", &ref) != B_OK)
				break;
			BString error(B_TRANSLATE("\"%name%\" is neither a picture nor "
				"a model"));
			error.ReplaceFirst("%name%", ref.name);
			_Log("%s", error.String());

			// said where the picture would be; a picture that is on show
			// stays
			if (!fPicture.IsSet()) {
				fPictureError = error;
				_UpdateNote();
			}
			break;
		}

		case MSG_SET_MODEL:
		case kMsgModel:
		{
			entry_ref ref;
			if (message->FindRef("ref", &ref) == B_OK)
				_Configure(&ref, message->what == MSG_SET_MODEL, NULL);
			break;
		}

		case MSG_COMMAND:
			_HandleCommand(message);
			break;

		case kMsgSource:
			_SetSource(message->GetInt32("source", SOURCE_PICTURE));
			break;

		case kMsgCameraInput:
			fCameraChoice = message->GetString("name", "");
			_SetSource(SOURCE_CAMERA, true);
			break;

		case kMsgDevice:
			_Configure(NULL, false, message->GetString("device", ""));
			break;

		case kMsgContinuous:
			_SetContinuous(!fContinuous);
			break;

		case MSG_ENGINE_DEVICES:
			fDevices = *message;
			_BuildDeviceMenu();
			break;

		case MSG_ENGINE_STATE:
			_EngineStateChanged(message);
			break;

		case MSG_ENGINE_RESULT:
			_ResultArrived(message);
			break;

		case MSG_CAMERA_INPUTS:
			fInputs.MakeEmpty();
			message->FindStrings("name", &fInputs);
			fDefaultInput = message->GetString("default", "");
			_BuildSourceMenu();
			break;

		case MSG_CAMERA_STATE:
			_CameraStateChanged(message);
			break;

		case MSG_CAMERA_FRAME:
			_CameraFrameArrived();
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


void
LookoutWindow::MenusBeginning()
{
	// models may have been installed since the last look
	_BuildModelMenu();
}


bool
LookoutWindow::QuitRequested()
{
	_SaveSettings();
	return true;
}


/*static*/ status_t
LookoutWindow::LoadSettings(BMessage& settings)
{
	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status != B_OK)
		return status;
	path.Append(kSettingsFile);

	BFile file(path.Path(), B_READ_ONLY);
	status = file.InitCheck();
	if (status != B_OK)
		return status;
	return settings.Unflatten(&file);
}


status_t
LookoutWindow::_SaveSettings() const
{
	BMessage settings('Lkot');
	settings.AddRect("frame", Frame());
	settings.AddString("device", fDeviceChoice);
	settings.AddString("source",
		fSource == SOURCE_CAMERA ? "camera" : "picture");
	settings.AddString("camera", fCameraChoice);
	if (fModelRef.name != NULL)
		settings.AddString("model", BPath(&fModelRef).Path());
	if (fPicture.IsSet())
		settings.AddString("picture", BPath(&fPictureRef).Path());

	BPath path;
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path, true);
	if (status != B_OK)
		return status;
	path.Append(kSettingsFile);

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = file.InitCheck();
	if (status != B_OK)
		return status;
	return settings.Flatten(&file);
}


// #pragma mark - what the user chooses


/*!	Switches between the picture and the camera. Every switch gets a new
	number, by which frames and results of the source before are told from
	the ones that count; they may still be on their way. */
void
LookoutWindow::_SetSource(int32 source, bool force)
{
	if (source == fSource && !force)
		return;

	fSource = source;
	fSourceNumber++;
	fEngine->SetSource(fSourceNumber);
	_ClearResult();

	if (source == SOURCE_CAMERA) {
		fPictureView->SetFrame(NULL);
		fCameraShown = false;
		fCameraState = CAMERA_STARTING;
		fCameraError = "";
		fCamera->Start(fCameraChoice, fSourceNumber);
	} else {
		// lets go of the camera: its light goes out
		fCamera->Stop();
		fCameraState = CAMERA_OFF;
		fPictureView->SetFrame(fPicture.Get());
		if (fPicture.IsSet()) {
			fPicture->SetSource(fSourceNumber);
			fEngine->SetFrame(fPicture.Get());
		}
	}

	_BuildSourceMenu();
	_UpdateNote();
	_UpdateStatus();
	_UpdateTitle();
	_Log("source: %s", source == SOURCE_CAMERA ? "camera" : "picture");
}


void
LookoutWindow::_SetPicture(BMessage* message)
{
	Snapshot* frame;
	if (message->FindPointer("frame", (void**)&frame) != B_OK)
		return;

	// the message's reference is ours now
	fPicture.SetTo(frame, true);
	fPictureError = "";
	message->FindRef("ref", &fPictureRef);

	// The picture of last time comes back quietly behind a camera that
	// was on last time; a picture the user opens is what he wants to see.
	if (message->GetBool("restore", false) && fSource == SOURCE_CAMERA)
		return;

	_SetSource(SOURCE_PICTURE, true);
}


/*!	Another model (\a byHand: one the user opened himself, which stays
	in the menu), another device, or both; NULL for what stays. */
void
LookoutWindow::_Configure(const entry_ref* model, bool byHand,
	const char* device)
{
	if (model != NULL) {
		fModelRef = *model;
		if (byHand)
			fOpenedModel = *model;
	}
	if (device != NULL)
		fDeviceChoice = device;

	fEngine->Configure(model, device);

	if (model != NULL)
		_BuildModelMenu();
	if (device != NULL)
		_BuildDeviceMenu();
	_UpdateNote();
}


void
LookoutWindow::_SetContinuous(bool continuous)
{
	fContinuous = continuous;
	fContinuousItem->SetMarked(continuous);
	fEngine->SetContinuous(continuous);
	_UpdateStatus();
}


void
LookoutWindow::_HandleCommand(BMessage* message)
{
	if (message->HasBool("verbose"))
		fVerbose = message->GetBool("verbose", false);

	const char* device = NULL;
	if (message->FindString("device", &device) == B_OK
		&& strcmp(device, "auto") == 0) {
		device = "";
	}
	entry_ref model;
	bool hasModel = message->FindRef("model", &model) == B_OK;
	if (hasModel || device != NULL)
		_Configure(hasModel ? &model : NULL, true, device);

	const char* source;
	if (message->FindString("source", &source) == B_OK)
		_SetSource(strcmp(source, "camera") == 0
			? SOURCE_CAMERA : SOURCE_PICTURE);

	if (message->HasBool("continuous"))
		_SetContinuous(message->GetBool("continuous", false));
}


// #pragma mark - what the engine and the camera say


void
LookoutWindow::_EngineStateChanged(BMessage* message)
{
	fEngineState = *message;

	// what was found with the model before says nothing about this one
	_ClearResult();
	_UpdateNote();
	_UpdateStatus();
	_Log("engine: %s", _StatusText().String());
}


void
LookoutWindow::_ResultArrived(BMessage* message)
{
	// of a picture that is not on show any more?
	if (message->GetInt32("source", -1) != fSourceNumber)
		return;

	fResult = *message;
	fHasResult = true;
	fPictureView->SetResult(message);
	fResultsView->SetResult(message);
	_UpdateStatus();

	if (!fVerbose)
		return;

	// every result of a picture, one per second of a stream of them
	bigtime_t now = system_time();
	bool stream = _IsStream() && message->GetFloat("rate", 0) > 0;
	if (stream && now - fLastLog < kLogInterval)
		return;

	BString found;
	const char* label;
	for (int32 i = 0; message->FindString("label", i, &label) == B_OK; i++) {
		BString one;
		one.SetToFormat("%s%s %.1f%%", i > 0 ? ", " : "", label,
			message->GetFloat("score", i, 0) * 100);
		BRect box;
		if (message->FindRect("box", i, &box) == B_OK) {
			one << BString().SetToFormat(" at (%.0f, %.0f) - (%.0f, %.0f)",
				box.left, box.top, box.right, box.bottom);
		}
		found << one;
	}

	if (fSource == SOURCE_CAMERA) {
		int32 frames = fCamera->CountFrames();
		float seconds = (now - fLastLog) / 1e6f;
		_Log("result: %s | camera %.1f frames per second | %s",
			_StatusText().String(), fLastLog > 0 && frames >= fLoggedFrames
				? (frames - fLoggedFrames) / seconds : 0.0f,
			found.String());
		fLoggedFrames = frames;
	} else
		_Log("result: %s | %s", _StatusText().String(), found.String());
	fLastLog = now;
}


void
LookoutWindow::_CameraStateChanged(BMessage* message)
{
	// The camera looper is behind the window by what it has to do; what it
	// says while the picture is on show is about the past.
	if (fSource != SOURCE_CAMERA)
		return;

	fCameraState = (camera_state)message->GetInt32("state", CAMERA_OFF);
	fCameraError = message->GetString("error", "");
	fCameraName = message->GetString("name", "");

	if (fCameraState != CAMERA_RUNNING) {
		fPictureView->SetFrame(NULL);
		fCameraShown = false;
		_ClearResult();
	}

	_UpdateNote();
	_UpdateStatus();
	_UpdateTitle();

	static const char* const kStates[] = { "off", "starting", "running",
		"no camera", "failed" };
	_Log("camera: %s %s%s %" B_PRId32 " x %" B_PRId32,
		kStates[fCameraState], fCameraName.String(), fCameraError.String(),
		message->GetInt32("width", 0), message->GetInt32("height", 0));
}


void
LookoutWindow::_CameraFrameArrived()
{
	SnapshotReference frame = fCamera->DisplaySlot()->Take();
	if (!frame.IsSet() || fSource != SOURCE_CAMERA
		|| frame->Source() != fSourceNumber) {
		return;
	}

	fPictureView->SetFrame(frame.Get());
	if (!fCameraShown) {
		fCameraShown = true;
		_UpdateNote();
	}
}


void
LookoutWindow::_ClearResult()
{
	fHasResult = false;
	fResult.MakeEmpty();
	fPictureView->SetResult(NULL);
	fResultsView->SetResult(NULL);
}


// #pragma mark - menus


void
LookoutWindow::_BuildSourceMenu()
{
	fSourceMenu->RemoveItems(0, fSourceMenu->CountItems(), true);

	BMessage* message = new BMessage(kMsgSource);
	message->AddInt32("source", SOURCE_PICTURE);
	BMenuItem* item = new BMenuItem(B_TRANSLATE("Picture"), message, '1');
	item->SetMarked(fSource == SOURCE_PICTURE);
	fSourceMenu->AddItem(item);

	message = new BMessage(kMsgSource);
	message->AddInt32("source", SOURCE_CAMERA);
	item = new BMenuItem(B_TRANSLATE("Camera"), message, '2');
	item->SetMarked(fSource == SOURCE_CAMERA);
	fSourceMenu->AddItem(item);

	// which camera, when there is a choice
	if (fInputs.CountStrings() < 2)
		return;

	BString chosen = fCameraChoice;
	if (chosen.IsEmpty() || !fInputs.HasString(chosen))
		chosen = fDefaultInput;

	fSourceMenu->AddSeparatorItem();
	for (int32 i = 0; i < fInputs.CountStrings(); i++) {
		message = new BMessage(kMsgCameraInput);
		message->AddString("name", fInputs.StringAt(i));
		item = new BMenuItem(fInputs.StringAt(i), message);
		item->SetMarked(fInputs.StringAt(i) == chosen);
		fSourceMenu->AddItem(item);
	}
}


void
LookoutWindow::_BuildDeviceMenu()
{
	fDeviceMenu->RemoveItems(0, fDeviceMenu->CountItems(), true);

	BMessage* message = new BMessage(kMsgDevice);
	message->AddString("device", "");
	BMenuItem* item = new BMenuItem(B_TRANSLATE("Automatic"), message, 'D');
	item->SetMarked(fDeviceChoice.IsEmpty());
	fDeviceMenu->AddItem(item);
	fDeviceMenu->AddSeparatorItem();

	bool found = fDeviceChoice.IsEmpty();
	const char* id;
	for (int32 i = 0; fDevices.FindString("device", i, &id) == B_OK; i++) {
		// "NVIDIA GeForce GTX 1070 (vulkan/0)"; "CPU" needs no "(cpu)"
		BString label(fDevices.GetString("name", i, id));
		if (label.ICompare(id) != 0)
			label << " (" << id << ")";

		message = new BMessage(kMsgDevice);
		message->AddString("device", id);
		item = new BMenuItem(label, message);
		item->SetMarked(fDeviceChoice == id);
		fDeviceMenu->AddItem(item);
		found |= fDeviceChoice == id;
	}

	if (!found) {
		// chosen once, and not there today
		BString label(B_TRANSLATE("%device% (not available)"));
		label.ReplaceFirst("%device%", fDeviceChoice);
		item = new BMenuItem(label, NULL);
		item->SetMarked(true);
		item->SetEnabled(false);
		fDeviceMenu->AddItem(item);
	}
}


/*!	The models in neural/models of the data directories, the user's
	before the system's, non-packaged before packaged. A name found twice
	is listed where it was found first. */
void
LookoutWindow::_FindModels(BStringList& paths) const
{
	BStringList directories;
	BPathFinder::FindPaths(B_FIND_PATH_DATA_DIRECTORY, "neural/models",
		B_FIND_PATH_EXISTING_ONLY, directories);

	BStringList names;
	for (int32 i = 0; i < directories.CountStrings(); i++) {
		BDirectory directory(directories.StringAt(i));
		BStringList found;
		entry_ref ref;
		while (directory.GetNextRef(&ref) == B_OK) {
			BEntry entry(&ref, true);
			entry_ref target;
			if (!entry.IsFile() || entry.GetRef(&target) != B_OK
				|| names.HasString(ref.name)
				|| !BNeuralModel::IsModel(&target)) {
				continue;
			}
			names.Add(ref.name);
			found.Add(BPath(&ref).Path());
		}
		found.Sort();
		paths.Add(found);
	}
}


void
LookoutWindow::_BuildModelMenu()
{
	if (fContinuousItem->Menu() != NULL)
		fModelMenu->RemoveItem(fContinuousItem);
	fModelMenu->RemoveItems(0, fModelMenu->CountItems(), true);

	BStringList paths;
	_FindModels(paths);

	// and the ones that are not in the usual places: opened by hand, or
	// the one of last time
	BString current;
	if (fModelRef.name != NULL)
		current = BPath(&fModelRef).Path();
	if (fOpenedModel.name != NULL) {
		BString opened(BPath(&fOpenedModel).Path());
		if (!paths.HasString(opened))
			paths.Add(opened);
	}
	if (!current.IsEmpty() && !paths.HasString(current))
		paths.Add(current);

	for (int32 i = 0; i < paths.CountStrings(); i++) {
		entry_ref ref;
		if (get_ref_for_path(paths.StringAt(i), &ref) != B_OK)
			continue;
		BMessage* message = new BMessage(kMsgModel);
		message->AddRef("ref", &ref);
		BMenuItem* item = new BMenuItem(model_label(paths.StringAt(i)),
			message);
		item->SetMarked(paths.StringAt(i) == current);
		fModelMenu->AddItem(item);
	}

	if (paths.IsEmpty()) {
		BMenuItem* item = new BMenuItem(B_TRANSLATE("No models installed"),
			NULL);
		item->SetEnabled(false);
		fModelMenu->AddItem(item);
	}

	fModelMenu->AddSeparatorItem();
	fContinuousItem->SetMarked(fContinuous);
	fModelMenu->AddItem(fContinuousItem);
}


/*!	For the first start: a detection model if one is installed (boxes on
	a picture are the better show), else any model. */
bool
LookoutWindow::_ChooseDefaultModel()
{
	BStringList paths;
	_FindModels(paths);

	entry_ref chosen;
	for (int32 i = 0; i < paths.CountStrings(); i++) {
		entry_ref ref;
		if (get_ref_for_path(paths.StringAt(i), &ref) != B_OK)
			continue;
		if (chosen.name == NULL)
			chosen = ref;

		BNeuralModel model(&ref);
		if (model.InitCheck() == B_OK && Engine::IsDetectionModel(model)) {
			chosen = ref;
			break;
		}
	}

	if (chosen.name == NULL)
		return false;

	fModelRef = chosen;
	return true;
}


// #pragma mark - what is shown


//!	What the middle of the picture says when there is something to say.
void
LookoutWindow::_UpdateNote()
{
	int32 state = fEngineState.GetInt32("state", ENGINE_IDLE);
	BString note;

	if (fSource == SOURCE_CAMERA && fCameraState == CAMERA_MISSING)
		note = B_TRANSLATE("No camera found");
	else if (fSource == SOURCE_CAMERA && fCameraState == CAMERA_FAILED)
		note = fCameraError;
	else if (fSource == SOURCE_CAMERA && fCameraState == CAMERA_STARTING)
		note = B_TRANSLATE("Starting the camera" B_UTF8_ELLIPSIS);
	else if (fSource == SOURCE_PICTURE && !fPicture.IsSet()) {
		if (!fPictureError.IsEmpty())
			note = fPictureError;
		else {
			note = B_TRANSLATE("Drop a picture here, or choose one with "
				"\"Open picture" B_UTF8_ELLIPSIS "\"");
		}
	} else if (fModelRef.name == NULL) {
		note = B_TRANSLATE("No model: choose one with \"Open model"
			B_UTF8_ELLIPSIS "\"");
	} else if (state == ENGINE_LOADING || state == ENGINE_IDLE)
		note = B_TRANSLATE("Loading model" B_UTF8_ELLIPSIS);
	else if (state == ENGINE_FAILED)
		note = fEngineState.GetString("error", "");
	else if (fSource == SOURCE_CAMERA && !fCameraShown)
		note = B_TRANSLATE("Waiting for the camera" B_UTF8_ELLIPSIS);

	fPictureView->SetNote(note);
}


//!	Whether runs follow each other, so that their rate means something.
bool
LookoutWindow::_IsStream() const
{
	return fSource == SOURCE_CAMERA || fContinuous;
}


/*!	The status line: the device, the time of a run, runs per second and
	how much of the model the device runs itself. It says what happens,
	not what was asked for: a device that runs none of the model is named
	as that, and the CPU as what does the work. */
BString
LookoutWindow::_StatusText() const
{
	int32 state = fEngineState.GetInt32("state", ENGINE_IDLE);
	BString modelName;
	if (fModelRef.name != NULL)
		modelName = model_label(fModelRef.name);

	if (fModelRef.name == NULL)
		return B_TRANSLATE("No model");

	if (state == ENGINE_FAILED) {
		BString text(B_TRANSLATE("%model%: %error%"));
		text.ReplaceFirst("%model%", modelName);
		text.ReplaceFirst("%error%", fEngineState.GetString("error", ""));
		return text;
	}

	if (state != ENGINE_READY) {
		BString text(B_TRANSLATE("Loading %model%" B_UTF8_ELLIPSIS));
		text.ReplaceFirst("%model%", modelName);
		return text;
	}

	// operators as of the last run, if there was one: a device that
	// failed since has handed its part to the CPU
	const BMessage& latest = fHasResult ? fResult : fEngineState;
	int32 onDevice = latest.GetInt32("on device", 0);
	int32 onCPU = latest.GetInt32("on cpu", 0);
	bool isCPU = fEngineState.GetInt32("device kind", B_NEURAL_DEVICE_CPU)
		== B_NEURAL_DEVICE_CPU;

	BString text;
	if (isCPU)
		text = fEngineState.GetString("device name", "CPU");
	else if (onDevice == 0) {
		text = B_TRANSLATE("CPU (%device% runs none of this model)");
		text.ReplaceFirst("%device%",
			fEngineState.GetString("device name", ""));
	} else
		text = fEngineState.GetString("device name", "");

	if (fEngineState.GetBool("choice missing", false)) {
		BString missing(B_TRANSLATE("%device% is not available"));
		missing.ReplaceFirst("%device%",
			fEngineState.GetString("choice", ""));
		text << " (" << missing << ")";
	}

	if (fHasResult) {
		BString time;
		time.SetToFormat(B_TRANSLATE("%.1f ms per run"),
			fResult.GetInt64("duration", 0) / 1000.0);
		text << kSeparator << time;

		float rate = fResult.GetFloat("rate", 0);
		if (_IsStream() && rate > 0) {
			BString runs;
			runs.SetToFormat(B_TRANSLATE("%.1f runs per second"), rate);
			text << kSeparator << runs;
		}
	}

	BString operators;
	if (isCPU) {
		operators.SetToFormat(B_TRANSLATE("%" B_PRId32 " operators on the "
			"CPU"), onCPU + onDevice);
	} else {
		operators.SetToFormat(B_TRANSLATE("%" B_PRId32 " of %" B_PRId32
			" operators on the device"), onDevice, onDevice + onCPU);
	}
	text << kSeparator << operators;

	// a device that fails runs: the answers are right (the CPU gives
	// them), the times are not what the device would do
	int32 failures = latest.GetInt32("failures", 0);
	if (failures > 0) {
		BString failed;
		failed.SetToFormat(B_TRANSLATE("%" B_PRId32 " runs failed on the "
			"device and were done by the CPU"), failures);
		text << kSeparator << failed;
	}
	return text;
}


void
LookoutWindow::_UpdateStatus()
{
	BString text = _StatusText();
	if (text == fStatusView->Text())
		return;

	fStatusView->SetText(text);
	// for windows too narrow to show all of it
	fStatusView->SetToolTip(text);
}


void
LookoutWindow::_UpdateTitle()
{
	BString name;
	if (fSource == SOURCE_CAMERA)
		name = fCameraName;
	else if (fPicture.IsSet())
		name = fPictureRef.name;

	BString title(B_TRANSLATE_SYSTEM_NAME("Lookout"));
	if (!name.IsEmpty())
		title << ": " << name;
	if (title != Title())
		SetTitle(title);
}


//!	With --verbose: what happens, on the standard output.
void
LookoutWindow::_Log(const char* format, ...) const
{
	if (!fVerbose)
		return;

	va_list arguments;
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);
	putchar('\n');
	fflush(stdout);
}
