/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Allwinner A733's display outputs, for the Radxa Cubie A7S:
	DisplayPort over the second USB-C port.

	The driver owns one frame buffer and shows a region of it on each output
	(the protocol of rpi_display, so that app_server's display layout and the
	Screens preferences work the same). While no display is attached, or the
	output cannot show anything yet, the DisplayPort output stands in for a
	1920x1080 display ("virtual"): the desktop then runs headless, which is
	how the board is brought up and how it runs as a server.

	The USB-C port controller (typec.h) gets the display into DisplayPort
	mode; once it reports hot plug, the display pipe (display_pipe.h) reads
	the sink's EDID, trains the link and shows the frame buffer. */


#include <new>
#include <stdlib.h>
#include <string.h>

#include <Accelerant.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <Drivers.h>
#include <driver_settings.h>
#include <KernelExport.h>

#include <boot_item.h>
#include <frame_buffer_console.h>
#include <graphic_driver.h>
#include <lock.h>
#include <retrace_semaphores.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <sunxi_display.h>

#include "display_pipe.h"
#include "registers.h"
#include "typec.h"


#define SUNXI_DISPLAY_DRIVER_MODULE_NAME	"drivers/graphics/sunxi_display/driver_v1"
#define SUNXI_DISPLAY_DEVICE_MODULE_NAME	"drivers/graphics/sunxi_display/device_v1"

#define ERROR(x...)	dprintf("sunxi_display: " x)
#define INFO(x...)	dprintf("sunxi_display: " x)

// what a headless output stands for
static const uint16 kVirtualWidth = 1920;
static const uint16 kVirtualHeight = 1080;


struct display_info {
	device_node*	node;
	mutex			lock;
	int32			openCount;
	void*			owner;			// the cookie that set a layout

	area_id			sharedArea;
	sunxi_display_shared_info* shared;

	area_id			bufferArea;
	uint8*			buffer;
	phys_addr_t		bufferAddress;
	size_t			bufferSize;

	port_id			changePort;
	int32			changeCode;
	void*			notificationOwner;

	sunxi::TypeCPort* typeC;
	thread_id		pollThread;
	int32			stopping;

	sunxi::DisplayPipe* pipe;
	int32			connectFailures;
	bigtime_t		nextConnect;
	bigtime_t		hotPlugLostAt;
	bigtime_t		retimedAt;		// the last mode change
	int32			retrains;		// link retrains since then

	// the mode the last layout asked for, and the display it was for: a
	// display that comes back gets it again
	sunxi_display_timing wantedTiming;
	uint32			wantedEdidLength;
	uint8			wantedEdid[256];

	// vertical blanks: TCON-TV1 interrupts at each one while it shows the
	// frame buffer; a timer stands in for it while nothing is shown
	RetraceSemaphores* retrace;
	int32			interrupt;		// installed, or -1
	int64			lastBlank;
	timer			retraceTimer;
	bool			retraceTimerAdded;
};


static device_manager_info* sDeviceManager;


//	#pragma mark - timings


