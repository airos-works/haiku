/*
 * Copyright 2026, air/OS. All rights reserved.
 * Copyright 2016-2017 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "debug.h"

#include <stdarg.h>
#include <string.h>

#include <boot/platform.h>
#include <boot/stage2.h>
#include <boot/stdio.h>
#include <util/ring_buffer.h>

#include "console.h"
#include "cpu.h"
#include "serial.h"


static char sBuffer[16384];
static uint32 sBufferPosition;

static ring_buffer* sDebugSyslogBuffer = NULL;

bool gDebugToScreen = false;


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
dprintf_args(const char* format, va_list args)
{
	char buffer[512];
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	if (length == 0)
		return;

	if (length >= (int)sizeof(buffer))
		length = sizeof(buffer) - 1;

	syslog_write(buffer, length);
	serial_puts(buffer, length);
	if (gDebugToScreen) {
		// scrolling the uncached frame buffer takes long: keep the lab
		// watchdog quiet meanwhile
		watchdog_pet();
		console_write_debug(buffer, length);
	}
}


extern "C" void
dprintf(const char* format, ...)
{
	va_list args;

	va_start(args, format);
	dprintf_args(format, args);
	va_end(args);
}


extern "C" void
panic(const char* format, ...)
{
	va_list args;

	platform_switch_to_text_mode();

	char buffer[512];
	va_start(args, format);
	int length = vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	if (length >= (int)sizeof(buffer))
		length = sizeof(buffer) - 1;

	static const char kHeader[] = "\n*** PANIC ***\n";
	serial_puts(kHeader, sizeof(kHeader) - 1);
	serial_puts(buffer, length);
	serial_puts("\n", 1);
	console_write_debug(kHeader, sizeof(kHeader) - 1);
	console_write_debug(buffer, length);

	// Where the firmware can restart the board (PSCI, the Pi 5), do that
	// after a while: a lab board without a hand at the power switch would
	// stay hung otherwise.
	cpu_reset_after(15000000);

	while (true)
		asm("wfe");
}


extern "C" void
rpi_handle_exception(uint64 vector, uint64 syndrome, uint64 address,
	uint64 faultAddress)
{
	panic("exception %" B_PRIu64 ": ESR %#" B_PRIx64 " at %#" B_PRIx64
		", FAR %#" B_PRIx64, vector, syndrome, address, faultAddress);
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
		gKernelArgs.debug_output = kernel_args_malloc(sBufferPosition);
		if (gKernelArgs.debug_output != NULL) {
			memcpy(gKernelArgs.debug_output, sBuffer, sBufferPosition);
			gKernelArgs.debug_size = sBufferPosition;
		}
	}
}


char*
platform_debug_get_log_buffer(size_t* _size)
{
	if (_size != NULL)
		*_size = sizeof(sBuffer);

	return sBuffer;
}
