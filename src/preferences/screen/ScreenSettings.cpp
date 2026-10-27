/*
 * Copyright 2001-2015, Haiku.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Rafael Romo
 *		Stefano Ceccherini (burton666@libero.it)
 *		Axel Dörfler, axeld@pinc-software.de
 */


#include "ScreenSettings.h"

#include <math.h>

#include <File.h>
#include <FindDirectory.h>
#include <Path.h>


static const char* kSettingsFileName = "Screen_data";


/*!	Whether a coordinate read from the file can be part of a window's
	position: a damaged file (data that never reached the disk before a
	reboot) gave 7.2e22, and the window could not be moved back from there.
*/
static bool
is_position(float coordinate)
{
	return isfinite(coordinate) && fabsf(coordinate) < 100000;
}


ScreenSettings::ScreenSettings()
{
	fWindowFrame.Set(0, 0, 640, 400);
	BPoint offset;

	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK) {
		path.Append(kSettingsFileName);

		BFile file(path.Path(), B_READ_ONLY);
		off_t size;
		if (file.InitCheck() == B_OK && file.GetSize(&size) == B_OK) {
			BRect frame;
			if (size >= (off_t)sizeof(BRect)
				&& file.Read(&frame, sizeof(BRect)) == sizeof(BRect)
				&& frame.IsValid() && is_position(frame.left)
				&& is_position(frame.top) && is_position(frame.right)
				&& is_position(frame.bottom)) {
				// the complete frame, as written by newer versions
				fWindowFrame = frame;
				return;
			}
			file.Seek(0, SEEK_SET);
			if (file.Read(&offset, sizeof(BPoint)) != sizeof(BPoint)
				|| !is_position(offset.x) || !is_position(offset.y))
				offset = BPoint();
		}
	}

	fWindowFrame.OffsetBy(offset);
}


ScreenSettings::~ScreenSettings()
{
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) < B_OK)
		return;

	path.Append(kSettingsFileName);

	BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	if (file.InitCheck() == B_OK)
		file.Write(&fWindowFrame, sizeof(BRect));
}


void
ScreenSettings::SetWindowFrame(BRect frame)
{
	fWindowFrame = frame;
}
