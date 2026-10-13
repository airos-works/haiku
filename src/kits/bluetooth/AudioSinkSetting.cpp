/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <AudioSinkSetting.h>

#include <stdio.h>
#include <string.h>

#include <File.h>
#include <FindDirectory.h>

#include <bluetooth/bdaddrUtils.h>


namespace Bluetooth {


status_t
GetAudioSinkSettingsPath(BPath& path)
{
	status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
	if (status == B_OK)
		status = path.Append("bluetooth_audio");
	return status;
}


status_t
GetAudioSink(bdaddr_t& address, BString& name)
{
	BPath path;
	status_t status = GetAudioSinkSettingsPath(path);
	if (status != B_OK)
		return status;

	BFile file(path.Path(), B_READ_ONLY);
	char buffer[512];
	ssize_t bytes = file.InitCheck() == B_OK
		? file.Read(buffer, sizeof(buffer) - 1) : -1;
	if (bytes <= 0)
		return B_ENTRY_NOT_FOUND;
	buffer[bytes] = '\0';

	// "sink AA:BB:CC:DD:EE:FF" and "name <rest of the line>"
	bool found = false;
	name = "";
	char* line = buffer;
	while (line != NULL && *line != '\0') {
		char* next = strchr(line, '\n');
		if (next != NULL)
			*next++ = '\0';
		if (strncmp(line, "sink ", 5) == 0) {
			address = bdaddrUtils::FromString(line + 5);
			found = !bdaddrUtils::Compare(address, bdaddrUtils::NullAddress());
		} else if (strncmp(line, "name ", 5) == 0)
			name = line + 5;
		line = next;
	}
	return found ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
SetAudioSink(const bdaddr_t* address, const char* name)
{
	BPath path;
	status_t status = GetAudioSinkSettingsPath(path);
	if (status != B_OK)
		return status;

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status = file.InitCheck();
	if (status != B_OK || address == NULL)
		return status;

	char text[18];
	snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
		address->b[5], address->b[4], address->b[3], address->b[2],
		address->b[1], address->b[0]);
	BString contents;
	contents.SetToFormat("sink %s\n", text);
	if (name != NULL && name[0] != '\0') {
		BString oneLine(name);
		oneLine.ReplaceAll('\n', ' ');
		contents << "name " << oneLine << "\n";
	}
	ssize_t written = file.Write(contents.String(), contents.Length());
	return written == contents.Length() ? B_OK : B_IO_ERROR;
}


} // namespace Bluetooth
