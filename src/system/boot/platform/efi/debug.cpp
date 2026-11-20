/*
 * Copyright 2016-2022 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "debug.h"

#include <string.h>

#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <kernel.h>
#include <util/ring_buffer.h>

#include "efi_platform.h"
#include "mmu.h"
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

// The syslog buffer the kernel keeps writing to (keep_debug_output_buffer),
// 1 MB. On x86 it is where the BIOS loader puts it, at 63 MB, whenever the
// firmware has that memory free: a restart that keeps RAM (a reset, or the
// kernel's own after a panic, panic_reboot_timeout) then finds the previous
// session's log there, a panic's kernel debugger output included, and the
// kernel saves it as /var/log/previous_syslog. Elsewhere, or when the memory
// is taken, the buffer goes anywhere and nothing is recovered.
static const size_t kSyslogBufferSize = 1024 * 1024;
static const char* const kDebugSyslogSignature = "Haiku syslog";
#if defined(__i386__) || defined(__x86_64__)
static const efi_physical_addr kFixedSyslogBuffer = 63 * 1024 * 1024;
#endif
static efi_physical_addr sFixedSyslogBuffer = 0;


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

	// Diagnostic build: a panic's message only reaches a console nobody may
	// see (Apple), so it goes into the log on the stick as well.
	static bool sPanicking = false;
	if (!sPanicking) {
		sPanicking = true;
		char message[512];
		va_start(args, format);
		vsnprintf(message, sizeof(message), format, args);
		va_end(args);
		dprintf("*** PANIC *** %s\n", message);
		platform_save_boot_log("PANIC (message at the end)");
	}

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
debug_reserve_syslog_buffer(void)
{
#if defined(__i386__) || defined(__x86_64__)
	efi_physical_addr address = kFixedSyslogBuffer;
	if (kBootServices->AllocatePages(AllocateAddress, EfiLoaderData,
			kSyslogBufferSize / B_PAGE_SIZE, &address) == EFI_SUCCESS)
		sFixedSyslogBuffer = address;
#endif
}


/*!	The syslog buffer at the fixed address: what the previous session wrote
	there goes to the kernel (previous_debug_output) when it asked for it.
*/
static bool
use_fixed_ring_buffer(void)
{
	if (sFixedSyslogBuffer == 0
		|| platform_add_allocated_region(sFixedSyslogBuffer, kSyslogBufferSize)
			!= B_OK) {
		gKernelArgs.previous_debug_size = 0;
		return false;
	}

	uint8* base = (uint8*)(addr_t)sFixedSyslogBuffer;
	size_t signatureLength = strlen(kDebugSyslogSignature);
	bool recover = memcmp(base, kDebugSyslogSignature, signatureLength) == 0;
	size_t offset = ROUNDUP(signatureLength, sizeof(void*));
	sDebugSyslogBuffer = create_ring_buffer_etc(base + offset,
		kSyslogBufferSize - offset, recover ? RING_BUFFER_INIT_FROM_BUFFER : 0);

	size_t readable = 0;
	if (recover && gKernelArgs.previous_debug_size != 0)
		readable = ring_buffer_readable(sDebugSyslogBuffer);
	gKernelArgs.previous_debug_size = 0;
	if (readable != 0) {
		uint8* previous = (uint8*)kernel_args_malloc(readable);
		if (previous != NULL) {
			ring_buffer_read(sDebugSyslogBuffer, previous, readable);
			gKernelArgs.previous_debug_output = previous;
			gKernelArgs.previous_debug_size = readable;
		}
	}

	gKernelArgs.debug_output = sDebugSyslogBuffer;
	gKernelArgs.debug_size = sDebugSyslogBuffer->size;
	return true;
}


