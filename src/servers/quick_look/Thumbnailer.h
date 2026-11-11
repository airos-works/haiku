/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef QUICK_LOOK_THUMBNAILER_H
#define QUICK_LOOK_THUMBNAILER_H


#include <Locker.h>
#include <OS.h>

#include <deque>


class AddOnManager;
class BMessage;


/*!	Makes thumbnails for Tracker on a thread of its own, so the panel never
	waits for one: the add-ons first, then a frame of a video through the
	Media Kit. The request messages are kept and answered from that thread. */
class Thumbnailer {
public:
								Thumbnailer(AddOnManager* addOns);
								~Thumbnailer();

			// Takes over a detached kMsgQuickLookThumbnail request
			void				Add(BMessage* request);
			bool				IsBusy();

private:
	static	status_t			_ThreadEntry(void* data);
			void				_Run();
			void				_Answer(BMessage* request);

			AddOnManager*		fAddOns;
			BLocker				fLock;
			std::deque<BMessage*> fQueue;
			sem_id				fSem;
			thread_id			fThread;
			bool				fWorking;
			bool				fQuit;
};


#endif	// QUICK_LOOK_THUMBNAILER_H
