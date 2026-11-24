/*
 * Copyright 2026, the air/OS project.
 * Distributed under the terms of the MIT License.
 */


/*!	What the USB stack does with a device that misbehaves, or is just odd.

	The device is usbredir_fake_device.py (next to this file), plugged into
	a QEMU guest through QEMU's usb-redir; this program runs in the guest and
	talks to it through usb_raw. Without the device a case prints SKIP and
	passes; every wait has a deadline.

	usb_fake_device_test transfers
		device "vendor": bulk and interrupt transfers of many sizes, short
		packets among them, arrive whole and with the length the device sent
	usb_fake_device_test errors
		device "vendor": after a transfer that ended with an error (a stall,
		a transaction error, babble), the next transfer on the endpoint works
	usb_fake_device_test alt-gap
		device "alt-gap": the second alternate setting of an interface, which
		the device numbers 2, can be selected and its endpoints used
	usb_fake_device_test no-interfaces
		device "no-interfaces": a configuration with interface descriptors
		and an interface count of 0 leaves the system standing
	usb_fake_device_test iso
		any device with an isochronous endpoint (QEMU's usb-audio): transfers
		of no packets, and of fewer bytes than packets, are refused
	usb_fake_device_test mouse [x]
		device "mouse" (through usb_hid, the newest /dev/input/mouse/usb
		entry): reports keep coming for 4 s, also after reports that failed
		(option error=), and, if x is given, carry that X movement
*/


#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>
#include <USB3.h>

#include <keyboard_mouse_driver.h>
#include <usb_raw.h>


static const uint16 kVendor = 0x1209;
static const bigtime_t kDeadline = 5000000;

static int sFailures;
static int sDevice = -1;

#define CHECK(condition, message...) \
	do { \
		if (!(condition)) { \
			printf("FAIL: "); \
			printf(message); \
			printf("\n"); \
			sFailures++; \
		} \
	} while (0)


// #pragma mark - finding devices


static bool
get_device_descriptor(int fd, usb_device_descriptor& descriptor)
{
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.device.descriptor = &descriptor;
	return ioctl(fd, B_USB_RAW_COMMAND_GET_DEVICE_DESCRIPTOR, &command,
			sizeof(command)) == 0
		&& command.device.status == B_USB_RAW_STATUS_SUCCESS;
}


typedef bool (*device_filter)(int fd);
static uint16 sWantedProduct;


static bool
is_fake_device(int fd)
{
	usb_device_descriptor descriptor;
	return get_device_descriptor(fd, descriptor)
		&& descriptor.vendor_id == kVendor
		&& descriptor.product_id == sWantedProduct;
}


static bool
get_endpoint(int fd, uint32 interface, uint32 endpoint,
	usb_endpoint_descriptor& descriptor)
{
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.endpoint.descriptor = &descriptor;
	command.endpoint.config_index = 0;
	command.endpoint.interface_index = interface;
	command.endpoint.endpoint_index = endpoint;
	return ioctl(fd, B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR, &command,
			sizeof(command)) == 0
		&& command.endpoint.status == B_USB_RAW_STATUS_SUCCESS;
}


static uint32 sIsoInterface;
static uint32 sIsoEndpoint;


static bool
set_alternate(int fd, uint32 interface, uint32 alternate)
{
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.alternate.config_index = 0;
	command.alternate.interface_index = interface;
	command.alternate.alternate_info = alternate;
	return ioctl(fd, B_USB_RAW_COMMAND_SET_ALT_INTERFACE, &command,
			sizeof(command)) == 0
		&& command.alternate.status == B_USB_RAW_STATUS_SUCCESS;
}


static bool
has_isochronous_endpoint(int fd)
{
	// an audio streaming interface has its endpoint in an alternate setting
	for (uint32 interface = 0; interface < 8; interface++) {
		usb_raw_command command;
		memset(&command, 0, sizeof(command));
		command.alternate.config_index = 0;
		command.alternate.interface_index = interface;
		if (ioctl(fd, B_USB_RAW_COMMAND_GET_ALT_INTERFACE_COUNT, &command,
				sizeof(command)) != 0
			|| command.alternate.status != B_USB_RAW_STATUS_SUCCESS) {
			continue;
		}

		uint32 alternates = command.alternate.alternate_info;
		for (uint32 alternate = 0; alternate < alternates; alternate++) {
			usb_endpoint_descriptor descriptor;
			memset(&command, 0, sizeof(command));
			command.endpoint_etc.descriptor = &descriptor;
			command.endpoint_etc.config_index = 0;
			command.endpoint_etc.interface_index = interface;
			command.endpoint_etc.alternate_index = alternate;
			command.endpoint_etc.endpoint_index = 0;
			if (ioctl(fd, B_USB_RAW_COMMAND_GET_ENDPOINT_DESCRIPTOR_ETC,
					&command, sizeof(command)) != 0
				|| command.endpoint_etc.status != B_USB_RAW_STATUS_SUCCESS
				|| (descriptor.attributes & 0x03) != 1) {
				continue;
			}

			if (!set_alternate(fd, interface, alternate))
				continue;

			sIsoInterface = interface;
			sIsoEndpoint = 0;
			return true;
		}
	}

	return false;
}


