/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef APP_LIST_H
#define APP_LIST_H

/*	AppList: the applications the home screen and the application drawer
	show. Today they all come from the applications and preferences
	directories (_ScanDirectory()); another kind of application gets its
	entries in from Scan() the same way.
*/

#include <Entry.h>
#include <String.h>

#include <vector>


class BBitmap;


struct AppEntry {
	BString		name;		// localized, as the Deskbar shows it
	BString		signature;
	entry_ref	ref;		// what to launch
	BBitmap*	icon;		// Theme::IconSize(), owned by the list
	bool		preference;	// from the preferences directory
};


class AppList {
public:
								AppList();
								~AppList();

			void				Scan();

			const std::vector<AppEntry*>& Apps() const { return fApps; }
			const std::vector<AppEntry*>& Favorites() const
									{ return fFavorites; }
			const std::vector<AppEntry*>& Dock() const { return fDock; }

			AppEntry*			FindBySignature(const char* signature) const;

private:
			void				_ScanDirectory(const char* path,
									bool preference);
			void				_LoadFavorites();
			void				_Clear();

			std::vector<AppEntry*> fApps;		// sorted by name
			std::vector<AppEntry*> fFavorites;	// home grid
			std::vector<AppEntry*> fDock;		// bottom row
};


#endif	// APP_LIST_H
