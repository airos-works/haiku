/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "QuickLookServer.h"

#include <stdio.h>
#include <string.h>

#include <Entry.h>
#include <MessageRunner.h>
#include <Path.h>

#include <QuickLookDefs.h>

#include "AddOnManager.h"
#include "PreviewWindow.h"


static const uint32 kMsgQuitIfIdle = 'quit';
// The server stays for a while after the panel closed, so that the next
// space bar press is answered at once; then it quits and frees the
// decoders the add-ons loaded.
static const bigtime_t kIdleQuitDelay = 3 * 60 * 1000000LL;


QuickLookServer::QuickLookServer()
	:
	BApplication(B_QUICK_LOOK_SIGNATURE),
	fAddOns(new AddOnManager()),
	fWindow(NULL),
	fQuitRunner(NULL)
{
}


QuickLookServer::~QuickLookServer()
{
	delete fQuitRunner;
	delete fAddOns;
}


void
QuickLookServer::ReadyToRun()
{
	if (fWindow == NULL) {
		fWindow = new PreviewWindow(fAddOns);
		fWindow->Run();
	}
	_ArmQuitTimer();
}


void
QuickLookServer::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgQuickLookShow:
		case kMsgQuickLookToggle:
			delete fQuitRunner;
			fQuitRunner = NULL;
			_Forward(message);
			break;

		case kMsgQuickLookHide:
			// From a client, or from the window once it is hidden.
			if (message->IsSourceRemote())
				_Forward(message);
			_ArmQuitTimer();
			break;

		case kMsgQuitIfIdle:
			if (fWindow == NULL || !fWindow->IsShowing())
				PostMessage(B_QUIT_REQUESTED);
			break;

		default:
			BApplication::MessageReceived(message);
	}
}


void
QuickLookServer::RefsReceived(BMessage* message)
{
	// Launched with files: show them.
	message->what = kMsgQuickLookShow;
	delete fQuitRunner;
	fQuitRunner = NULL;
	_Forward(message);
}


void
QuickLookServer::ArgvReceived(int32 argc, char** argv)
{
	BMessage message(kMsgQuickLookShow);
	for (int32 i = 1; i < argc; i++) {
		BEntry entry(argv[i]);
		entry_ref ref;
		if (entry.Exists() && entry.GetRef(&ref) == B_OK)
			message.AddRef("refs", &ref);
	}
	if (message.HasRef("refs")) {
		delete fQuitRunner;
		fQuitRunner = NULL;
		_Forward(&message);
	}
}


bool
QuickLookServer::QuitRequested()
{
	if (fWindow != NULL && fWindow->Lock()) {
		fWindow->Quit();
		fWindow = NULL;
	}
	return true;
}


void
QuickLookServer::_Forward(BMessage* message)
{
	if (fWindow == NULL) {
		fWindow = new PreviewWindow(fAddOns);
		fWindow->Run();
	}
	fWindow->PostMessage(message);
}


void
QuickLookServer::_ArmQuitTimer()
{
	delete fQuitRunner;
	BMessage quit(kMsgQuitIfIdle);
	fQuitRunner = new BMessageRunner(this, &quit, kIdleQuitDelay, 1);
}


int
main()
{
	QuickLookServer server;
	server.Run();
	return 0;
}
