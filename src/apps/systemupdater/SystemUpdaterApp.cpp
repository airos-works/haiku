/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include <Application.h>
#include <Window.h>

#include "CommandLine.h"
#include "Constants.h"
#include "UpdaterWindow.h"


class SystemUpdaterApp : public BApplication {
public:
	SystemUpdaterApp()
		:
		BApplication(kAppSignature),
		fWindow(NULL)
	{
	}

	virtual void ReadyToRun()
	{
		fWindow = new UpdaterWindow();
		fWindow->Show();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case B_PACKAGE_UPDATE:
				if (fWindow != NULL)
					fWindow->PostMessage(message);
				break;
			default:
				BApplication::MessageReceived(message);
		}
	}

private:
	UpdaterWindow*	fWindow;
};


int
main(int argc, char** argv)
{
	if (is_command_line(argc, argv))
		return run_command_line(argc, argv);

	SystemUpdaterApp app;
	app.Run();
	return 0;
}
