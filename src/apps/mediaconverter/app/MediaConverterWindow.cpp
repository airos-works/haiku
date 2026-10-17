/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "MediaConverterWindow.h"

#include <Alert.h>
#include <Application.h>
#include <Bitmap.h>
#include <Box.h>
#include <Button.h>
#include <Catalog.h>
#include <ControlLook.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <LayoutBuilder.h>
#include <Menu.h>
#include <MenuBar.h>
#include <MenuItem.h>
#include <Mime.h>
#include <NodeInfo.h>
#include <Notification.h>
#include <Path.h>
#include <Roster.h>
#include <ScrollView.h>
#include <SplitView.h>
#include <StatusBar.h>
#include <StringView.h>

#include <stdio.h>

#include "DiscSource.h"
#include "HardwareCaps.h"
#include "HardwareWindow.h"
#include "JobProcess.h"
#include "MediaConverterApp.h"
#include "MediaProbe.h"
#include "Messages.h"
#include "NvencEncoder.h"
#include "OutputView.h"
#include "Presets.h"
#include "QueueView.h"
#include "SourceView.h"
#include "Transcoder.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "MediaConverter"


static const char* kSettingsFile = "MediaConverter settings";


// #pragma mark - worker side


struct ProbeRequest {
	BMessenger			target;
	std::vector<std::pair<int32, BString> > files;
};


struct PreviewRequest {
	BMessenger			target;
	int32				id;
	BString				path;
	float				position;
	bool				cover;		// a song's artwork rather than a picture
};


struct ConvertRequest {
	BMessenger			target;
	int32				id;
	JobProcess*			process;
};


class WindowListener : public TranscodeListener {
public:
	WindowListener(const BMessenger& target, int32 id)
		:
		fTarget(target),
		fID(id)
	{
	}

	virtual void TranscodeStarted(const TranscodeReport& report)
	{
		BMessage message(kMsgJobStarted);
		message.AddInt32("id", fID);
		message.AddString("encoder", report.videoEncoder);
		message.AddString("settings", report.videoSettings);
		message.AddInt32("backend", report.backend);
		message.AddBool("fallback", report.cpuFallback);
		message.AddString("reason", report.fallbackReason);
		fTarget.SendMessage(&message);
	}

	virtual void TranscodeProgressed(const TranscodeProgress& progress)
	{
		BMessage message(kMsgJobProgress);
		message.AddInt32("id", fID);
		message.AddData("progress", B_RAW_TYPE, &progress, sizeof(progress));
		fTarget.SendMessage(&message);
	}

private:
	BMessenger	fTarget;
	int32		fID;
};


/*!	NVIDIA formats count once a test picture has come back through the
	card (see NvencEncoder::SelfTest); the test runs here rather than in
	the way of the first conversion, and the window is told what came of
	it. */
static status_t
test_nvidia_encoders(void* data)
{
	BMessenger* target = (BMessenger*)data;
	NvencEncoder::SelfTestAll();
	target->SendMessage(kMsgHardwareTested);
	delete target;
	return B_OK;
}


status_t
MediaConverterWindow::_ProbeThread(void* data)
{
	ProbeRequest* request = (ProbeRequest*)data;
	for (size_t i = 0; i < request->files.size(); i++) {
		SourceInfo* info = new SourceInfo;
		BString error;
		status_t status = MediaProbe::Probe(request->files[i].second.String(),
			*info, &error);
		BMessage message(kMsgSourceProbed);
		message.AddInt32("id", request->files[i].first);
		message.AddInt32("status", status);
		message.AddString("error", error);
		message.AddPointer("info", info);
		if (request->target.SendMessage(&message) != B_OK)
			delete info;
	}
	delete request;
	return B_OK;
}


status_t
MediaConverterWindow::_PreviewThread(void* data)
{
	PreviewRequest* request = (PreviewRequest*)data;
	BBitmap* bitmap = request->cover
		? MediaProbe::CoverArt(request->path.String(), 480, 480)
		: MediaProbe::Preview(request->path.String(), request->position, 640,
			360);
	BMessage message(kMsgPreviewReady);
	message.AddInt32("id", request->id);
	message.AddPointer("bitmap", bitmap);
	if (request->target.SendMessage(&message) != B_OK)
		delete bitmap;
	delete request;
	return B_OK;
}


status_t
MediaConverterWindow::_ConvertThread(void* data)
{
	ConvertRequest* request = (ConvertRequest*)data;
	WindowListener listener(request->target, request->id);
	BString error;
	status_t status = request->process->Run(&listener, error);
	BMessage message(kMsgJobFinished);
	message.AddInt32("id", request->id);
	message.AddInt32("status", status);
	message.AddString("error", error);
	const TranscodeReport& report = request->process->Report();
	message.AddString("encoder", report.videoEncoder);
	message.AddString("notes", report.notes);
	message.AddBool("fallback", report.cpuFallback);
	message.AddBool("retried", request->process->RetriedOnCpu());
	request->target.SendMessage(&message);
	delete request;
	return B_OK;
}


