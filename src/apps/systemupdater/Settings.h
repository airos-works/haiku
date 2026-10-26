/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SETTINGS_H
#define SETTINGS_H


#include <Rect.h>
#include <String.h>


// The user's choices, in ~/config/settings/SystemUpdater.
struct Settings {
			BString				feed;
				// empty: the address the installed release names
			bool				checkAutomatically;
			int32				versionsToKeep;
			BRect				windowFrame;

								Settings();

			status_t			Load();
			status_t			Save() const;
};


#endif	// SETTINGS_H
