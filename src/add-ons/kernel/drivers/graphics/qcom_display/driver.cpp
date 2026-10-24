/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Qualcomm SC8280XP's display outputs, for the Radxa Dragon Q8B: HDMI
	through the board's DisplayPort to HDMI bridge (DisplayPort controller 2
	of MDSS 0), and later DisplayPort over the two USB-C ports.

	The driver owns one frame buffer and shows a region of it on each output
	(the protocol of rpi_display and sunxi_display, so that app_server's
	display layout and the Screens preferences work the same). So far it
	takes over the path the firmware lit for its boot screen (dpu.h) and keeps
	the firmware's mode; without one, the output stands in for a 1920x1080
	display ("virtual"), and the desktop runs headless. */


#include <new>
#include <stdlib.h>
#include <string.h>

#include <Accelerant.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <Drivers.h>
#include <driver_settings.h>
#include <KernelExport.h>

#include <frame_buffer_console.h>
#include <graphic_driver.h>
#include <lock.h>
#include <retrace_semaphores.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <qcom_display.h>
#include <qcom_usbc_ports.h>

#include "display_port.h"
#include "dpu.h"
#include "usbc_dp.h"


#define QCOM_DISPLAY_DRIVER_MODULE_NAME	"drivers/graphics/qcom_display/driver_v1"
#define QCOM_DISPLAY_DEVICE_MODULE_NAME	"drivers/graphics/qcom_display/device_v1"

#define ERROR(x...)	dprintf("qcom_display: " x)
#define INFO(x...)	dprintf("qcom_display: " x)

// what a headless output stands for
static const uint16 kVirtualWidth = 1920;
static const uint32 kUsbcConnectors = 2;
static const uint16 kVirtualHeight = 1080;

// MDSS 0's interrupt (SPI 83) when the device tree names none
static const uint32 kMdssInterrupt = 83 + 32;

// the board's CH7218A bridge: HDMI 2.0, 600 MHz
static const uint32 kBridgeMaxPixelClock = 600000;


struct display_info {
	device_node*	node;
	mutex			lock;
	int32			openCount;
	void*			owner;			// the cookie that set a layout

	area_id			sharedArea;
	qcom_display_shared_info* shared;

	area_id			bufferArea;
	uint8*			buffer;
	phys_addr_t		bufferAddress;
	size_t			bufferSize;

	port_id			changePort;
	int32			changeCode;
	void*			notificationOwner;

	qcom::Dpu*		dpu;			// NULL: nothing to show on
	qcom::DisplayPort* port;		// the DisplayPort controller it feeds
	// the USB-C connectors, outputs 1 and 2
	qcom::UsbcDisplayPort* usbc[kUsbcConnectors];
	bool			usbcRunning[kUsbcConnectors];
	bool			usbcForced[kUsbcConnectors];
	qcom_usbc_ports_module_info* usbcPorts;
	thread_id		hotPlugThread;
	volatile bool	quit;

