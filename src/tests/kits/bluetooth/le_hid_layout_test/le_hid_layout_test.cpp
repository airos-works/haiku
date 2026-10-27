/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Tests the saved layout of an LE mouse (LEHIDService::Save()/Load()):
	what the mouse add-on uses instead of discovering the HID service again
	on every reconnection. A layout written by hand loads with every field,
	saves back byte for byte, and a damaged, truncated, oversized or
	world-readable file is refused.

	le_hid_layout_test [directory]
*/


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <LEHIDService.h>

#include <vector>


using Bluetooth::LEHIDService;

typedef std::vector<uint8> Bytes;

static int sFailures = 0;


static void
check(bool condition, const char* what)
{
	printf("%s: %s\n", condition ? "PASS" : "FAIL", what);
	if (!condition)
		sFailures++;
}


static void
put16(Bytes& data, uint16 value)
{
	data.push_back(value & 0xff);
	data.push_back(value >> 8);
}


// An MX Master style layout: a 64-byte report map, three input reports and
// a battery level that notifies.
static Bytes
layout()
{
	static const char kMagic[] = "HLHIDLY1";
	Bytes data(kMagic, kMagic + 8);
	put16(data, 64);
	for (int i = 0; i < 64; i++)
		data.push_back(0x05 + i);
	data.push_back(3);
	const uint16 handles[3][2] = { { 0x1b, 0x1c }, { 0x1f, 0x20 },
		{ 0x23, 0x24 } };
	for (int i = 0; i < 3; i++) {
		put16(data, handles[i][0]);
		put16(data, handles[i][1]);
		data.push_back(i + 2);
	}
	put16(data, 0x0e);
	put16(data, 0x0f);
	return data;
}


static bool
write_file(const char* path, const Bytes& data, mode_t mode = 0600)
{
	unlink(path);
	int descriptor = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (descriptor < 0)
		return false;
	fchmod(descriptor, mode);
	bool written = write(descriptor, data.data(), data.size())
		== (ssize_t)data.size();
	close(descriptor);
	return written;
}


static Bytes
read_file(const char* path)
{
	Bytes data;
	int descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return data;
	uint8 buffer[1024];
	ssize_t length;
	while ((length = read(descriptor, buffer, sizeof(buffer))) > 0)
		data.insert(data.end(), buffer, buffer + length);
	close(descriptor);
	return data;
}


static bool
loads(const char* path, const Bytes& data, mode_t mode = 0600)
{
	LEHIDService service;
	return write_file(path, data, mode) && service.Load(path) == B_OK;
}


int
main(int argc, char** argv)
{
	const char* directory = argc > 1 ? argv[1] : "/tmp";
	char path[1024], copy[1024];
	snprintf(path, sizeof(path), "%s/le_hid_layout_test.layout", directory);
	snprintf(copy, sizeof(copy), "%s/le_hid_layout_test.copy", directory);

	LEHIDService empty;
	check(empty.Save(copy) == B_NO_INIT, "nothing discovered: nothing saved");
	check(empty.Load("/nonexistent/layout") == B_ENTRY_NOT_FOUND,
		"no saved layout: B_ENTRY_NOT_FOUND");

	const Bytes good = layout();
	LEHIDService service;
	check(write_file(path, good) && service.Load(path) == B_OK,
		"a saved layout loads");
	check(service.ReportMap().size() == 64 && service.ReportMap()[0] == 0x05
		&& service.ReportMap()[63] == 0x05 + 63, "report map");
	check(service.InputReports().size() == 3
		&& service.InputReports()[1].valueHandle == 0x1f
		&& service.InputReports()[1].configurationHandle == 0x20
		&& service.InputReports()[2].reportID == 4, "input reports");
	check(service.FindInputReport(0x23) != NULL
		&& service.FindInputReport(0x23)->reportID == 4
		&& service.FindInputReport(0x0e) == NULL, "reports by handle");
	check(service.BatteryHandle() == 0x0e
		&& service.BatteryConfiguration() == 0x0f, "battery");

	check(service.Save(copy) == B_OK && read_file(copy) == good,
		"saves back byte for byte");
	struct stat info;
	check(stat(copy, &info) == 0 && (info.st_mode & 0777) == 0600,
		"saved private (0600)");

	Bytes damaged = good;
	damaged[3] ^= 0xff;
	check(!loads(path, damaged), "wrong magic refused");
	damaged = good;
	damaged.pop_back();
	check(!loads(path, damaged), "truncated refused");
	damaged = good;
	damaged.push_back(0);
	check(!loads(path, damaged), "trailing bytes refused");
	damaged = good;
	damaged[8] = 0xff;
	damaged[9] = 0xff;
	check(!loads(path, damaged), "report map longer than the file refused");
	damaged = good;
	damaged[10 + 64] = 200;
	check(!loads(path, damaged), "too many reports refused");
	damaged = good;
	damaged[10 + 64 + 1] = 0;
	damaged[10 + 64 + 2] = 0;
	check(!loads(path, damaged), "zero handle refused");
	check(!loads(path, good, 0644), "readable by others: refused");
	Bytes huge = good;
	huge.resize(64 * 1024, 0);
	check(!loads(path, huge), "oversized file refused");

	LEHIDService reloaded;
	check(write_file(path, damaged) && reloaded.Load(path) != B_OK
		&& reloaded.InputReports().empty() && reloaded.ReportMap().empty()
		&& reloaded.BatteryHandle() == 0, "a refused layout leaves nothing");

	unlink(path);
	unlink(copy);
	printf("%s: %d failure%s\n", sFailures == 0 ? "ALL PASSED" : "FAILED",
		sFailures, sFailures == 1 ? "" : "s");
	return sFailures == 0 ? 0 : 1;
}
