/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	aicload: the AIC8800D80's firmware loader from userland (usb_raw), for
	the lab. It runs the kernel driver's loader core against a chip in ROM
	mode, and can probe or reboot a chip that runs the firmware.

		aicload [firmware directory]	0x8d80: load and start the firmware
		aicload --probe [--data-in] [--stack-start]
										0x8d81: chip ID, firmware version and
										MAC address over the message pipes
		aicload --reboot				0x8d81: back to the ROM (0x8d80)
		aicload --list					print the interfaces and endpoints

	usb_raw allows one transfer per device and a read can only be ended by
	a deadly signal, which is enough for a strict request/confirm protocol;
	a watchdog ends the team after 10 s without progress. */


#include <OS.h>
#include <USBKit.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aic_loader.h"


#define MM_SET_STACK_START_REQ	123
#define MM_SET_STACK_START_CFM	124
#define MM_GET_MAC_ADDR_REQ		115
#define MM_GET_MAC_ADDR_CFM		116
#define MM_GET_FW_VERSION_REQ	128
#define MM_GET_FW_VERSION_CFM	129


static const char* sFirmwareDirectory
	= "/boot/system/non-packaged/data/firmware/aic8800wifi";
static volatile bigtime_t sLastProgress;


struct Pipes {
	const BUSBEndpoint*	in;
	const BUSBEndpoint*	out;
};


static int
bulk_out(void* cookie, const uint8_t* buffer, size_t length)
{
	Pipes* pipes = (Pipes*)cookie;
	ssize_t written = pipes->out->BulkTransfer((void*)buffer, length);
	sLastProgress = system_time();
	return written == (ssize_t)length ? 0 : -1;
}


static int
bulk_in(void* cookie, uint8_t* buffer, size_t capacity, int /*timeoutMs*/)
{
	Pipes* pipes = (Pipes*)cookie;
	ssize_t read = pipes->in->BulkTransfer(buffer, capacity);
	sLastProgress = system_time();
	return read < 0 ? -1 : (int)read;
}


static long
load_file(void*, const char* name, uint8_t** _data)
{
	char path[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "%s/%s", sFirmwareDirectory, name);
	FILE* file = fopen(path, "rb");
	if (file == NULL)
		return -1;

	fseek(file, 0, SEEK_END);
	long size = ftell(file);
	fseek(file, 0, SEEK_SET);
	*_data = (uint8_t*)calloc(1, size + 1024);
	if (*_data == NULL || fread(*_data, 1, size, file) != (size_t)size)
		size = -1;
	fclose(file);
	return size;
}


static void
release_file(void*, uint8_t* data)
{
	free(data);
}


static void
log_line(void*, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
	fflush(stdout);
}


static void
sleep_ms(void*, int milliseconds)
{
	snooze(milliseconds * 1000LL);
}


static status_t
watchdog(void*)
{
	while (true) {
		snooze(500000);
		if (system_time() - sLastProgress > 10000000) {
			printf("aicload: no USB progress for 10 s, giving up\n");
			exit(1);
		}
	}
	return B_OK;
}


class Roster : public BUSBRoster {
public:
	BUSBDevice*		fDevice = NULL;
	uint16			fProduct = 0;

	status_t DeviceAdded(BUSBDevice* device)
	{
		if (device->VendorID() != AIC_USB_VENDOR
			|| (device->ProductID() != AIC_USB_PRODUCT_ROM
				&& device->ProductID() != AIC_USB_PRODUCT_FIRMWARE))
			return B_ERROR;

		fProduct = device->ProductID();
		fDevice = device;
		printf("aicload: %04x:%04x at %s\n", device->VendorID(),
			device->ProductID(), device->Location());
		return B_OK;
	}

	void DeviceRemoved(BUSBDevice* device)
	{
		if (device == fDevice) {
			printf("aicload: %04x left the bus (%" B_PRIdBIGTIME " ms)\n",
				fProduct, system_time() / 1000);
			fDevice = NULL;
		}
	}
};


