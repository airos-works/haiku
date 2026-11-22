/*
 * Copyright 2026, air/OS.
 * Copyright 2026 Renku Mobile contributors.
 * Distributed under the terms of the MIT License.
 */


#include "AppList.h"

#include "Theme.h"

#include <MobileKit.h>

#include <AppFileInfo.h>
#include <Bitmap.h>
#include <Directory.h>
#include <File.h>
#include <LocaleRoster.h>
#include <NodeInfo.h>
#include <Roster.h>

#include <algorithm>
#include <string.h>


// The favourites are a fixed list for now: whichever of these are installed,
// in this order. A settings file comes with a way to edit the home screen.
static const char* kDefaultDock[] = {
	"application/x-vnd.Kunanyi-Summit",
	"application/x-vnd.air-Mail",
	"application/x-vnd.Be-MAIL",
	"application/x-vnd.Haiku-Terminal",
	"application/x-vnd.airOS-airTime",
	NULL
};

static const char* kDefaultGrid[] = {
	"application/x-vnd.storytold-WordCraft",
	"application/x-vnd.storytold-GridCraft",
	"application/x-vnd.storytold-PhotoCraft",
	"application/x-vnd.storytold-PdfCraft",
	"application/x-vnd.Amp",
	"application/x-vnd.Haiku-StyledEdit",
	"application/x-vnd.Haiku-ShowImage",
	"application/x-vnd.Be-PEPL",
	"application/x-vnd.Haiku-DeskCalc",
	"application/x-vnd.airOS-airShot",
	"application/x-vnd.Haiku-SoundRecorder",
	"application/x-vnd.Haiku-ActivityMonitor",
	"application/x-vnd.airos-SystemUpdater",
	"application/x-vnd.Haiku-Screen",
	"application/x-vnd.Haiku-Network",
	"application/x-vnd.Haiku-Appearance",
	NULL
};

// What makes no sense in the shell: what it stands in for, what lives in the
// Deskbar's tray, and what fights it for the screen.
static const char* kHidden[] = {
	"application/x-vnd.Be-TRAK",
	"application/x-vnd.Be-TSKB",
	"application/x-vnd.Haiku-Installer",
	"application/x-vnd.Haiku-LaunchBox",
	"application/x-vnd.Haiku-BootManager",
	"application/x-vnd.Be-WORK",
	"application/x-vnd.Haiku-NetworkStatus",
	"application/x-vnd.Haiku-PowerStatus",
	"application/x-vnd.Haiku-ProcessController",
	"application/x-vnd.Haiku-BluetoothStatus",
	"application/x-vnd.Haiku-WiFiStatus",
	"application/x-vnd.mmu.AutoRaise",
	"application/x-vnd.Haiku-DeskbarPreferences",
	MOBILE_SHELL_SIGNATURE,
	NULL
};


static bool
is_hidden(const char* signature)
{
	for (int32 i = 0; kHidden[i] != NULL; i++) {
		if (strcasecmp(kHidden[i], signature) == 0)
			return true;
	}
	return false;
}


AppList::AppList()
{
}


AppList::~AppList()
{
	_Clear();
}


void
AppList::Scan()
{
	_Clear();

	_ScanDirectory("/boot/system/apps", false);
	_ScanDirectory("/boot/system/preferences", true);
	_ScanDirectory("/boot/system/non-packaged/apps", false);
	_ScanDirectory("/boot/home/config/apps", false);
	_ScanDirectory("/boot/home/config/non-packaged/apps", false);

	std::sort(fApps.begin(), fApps.end(), [](AppEntry* a, AppEntry* b) {
		return a->name.ICompare(b->name) < 0;
	});

	_LoadFavorites();
}


AppEntry*
AppList::FindBySignature(const char* signature) const
{
	for (AppEntry* app : fApps) {
		if (app->signature.ICompare(signature) == 0)
			return app;
	}
	return NULL;
}


void
AppList::_ScanDirectory(const char* path, bool preference)
{
	BDirectory directory(path);
	if (directory.InitCheck() != B_OK)
		return;

	BEntry entry;
	while (directory.GetNextEntry(&entry, true) == B_OK) {
		if (entry.IsDirectory())
			continue;

		BFile file(&entry, B_READ_ONLY);
		BAppFileInfo info(&file);
		char signature[B_MIME_TYPE_LENGTH];
		if (info.InitCheck() != B_OK || info.GetSignature(signature) != B_OK)
			continue;

		uint32 flags = 0;
		if (info.GetAppFlags(&flags) == B_OK && (flags & B_BACKGROUND_APP) != 0)
			continue;
		if (is_hidden(signature) || FindBySignature(signature) != NULL)
			continue;

		entry_ref ref;
		if (entry.GetRef(&ref) != B_OK || !BMobileKit::SupportsMobileView(ref))
			continue;

		AppEntry* app = new AppEntry;
		app->ref = ref;
		app->signature = signature;
		app->preference = preference;

		if (BLocaleRoster::Default()->GetLocalizedFileName(app->name, app->ref,
				false) != B_OK) {
			app->name = app->ref.name;
		}

		int32 size = (int32)Theme::IconSize();
		app->icon = new BBitmap(BRect(0, 0, size - 1, size - 1), B_RGBA32);
		if (BNodeInfo::GetTrackerIcon(&app->ref, app->icon, (icon_size)size)
				!= B_OK) {
			delete app->icon;
			app->icon = NULL;
		}

		fApps.push_back(app);
	}
}


void
AppList::_LoadFavorites()
{
	for (int32 i = 0; kDefaultDock[i] != NULL; i++) {
		if (AppEntry* app = FindBySignature(kDefaultDock[i]))
			fDock.push_back(app);
	}
	for (int32 i = 0; kDefaultGrid[i] != NULL; i++) {
		if (AppEntry* app = FindBySignature(kDefaultGrid[i]))
			fFavorites.push_back(app);
	}
}


void
AppList::_Clear()
{
	for (AppEntry* app : fApps) {
		delete app->icon;
		delete app;
	}
	fApps.clear();
	fFavorites.clear();
	fDock.clear();
}