// #pragma mark - MediaConverterWindow


MediaConverterWindow::MediaConverterWindow()
	:
	BWindow(BRect(80, 80, 1180, 820), B_TRANSLATE_SYSTEM_NAME("MediaConverter"),
		B_DOCUMENT_WINDOW, B_AUTO_UPDATE_SIZE_LIMITS
			| B_ASYNCHRONOUS_CONTROLS),
	fOpenPanel(NULL),
	fFolderPanel(NULL),
	fHardwareWindow(NULL),
	fConverting(false),
	fStopping(false),
	fWarnCpu(true),
	fProcess(NULL),
	fThread(-1),
	fCurrentID(-1),
	fJobsDone(0),
	fJobsTotal(0),
	fConvertWhenReady(false)
{
	// the inventory first: the output view's menus ask it
	HardwareInventory::Default().Probe();

	BMenuBar* menuBar = new BMenuBar("menu");
	BMenu* fileMenu = new BMenu(B_TRANSLATE("File"));
	fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Add files" B_UTF8_ELLIPSIS),
		new BMessage(kMsgAddFiles), 'O'));
	fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Remove from the list"),
		new BMessage(kMsgRemoveFiles)));
	fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Clear finished"),
		new BMessage(kMsgClearFinished)));
	fileMenu->AddSeparatorItem();
	fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Convert"),
		new BMessage(kMsgConvert), 'R'));
	fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Stop"),
		new BMessage(kMsgStop), '.'));
	fileMenu->AddSeparatorItem();
	BMenuItem* about = new BMenuItem(B_TRANSLATE("About MediaConverter"),
		new BMessage(B_ABOUT_REQUESTED));
	about->SetTarget(be_app);
	fileMenu->AddItem(about);
	fileMenu->AddItem(new BMenuItem(B_TRANSLATE("Quit"),
		new BMessage(B_QUIT_REQUESTED), 'Q'));
	menuBar->AddItem(fileMenu);

	BMenu* settingsMenu = new BMenu(B_TRANSLATE("Settings"));
	fWarnItem = new BMenuItem(B_TRANSLATE("Warn before encoding on the CPU"),
		new BMessage(kMsgToggleCpuWarning));
	settingsMenu->AddItem(fWarnItem);
	settingsMenu->AddSeparatorItem();
	settingsMenu->AddItem(new BMenuItem(
		B_TRANSLATE("Video hardware" B_UTF8_ELLIPSIS),
		new BMessage(kMsgShowHardware), 'H'));
	menuBar->AddItem(settingsMenu);

	fQueue = new QueueView();
	fQueue->SetSelectionMessage(new BMessage(kMsgQueueSelection));
	fQueue->SetInvocationMessage(new BMessage(kMsgQueueInvoked));
	BScrollView* queueScroll = new BScrollView("queue scroll", fQueue, 0,
		false, true, B_FANCY_BORDER);
	queueScroll->SetExplicitMinSize(BSize(
		be_control_look->DefaultLabelSpacing() * 48, B_SIZE_UNSET));
	queueScroll->SetExplicitMaxSize(BSize(
		be_control_look->DefaultLabelSpacing() * 90, B_SIZE_UNLIMITED));
	fAddButton = new BButton("add", "+", new BMessage(kMsgAddFiles));
	fRemoveButton = new BButton("remove", "−", new BMessage(kMsgRemoveFiles));
	float buttonSize = be_control_look->DefaultLabelSpacing() * 6;
	fAddButton->SetExplicitSize(BSize(buttonSize, B_SIZE_UNSET));
	fRemoveButton->SetExplicitSize(BSize(buttonSize, B_SIZE_UNSET));
	fAddButton->SetToolTip(B_TRANSLATE("Add files"));
	fRemoveButton->SetToolTip(B_TRANSLATE("Remove the selected files"));

	fSource = new SourceView();
	fOutput = new OutputView();

	fProgress = new BStatusBar("progress");
	fProgress->SetBarHeight(be_control_look->DefaultLabelSpacing() * 3);
	fStatus = new BStringView("status", "");
	fStatus->SetExplicitMinSize(BSize(50, B_SIZE_UNSET));
	fStatus->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
	fStopButton = new BButton("stop", B_TRANSLATE("Stop"),
		new BMessage(kMsgStop));
	fConvertButton = new BButton("convert", B_TRANSLATE("Convert"),
		new BMessage(kMsgConvert));

	BBox* sourceBox = new BBox("source box");
	sourceBox->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	sourceBox->SetLabel(B_TRANSLATE("Source"));
	BLayoutBuilder::Group<>(sourceBox, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_BIG_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fSource);
	BBox* outputBox = new BBox("output box");
	outputBox->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
	outputBox->SetLabel(B_TRANSLATE("Output"));
	BLayoutBuilder::Group<>(outputBox, B_VERTICAL)
		.SetInsets(B_USE_DEFAULT_SPACING, B_USE_BIG_SPACING,
			B_USE_DEFAULT_SPACING, B_USE_DEFAULT_SPACING)
		.Add(fOutput);

	BSplitView* split = new BSplitView(B_HORIZONTAL, B_USE_SMALL_SPACING);
	BLayoutBuilder::Split<>(split)
		.AddGroup(B_VERTICAL, B_USE_SMALL_SPACING, 1)
			.Add(queueScroll, 1)
			.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
				.Add(fAddButton)
				.Add(fRemoveButton)
				.AddGlue()
			.End()
		.End()
		.AddGroup(B_VERTICAL, B_USE_DEFAULT_SPACING, 3)
			.Add(sourceBox, 2)
			.Add(outputBox, 3)
		.End();
	split->SetCollapsible(false);

	BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
		.Add(menuBar)
		.AddGroup(B_VERTICAL, B_USE_DEFAULT_SPACING)
			.SetInsets(B_USE_WINDOW_INSETS)
			.Add(split, 1)
			.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
				.AddGroup(B_VERTICAL, 0, 1)
					.Add(fStatus)
					.Add(fProgress)
				.End()
				.Add(fStopButton)
				.Add(fConvertButton)
			.End()
		.End();

	SetDefaultButton(fConvertButton);
	_LoadSettings();
	fWarnItem->SetMarked(fWarnCpu);
	_SelectionChanged();
	_UpdateControls();
	_UpdateStatus();

	HardwareInventory& inventory = HardwareInventory::Default();
	for (int32 i = 0; i < inventory.CountDevices(); i++) {
		const GpuDevice& device = inventory.DeviceAt(i);
		if (device.backend != kBackendNVENC || !device.ready)
			continue;
		BMessenger* target = new BMessenger(this);
		thread_id thread = spawn_thread(test_nvidia_encoders,
			"test NVIDIA encoders", B_LOW_PRIORITY, target);
		if (thread < 0 || resume_thread(thread) != B_OK)
			delete target;
		break;
	}
}


