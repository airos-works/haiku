/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * bt_firmware brings USB Bluetooth controllers that need a firmware download
 * into operational mode before bluetooth_server opens them through
 * h2generic. It handles Intel (Wireless 7260 to Wi-Fi 7 BE200) and Realtek
 * (RTL8723 to RTL8922) controllers; MediaTek MT7921/MT7922 radios are set
 * up by h2generic itself. Controllers that need nothing are left alone.
 */


#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>

#include "BluetoothFirmware.h"
#include "IntelBluetoothFirmware.h"
#include "RealtekBluetoothFirmware.h"
#include "usb_raw.h"


bool gVerbose = false;
bool gControllerReset = false;


enum Vendor {
	kVendorNone = 0,
	kVendorIntel,
	kVendorRealtek
};


static bool
IsBluetoothClass(uint8_t deviceClass, uint8_t subclass, uint8_t protocol)
{
	return deviceClass == 0xe0 && subclass == 0x01 && protocol == 0x01;
}


static Vendor
IdentifyDevice(int fd, const IntelBluetooth::UsbDevice** intelDevice = NULL,
	uint16_t* vendorId = NULL, uint16_t* productId = NULL)
{
	usb_device_descriptor descriptor;
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.device.descriptor = &descriptor;
	if (ioctl(fd, B_USB_RAW_COMMAND_GET_DEVICE_DESCRIPTOR, &command,
			sizeof(command)) < 0
		|| command.device.status != B_USB_RAW_STATUS_SUCCESS) {
		return kVendorNone;
	}
	if (vendorId != NULL)
		*vendorId = descriptor.vendor_id;
	if (productId != NULL)
		*productId = descriptor.product_id;

	// Future parts keep the Bluetooth class on the device or on its first
	// interface; the controller's own version reply decides the rest.
	bool bluetoothClass = IsBluetoothClass(descriptor.device_class,
		descriptor.device_subclass, descriptor.device_protocol);
	if (!bluetoothClass) {
		usb_interface_descriptor interface;
		memset(&command, 0, sizeof(command));
		command.interface.descriptor = &interface;
		command.interface.config_index = 0;
		command.interface.interface_index = 0;
		bluetoothClass = ioctl(fd, B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR,
				&command, sizeof(command)) >= 0
			&& command.interface.status == B_USB_RAW_STATUS_SUCCESS
			&& IsBluetoothClass(interface.interface_class,
				interface.interface_subclass, interface.interface_protocol);
	}

	const IntelBluetooth::UsbDevice* intel = IntelBluetooth::FindUsbDevice(
		descriptor.vendor_id, descriptor.product_id);
	if (intelDevice != NULL)
		*intelDevice = intel;
	if (intel != NULL || (descriptor.vendor_id == IntelBluetooth::kVendorIntel
			&& bluetoothClass)) {
		return kVendorIntel;
	}
	if (RealtekBluetooth::IsListedUsbDevice(descriptor.vendor_id,
			descriptor.product_id)
		|| (descriptor.vendor_id == RealtekBluetooth::kVendorRealtek
			&& bluetoothClass)) {
		return kVendorRealtek;
	}
	return kVendorNone;
}


static void
FindControllers(const char* directoryPath, std::vector<std::string>& found,
	unsigned depth = 0)
{
	if (depth > 6)
		return;
	DIR* directory = opendir(directoryPath);
	if (directory == NULL)
		return;
	struct dirent* entry;
	while ((entry = readdir(directory)) != NULL) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0
			|| strcmp(entry->d_name, "hub") == 0) {
			continue;
		}
		std::string path = std::string(directoryPath) + "/" + entry->d_name;
		struct stat info;
		if (stat(path.c_str(), &info) != 0)
			continue;
		if (S_ISDIR(info.st_mode)) {
			FindControllers(path.c_str(), found, depth + 1);
			continue;
		}
		const int fd = open(path.c_str(), O_RDWR);
		if (fd < 0)
			continue;
		const Vendor vendor = IdentifyDevice(fd);
		close(fd);
		if (vendor != kVendorNone)
			found.push_back(path);
	}
	closedir(directory);
}


static const char*
DescribeDevice(int fd)
{
	static char description[96];
	const IntelBluetooth::UsbDevice* intel = NULL;
	uint16_t vendor = 0, product = 0;
	switch (IdentifyDevice(fd, &intel, &vendor, &product)) {
		case kVendorIntel:
			snprintf(description, sizeof(description),
				"%04x:%04x Intel %s Bluetooth", vendor, product,
				intel != NULL ? intel->name : "(unlisted)");
			break;
		case kVendorRealtek:
			snprintf(description, sizeof(description),
				"%04x:%04x Realtek Bluetooth", vendor, product);
			break;
		default:
			snprintf(description, sizeof(description),
				"%04x:%04x not handled", vendor, product);
			break;
	}
	return description;
}


static int
SetupDevice(const char* path, bool infoOnly)
{
	const int fd = open(path, O_RDWR);
	if (fd < 0) {
		ERROR("cannot open %s: %s\n", path, strerror(errno));
		return 1;
	}
	LOG("%s: %s\n", path, DescribeDevice(fd));
	const IntelBluetooth::UsbDevice* intel = NULL;
	const Vendor vendor = IdentifyDevice(fd, &intel);
	if (vendor == kVendorNone) {
		ERROR("%s needs no firmware from this tool\n", path);
		return 1;
	}

	Controller* controller = new Controller(fd, path);
	bool ok = controller->Start();
	if (ok) {
		ok = vendor == kVendorIntel
			? SetupIntel(*controller, intel, infoOnly)
			: SetupRealtek(*controller, infoOnly);
	}
	if (ok && !infoOnly)
		LOG("%s: ready\n", path);
	if (!ok && gControllerReset)
		return kExitRetryAfterReset;
	return ok ? 0 : 1;
}


