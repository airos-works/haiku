/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "OSDServer.h"

#include <OnScreenDisplay.h>

#include "LevelWindow.h"


OSDServer::OSDServer(status_t& error)
	:
	BServer(kOSDServerSignature, true, &error),
	fLevelWindow(NULL)
{
}


void
OSDServer::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case kMsgShowLevel:
			// the message that started the server can come before
			// B_READY_TO_RUN, so the window is made when it is needed
			if (fLevelWindow == NULL) {
				fLevelWindow = new LevelWindow();
				// running, but hidden until there is a level to show
				fLevelWindow->Hide();
				fLevelWindow->Show();
			}
			fLevelWindow->PostMessage(message);
			break;

		default:
			BServer::MessageReceived(message);
			break;
	}
}


//	#pragma mark -


int
main(int argc, char** argv)
{
	status_t error;
	OSDServer server(error);
	if (error != B_OK)
		return 1;

	server.Run();
	return 0;
}