MediaConverterWindow::~MediaConverterWindow()
{
	delete fOpenPanel;
	delete fFolderPanel;
}


bool
MediaConverterWindow::QuitRequested()
{
	if (fConverting) {
		BAlert* alert = new BAlert(B_TRANSLATE("Quit"),
			B_TRANSLATE("A conversion is running. Stop it and quit?"),
			B_TRANSLATE("Keep converting"), B_TRANSLATE("Stop and quit"),
			NULL, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		if (alert->Go() == 0)
			return false;
		fStopping = true;
		if (fProcess != NULL)
			fProcess->Cancel();
		if (fThread >= 0) {
			status_t result;
			wait_for_thread(fThread, &result);
		}
		delete fProcess;
		fProcess = NULL;
	}
	_SaveSettings();
	if (fHardwareWindow != NULL && fHardwareWindow->Lock())
		fHardwareWindow->Quit();
	be_app->PostMessage(B_QUIT_REQUESTED);
	return true;
}


void
MediaConverterWindow::MessageReceived(BMessage* message)
{
	if (message->WasDropped() && message->HasRef("refs")) {
		_AddRefs(message);
		return;
	}

	switch (message->what) {
		case B_REFS_RECEIVED:
			_AddRefs(message);
			break;

		case kMsgAddFiles:
			if (fOpenPanel == NULL) {
				fOpenPanel = new BFilePanel(B_OPEN_PANEL, new BMessenger(this),
					NULL, B_FILE_NODE | B_DIRECTORY_NODE, true);
				fOpenPanel->Window()->SetTitle(B_TRANSLATE(
					"MediaConverter: Add files"));
			}
			fOpenPanel->Show();
			break;

		case kMsgRemoveFiles:
			_RemoveSelected();
			break;

		case kMsgClearFinished:
			_ClearFinished();
			break;

		case kMsgQueueSelection:
			_SelectionChanged();
			break;

		case kMsgQueueInvoked:
		{
			// a finished file plays in its preferred application
			QueueItem* item = fQueue->SelectedItem();
			if (item == NULL || item->State() != kJobDone)
				break;
			entry_ref ref;
			if (get_ref_for_path(item->Destination().String(), &ref) == B_OK)
				be_roster->Launch(&ref);
			break;
		}

		case kMsgSourceProbed:
		{
			int32 id;
			SourceInfo* info = NULL;
			if (message->FindInt32("id", &id) != B_OK
				|| message->FindPointer("info", (void**)&info) != B_OK)
				break;
			QueueItem* item = fQueue->ItemByID(id);
			if (item != NULL) {
				int32 status = B_ERROR;
				message->FindInt32("status", &status);
				if (status == B_OK && (info->HasVideo()
						|| !info->audio.empty())) {
					item->SetInfo(*info);
					if (info->IsDisc())
						item->SetDiscTitle(info->discTitle);
					item->SetState(kJobWaiting);
				} else {
					item->SetState(kJobUnreadable);
					BString error = message->GetString("error",
						B_TRANSLATE("Not a video or audio file."));
					item->SetMessage(error.String());
				}
				fQueue->InvalidateItem(item);
				if (item == fQueue->SelectedItem())
					_SelectionChanged();
			}
			delete info;
			_UpdateControls();
			_UpdateStatus();
			if (fConvertWhenReady)
				_Convert();
			break;
		}

		case kMsgDiscTitleChosen:
		{
			// another title of a disc: read it as a new source
			QueueItem* item = fQueue->ItemByID(message->GetInt32("id", -1));
			int32 title = message->GetInt32("title", -1);
			if (item == NULL || !item->IsDisc() || title < 0
				|| title == item->DiscTitle() || !item->IsPending())
				break;
			item->SetDiscTitle(title);
			item->SetInfo(SourceInfo());
			item->SetState(kJobProbing);
			fQueue->InvalidateItem(item);
			BList items;
			items.AddItem(item);
			_StartProbing(&items);
			_SelectionChanged();
			_UpdateControls();
			break;
		}

		case kMsgPreviewReady:
		{
			int32 id;
			BBitmap* bitmap = NULL;
			if (message->FindInt32("id", &id) == B_OK
				&& message->FindPointer("bitmap", (void**)&bitmap) == B_OK)
				fSource->SetPreview(bitmap, id);
			break;
		}

		case kMsgSettingsChanged:
			_UpdateStatus();
			break;

		case kMsgPresetChosen:
			// from the command line: the output view applies it to the
			// settings of the preset's kind
			if (message->WasDelivered() && fOutput->Looper() == this) {
				BMessage preset(*message);
				preset.AddBool("by kind", true);
				PostMessage(&preset, fOutput);
			}
			break;

		case kMsgChooseFolder:
			if (fFolderPanel == NULL) {
				fFolderPanel = new BFilePanel(B_OPEN_PANEL,
					new BMessenger(this), NULL, B_DIRECTORY_NODE, false,
					new BMessage(kMsgFolderChosen));
				fFolderPanel->SetButtonLabel(B_DEFAULT_BUTTON,
					B_TRANSLATE("Choose"));
				fFolderPanel->Window()->SetTitle(B_TRANSLATE(
					"MediaConverter: Save to"));
			}
			fFolderPanel->Show();
			break;

		case kMsgFolderChosen:
		{
			entry_ref ref;
			if (message->FindRef("refs", &ref) == B_OK) {
				BPath path(&ref);
				fOutput->SetDestination(path.Path(), false);
			}
			break;
		}

		case kMsgConvert:
			_Convert();
			break;

		case kMsgCpuWarningAnswer:
		{
			int32 which = message->GetInt32("which", 0);
			if (which == 0 || fConverting)
				break;
			if (which == 1) {
				fWarnCpu = false;
				fWarnItem->SetMarked(false);
			}
			_StartConverting();
			break;
		}

		case kMsgStop:
			if (fConverting) {
				fStopping = true;
				if (fProcess != NULL)
					fProcess->Cancel();
				_UpdateControls();
			}
			break;

		case kMsgToggleCpuWarning:
			fWarnCpu = !fWarnCpu;
			fWarnItem->SetMarked(fWarnCpu);
			break;

		case kMsgHardwareTested:
			fOutput->UpdateHardwareStatus();
			if (fHardwareWindow != NULL && fHardwareWindow->Lock()) {
				fHardwareWindow->PostMessage(kMsgHardwareTested);
				fHardwareWindow->Unlock();
			}
			break;

		case kMsgShowHardware:
			if (fHardwareWindow == NULL || !fHardwareWindow->Lock()) {
				fHardwareWindow = new HardwareWindow(BRect(0, 0, 900, 500));
				fHardwareWindow->Show();
			} else {
				fHardwareWindow->Activate();
				fHardwareWindow->Unlock();
			}
			break;

		case kMsgJobStarted:
		{
			int32 id;
			if (message->FindInt32("id", &id) != B_OK)
				break;
			QueueItem* item = fQueue->ItemByID(id);
			if (item == NULL)
				break;
			item->Report().videoEncoder = message->GetString("encoder", "");
			item->Report().videoSettings = message->GetString("settings", "");
			item->Report().cpuFallback = message->GetBool("fallback", false);
			_UpdateStatus();
			break;
		}

		case kMsgJobProgress:
		{
			int32 id;
			const void* data;
			ssize_t size;
			if (message->FindInt32("id", &id) != B_OK
				|| message->FindData("progress", B_RAW_TYPE, &data, &size)
					!= B_OK || size != sizeof(TranscodeProgress))
				break;
			QueueItem* item = fQueue->ItemByID(id);
			if (item == NULL)
				break;
			item->SetProgress(*(const TranscodeProgress*)data);
			fQueue->InvalidateItem(item);
			_UpdateStatus();
			break;
		}

		case kMsgJobFinished:
			_JobFinished(message);
			break;

		default:
			BWindow::MessageReceived(message);
			break;
	}
}


// #pragma mark - the queue


void
MediaConverterWindow::_AddRefs(BMessage* message)
{
	BList added;
	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		BEntry entry(&ref, true);
		BPath path(&entry);
		if (identify_disc(path.Path()) != kDiscNone) {
			// a DVD or Blu-ray: its main title (a disc can be added again,
			// for another title)
			entry.GetRef(&ref);
			QueueItem* item = new QueueItem(ref);
			item->SetDisc(true);
			fQueue->AddItem(item);
			added.AddItem(item);
			continue;
		}
		if (entry.IsDirectory()) {
			// a folder: the files and discs in it
			BDirectory directory(&entry);
			entry_ref child;
			while (directory.GetNextRef(&child) == B_OK) {
				BEntry childEntry(&child, true);
				BPath childPath(&childEntry);
				bool disc = identify_disc(childPath.Path()) != kDiscNone;
				if (childEntry.IsFile() || disc) {
					childEntry.GetRef(&child);
					QueueItem* item = new QueueItem(child);
					item->SetDisc(disc);
					fQueue->AddItem(item);
					added.AddItem(item);
				}
			}
			continue;
		}
		entry.GetRef(&ref);
		bool known = false;
		for (int32 j = 0; j < fQueue->CountItems(); j++)
			known |= fQueue->QueueItemAt(j)->Ref() == ref;
		if (known)
			continue;
		QueueItem* item = new QueueItem(ref);
		fQueue->AddItem(item);
		added.AddItem(item);
	}
	if (added.IsEmpty())
		return;
	if (fQueue->CurrentSelection() < 0)
		fQueue->Select(fQueue->IndexOf((QueueItem*)added.ItemAt(0)));
	_StartProbing(&added);
	_UpdateControls();
	_UpdateStatus();
}


