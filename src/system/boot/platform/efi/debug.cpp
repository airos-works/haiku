/*
 * Copyright 2016-2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "debug.h"

#include <string.h>

#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <util/ring_buffer.h>

#include "efi_platform.h"
#include "serial.h"

#include <efi/protocol/loaded-image.h>
#include <efi/protocol/simple-file-system.h>


// Diagnostic build: the log is large enough for a whole boot loader run with
// partition tracing, and it can be saved to the boot volume (see
// platform_save_boot_log()). The kernel still gets the first 16 KB only.
static char sBuffer[1024 * 1024];
static uint32 sBufferPosition;
static const uint32 kKernelLogSize = 16384;

static ring_buffer* sDebugSyslogBuffer = NULL;


static void
syslog_write(const char* buffer, size_t length)
{
	if (sDebugSyslogBuffer != NULL) {
		ring_buffer_write(sDebugSyslogBuffer, (const uint8*)buffer, length);
	} else if (sBufferPosition + length < sizeof(sBuffer)) {
		memcpy(sBuffer + sBufferPosition, buffer, length);
		sBufferPosition += length;
	}
}


static void
dprintf_args(const char *format, va_list args)
{
	char buffer[512];
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	if (length == 0)
		return;

	if (length >= (int)sizeof(buffer))
		length = sizeof(buffer) - 1;

	syslog_write(buffer, length);
	serial_puts(buffer, length);
}


extern "C" void
dprintf(const char *format, ...)
{
	va_list args;

	va_start(args, format);
	dprintf_args(format, args);
	va_end(args);
}


extern "C" void
panic(const char *format, ...)
{
	va_list args;

	platform_switch_to_text_mode();

	puts("*** PANIC ***");

	va_start(args, format);
	vprintf(format, args);
	va_end(args);

	while (true)
		kBootServices->Stall(1000000);
}


static void
allocate_ring_buffer(void)
{
	// allocate 1 MB memory
	void* buffer = NULL;
	size_t size = 1024 * 1024;

	if (platform_allocate_region(&buffer, size, 0) != B_OK)
		return;

	sDebugSyslogBuffer = create_ring_buffer_etc(buffer, size, 0);

	gKernelArgs.debug_output = sDebugSyslogBuffer;
	gKernelArgs.debug_size = sDebugSyslogBuffer->size;
}


void
debug_cleanup(void)
{
	allocate_ring_buffer();

	if (sDebugSyslogBuffer != NULL) {
		if (gKernelArgs.keep_debug_output_buffer) {
			// copy the output gathered so far into the ring buffer
			ring_buffer_clear(sDebugSyslogBuffer);
			ring_buffer_write(sDebugSyslogBuffer, (uint8*)sBuffer,
				sBufferPosition);
		}
	} else {
		gKernelArgs.keep_debug_output_buffer = false;
	}

	if (!gKernelArgs.keep_debug_output_buffer) {
		uint32 size = sBufferPosition < kKernelLogSize
			? sBufferPosition : kKernelLogSize;
		gKernelArgs.debug_output = kernel_args_malloc(size);
		if (gKernelArgs.debug_output != NULL) {
			memcpy(gKernelArgs.debug_output, sBuffer, size);
			gKernelArgs.debug_size = size;
		}
	}
}


// Returns the length of a device path without its last node (the path of
// the device it is on), or 0.
static size_t
diag_parent_path_length(const efi_device_path_protocol* path)
{
	const uint8* bytes = (const uint8*)path;
	size_t offset = 0;
	size_t lastNode = 0;
	for (int i = 0; i < 64; i++) {
		size_t length = bytes[offset + 2] | (bytes[offset + 3] << 8);
		if (bytes[offset] == 0x7f || length < 4)
			return lastNode;
		lastNode = offset;
		offset += length;
	}
	return 0;
}


static efi_status
diag_write_log(efi_handle handle, const char* stage)
{
	static efi_guid fileSystemGUID = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	static const char16_t kName[] = u"\\airos-loader.log";

	efi_simple_file_system_protocol* fileSystem = NULL;
	efi_file_protocol* root = NULL;
	efi_file_protocol* file = NULL;
	efi_status status = kBootServices->HandleProtocol(handle, &fileSystemGUID,
		(void**)&fileSystem);
	if (status == EFI_SUCCESS)
		status = fileSystem->OpenVolume(fileSystem, &root);
	if (status != EFI_SUCCESS)
		return status;

	// Delete an older log first: Write() does not truncate.
	if (root->Open(root, &file, kName, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE,
			0) == EFI_SUCCESS) {
		file->Delete(file);
		file = NULL;
	}
	status = root->Open(root, &file, kName, EFI_FILE_MODE_READ
		| EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0);
	if (status == EFI_SUCCESS) {
		char header[128];
		size_t length = snprintf(header, sizeof(header),
			"=== airOS boot loader log, stage: %s ===\n", stage);
		status = file->Write(file, &length, header);
		length = sBufferPosition;
		if (status == EFI_SUCCESS)
			status = file->Write(file, &length, sBuffer);
		if (status == EFI_SUCCESS)
			status = file->Flush(file);
		file->Close(file);
	}
	root->Close(root);
	return status;
}


/*!	Writes the boot loader log to \airos-loader.log on the volume the loader
	was started from (through the firmware's FAT driver), replacing an earlier
	one. If that volume cannot be written (an El Torito image, say), the log
	goes to another file system on the same disk, such as the EFI system
	partition of a USB stick. \a stage says how far the loader got.
*/
void
platform_save_boot_log(const char* stage)
{
	static efi_guid loadedImageGUID = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	static efi_guid fileSystemGUID = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	static efi_guid devicePathGUID = EFI_DEVICE_PATH_PROTOCOL_GUID;

	dprintf("diag: saving the log (stage: %s, %" B_PRIu32 " bytes)\n", stage,
		sBufferPosition);

	efi_loaded_image_protocol* loadedImage = NULL;
	efi_status status = kBootServices->HandleProtocol(kImage, &loadedImageGUID,
		(void**)&loadedImage);
	if (status != EFI_SUCCESS) {
		dprintf("diag: no loaded image protocol: %#" B_PRIx64 "\n",
			(uint64)status);
		return;
	}

	status = diag_write_log(loadedImage->DeviceHandle, stage);
	if (status == EFI_SUCCESS) {
		dprintf("diag: log saved to \\airos-loader.log on the loader's "
			"volume\n");
		return;
	}
	dprintf("diag: saving the log on the loader's volume failed: %#" B_PRIx64
		"\n", (uint64)status);

	efi_device_path_protocol* imagePath = NULL;
	if (kBootServices->HandleProtocol(loadedImage->DeviceHandle,
			&devicePathGUID, (void**)&imagePath) != EFI_SUCCESS)
		return;
	size_t parentLength = diag_parent_path_length(imagePath);
	if (parentLength == 0)
		return;

	size_t size = 0;
	if (kBootServices->LocateHandle(ByProtocol, &fileSystemGUID, NULL, &size,
			NULL) != EFI_BUFFER_TOO_SMALL)
		return;
	efi_handle handles[size / sizeof(efi_handle)];
	if (kBootServices->LocateHandle(ByProtocol, &fileSystemGUID, NULL, &size,
			handles) != EFI_SUCCESS)
		return;

	for (size_t i = 0; i < size / sizeof(efi_handle); i++) {
		efi_device_path_protocol* path = NULL;
		if (handles[i] == loadedImage->DeviceHandle
			|| kBootServices->HandleProtocol(handles[i], &devicePathGUID,
				(void**)&path) != EFI_SUCCESS
			|| diag_parent_path_length(path) != parentLength
			|| memcmp(path, imagePath, parentLength) != 0) {
			continue;
		}
		status = diag_write_log(handles[i], stage);
		dprintf("diag: saving the log on file system %p of the same disk: %#"
			B_PRIx64 "\n", handles[i], (uint64)status);
		if (status == EFI_SUCCESS)
			return;
	}
}


char*
platform_debug_get_log_buffer(size_t *_size)
{
	if (_size != NULL)
		*_size = sBufferPosition;

	return sBuffer;
}
