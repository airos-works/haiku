/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "AddOnManager.h"

#include <algorithm>
#include <stdio.h>
#include <sys/stat.h>

#include <Directory.h>
#include <File.h>
#include <FindDirectory.h>
#include <Mime.h>
#include <NodeInfo.h>
#include <PathFinder.h>
#include <StringList.h>
#include <SymLink.h>
#include <syslog.h>

#include <add-ons/quick_look/QuickLook.h>


typedef BQuickLookAddOn* (*instantiate_function)(image_id);


AddOnManager::AddOnManager()
	:
	fScanned(false)
{
}


AddOnManager::~AddOnManager()
{
	_Unload();
}


BQuickLookPreview*
AddOnManager::CreatePreview(const entry_ref& ref, BString* mimeType,
	BString* error)
{
	if (!fScanned || _DirectoriesChanged())
		_Rescan();

	*mimeType = _TypeOf(ref);
	error->Truncate(0);

	struct Bid {
		float			score;
		BQuickLookAddOn* addOn;
		const char*		name;
	};
	std::vector<Bid> bids;
	for (size_t i = 0; i < fAddOns.size(); i++) {
		float score = fAddOns[i].addOn->Supports(ref, mimeType->String());
		if (score > 0)
			bids.push_back({score, fAddOns[i].addOn, fAddOns[i].path.Leaf()});
	}
	std::stable_sort(bids.begin(), bids.end(),
		[](const Bid& a, const Bid& b) { return a.score > b.score; });

	for (size_t i = 0; i < bids.size(); i++) {
		BString why;
		BQuickLookPreview* preview = bids[i].addOn->CreatePreview(ref,
			mimeType->String(), &why);
		if (preview != NULL)
			return preview;
		if (why.Length() > 0) {
			syslog(LOG_INFO, "quick_look: %s declined %s: %s", bids[i].name,
				ref.name, why.String());
			*error = why;
		}
	}
	return NULL;
}


void
AddOnManager::_Rescan()
{
	// Add-ons that are loaded stay loaded: a preview being shown may still
	// run their code. Only newly installed ones are picked up; a replaced
	// one is seen when the server next starts.
	fDirectories.clear();

	BStringList paths;
	BPathFinder::FindPaths(B_FIND_PATH_ADD_ONS_DIRECTORY, "quick_look",
		B_FIND_PATH_EXISTING_ONLY, paths);
	for (int32 i = 0; i < paths.CountStrings(); i++) {
		BPath path(paths.StringAt(i));
		struct stat st;
		Directory directory;
		directory.path = path;
		directory.modified = stat(path.Path(), &st) == 0 ? st.st_mtime : 0;
		fDirectories.push_back(directory);
		_LoadDirectory(path);
	}
	fScanned = true;
}


bool
AddOnManager::_DirectoriesChanged()
{
	// A directory that appeared since the last scan is seen through the
	// count; one that changed through its time.
	BStringList paths;
	BPathFinder::FindPaths(B_FIND_PATH_ADD_ONS_DIRECTORY, "quick_look",
		B_FIND_PATH_EXISTING_ONLY, paths);
	if ((size_t)paths.CountStrings() != fDirectories.size())
		return true;
	for (size_t i = 0; i < fDirectories.size(); i++) {
		struct stat st;
		if (stat(fDirectories[i].path.Path(), &st) != 0
			|| st.st_mtime != fDirectories[i].modified)
			return true;
	}
	return false;
}


void
AddOnManager::_LoadDirectory(const BPath& directoryPath)
{
	BDirectory directory(directoryPath.Path());
	BEntry entry;
	while (directory.GetNextEntry(&entry, true) == B_OK) {
		BPath path;
		if (entry.GetPath(&path) != B_OK || !entry.IsFile())
			continue;
		// The same add-on in a higher priority directory wins.
		bool known = false;
		for (size_t i = 0; i < fAddOns.size(); i++) {
			if (strcmp(fAddOns[i].path.Leaf(), path.Leaf()) == 0)
				known = true;
		}
		if (known)
			continue;

		image_id image = load_add_on(path.Path());
		if (image < 0) {
			syslog(LOG_WARNING, "quick_look: %s does not load: %s",
				path.Path(), strerror(image));
			continue;
		}
		instantiate_function instantiate;
		if (get_image_symbol(image, "instantiate_quick_look_add_on",
				B_SYMBOL_TYPE_TEXT, (void**)&instantiate) != B_OK) {
			syslog(LOG_WARNING, "quick_look: %s is not a Quick Look add-on",
				path.Path());
			unload_add_on(image);
			continue;
		}
		BQuickLookAddOn* addOn = instantiate(image);
		if (addOn == NULL) {
			unload_add_on(image);
			continue;
		}
		AddOn loaded;
		loaded.path = path;
		loaded.image = image;
		loaded.addOn = addOn;
		fAddOns.push_back(loaded);
	}
}


void
AddOnManager::_Unload()
{
	for (size_t i = 0; i < fAddOns.size(); i++) {
		delete fAddOns[i].addOn;
		unload_add_on(fAddOns[i].image);
	}
	fAddOns.clear();
}


BString
AddOnManager::_TypeOf(const entry_ref& ref)
{
	BEntry entry(&ref, true);
	if (entry.IsDirectory())
		return "application/x-vnd.Be-directory";

	char type[B_MIME_TYPE_LENGTH];
	BNode node(&entry);
	BNodeInfo info(&node);
	if (info.InitCheck() == B_OK && info.GetType(type) == B_OK
		&& type[0] != '\0')
		return type;

	// Not typed yet: sniff it, and remember the result as Tracker would.
	entry_ref resolved;
	entry.GetRef(&resolved);
	update_mime_info(BPath(&resolved).Path(), false, true, false);
	if (info.InitCheck() == B_OK && info.GetType(type) == B_OK
		&& type[0] != '\0')
		return type;

	BMimeType mimeType;
	if (BMimeType::GuessMimeType(&resolved, &mimeType) == B_OK)
		return mimeType.Type();
	return B_FILE_MIME_TYPE;
}