void
MediaConverterWindow::_StartProbing(BList* items)
{
	ProbeRequest* request = new ProbeRequest;
	request->target = BMessenger(this);
	for (int32 i = 0; i < items->CountItems(); i++) {
		QueueItem* item = (QueueItem*)items->ItemAt(i);
		request->files.push_back(std::make_pair(item->ID(), item->Path()));
	}
	thread_id thread = spawn_thread(_ProbeThread, "probe sources",
		B_LOW_PRIORITY, request);
	if (thread < 0 || resume_thread(thread) != B_OK)
		delete request;
}


void
MediaConverterWindow::_StartPreview(QueueItem* item)
{
	if (item == NULL || (!item->Info().HasVideo() && item->Info().audio.empty()))
		return;
	PreviewRequest* request = new PreviewRequest;
	request->target = BMessenger(this);
	request->id = item->ID();
	request->path = item->Path();
	request->position = 0.1f;
	request->cover = !item->Info().HasVideo();
	thread_id thread = spawn_thread(_PreviewThread, "preview",
		B_LOW_PRIORITY, request);
	if (thread < 0 || resume_thread(thread) != B_OK)
		delete request;
}


void
MediaConverterWindow::_RemoveSelected()
{
	for (int32 i = fQueue->CountItems() - 1; i >= 0; i--) {
		QueueItem* item = fQueue->QueueItemAt(i);
		if (!item->IsSelected() || item->State() == kJobConverting)
			continue;
		fQueue->RemoveItem(i);
		delete item;
	}
	_SelectionChanged();
	_UpdateControls();
	_UpdateStatus();
}