static int
find_device(const char* directory, device_filter filter)
{
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return -1;

	int result = -1;
	while (dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.' || strcmp(entry->d_name, "raw") == 0)
			continue;

		char path[B_PATH_NAME_LENGTH];
		snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
		struct stat st;
		if (stat(path, &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			result = find_device(path, filter);
		} else {
			int fd = open(path, O_RDWR);
			if (fd >= 0 && filter(fd)) {
				printf("using %s\n", path);
				result = fd;
			} else if (fd >= 0)
				close(fd);
		}

		if (result >= 0)
			break;
	}

	closedir(dir);
	return result;
}


static bool
open_fake_device(uint16 product, const char* name)
{
	// it may have been plugged in just now
	sWantedProduct = product;
	for (int i = 0; i < 20; i++) {
		sDevice = find_device("/dev/bus/usb", is_fake_device);
		if (sDevice >= 0)
			return true;
		snooze(250000);
	}

	printf("SKIP: no fake \"%s\" device (%04x:%04x), see "
		"usbredir_fake_device.py\n", name, kVendor, product);
	return false;
}


// #pragma mark - transfers with a deadline


struct transfer {
	uint32		op;
	usb_raw_command command;
	int			result;
	sem_id		done;
};


static status_t
transfer_thread(void* data)
{
	transfer* info = (transfer*)data;
	info->result = ioctl(sDevice, info->op, &info->command,
		sizeof(info->command));
	release_sem(info->done);
	return B_OK;
}


/*!	Runs the command; false if it does not come back in time (the transfer
	is cancelled then: usb_raw does that when the waiting thread is killed).
*/
static bool
run(uint32 op, usb_raw_command& command, const char* what)
{
	transfer info;
	info.op = op;
	info.command = command;
	info.result = -1;
	info.done = create_sem(0, "transfer done");

	thread_id thread = spawn_thread(transfer_thread, "transfer",
		B_NORMAL_PRIORITY, &info);
	resume_thread(thread);
	status_t status = acquire_sem_etc(info.done, 1, B_RELATIVE_TIMEOUT,
		kDeadline);
	if (status != B_OK) {
		printf("FAIL: %s: no answer in %" B_PRIdBIGTIME " s\n", what,
			kDeadline / 1000000);
		sFailures++;
		kill_thread(thread);
		wait_for_thread(thread, &status);
		delete_sem(info.done);
		return false;
	}

	wait_for_thread(thread, &status);
	delete_sem(info.done);
	command = info.command;
	if (info.result != 0)
		command.transfer.status = B_USB_RAW_STATUS_FAILED;
	return true;
}


/*!	A vendor request without data to the fake device. */
static bool
tell(uint8 request, uint16 value, uint16 index)
{
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.control.request_type = 0x40;
	command.control.request = request;
	command.control.value = value;
	command.control.index = index;
	// (usb_raw wants a user address even for no data)
	uint8 nothing = 0;
	command.control.data = &nothing;
	if (!run(B_USB_RAW_COMMAND_CONTROL_TRANSFER, command, "vendor request"))
		return false;

	CHECK(command.control.status == B_USB_RAW_STATUS_SUCCESS,
		"vendor request 0x%02x: status %d", request,
		(int)command.control.status);
	return command.control.status == B_USB_RAW_STATUS_SUCCESS;
}


static bool
counters(uint32 values[3])
{
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.control.request_type = 0xc0;
	command.control.request = 0x20;
	command.control.length = 12;
	command.control.data = values;
	return run(B_USB_RAW_COMMAND_CONTROL_TRANSFER, command, "counters")
		&& command.control.status == B_USB_RAW_STATUS_SUCCESS
		&& command.control.length == 12;
}


// endpoint indices of the vendor device's interface
enum { INTERRUPT_IN = 0, BULK_IN = 1, BULK_OUT = 2 };
static const uint8 kAddress[3] = { 0x81, 0x82, 0x02 };


/*!	A transfer on an endpoint of interface 0. Returns the status usb_raw
	gives (0 is success), or kNoAnswer when it did not come back; length is
	updated.
*/
static const int32 kNoAnswer = -1;


static int32
do_transfer(uint32 endpoint, void* data, size_t& length, const char* what)
{
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.transfer.interface = 0;
	command.transfer.endpoint = endpoint;
	command.transfer.data = data;
	command.transfer.length = length;
	if (!run(endpoint == INTERRUPT_IN ? B_USB_RAW_COMMAND_INTERRUPT_TRANSFER
			: B_USB_RAW_COMMAND_BULK_TRANSFER, command, what)) {
		return kNoAnswer;
	}

	length = command.transfer.length;
	return command.transfer.status;
}


static bool
is_pattern(const uint8* data, size_t length, uint32 seed)
{
	for (size_t i = 0; i < length; i++) {
		if (data[i] != (uint8)(seed + i * 7))
			return false;
	}
	return true;
}


// #pragma mark - the cases


static void
test_transfers()
{
	if (!open_fake_device(0x0001, "vendor"))
		return;

	uint32 before[3];
	if (!counters(before)) {
		CHECK(false, "the device's counters cannot be read");
		return;
	}

	uint32 seed = before[0];
	const size_t kBufferSize = 300000;
	uint8* buffer = (uint8*)malloc(kBufferSize);

	// IN, as much as asked for: one packet, many, more than one request of
	// the controller holds
	static const size_t kSizes[] = { 1, 7, 511, 512, 513, 1024, 4095, 4096,
		4097, 16384, 65536, 65537, 131072, 262144 };
	bigtime_t start = system_time();
	size_t total = 0;
	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
		memset(buffer, 0xee, kBufferSize);
		size_t length = kSizes[i];
		int32 status = do_transfer(BULK_IN, buffer, length, "bulk in");
		CHECK(status == 0 && length == kSizes[i]
			&& is_pattern(buffer, length, seed),
			"bulk in of %zu bytes: status %d, %zu bytes, data %s", kSizes[i],
			(int)status, length,
			is_pattern(buffer, length, seed) ? "right" : "wrong");
		CHECK(buffer[kSizes[i]] == 0xee, "bulk in of %zu bytes wrote past "
			"the buffer", kSizes[i]);
		total += length;
		seed++;
	}
	printf("bulk in: %zu transfers, %zu bytes, %" B_PRIdBIGTIME " ms\n",
		sizeof(kSizes) / sizeof(kSizes[0]), total,
		(system_time() - start) / 1000);

	// IN, the device sends less (also nothing, and whole packets only)
	static const size_t kShort[][2] = { { 4096, 10 }, { 4096, 0 },
		{ 4096, 512 }, { 4096, 513 }, { 65536, 1 }, { 65536, 1000 },
		{ 65536, 4096 }, { 65536, 40000 }, { 262144, 3 }, { 262144, 60000 },
		{ 512, 511 }, { 64, 1 } };
	for (size_t i = 0; i < sizeof(kShort) / sizeof(kShort[0]); i++) {
		size_t asked = kShort[i][0];
		size_t sent = kShort[i][1];
		if (!tell(0x11, sent, kAddress[BULK_IN]))
			break;

		memset(buffer, 0xee, kBufferSize);
		size_t length = asked;
		int32 status = do_transfer(BULK_IN, buffer, length, "short bulk in");
		bool rest = true;
		for (size_t j = sent; j < asked; j++)
			rest = rest && buffer[j] == 0xee;
		CHECK(status == 0 && length == sent && is_pattern(buffer, sent, seed),
			"bulk in of %zu bytes, %zu sent: status %d, %zu bytes, data %s",
			asked, sent, (int)status, length,
			is_pattern(buffer, sent, seed) ? "right" : "wrong");
		CHECK(rest, "bulk in of %zu bytes, %zu sent: the buffer behind them "
			"was written to", asked, sent);
		seed++;
	}
	printf("short bulk in: %zu transfers\n",
		sizeof(kShort) / sizeof(kShort[0]));

	// OUT
	uint32 outBytes = 0;
	uint32 outSum = 0;
	for (size_t i = 0; i < sizeof(kSizes) / sizeof(kSizes[0]); i++) {
		for (size_t j = 0; j < kSizes[i]; j++) {
			buffer[j] = (uint8)(j * 13 + i);
			outSum += buffer[j];
		}

		size_t length = kSizes[i];
		int32 status = do_transfer(BULK_OUT, buffer, length, "bulk out");
		CHECK(status == 0 && length == kSizes[i],
			"bulk out of %zu bytes: status %d, %zu bytes", kSizes[i],
			(int)status, length);
		outBytes += kSizes[i];
	}

	uint32 after[3];
	CHECK(counters(after), "the device's counters cannot be read");
	CHECK(after[1] - before[1] == outBytes && after[2] - before[2] == outSum,
		"bulk out: the device got %" B_PRIu32 " bytes with sum %" B_PRIu32
		", sent were %" B_PRIu32 " with sum %" B_PRIu32, after[1] - before[1],
		after[2] - before[2], outBytes, outSum);
	printf("bulk out: %zu transfers, %" B_PRIu32 " bytes\n",
		sizeof(kSizes) / sizeof(kSizes[0]), outBytes);
	seed = after[0];

	// interrupt IN: packets shorter than the buffer
	static const size_t kPackets[] = { 8, 1, 64, 3, 0, 63 };
	for (size_t i = 0; i < sizeof(kPackets) / sizeof(kPackets[0]); i++) {
		if (!tell(0x12, kPackets[i], kAddress[INTERRUPT_IN]))
			break;

		memset(buffer, 0xee, 128);
		size_t length = 64;
		int32 status = do_transfer(INTERRUPT_IN, buffer, length,
			"interrupt in");
		CHECK(status == 0 && length == kPackets[i]
			&& is_pattern(buffer, length, seed) && buffer[length] == 0xee
			&& buffer[64] == 0xee,
			"interrupt in, %zu bytes sent: status %d, %zu bytes", kPackets[i],
			(int)status, length);
		seed++;
	}
	printf("interrupt in: %zu transfers\n",
		sizeof(kPackets) / sizeof(kPackets[0]));

	free(buffer);
}


