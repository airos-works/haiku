/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Raspberry Pi 4's HDMI outputs through the VideoCore firmware.

	The firmware sets the outputs' initial video modes and runs the
	compositor (HVS). This driver owns one frame buffer and asks the firmware
	for one plane per output, each showing a region of that buffer scaled to
	the output's mode: the "firmware KMS" property tags. The accelerant
	decides the regions (app_server's display layout). */


#include <new>
#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <Drivers.h>
#include <KernelExport.h>

#include <boot_item.h>
#include <frame_buffer_console.h>
#include <graphic_driver.h>
#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <retrace_follower.h>
#include <rpi_display.h>
#include <rpi_firmware.h>

#include "edid_timing.h"


#define RPI_DISPLAY_DRIVER_MODULE_NAME	"drivers/graphics/rpi_display/driver_v1"
#define RPI_DISPLAY_DEVICE_MODULE_NAME	"drivers/graphics/rpi_display/device_v1"

#define ERROR(x...)	dprintf("rpi_display: " x)
#define INFO(x...)	dprintf("rpi_display: " x)

// property tags
#define TAG_GET_EDID_BLOCK_DISPLAY		0x00030023
#define TAG_FB_GET_PHYSICAL_SIZE		0x00040003
#define TAG_FB_GET_NUM_DISPLAYS			0x00040013
#define TAG_FB_GET_DISPLAY_ID			0x00040016
#define TAG_FB_SET_DISPLAY_NUM			0x00048013
#define TAG_SET_TIMING					0x00048017
#define TAG_SET_DISPLAY_POWER			0x00048019
#define TAG_SET_PLANE					0x00048015

#define VC_IMAGE_ARGB8888				43
#define VC_IMAGE_XRGB8888				44
// plane ids are the firmware's, counted across the displays. The main
// planes use the fork's three-a-display numbering; the cursor rides the
// fuller scheme Linux's firmware-KMS driver gets (eight a display).
#define PLANES_PER_DISPLAY				3
#define CURSOR_PLANES_PER_DISPLAY		8
#define PLANE_LAYER						1
	// above the firmware's own frame buffer (the boot console)
#define CURSOR_PLANE_OFFSET				7
#define CURSOR_PLANE_LAYER				(PLANE_LAYER + 1)

// The VideoCore sees the ARM's first gigabyte at this bus address.
#define VC_BUS_OFFSET					0xc0000000

// The compositor (HVS) of the BCM2711, read only: the firmware drives it
// (Linux's vc4_regs.h, vc4_hvs.c and vc4_crtc.c). HDMI0 is pixel valve 2,
// fed by the compositor's output 4; HDMI1 pixel valve 4, output 5. Each
// output takes one of the three channels, as a mux says.
#define HVS_DISPDITHER					0x14	// output 5's mux: 31..30
#define HVS_DISPEOLN					0x18	// output 4's mux: 31..30
#define HVS_DISPCTRLX(x)				(0x40 + (x) * 0x10)
#define HVS_DISPCTRLX_ENABLE			(1u << 31)
#define HVS_DISPSTAT1					0x58	// frame counts 0 and 1
#define HVS_DISPSTAT2					0x68	// frame count 2
#define HVS_MUX_NONE					3

struct firmware_plane {
	uint8	display;
	uint8	plane_id;
	uint8	image_type;
	int8	layer;
	uint16	width;
	uint16	height;
	uint16	pitch;
	uint16	vpitch;
	uint32	src_x;			// 16.16
	uint32	src_y;
	uint32	src_w;
	uint32	src_h;
	int16	dst_x;
	int16	dst_y;
	uint16	dst_w;
	uint16	dst_h;
	uint8	alpha;
	uint8	num_planes;
	uint8	is_vu;
	uint8	color_encoding;
	uint32	planes[4];
	uint32	transform;
};

struct display_info {
	device_node*	node;
	mutex			lock;
	int32			openCount;
	void*			owner;			// the cookie that set a layout

	area_id			sharedArea;
	rpi_display_shared_info* shared;

	area_id			bufferArea;
	uint8*			buffer;
	phys_addr_t		bufferAddress;
	size_t			bufferSize;

	area_id			hpdAreas[2];
	volatile uint32*	hpdRegisters[2];
	thread_id		pollThread;
	int32			stopping;
	port_id			changePort;
	int32			changeCode;
	void*			notificationOwner;

	// vertical blanks, from the compositor's frame counter
	area_id			hvsArea;
	volatile uint8*	hvs;
	RetraceSemaphores* retrace;
	class HvsRetraceFollower* retraceFollower;

	// a hardware cursor, composited by the firmware over the frame buffer;
	// the position is the hotspot, in frame buffer pixels
	area_id			cursorArea;
	uint8*			cursorBuffer;
	phys_addr_t		cursorBufferAddress;
	bool			cursorDefined;
	bool			cursorVisible;
	int32			cursorX;
	int32			cursorY;
	rpi_display_cursor cursor;
	// what the cursor rides: overridable for experiments
	uint8			cursorPlaneOffset;
	int8			cursorLayer;
	uint8			cursorImageType;
};


static device_manager_info* sDeviceManager;
static rpi_firmware_module_info* sFirmware;


//	#pragma mark - firmware


static status_t
set_plane(const firmware_plane& plane)
{
	firmware_plane request = plane;
	return sFirmware->property(TAG_SET_PLANE, &request, sizeof(request));
}


static void
remove_plane(uint32 index, uint32 display)
{
	firmware_plane plane = {};
	plane.display = display;
	plane.plane_id = index * PLANES_PER_DISPLAY;
	set_plane(plane);
}


static void
read_edid(rpi_display_output& output)
{
	output.edid_length = 0;
	memset(output.edid, 0, sizeof(output.edid));
	for (uint32 block = 0; block < 2; block++) {
		struct {
			uint32 block;
			uint32 display;
			uint8 data[128];
		} edid = {block, output.id, {}};
		if (sFirmware->property(TAG_GET_EDID_BLOCK_DISPLAY, &edid,
				sizeof(edid)) != B_OK || edid.display != 0
			|| !valid_edid_block(edid.data, block == 0)) {
			break;
		}
		memcpy(output.edid + block * 128, edid.data, 128);
		output.edid_length = (block + 1) * 128;
		if (block == 0 && edid.data[126] == 0)
			break;
	}
}


/*! The firmware's boot-time modes, mapped to stable physical connectors. */
static status_t
read_outputs(display_info* info)
{
	rpi_display_shared_info& shared = *info->shared;
	shared.output_count = RPI_DISPLAY_MAX_OUTPUTS;
	shared.outputs[0].id = 2;
	shared.outputs[1].id = 7;
	uint32 count = 0;
	status_t status = sFirmware->property(TAG_FB_GET_NUM_DISPLAYS, &count,
		sizeof(count));
	if (status != B_OK)
		return status;
	if (count > RPI_DISPLAY_MAX_OUTPUTS)
		count = RPI_DISPLAY_MAX_OUTPUTS;
	bool found = false;
	for (uint32 i = 0; i < count; i++) {
		uint32 number = i;
		sFirmware->property(TAG_FB_SET_DISPLAY_NUM, &number, sizeof(number));
		uint32 id[2] = {i, 0};
		uint32 size[2] = {0, 0};
		if (sFirmware->property(TAG_FB_GET_DISPLAY_ID, id, sizeof(id)) != B_OK
			|| sFirmware->property(TAG_FB_GET_PHYSICAL_SIZE, size,
				sizeof(size)) != B_OK || size[0] == 0 || size[1] == 0
			|| (id[0] != 2 && id[0] != 7)) {
			continue;
		}
		uint32 index = id[0] == 2 ? 0 : 1;
		rpi_display_output& output = shared.outputs[index];
		output.flags = RPI_DISPLAY_OUTPUT_CONNECTED;
		output.native_width = size[0];
		output.native_height = size[1];
		read_edid(output);
		found = true;
		INFO("HDMI%" B_PRIu32 ": display id %" B_PRIu32 ", %ux%u, %"
			B_PRIu32 " bytes of EDID\n", index, output.id,
			output.native_width, output.native_height, output.edid_length);
	}
	uint32 number = 0;
	sFirmware->property(TAG_FB_SET_DISPLAY_NUM, &number, sizeof(number));
	return found ? B_OK : B_DEVICE_NOT_FOUND;
}


static status_t
connect_output(rpi_display_output& output)
{
	rpi_display_output connected = {};
	connected.id = output.id;
	read_edid(connected);
	firmware_timing timing;
	if (!preferred_edid_timing(connected.edid, connected.edid_length, timing))
		return B_DEV_NOT_READY;
	timing.display = output.id;
	status_t status = sFirmware->property(TAG_SET_TIMING, &timing,
		sizeof(timing));
	if (status != B_OK)
		return status;
	uint32 power[2] = {output.id, 1};
	status = sFirmware->property(TAG_SET_DISPLAY_POWER, power, sizeof(power));
	if (status != B_OK)
		return status;
	connected.native_width = timing.hdisplay;
	connected.native_height = timing.vdisplay;
	connected.flags = RPI_DISPLAY_OUTPUT_CONNECTED;
	output = connected;
	return B_OK;
}


static int32
poll_outputs(void* cookie)
{
	display_info* info = (display_info*)cookie;
	bool previous[2] = {false, false};
	bigtime_t retryAt[2] = {0, 0};
	while (atomic_get(&info->stopping) == 0) {
		snooze(250000);
		MutexLocker locker(info->lock);
		bool changed = false;
		for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
			if (info->hpdRegisters[i] == NULL)
				continue;
			bool present = (*info->hpdRegisters[i] & 1) != 0;
			// Two matching samples suppress short HPD pulses and contact bounce.
			bool stable = previous[i] == present;
			previous[i] = present;
			rpi_display_output& output = info->shared->outputs[i];
			bool connected = (output.flags & RPI_DISPLAY_OUTPUT_CONNECTED) != 0;
			if (!stable || connected == present || system_time() < retryAt[i])
				continue;
			if (present) {
				status_t status = connect_output(output);
				if (status != B_OK) {
					retryAt[i] = system_time() + 2000000;
					continue;
				}
			} else {
				remove_plane(i, output.id);
				output.flags = 0;
				output.edid_length = 0;
				retryAt[i] = 0;
			}
			changed = true;
			INFO("HDMI%" B_PRIu32 " %s (%ux%u)\n", i,
				present ? "connected" : "disconnected", output.native_width,
				output.native_height);
		}
		if (changed && info->changePort >= 0) {
			// Never let a full or abandoned app_server port block the driver.
			write_port_etc(info->changePort, info->changeCode, NULL, 0,
				B_RELATIVE_TIMEOUT, 0);
		}
	}
	return B_OK;
}