void
MediaConverterWindow::_ClearFinished()
{
	for (int32 i = fQueue->CountItems() - 1; i >= 0; i--) {
		QueueItem* item = fQueue->QueueItemAt(i);
		if (item->State() != kJobDone && item->State() != kJobUnreadable)
			continue;
		fQueue->RemoveItem(i);
		delete item;
	}
	_SelectionChanged();
	_UpdateControls();
	_UpdateStatus();
}


void
MediaConverterWindow::_SelectionChanged()
{
	QueueItem* item = fQueue->SelectedItem();
	if (item == NULL || item->State() == kJobProbing
		|| item->State() == kJobUnreadable) {
		fSource->SetSource(NULL, -1);
		fOutput->SetSource(NULL);
		return;
	}
	if (fSource->ItemID() != item->ID()) {
		fSource->SetSource(&item->Info(), item->ID());
		_StartPreview(item);
	}
	fOutput->SetSource(&item->Info());
	fRemoveButton->SetEnabled(item->State() != kJobConverting);
}


// #pragma mark - converting


BString
MediaConverterWindow::_DestinationFor(QueueItem* item,
	const OutputSettings& settings)
{
	BPath source(&item->Ref());
	BString folder;
	if (fOutput->NextToSource() || fOutput->DestinationFolder().IsEmpty()) {
		if (item->IsDisc())
			folder = disc_folder(source.Path());
		else {
			BPath parent;
			source.GetParent(&parent);
			folder = parent.Path();
		}
	} else
		folder = fOutput->DestinationFolder();

	BString base(source.Leaf());
	int32 dot = base.FindLast('.');
	if (dot > 0)
		base.Truncate(dot);
	if (item->IsDisc()) {
		// "Movie", or "Movie (title 3)" for another title than the main one
		base = item->Name();
		base.ReplaceAll('/', '-');
	}
	const char* extension = container_info(settings.container).extension;

	BString path;
	path.SetToFormat("%s/%s.%s", folder.String(), base.String(), extension);
	for (int attempt = 2; attempt < 1000; attempt++) {
		BEntry entry(path.String());
		if (!entry.Exists() && path != source.Path())
			break;
		path.SetToFormat("%s/%s (%d).%s", folder.String(), base.String(),
			attempt, extension);
	}
	return path;
}


