/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	A device behind a USB hub must enumerate.

	airos-ci's smoke test boots its images in QEMU with a QEMU usb-hub (a full
	speed hub, 0409:55aa) on the xHCI controller and a mouse on the hub's
	first port. The xHCI driver used to build the route string of a device
	behind a full speed hub from the port of the hub's transaction
	translator (the hub's own root port) instead of the device's port on the
	hub, and Address Device failed (TRB error), so nothing behind such a hub
	ever worked.

	Looks for that hub under /dev/bus/usb and for a device behind it. Without
	such a hub (a boot without one) the test prints SKIP and passes. Ends
	within a minute either way.
*/


#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <OS.h>
#include <USBKit.h>


static const uint16 kQemuHubVendor = 0x0409;
static const uint16 kQemuHubProduct = 0x55aa;

// how long a hub may take to show up after the boot, and its device
static const bigtime_t kHubDeadline = 20000000;
static const bigtime_t kChildDeadline = 50000000;


struct scan_result {
	char	hub[B_PATH_NAME_LENGTH];
	char	child[B_PATH_NAME_LENGTH];
	uint16	childVendor;
	uint16	childProduct;
};


static bool
is_qemu_hub(const char* path)
{
	BUSBDevice device(path);
	return device.InitCheck() == B_OK && device.VendorID() == kQemuHubVendor
		&& device.ProductID() == kQemuHubProduct;
}


//!	Walks the directory of a hub (or a bus); fills in result when it finds
//	the QEMU hub, and a device behind it.
static void
scan(const char* directory, scan_result& result)
{
	char hubPath[B_PATH_NAME_LENGTH];
	snprintf(hubPath, sizeof(hubPath), "%s/hub", directory);
	struct stat st;
	bool qemuHub = stat(hubPath, &st) == 0 && is_qemu_hub(hubPath);
	if (qemuHub && result.hub[0] == '\0')
		strlcpy(result.hub, hubPath, sizeof(result.hub));

	DIR* dir = opendir(directory);
	if (dir == NULL)
		return;
	while (dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0
			|| strcmp(entry->d_name, "hub") == 0) {
			continue;
		}
		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		if (stat(path, &st) != 0)
			continue;

		if (qemuHub && result.child[0] == '\0') {
			// whatever sits on one of its ports: a device or another hub
			char devicePath[B_PATH_NAME_LENGTH + 8];
			if (S_ISDIR(st.st_mode))
				snprintf(devicePath, sizeof(devicePath), "%s/hub", path);
			else
				strlcpy(devicePath, path, sizeof(devicePath));
			BUSBDevice device(devicePath);
			if (device.InitCheck() == B_OK) {
				strlcpy(result.child, devicePath, sizeof(result.child));
				result.childVendor = device.VendorID();
				result.childProduct = device.ProductID();
			}
		}
		if (S_ISDIR(st.st_mode))
			scan(path, result);
	}
	closedir(dir);
}


int
main()
{
	bigtime_t start = system_time();
	scan_result result;
	while (true) {
		memset(&result, 0, sizeof(result));
		scan("/dev/bus/usb", result);
		bigtime_t waited = system_time() - start;
		if (result.child[0] != '\0')
			break;
		if (result.hub[0] == '\0' && waited > kHubDeadline) {
			printf("SKIP: no QEMU usb-hub (%04x:%04x) under /dev/bus/usb\n",
				kQemuHubVendor, kQemuHubProduct);
			return 0;
		}
		if (waited > kChildDeadline) {
			printf("FAIL: no device enumerated behind the hub %s\n",
				result.hub);
			return 1;
		}
		snooze(1000000);
	}

	printf("PASS: %s behind the hub %s (%04x:%04x)\n", result.child,
		result.hub, result.childVendor, result.childProduct);
	return 0;
}
