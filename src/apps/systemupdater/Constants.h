/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SYSTEM_UPDATER_CONSTANTS_H
#define SYSTEM_UPDATER_CONSTANTS_H


#include <SupportDefs.h>


#define kAppSignature			"application/x-vnd.airos-SystemUpdater"

// the system packages and the package daemon's administrative directory
#define kPackagesDirectory		"/boot/system/packages"
#define kAdminDirectory			kPackagesDirectory "/administrative"
#define kActivationFile			"activated-packages"

// System Updater's own files (see docs/airos/SYSTEM-UPDATER.md); the boot
// control files are named in <airos_boot_control.h>
#define kStateDirectory			kAdminDirectory "/airos"
#define kReleasesDirectory		kStateDirectory "/releases"
#define kDownloadsDirectory		kStateDirectory "/downloads"
#define kLogFile				kStateDirectory "/log"
#define kCheckFile				kStateDirectory "/last-check"
#define kFailedFile				kStateDirectory "/failed"
	// the versions of releases that did not start, one per line

// what the airos_release and airos_payload packages install
#define kReleaseFile			"/boot/system/data/airos/release"
#define kPayloadDirectory		"/boot/system/data/airos/non-packaged"
#define kNonPackagedDirectory	"/boot/system/non-packaged"
#define kEfiLoaderFile			"/boot/system/data/platform_loaders/" \
									"haiku_loader.efi"

#define kReleasePackageName		"airos_release"
#define kPayloadPackageName		"airos_payload"

// where an installation without an airos_release package looks
#define kDefaultFeedBase		"http://192.168.1.200/updates/"

// the user's settings
#define kSettingsFileName		"SystemUpdater"

// versions kept besides the current one
static const int32 kDefaultVersionsToKeep = 2;

// starts an update may take to reach the desktop before the boot loader
// falls back to the version before it
static const int32 kTrialAttempts = 2;

// how long the desktop must have run before an update counts as working
static const bigtime_t kConfirmDelay = 30000000;


#endif	// SYSTEM_UPDATER_CONSTANTS_H