void
MediaConverterWindow::_Convert()
{
	if (fConverting)
		return;
	// files still being read: convert once they are
	for (int32 i = 0; i < fQueue->CountItems(); i++) {
		if (fQueue->QueueItemAt(i)->State() == kJobProbing) {
			fConvertWhenReady = true;
			return;
		}
	}
	fConvertWhenReady = false;

	// what will run on the CPU against the user's wish, and what cannot run;
	// films and songs each with their own settings
	BString cpuFormats;
	int32 pending = 0;
	for (int32 i = 0; i < fQueue->CountItems(); i++) {
		QueueItem* item = fQueue->QueueItemAt(i);
		if (!item->IsPending())
			continue;
		pending++;
		OutputSettings settings = fOutput->SettingsFor(
			item->Info().IsSoundOnly());
		EncoderDecision decision = decide_encoder(settings, &item->Info());
		if (decision.impossible) {
			BAlert* alert = new BAlert(B_TRANSLATE("Cannot convert"),
				decision.text.String(), B_TRANSLATE("OK"), NULL, NULL,
				B_WIDTH_AS_USUAL, B_STOP_ALERT);
			alert->Go(NULL);
			return;
		}
		if (decision.cpuFallback && cpuFormats.IsEmpty())
			cpuFormats = decision.text;
	}
	if (pending == 0)
		return;

	if (!cpuFormats.IsEmpty() && fWarnCpu) {
		BString text = cpuFormats;
		text << "\n\n" << B_TRANSLATE("A format the GPU can make (see "
			"Settings ▸ Video hardware) converts faster and more quietly.");
		BAlert* alert = new BAlert(B_TRANSLATE("Encoding on the CPU"),
			text.String(), B_TRANSLATE("Cancel"),
			B_TRANSLATE("Don't warn again"), B_TRANSLATE("Encode on the CPU"),
			B_WIDTH_AS_USUAL, B_OFFSET_SPACING, B_WARNING_ALERT);
		alert->SetShortcut(0, B_ESCAPE);
		// asynchronous: the window goes on answering while it is up
		alert->Go(new BInvoker(new BMessage(kMsgCpuWarningAnswer), this));
		return;
	}
	_StartConverting();
}


const OutputSettings&
MediaConverterWindow::_RunSettingsFor(QueueItem* item) const
{
	return fRunSettings[item->Info().IsSoundOnly() ? 1 : 0];
}


void
MediaConverterWindow::_StartConverting()
{
	int32 pending = 0;
	for (int32 i = 0; i < fQueue->CountItems(); i++)
		pending += fQueue->QueueItemAt(i)->IsPending() ? 1 : 0;
	if (pending == 0)
		return;

	fRunSettings[0] = fOutput->SettingsFor(false);
	fRunSettings[1] = fOutput->SettingsFor(true);
	fConverting = true;
	fStopping = false;
	fJobsDone = 0;
	fJobsTotal = pending;
	for (int32 i = 0; i < fQueue->CountItems(); i++) {
		QueueItem* item = fQueue->QueueItemAt(i);
		if (item->IsPending()) {
			item->SetState(kJobWaiting);
			item->SetMessage("");
			fQueue->InvalidateItem(item);
		}
	}
	if (!_StartNext()) {
		fConverting = false;
	}
	_UpdateControls();
	_UpdateStatus();
}


