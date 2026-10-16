/*
 * Copyright 2003-2026, Haiku, Inc.
 * Distributed under the terms of the MIT license.
 *
 * Authors:
 *		Sikosis, Jérôme Duval
 *		yourpalal, Alex Wilson
 */
#ifndef MEDIA_WINDOW_H
#define MEDIA_WINDOW_H


#include <String.h>
#include <Window.h>

#include "MediaModel.h"


class BButton;
class BGroupView;
class BMessageRunner;
class Page;
class Sidebar;


class MediaWindow : public BWindow {
public:
								MediaWindow(BRect frame, const char* page);
	virtual						~MediaWindow();

	virtual	bool				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);

	virtual	BHandler*			ResolveSpecifier(BMessage* message,
									int32 index, BMessage* specifier,
									int32 what, const char* property);
	virtual	status_t			GetSupportedSuites(BMessage* data);

			const BString&		PageKey() const { return fPageKey; }

private:
			void				_InitWindow();
			void				_StartWatching();
			void				_Rescan(bool force);
			void				_ScheduleRescan();
			void				_RebuildSidebar();
			void				_ShowPage(const char* key);
			Page*				_CreatePage(const char* key);
			void				_ShowServerDown();
			void				_SetDefault(BMessage* message);
			bool				_HandleScripting(BMessage* message);

	static	status_t			_RestartThread(void* data);
	static	bool				_RestartProgress(int stage,
									const char* message, void* cookie);

			Sidebar*			fSidebar;
			BGroupView*			fPageHolder;
			Page*				fPage;
			BString				fPageKey;
			BString				fWantedKey;
			BButton*			fRestartButton;

			MediaModel			fModel;
			bool				fServerRunning;
			bool				fWatching;
			thread_id			fRestartThread;
			BMessageRunner*		fRescanRunner;
};


#endif	// MEDIA_WINDOW_H