static void
test_errors()
{
	if (!open_fake_device(0x0001, "vendor"))
		return;

	static const struct {
		uint16		status;
		const char*	name;
	} kErrors[] = {
		{ 4, "stall" },
		{ 3, "transaction error" },
		{ 6, "babble" }
	};
	static const char* const kEndpointNames[] = { "interrupt in", "bulk in",
		"bulk out" };

	uint8 buffer[1024];
	for (int round = 0; round < 2; round++) {
		for (size_t i = 0; i < sizeof(kErrors) / sizeof(kErrors[0]); i++) {
			for (uint32 endpoint = 0; endpoint < 3; endpoint++) {
				char what[64];
				snprintf(what, sizeof(what), "%s, %s", kEndpointNames[endpoint],
					kErrors[i].name);

				if (!tell(0x10, kErrors[i].status, kAddress[endpoint]))
					return;

				memset(buffer, 0x55, sizeof(buffer));
				size_t length = endpoint == INTERRUPT_IN ? 64 : 512;
				int32 status = do_transfer(endpoint, buffer, length, what);
				CHECK(status != 0, "%s: the transfer did not fail", what);
				if (status == kNoAnswer)
					continue;

				// the transfer after it
				uint32 values[3];
				if (!counters(values)) {
					CHECK(false, "%s: the device does not answer on its "
						"control endpoint afterwards", what);
					return;
				}

				if (endpoint == INTERRUPT_IN
					&& !tell(0x12, 8, kAddress[INTERRUPT_IN])) {
					return;
				}

				length = endpoint == INTERRUPT_IN ? 64 : 512;
				int32 next = do_transfer(endpoint, buffer, length, what);
				bool right = next == 0
					&& length == (endpoint == INTERRUPT_IN ? 8u : 512u)
					&& (endpoint == BULK_OUT
						|| is_pattern(buffer, length, values[0]));
				CHECK(right, "%s: the next transfer: status %d, %zu bytes",
					what, (int)next, length);
				printf("%s: failed with 0x%x, the next transfer %s\n", what,
					(unsigned)status, right ? "works"
						: next == kNoAnswer ? "DOES NOT COME BACK" : "FAILS");
			}
		}
	}
}