bool
MediaConverterWindow::_StartNext()
{
	if (fStopping)
		return false;
	QueueItem* next = NULL;
	for (int32 i = 0; i < fQueue->CountItems(); i++) {
		QueueItem* item = fQueue->QueueItemAt(i);
		if (item->State() == kJobWaiting) {
			next = item;
			break;
		}
	}
	if (next == NULL)
		return false;

	TranscodeJob job;
	job.source = next->Path();
	job.settings = _RunSettingsFor(next);
	job.info = next->Info();
	job.destination = _DestinationFor(next, job.settings);
	next->SetDestination(job.destination.String());
	next->SetState(kJobConverting);
	TranscodeProgress zero;
	memset(&zero, 0, sizeof(zero));
	zero.remaining = -1;
	next->SetProgress(zero);
	fQueue->InvalidateItem(next);

	fProcess = new JobProcess(job);
	fCurrentID = next->ID();
	ConvertRequest* request = new ConvertRequest;
	request->target = BMessenger(this);
	request->id = next->ID();
	request->process = fProcess;
	fThread = spawn_thread(_ConvertThread, "convert", B_NORMAL_PRIORITY,
		request);
	if (fThread < 0 || resume_thread(fThread) != B_OK) {
		delete request;
		delete fProcess;
		fProcess = NULL;
		next->SetState(kJobFailed);
		next->SetMessage(B_TRANSLATE("Could not start."));
		return false;
	}
	return true;
}


void
MediaConverterWindow::_JobFinished(BMessage* message)
{
	if (fThread >= 0) {
		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}
	delete fProcess;
	fProcess = NULL;

	int32 id = message->GetInt32("id", -1);
	status_t status = message->GetInt32("status", B_ERROR);
	QueueItem* item = fQueue->ItemByID(id);
	if (item != NULL) {
		fJobsDone++;
		item->Report().notes = message->GetString("notes", "");
		if (status == B_OK) {
			item->SetState(kJobDone);
			BEntry entry(item->Destination().String());
			off_t size = 0;
			entry.GetSize(&size);
			BString text;
			BPath path(item->Destination().String());
			text.SetToFormat(B_TRANSLATE("Done: %s, %s · %s"), path.Leaf(),
				MediaProbe::FormatSize(size).String(),
				message->GetString("encoder", ""));
			if (message->GetBool("retried", false))
				text << B_TRANSLATE(" (the graphics card stopped)");
			item->SetMessage(text.String());
			fLastOutput = item->Destination();
			// let Tracker know what the new file is
			BNode node(item->Destination().String());
			BNodeInfo nodeInfo(&node);
			nodeInfo.SetType(container_info(
				_RunSettingsFor(item).container).mimeType);
		} else if (status == B_CANCELED) {
			item->SetState(kJobCancelled);
			item->SetMessage(B_TRANSLATE("Stopped."));
			BEntry(item->Destination().String()).Remove();
		} else {
			item->SetState(kJobFailed);
			BString text;
			text.SetToFormat(B_TRANSLATE("Failed: %s"),
				message->GetString("error", B_TRANSLATE("unknown error")));
			item->SetMessage(text.String());
			BEntry(item->Destination().String()).Remove();
		}
		fQueue->InvalidateItem(item);
	}

	if (!_StartNext()) {
		bool stopped = fStopping;
		fConverting = false;
		fStopping = false;
		if (!stopped && fJobsDone > 0) {
			BNotification notification(B_INFORMATION_NOTIFICATION);
			notification.SetGroup(B_TRANSLATE_SYSTEM_NAME("MediaConverter"));
			notification.SetTitle(B_TRANSLATE("Conversion finished"));
			BString content;
			content.SetToFormat(B_TRANSLATE("%" B_PRId32 " of %" B_PRId32
				" files converted."), fJobsDone, fJobsTotal);
			notification.SetContent(content);
			notification.Send();
		}
	}
	_UpdateControls();
	_UpdateStatus();
}


void
MediaConverterWindow::_UpdateControls()
{
	bool pending = false;
	for (int32 i = 0; i < fQueue->CountItems(); i++)
		pending |= fQueue->QueueItemAt(i)->IsPending();
	fConvertButton->SetEnabled(!fConverting && pending);
	fStopButton->SetEnabled(fConverting && !fStopping);
	fOutput->SetEnabled(!fConverting);
	QueueItem* selected = fQueue->SelectedItem();
	fRemoveButton->SetEnabled(selected != NULL
		&& selected->State() != kJobConverting);
}


