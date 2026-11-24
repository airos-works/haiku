/*
 * Copyright 2015, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 */


#include "Utility.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <device/scsi.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <fs_info.h>
#include <Volume.h>


namespace {


status_t
IssueDeviceCommand(const char* path, int opcode, void* buffer,
	size_t bufferSize)
{
	fs_info info;
	if (fs_stat_dev(dev_for_path(path), &info) == B_OK) {
		if (strcmp(info.fsh_name, "devfs") != 0)
			path = info.device_name;
	}

	int device = open(path, O_RDONLY);
	if (device < 0)
		return device;

	status_t status = B_OK;

	if (ioctl(device, opcode, buffer, bufferSize) != 0) {
		fprintf(stderr, "Failed to process %d on %s: %s\n", opcode, path,
			strerror(errno));
		status = errno;
	}
	close(device);
	return status;
}


}	// private namespace


namespace Utility {


bool
IsReadOnlyVolume(dev_t device)
{
	BVolume volume;
	status_t status = volume.SetTo(device);
	if (status != B_OK) {
		fprintf(stderr, "Failed to get BVolume for device %" B_PRIdDEV
			": %s\n", device, strerror(status));
		return false;
	}

	BDiskDeviceRoster roster;
	BDiskDevice diskDevice;
	BPartition* partition;
	status = roster.FindPartitionByVolume(volume, &diskDevice, &partition);
	if (status != B_OK) {
		fprintf(stderr, "Failed to get partition for device %" B_PRIdDEV
			": %s\n", device, strerror(status));
		return false;
	}

	return partition->IsReadOnly();
}


bool
IsReadOnlyVolume(const char* path)
{
	return IsReadOnlyVolume(dev_for_path(path));
}


status_t
BlockMedia(const char* path, bool block)
{
	return IssueDeviceCommand(path, B_SCSI_PREVENT_ALLOW, &block,
		sizeof(block));
}


status_t
EjectMedia(const char* path)
{
	return IssueDeviceCommand(path, B_EJECT_DEVICE, NULL, 0);
}


BString
TranslatePath(const char* originalPath)
{
	BString path = originalPath;

	// TODO: get actual home directory!
	const char* home = "/boot/home";
	path.ReplaceAll("$HOME", home);
	path.ReplaceAll("${HOME}", home);
	if (path.StartsWith("~/"))
		path.ReplaceFirst("~", home);

	return path;
}


/*!	Takes the next piece of the output of the shell's "export -p", and adds
	the variables of its complete lines to \a environment.

	A read from a pipe ends anywhere, not at the end of a line: what follows
	the last line break is kept in \a rest and put in front of the next piece.
	At the end of the output, pass what \a rest still holds to
	ParseExportVariable().
*/
void
ParseExportOutput(BStringList& environment, BString& rest, const char* data,
	size_t length)
{
	const char* end = data + length;
	while (data < end) {
		const char* separator = (const char*)memchr(data, '\n', end - data);
		if (separator == NULL) {
			rest.Append(data, end - data);
			return;
		}

		rest.Append(data, separator - data);
		ParseExportVariable(environment, rest);
		rest.Truncate(0);

		data = separator + 1;
	}
}


/*!	Adds the variable of a line like \c export NAME="value" to
	\a environment; other lines are ignored.
*/
void
ParseExportVariable(BStringList& environment, const BString& line)
{
	if (!line.StartsWith("export "))
		return;

	int separator = line.FindFirst("=\"");
	if (separator < 0)
		return;

	BString variable;
	line.CopyInto(variable, 7, separator - 7);

	BString value;
	line.CopyInto(value, separator + 2, line.Length() - separator - 3);

	variable << "=" << value;
	environment.Add(variable);
}


}	// namespace Utility
