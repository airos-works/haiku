/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Device.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>

#include <Autolock.h>
#include <Message.h>
#include <OS.h>


Device::Device()
	:
	fLock("amdxdna device"),
	fFD(-1),
	fUsers(0),
	fKnown(false)
{
	memset(&fMetadata, 0, sizeof(fMetadata));
	memset(&fFirmware, 0, sizeof(fFirmware));
	fDriver[0] = '\0';
	fDriverVersion[0] = fDriverVersion[1] = 0;
}


Device::~Device()
{
	_Close();
}


bool
Device::Exists()
{
	// Looking is all: opening the node starts the NPU.
	struct stat st;
	return stat(XDNA_DEVICE_PATH, &st) == 0 && S_ISCHR(st.st_mode);
}


status_t
Device::Acquire()
{
	BAutolock locker(fLock);
	if (fUsers == 0) {
		status_t status = _Open();
		if (status != B_OK)
			return status;
	}
	fUsers++;
	return B_OK;
}


void
Device::Release()
{
	BAutolock locker(fLock);
	if (--fUsers == 0)
		_Close();
}


status_t
Device::_Open()
{
	// The driver lets nobody else in (a client can have the NPU read and
	// write any memory), and tells so; asking first keeps the reason
	// plain.
	if (geteuid() != 0)
		return B_PERMISSION_DENIED;

	// This is where the NPU is powered up and gets its firmware.
	fFD = open(XDNA_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fFD < 0) {
		status_t status = errno;
		syslog(LOG_WARNING, "amdxdna.neural: cannot open %s: %s",
			XDNA_DEVICE_PATH, strerror(status));
		return status;
	}

	xdna_drm_version version;
	memset(&version, 0, sizeof(version));
	version.name = fDriver;
	version.name_len = sizeof(fDriver) - 1;
	if (ioctl(fFD, XDNA_IOCTL_VERSION, &version) == 0) {
		fDriver[sizeof(fDriver) - 1] = '\0';
		fDriverVersion[0] = version.version_major;
		fDriverVersion[1] = version.version_minor;
	} else
		fDriver[0] = '\0';

	xdna_get_info query;
	memset(&query, 0, sizeof(query));
	query.param = XDNA_QUERY_AIE_METADATA;
	query.buffer_size = sizeof(fMetadata);
	query.buffer = (addr_t)&fMetadata;
	status_t status = B_OK;
	if (ioctl(fFD, XDNA_IOCTL_GET_INFO, &query) != 0)
		status = errno;
	else if (fMetadata.cols == 0 || fMetadata.core.row_count == 0)
		status = B_BAD_DATA;

	if (status == B_OK) {
		query.param = XDNA_QUERY_FIRMWARE_VERSION;
		query.buffer_size = sizeof(fFirmware);
		query.buffer = (addr_t)&fFirmware;
		if (ioctl(fFD, XDNA_IOCTL_GET_INFO, &query) != 0)
			memset(&fFirmware, 0, sizeof(fFirmware));
		fKnown = true;

		// the heap, which has to be mapped before anything is put in it
		status = CreateBuffer(XDNA_BO_DEV_HEAP, XDNA_HEAP_SIZE, fHeap);
	}

	if (status != B_OK) {
		syslog(LOG_WARNING, "amdxdna.neural: the NPU is of no use: %s",
			strerror(status));
		_Close();
	}
	return status;
}


void
Device::_Close()
{
	if (fFD < 0)
		return;
	DeleteBuffer(fHeap);
	// The last close: the driver powers the NPU down.
	close(fFD);
	fFD = -1;
}