void
MediaConverterWindow::_UpdateStatus()
{
	if (fConverting) {
		QueueItem* item = fQueue->ItemByID(fCurrentID);
		float fraction = item != NULL ? item->Progress() : 0;
		BString text;
		text.SetToFormat(B_TRANSLATE("Converting %" B_PRId32 " of %" B_PRId32
			": %s"), min_c(fJobsDone + 1, fJobsTotal), fJobsTotal,
			item != NULL ? item->Ref().name : "");
		if (item != NULL && !item->Report().videoEncoder.IsEmpty()) {
			text << " · " << item->Report().videoEncoder;
			if (item->Report().cpuFallback)
				text << B_TRANSLATE(" (CPU)");
		}
		if (fStopping)
			text = B_TRANSLATE("Stopping…");
		fStatus->SetText(text.String());
		float total = fJobsTotal > 0
			? (fJobsDone + fraction) / fJobsTotal * 100 : 0;
		fProgress->SetTo(total);
		return;
	}

	int32 waiting = 0, done = 0, failed = 0;
	for (int32 i = 0; i < fQueue->CountItems(); i++) {
		switch (fQueue->QueueItemAt(i)->State()) {
			case kJobWaiting:
			case kJobCancelled:
			case kJobProbing:
				waiting++;
				break;
			case kJobDone:
				done++;
				break;
			case kJobFailed:
				failed++;
				waiting++;
				break;
			default:
				break;
		}
	}
	BString text;
	if (fQueue->CountItems() == 0)
		text = B_TRANSLATE("Add the files to convert.");
	else if (waiting == 0)
		text.SetToFormat(B_TRANSLATE("%" B_PRId32 " converted."), done);
	else {
		text.SetToFormat(B_TRANSLATE("%" B_PRId32 " to convert"), waiting);
		if (done > 0) {
			BString more;
			more.SetToFormat(B_TRANSLATE(", %" B_PRId32 " done"), done);
			text << more;
		}
		if (failed > 0) {
			BString more;
			more.SetToFormat(B_TRANSLATE(", %" B_PRId32 " failed"), failed);
			text << more;
		}
		text << ".";
	}
	fStatus->SetText(text.String());
	fProgress->SetTo(waiting == 0 && done > 0 ? 100 : 0);
}


// #pragma mark - settings


void
MediaConverterWindow::_LoadSettings()
{
	OutputSettings settings = Presets::At(Presets::DefaultIndex()).settings;
	BString preset = Presets::At(Presets::DefaultIndex()).name;
	OutputSettings soundSettings
		= Presets::At(Presets::DefaultIndex(true)).settings;
	BString soundPreset = Presets::At(Presets::DefaultIndex(true)).name;
	BString folder;
	bool nextToSource = true;

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
		path.Append(kSettingsFile);
		BFile file(path.Path(), B_READ_ONLY);
		BMessage archive;
		if (file.InitCheck() == B_OK && archive.Unflatten(&file) == B_OK) {
			BRect frame;
			if (archive.FindRect("frame", &frame) == B_OK) {
				MoveTo(frame.LeftTop());
				ResizeTo(frame.Width(), frame.Height());
			}
			BMessage output;
			if (archive.FindMessage("output", &output) == B_OK)
				settings.Unarchive(output);
			preset = archive.GetString("preset", "");
			BMessage sound;
			if (archive.FindMessage("sound output", &sound) == B_OK) {
				soundSettings.Unarchive(sound);
				soundPreset = archive.GetString("sound preset", "");
			}
			folder = archive.GetString("folder", "");
			nextToSource = archive.GetBool("next to source", true);
			fWarnCpu = archive.GetBool("warn cpu", true);
		}
	}
	if (folder.IsEmpty()) {
		BPath home;
		if (find_directory(B_USER_DIRECTORY, &home) == B_OK)
			folder = home.Path();
	}
	fOutput->SetSettingsFor(false, settings,
		preset.IsEmpty() ? NULL : preset.String());
	fOutput->SetSettingsFor(true, soundSettings,
		soundPreset.IsEmpty() ? NULL : soundPreset.String());
	fOutput->SetDestination(folder.String(), nextToSource);
	MoveOnScreen();
}


void
MediaConverterWindow::_SaveSettings()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK)
		return;
	path.Append(kSettingsFile);
	BMessage archive;
	archive.AddRect("frame", Frame());
	BMessage output;
	fOutput->SettingsFor(false).Archive(output);
	archive.AddMessage("output", &output);
	archive.AddString("preset", fOutput->PresetNameFor(false));
	BMessage sound;
	fOutput->SettingsFor(true).Archive(sound);
	archive.AddMessage("sound output", &sound);
	archive.AddString("sound preset", fOutput->PresetNameFor(true));
	archive.AddString("folder", fOutput->DestinationFolder());
	archive.AddBool("next to source", fOutput->NextToSource());
	archive.AddBool("warn cpu", fWarnCpu);
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		archive.Flatten(&file);
}
