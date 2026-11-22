/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TABLET_MODE_SETTINGS_H
#define _TABLET_MODE_SETTINGS_H


/*	What the desktop does when a 2-in-1 laptop is folded into a stand, a
	tent or a tablet (see <accelerometer.h>): the settings tablet_server
	follows and the Screen preferences set. Kept as a driver settings file:

		upright_rotation	true	# turn the picture the right way up
		tablet_view			desktop	# or "mobile": air/OS Mobile
*/


#include <stdio.h>
#include <string.h>

#include <driver_settings.h>
#include <File.h>
#include <FindDirectory.h>
#include <Path.h>
#include <String.h>


namespace BPrivate {


#define TABLET_SERVER_SIGNATURE		"application/x-vnd.airOS-tablet_server"
#define MOBILE_SHELL_SIGNATURE		"application/x-vnd.airOS-MobileShell"
#define MOBILE_SHELL_PATH			"/boot/system/apps/MobileShell"

// to tablet_server: the settings file changed
static const uint32 kMsgTabletSettingsChanged = 'TMsc';


enum tablet_view {
	TABLET_VIEW_DESKTOP = 0,	// the desktop as it is
	TABLET_VIEW_MOBILE			// air/OS Mobile, the touch shell
};


struct TabletModeSettings {
	bool		uprightRotation;
	tablet_view	view;

	TabletModeSettings()
		:
		uprightRotation(true),
		view(TABLET_VIEW_DESKTOP)
	{
	}

	static status_t Path(BPath& path)
	{
		status_t status = find_directory(B_USER_SETTINGS_DIRECTORY, &path);
		if (status == B_OK)
			status = path.Append("tablet_mode");
		return status;
	}

	void Load()
	{
		*this = TabletModeSettings();
		BPath path;
		if (Path(path) != B_OK)
			return;
		void* handle = load_driver_settings(path.Path());
		if (handle == NULL)
			return;
		uprightRotation = get_driver_boolean_parameter(handle,
			"upright_rotation", true, true);
		const char* value = get_driver_parameter(handle, "tablet_view", NULL,
			NULL);
		if (value != NULL && strcmp(value, "mobile") == 0)
			view = TABLET_VIEW_MOBILE;
		unload_driver_settings(handle);
	}

	status_t Save() const
	{
		BPath path;
		status_t status = Path(path);
		if (status != B_OK)
			return status;
		BFile file(path.Path(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		status = file.InitCheck();
		if (status != B_OK)
			return status;
		BString text;
		text << "# What happens when this machine is folded into a tablet\n"
			<< "# (the Screen preferences set this)\n"
			<< "upright_rotation " << (uprightRotation ? "true" : "false")
			<< "\ntablet_view "
			<< (view == TABLET_VIEW_MOBILE ? "mobile" : "desktop") << "\n";
		ssize_t written = file.Write(text.String(), text.Length());
		return written == text.Length() ? B_OK
			: written < 0 ? (status_t)written : B_IO_ERROR;
	}
};


}	// namespace BPrivate


using BPrivate::TabletModeSettings;


#endif	// _TABLET_MODE_SETTINGS_H