//	#pragma mark - vertical blank


/*!	The firmware gives no interrupt for the blanks, but the compositor
	counts the frames of each channel, and the count goes up at the end of
	a frame. The one followed is the channel of the first output shown.
*/
class HvsRetraceFollower : public RetraceFollower {
public:
	HvsRetraceFollower(display_info* info)
		:
		RetraceFollower(*info->retrace, 0x3f),
		fInfo(info),
		fChannel(-1),
		fLooks(0)
	{
	}

protected:
	virtual int64 FrameCount()
	{
		// the outputs and their channels change now and then: once a second
		if (fChannel < 0 || ++fLooks >= 300) {
			fChannel = _Channel();
			fLooks = 0;
		}
		if (fChannel < 0)
			return -1;
		switch (fChannel) {
			case 0:
				return (_Read(HVS_DISPSTAT1) >> 20) & 0x3f;
			case 1:
				return (_Read(HVS_DISPSTAT1) >> 14) & 0x3f;
			default:
				return (_Read(HVS_DISPSTAT2) >> 14) & 0x3f;
		}
	}

private:
	uint32 _Read(uint32 offset)
	{
		return *(volatile uint32*)(fInfo->hvs + offset);
	}

	int32 _Channel()
	{
		const rpi_display_shared_info& shared = *fInfo->shared;
		for (uint32 i = 0; i < shared.output_count; i++) {
			if ((shared.outputs[i].flags & RPI_DISPLAY_OUTPUT_ENABLED) == 0)
				continue;
			uint32 mux = _Read(i == 0 ? HVS_DISPEOLN : HVS_DISPDITHER) >> 30;
			if (mux == HVS_MUX_NONE
				|| (_Read(HVS_DISPCTRLX(mux)) & HVS_DISPCTRLX_ENABLE) == 0) {
				continue;
			}
			return mux;
		}
		return -1;
	}