/*	In ROM mode the vendor interface (ff/ff/ff) has one bulk IN/OUT pair.
	With the firmware running it is one interface of three, with data on the
	first bulk IN/OUT pair and messages on the second. */
static bool
find_pipes(BUSBDevice* device, Pipes& data, Pipes& message, bool print)
{
	const BUSBConfiguration* configuration = device->ActiveConfiguration();
	if (configuration == NULL)
		return false;

	if (print) {
		printf("aicload: device class %02x/%02x/%02x, %" B_PRIu32
			" interfaces\n", device->Class(), device->Subclass(),
			device->Protocol(), configuration->CountInterfaces());
	}

	const BUSBInterface* wlan = NULL;
	for (uint32 i = 0; i < configuration->CountInterfaces(); i++) {
		const BUSBInterface* interface = configuration->InterfaceAt(i);
		if (print) {
			printf("  interface %" B_PRIu32 ": class %02x/%02x/%02x, %"
				B_PRIu32 " alternates, %" B_PRIu32 " endpoints\n", i,
				interface->Class(), interface->Subclass(),
				interface->Protocol(), interface->CountAlternates(),
				interface->CountEndpoints());
			for (uint32 e = 0; e < interface->CountEndpoints(); e++) {
				const BUSBEndpoint* endpoint = interface->EndpointAt(e);
				printf("    endpoint %#04x %s %s, max packet %u\n",
					endpoint->Descriptor()->endpoint_address,
					endpoint->IsBulk() ? "bulk"
						: endpoint->IsInterrupt() ? "interrupt"
						: endpoint->IsIsochronous() ? "isochronous"
						: "control",
					endpoint->IsInput() ? "in" : "out",
					endpoint->MaxPacketSize());
			}
		}
		if (wlan == NULL && interface->Class() == 0xff
			&& interface->Subclass() == 0xff)
			wlan = interface;
	}
	if (wlan == NULL)
		return false;

	data.in = data.out = message.in = message.out = NULL;
	for (uint32 i = 0; i < wlan->CountEndpoints(); i++) {
		const BUSBEndpoint* endpoint = wlan->EndpointAt(i);
		if (!endpoint->IsBulk())
			continue;
		const BUSBEndpoint** first = endpoint->IsInput() ? &data.in : &data.out;
		const BUSBEndpoint** second
			= endpoint->IsInput() ? &message.in : &message.out;
		if (*first == NULL)
			*first = endpoint;
		else if (*second == NULL)
			*second = endpoint;
	}
	if (message.in == NULL)
		message.in = data.in;
	if (message.out == NULL)
		message.out = data.out;
	return data.in != NULL && data.out != NULL;
}


static int
probe_firmware(const aic_loader_ops& ops, Pipes* pipes, bool stackStart)
{
	uint8_t confirm[64];
	uint8_t address[4] = { 0x00, 0x00, 0x50, 0x40 };
	int status = aic_request(&ops, pipes, AIC_DBG_MEM_READ_REQ, AIC_TASK_DBG,
		address, sizeof(address), AIC_DBG_MEM_READ_CFM, confirm, 8);
	if (status != 0) {
		printf("aicload: DBG_MEM_READ failed (%d)\n", status);
		return status;
	}
	printf("aicload: [0x40500000] = %02x%02x%02x%02x (chip ID %#x)\n",
		confirm[7], confirm[6], confirm[5], confirm[4], confirm[6]);

	if (stackStart) {
		uint8_t start[4] = { 1, 0, 0x20, 0 };
		status = aic_request(&ops, pipes, MM_SET_STACK_START_REQ, AIC_TASK_MM,
			start, sizeof(start), MM_SET_STACK_START_CFM, confirm, 2);
		printf("aicload: MM_SET_STACK_START: %d (5 GHz %u, vendor info "
			"%#x)\n", status, confirm[0], confirm[1]);
	}

	uint8_t zero = 0;
	memset(confirm, 0, sizeof(confirm));
	status = aic_request(&ops, pipes, MM_GET_FW_VERSION_REQ, AIC_TASK_MM,
		&zero, 1, MM_GET_FW_VERSION_CFM, confirm, sizeof(confirm));
	if (status == 0) {
		printf("aicload: firmware \"%.*s\"\n", confirm[0] < 63 ? confirm[0] : 63,
			(const char*)confirm + 1);
	} else
		printf("aicload: MM_GET_FW_VERSION failed (%d)\n", status);

	uint8_t get[4] = { 1, 0, 0, 0 };
	status = aic_request(&ops, pipes, MM_GET_MAC_ADDR_REQ, AIC_TASK_MM, get,
		sizeof(get), MM_GET_MAC_ADDR_CFM, confirm, 6);
	if (status == 0) {
		printf("aicload: MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
			confirm[0], confirm[1], confirm[2], confirm[3], confirm[4],
			confirm[5]);
	} else
		printf("aicload: MM_GET_MAC_ADDR failed (%d)\n", status);
	return status;
}


