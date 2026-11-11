/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "FolderStateStore.h"

#include <Directory.h>
#include <File.h>
#include <Path.h>
#include <String.h>
#include <Volume.h>

#include <fs_attr.h>
#include <stdio.h>
#include <sys/stat.h>

#include <vector>
#include <algorithm>

#include "Attributes.h"
#include "FSUtils.h"


namespace BPrivate {


static const char* kStoreDirName = "FolderStates";
static const char* kAttrStoreKey = "_trk/folder_state_key";
static const int32 kMaxStoredFolders = 4000;


static uint64
HashKey(const char* key)
{
	// FNV-1a
	uint64 hash = 0xcbf29ce484222325ULL;
	for (const uint8* c = (const uint8*)key; *c != '\0'; c++) {
		hash ^= *c;
		hash *= 0x100000001b3ULL;
	}
	return hash;
}


static status_t
GetStoreDir(BPath* path, bool create)
{
	status_t status = FSFindTrackerSettingsDir(path, create);
	if (status != B_OK)
		return status;

	status = path->Append(kStoreDirName);
	if (status != B_OK)
		return status;

	if (create)
		create_directory(path->Path(), 0755);

	return B_OK;
}


static void
PruneStore(const char* dirPath)
{
	// keep the store bounded: when it is full, drop the tenth of the files
	// that were written to least recently
	BDirectory dir(dirPath);
	if (dir.InitCheck() != B_OK || dir.CountEntries() < kMaxStoredFolders)
		return;

	std::vector<std::pair<time_t, BString> > files;
	BEntry entry;
	while (dir.GetNextEntry(&entry) == B_OK) {
		struct stat st;
		if (entry.GetStat(&st) == B_OK)
			files.push_back(std::make_pair(st.st_mtime, BString(entry.Name())));
	}

	std::sort(files.begin(), files.end());
	size_t removeCount = files.size() / 10 + 1;
	for (size_t i = 0; i < removeCount && i < files.size(); i++) {
		if (dir.FindEntry(files[i].second.String(), &entry) == B_OK)
			entry.Remove();
	}
}


bool
FolderStateUsesStore(dev_t device)
{
	BVolume volume(device);
	if (volume.InitCheck() != B_OK)
		return false;

	return volume.IsReadOnly() || !volume.KnowsAttr();
}


status_t
OpenFolderStateStoreNode(const entry_ref* ref, bool create, BNode* node)
{
	BPath folderPath;
	status_t status = folderPath.SetTo(ref);
	if (status != B_OK)
		return status;

	BPath storePath;
	status = GetStoreDir(&storePath, create);
	if (status != B_OK)
		return status;

	char name[32];
	snprintf(name, sizeof(name), "%016" B_PRIx64,
		HashKey(folderPath.Path()));

	BPath filePath(storePath);
	filePath.Append(name);

	BFile file;
	status = file.SetTo(filePath.Path(), B_READ_WRITE);
	if (status == B_OK) {
		BString key;
		if (file.ReadAttrString(kAttrStoreKey, &key) != B_OK
			|| key != folderPath.Path()) {
			// another folder with the same hash: never mix up their state
			if (!create)
				return B_ENTRY_NOT_FOUND;

			char attrName[B_ATTR_NAME_LENGTH];
			while (file.GetNextAttrName(attrName) == B_OK) {
				file.RemoveAttr(attrName);
				file.RewindAttrs();
			}
			BString pathKey(folderPath.Path());
			file.WriteAttrString(kAttrStoreKey, &pathKey);
		}
	} else if (status == B_ENTRY_NOT_FOUND && create) {
		PruneStore(storePath.Path());
		status = file.SetTo(filePath.Path(),
			B_READ_WRITE | B_CREATE_FILE | B_FAIL_IF_EXISTS);
		if (status != B_OK)
			return status;

		BString pathKey(folderPath.Path());
		file.WriteAttrString(kAttrStoreKey, &pathKey);
	} else
		return status;

	file.Unset();
	return node->SetTo(filePath.Path());
}


status_t
GetFolderStateNode(const BEntry& entry, BNode* node)
{
	entry_ref ref;
	struct stat st;
	if (entry.GetRef(&ref) == B_OK && entry.GetStat(&st) == B_OK
		&& FolderStateUsesStore(st.st_dev)
		&& OpenFolderStateStoreNode(&ref, false, node) == B_OK) {
		return B_OK;
	}

	return node->SetTo(&entry);
}


status_t
WriteFolderViewState(const entry_ref* ref, dev_t device, const void* viewState,
	size_t viewStateSize, const void* columns, size_t columnsSize,
	const BRect* frame)
{
	BNode node;
	status_t status;
	if (FolderStateUsesStore(device))
		status = OpenFolderStateStoreNode(ref, true, &node);
	else
		status = node.SetTo(ref);
	if (status != B_OK)
		return status;

	// the attribute of the other byte order would be read when the native
	// one is missing only; drop it so it never brings back an old view
	node.RemoveAttr(kAttrViewStateForeign);
	node.RemoveAttr(kAttrColumnsForeign);

	ssize_t written = node.WriteAttr(kAttrViewState, B_RAW_TYPE, 0, viewState,
		viewStateSize);
	if (written < 0)
		return written;
	written = node.WriteAttr(kAttrColumns, B_RAW_TYPE, 0, columns,
		columnsSize);
	if (written < 0)
		return written;

	attr_info info;
	if (frame != NULL && node.GetAttrInfo(kAttrWindowFrame, &info) != B_OK)
		node.WriteAttr(kAttrWindowFrame, B_RECT_TYPE, 0, frame, sizeof(BRect));

	return B_OK;
}


} // namespace BPrivate
