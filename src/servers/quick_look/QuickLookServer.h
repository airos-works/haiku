/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef QUICK_LOOK_SERVER_H
#define QUICK_LOOK_SERVER_H


#include <Server.h>


class AddOnManager;
class BMessageRunner;
class PreviewWindow;


/*!	A BServer rather than a BApplication: only BServer's constructor asks
	launch_daemon for the port it made for an on-demand service, and a
	plain BApplication would read its own port while the panel's messages
	queued, unread, in launch_daemon's. */
class QuickLookServer : public BServer {
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