	// vertical blanks: the timing engine's interrupt while it runs; a
	// timer stands in for it when it does not
	RetraceSemaphores* retrace;
	int32			interrupt;		// installed, or -1
	int64			lastBlank;
	int64			firstBlank;
	int32			blanks;
	timer			retraceTimer;
	bool			retraceTimerAdded;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - timings


static qcom_display_timing
to_shared(const qcom::display_timing& timing)
{
	qcom_display_timing shared = {};
	shared.pixel_clock = timing.pixel_clock;
	shared.h_display = timing.h_display;
	shared.h_sync_start = timing.h_sync_start;
	shared.h_sync_end = timing.h_sync_end;
	shared.h_total = timing.h_total;
	shared.v_display = timing.v_display;
	shared.v_sync_start = timing.v_sync_start;
	shared.v_sync_end = timing.v_sync_end;
	shared.v_total = timing.v_total;
	shared.flags = (timing.h_sync_positive ? B_POSITIVE_HSYNC : 0)
		| (timing.v_sync_positive ? B_POSITIVE_VSYNC : 0);
	return shared;
}


static qcom::display_timing
from_shared(const qcom_display_timing& shared)
{
	qcom::display_timing timing;
	timing.pixel_clock = shared.pixel_clock;
	timing.h_display = shared.h_display;
	timing.h_sync_start = shared.h_sync_start;
	timing.h_sync_end = shared.h_sync_end;
	timing.h_total = shared.h_total;
	timing.v_display = shared.v_display;
	timing.v_sync_start = shared.v_sync_start;
	timing.v_sync_end = shared.v_sync_end;
	timing.v_total = shared.v_total;
	timing.h_sync_positive = (shared.flags & B_POSITIVE_HSYNC) != 0;
	timing.v_sync_positive = (shared.flags & B_POSITIVE_VSYNC) != 0;
	return timing;
}


static bool
same_timing(const qcom_display_timing& a, const qcom_display_timing& b)
{
	return memcmp(&a, &b, sizeof(a)) == 0;
}


/*!	A mode the driver can set: a sane size and a pixel clock the output
	carries.
*/
static bool
valid_timing(const qcom_display_timing& timing, uint32 maxPixelClock)
{
	return timing.h_display >= 320 && timing.h_display <= 5120
		&& timing.v_display >= 200 && timing.v_display <= 4096
		&& timing.h_sync_start >= timing.h_display
		&& timing.h_sync_end > timing.h_sync_start
		&& timing.h_total >= timing.h_sync_end
		&& timing.v_sync_start >= timing.v_display
		&& timing.v_sync_end > timing.v_sync_start
		&& timing.v_total >= timing.v_sync_end
		&& timing.pixel_clock >= 20000
		&& (maxPixelClock == 0 || timing.pixel_clock <= maxPixelClock);
}


/*!	What a headless output stands for: 1920x1080 at 60 Hz (CEA-861). */
static qcom_display_timing
virtual_timing()
{
	qcom_display_timing timing = {};
	timing.pixel_clock = 148500;
	timing.h_display = kVirtualWidth;
	timing.h_sync_start = 2008;
	timing.h_sync_end = 2052;
	timing.h_total = 2200;
	timing.v_display = kVirtualHeight;
	timing.v_sync_start = 1084;
	timing.v_sync_end = 1089;
	timing.v_total = 1125;
	timing.flags = B_POSITIVE_HSYNC | B_POSITIVE_VSYNC;
	return timing;
}


//	#pragma mark - outputs


static bool usbc_forced(uint32 connector);
static status_t connect_usbc(display_info* info, uint32 connector);


static void
init_outputs(display_info* info)
{
	qcom_display_shared_info& shared = *info->shared;
	shared.output_count = 1;

	qcom_display_output& output = shared.outputs[0];
	output.id = QCOM_DISPLAY_OUTPUT_HDMI0;
	output.flags = QCOM_DISPLAY_OUTPUT_CONNECTED;
	output.edid_length = 0;
	if (info->dpu != NULL) {
		qcom::display_timing timing = info->dpu->Timing();
		// the stream's own attributes have the real porches and polarities,
		// and the pixel RCG the exact clock
		if (info->port != NULL) {
			uint32 pixelClock = info->port->PixelClock(info->dpu->WideBus());
			if (pixelClock != 0 && pixelClock > timing.pixel_clock * 99 / 100
				&& pixelClock < timing.pixel_clock * 101 / 100) {
				timing.pixel_clock = pixelClock;
			}
			qcom::display_timing stream;
			if (info->port->StreamTiming(stream) == B_OK
				&& stream.h_display == timing.h_display
				&& stream.v_display == timing.v_display
				&& stream.h_total == timing.h_total
				&& stream.v_total == timing.v_total) {
				stream.pixel_clock = timing.pixel_clock;
				timing = stream;
			}
			if (info->port->HotPlug()) {
				uint32 length = 0;
				status_t status = info->port->ReadEdid(output.edid, &length);
				if (status == B_OK)
					output.edid_length = length;
				else
					ERROR("HDMI-1: no EDID: %s\n", strerror(status));
			}
		}
		output.native_width = timing.h_display;
		output.native_height = timing.v_display;
		output.native_timing = to_shared(timing);
		output.timing = output.native_timing;
		// modes are set on the link the firmware trained
		output.max_pixel_clock = timing.pixel_clock;
		if (info->port != NULL && info->port->MaxPixelClock() != 0) {
			output.max_pixel_clock = min_c(info->port->MaxPixelClock(),
				kBridgeMaxPixelClock);
		}
		INFO("HDMI-1: %ux%u, %" B_PRIu32 " kHz (%u/%u/%u, %u/%u/%u), the "
			"firmware's mode; %s, EDID %" B_PRIu32 " bytes\n",
			output.native_width, output.native_height, timing.pixel_clock,
			timing.h_sync_start, timing.h_sync_end, timing.h_total,
			timing.v_sync_start, timing.v_sync_end, timing.v_total,
			info->port != NULL && info->port->HotPlug()
				? "hot plug" : "no hot plug", output.edid_length);
		if (output.edid_length >= 128) {
			// the monitor's name, from the descriptors
			for (uint32 d = 54; d + 18 <= 126; d += 18) {
				const uint8* descriptor = output.edid + d;
				if (descriptor[0] != 0 || descriptor[1] != 0
					|| descriptor[3] != 0xfc) {
					continue;
				}
				char name[14] = {};
				for (int c = 0; c < 13 && descriptor[5 + c] != '\n'; c++)
					name[c] = descriptor[5 + c];
				INFO("HDMI-1: %s\n", name);
			}
		}
	} else {
		output.flags |= QCOM_DISPLAY_OUTPUT_VIRTUAL;
		output.native_width = kVirtualWidth;
		output.native_height = kVirtualHeight;
		output.native_timing = virtual_timing();
		output.timing = output.native_timing;
		output.max_pixel_clock = 0;
		INFO("HDMI-1: nothing lit, standing in for a %ux%u display\n",
			output.native_width, output.native_height);
	}

	// the USB-C connectors as outputs 1 and 2, connected when the ADSP
	// reports a DisplayPort sink on them (hot_plug_thread()): they show the
	// HDMI output's mode, trained to the sink if one answers. The lab's
	// "usbc_dp0 forced" ("usbc_dp1 ...") has one connected from the start.
	if (info->dpu == NULL || info->dpu->Interface() != 6)
		return;
	static const uint32 kIds[] = {QCOM_DISPLAY_OUTPUT_DP0,
		QCOM_DISPLAY_OUTPUT_DP1};
	for (uint32 connector = 0; connector < kUsbcConnectors; connector++) {
		qcom_display_output& usbc = shared.outputs[1 + connector];
		usbc = output;
		usbc.id = kIds[connector];
		usbc.flags = 0;
		usbc.edid_length = 0;
		info->usbcForced[connector] = usbc_forced(connector);
		if (info->usbcForced[connector]
			&& connect_usbc(info, connector) == B_OK) {
			INFO("DP-%" B_PRIu32 ": forced on as output %" B_PRIu32 ", %ux%u\n",
				connector + 1, connector + 1, usbc.native_width,
				usbc.native_height);
		}
	}
	shared.output_count = 1 + kUsbcConnectors;
}


/*!	Powers a USB-C connector's DisplayPort up and marks its output
	connected. The caller holds the lock, or nobody else runs yet. */
static status_t
connect_usbc(display_info* info, uint32 connector)
{
	if (info->usbc[connector] == NULL) {
		qcom::UsbcDisplayPort* usbc
			= new(std::nothrow) qcom::UsbcDisplayPort(connector);
		if (usbc == NULL)
			return B_NO_MEMORY;
		status_t status = usbc->PowerUp();
		if (status != B_OK) {
			delete usbc;
			return status;
		}
		info->usbc[connector] = usbc;
	}
	info->shared->outputs[1 + connector].flags |= QCOM_DISPLAY_OUTPUT_CONNECTED;
	return B_OK;
}


static void
disconnect_usbc(display_info* info, uint32 connector)
{
	if (info->usbcRunning[connector])
		info->usbc[connector]->Stop();
	info->usbcRunning[connector] = false;
	info->shared->outputs[1 + connector].flags = 0;
}


static void
notify_change(display_info* info)
{
	if (info->changePort >= 0) {
		write_port_etc(info->changePort, info->changeCode, NULL, 0,
			B_RELATIVE_TIMEOUT, 0);
	}
}


/*!	Follows the ADSP's reports on the USB-C ports (qcom_adsp's
	qcom_usbc_ports module): a DisplayPort sink with its hot plug detect up
	connects the port's output, anything else disconnects it; app_server is
	told. */
static status_t
hot_plug_thread(void* data)
{
	display_info* info = (display_info*)data;
	uint32 generations[kUsbcConnectors] = {};
	while (!info->quit) {
		snooze(300000);
		for (uint32 connector = 0; connector < kUsbcConnectors; connector++) {
			qcom_usbc_port port;
			if (info->usbcPorts->get_port(connector, &port) != B_OK
				|| port.generation == generations[connector])
				continue;
			generations[connector] = port.generation;
			bool sink = port.display_port && port.hpd;
			MutexLocker locker(info->lock);
			bool connected = (info->shared->outputs[1 + connector].flags
				& QCOM_DISPLAY_OUTPUT_CONNECTED) != 0;
			if (sink && !connected) {
				status_t status = connect_usbc(info, connector);
				INFO("DP-%" B_PRIu32 ": a DisplayPort sink on USB-C port %"
					B_PRIu32 " (pin assignment %u, orientation %u): %s\n",
					connector + 1, connector, port.pin_assignment,
					port.orientation,
					status == B_OK ? "connected" : strerror(status));
				if (status == B_OK)
					notify_change(info);
			} else if (!sink && connected && !info->usbcForced[connector]) {
				disconnect_usbc(info, connector);
				INFO("DP-%" B_PRIu32 ": the sink on USB-C port %" B_PRIu32
					" is gone\n", connector + 1, connector);
				notify_change(info);
			}
		}
	}
	return B_OK;
}


/*!	Whether the driver settings ask for a USB-C connector without a
	monitor (lab: "usbc_dp0 forced", "usbc_dp1 forced"). */
static bool
usbc_forced(uint32 connector)
{
	void* handle = load_driver_settings("qcom_display");
	if (handle == NULL)
		return false;
	char name[16];
	snprintf(name, sizeof(name), "usbc_dp%" B_PRIu32, connector);
	const char* value = get_driver_parameter(handle, name, "", "");
	bool forced = strcmp(value, "forced") == 0;
	unload_driver_settings(handle);
	return forced;
}


/*!	Output 0's region of the frame buffer once a layout is set. */
static bool
has_layout(display_info* info)
{
	const qcom_display_shared_info& shared = *info->shared;
	return shared.bytes_per_row != 0
		&& (shared.outputs[0].flags & QCOM_DISPLAY_OUTPUT_ENABLED) != 0;
}


static void
update_usbc_scanout(display_info* info, uint32 connector)
{
	const qcom_display_shared_info& shared = *info->shared;
	qcom::UsbcDisplayPort* usbc = info->usbc[connector];
	if (usbc == NULL || shared.output_count < 2 + connector)
		return;
	const qcom_display_output& output = shared.outputs[1 + connector];
	if ((output.flags & QCOM_DISPLAY_OUTPUT_ENABLED) == 0
		|| shared.bytes_per_row == 0) {
		if (info->usbcRunning[connector])
			usbc->Stop();
		info->usbcRunning[connector] = false;
		return;
	}
	phys_addr_t address = info->bufferAddress
		+ (phys_addr_t)output.y * shared.bytes_per_row + output.x * 4;
	status_t status = info->usbcRunning[connector]
		? usbc->SetScanout(address, shared.bytes_per_row, output.width,
			output.height)
		: usbc->Start(address, shared.bytes_per_row, output.width,
			output.height, info->dpu->Timing(), info->dpu->WideBus());
	if (status == B_OK) {
		INFO("DP-%" B_PRIu32 ": scans out %ux%u at %" B_PRId32 ",%" B_PRId32
			" (%#" B_PRIxPHYSADDR ", frame buffer %#" B_PRIxPHYSADDR ")\n",
			connector + 1, output.width, output.height, output.x, output.y,
			address, info->bufferAddress);
		info->usbcRunning[connector] = true;
	} else {
		ERROR("DP-%" B_PRIu32 ": cannot show %ux%u at %" B_PRId32 ",%" B_PRId32
			": %s\n", connector + 1, output.width, output.height, output.x,
			output.y, strerror(status));
	}
}


static void
update_scanout(display_info* info)
{
	for (uint32 connector = 0; connector < kUsbcConnectors; connector++)
		update_usbc_scanout(info, connector);
	if (info->dpu == NULL || !has_layout(info))
		return;
	const qcom_display_shared_info& shared = *info->shared;
	const qcom_display_output& output = shared.outputs[0];
	phys_addr_t address = info->bufferAddress
		+ (phys_addr_t)output.y * shared.bytes_per_row + output.x * 4;
	status_t status = info->dpu->SetScanout(address, shared.bytes_per_row,
		output.width, output.height);
	if (status != B_OK) {
		ERROR("HDMI-1: cannot show %ux%u: %s\n", output.width, output.height,
			strerror(status));
	}
}


/*!	Output 0 at \a wanted, on the link the firmware trained: the timing
	engine stopped, the stream idle, both retimed, started again. Back to the
	mode it had when the new one does not come up.
*/
static status_t
set_mode(display_info* info, const qcom_display_timing& wanted)
{
	if (info->dpu == NULL || info->port == NULL)
		return B_NOT_SUPPORTED;
	qcom::display_timing timing = from_shared(wanted);
	qcom::display_timing old = info->dpu->Timing();
	bool wideBus = info->dpu->WideBus();

	// the stream goes idle while the timing engine still feeds it, as
	// Linux disables the bridge before the encoder
	status_t status = info->port->StopStream();
	info->dpu->StopTiming();
	if (status == B_OK)
		status = info->port->SetStream(timing, wideBus);
	if (status == B_OK)
		status = info->dpu->SetTiming(timing);
	if (status == B_OK) {
		info->dpu->StartTiming();
		status = info->port->StartStream();
	}
	if (status == B_OK)
		return B_OK;

	ERROR("HDMI-1: %ux%u at %" B_PRIu32 " kHz does not come up (%s), back to "
		"%ux%u\n", timing.h_display, timing.v_display, timing.pixel_clock,
		strerror(status), old.h_display, old.v_display);
	info->port->StopStream();
	info->dpu->StopTiming();
	if (info->port->SetStream(old, wideBus) == B_OK
		&& info->dpu->SetTiming(old) == B_OK) {
		info->dpu->StartTiming();
		info->port->StartStream();
	}
	return status;
}


//	#pragma mark - vertical blank


static int32
display_interrupt(void* data)
{
	display_info* info = (display_info*)data;
	if (info->dpu == NULL || !info->dpu->AcknowledgeBlank())
		return B_UNHANDLED_INTERRUPT;

	bigtime_t now = system_time();
	atomic_set64(&info->lastBlank, now);
	// the rate, once, for the log
	int32 blanks = atomic_add(&info->blanks, 1);
	if (blanks == 0)
		info->firstBlank = now;
	else if (blanks == 300) {
		INFO("300 blanks in %" B_PRId64 " us\n", now - info->firstBlank);
	}
	return info->retrace->Release() ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


/*!	At 60 Hz, while no blank interrupt comes, so that waiting for a blank
	never waits long.
*/
static int32
retrace_timer(timer* timer)
{
	display_info* info = (display_info*)timer->user_data;
	if (system_time() - atomic_get64(&info->lastBlank) < 100000)
		return B_HANDLED_INTERRUPT;
	return info->retrace->Release() ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


static uint32
display_interrupt_number(display_info* info)
{
	uint64 interrupt = kMdssInterrupt;
	device_node* parent = sDeviceManager->get_parent_node(info->node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (parent != NULL && sDeviceManager->get_driver(parent,
			(driver_module_info**)&fdt, (void**)&device) == B_OK
		&& !fdt->get_interrupt(device, 0, NULL, &interrupt)) {
		interrupt = kMdssInterrupt;
	}
	if (parent != NULL)
		sDeviceManager->put_node(parent);
	return (uint32)interrupt;
}


static void
init_retrace(display_info* info)
{
	info->interrupt = -1;
	info->retrace = new(std::nothrow) RetraceSemaphores;
	if (info->retrace == NULL || info->dpu == NULL)
		return;

	uint32 interrupt = display_interrupt_number(info);
	status_t status = install_io_interrupt_handler(interrupt,
		display_interrupt, info, 0);
	if (status != B_OK) {
		ERROR("no vertical blank interrupt %" B_PRIu32 ": %s\n", interrupt,
			strerror(status));
		return;
	}
	info->interrupt = interrupt;
	info->dpu->EnableBlankInterrupt(true);
}


static void
uninit_retrace(display_info* info)
{
	if (info->retraceTimerAdded)
		cancel_timer(&info->retraceTimer);
	if (info->dpu != NULL)
		info->dpu->EnableBlankInterrupt(false);
	if (info->interrupt >= 0)
		remove_io_interrupt_handler(info->interrupt, display_interrupt, info);
	delete info->retrace;
	info->retrace = NULL;
}


//	#pragma mark - frame buffer


/*!	A frame buffer of at least \a size bytes: contiguous, below 4 GiB (the
	pipes fetch 32-bit addresses), write-combining. An existing one that is
	large enough stays.
*/
static status_t
ensure_buffer(display_info* info, size_t size)
{
	size = ROUNDUP(size, B_PAGE_SIZE);
	if (info->bufferArea >= 0 && info->bufferSize >= size)
		return B_OK;

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 32;
	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "qcom display frame buffer",
		size, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < 0)
		return area;

	physical_entry entry;
	status_t status = get_memory_map(address, size, &entry, 1);
	if (status == B_OK) {
		status = vm_set_area_memory_type(area, entry.address,
			B_WRITE_COMBINING_MEMORY);
	}
	if (status != B_OK) {
		delete_area(area);
		return status;
	}
	memset(address, 0, size);

	// Whoever still maps the old buffer keeps its memory; nothing shows it.
	if (info->bufferArea >= 0)
		delete_area(info->bufferArea);

	info->bufferArea = area;
	info->buffer = (uint8*)address;
	info->bufferAddress = entry.address;
	info->bufferSize = size;
	info->shared->generation++;
	info->shared->physical_address = entry.address;
	return B_OK;
}


/*!	The pipe does not scale and the mode stays the firmware's, so each
	region is at most the output's mode; a smaller one shows in its top left
	corner.
*/
static status_t
set_layout(display_info* info, const qcom_display_layout& layout)
{
	qcom_display_shared_info& shared = *info->shared;

	if (layout.version != QCOM_DISPLAY_VERSION || layout.width == 0
		|| layout.height == 0 || layout.width > 8192 || layout.height > 8192) {
		return B_BAD_VALUE;
	}

	uint32 enabled = 0;
	for (uint32 i = 0; i < shared.output_count; i++) {
		const qcom_display_output& request = layout.outputs[i];
		if ((request.flags & QCOM_DISPLAY_OUTPUT_ENABLED) == 0)
			continue;
		const qcom_display_output& output = shared.outputs[i];
		const qcom_display_timing& timing = request.timing.h_display != 0
			? request.timing : output.timing;
		if ((output.flags & QCOM_DISPLAY_OUTPUT_CONNECTED) == 0
			|| request.x < 0 || request.y < 0 || request.width == 0
			|| request.height == 0
			|| request.x + request.width > (int32)layout.width
			|| request.y + request.height > (int32)layout.height
			|| request.width > timing.h_display
			|| request.height > timing.v_display) {
			return B_BAD_VALUE;
		}
		if (request.timing.h_display != 0
			&& !valid_timing(request.timing, output.max_pixel_clock)) {
			return B_BAD_VALUE;
		}
		enabled++;
	}
	if (enabled == 0)
		return B_BAD_VALUE;

	uint32 bytesPerRow = layout.width * 4;
	status_t status = ensure_buffer(info, (size_t)bytesPerRow * layout.height);
	if (status != B_OK)
		return status;
	bool modeChanged = false;

	for (uint32 i = 0; i < shared.output_count; i++) {
		qcom_display_output& output = shared.outputs[i];
		const qcom_display_output& request = layout.outputs[i];
		if ((output.flags & QCOM_DISPLAY_OUTPUT_CONNECTED) == 0)
			continue;

		uint32 kept = output.flags & (QCOM_DISPLAY_OUTPUT_CONNECTED
			| QCOM_DISPLAY_OUTPUT_VIRTUAL);
		if ((request.flags & QCOM_DISPLAY_OUTPUT_ENABLED) == 0) {
			output.flags = kept;
			continue;
		}

		output.flags = kept | QCOM_DISPLAY_OUTPUT_ENABLED
			| (request.flags & QCOM_DISPLAY_OUTPUT_MIRROR);
		// a headless output just takes the mode; a display gets it set
		if (request.timing.h_display != 0
			&& !same_timing(request.timing, output.timing)) {
			if ((output.flags & QCOM_DISPLAY_OUTPUT_VIRTUAL) != 0)
				output.timing = request.timing;
			else if (i == 0) {
				INFO("HDMI-1: mode %ux%u, %" B_PRIu32 " kHz\n",
					request.timing.h_display, request.timing.v_display,
					request.timing.pixel_clock);
				status = set_mode(info, request.timing);
				if (status != B_OK)
					return status;
				output.timing = request.timing;
				modeChanged = true;
			}
		}
		output.x = request.x;
		output.y = request.y;
		output.width = request.width;
		output.height = request.height;
		output.mode_width = request.mode_width;
		output.mode_height = request.mode_height;
		output.scale = request.scale;
		output.render_scale = request.render_scale;

		INFO("output %" B_PRIu32 ": %ux%u at %" B_PRId32 ",%" B_PRId32
			" of %" B_PRIu32 "x%" B_PRIu32 "%s%s\n", i, request.width,
			request.height, request.x, request.y, layout.width, layout.height,
			(request.flags & QCOM_DISPLAY_OUTPUT_MIRROR) != 0 ? ", mirror" : "",
			(output.flags & QCOM_DISPLAY_OUTPUT_VIRTUAL) != 0
				? ", headless" : "");
	}

	shared.width = layout.width;
	shared.height = layout.height;
	shared.bytes_per_row = bytesPerRow;
	update_scanout(info);
	if (modeChanged)
		info->dpu->LogState("HDMI-1 after the mode change");

	// the kernel's console and debugger follow
	frame_buffer_update((addr_t)info->buffer, layout.width, layout.height, 32,
		bytesPerRow);
	return B_OK;
}


static void
release_layout(display_info* info)
{
	qcom_display_shared_info& shared = *info->shared;
	for (uint32 i = 0; i < shared.output_count; i++) {
		shared.outputs[i].flags &= QCOM_DISPLAY_OUTPUT_CONNECTED
			| QCOM_DISPLAY_OUTPUT_VIRTUAL;
	}
}


//	#pragma mark - device


static status_t
display_init_device(void* _info, void** _cookie)
{
	display_info* info = (display_info*)_info;

	info->sharedArea = create_area("qcom display shared info",
		(void**)&info->shared, B_ANY_KERNEL_ADDRESS,
		ROUNDUP(sizeof(qcom_display_shared_info), B_PAGE_SIZE), B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (info->sharedArea < 0)
		return info->sharedArea;
	memset(info->shared, 0, sizeof(qcom_display_shared_info));
	info->shared->version = QCOM_DISPLAY_VERSION;
	info->bufferArea = -1;

	// the path the firmware lit, if any
	info->dpu = new(std::nothrow) qcom::Dpu;
	if (info->dpu != NULL && info->dpu->Init() != B_OK) {
		delete info->dpu;
		info->dpu = NULL;
	}
	// INTF_6 feeds DisplayPort controller 2, the HDMI bridge's
	if (info->dpu != NULL && info->dpu->Interface() == 6) {
		info->port = new(std::nothrow) qcom::DisplayPort(2);
		if (info->port != NULL && info->port->Init() != B_OK) {
			delete info->port;
			info->port = NULL;
		}
	}

	init_outputs(info);

	// a frame buffer for the first output, taken while contiguous memory is
	// still easy to find
	// (side by side if there are two)
	const qcom_display_shared_info& shared = *info->shared;
	size_t width = 0, height = 0;
	for (uint32 i = 0; i < shared.output_count; i++) {
		width += shared.outputs[i].native_width;
		height = max_c(height, (size_t)shared.outputs[i].native_height);
	}
	status_t status = ensure_buffer(info, width * 4 * height);
	if (status != B_OK) {
		ERROR("no memory for the frame buffer: %s\n", strerror(status));
		for (qcom::UsbcDisplayPort* usbc : info->usbc)
			delete usbc;
		delete info->port;
		delete info->dpu;
		delete_area(info->sharedArea);
		return status;
	}

	mutex_init(&info->lock, "qcom display");
	info->changePort = -1;
	init_retrace(info);

	// the USB-C ports' reports, from the ADSP's driver
	info->hotPlugThread = -1;
	if (info->shared->output_count > 1
		&& get_module(QCOM_USBC_PORTS_MODULE_NAME,
			(module_info**)&info->usbcPorts) == B_OK) {
		info->hotPlugThread = spawn_kernel_thread(hot_plug_thread,
			"qcom display hot plug", B_LOW_PRIORITY, info);
		if (info->hotPlugThread >= 0)
			resume_thread(info->hotPlugThread);
	}

	*_cookie = info;
	return B_OK;
}


static void
display_uninit_device(void* cookie)
{
	display_info* info = (display_info*)cookie;
	uninit_retrace(info);
	if (info->hotPlugThread >= 0) {
		info->quit = true;
		status_t result;
		wait_for_thread(info->hotPlugThread, &result);
	}
	if (info->usbcPorts != NULL)
		put_module(QCOM_USBC_PORTS_MODULE_NAME);
	for (qcom::UsbcDisplayPort* usbc : info->usbc)
		delete usbc;
	delete info->port;
	delete info->dpu;
	mutex_destroy(&info->lock);
	if (info->bufferArea >= 0)
		delete_area(info->bufferArea);
	delete_area(info->sharedArea);
}


static status_t
display_open(void* _info, const char* path, int openMode, void** _cookie)
{
	display_info* info = (display_info*)_info;

	// the cookie only tells one open from another
	void* cookie = malloc(1);
	if (cookie == NULL)
		return B_NO_MEMORY;

	atomic_add(&info->openCount, 1);
	*_cookie = cookie;
	return B_OK;
}


static status_t
display_close(void* cookie)
{
	return B_OK;
}


// device_manager hands the hooks the cookie of open(); the device is found
// through a global, there is only one.
static display_info* sInfo;


/*!	Register access from userland is for bringing the hardware up; the lab
	images switch it on in the driver settings.
*/
static bool
debug_registers_allowed()
{
	void* handle = load_driver_settings("qcom_display");
	if (handle == NULL)
		return false;
	bool allowed = get_driver_boolean_parameter(handle, "debug_registers",
		false, true);
	unload_driver_settings(handle);
	return allowed;
}


static status_t
display_free(void* cookie)
{
	display_info* info = sInfo;
	MutexLocker locker(info->lock);
	if (info->notificationOwner == cookie) {
		info->notificationOwner = NULL;
		info->changePort = -1;
	}
	if (info->owner == cookie) {
		release_layout(info);
		info->owner = NULL;
	}
	atomic_add(&info->openCount, -1);
	free(cookie);
	return B_OK;
}


static status_t
display_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	display_info* info = sInfo;

	switch (op) {
		case B_GET_ACCELERANT_SIGNATURE:
			return user_strlcpy((char*)buffer, QCOM_DISPLAY_ACCELERANT,
				B_FILE_NAME_LENGTH) < B_OK ? B_BAD_ADDRESS : B_OK;

		case QCOM_DISPLAY_GET_SHARED_AREA:
			return user_memcpy(buffer, &info->sharedArea, sizeof(area_id));

		case QCOM_DISPLAY_GET_STATE:
		{
			if (length != sizeof(qcom_display_shared_info))
				return B_BAD_VALUE;
			MutexLocker locker(info->lock);
			return user_memcpy(buffer, info->shared, sizeof(*info->shared));
		}

		case QCOM_DISPLAY_SET_CHANGE_PORT:
		{
			qcom_display_change_port request;
			if (length != sizeof(request))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&request, buffer, sizeof(request));
			if (status != B_OK)
				return status;
			if (request.port >= 0) {
				port_info port;
				status = get_port_info(request.port, &port);
				if (status != B_OK || port.team != team_get_current_team_id())
					return B_NOT_ALLOWED;
			}
			MutexLocker locker(info->lock);
			if (info->notificationOwner != NULL
				&& info->notificationOwner != cookie)
				return B_BUSY;
			info->notificationOwner = request.port >= 0 ? cookie : NULL;
			info->changePort = request.port;
			info->changeCode = request.code;
			// Catch changes between the initial output query and registration.
			if (request.port >= 0)
				write_port_etc(request.port, request.code, NULL, 0,
					B_RELATIVE_TIMEOUT, 0);
			return B_OK;
		}

		case QCOM_DISPLAY_SET_LAYOUT:
		{
			qcom_display_layout layout;
			if (length != sizeof(layout))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&layout, buffer, sizeof(layout));
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			if (info->owner != NULL && info->owner != cookie)
				return B_BUSY;
			status = set_layout(info, layout);
			if (status == B_OK)
				info->owner = cookie;
			return status;
		}

		case QCOM_DISPLAY_DEBUG_REGISTER:
		{
			if (!debug_registers_allowed() || info->dpu == NULL)
				return B_NOT_ALLOWED;
			qcom_display_register request;
			if (length != sizeof(request)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_VALUE;
			}
			MutexLocker locker(info->lock);
			status_t status = info->dpu->DebugRegister(request.address,
				request.value, request.write != 0);
			if (status != B_OK)
				return status;
			return user_memcpy(buffer, &request, sizeof(request));
		}

		case QCOM_DISPLAY_GET_RETRACE_SEMAPHORE:
		{
			if (info->retrace == NULL)
				return B_NOT_SUPPORTED;
			MutexLocker locker(info->lock);
			sem_id semaphore = info->retrace->Get();
			if (semaphore < 0)
				return semaphore;
			if (!info->retraceTimerAdded) {
				info->retraceTimer.user_data = info;
				add_timer(&info->retraceTimer, retrace_timer, 1000000 / 60,
					B_PERIODIC_TIMER);
				info->retraceTimerAdded = true;
			}
			locker.Unlock();
			return user_memcpy(buffer, &semaphore, sizeof(semaphore));
		}

		case QCOM_DISPLAY_CLONE_FRAME_BUFFER:
		{
			MutexLocker locker(info->lock);
			void* address = NULL;
			area_id area = vm_clone_area(team_get_current_team_id(),
				"qcom display frame buffer", &address, B_ANY_ADDRESS,
				B_READ_AREA | B_WRITE_AREA, 0, info->bufferArea, true);
			if (area < 0)
				return area;

			area_info areaInfo = {};
			areaInfo.area = area;
			areaInfo.address = address;
			areaInfo.size = info->bufferSize;
			status_t status = user_memcpy(buffer, &areaInfo, sizeof(areaInfo));
			if (status != B_OK)
				vm_delete_area(team_get_current_team_id(), area, true);
			return status;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver


static float
display_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return -1.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "qcom,sc8280xp-mdss") != 0) {
		return -1.0f;
	}

	// MDSS 0 only: MDSS 1 (0x22000000) drives no output on the Dragon
	const char* name;
	if (sDeviceManager->get_attr_string(parent, "fdt/name", &name, false)
			!= B_OK
		|| strcmp(name, "display-subsystem@ae00000") != 0) {
		return -1.0f;
	}

	return 1.0f;
}


static status_t
display_register_device(device_node* parent)
{
	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Qualcomm display subsystem" } },
		{}
	};

	return sDeviceManager->register_node(parent,
		QCOM_DISPLAY_DRIVER_MODULE_NAME, attributes, NULL, NULL);
}


static status_t
display_init_driver(device_node* node, void** _cookie)
{
	display_info* info = (display_info*)calloc(1, sizeof(display_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	sInfo = info;
	*_cookie = info;
	return B_OK;
}


static void
display_uninit_driver(void* cookie)
{
	free(cookie);
}


static status_t
display_register_child_devices(void* cookie)
{
	display_info* info = (display_info*)cookie;
	return sDeviceManager->publish_device(info->node, QCOM_DISPLAY_DEVICE,
		QCOM_DISPLAY_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};

static device_module_info sDisplayDevice = {
	{
		QCOM_DISPLAY_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	display_init_device,
	display_uninit_device,
	NULL,	// removed
	display_open,
	display_close,
	display_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	display_control,
	NULL,	// select
	NULL,	// deselect
};

static driver_module_info sDisplayDriver = {
	{
		QCOM_DISPLAY_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	display_supports_device,
	display_register_device,
	display_init_driver,
	display_uninit_driver,
	display_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sDisplayDriver,
	(module_info*)&sDisplayDevice,
	NULL
};