void
Device::GetDetails(BMessage* details)
{
	BAutolock locker(fLock);
	details->AddString("path", XDNA_DEVICE_PATH);
	details->AddString("driver", "amdxdna");
	if (!fKnown)
		return;

	char text[128];
	if (fDriver[0] != '\0') {
		snprintf(text, sizeof(text), "%s %" B_PRId32 ".%" B_PRId32, fDriver,
			fDriverVersion[0], fDriverVersion[1]);
		details->AddString("driver interface", text);
	}
	snprintf(text, sizeof(text), "%" B_PRIu32 ".%" B_PRIu32 ".%" B_PRIu32
		".%" B_PRIu32, fFirmware.major, fFirmware.minor, fFirmware.patch,
		fFirmware.build);
	details->AddString("version", text);
	details->AddInt32("columns", fMetadata.cols);
	details->AddInt32("rows", fMetadata.rows);
	details->AddInt32("cores", fMetadata.cols * fMetadata.core.row_count);
	snprintf(text, sizeof(text), "%" B_PRIu32 ".%" B_PRIu32,
		fMetadata.version.major, fMetadata.version.minor);
	details->AddString("aie version", text);
}


status_t
Device::CreateBuffer(uint32 type, size_t size, Buffer& buffer)
{
	buffer = Buffer();
	if (fFD < 0)
		return B_NO_INIT;

	xdna_create_bo create;
	memset(&create, 0, sizeof(create));
	create.size = size;
	create.type = type;
	if (ioctl(fFD, XDNA_IOCTL_CREATE_BO, &create) != 0)
		return errno;
	buffer.handle = create.handle;
	buffer.type = type;
	buffer.size = size;

	xdna_get_bo_info info;
	memset(&info, 0, sizeof(info));
	info.handle = buffer.handle;
	status_t status = B_OK;
	if (ioctl(fFD, XDNA_IOCTL_GET_BO_INFO, &info) != 0)
		status = errno;

	if (status == B_OK && type == XDNA_BO_DEV) {
		// a part of the heap, at the heap's addresses
		if (fHeap.data == NULL || info.xdna_addr < fHeap.address
			|| info.xdna_addr + size > fHeap.address + fHeap.size) {
			status = B_BAD_DATA;
		} else {
			buffer.address = info.xdna_addr;
			buffer.data = (uint8*)fHeap.data + (info.xdna_addr - fHeap.address);
			memset(buffer.data, 0, size);
		}
	} else if (status == B_OK) {
		xdna_map map;
		memset(&map, 0, sizeof(map));
		map.offset = info.map_offset;
		map.size = (size + B_PAGE_SIZE - 1) & ~((size_t)B_PAGE_SIZE - 1);
		map.flags = XDNA_MAP_WRITE;
		if (ioctl(fFD, XDNA_IOCTL_MAP, &map) != 0) {
			status = errno;
		} else {
			buffer.data = (void*)(addr_t)map.address;
			buffer.mapped = true;
			// the address is known once the buffer is mapped
			if (ioctl(fFD, XDNA_IOCTL_GET_BO_INFO, &info) != 0)
				status = errno;
			else if (info.xdna_addr == XDNA_INVALID_ADDRESS)
				status = B_BAD_DATA;
			buffer.address = info.xdna_addr;
		}
	}

	if (status != B_OK)
		DeleteBuffer(buffer);
	return status;
}


void
Device::DeleteBuffer(Buffer& buffer)
{
	if (fFD >= 0 && buffer.mapped) {
		xdna_unmap unmap;
		unmap.address = (addr_t)buffer.data;
		ioctl(fFD, XDNA_IOCTL_UNMAP, &unmap);
	}
	if (fFD >= 0 && buffer.handle != 0) {
		xdna_gem_close close;
		close.handle = buffer.handle;
		close.pad = 0;
		ioctl(fFD, XDNA_IOCTL_GEM_CLOSE, &close);
	}
	buffer = Buffer();
}


void
Device::Flush(const Buffer& buffer)
{
	if (buffer.data == NULL)
		return;

	// The lines out of the processor's caches, which is what the driver's
	// sync ioctl does where it has one: without an IOMMU all buffers are
	// of the driver's contiguous memory, and for those Linux's driver
	// offers none (and says so in the log at every call).
	__builtin_ia32_mfence();
	for (size_t offset = 0; offset < buffer.size; offset += 64)
		__builtin_ia32_clflush((uint8*)buffer.data + offset);
	__builtin_ia32_mfence();
}
