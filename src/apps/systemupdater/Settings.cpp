/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Settings.h"

#include <File.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Path.h>

#include "Constants.h"


static status_t
settings_path(BPath& _path)
{
	status_t error = find_directory(B_USER_SETTINGS_DIRECTORY, &_path, true);
	if (error == B_OK)
		error = _path.Append(kSettingsFileName);
	return error;
}


Settings::Settings()
	:
	checkAutomatically(true),
	versionsToKeep(kDefaultVersionsToKeep),
	windowFrame()
{
}


status_t
Settings::Load()
{
	BPath path;
	status_t error = settings_path(path);
	if (error != B_OK)
		return error;
	BFile file(path.Path(), B_READ_ONLY);
	BMessage archive;
	error = file.InitCheck();
	if (error == B_OK)
		error = archive.Unflatten(&file);
	if (error != B_OK)
		return error;

	const char* value;
	if (archive.FindString("feed", &value) == B_OK)
		feed = value;
	bool flag;
	if (archive.FindBool("check automatically", &flag) == B_OK)
		checkAutomatically = flag;
	int32 count;
	if (archive.FindInt32("versions to keep", &count) == B_OK && count >= 1
		&& count <= 10) {
		versionsToKeep = count;
	}
	BRect frame;
	if (archive.FindRect("window frame", &frame) == B_OK)
		windowFrame = frame;
	return B_OK;
}


status_t
Settings::Save() const
{
	BPath path;
	status_t error = settings_path(path);
	if (error != B_OK)
		return error;
	BMessage archive;
	archive.AddString("feed", feed);
	archive.AddBool("check automatically", checkAutomatically);
	archive.AddInt32("versions to keep", versionsToKeep);
	if (windowFrame.IsValid())
		archive.AddRect("window frame", windowFrame);
	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	error = file.InitCheck();
	if (error == B_OK)
		error = archive.Flatten(&file);
	return error;
}
