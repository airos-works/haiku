/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef UPDATE_WORKER_H
#define UPDATE_WORKER_H


#include <Looper.h>
#include <Messenger.h>

#include "Feed.h"
#include "Updater.h"


enum {
	// to the worker
	kMsgCheck				= 'chck',
	kMsgInstall				= 'inst',
		// "id": the release
	kMsgGoBack				= 'gobk',
		// "state"
	kMsgRemoveFailed		= 'rmfl',
	kMsgCloseApps			= 'clap',
		// the user's applications quit, as in a shutdown
	kMsgCloseDesktop		= 'clds',
		// then Deskbar and Tracker

	// from the worker
	kMsgStatus				= 'stat',
		// "text", "fraction" (< 0: none)
	kMsgCheckDone			= 'chdn',
		// "error", or "available", "id", "title", "revision", "size",
		// "packages" (count, -1: unknown), "package" (BMessages: "name",
		// "from", "to" - empty when new or removed - and "size", -1 for
		// none), "changes" (the release notes), "count" (newer releases)
	kMsgInstallDone			= 'indn',
		// "error", "title", "cancelled"
	kMsgInstallCommitting	= 'incm',
		// everything is downloaded: the install goes on to the end
	kMsgCloseAppsDone		= 'cadn',
		// "error" (status_t), "app" (the one that would not quit)
	kMsgCloseDesktopDone	= 'cddn',
	kMsgGoBackDone			= 'gbdn',
		// "error", "title"
	kMsgRemoveFailedDone	= 'rfdn'
		// "error"
};


// Does what takes long - talking to the build server, downloading,
// installing - away from the window, and reports to it.
class UpdateWorker : public BLooper, private UpdaterListener {
public:
								UpdateWorker(const BMessenger& target);

	virtual	void				MessageReceived(BMessage* message);

			void				Cancel();
									// from any thread

	static	BString				FormatChanges(const BMessage& changes,
									const char* title);
									// the release notes; the packages are
									// in the check's "package" rows

private:
	virtual	void				UpdaterStatus(const char* text);
	virtual	void				UpdaterProgress(float fraction);

			void				_Check();
			void				_Install(const char* id);
			void				_GoBack(const char* state);
			void				_RemoveFailed();
			void				_CloseApps();
			void				_CloseDesktop();
			void				_AddPackageRows(const UpdatePlan& plan,
									BMessage& reply);
			void				_AddFeedPackageRows(BMessage& reply);

private:
			BMessenger			fTarget;
			Updater				fUpdater;
			Feed				fFeed;
			BString				fStatusText;
			bigtime_t			fLastProgress;
};


#endif	// UPDATE_WORKER_H
