/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef INSTALL_WINDOW_H
#define INSTALL_WINDOW_H


#include <Messenger.h>
#include <Window.h>


class BButton;
class BStatusBar;
class BStringView;
class BTextView;


enum {
	// to the target
	kMsgCancelInstall		= 'xins',
	kMsgRestartNow			= 'rsnw'
};


// What the screen shows once the applications are closed for an update:
// the download and the install, then the restart. Stays in front of every
// window; Cancel works until the install itself begins.
class InstallWindow : public BWindow {
public:
								InstallWindow(const char* title,
									const BMessenger& target);

	virtual	bool				QuitRequested();

			void				SetStatus(const char* text, float fraction);
			void				SetCommitting();
									// no going back from here
			void				SetRestarting(const char* text);
			void				SetRestartStalled();
									// the restart did not happen

private:
			BStringView*		fTitleView;
			BTextView*			fTextView;
			BStatusBar*			fProgressBar;
			BButton*			fCancelButton;
			BButton*			fRestartButton;
			BMessenger			fTarget;
};


#endif	// INSTALL_WINDOW_H
