/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The AIC8800D80's USB side, see aic_usb.h.

	The chip shows up twice. In ROM mode (a69c:8d80) it has one vendor
	interface with one bulk pipe each way, over which the loader writes the
	Bluetooth patch and the Wi-Fi firmware and starts it. The chip then
	leaves the bus and comes back running the firmware (a69c:8d81) as a
	composite device: Bluetooth on interfaces 0 and 1 (h2generic's), Wi-Fi on
	the vendor interface 2 with a data pipe and a message pipe each way.

	The driver is registered with the USB stack for both identities, under
	its own name for republishing: when the chip comes back as the other
	one, the USB stack loads the driver again (a registration outlives the
	driver), and init_hardware then finds the running firmware.

	A firmware this boot did not load (a warm restart from another system
	leaves the chip running) is sent back to its ROM first; a named kernel
	area, which outlives the driver, tells the two apart.

	Rules, all learned elsewhere: the host controller's completion callbacks
	and device_removed() never block; every wait has a time limit; one
	transfer at a time per pipe (EHCI starts every queued transfer with the
	pipe's data toggle of the moment it was queued). */


#include "aic_usb.h"

#include <new>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>

#include <FindDirectory.h>
#include <KernelExport.h>
#include <USB3.h>
#include <driver_settings.h>
#include <lock.h>
#include <util/AutoLock.h>

#include "aic_loader.h"


#define DRIVER_NAME			"aic8800wifi"
#define TRACE_ALWAYS(x...)	dprintf(DRIVER_NAME ": " x)
#define TRACE(x...)			do { if (sDebug) dprintf(DRIVER_NAME ": " x); } \
								while (0)

#define DATA_RX_SIZE		20480	/* what the firmware aggregates into */
#define MESSAGE_RX_SIZE		2048
#define TX_SLOTS			64
#define BOOT_WINDOW			(60 * 1000000LL)
#define LOADER_WAIT			(15 * 1000000LL)
#define ROM_ECHO_TIME		(5 * 1000000LL)

#define MARKER_NAME			DRIVER_NAME " firmware"
#define MARKER_MAGIC		0x41494338	/* "AIC8" */


struct aic_device {
	usb_device			device;
	uint16				product;
	usb_pipe			dataIn;
	usb_pipe			dataOut;
	usb_pipe			messageIn;
	usb_pipe			messageOut;
	bool				present;
};

struct rx_pipe {
	int					index;
	const char*			name;
	usb_pipe			pipe;
	size_t				size;
	uint8*				buffers[2];
	sem_id				done;
	thread_id			thread;
	volatile status_t	status;
	volatile size_t		length;
};

struct marker {
	uint32				magic;
	uint32				chip_register;
	bigtime_t			loaded;
};


static usb_module_info* sUSB;
static bool sDebug;

static mutex sDeviceLock = MUTEX_INITIALIZER(DRIVER_NAME " device");
static aic_device sRom;
static aic_device sFirmware;
static volatile bool sGone = true;
static thread_id sLoaderThread = -1;
static uint32 sChipRegister;

static const aic_usb_callbacks* sCallbacks;
static void* sCookie;
static volatile bool sRunning;
static rx_pipe sDataRx;
static rx_pipe sMessageRx;

static mutex sRequestLock = MUTEX_INITIALIZER(DRIVER_NAME " request");
static mutex sConfirmLock = MUTEX_INITIALIZER(DRIVER_NAME " confirm");
static sem_id sConfirmSem = -1;
static sem_id sMessageOutSem = -1;
static volatile status_t sMessageOutStatus;
static uint16 sPendingId;
static uint8* sPendingBuffer;
static size_t sPendingCapacity;
static size_t sPendingLength;
static bool sPendingDone;
static uint8 sMessageBuffer[AIC_MESSAGE_MAX + 4];

static mutex sTxLock = MUTEX_INITIALIZER(DRIVER_NAME " tx");
static uint8* sTxBuffers;
static size_t sTxLengths[TX_SLOTS];
static int32 sTxHead;
static int32 sTxTail;
static int32 sTxCount;
static bool sTxBusy;
static bool sTxWasFull;
static uint32 sTxErrors;


static inline uint16
read16(const uint8* p)
{
	return (uint16)(p[0] | (p[1] << 8));
}


static inline uint32
read32(const uint8* p)
{
	return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16)
		| ((uint32)p[3] << 24);
}


//	#pragma mark - the loaded marker


static bool
marker_get(marker* _marker)
{
	area_id area = find_area(MARKER_NAME);
	if (area < 0)
		return false;

	area_info info;
	if (get_area_info(area, &info) != B_OK)
		return false;

	memcpy(_marker, info.address, sizeof(marker));
	return _marker->magic == MARKER_MAGIC;
}


static void
marker_set(uint32 chipRegister)
{
	marker* address;
	area_id area = find_area(MARKER_NAME);
	if (area >= 0) {
		area_info info;
		if (get_area_info(area, &info) != B_OK)
			return;
		address = (marker*)info.address;
	} else {
		area = create_area(MARKER_NAME, (void**)&address,
			B_ANY_KERNEL_ADDRESS, B_PAGE_SIZE, B_FULL_LOCK,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (area < 0)
			return;
	}

	address->magic = MARKER_MAGIC;
	address->chip_register = chipRegister;
	address->loaded = system_time();
}


static void
marker_clear()
{
	area_id area = find_area(MARKER_NAME);
	if (area >= 0)
		delete_area(area);
}


//	#pragma mark - the device


static status_t
find_pipes(usb_device device, uint16 product, aic_device* found)
{
	const usb_configuration_info* configuration
		= sUSB->get_configuration(device);
	if (configuration == NULL)
		return B_ERROR;

	for (size_t i = 0; i < configuration->interface_count; i++) {
		const usb_interface_info* interface
			= configuration->interface[i].active;
		if (interface == NULL || interface->descr->interface_class != 0xff
			|| interface->descr->interface_subclass != 0xff)
			continue;

		usb_pipe in[2] = { 0, 0 };
		usb_pipe out[2] = { 0, 0 };
		int inCount = 0, outCount = 0;
		for (size_t e = 0; e < interface->endpoint_count; e++) {
			const usb_endpoint_descriptor* endpoint
				= interface->endpoint[e].descr;
			if ((endpoint->attributes & 0x03) != USB_ENDPOINT_ATTR_BULK)
				continue;
			if ((endpoint->endpoint_address & USB_ENDPOINT_ADDR_DIR_IN) != 0) {
				if (inCount < 2)
					in[inCount++] = interface->endpoint[e].handle;
			} else if (outCount < 2)
				out[outCount++] = interface->endpoint[e].handle;
		}
		if (inCount == 0 || outCount == 0)
			return B_ERROR;

		found->device = device;
		found->product = product;
		found->dataIn = in[0];
		found->dataOut = out[0];
		found->messageIn = inCount > 1 ? in[1] : in[0];
		found->messageOut = outCount > 1 ? out[1] : out[0];
		return B_OK;
	}
	return B_ERROR;
}


static status_t loader_thread(void* data);


static status_t
device_added(usb_device device, void** _cookie)
{
	const usb_device_descriptor* descriptor
		= sUSB->get_device_descriptor(device);
	if (descriptor == NULL || descriptor->vendor_id != AIC_USB_VENDOR)
		return B_ERROR;

	aic_device* slot;
	if (descriptor->product_id == AIC_USB_PRODUCT_ROM)
		slot = &sRom;
	else if (descriptor->product_id == AIC_USB_PRODUCT_FIRMWARE)
		slot = &sFirmware;
	else
		return B_ERROR;

	aic_device found;
	if (find_pipes(device, descriptor->product_id, &found) != B_OK) {
		TRACE_ALWAYS("%04x: no vendor interface with bulk pipes\n",
			descriptor->product_id);
		return B_ERROR;
	}

	MutexLocker locker(sDeviceLock);
	if (slot->present) {
		TRACE_ALWAYS("a second %04x is not handled\n", descriptor->product_id);
		return B_ERROR;
	}

	*slot = found;
	slot->present = true;
	*_cookie = slot;
	TRACE_ALWAYS("%04x attached (%s)\n", descriptor->product_id,
		slot == &sRom ? "ROM loader" : "firmware running");

	marker loaded;
	if (slot == &sRom && marker_get(&loaded)
		&& system_time() - loaded.loaded < ROM_ECHO_TIME) {
		// Right after the start request the ROM can show itself once more
		// on its way off the bus; the firmware takes over moments later.
		TRACE_ALWAYS("%04x shows up again while the firmware starts; "
			"left alone\n", descriptor->product_id);
		return B_OK;
	}

	if (slot == &sFirmware) {
		bool wasGone = sGone;
		sGone = false;
		locker.Unlock();

		// the driver runs and lost the chip before: it starts over
		MutexLocker txLocker(sTxLock);
		if (wasGone && sCallbacks != NULL)
			sCallbacks->back(sCookie);
	} else if (sLoaderThread < 0) {
		// never from the thread that explores the bus: the chip's coming
		// back has to be explored
		sLoaderThread = spawn_kernel_thread(loader_thread,
			DRIVER_NAME " loader", B_NORMAL_PRIORITY, NULL);
		if (sLoaderThread >= 0)
			resume_thread(sLoaderThread);
	}
	return B_OK;
}


static status_t
device_removed(void* cookie)
{
	aic_device* slot = (aic_device*)cookie;

	MutexLocker locker(sDeviceLock);
	slot->present = false;
	TRACE_ALWAYS("%04x left the bus\n", slot->product);
	if (slot != &sFirmware)
		return B_OK;

	// Wake everyone who waits for the chip; nothing of what they queued
	// completes: a removed device's transfers are dropped unannounced.
	sGone = true;
	if (sConfirmSem >= 0)
		release_sem_etc(sConfirmSem, 1, B_DO_NOT_RESCHEDULE);
	if (sMessageOutSem >= 0)
		release_sem_etc(sMessageOutSem, 1, B_DO_NOT_RESCHEDULE);
	if (sDataRx.done >= 0)
		release_sem_etc(sDataRx.done, 1, B_DO_NOT_RESCHEDULE);
	if (sMessageRx.done >= 0)
		release_sem_etc(sMessageRx.done, 1, B_DO_NOT_RESCHEDULE);
	locker.Unlock();

	MutexLocker txLocker(sTxLock);
	sTxBusy = false;
	if (sCallbacks != NULL)
		sCallbacks->gone(sCookie);
	return B_OK;
}


static usb_notify_hooks sNotifyHooks = {
	&device_added,
	&device_removed
};


//	#pragma mark - the loader


struct loader_pipes {
	usb_pipe			in;
	usb_pipe			out;
	sem_id				done;
	volatile status_t	status;
	volatile size_t		length;
};


static void
loader_callback(void* cookie, status_t status, void* data, size_t length)
{
	loader_pipes* pipes = (loader_pipes*)cookie;
	pipes->status = status;
	pipes->length = length;
	release_sem_etc(pipes->done, 1, B_DO_NOT_RESCHEDULE);
}


static int
loader_transfer(loader_pipes* pipes, usb_pipe pipe, uint8_t* buffer,
	size_t length, bigtime_t timeout)
{
	if (sUSB->queue_bulk(pipe, buffer, length, loader_callback, pipes)
			!= B_OK)
		return -1;

	if (acquire_sem_etc(pipes->done, 1, B_RELATIVE_TIMEOUT, timeout)
			!= B_OK) {
		sUSB->cancel_queued_transfers(pipe);
		acquire_sem_etc(pipes->done, 1, B_RELATIVE_TIMEOUT, 100000);
		return -1;
	}
	if (pipes->status != B_OK)
		return -1;
	return (int)pipes->length;
}


static int
loader_bulk_out(void* cookie, const uint8_t* buffer, size_t length)
{
	loader_pipes* pipes = (loader_pipes*)cookie;
	int written = loader_transfer(pipes, pipes->out, (uint8_t*)buffer,
		length, 2000000);
	return written == (int)length ? 0 : -1;
}


static int
loader_bulk_in(void* cookie, uint8_t* buffer, size_t capacity, int timeoutMs)
{
	loader_pipes* pipes = (loader_pipes*)cookie;
	return loader_transfer(pipes, pipes->in, buffer, capacity,
		timeoutMs * 1000LL);
}


/*	Sends a running firmware back to its ROM, on the message pipe. Nothing
	answers; the chip leaves the bus after the delay it is given. */
static void
send_reboot(const aic_device& device)
{
	static const aic_loader_ops kOps = {
		loader_bulk_out, loader_bulk_in, NULL, NULL, NULL, NULL
	};

	loader_pipes pipes;
	pipes.in = device.messageIn;
	pipes.out = device.messageOut;
	pipes.done = create_sem(0, DRIVER_NAME " reboot");
	if (pipes.done < 0)
		return;
	if (aic_send_reboot(&kOps, &pipes) != 0)
		TRACE_ALWAYS("sending the restart request failed\n");
	delete_sem(pipes.done);
}


static long
loader_load_file(void*, const char* name, uint8_t** _data)
{
	uint8* data;
	size_t size;
	if (aic_usb_read_file(name, &data, &size) != B_OK)
		return -1;

	// the loader may read up to a block past the end
	uint8* padded = (uint8*)realloc(data, size + 1024);
	if (padded == NULL) {
		free(data);
		return -1;
	}
	memset(padded + size, 0, 1024);
	*_data = padded;
	return (long)size;
}


static void
loader_release_file(void*, uint8_t* data)
{
	free(data);
}


static void
loader_log(void*, const char* format, ...)
{
	char line[256];
	va_list args;
	va_start(args, format);
	vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	dprintf(DRIVER_NAME ": %s", line);
}


static void
loader_sleep(void*, int milliseconds)
{
	snooze(milliseconds * 1000LL);
}


static status_t
loader_thread(void*)
{
	MutexLocker locker(sDeviceLock);
	aic_device rom = sRom;
	locker.Unlock();

	if (rom.present) {
		loader_pipes pipes;
		pipes.in = rom.dataIn;
		pipes.out = rom.dataOut;
		pipes.done = create_sem(0, DRIVER_NAME " loader");

		static const aic_loader_ops kOps = {
			loader_bulk_out, loader_bulk_in, loader_load_file,
			loader_release_file, loader_log, loader_sleep
		};

		// "bt_mode <n>" in the driver settings picks how Bluetooth uses
		// the module's antenna pins (see aic_loader.h); read at every
		// load, so that a firmware restart takes a changed setting.
		int btMode = AIC_BT_MODE_DEFAULT;
		void* settings = load_driver_settings(DRIVER_NAME);
		if (settings != NULL) {
			const char* value = get_driver_parameter(settings, "bt_mode",
				NULL, NULL);
			if (value != NULL)
				btMode = strtol(value, NULL, 10);
			unload_driver_settings(settings);
		}
		btMode = aic_loader_set_bt_mode(btMode);

		TRACE_ALWAYS("loading the firmware (Bluetooth mode %d)\n", btMode);
		bigtime_t start = system_time();
		aic_loader_result result;
		memset(&result, 0, sizeof(result));
		int status = pipes.done >= 0
			? aic_load_firmware_d80(&kOps, &pipes, &result) : -1;
		delete_sem(pipes.done);

		if (status == 0) {
			sChipRegister = result.chip_register;
			marker_set(result.chip_register);
			TRACE_ALWAYS("firmware %s (%#010" B_PRIx32 ") started in %"
				B_PRIdBIGTIME " ms, %u messages; chip %#x\n",
				result.firmware_name, (uint32)result.firmware_version,
				(system_time() - start) / 1000, result.messages,
				result.chip_id);
		} else {
			TRACE_ALWAYS("loading the firmware failed (%d) after %u "
				"messages\n", status, result.messages);
		}
	}

	locker.Lock();
	sLoaderThread = -1;
	return B_OK;
}


//	#pragma mark - receiving


static void
handle_message(const uint8* body, size_t length)
{
	// id, two task IDs, parameter length, a pattern word, parameters
	if (length < 12)
		return;

	uint16 id = read16(body);
	size_t parameterLength = read16(body + 6);
	if (parameterLength > length - 12)
		parameterLength = length - 12;
	const uint8* parameters = body + 12;

	MutexLocker locker(sConfirmLock);
	if (sPendingId != 0 && id == sPendingId) {
		size_t count = min_c(parameterLength, sPendingCapacity);
		if (sPendingBuffer != NULL && count > 0)
			memcpy(sPendingBuffer, parameters, count);
		sPendingLength = parameterLength;
		sPendingId = 0;
		sPendingDone = true;
		release_sem_etc(sConfirmSem, 1, B_DO_NOT_RESCHEDULE);
		return;
	}
	locker.Unlock();

	sCallbacks->message(sCookie, id, parameters, parameterLength);
}


/*	One IN transfer holds one or more packets, each with a 4-byte header:
	a 16-bit length, a type. Messages are padded to 4 bytes after the
	header; a data frame is the firmware's 60-byte receive header and an
	802.11 frame of that length, unpadded. */
static void
parse_transfer(rx_pipe* rx, const uint8* buffer, size_t length)
{
	size_t offset = 0;
	bool data = false;

	while (offset + 4 <= length) {
		const uint8* packet = buffer + offset;
		size_t packetLength = read16(packet);
		uint8 type = packet[2];
		if (packetLength == 0)
			break;

		if ((type & AIC_TYPE_CONFIG) == 0) {
			size_t size = packetLength + 60;
			if (offset + size > length) {
				TRACE_ALWAYS("%s: frame of %zu bytes cut off at %zu\n",
					rx->name, size, length - offset);
				break;
			}
			sCallbacks->data(sCookie, rx->index, packet, size);
			data = true;
			offset += size;
			continue;
		}

		if (offset + 4 + packetLength > length) {
			TRACE_ALWAYS("%s: message of %zu bytes cut off at %zu\n",
				rx->name, packetLength, length - offset - 4);
			break;
		}

		switch (type & 0x7f) {
			case AIC_TYPE_MESSAGE:
				handle_message(packet + 4, packetLength);
				break;
			case AIC_TYPE_DATA_CONFIRM:
				if (packetLength >= 8) {
					sCallbacks->tx_confirm(sCookie, read32(packet + 4),
						read32(packet + 8));
				}
				break;
			case AIC_TYPE_PRINT:
				if (sDebug) {
					char text[128];
					size_t count = min_c(packetLength, sizeof(text) - 1);
					memcpy(text, packet + 4, count);
					text[count] = '\0';
					dprintf(DRIVER_NAME ": firmware: %s\n", text);
				}
				break;
			default:
				TRACE("%s: packet type %#x\n", rx->name, type);
				break;
		}
		offset += 4 + ((packetLength + 3) & ~(size_t)3);
	}

	if (data)
		sCallbacks->data_done(sCookie, rx->index);
}


static void
rx_callback(void* cookie, status_t status, void* data, size_t length)
{
	rx_pipe* rx = (rx_pipe*)cookie;
	rx->status = status;
	rx->length = length;
	release_sem_etc(rx->done, 1, B_DO_NOT_RESCHEDULE);
}


/*	Keeps one transfer queued on the pipe: when one completes, the other
	buffer goes out before the first is parsed. */
static status_t
rx_thread(void* data)
{
	rx_pipe* rx = (rx_pipe*)data;
	int current = 0;
	bool queued = false;
	int32 errors = 0;

	while (sRunning && !sGone) {
		if (!queued) {
			if (sUSB->queue_bulk(rx->pipe, rx->buffers[current], rx->size,
					rx_callback, rx) != B_OK) {
				if (++errors % 100 == 1)
					TRACE_ALWAYS("%s: cannot queue a transfer\n", rx->name);
				snooze(20000);
				continue;
			}
			queued = true;
		}

		if (acquire_sem_etc(rx->done, 1, B_RELATIVE_TIMEOUT, 500000) != B_OK)
			continue;
		if (!sRunning || sGone)
			break;
		queued = false;

		status_t status = rx->status;
		size_t length = rx->length;
		const uint8* buffer = rx->buffers[current];
		if (status != B_OK) {
			if (++errors % 100 == 1) {
				TRACE_ALWAYS("%s: transfer failed: %s (%" B_PRId32 ")\n",
					rx->name, strerror(status), errors);
			}
			if (status == B_DEV_STALLED)
				sUSB->clear_feature(rx->pipe, USB_FEATURE_ENDPOINT_HALT);
			snooze(errors > 10 ? 100000 : 1000);
			continue;
		}
		errors = 0;

		current ^= 1;
		queued = sUSB->queue_bulk(rx->pipe, rx->buffers[current], rx->size,
			rx_callback, rx) == B_OK;
		parse_transfer(rx, buffer, length);
	}

	if (queued && !sGone) {
		sUSB->cancel_queued_transfers(rx->pipe);
		acquire_sem_etc(rx->done, 1, B_RELATIVE_TIMEOUT, 100000);
	}
	return B_OK;
}


static status_t
rx_start(rx_pipe* rx, int index, const char* name, usb_pipe pipe, size_t size)
{
	rx->index = index;
	rx->name = name;
	rx->pipe = pipe;
	rx->size = size;
	rx->buffers[0] = (uint8*)malloc(size);
	rx->buffers[1] = (uint8*)malloc(size);
	rx->done = create_sem(0, name);
	rx->thread = -1;
	if (rx->buffers[0] == NULL || rx->buffers[1] == NULL || rx->done < 0)
		return B_NO_MEMORY;

	rx->thread = spawn_kernel_thread(rx_thread, name, B_URGENT_DISPLAY_PRIORITY,
		rx);
	if (rx->thread < 0)
		return rx->thread;
	resume_thread(rx->thread);
	return B_OK;
}


static void
rx_stop(rx_pipe* rx)
{
	if (rx->thread >= 0) {
		release_sem_etc(rx->done, 1, 0);
		status_t result;
		status_t status = wait_for_thread_etc(rx->thread, B_RELATIVE_TIMEOUT,
			3000000, &result);
		if (status != B_OK && status != B_BAD_THREAD_ID) {
			// (a thread that ended already is gone)
			TRACE_ALWAYS("%s: the thread does not end\n", rx->name);
		}
		rx->thread = -1;
	}
	if (rx->done >= 0)
		delete_sem(rx->done);
	rx->done = -1;
	free(rx->buffers[0]);
	free(rx->buffers[1]);
	rx->buffers[0] = rx->buffers[1] = NULL;
}


//	#pragma mark - sending


static void
message_out_callback(void* cookie, status_t status, void* data, size_t length)
{
	sMessageOutStatus = status;
	release_sem_etc(sMessageOutSem, 1, B_DO_NOT_RESCHEDULE);
}


static void tx_callback(void* cookie, status_t status, void* data,
	size_t length);


static void
tx_start_locked()
{
	while (sTxCount > 0 && sRunning && !sGone) {
		if (sUSB->queue_bulk(sFirmware.dataOut,
				sTxBuffers + sTxTail * AIC_USB_TX_MAX, sTxLengths[sTxTail],
				tx_callback, NULL) == B_OK) {
			sTxBusy = true;
			return;
		}
		if (sTxErrors++ % 100 == 0)
			TRACE_ALWAYS("tx: cannot queue a frame\n");
		sTxTail = (sTxTail + 1) % TX_SLOTS;
		sTxCount--;
	}
	sTxBusy = false;
}


static void
tx_callback(void* cookie, status_t status, void* data, size_t length)
{
	MutexLocker locker(sTxLock);
	if (status != B_OK && sTxErrors++ % 100 == 0) {
		TRACE_ALWAYS("tx: transfer failed: %s (%" B_PRIu32 ")\n",
			strerror(status), sTxErrors);
	}
	if (sTxCount > 0) {
		sTxTail = (sTxTail + 1) % TX_SLOTS;
		sTxCount--;
	}
	sTxBusy = false;
	tx_start_locked();

	if (sTxWasFull && sTxCount < TX_SLOTS / 2) {
		sTxWasFull = false;
		if (sCallbacks != NULL)
			sCallbacks->tx_ready(sCookie);
	}
}


//	#pragma mark - API


static bool
boot_attach_allowed()
{
	if (system_time() >= BOOT_WINDOW)
		return true;

	bool allowed = true;
	void* settings = load_driver_settings(DRIVER_NAME);
	if (settings != NULL) {
		allowed = get_driver_boolean_parameter(settings, "attach_at_boot",
			true, true);
		const char* until = get_driver_parameter(settings,
			"attach_at_boot_until", NULL, NULL);
		if (allowed && until != NULL
			&& (uint64)real_time_clock() >= strtoull(until, NULL, 10))
			allowed = false;
		unload_driver_settings(settings);
	}
	return allowed;
}


status_t
aic_usb_init_hardware()
{
	void* settings = load_driver_settings(DRIVER_NAME);
	if (settings != NULL) {
		sDebug = get_driver_boolean_parameter(settings, "debug", false, true);
		unload_driver_settings(settings);
	}

	if (!boot_attach_allowed()) {
		TRACE_ALWAYS("not attaching during boot (driver settings); touch "
			"the driver's file once it is up\n");
		return B_ERROR;
	}

	if (get_module(B_USB_MODULE_NAME, (module_info**)&sUSB) != B_OK)
		return B_ERROR;

	static const usb_support_descriptor kDevices[] = {
		{ 0, 0, 0, AIC_USB_VENDOR, AIC_USB_PRODUCT_ROM },
		{ 0, 0, 0, AIC_USB_VENDOR, AIC_USB_PRODUCT_FIRMWARE },
	};
	sUSB->register_driver(DRIVER_NAME, kDevices, B_COUNT_OF(kDevices),
		DRIVER_NAME);
	sUSB->install_notify(DRIVER_NAME, &sNotifyHooks);
		// reports the chip if it is there

	// a load that device_added() started
	MutexLocker locker(sDeviceLock);
	thread_id loader = sLoaderThread;
	locker.Unlock();
	if (loader >= 0) {
		status_t result;
		if (wait_for_thread_etc(loader, B_RELATIVE_TIMEOUT, LOADER_WAIT,
				&result) != B_OK) {
			// It cannot be left running in code that is about to go
			// away; it will end at its transfers' time limits.
			TRACE_ALWAYS("the loader does not end\n");
			wait_for_thread(loader, &result);
		}
	}

	locker.Lock();
	bool firmware = sFirmware.present;
	aic_device device = sFirmware;
	locker.Unlock();

	if (firmware) {
		marker loaded;
		if (marker_get(&loaded)) {
			sChipRegister = loaded.chip_register;
			TRACE_ALWAYS("the firmware runs (loaded %" B_PRIdBIGTIME
				" ms ago)\n", (system_time() - loaded.loaded) / 1000);
			return B_OK;
		}

		// Someone else's: the ROM loader is what we know to start from.
		TRACE_ALWAYS("the chip runs firmware this boot did not load; "
			"restarting it\n");
		send_reboot(device);
	} else if (loader < 0)
		TRACE_ALWAYS("no chip\n");

	// the USB stack loads the driver again when the chip comes back
	aic_usb_uninit_hardware();
	return B_ERROR;
}


void
aic_usb_uninit_hardware()
{
	if (sUSB == NULL)
		return;

	sUSB->uninstall_notify(DRIVER_NAME);

	MutexLocker locker(sDeviceLock);
	thread_id loader = sLoaderThread;
	locker.Unlock();
	if (loader >= 0) {
		status_t result;
		wait_for_thread(loader, &result);
	}

	put_module(B_USB_MODULE_NAME);
	sUSB = NULL;
}


status_t
aic_usb_start(const aic_usb_callbacks* callbacks, void* cookie)
{
	MutexLocker locker(sDeviceLock);
	if (!sFirmware.present || sGone)
		return B_DEV_NOT_READY;
	aic_device device = sFirmware;
	locker.Unlock();

	sConfirmSem = create_sem(0, DRIVER_NAME " confirm");
	sMessageOutSem = create_sem(0, DRIVER_NAME " message out");
	sTxBuffers = (uint8*)malloc(TX_SLOTS * AIC_USB_TX_MAX);
	if (sConfirmSem < 0 || sMessageOutSem < 0 || sTxBuffers == NULL) {
		aic_usb_stop();
		return B_NO_MEMORY;
	}
	sTxHead = sTxTail = sTxCount = 0;
	sTxBusy = sTxWasFull = false;

	{
		MutexLocker txLocker(sTxLock);
		sCallbacks = callbacks;
		sCookie = cookie;
	}
	sRunning = true;

	// Both pipes are read before the first message: the firmware may
	// answer on either.
	status_t status = rx_start(&sMessageRx, AIC_USB_PIPE_MESSAGE,
		DRIVER_NAME " message rx", device.messageIn, MESSAGE_RX_SIZE);
	if (status == B_OK) {
		status = rx_start(&sDataRx, AIC_USB_PIPE_DATA,
			DRIVER_NAME " data rx", device.dataIn, DATA_RX_SIZE);
	}
	if (status != B_OK)
		aic_usb_stop();
	return status;
}


void
aic_usb_stop()
{
	sRunning = false;
	rx_stop(&sMessageRx);
	rx_stop(&sDataRx);

	// with no lock held: a cancel calls the completion hooks right here
	if (!sGone) {
		sUSB->cancel_queued_transfers(sFirmware.dataOut);
		sUSB->cancel_queued_transfers(sFirmware.messageOut);
	}

	{
		MutexLocker txLocker(sTxLock);
		sCallbacks = NULL;
		sCookie = NULL;
		sTxCount = 0;
		sTxBusy = false;
	}
	free(sTxBuffers);
	sTxBuffers = NULL;

	if (sConfirmSem >= 0)
		delete_sem(sConfirmSem);
	if (sMessageOutSem >= 0)
		delete_sem(sMessageOutSem);
	sConfirmSem = sMessageOutSem = -1;
}


bool
aic_usb_gone()
{
	return sGone;
}


status_t
aic_usb_request(uint16 id, uint16 task, const void* parameters,
	uint16 parameterLength, uint16 confirmId, void* confirm,
	size_t confirmCapacity, size_t* _confirmLength, bigtime_t timeout)
{
	if ((size_t)parameterLength + AIC_MESSAGE_HEADER > AIC_MESSAGE_MAX)
		return B_BAD_VALUE;

	MutexLocker requestLocker(sRequestLock);
	if (sGone || !sRunning)
		return B_DEV_NOT_READY;

	// a confirmation that came after its request gave up
	while (acquire_sem_etc(sConfirmSem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
		;
	while (acquire_sem_etc(sMessageOutSem, 1, B_RELATIVE_TIMEOUT, 0) == B_OK)
		;

	if (confirmId != 0) {
		MutexLocker locker(sConfirmLock);
		sPendingId = confirmId;
		sPendingBuffer = (uint8*)confirm;
		sPendingCapacity = confirm != NULL ? confirmCapacity : 0;
		sPendingLength = 0;
		sPendingDone = false;
	}

	size_t length = aic_build_message(sMessageBuffer, id, task, parameters,
		parameterLength);
	if (length % 512 == 0) {
		// no zero-length packet can be asked for: pad past the boundary
		memset(sMessageBuffer + length, 0, 4);
		length += 4;
	}

	status_t status = sUSB->queue_bulk(sFirmware.messageOut, sMessageBuffer,
		length, message_out_callback, NULL);
	if (status == B_OK) {
		status = acquire_sem_etc(sMessageOutSem, 1, B_RELATIVE_TIMEOUT,
			1000000);
		if (status != B_OK) {
			sUSB->cancel_queued_transfers(sFirmware.messageOut);
			acquire_sem_etc(sMessageOutSem, 1, B_RELATIVE_TIMEOUT, 100000);
		} else
			status = sMessageOutStatus;
	}

	if (status == B_OK && confirmId != 0) {
		status = acquire_sem_etc(sConfirmSem, 1, B_RELATIVE_TIMEOUT, timeout);
		MutexLocker locker(sConfirmLock);
		if (sPendingDone) {
			status = B_OK;
			if (_confirmLength != NULL)
				*_confirmLength = sPendingLength;
		} else if (status == B_OK)
			status = B_DEV_NOT_READY;
	}

	{
		MutexLocker locker(sConfirmLock);
		sPendingId = 0;
		sPendingBuffer = NULL;
	}

	if (sGone)
		return B_DEV_NOT_READY;
	if (status != B_OK) {
		TRACE_ALWAYS("message %#06x (waiting for %#06x): %s\n", id,
			confirmId, strerror(status));
	}
	return status;
}


status_t
aic_usb_send(const void* frame, size_t length)
{
	if (length > AIC_USB_TX_MAX)
		return B_BAD_VALUE;

	MutexLocker locker(sTxLock);
	if (sGone || !sRunning || sTxBuffers == NULL)
		return B_DEV_NOT_READY;
	if (sTxCount == TX_SLOTS) {
		sTxWasFull = true;
		return B_WOULD_BLOCK;
	}

	memcpy(sTxBuffers + sTxHead * AIC_USB_TX_MAX, frame, length);
	sTxLengths[sTxHead] = length;
	sTxHead = (sTxHead + 1) % TX_SLOTS;
	sTxCount++;
	if (!sTxBusy)
		tx_start_locked();
	return B_OK;
}


bool
aic_usb_send_space()
{
	MutexLocker locker(sTxLock);
	if (sTxCount < TX_SLOTS)
		return true;
	sTxWasFull = true;
	return false;
}


uint32
aic_usb_chip_register()
{
	return sChipRegister;
}


status_t
aic_usb_read_file(const char* name, uint8** _data, size_t* _size)
{
	static const directory_which kDirectories[] = {
		B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY
	};

	char path[B_PATH_NAME_LENGTH];
	int fd = -1;
	for (size_t i = 0; i < B_COUNT_OF(kDirectories) && fd < 0; i++) {
		if (find_directory(kDirectories[i], -1, false, path, sizeof(path))
				!= B_OK)
			continue;
		strlcat(path, "/firmware/" DRIVER_NAME "/", sizeof(path));
		strlcat(path, name, sizeof(path));
		fd = open(path, O_RDONLY);
	}
	if (fd < 0)
		return B_ENTRY_NOT_FOUND;

	off_t size = lseek(fd, 0, SEEK_END);
	lseek(fd, 0, SEEK_SET);
	uint8* data = size > 0 && size < 4 * 1024 * 1024
		? (uint8*)malloc(size + 1) : NULL;
	if (data == NULL || read(fd, data, size) != size) {
		free(data);
		close(fd);
		return B_ERROR;
	}
	close(fd);

	data[size] = '\0';
	*_data = data;
	*_size = size;
	return B_OK;
}


void
aic_usb_reboot_chip()
{
	marker_clear();

	MutexLocker locker(sDeviceLock);
	if (!sFirmware.present || sGone)
		return;
	aic_device device = sFirmware;
	locker.Unlock();

	TRACE_ALWAYS("restarting the chip from its ROM\n");
	send_reboot(device);
}