static sunxi_display_timing
to_shared(const sunxi::display_timing& timing)
{
	sunxi_display_timing shared = {};
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


static sunxi::display_timing
from_shared(const sunxi_display_timing& shared)
{
	sunxi::display_timing timing;
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
same_timing(const sunxi_display_timing& a, const sunxi_display_timing& b)
{
	return memcmp(&a, &b, sizeof(a)) == 0;
}


/*!	A mode the driver can set: a sane size and a pixel clock the link
	carries (the PLL is checked when the mode is set).
*/
static bool
valid_timing(const sunxi_display_timing& timing, uint32 maxPixelClock)
{
	return timing.h_display >= 320 && timing.h_display <= 4096
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
static sunxi_display_timing
virtual_timing()
{
	sunxi_display_timing timing = {};
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


static void
set_virtual(sunxi_display_output& output)
{
	output.flags |= SUNXI_DISPLAY_OUTPUT_VIRTUAL;
	output.native_width = kVirtualWidth;
	output.native_height = kVirtualHeight;
	output.native_timing = virtual_timing();
	output.timing = output.native_timing;
	output.max_pixel_clock = 0;
}


//	#pragma mark - outputs


static void
init_outputs(display_info* info)
{
	sunxi_display_shared_info& shared = *info->shared;
	shared.output_count = 1;

	sunxi_display_output& output = shared.outputs[0];
	output.id = SUNXI_DISPLAY_OUTPUT_DP0;
	output.flags = SUNXI_DISPLAY_OUTPUT_CONNECTED;
	set_virtual(output);
	output.edid_length = 0;
	INFO("DP-1: no display, standing in for a %ux%u one\n",
		output.native_width, output.native_height);
}


static status_t ensure_buffer(display_info* info, size_t size);


static void
notify_change(display_info* info)
{
	if (info->changePort >= 0) {
		write_port_etc(info->changePort, info->changeCode, NULL, 0,
			B_RELATIVE_TIMEOUT, 0);
	}
}


/*!	Output 0's region of the frame buffer once a layout is set, the top left
	corner at the output's mode size before. A region smaller than the mode
	shows in its top left corner (the display engine does not scale yet).
*/
static bool
has_layout(display_info* info)
{
	const sunxi_display_shared_info& shared = *info->shared;
	return shared.bytes_per_row != 0
		&& (shared.outputs[0].flags & SUNXI_DISPLAY_OUTPUT_ENABLED) != 0;
}


static phys_addr_t
scanout_address(display_info* info)
{
	const sunxi_display_shared_info& shared = *info->shared;
	const sunxi_display_output& output = shared.outputs[0];
	if (!has_layout(info))
		return info->bufferAddress;
	return info->bufferAddress + (phys_addr_t)output.y * shared.bytes_per_row
		+ output.x * 4;
}


static void
scanout_size(display_info* info, uint32& width, uint32& height)
{
	const sunxi_display_output& output = info->shared->outputs[0];
	if (has_layout(info)) {
		width = output.width;
		height = output.height;
	} else {
		width = output.timing.h_display;
		height = output.timing.v_display;
	}
}


static uint32
scanout_bytes_per_row(display_info* info)
{
	const sunxi_display_shared_info& shared = *info->shared;
	if (has_layout(info))
		return shared.bytes_per_row;
	return shared.outputs[0].timing.h_display * 4;
}


static void
update_scanout(display_info* info)
{
	uint32 width, height;
	scanout_size(info, width, height);
	status_t status = info->pipe->SetScanout(scanout_address(info),
		scanout_bytes_per_row(info), width, height);
	if (status != B_OK)
		ERROR("DP-1: cannot show %" B_PRIu32 "x%" B_PRIu32 ": %s\n", width,
			height, strerror(status));
}


/*!	A display is there: read what it is, show the frame buffer on it, and
	let app_server know.
*/
static void
connect_display(display_info* info)
{
	uint8 edid[256];
	uint32 edidLength = 0;
	sunxi::display_timing native;
	status_t status = info->pipe->Discover(info->typeC->Flipped(), edid,
		&edidLength, native);

	MutexLocker locker(info->lock);
	sunxi_display_output& output = info->shared->outputs[0];
	sunxi_display_timing timing = to_shared(native);
	if (status == B_OK) {
		// the same display as before gets the mode the layout asked for
		uint32 maxPixelClock = info->pipe->MaxPixelClock();
		if (info->wantedTiming.h_display != 0
			&& info->wantedEdidLength == edidLength
			&& memcmp(info->wantedEdid, edid, edidLength) == 0
			&& valid_timing(info->wantedTiming, maxPixelClock)) {
			timing = info->wantedTiming;
		}
		output.native_width = native.h_display;
		output.native_height = native.v_display;
		output.native_timing = to_shared(native);
		output.max_pixel_clock = maxPixelClock;
		output.timing = timing;
		status = ensure_buffer(info,
			(size_t)timing.h_display * 4 * timing.v_display);
	}
	if (status == B_OK) {
		status = info->pipe->Enable(from_shared(timing), scanout_address(info),
			scanout_bytes_per_row(info));
		if (status != B_OK && !same_timing(timing, output.native_timing)) {
			ERROR("DP-1: the wanted %ux%u does not come up (%s), using %ux%u\n",
				timing.h_display, timing.v_display, strerror(status),
				native.h_display, native.v_display);
			timing = output.native_timing;
			output.timing = timing;
			status = info->pipe->Enable(native, scanout_address(info),
				scanout_bytes_per_row(info));
		}
		if (status == B_OK)
			update_scanout(info);
	}
	if (status != B_OK) {
		info->connectFailures++;
		info->nextConnect = system_time()
			+ (info->connectFailures < 5 ? 2000000 : 30000000);
		ERROR("DP-1: no picture (%s), trying again in %" B_PRId64 " s\n",
			strerror(status), (info->nextConnect - system_time()) / 1000000);
		set_virtual(output);
		return;
	}

	info->connectFailures = 0;
	output.flags &= ~SUNXI_DISPLAY_OUTPUT_VIRTUAL;
	output.edid_length = edidLength;
	memcpy(output.edid, edid, edidLength);
	INFO("DP-1: %ux%u (%" B_PRIu32 " kHz; preferred %ux%u), %" B_PRIu32
		" lanes at %" B_PRIu32 " Mbit/s\n", timing.h_display,
		timing.v_display, timing.pixel_clock, native.h_display,
		native.v_display, info->pipe->Lanes(),
		info->pipe->LinkRate() / 100);
	notify_change(info);
}


static void
disconnect_display(display_info* info)
{
	MutexLocker locker(info->lock);
	info->pipe->Disable();
	sunxi_display_output& output = info->shared->outputs[0];
	set_virtual(output);
	output.edid_length = 0;
	info->connectFailures = 0;
	info->nextConnect = 0;
	INFO("DP-1: display gone, standing in for a %ux%u one\n",
		output.native_width, output.native_height);
	notify_change(info);
}


/*!	The display dropped hot plug and raised it again while it was shown:
	adapters do so when their picture goes away, a mode change among other
	things, and may have lost the link then. Trains it again (a few times
	after each mode change, so that a sink that drops hot plug on every
	training does not keep this going).
*/
static void
recover_link(display_info* info)
{
	MutexLocker locker(info->lock);
	if (!info->pipe->Enabled() || info->pipe->LinkOk() || info->retrains >= 3)
		return;
	info->retrains++;
	const sunxi_display_timing& timing = info->shared->outputs[0].timing;
	INFO("DP-1: link lost, training it again for %ux%u\n", timing.h_display,
		timing.v_display);
	info->pipe->Disable();
	status_t status = info->pipe->Enable(from_shared(timing),
		scanout_address(info), scanout_bytes_per_row(info));
	if (status == B_OK)
		update_scanout(info);
	else
		ERROR("DP-1: the link does not come back: %s\n", strerror(status));
}


/*!	Runs the USB-C port's state machine: often while something happens
	there, otherwise when the port controller signals (PL3, active low) and
	now and then. Connects and disconnects the display as hot plug says;
	adapters drop HPD now and then while they have no picture, so a drop
	only counts after a second, and after a mode change, which takes their
	picture away for a while, only after several.
*/
static status_t
poll_outputs(void* cookie)
{
	display_info* info = (display_info*)cookie;
	int32 changes = info->typeC->Changes();
	bigtime_t lastPoll = 0;
	bool hotPlugBounced = false;
	while (atomic_get(&info->stopping) == 0) {
		snooze(20000);
		bigtime_t now = system_time();
		bool interrupt = !sunxi::r_pin_get(0, 3);
		if (!interrupt && info->typeC->Idle() && now - lastPoll < 250000)
			continue;
		lastPoll = now;
		info->typeC->Poll();

		int32 current = info->typeC->Changes();
		if (current != changes) {
			changes = current;
			INFO("DP-1: %s, HPD %s\n", info->typeC->DisplayPortReady()
					? "alt mode ready" : "no alt mode",
				info->typeC->HotPlug() ? "high" : "low");
		}

		if (info->pipe == NULL)
			continue;
		bool present = info->typeC->DisplayPortReady()
			&& info->typeC->HotPlug();
		if (present) {
			if (info->hotPlugLostAt != 0 && info->pipe->Enabled())
				hotPlugBounced = true;
			info->hotPlugLostAt = 0;
		} else if (info->hotPlugLostAt == 0)
			info->hotPlugLostAt = now;

		if (info->pipe->Enabled()) {
			bigtime_t grace = now - info->retimedAt < 10000000
				? 8000000 : 1000000;
			if (!present && now - info->hotPlugLostAt > grace) {
				hotPlugBounced = false;
				disconnect_display(info);
			} else if (present && hotPlugBounced) {
				hotPlugBounced = false;
				// give the sink a moment to come up before asking it
				snooze(100000);
				recover_link(info);
			}
		} else if (present && now >= info->nextConnect)
			connect_display(info);
	}
	return B_OK;
}


//	#pragma mark - vertical blank


static int32
display_interrupt(void* data)
{
	display_info* info = (display_info*)data;
	if (info->pipe == NULL || !info->pipe->AcknowledgeBlank())
		return B_UNHANDLED_INTERRUPT;

	atomic_set64(&info->lastBlank, system_time());
	return info->retrace->Release() ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


/*!	At 60 Hz, while the output shows nothing (no display, or the display
	is being retimed), so that waiting for a blank never waits long.
*/
static int32
retrace_timer(timer* timer)
{
	display_info* info = (display_info*)timer->user_data;
	if (system_time() - atomic_get64(&info->lastBlank) < 100000)
		return B_HANDLED_INTERRUPT;
	return info->retrace->Release() ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
}


/*!	TCON-TV1's interrupt from the device tree; the driver's addresses are
	the A733's, so its own number when the tree has none.
*/
static uint32
display_interrupt_number(display_info* info)
{
	uint64 interrupt = sunxi::kTconTv1Interrupt;
	device_node* parent = sDeviceManager->get_parent_node(info->node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (parent != NULL && sDeviceManager->get_driver(parent,
			(driver_module_info**)&fdt, (void**)&device) == B_OK
		&& !fdt->get_interrupt(device, 0, NULL, &interrupt)) {
		interrupt = sunxi::kTconTv1Interrupt;
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
	if (info->retrace == NULL || info->pipe == NULL)
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
}


static void
uninit_retrace(display_info* info)
{
	if (info->retraceTimerAdded)
		cancel_timer(&info->retraceTimer);
	if (info->interrupt >= 0)
		remove_io_interrupt_handler(info->interrupt, display_interrupt, info);
	delete info->retrace;
	info->retrace = NULL;
}


//	#pragma mark - frame buffer


/*!	A frame buffer of at least \a size bytes: contiguous, write-combining.
	An existing one that is large enough stays.
*/
static status_t
ensure_buffer(display_info* info, size_t size)
{
	size = ROUNDUP(size, B_PAGE_SIZE);
	if (info->bufferArea >= 0 && info->bufferSize >= size)
		return B_OK;

	// The display engine reads 32-bit addresses.
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 32;
	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "sunxi display frame buffer",
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


static status_t
set_layout(display_info* info, const sunxi_display_layout& layout)
{
	sunxi_display_shared_info& shared = *info->shared;

	if (layout.version != SUNXI_DISPLAY_VERSION || layout.width == 0
		|| layout.height == 0 || layout.width > 8192 || layout.height > 8192) {
		return B_BAD_VALUE;
	}

	uint32 enabled = 0;
	for (uint32 i = 0; i < shared.output_count; i++) {
		const sunxi_display_output& request = layout.outputs[i];
		if ((request.flags & SUNXI_DISPLAY_OUTPUT_ENABLED) == 0)
			continue;
		if ((shared.outputs[i].flags & SUNXI_DISPLAY_OUTPUT_CONNECTED) == 0
			|| request.x < 0 || request.y < 0 || request.width == 0
			|| request.height == 0
			|| request.x + request.width > (int32)layout.width
			|| request.y + request.height > (int32)layout.height) {
			return B_BAD_VALUE;
		}
		if (request.timing.h_display != 0
			&& !valid_timing(request.timing,
				shared.outputs[i].max_pixel_clock)) {
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

	for (uint32 i = 0; i < shared.output_count; i++) {
		sunxi_display_output& output = shared.outputs[i];
		const sunxi_display_output& request = layout.outputs[i];
		if ((output.flags & SUNXI_DISPLAY_OUTPUT_CONNECTED) == 0)
			continue;

		uint32 kept = output.flags & (SUNXI_DISPLAY_OUTPUT_CONNECTED
			| SUNXI_DISPLAY_OUTPUT_VIRTUAL);
		if ((request.flags & SUNXI_DISPLAY_OUTPUT_ENABLED) == 0) {
			output.flags = kept;
			continue;
		}

		output.flags = kept | SUNXI_DISPLAY_OUTPUT_ENABLED
			| (request.flags & SUNXI_DISPLAY_OUTPUT_MIRROR);
		sunxi_display_timing timing = request.timing.h_display != 0
			? request.timing : output.native_timing;
		if (i == 0 && (output.flags & SUNXI_DISPLAY_OUTPUT_VIRTUAL) == 0) {
			info->wantedTiming = timing;
			info->wantedEdidLength = output.edid_length;
			memcpy(info->wantedEdid, output.edid, output.edid_length);
		}
		// a headless output just takes the mode
		if ((output.flags & SUNXI_DISPLAY_OUTPUT_VIRTUAL) != 0)
			output.timing = timing;
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
			(request.flags & SUNXI_DISPLAY_OUTPUT_MIRROR) != 0 ? ", mirror" : "",
			(output.flags & SUNXI_DISPLAY_OUTPUT_VIRTUAL) != 0
				? ", headless" : "");
	}

	shared.width = layout.width;
	shared.height = layout.height;
	shared.bytes_per_row = bytesPerRow;

	if (info->pipe != NULL && info->pipe->Enabled()) {
		sunxi_display_output& output = shared.outputs[0];
		sunxi_display_timing timing = info->wantedTiming;
		if ((output.flags & SUNXI_DISPLAY_OUTPUT_ENABLED) != 0
			&& (output.flags & SUNXI_DISPLAY_OUTPUT_VIRTUAL) == 0
			&& timing.h_display != 0 && !same_timing(timing, output.timing)) {
			// a new mode: the whole path again, with the new timing
			INFO("DP-1: mode %ux%u, %" B_PRIu32 " kHz\n", timing.h_display,
				timing.v_display, timing.pixel_clock);
			info->pipe->Disable();
			info->retimedAt = system_time();
			info->retrains = 0;
			status = info->pipe->Enable(from_shared(timing),
				scanout_address(info), scanout_bytes_per_row(info));
			if (status != B_OK) {
				ERROR("DP-1: %ux%u does not come up (%s), back to %ux%u\n",
					timing.h_display, timing.v_display, strerror(status),
					output.native_timing.h_display,
					output.native_timing.v_display);
				timing = output.native_timing;
				status = info->pipe->Enable(from_shared(timing),
					scanout_address(info), scanout_bytes_per_row(info));
			}
			if (status == B_OK)
				output.timing = timing;
			else
				ERROR("DP-1: no picture after the mode change: %s\n",
					strerror(status));
		}
		if (info->pipe->Enabled())
			update_scanout(info);
	}

	// the kernel's console and debugger follow
	frame_buffer_update((addr_t)info->buffer, layout.width, layout.height, 32,
		bytesPerRow);
	return B_OK;
}


static void
release_layout(display_info* info)
{
	sunxi_display_shared_info& shared = *info->shared;
	for (uint32 i = 0; i < shared.output_count; i++) {
		shared.outputs[i].flags &= SUNXI_DISPLAY_OUTPUT_CONNECTED
			| SUNXI_DISPLAY_OUTPUT_VIRTUAL;
	}
}


//	#pragma mark - device


static status_t
display_init_device(void* _info, void** _cookie)
{
	display_info* info = (display_info*)_info;

	info->sharedArea = create_area("sunxi display shared info",
		(void**)&info->shared, B_ANY_KERNEL_ADDRESS,
		ROUNDUP(sizeof(sunxi_display_shared_info), B_PAGE_SIZE), B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (info->sharedArea < 0)
		return info->sharedArea;
	memset(info->shared, 0, sizeof(sunxi_display_shared_info));
	info->shared->version = SUNXI_DISPLAY_VERSION;
	info->bufferArea = -1;

	init_outputs(info);

	// a frame buffer for the first output, taken while contiguous memory is
	// still easy to find
	const sunxi_display_output& first = info->shared->outputs[0];
	status_t status = ensure_buffer(info,
		(size_t)first.native_width * 4 * first.native_height);
	if (status != B_OK) {
		ERROR("no memory for the frame buffer: %s\n", strerror(status));
		delete_area(info->sharedArea);
		return status;
	}

	mutex_init(&info->lock, "sunxi display");
	info->changePort = -1;

	// the path from the display engine to the USB-C port
	info->pipe = new(std::nothrow) sunxi::DisplayPipe;
	if (info->pipe != NULL) {
		status = info->pipe->Init();
		if (status != B_OK) {
			ERROR("the display path does not come up: %s\n",
				strerror(status));
			delete info->pipe;
			info->pipe = NULL;
		}
	}

	// the USB-C port the DisplayPort output goes through
	info->pollThread = -1;
	info->typeC = new(std::nothrow) sunxi::TypeCPort;
	if (info->typeC != NULL) {
		sunxi::r_pin_set_function(0, 3, sunxi::PIN_FUNCTION_INPUT);
		status = info->typeC->Init();
		if (status != B_OK) {
			ERROR("no USB-C port controller: %s\n", strerror(status));
			delete info->typeC;
			info->typeC = NULL;
		}
	}
	if (info->typeC != NULL) {
		info->pollThread = spawn_kernel_thread(poll_outputs,
			"sunxi display outputs", B_NORMAL_PRIORITY, info);
		if (info->pollThread >= 0)
			resume_thread(info->pollThread);
	}

	init_retrace(info);

	*_cookie = info;
	return B_OK;
}


static void
display_uninit_device(void* cookie)
{
	display_info* info = (display_info*)cookie;
	atomic_set(&info->stopping, 1);
	if (info->pollThread >= 0) {
		status_t result;
		wait_for_thread(info->pollThread, &result);
	}
	delete info->typeC;
	if (info->pipe != NULL)
		info->pipe->Disable();
	uninit_retrace(info);
	delete info->pipe;
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
	void* handle = load_driver_settings("sunxi_display");
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
			return user_strlcpy((char*)buffer, SUNXI_DISPLAY_ACCELERANT,
				B_FILE_NAME_LENGTH) < B_OK ? B_BAD_ADDRESS : B_OK;

		case SUNXI_DISPLAY_GET_SHARED_AREA:
			return user_memcpy(buffer, &info->sharedArea, sizeof(area_id));

		case SUNXI_DISPLAY_GET_STATE:
		{
			if (length != sizeof(sunxi_display_shared_info))
				return B_BAD_VALUE;
			MutexLocker locker(info->lock);
			return user_memcpy(buffer, info->shared, sizeof(*info->shared));
		}

		case SUNXI_DISPLAY_SET_CHANGE_PORT:
		{
			sunxi_display_change_port request;
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

		case SUNXI_DISPLAY_SET_LAYOUT:
		{
			sunxi_display_layout layout;
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

		case SUNXI_DISPLAY_DEBUG_REGISTER:
		{
			if (!debug_registers_allowed() || info->pipe == NULL)
				return B_NOT_ALLOWED;
			sunxi_display_register request;
			if (length != sizeof(request)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK) {
				return B_BAD_VALUE;
			}
			MutexLocker locker(info->lock);
			status_t status = info->pipe->DebugRegister(request.address,
				request.value, request.write != 0);
			if (status != B_OK)
				return status;
			return user_memcpy(buffer, &request, sizeof(request));
		}

		case SUNXI_DISPLAY_GET_RETRACE_SEMAPHORE:
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

		case SUNXI_DISPLAY_CLONE_FRAME_BUFFER:
		{
			MutexLocker locker(info->lock);
			void* address = NULL;
			area_id area = vm_clone_area(team_get_current_team_id(),
				"sunxi display frame buffer", &address, B_ANY_ADDRESS,
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
		|| strcmp(compatible, "allwinner,sun60i-a733-display-engine") != 0) {
		return -1.0f;
	}

	return 1.0f;
}


static status_t
display_register_device(device_node* parent)
{
	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Allwinner display engine" } },
		{}
	};

	return sDeviceManager->register_node(parent,
		SUNXI_DISPLAY_DRIVER_MODULE_NAME, attributes, NULL, NULL);
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
	return sDeviceManager->publish_device(info->node, SUNXI_DISPLAY_DEVICE,
		SUNXI_DISPLAY_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};

static device_module_info sDisplayDevice = {
	{
		SUNXI_DISPLAY_DEVICE_MODULE_NAME,
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
		SUNXI_DISPLAY_DRIVER_MODULE_NAME,
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
