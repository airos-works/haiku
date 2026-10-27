/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef QUICK_LOOK_ADD_ON_MANAGER_H
#define QUICK_LOOK_ADD_ON_MANAGER_H


#include <vector>

#include <Entry.h>
#include <Path.h>
#include <String.h>
#include <image.h>


class BQuickLookAddOn;
class BQuickLookPreview;


/*!	Loads the add-ons from every add-ons/quick_look directory and asks them
	for previews. Add-ons stay loaded; the directories are looked at again
	whenever one of them changed, so a newly installed application's add-on
	is found without a restart. Loaded add-ons are never unloaded while the
	server runs: a preview may still be running their code. */
class AddOnManager {
public:
								AddOnManager();
								~AddOnManager();

	/*!	The best preview for the entry: the add-ons' bids are sorted and
		each is asked in turn until one makes a view. Returns NULL when none
		does, with the type found in *mimeType and the last error. */
			BQuickLookPreview*	CreatePreview(const entry_ref& ref,
									BString* mimeType, BString* error);

			int32				CountAddOns() const { return fAddOns.size(); }

private:
			struct AddOn {
				BPath			path;
				image_id		image;
				BQuickLookAddOn* addOn;
			};
			struct Directory {
				BPath			path;
				time_t			modified;
			};

			void				_Rescan();
			bool				_DirectoriesChanged();
			void				_LoadDirectory(const BPath& path);
			void				_Unload();
			BString				_TypeOf(const entry_ref& ref);

			std::vector<Directory> fDirectories;
			std::vector<AddOn>	fAddOns;
			bool				fScanned;
};


#endif	// QUICK_LOOK_ADD_ON_MANAGER_H