/*!	Each controller gets its own process: a transfer that never completes
	can only be ended by the watchdog signal terminating it.
*/
static int
RunSetup(const std::string& path, bool info)
{
	fflush(stdout);
	fflush(stderr);
	const pid_t child = fork();
	if (child == 0) {
		const int result = SetupDevice(path.c_str(), info);
		fflush(stdout);
		fflush(stderr);
		_exit(result);
	}
	int status = 0;
	if (child < 0 || waitpid(child, &status, 0) != child)
		return 1;
	if (WIFSIGNALED(status)) {
		ERROR("%s: %s\n", path.c_str(),
			WTERMSIG(status) == SIGALRM
				? "the controller stopped answering (timeout)"
				: "setup crashed");
		return 1;
	}
	return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}


static void
PrintSupported()
{
	for (size_t i = 0; i < IntelBluetooth::CountUsbDevices(); i++) {
		const IntelBluetooth::UsbDevice* device
			= IntelBluetooth::UsbDeviceAt(i);
		printf("%04x:%04x  Intel %s\n", IntelBluetooth::kVendorIntel,
			device->product, device->name);
	}
	printf("%04x:*     Intel, any Bluetooth class device\n",
		IntelBluetooth::kVendorIntel);
	for (size_t i = 0; i < RealtekBluetooth::CountListedUsbDevices(); i++) {
		const RealtekBluetooth::UsbId* device
			= RealtekBluetooth::ListedUsbDeviceAt(i);
		printf("%04x:%04x  Realtek module\n", device->vendor,
			device->product);
	}
	printf("%04x:*     Realtek, any Bluetooth class device\n",
		RealtekBluetooth::kVendorRealtek);
	for (size_t i = 0; i < RealtekBluetooth::CountIcs(); i++) {
		const RealtekBluetooth::IcInfo* ic = RealtekBluetooth::IcAt(i);
		printf("Realtek chip %s (LMP subversion %04x, HCI revision %x): "
			"rtl_bt/%s.bin\n", ic->name, ic->lmpSubversion, ic->hciRevision,
			ic->firmware);
	}
}


static void
Usage(const char* name)
{
	fprintf(stderr,
		"usage: %s [options] [USB_RAW_DEVICE...]\n"
		"Loads firmware into Intel and Realtek USB Bluetooth controllers.\n\n"
		"  --present            exit 0 if such a controller is attached\n"
		"  --list               list attached controllers\n"
		"  --info               show state and firmware choice, change "
			"nothing\n"
		"  --firmware-dir DIR   search DIR/<vendor>/ before the standard\n"
		"                       <data>/firmware/{intel,rtl_bt} directories\n"
		"  --verbose            trace transport details\n"
		"  --supported          print the devices this tool handles\n",
		name);
}


int
main(int argc, char** argv)
{
	bool present = false, list = false, info = false;
	std::vector<std::string> devices;
	std::vector<std::string> extraDirectories;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--present") == 0)
			present = true;
		else if (strcmp(argv[i], "--list") == 0)
			list = true;
		else if (strcmp(argv[i], "--info") == 0)
			info = true;
		else if (strcmp(argv[i], "--verbose") == 0)
			gVerbose = true;
		else if (strcmp(argv[i], "--firmware-dir") == 0 && i + 1 < argc)
			extraDirectories.push_back(argv[++i]);
		else if (strcmp(argv[i], "--supported") == 0) {
			PrintSupported();
			return 0;
		} else if (argv[i][0] == '-') {
			Usage(argv[0]);
			return 2;
		} else
			devices.push_back(argv[i]);
	}

	InitFirmwareDirectories(extraDirectories);

	if (devices.empty())
		FindControllers("/dev/bus/usb", devices);
	if (present)
		return devices.empty() ? 1 : 0;
	if (devices.empty()) {
		ERROR("no Intel or Realtek Bluetooth controller found\n");
		return 3;
	}
	if (list) {
		for (size_t i = 0; i < devices.size(); i++) {
			const int fd = open(devices[i].c_str(), O_RDWR);
			printf("%s  %s\n", devices[i].c_str(),
				fd >= 0 ? DescribeDevice(fd) : strerror(errno));
			if (fd >= 0)
				close(fd);
		}
		return 0;
	}

	int failures = 0;
	std::vector<std::string> ready;
	bool retry = false;
	for (size_t i = 0; i < devices.size(); i++) {
		const int result = RunSetup(devices[i], info);
		if (result == 0)
			ready.push_back(devices[i]);
		else if (result == kExitRetryAfterReset)
			retry = true;
		else
			failures++;
	}

	if (retry) {
		// A controller reset after a failed download comes back as a new USB
		// device (with a new path, perhaps): give it one more try.
		std::vector<std::string> again;
		for (int attempt = 0; attempt < 20; attempt++) {
			usleep(500000);
			again.clear();
			std::vector<std::string> found;
			FindControllers("/dev/bus/usb", found);
			for (size_t i = 0; i < found.size(); i++) {
				if (std::find(ready.begin(), ready.end(), found[i])
						== ready.end()) {
					again.push_back(found[i]);
				}
			}
			if (!again.empty() && attempt >= 2)
				break;
		}
		if (again.empty()) {
			ERROR("the reset controller did not come back\n");
			failures++;
		}
		for (size_t i = 0; i < again.size(); i++) {
			LOG("%s: trying again after the reset\n", again[i].c_str());
			if (RunSetup(again[i], info) != 0)
				failures++;
		}
	}
	return failures == 0 ? 0 : 1;
}
