/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_FOLDER_STATE_STORE_H
#define _TRACKER_FOLDER_STATE_STORE_H


#include <Entry.h>
#include <Node.h>
#include <Rect.h>


namespace BPrivate {


// Tracker keeps a folder's window frame, view state and columns in
// attributes of the folder itself. Folders on volumes that are read-only
// or cannot hold attributes (FAT, NTFS, ISO 9660, network shares,
// packagefs) keep them instead in a per-user store, one file per folder in
// ~/config/settings/Tracker/FolderStates, carrying the same attributes.

// True when the state of the folder at ref, on device, belongs in the store.
bool FolderStateUsesStore(dev_t device);

// Opens the store file of the folder at ref. With create, a missing file is
// made (and an existing one of another folder is taken over). Without it,
// B_ENTRY_NOT_FOUND is returned when the folder has nothing stored.
status_t OpenFolderStateStoreNode(const entry_ref* ref, bool create,
	BNode* node);

// Sets node to where the saved state of the folder at entry is read from:
// its store file if it has one, or else the folder itself.
status_t GetFolderStateNode(const BEntry& entry, BNode* node);

// Writes the view state and columns (raw attribute data) into the folder at
// ref, or its store file. With frame, the frame is written too if the folder
// has none yet, so the folder counts as having saved state.
status_t WriteFolderViewState(const entry_ref* ref, dev_t device,
	const void* viewState, size_t viewStateSize, const void* columns,
	size_t columnsSize, const BRect* frame);


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_FOLDER_STATE_STORE_H
