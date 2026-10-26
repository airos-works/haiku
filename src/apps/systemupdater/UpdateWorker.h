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

	// from the worker
	kMsgStatus				= 'stat',
		// "text", "fraction" (< 0: none)
	kMsgCheckDone			= 'chdn',
		// "error", or "available", "id", "title", "size", "changes",
		// "count" (newer releases), "checked" (when the feed answered)
	kMsgInstallDone			= 'indn',
		// "error", "title"
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

private:
	virtual	void				UpdaterStatus(const char* text);
	virtual	void				UpdaterProgress(float fraction);

			void				_Check();
			void				_Install(const char* id);
			void				_GoBack(const char* state);
			void				_RemoveFailed();

private:
			BMessenger			fTarget;
			Updater				fUpdater;
			Feed				fFeed;
			BString				fStatusText;
			bigtime_t			fLastProgress;
};


#endif	// UPDATE_WORKER_H