static void
test_alt_gap()
{
	if (!open_fake_device(0x0002, "alt-gap"))
		return;

	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.alternate.config_index = 0;
	command.alternate.interface_index = 0;
	CHECK(ioctl(sDevice, B_USB_RAW_COMMAND_GET_ALT_INTERFACE_COUNT, &command,
			sizeof(command)) == 0
		&& command.alternate.status == B_USB_RAW_STATUS_SUCCESS
		&& command.alternate.alternate_info == 2,
		"the interface does not have 2 alternate settings");

	// several times: what the active setting points to is used anew each
	// time
	for (int i = 0; i < 20; i++) {
		if (!set_alternate(sDevice, 0, 1)) {
			CHECK(false, "the second alternate setting cannot be selected");
			return;
		}

		usb_endpoint_descriptor descriptor;
		CHECK(get_endpoint(sDevice, 0, BULK_IN, descriptor)
			&& descriptor.endpoint_address == kAddress[BULK_IN],
			"the endpoints of the second alternate setting are not there");

		uint32 values[3];
		if (!counters(values)) {
			CHECK(false, "the device's counters cannot be read");
			return;
		}

		uint8 buffer[512];
		size_t length = sizeof(buffer);
		int32 status = do_transfer(BULK_IN, buffer, length, "bulk in");
		CHECK(status == 0 && length == sizeof(buffer)
			&& is_pattern(buffer, length, values[0]),
			"bulk in on the second alternate setting: status %d, %zu bytes",
			(int)status, length);

		CHECK(set_alternate(sDevice, 0, 0),
			"the first alternate setting cannot be selected again");
		if (sFailures != 0)
			return;
	}

	printf("selected the second alternate setting (number 2) 20 times and "
		"read from its bulk endpoint\n");
}


