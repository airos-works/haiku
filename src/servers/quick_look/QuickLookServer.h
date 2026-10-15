/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef QUICK_LOOK_SERVER_H
#define QUICK_LOOK_SERVER_H


#include <Application.h>


class AddOnManager;
class BMessageRunner;
class PreviewWindow;


class QuickLookServer : public BApplication {
public:
								QuickLookServer();
	virtual						~QuickLookServer();

	virtual	void				ReadyToRun();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				RefsReceived(BMessage* message);
	virtual	void				ArgvReceived(int32 argc, char** argv);
	virtual	bool				QuitRequested();

private:
			void				_Forward(BMessage* message);
			void				_ArmQuitTimer();

			AddOnManager*		fAddOns;
			PreviewWindow*		fWindow;
			BMessageRunner*		fQuitRunner;
};


#endif	// QUICK_LOOK_SERVER_H