	display_info*	fInfo;
	int32			fChannel;
	int32			fLooks;
};


/*!	The compositor's registers, read only, from the device tree node the
	driver is attached to.
*/
static void
map_compositor(display_info* info)
{
	info->hvsArea = -1;
	device_node* parent = sDeviceManager->get_parent_node(info->node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	uint64 base = 0, size = 0;
	status_t status = parent != NULL ? sDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device) : B_ERROR;
	if (status == B_OK && !fdt->get_reg(device, 0, &base, &size))
		status = B_BAD_DATA;
	if (parent != NULL)
		sDeviceManager->put_node(parent);
	if (status != B_OK || size < HVS_DISPSTAT2 + 4) {
		ERROR("no compositor registers, no vertical blanks\n");
		return;
	}

	phys_addr_t page = base & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* address;
	info->hvsArea = map_physical_memory("BCM2711 HVS", page, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA,
		&address);
	if (info->hvsArea < 0) {
		ERROR("cannot map the compositor: %s\n", strerror(info->hvsArea));
		return;
	}
	info->hvs = (volatile uint8*)address + (base - page);
}


//	#pragma mark - frame buffer


/*!	A frame buffer of at least \a size bytes: contiguous, where the
	VideoCore reaches it, write-combining. An existing one that is large
	enough stays.
*/
static status_t
ensure_buffer(display_info* info, size_t size)
{
	size = ROUNDUP(size, B_PAGE_SIZE);
	if (info->bufferArea >= 0 && info->bufferSize >= size)
		return B_OK;

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 30;
	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "rpi display frame buffer",
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
ensure_cursor_buffer(display_info* info)
{
	if (info->cursorArea >= 0)
		return B_OK;

	size_t size = ROUNDUP(RPI_DISPLAY_CURSOR_MAX * RPI_DISPLAY_CURSOR_MAX * 4,
		B_PAGE_SIZE);
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 30;
	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "rpi display cursor",
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

	info->cursorArea = area;
	info->cursorBuffer = (uint8*)address;
	info->cursorBufferAddress = entry.address;
	return B_OK;
}


/*!	Where an output's region lands on it: the aspect-kept fit, centered.
	Shared by the frame buffer's plane and the cursor's, which rides over
	the same destination.
*/
static void
output_dest(uint32 nativeWidth, uint32 nativeHeight, uint32 width,
	uint32 height, int32* destX, int32* destY, uint32* destWidth,
	uint32* destHeight)
{
	uint32 w = nativeWidth;
	uint32 h = (uint64)height * w / width;
	if (h > nativeHeight) {
		h = nativeHeight;
		w = (uint64)width * h / height;
	}
	// a pixel of rounding is not worth a border
	if (nativeWidth - w <= 2)
		w = nativeWidth;
	if (nativeHeight - h <= 2)
		h = nativeHeight;
	*destX = (nativeWidth - w) / 2;
	*destY = (nativeHeight - h) / 2;
	*destWidth = w;
	*destHeight = h;
}


/*!	Puts the cursor's plane on the output whose region holds the hotspot,
	updated in place when it already rides there; takes it off the others.
	A zeroed plane removes it, which is how a hidden cursor is drawn.
	Caller holds the driver lock.
*/
static status_t
update_cursor_plane(display_info* info)
{
	const rpi_display_shared_info& shared = *info->shared;
	int32 on = -1;
	if (info->cursorDefined && info->cursorVisible) {
		for (uint32 i = 0; i < shared.output_count; i++) {
			const rpi_display_output& output = shared.outputs[i];
			if ((output.flags & RPI_DISPLAY_OUTPUT_ENABLED) == 0)
				continue;
			if (info->cursorX >= output.x
				&& info->cursorX < output.x + output.width
				&& info->cursorY >= output.y
				&& info->cursorY < output.y + output.height) {
				on = (int32)i;
				break;
			}
		}
	}

	for (uint32 i = 0; i < shared.output_count; i++) {
		const rpi_display_output& output = shared.outputs[i];
		if ((output.flags & RPI_DISPLAY_OUTPUT_ENABLED) == 0
			|| (output.flags & RPI_DISPLAY_OUTPUT_CONNECTED) == 0)
			continue;

		firmware_plane plane = {};
		plane.display = output.id;
		plane.plane_id = i * CURSOR_PLANES_PER_DISPLAY + info->cursorPlaneOffset;
		if ((int32)i == on) {
			int32 destX, destY;
			uint32 destWidth, destHeight;
			output_dest(output.native_width, output.native_height,
				output.width, output.height, &destX, &destY, &destWidth,
				&destHeight);
			double scaleX = (double)destWidth / output.width;
			double scaleY = (double)destHeight / output.height;

			plane.image_type = info->cursorImageType;
			plane.layer = info->cursorLayer;
			plane.width = info->cursor.width;
			plane.height = info->cursor.height;
			plane.pitch = info->cursor.width * 4;
			plane.vpitch = 0;
			plane.src_x = 0;
			plane.src_y = 0;
			plane.src_w = info->cursor.width << 16;
			plane.src_h = info->cursor.height << 16;
			plane.dst_x = (int16)(destX + (int32)((info->cursorX - output.x)
				* scaleX - info->cursor.hot_x * scaleX
				+ (info->cursorX >= output.x ? 0.5 : -0.5)));
			plane.dst_y = (int16)(destY + (int32)((info->cursorY - output.y)
				* scaleY - info->cursor.hot_y * scaleY
				+ (info->cursorY >= output.y ? 0.5 : -0.5)));
			int32 cursorWidth
				= (int32)(info->cursor.width * scaleX + 0.5);
			int32 cursorHeight
				= (int32)(info->cursor.height * scaleY + 0.5);
			plane.dst_w = (uint16)(cursorWidth > 1 ? cursorWidth : 1);
			plane.dst_h = (uint16)(cursorHeight > 1 ? cursorHeight : 1);
			plane.alpha = 0xff;
			plane.num_planes = 1;
			plane.planes[0]
				= (uint32)info->cursorBufferAddress | VC_BUS_OFFSET;
		}
		status_t status = set_plane(plane);
		if (status != B_OK) {
			ERROR("the firmware did not take output %" B_PRIu32 "'s cursor "
				"plane: %s\n", i, strerror(status));
			return status;
		}
	}
	return B_OK;
}


static status_t
set_layout(display_info* info, const rpi_display_layout& layout)
{
	rpi_display_shared_info& shared = *info->shared;

	if (layout.version != RPI_DISPLAY_VERSION || layout.width == 0
		|| layout.height == 0 || layout.width > 8192 || layout.height > 8192) {
		return B_BAD_VALUE;
	}

	uint32 enabled = 0;
	for (uint32 i = 0; i < shared.output_count; i++) {
		const rpi_display_output& request = layout.outputs[i];
		if ((request.flags & RPI_DISPLAY_OUTPUT_ENABLED) == 0)
			continue;
		if ((shared.outputs[i].flags & RPI_DISPLAY_OUTPUT_CONNECTED) == 0
			|| request.x < 0 || request.y < 0 || request.width == 0
			|| request.height == 0
			|| request.x + request.width > (int32)layout.width
			|| request.y + request.height > (int32)layout.height) {
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
		rpi_display_output& output = shared.outputs[i];
		const rpi_display_output& request = layout.outputs[i];
		if ((output.flags & RPI_DISPLAY_OUTPUT_CONNECTED) == 0)
			continue;

		if ((request.flags & RPI_DISPLAY_OUTPUT_ENABLED) == 0) {
			output.flags = RPI_DISPLAY_OUTPUT_CONNECTED;
			remove_plane(i, output.id);
			continue;
		}

		// the region as large as it gets on the output with its shape kept
		int32 destX, destY;
		uint32 destWidth, destHeight;
		output_dest(output.native_width, output.native_height, request.width,
			request.height, &destX, &destY, &destWidth, &destHeight);

		firmware_plane plane = {};
		plane.display = output.id;
		plane.plane_id = i * PLANES_PER_DISPLAY;
		plane.image_type = VC_IMAGE_XRGB8888;
		plane.layer = PLANE_LAYER;
		plane.width = layout.width;
		plane.height = layout.height;
		plane.pitch = bytesPerRow;
		plane.vpitch = 1;
		plane.src_x = request.x << 16;
		plane.src_y = request.y << 16;
		plane.src_w = request.width << 16;
		plane.src_h = request.height << 16;
		plane.dst_x = (int16)destX;
		plane.dst_y = (int16)destY;
		plane.dst_w = (uint16)destWidth;
		plane.dst_h = (uint16)destHeight;
		plane.alpha = 0xff;
		plane.num_planes = 1;
		plane.planes[0] = (uint32)info->bufferAddress | VC_BUS_OFFSET;
		status = set_plane(plane);
		if (status != B_OK) {
			ERROR("the firmware did not take output %" B_PRIu32 "'s plane: "
				"%s\n", i, strerror(status));
			return status;
		}

		output.flags = RPI_DISPLAY_OUTPUT_CONNECTED | RPI_DISPLAY_OUTPUT_ENABLED
			| (request.flags & RPI_DISPLAY_OUTPUT_MIRROR);
		output.x = request.x;
		output.y = request.y;
		output.width = request.width;
		output.height = request.height;
		output.mode_width = request.mode_width;
		output.mode_height = request.mode_height;
		output.scale = request.scale;
		output.render_scale = request.render_scale;

		INFO("output %" B_PRIu32 ": %ux%u at %" B_PRId32 ",%" B_PRId32
			" of %" B_PRIu32 "x%" B_PRIu32 " shown %" B_PRIu32 "x%" B_PRIu32
			"%s\n", i, request.width, request.height, request.x, request.y,
			layout.width, layout.height, destWidth, destHeight,
			(request.flags & RPI_DISPLAY_OUTPUT_MIRROR) != 0 ? ", mirror" : "");
	}

	shared.width = layout.width;
	shared.height = layout.height;
	shared.bytes_per_row = bytesPerRow;

	// the cursor rides over the new planes
	update_cursor_plane(info);

	// the kernel's console and debugger follow
	frame_buffer_update((addr_t)info->buffer, layout.width, layout.height, 32,
		bytesPerRow);
	return B_OK;
}


/*!	Back to the firmware's own frame buffer, for the console. */
static void
release_layout(display_info* info)
{
	rpi_display_shared_info& shared = *info->shared;
	for (uint32 i = 0; i < shared.output_count; i++) {
		if ((shared.outputs[i].flags & RPI_DISPLAY_OUTPUT_ENABLED) != 0) {
			remove_plane(i, shared.outputs[i].id);
			// the cursor rides its own plane
			firmware_plane cursor = {};
			cursor.display = shared.outputs[i].id;
			cursor.plane_id = i * CURSOR_PLANES_PER_DISPLAY + info->cursorPlaneOffset;
			set_plane(cursor);
		}
		shared.outputs[i].flags &= RPI_DISPLAY_OUTPUT_CONNECTED;
	}

	frame_buffer_boot_info* bootInfo = (frame_buffer_boot_info*)get_boot_item(
		FRAME_BUFFER_BOOT_INFO, NULL);
	if (bootInfo != NULL) {
		frame_buffer_update(bootInfo->frame_buffer, bootInfo->width,
			bootInfo->height, bootInfo->depth, bootInfo->bytes_per_row);
	}
}


//	#pragma mark - device


static status_t
display_init_device(void* _info, void** _cookie)
{
	display_info* info = (display_info*)_info;

	info->sharedArea = create_area("rpi display shared info",
		(void**)&info->shared, B_ANY_KERNEL_ADDRESS,
		ROUNDUP(sizeof(rpi_display_shared_info), B_PAGE_SIZE), B_FULL_LOCK,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA | B_CLONEABLE_AREA);
	if (info->sharedArea < 0)
		return info->sharedArea;
	memset(info->shared, 0, sizeof(rpi_display_shared_info));
	info->shared->version = RPI_DISPLAY_VERSION;
	info->bufferArea = -1;
	info->cursorArea = -1;

	status_t status = read_outputs(info);
	if (status != B_OK) {
		ERROR("no display from the firmware: %s\n", strerror(status));
		delete_area(info->sharedArea);
		return status;
	}

	// Room for the outputs side by side, taken while contiguous memory is
	// still easy to find.
	const rpi_display_shared_info& shared = *info->shared;
	uint32 width = 0, height = 0;
	for (uint32 i = 0; i < shared.output_count; i++) {
		width += shared.outputs[i].native_width;
		if (shared.outputs[i].native_height > height)
			height = shared.outputs[i].native_height;
	}
	status = ensure_buffer(info, (size_t)width * 4 * height);
	if (status != B_OK) {
		ERROR("no memory for the frame buffer: %s\n", strerror(status));
		delete_area(info->sharedArea);
		return status;
	}

	mutex_init(&info->lock, "rpi display");
	info->changePort = -1;
	info->pollThread = -1;
	info->cursorPlaneOffset = CURSOR_PLANE_OFFSET;
	info->cursorLayer = CURSOR_PLANE_LAYER;
	info->cursorImageType = VC_IMAGE_ARGB8888;
	// BCM2711 only (the driver's compatible match). The VC5 HDMI_HOTPLUG
	// register is at offset 0x1a8 of each HDMI block, bit 0 = connected.
	// These read-only maps do not touch clocks, PHYs or interrupt state;
	// those remain owned by the VideoCore firmware.
	for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
		void* registers;
		info->hpdAreas[i] = map_physical_memory("BCM2711 HDMI hotplug",
			0xfef00000 + i * 0x5000, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA,
			&registers);
		if (info->hpdAreas[i] >= 0)
			info->hpdRegisters[i] = (volatile uint32*)((uint8*)registers + 0x8a8);
		else
			ERROR("cannot map HDMI%" B_PRIu32 " hotplug register\n", i);
	}
	info->pollThread = spawn_kernel_thread(poll_outputs, "rpi HDMI hotplug",
		B_NORMAL_PRIORITY, info);
	if (info->pollThread >= 0)
		resume_thread(info->pollThread);
	else
		ERROR("cannot start HDMI hotplug thread\n");

	map_compositor(info);
	if (info->hvsArea >= 0)
		info->retrace = new(std::nothrow) RetraceSemaphores;
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
	for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
		if (info->hpdAreas[i] >= 0)
			delete_area(info->hpdAreas[i]);
	}
	delete info->retraceFollower;
	delete info->retrace;
	if (info->hvsArea >= 0)
		delete_area(info->hvsArea);
	mutex_destroy(&info->lock);
	if (info->bufferArea >= 0)
		delete_area(info->bufferArea);
	if (info->cursorArea >= 0)
		delete_area(info->cursorArea);
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
			return user_strlcpy((char*)buffer, RPI_DISPLAY_ACCELERANT,
				B_FILE_NAME_LENGTH) < B_OK ? B_BAD_ADDRESS : B_OK;

		case RPI_DISPLAY_GET_SHARED_AREA:
			return user_memcpy(buffer, &info->sharedArea, sizeof(area_id));

		case RPI_DISPLAY_GET_STATE:
		{
			if (length != sizeof(rpi_display_shared_info))
				return B_BAD_VALUE;
			MutexLocker locker(info->lock);
			return user_memcpy(buffer, info->shared, sizeof(*info->shared));
		}

		case RPI_DISPLAY_SET_CHANGE_PORT:
		{
			rpi_display_change_port request;
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

		case RPI_DISPLAY_SET_LAYOUT:
		{
			rpi_display_layout layout;
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

		case RPI_DISPLAY_GET_RETRACE_SEMAPHORE:
		{
			if (info->retrace == NULL)
				return B_NOT_SUPPORTED;
			MutexLocker locker(info->lock);
			sem_id semaphore = info->retrace->Get();
			if (semaphore < 0)
				return semaphore;
			// followed from the first time a program wants the blanks
			if (info->retraceFollower == NULL) {
				info->retraceFollower
					= new(std::nothrow) HvsRetraceFollower(info);
				if (info->retraceFollower == NULL)
					return B_NO_MEMORY;
				info->retraceFollower->Start("rpi display blanks");
			}
			locker.Unlock();
			return user_memcpy(buffer, &semaphore, sizeof(semaphore));
		}

		case RPI_DISPLAY_CLONE_CURSOR_BUFFER:
		{
			MutexLocker locker(info->lock);
			status_t status = ensure_cursor_buffer(info);
			if (status != B_OK)
				return status;
			void* address = NULL;
			area_id area = vm_clone_area(team_get_current_team_id(),
				"rpi display cursor", &address, B_ANY_ADDRESS,
				B_READ_AREA | B_WRITE_AREA, 0, info->cursorArea, true);
			if (area < 0)
				return area;

			area_info areaInfo = {};
			areaInfo.area = area;
			areaInfo.address = address;
			areaInfo.size = RPI_DISPLAY_CURSOR_MAX * RPI_DISPLAY_CURSOR_MAX * 4;
			status = user_memcpy(buffer, &areaInfo, sizeof(areaInfo));
			if (status != B_OK)
				vm_delete_area(team_get_current_team_id(), area, true);
			return status;
		}

		case RPI_DISPLAY_DEFINE_CURSOR:
		{
			if (length != sizeof(rpi_display_cursor))
				return B_BAD_VALUE;
			rpi_display_cursor cursor;
			status_t status = user_memcpy(&cursor, buffer, sizeof(cursor));
			if (status != B_OK)
				return status;
			if (cursor.width == 0 || cursor.width > RPI_DISPLAY_CURSOR_MAX
				|| cursor.height == 0
				|| cursor.height > RPI_DISPLAY_CURSOR_MAX)
				return B_BAD_VALUE;

			MutexLocker locker(info->lock);
			status = ensure_cursor_buffer(info);
			if (status != B_OK)
				return status;
			info->cursor = cursor;
			info->cursorDefined = true;
			return update_cursor_plane(info);
		}

		case RPI_DISPLAY_MOVE_CURSOR:
		{
			if (length != sizeof(rpi_display_cursor_position))
				return B_BAD_VALUE;
			rpi_display_cursor_position position;
			status_t status = user_memcpy(&position, buffer, sizeof(position));
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			info->cursorX = position.x;
			info->cursorY = position.y;
			if (!info->cursorDefined)
				return B_OK;
			return update_cursor_plane(info);
		}

		case RPI_DISPLAY_SHOW_CURSOR:
		{
			if (length != sizeof(bool))
				return B_BAD_VALUE;
			bool visible;
			status_t status = user_memcpy(&visible, buffer, sizeof(visible));
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			info->cursorVisible = visible;
			if (!info->cursorDefined)
				return B_OK;
			return update_cursor_plane(info);
		}

		case RPI_DISPLAY_CURSOR_CONFIG:
		{
			if (length != sizeof(rpi_display_cursor_config))
				return B_BAD_VALUE;
			rpi_display_cursor_config config;
			status_t status = user_memcpy(&config, buffer, sizeof(config));
			if (status != B_OK)
				return status;

			MutexLocker locker(info->lock);
			info->cursorPlaneOffset = config.plane_offset;
			info->cursorLayer = config.layer;
			info->cursorImageType = config.image_type;
			if (!info->cursorDefined)
				return B_OK;
			return update_cursor_plane(info);
		}

		case RPI_DISPLAY_CLONE_FRAME_BUFFER:
		{
			MutexLocker locker(info->lock);
			void* address = NULL;
			area_id area = vm_clone_area(team_get_current_team_id(),
				"rpi display frame buffer", &address, B_ANY_ADDRESS,
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

	// The compositor's node stands for the display hardware; the firmware
	// drives it.
	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK || strcmp(compatible, "brcm,bcm2711-hvs") != 0) {
		return -1.0f;
	}

	return 1.0f;
}


static status_t
display_register_device(device_node* parent)
{
	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Raspberry Pi HDMI outputs" } },
		{}
	};

	return sDeviceManager->register_node(parent, RPI_DISPLAY_DRIVER_MODULE_NAME,
		attributes, NULL, NULL);
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
	return sDeviceManager->publish_device(info->node, RPI_DISPLAY_DEVICE,
		RPI_DISPLAY_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{RPI_FIRMWARE_MODULE_NAME, (module_info**)&sFirmware},
	{}
};

static device_module_info sDisplayDevice = {
	{
		RPI_DISPLAY_DEVICE_MODULE_NAME,
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
		RPI_DISPLAY_DRIVER_MODULE_NAME,
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