int
main(int argc, char** argv)
{
	bool probe = false, reboot = false, list = false, dataIn = false;
	bool stackStart = false;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--probe") == 0)
			probe = true;
		else if (strcmp(argv[i], "--reboot") == 0)
			reboot = true;
		else if (strcmp(argv[i], "--list") == 0)
			list = true;
		else if (strcmp(argv[i], "--data-in") == 0)
			dataIn = true;
		else if (strcmp(argv[i], "--stack-start") == 0)
			stackStart = true;
		else
			sFirmwareDirectory = argv[i];
	}

	Roster roster;
	roster.Start();
	for (int i = 0; i < 20 && roster.fDevice == NULL; i++)
		snooze(100000);
	if (roster.fDevice == NULL) {
		printf("aicload: no a69c:8d80 or a69c:8d81 device\n");
		return 1;
	}
	BUSBDevice* device = roster.fDevice;

	Pipes data, message;
	if (!find_pipes(device, data, message, true)) {
		printf("aicload: no vendor interface with bulk pipes\n");
		return 1;
	}
	if (list) {
		roster.Stop();
		return 0;
	}

	aic_loader_ops ops = { bulk_out, bulk_in, load_file, release_file,
		log_line, sleep_ms };

	sLastProgress = system_time();
	resume_thread(spawn_thread(watchdog, "aicload watchdog",
		B_NORMAL_PRIORITY, NULL));

	if (roster.fProduct == AIC_USB_PRODUCT_FIRMWARE) {
		if (reboot) {
			printf("aicload: asking the firmware to reboot (%" B_PRIdBIGTIME
				" ms)\n", system_time() / 1000);
			aic_send_reboot(&ops, &message);
			for (int i = 0; i < 50 && roster.fDevice != NULL; i++) {
				snooze(100000);
				sLastProgress = system_time();
			}
			roster.Stop();
			return 0;
		}
		Pipes probePipes = { dataIn ? data.in : message.in, message.out };
		int status = probe_firmware(ops, &probePipes, stackStart);
		roster.Stop();
		return status == 0 ? 0 : 1;
	}
	if (probe || reboot) {
		printf("aicload: the chip is in ROM mode (8d80); load it first\n");
		return 1;
	}

	bigtime_t start = system_time();
	aic_loader_result result;
	int status = aic_load_firmware_d80(&ops, &data, &result);
	printf("aicload: %s after %" B_PRIdBIGTIME " ms (at %" B_PRIdBIGTIME
		" ms), %u messages, chip %#x (register %#010x), %s %#010x\n",
		status == 0 ? "started" : "FAILED", (system_time() - start) / 1000,
		system_time() / 1000, result.messages, result.chip_id,
		result.chip_register,
		result.firmware_name != NULL ? result.firmware_name : "-",
		result.firmware_version);

	// the chip leaves the bus and comes back as 0x8d81
	for (int i = 0; i < 50 && roster.fDevice != NULL; i++) {
		snooze(100000);
		sLastProgress = system_time();
	}
	roster.Stop();
	return status == 0 ? 0 : 1;
}
