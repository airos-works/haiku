/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TRACKER_FOLDER_SIZES_H
#define _TRACKER_FOLDER_SIZES_H


#include <Entry.h>
#include <Messenger.h>
#include <Node.h>


namespace BPrivate {


// Folder sizes for list views that show them: the total size of the files
// below a folder, on the same volume, worked out by one background thread
// and kept in a cache shared by all windows.

const uint32 kFolderSizeReady = 'Tfsr';
	// sent to the requester: "device" (int32), "node" (int64), "size" (int64)

// Gets the cached size of a folder, true when there is one.
bool GetFolderSize(const node_ref& node, off_t* size);

// Asks for the size of the folder at ref; target gets kFolderSizeReady.
// A cached size younger than maxAge (or any, with maxAge < 0) is sent at once.
void RequestFolderSize(const entry_ref& ref, const node_ref& node,
	const BMessenger& target, bigtime_t maxAge = 30000000);

// Drops what is known about a folder, after it changed.
void ForgetFolderSize(const node_ref& node);

// Whether sizes of folders on this volume are worked out at all (not on
// network file systems, where walking a tree is slow).
bool CanCalculateFolderSizes(dev_t device);


} // namespace BPrivate

using namespace BPrivate;


#endif	// _TRACKER_FOLDER_SIZES_H