void
debug_cleanup(void)
{
	bool fixed = use_fixed_ring_buffer();
	if (!fixed)
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

	if (fixed) {
		// The next start recovers this session's log only if the kernel
		// keeps writing it here.
		void* base = (void*)(addr_t)sFixedSyslogBuffer;
		size_t signatureLength = strlen(kDebugSyslogSignature);
		if (gKernelArgs.keep_debug_output_buffer)
			memcpy(base, kDebugSyslogSignature, signatureLength);
		else
			memset(base, 0, signatureLength);
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
diag_write_file(efi_handle handle, const char16_t* kName, const char* header,
	const void* data, size_t dataLength)
{
	static efi_guid fileSystemGUID = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;

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
		size_t length = strlen(header);
		status = file->Write(file, &length, (void*)header);
		length = dataLength;
		if (status == EFI_SUCCESS && length != 0)
			status = file->Write(file, &length, (void*)data);
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
static void
diag_save_file(const char16_t* name, const char* header, const void* data,
	size_t dataLength)
{
	static efi_guid loadedImageGUID = EFI_LOADED_IMAGE_PROTOCOL_GUID;
	static efi_guid fileSystemGUID = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
	static efi_guid devicePathGUID = EFI_DEVICE_PATH_PROTOCOL_GUID;

	efi_loaded_image_protocol* loadedImage = NULL;
	efi_status status = kBootServices->HandleProtocol(kImage, &loadedImageGUID,
		(void**)&loadedImage);
	if (status != EFI_SUCCESS) {
		dprintf("diag: no loaded image protocol: %#" B_PRIx64 "\n",
			(uint64)status);
		return;
	}

	status = diag_write_file(loadedImage->DeviceHandle, name, header, data,
		dataLength);
	if (status == EFI_SUCCESS) {
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
		status = diag_write_file(handles[i], name, header, data, dataLength);
		dprintf("diag: saving the log on file system %p of the same disk: %#"
			B_PRIx64 "\n", handles[i], (uint64)status);
		if (status == EFI_SUCCESS)
			return;
	}
}


void
platform_save_boot_log(const char* stage)
{
	dprintf("diag: saving the log (stage: %s, %" B_PRIu32 " bytes)\n", stage,
		sBufferPosition);
	extern uint64 gDiagReadCalls, gDiagReadBytes;
	char header[320];
	snprintf(header, sizeof(header),
		"=== airOS boot loader log, last stage reached: %s ===\n"
		"=== at %" B_PRId64 " ms, %" B_PRIu64 " block reads, %" B_PRIu64
		" KB read ===\n", stage, system_time() / 1000, gDiagReadCalls,
		gDiagReadBytes / 1024);
	diag_save_file(u"\\airos-loader.log", header, sBuffer, sBufferPosition);
}


/*!	Saves what the previous session left in the syslog buffer at the fixed
	address (a restart that kept RAM) to \\airos-prev-syslog.log.
*/
void
platform_save_previous_syslog(void)
{
	if (sFixedSyslogBuffer == 0) {
		dprintf("diag: fixed syslog buffer not available\n");
		return;
	}
	uint8* base = (uint8*)(addr_t)sFixedSyslogBuffer;
	size_t signatureLength = strlen(kDebugSyslogSignature);
	if (memcmp(base, kDebugSyslogSignature, signatureLength) != 0) {
		dprintf("diag: no syslog from a previous session in RAM\n");
		return;
	}
	size_t offset = ROUNDUP(signatureLength, sizeof(void*));
	ring_buffer* buffer = create_ring_buffer_etc(base + offset,
		kSyslogBufferSize - offset, RING_BUFFER_INIT_FROM_BUFFER);
	size_t readable = buffer != NULL ? ring_buffer_readable(buffer) : 0;
	dprintf("diag: previous session's syslog in RAM: %zu bytes\n", readable);
	if (readable == 0 || readable > kSyslogBufferSize)
		return;
	void* data = NULL;
	if (kBootServices->AllocatePool(EfiLoaderData, readable, &data)
			!= EFI_SUCCESS)
		return;
	readable = ring_buffer_peek(buffer, 0, data, readable);
	diag_save_file(u"\\airos-prev-syslog.log",
		"=== previous session's kernel syslog (from RAM) ===\n", data,
		readable);
	kBootServices->FreePool(data);
}


char*
platform_debug_get_log_buffer(size_t *_size)
{
	if (_size != NULL)
		*_size = sBufferPosition;

	return sBuffer;
}