static void
test_no_interfaces()
{
	if (!open_fake_device(0x0003, "no-interfaces"))
		return;

	// That the device is here at all is most of it: its descriptors were
	// parsed when it was plugged in.
	uint8 data[256];
	usb_raw_command command;
	memset(&command, 0, sizeof(command));
	command.config_etc.descriptor = (usb_configuration_descriptor*)data;
	command.config_etc.config_index = 0;
	command.config_etc.length = sizeof(data);
	CHECK(ioctl(sDevice, B_USB_RAW_COMMAND_GET_CONFIGURATION_DESCRIPTOR_ETC,
			&command, sizeof(command)) == 0
		&& command.config_etc.status == B_USB_RAW_STATUS_SUCCESS,
		"no configuration descriptor");
	usb_configuration_descriptor* configuration
		= (usb_configuration_descriptor*)data;
	CHECK(configuration->number_interfaces == 0,
		"the configuration has %u interfaces", configuration->number_interfaces);

	usb_interface_descriptor interface;
	memset(&command, 0, sizeof(command));
	command.interface.descriptor = &interface;
	command.interface.config_index = 0;
	command.interface.interface_index = 0;
	CHECK(ioctl(sDevice, B_USB_RAW_COMMAND_GET_INTERFACE_DESCRIPTOR, &command,
			sizeof(command)) != 0
		|| command.interface.status != B_USB_RAW_STATUS_SUCCESS,
		"an interface of a configuration without any");

	uint32 values[3];
	CHECK(counters(values), "the device does not answer");
	printf("the device with interface descriptors and an interface count of "
		"0 is there and answers\n");
}


static void
test_iso()
{
	sDevice = find_device("/dev/bus/usb", has_isochronous_endpoint);
	if (sDevice < 0) {
		printf("SKIP: no device with an isochronous endpoint\n");
		return;
	}

	usb_iso_packet_descriptor descriptors[8];
	uint8 data[1024];

	// { packets, bytes }
	static const uint32 kTransfers[][2] = { { 0, 0 }, { 0, 512 }, { 8, 0 },
		{ 8, 4 } };
	for (size_t i = 0; i < sizeof(kTransfers) / sizeof(kTransfers[0]); i++) {
		memset(descriptors, 0, sizeof(descriptors));
		for (uint32 j = 0; j < 8; j++)
			descriptors[j].request_length = kTransfers[i][1] / 8;

		usb_raw_command command;
		memset(&command, 0, sizeof(command));
		command.isochronous.interface = sIsoInterface;
		command.isochronous.endpoint = sIsoEndpoint;
		command.isochronous.data = data;
		command.isochronous.length = kTransfers[i][1];
		command.isochronous.packet_descriptors = descriptors;
		command.isochronous.packet_count = kTransfers[i][0];

		char what[64];
		snprintf(what, sizeof(what), "isochronous transfer of %" B_PRIu32
			" packets, %" B_PRIu32 " bytes", kTransfers[i][0],
			kTransfers[i][1]);
		if (!run(B_USB_RAW_COMMAND_ISOCHRONOUS_TRANSFER, command, what))
			break;

		CHECK(command.isochronous.status != B_USB_RAW_STATUS_SUCCESS,
			"%s was accepted", what);
		printf("%s: refused (status %d)\n", what,
			(int)command.isochronous.status);
	}

	set_alternate(sDevice, sIsoInterface, 0);
}


struct mouse_reader {
	int			fd;
	int32		reports;
	int32		errors;
	int32		reportsAfterError;
	int32		lastX;
};


static status_t
mouse_reader_thread(void* data)
{
	mouse_reader* reader = (mouse_reader*)data;
	while (true) {
		mouse_movement movement;
		if (ioctl(reader->fd, MS_READ, &movement, sizeof(movement)) == 0) {
			reader->lastX = movement.xdelta;
			atomic_add(&reader->reportsAfterError, 1);
			atomic_add(&reader->reports, 1);
		} else if (errno == B_INTERRUPTED)
			return B_OK;
		else {
			atomic_add(&reader->errors, 1);
			atomic_set(&reader->reportsAfterError, 0);
			snooze(1000);
		}
	}
}


static void
test_mouse(int32 expectedX, bool checkX)
{
	// the newest mouse is the one plugged in last
	char path[B_PATH_NAME_LENGTH] = "";
	for (int i = 0; i < 20 && path[0] == '\0'; i++) {
		if (i > 0)
			snooze(250000);
		DIR* dir = opendir("/dev/input/mouse/usb");
		if (dir == NULL)
			continue;
		int newest = -1;
		while (dirent* entry = readdir(dir)) {
			if (entry->d_name[0] != '.' && atoi(entry->d_name) > newest)
				newest = atoi(entry->d_name);
		}
		closedir(dir);
		if (newest >= 0) {
			snprintf(path, sizeof(path), "/dev/input/mouse/usb/%d",
				newest);
		}
	}

	if (path[0] == '\0') {
		printf("SKIP: no USB mouse\n");
		return;
	}

	mouse_reader reader = {};
	reader.fd = open(path, O_RDONLY);
	if (reader.fd < 0) {
		CHECK(false, "%s cannot be opened: %s", path, strerror(errno));
		return;
	}
	printf("using %s\n", path);

	thread_id thread = spawn_thread(mouse_reader_thread, "mouse reader",
		B_NORMAL_PRIORITY, &reader);
	resume_thread(thread);
	snooze(4000000);
	// (the reader waits in the driver; this interrupts it)
	send_signal(thread, SIGINT);
	kill_thread(thread);
	status_t status;
	wait_for_thread(thread, &status);
	close(reader.fd);

	printf("4 s: %" B_PRId32 " reports, %" B_PRId32 " failed reads, %"
		B_PRId32 " reports after the last failure, last X %" B_PRId32 "\n",
		reader.reports, reader.errors, reader.reportsAfterError,
		reader.lastX);
	// the device sends one every 50 ms, and fails every 20th
	CHECK(reader.reports >= 20, "too few reports");
	CHECK(reader.reportsAfterError >= 5,
		"no reports after the last failed one");
	if (checkX) {
		CHECK(reader.lastX == expectedX, "X is %" B_PRId32 ", not %" B_PRId32,
			reader.lastX, expectedX);
	}
}


static void
test_mouse()
{
	test_mouse(0, false);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);

	static const struct {
		const char*	name;
		void		(*function)();
	} kCases[] = {
		{ "transfers", test_transfers },
		{ "errors", test_errors },
		{ "alt-gap", test_alt_gap },
		{ "no-interfaces", test_no_interfaces },
		{ "iso", test_iso },
		{ "mouse", test_mouse }
	};

	if (argc == 3 && strcmp(argv[1], "mouse") == 0) {
		test_mouse(strtol(argv[2], NULL, 0), true);
		printf("mouse: %s\n", sFailures == 0 ? "PASSED" : "FAILED");
		return sFailures == 0 ? 0 : 1;
	}

	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		if (argc == 2 && strcmp(argv[1], kCases[i].name) == 0) {
			kCases[i].function();
			printf("%s: %s\n", kCases[i].name,
				sFailures == 0 ? "PASSED" : "FAILED");
			return sFailures == 0 ? 0 : 1;
		}
	}

	fprintf(stderr, "usage: %s transfers|errors|alt-gap|no-interfaces|iso"
		"|mouse [x]\n", argv[0]);
	return 2;
}
