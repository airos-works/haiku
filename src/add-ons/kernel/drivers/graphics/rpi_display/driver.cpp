/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Raspberry Pi's HDMI outputs.

	The firmware sets the outputs' initial video modes and runs the
	compositor (HVS). This driver owns one frame buffer and shows a region
	of it on each output; the accelerant decides the regions (app_server's
	display layout).

	On the Pi 4 (BCM2711) it asks the firmware for one plane per output,
	scaled to the output's mode: the "firmware KMS" property tags.

	The Pi 5's (BCM2712) firmware accepts those requests but shows nothing.
	There the driver writes the compositor's display lists itself: each
	output's channel gets a copy of the plane the firmware set up for its
	own frame buffer, pointed at the output's region, unscaled. The modes
	stay the firmware's (RPI_DISPLAY_FIXED_MODES). The cursor is a second
	plane in the list of the output it is on. */


#include <new>
#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <Drivers.h>
#include <KernelExport.h>

#include <boot_item.h>
#include <driver_settings.h>
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
#define TAG_GET_DISPLAY_CFG				0x00040018
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

// The BCM2712's compositor (HVS generation 6, D0 stepping; Linux's
// vc4_regs.h SCALER6D_*, vc4_plane.c vc6_plane_mode_set()). Channel n feeds
// HDMI n. The display lists live in the compositor's own memory, in words
// from offset 0x4000.
#define HVS6_MAP_SIZE					0x8000
#define HVS6_CXM_SIZE					0x04	// display list words
#define HVS6_DISP_CTRL0(n)				(0x100 + (n) * 0x40)
#define HVS6_DISP_CTRL0_ENB				(1u << 31)
#define HVS6_DISP_LPTRS(n)				(0x110 + (n) * 0x40)
#define HVS6_DISP_STATUS(n)				(0x118 + (n) * 0x40)
#define HVS6_DLIST						0x4000
#define HVS6_CTL0_END					(1u << 31)
#define HVS6_CTL0_VALID					(1u << 30)
#define HVS6_CTL0_NEXT_SHIFT			24
#define HVS6_CTL0_NEXT_MASK				(0x3fu << 24)
#define HVS6_CTL0_UNITY					(1u << 15)
#define HVS6_PLANE_WORDS				8
	// CTL0, POS0, CTL2, POS2, context, PTR0..2 of an unscaled RGB plane
#define HVS6_CURSOR_CTL0				0x4800c007
	// valid, 8 words, alpha from the pixels, unscaled, ARGB order, RGBA8888
#define HVS6_CURSOR_CTL2				0x2000fff0
	// premultiplied, plane alpha 0xfff
#define HVS6_CURSOR_UPM_BASE			0x3f0
	// the top of the 1024 UPM (fetch buffer) units of 256 bytes, far from
	// the firmware's planes (0 and 0x199); 4 units a cursor (3 needed)
#define HVS6_CURSOR_UPM_HANDLE			30
	// handles 30 and 31 (the firmware's planes use 0 and 8)
#define HVS6_DISP_CTRL0_RESET			(1u << 30)
#define HVS6_DISP_CTRL1(n)				(0x104 + (n) * 0x40)
#define HVS6_DISP_CTRL1_INTLACE			(1u << 0)
// scaling (vc4_plane.c vc4_write_ppf(), vc4_write_tpz(), the LBM)
#define HVS6_CTL0_SCL_SHIFT				5	// SCL0 bits 7:5, SCL1 10:8
#define HVS6_SCL_H_PPF_V_PPF			0
#define HVS6_SCL_H_TPZ_V_PPF			1
#define HVS6_SCL_H_PPF_V_TPZ			2
#define HVS6_SCL_H_TPZ_V_TPZ			3
#define HVS6_SCL_H_PPF_V_NONE			4
#define HVS6_SCL_H_NONE_V_PPF			5
#define HVS6_SCL_H_NONE_V_TPZ			6
#define HVS6_SCL_H_TPZ_V_NONE			7
#define HVS6_PPF_AGC					(1u << 30)
#define HVS6_KERNEL_WORD				0xdf0
	// the scaling filter, 11 words in the display list memory
#define HVS6_KERNEL_WORDS				11
#define HVS6_LBM_PER_OUTPUT				512
#define HVS6_LBM_CURSOR					480
	// a region's plane needs at most 4096 * 4 * 3 / 128 = 384 words (four
	// words a component, red, green and blue, a line), the cursor's 64 *
	// 4 * 4 / 128 = 8 from here (Linux's vc4_lbm_channel_size())
	// of the 1024 line buffer words: the firmware's planes use none
#define HVS6_SLOT_BASE					0xe00
#define HVS6_SLOT_WORDS					0x40
	// two slots a channel at the end of the list memory, used in turn

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

	// the BCM2712's display lists: the firmware's plane of each channel,
	// its list head, and which of the driver's two slots is next
	bool			bcm2712;
	uint32			firmwareHead[RPI_DISPLAY_MAX_OUTPUTS];
	uint32			firmwarePlane[RPI_DISPLAY_MAX_OUTPUTS][HVS6_PLANE_WORDS];
	bool			hasFirmwarePlane[RPI_DISPLAY_MAX_OUTPUTS];
	uint32			nextSlot[RPI_DISPLAY_MAX_OUTPUTS];
	// what each channel shows: the region, and whether the cursor's plane
	// is in its list (at activeSlot + HVS6_PLANE_WORDS)
	bool			regionShown[RPI_DISPLAY_MAX_OUTPUTS];
	int32			regionX[RPI_DISPLAY_MAX_OUTPUTS];
	int32			regionY[RPI_DISPLAY_MAX_OUTPUTS];
	uint32			regionWidth[RPI_DISPLAY_MAX_OUTPUTS];
	uint32			regionHeight[RPI_DISPLAY_MAX_OUTPUTS];
	uint32			regionPitch[RPI_DISPLAY_MAX_OUTPUTS];
	uint32			activeSlot[RPI_DISPLAY_MAX_OUTPUTS];
	bool			cursorIn[RPI_DISPLAY_MAX_OUTPUTS];
	uint32			cursorOffset[RPI_DISPLAY_MAX_OUTPUTS];
	uint32			cursorWordCount[RPI_DISPLAY_MAX_OUTPUTS];
	uint32			cursorWords[RPI_DISPLAY_MAX_OUTPUTS][32];

	// the BCM2712's HDMI encoders and pixel valves, for mode setting
	area_id			modeAreas[RPI_DISPLAY_MAX_OUTPUTS][4];
	volatile uint8*	hdmiRegs[RPI_DISPLAY_MAX_OUTPUTS];	// HDMI block
	volatile uint8*	phyRegs[RPI_DISPLAY_MAX_OUTPUTS];
	volatile uint8*	rmRegs[RPI_DISPLAY_MAX_OUTPUTS];
	volatile uint8*	packetRegs[RPI_DISPLAY_MAX_OUTPUTS];
	volatile uint8*	pvRegs[RPI_DISPLAY_MAX_OUTPUTS];
	area_id			hdArea;
	volatile uint8*	hdRegs;
	uint32			modeClock[RPI_DISPLAY_MAX_OUTPUTS];	// kHz, ours
	// the DDC's I2C controllers, for the monitors' SCDC
	area_id			ddcArea;
	volatile uint8*	ddcRegs[RPI_DISPLAY_MAX_OUTPUTS];
	bool			scrambling[RPI_DISPLAY_MAX_OUTPUTS];

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
	bool			cursorCanvas;

	// copies by the SoC's DMA engine (RPI_DISPLAY_COPY): one channel fills
	// sources and shadows, the other copies to the frame buffer, so that a
	// flush at a blank does not wait behind a long copy into a shadow
	struct display_dma_channel* dmaChannels;
	struct display_source* sources;
	uint32			nextSourceID;
};


static device_manager_info* sDeviceManager;
static rpi_firmware_module_info* sFirmware;
// the highest pixel clock (kHz) each HDMI output can send, from the firmware
static uint32 sMaxPixelClock[RPI_DISPLAY_MAX_OUTPUTS] = {
	RPI_DEFAULT_MAX_PIXEL_CLOCK, RPI_DEFAULT_MAX_PIXEL_CLOCK
};


//	#pragma mark - firmware


static status_t
set_plane(const firmware_plane& plane)
{
	firmware_plane request = plane;
	return sFirmware->property(TAG_SET_PLANE, &request, sizeof(request));
}


static display_info* sInfo;
static void hvs6_restore(display_info* info, uint32 index);


static void
remove_plane(uint32 index, uint32 display)
{
	if (sInfo != NULL && sInfo->bcm2712) {
		hvs6_restore(sInfo, index);
		return;
	}
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


/*!	The pixel clocks the firmware drives the HDMI outputs up to, in Hz
	(more than 300 MHz with hdmi_enable_4kp60, on HDMI0 only).
*/
static void
read_pixel_clock_limits()
{
	uint32 limits[RPI_DISPLAY_MAX_OUTPUTS] = {};
	if (sFirmware->property(TAG_GET_DISPLAY_CFG, limits, sizeof(limits))
			!= B_OK) {
		return;
	}
	for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
		if (limits[i] >= 25000000)
			sMaxPixelClock[i] = limits[i] / 1000;
	}
	INFO("pixel clocks up to %" B_PRIu32 " and %" B_PRIu32 " kHz\n",
		sMaxPixelClock[0], sMaxPixelClock[1]);
}


static status_t connect_output(rpi_display_output& output);


/*!	The outputs, mapped to stable physical connectors, each set to the mode
	its monitor prefers. The firmware's own boot mode is no use: what it
	reports is the size of its frame buffer, which it scales to the mode (a
	4K monitor gave 1920x1280).
*/
static status_t read_outputs_bcm2712(display_info* info);


static status_t
read_outputs(display_info* info)
{
	rpi_display_shared_info& shared = *info->shared;
	shared.output_count = RPI_DISPLAY_MAX_OUTPUTS;
	shared.outputs[0].id = 2;
	shared.outputs[1].id = 7;
	if (info->bcm2712)
		return read_outputs_bcm2712(info);
	read_pixel_clock_limits();
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
		// Without an EDID timing the firmware's guess stands.
		connect_output(output);
		found = true;
		INFO("HDMI%" B_PRIu32 ": display id %" B_PRIu32 ", %ux%u, %"
			B_PRIu32 " bytes of EDID\n", index, output.id,
			output.native_width, output.native_height, output.edid_length);
	}
	uint32 number = 0;
	sFirmware->property(TAG_FB_SET_DISPLAY_NUM, &number, sizeof(number));
	return found ? B_OK : B_DEVICE_NOT_FOUND;
}


static status_t connect_output_bcm2712(rpi_display_output& output);


static status_t
connect_output(rpi_display_output& output)
{
	if (sInfo != NULL && sInfo->bcm2712)
		return connect_output_bcm2712(output);
	rpi_display_output connected = {};
	connected.id = output.id;
	read_edid(connected);
	firmware_timing timing;
	if (!preferred_edid_timing(connected.edid, connected.edid_length, timing,
			sMaxPixelClock[output.id == 2 ? 0 : 1])) {
		return B_DEV_NOT_READY;
	}
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
	INFO("display %" B_PRIu32 ": %ux%u at %u Hz (%" B_PRIu32 " kHz, code %u)\n",
		output.id, timing.hdisplay, timing.vdisplay, timing.vrefresh,
		timing.clock, timing.video_id_code);
	return B_OK;
}


static void bcm2712_check_scrambling(display_info* info, uint32 index);


static int32
poll_outputs(void* cookie)
{
	display_info* info = (display_info*)cookie;
	bool previous[2] = {false, false};
	bigtime_t retryAt[2] = {0, 0};
	uint32 polls = 0;
	while (atomic_get(&info->stopping) == 0) {
		snooze(250000);
		MutexLocker locker(info->lock);
		if (info->bcm2712 && ++polls % 4 == 0) {
			for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
				if (info->hpdRegisters[i] != NULL
					&& (*info->hpdRegisters[i] & 1) != 0) {
					bcm2712_check_scrambling(info, i);
				}
			}
		}
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
		if (fInfo->bcm2712)
			return (_Read(HVS6_DISP_STATUS(fChannel)) >> 16) & 0x3f;
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
			if (fInfo->bcm2712) {
				// channel n is HDMI n
				if ((_Read(HVS6_DISP_CTRL0(i)) & HVS6_DISP_CTRL0_ENB) != 0)
					return i;
				continue;
			}
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
	size_t mapSize = info->bcm2712 ? HVS6_MAP_SIZE : B_PAGE_SIZE;
	if (status != B_OK || size < (info->bcm2712 ? HVS6_MAP_SIZE
			: HVS_DISPSTAT2 + 4)) {
		ERROR("no compositor registers, no vertical blanks\n");
		return;
	}

	phys_addr_t page = base & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* address;
	// the BCM2712's display lists are the driver's to write
	info->hvsArea = map_physical_memory(info->bcm2712 ? "BCM2712 HVS"
			: "BCM2711 HVS", page, mapSize,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA
			| (info->bcm2712 ? B_KERNEL_WRITE_AREA : 0),
		&address);
	if (info->hvsArea < 0) {
		ERROR("cannot map the compositor: %s\n", strerror(info->hvsArea));
		return;
	}
	info->hvs = (volatile uint8*)address + (base - page);
}


//	#pragma mark - BCM2712 display lists


static void output_dest(uint32 nativeWidth, uint32 nativeHeight, uint32 width,
	uint32 height, int32* destX, int32* destY, uint32* destWidth,
	uint32* destHeight);


static bool
is_bcm2712(device_node* node)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	if (parent == NULL)
		return false;
	const char* compatible;
	bool result = sDeviceManager->get_attr_string(parent, "fdt/compatible",
			&compatible, false) == B_OK
		&& strcmp(compatible, "brcm,bcm2712-hvs") == 0;
	sDeviceManager->put_node(parent);
	return result;
}


static inline uint32
hvs6_read(display_info* info, uint32 offset)
{
	return *(volatile uint32*)(info->hvs + offset);
}


static inline void
hvs6_write(display_info* info, uint32 offset, uint32 value)
{
	*(volatile uint32*)(info->hvs + offset) = value;
}


static inline volatile uint32*
hvs6_dlist(display_info* info, uint32 word)
{
	return (volatile uint32*)(info->hvs + HVS6_DLIST) + word;
}


/*!	Takes note of the plane the firmware put on channel \a index for its
	frame buffer: the driver's planes are copies of it. Only an unscaled
	RGB plane of eight words is understood.
*/
static bool
hvs6_read_firmware_plane(display_info* info, uint32 index)
{
	uint32 control = hvs6_read(info, HVS6_DISP_CTRL0(index));
	if ((control & HVS6_DISP_CTRL0_ENB) == 0)
		return false;

	uint32 head = hvs6_read(info, HVS6_DISP_LPTRS(index)) & 0xfff;
	uint32 listWords = hvs6_read(info, HVS6_CXM_SIZE);
	if (listWords > 0x1000 || listWords < HVS6_SLOT_BASE + 4 * HVS6_SLOT_WORDS)
		listWords = 0x1000;
	if (info->hasFirmwarePlane[index] && head >= HVS6_SLOT_BASE)
		return true;	// showing one of the driver's lists already
	if (head + HVS6_PLANE_WORDS + 1 > HVS6_SLOT_BASE)
		return false;

	volatile uint32* list = hvs6_dlist(info, head);
	uint32* plane = info->firmwarePlane[index];
	for (uint32 i = 0; i < HVS6_PLANE_WORDS; i++)
		plane[i] = list[i];
	if ((plane[0] & (HVS6_CTL0_VALID | HVS6_CTL0_END)) != HVS6_CTL0_VALID
		|| (plane[0] & HVS6_CTL0_UNITY) == 0
		|| (list[HVS6_PLANE_WORDS] & HVS6_CTL0_END) == 0) {
		ERROR("HDMI%" B_PRIu32 ": the firmware's display list is not one "
			"unscaled plane: %#" B_PRIx32 " %#" B_PRIx32 " ... %#" B_PRIx32
			"\n", index, plane[0], plane[1], list[HVS6_PLANE_WORDS]);
		return false;
	}
	info->firmwareHead[index] = head;
	info->hasFirmwarePlane[index] = true;
	return true;
}


/*!	The words of a plane's scaling (after its pointer words): the line
	buffer at \a lbm, the parameters, the filter. Returns the count; sets
	the scaler field of \a control.
*/
static uint32
hvs6_scaling_words(uint32 width, uint32 height, uint32 destWidth,
	uint32 destHeight, uint32 lbm, uint32& control, uint32* words)
{
	enum { NONE, PPF, TPZ };
	// as Linux picks it: polyphase down to 2/3, trapezoidal below
	int xScaling = destWidth == width ? NONE
		: 3 * destWidth >= 2 * width ? PPF : TPZ;
	int yScaling = destHeight == height ? NONE
		: 3 * destHeight >= 2 * height ? PPF : TPZ;
	static const uint8 kScl[3][3] = {
		// y: NONE, PPF, TPZ
		{ 0, HVS6_SCL_H_NONE_V_PPF, HVS6_SCL_H_NONE_V_TPZ },	// x NONE
		{ HVS6_SCL_H_PPF_V_NONE, HVS6_SCL_H_PPF_V_PPF,
			HVS6_SCL_H_PPF_V_TPZ },								// x PPF
		{ HVS6_SCL_H_TPZ_V_NONE, HVS6_SCL_H_TPZ_V_PPF,
			HVS6_SCL_H_TPZ_V_TPZ }								// x TPZ
	};
	control &= ~(HVS6_CTL0_UNITY | (0x3fu << HVS6_CTL0_SCL_SHIFT));
	if (xScaling == NONE && yScaling == NONE) {
		control |= HVS6_CTL0_UNITY;
		return 0;
	}
	uint32 scl = kScl[xScaling][yScaling];
	control |= scl << HVS6_CTL0_SCL_SHIFT | scl << (HVS6_CTL0_SCL_SHIFT + 3);

	uint32 count = 0;
	if (yScaling != NONE)
		words[count++] = lbm;
	// horizontal, then vertical parameters (16.16 scales)
	for (int axis = 0; axis < 2; axis++) {
		int scaling = axis == 0 ? xScaling : yScaling;
		uint32 source = (axis == 0 ? width : height) << 16;
		uint32 dest = axis == 0 ? destWidth : destHeight;
		if (scaling == PPF) {
			uint32 scale = (source / dest) & ~1u;
			// half a pixel in, and half the scale's rounding
			int32 phase = -(1 << 6 >> 1)
				+ (((int32)(source - dest * scale) >> 10) >> 1);
			if (phase >= 1 << 6)
				phase = (1 << 6) - 1;
			words[count++] = HVS6_PPF_AGC | (scale & 0x1ffff) << 8
				| ((uint32)phase & 0x7f);
			if (axis == 1)
				words[count++] = 0xc0c0c0c0;
		} else if (scaling == TPZ) {
			uint32 scale = source / dest;
			words[count++] = (scale & 0x1fffff) << 8;
			words[count++] = (0xffffffffu / scale) & 0xffff;
			if (axis == 1)
				words[count++] = 0xc0c0c0c0;
		}
	}
	// the filter for each polyphase pass
	if (xScaling == PPF || yScaling == PPF) {
		for (int i = 0; i < 4; i++)
			words[count++] = HVS6_KERNEL_WORD;
	}
	return count;
}


/*!	The words of the cursor's plane on output \a index, scaled as the
	output's region is, cut to the output; 0 if it is not on it.
*/
static uint32
hvs6_cursor_plane(display_info* info, uint32 index, uint32* words)
{
	const rpi_display_output& output = info->shared->outputs[index];
	if (!info->cursorDefined || !info->cursorVisible
		|| info->cursorBufferAddress == 0 || !info->regionShown[index]) {
		return 0;
	}
	int32 regionX = info->regionX[index];
	int32 regionY = info->regionY[index];
	int32 width = info->regionWidth[index];
	int32 height = info->regionHeight[index];
	if (info->cursorX < regionX || info->cursorX >= regionX + width
		|| info->cursorY < regionY || info->cursorY >= regionY + height) {
		return 0;
	}

	// where the image lands on the output, through the region's scaling
	int32 destX, destY;
	uint32 destWidth, destHeight;
	output_dest(output.native_width, output.native_height, width, height,
		&destX, &destY, &destWidth, &destHeight);
	int32 left = destX + (int32)((int64)(info->cursorX - regionX
		- info->cursor.hot_x) * (int32)destWidth / width);
	int32 top = destY + (int32)((int64)(info->cursorY - regionY
		- info->cursor.hot_y) * (int32)destHeight / height);
	int32 right = destX + (int32)((int64)(info->cursorX - regionX
		- info->cursor.hot_x + info->cursor.width) * (int32)destWidth
		/ width);
	int32 bottom = destY + (int32)((int64)(info->cursorY - regionY
		- info->cursor.hot_y + info->cursor.height) * (int32)destHeight
		/ height);

	// cut to the output, the source in proportion
	int32 sourceWidth = info->cursor.width;
	int32 sourceHeight = info->cursor.height;
	int32 skipX = 0, skipY = 0;
	if (left < 0) {
		skipX = (int32)((int64)-left * sourceWidth / (right - left) + 1);
		left += (int32)((int64)skipX * (right - left) / sourceWidth);
	}
	if (top < 0) {
		skipY = (int32)((int64)-top * sourceHeight / (bottom - top) + 1);
		top += (int32)((int64)skipY * (bottom - top) / sourceHeight);
	}
	if (left < 0)
		left = 0;
	if (top < 0)
		top = 0;
	sourceWidth -= skipX;
	sourceHeight -= skipY;
	if (right > (int32)output.native_width) {
		int32 cut = right - output.native_width;
		int32 cutSource = (int32)((int64)cut * info->cursor.width
			/ (right - left + 1) + 1);
		sourceWidth -= cutSource;
		right = output.native_width;
	}
	if (bottom > (int32)output.native_height) {
		int32 cut = bottom - output.native_height;
		int32 cutSource = (int32)((int64)cut * info->cursor.height
			/ (bottom - top + 1) + 1);
		sourceHeight -= cutSource;
		bottom = output.native_height;
	}
	int32 cursorWidth = right - left;
	int32 cursorHeight = bottom - top;
	if (sourceWidth <= 0 || sourceHeight <= 0 || cursorWidth <= 0
		|| cursorHeight <= 0) {
		return 0;
	}

	const uint32 pitch = RPI_DISPLAY_CURSOR_MAX * 4;
	uint64 address = info->cursorBufferAddress + (uint64)skipY * pitch
		+ skipX * 4;
	uint32 control = HVS6_CURSOR_CTL0;
	uint32 scaling[16];
	uint32 scalingCount = hvs6_scaling_words(sourceWidth, sourceHeight,
		cursorWidth, cursorHeight,
		index * HVS6_LBM_PER_OUTPUT + HVS6_LBM_CURSOR, control, scaling);
	bool scaled = scalingCount != 0;

	uint32 count = 0;
	words[count++] = control & ~HVS6_CTL0_NEXT_MASK;
	words[count++] = (uint32)top << 16 | (uint32)left;
	words[count++] = HVS6_CURSOR_CTL2;
	if (scaled) {
		words[count++] = (uint32)(cursorHeight - 1) << 16
			| (uint32)(cursorWidth - 1);
	}
	words[count++] = (uint32)(sourceHeight - 1) << 16
		| (uint32)(sourceWidth - 1);
	words[count++] = 0xc0c0c0c0;
	words[count++] = (HVS6_CURSOR_UPM_BASE + index * 4) << 16
		| (HVS6_CURSOR_UPM_HANDLE + index) << 10
		| (uint32)((address >> 32) & 0xff);
	words[count++] = (uint32)address;
	words[count++] = pitch;
	for (uint32 i = 0; i < scalingCount; i++)
		words[count++] = scaling[i];
	words[0] |= count << HVS6_CTL0_NEXT_SHIFT;
	return count;
}


/*!	Writes output \a index's display list, its region and the cursor's
	plane over it if the cursor is there, into the slot the compositor is
	not reading, then points the channel at it: the compositor takes it at
	the next frame.
*/
static void
hvs6_write_list(display_info* info, uint32 index)
{
	const rpi_display_output& output = info->shared->outputs[index];
	uint32 width = info->regionWidth[index];
	uint32 height = info->regionHeight[index];
	uint32 pitch = info->regionPitch[index];
	int32 destX, destY;
	uint32 destWidth, destHeight;
	output_dest(output.native_width, output.native_height, width, height,
		&destX, &destY, &destWidth, &destHeight);
	uint64 address = info->bufferAddress
		+ (uint64)info->regionY[index] * pitch + info->regionX[index] * 4;
	const uint32* plane = info->firmwarePlane[index];

	uint32 control = plane[0] & ~HVS6_CTL0_NEXT_MASK;
	uint32 scaling[16];
	uint32 scalingCount = hvs6_scaling_words(width, height, destWidth,
		destHeight, index * HVS6_LBM_PER_OUTPUT, control, scaling);

	uint32 words[64];
	uint32 count = 0;
	words[count++] = control;
	words[count++] = (uint32)destY << 16 | (uint32)destX;
	words[count++] = plane[2];
	if (scalingCount != 0)
		words[count++] = (destHeight - 1) << 16 | (destWidth - 1);
	words[count++] = (height - 1) << 16 | (width - 1);
	words[count++] = 0xc0c0c0c0;
		// context, the compositor's to write
	words[count++] = (plane[5] & ~0xffu) | (uint32)((address >> 32) & 0xff);
		// keeps the firmware's UPM (fetch buffer) settings
	words[count++] = (uint32)address;
	words[count++] = (plane[7] & ~0x1ffffu) | pitch;
	for (uint32 i = 0; i < scalingCount; i++)
		words[count++] = scaling[i];
	words[0] |= count << HVS6_CTL0_NEXT_SHIFT;

	uint32 cursorCount = hvs6_cursor_plane(info, index, words + count);
	info->cursorIn[index] = cursorCount != 0;
	info->cursorOffset[index] = count;
	info->cursorWordCount[index] = cursorCount;
	memcpy(info->cursorWords[index], words + count, cursorCount * 4);
	count += cursorCount;
	words[count++] = HVS6_CTL0_END;

	uint32 slot = HVS6_SLOT_BASE + index * 2 * HVS6_SLOT_WORDS
		+ info->nextSlot[index] * HVS6_SLOT_WORDS;
	info->nextSlot[index] ^= 1;
	volatile uint32* list = hvs6_dlist(info, slot);
	for (uint32 i = 0; i < count; i++)
		list[i] = words[i];
	memory_full_barrier();
	hvs6_write(info, HVS6_DISP_LPTRS(index), slot);
	info->activeSlot[index] = slot;
}


/*!	The scaling filter (Linux's Mitchell/Netravali B = C = 1/3 kernel, laid
	out as vc4_hvs_upload_linear_kernel() does), once.
*/
static void
hvs6_upload_kernel(display_info* info)
{
	static const int16 kCoefficients[16] = {
		0, -2, -6, -8, -10, -8, -3, 2, 18, 50, 82, 119, 155, 187, 213, 227
	};
	uint32 half[6];
	for (int i = 0; i < 6; i++) {
		int16 c0 = kCoefficients[i * 3];
		int16 c1 = i * 3 + 1 < 16 ? kCoefficients[i * 3 + 1] : 0;
		int16 c2 = i * 3 + 2 < 16 ? kCoefficients[i * 3 + 2] : 0;
		if (i == 5) {
			// c15, c15, 0
			c1 = kCoefficients[15];
			c2 = 0;
		}
		half[i] = ((uint32)c0 & 0x1ff) | ((uint32)c1 & 0x1ff) << 9
			| ((uint32)c2 & 0x1ff) << 18;
	}
	volatile uint32* kernel = hvs6_dlist(info, HVS6_KERNEL_WORD);
	for (int i = 0; i < HVS6_KERNEL_WORDS; i++)
		kernel[i] = i < 6 ? half[i] : half[HVS6_KERNEL_WORDS - i - 1];
}


/*!	Shows the region (x, y, width, height) of the frame buffer on output
	\a index, unscaled, centered when it is smaller than the mode.
*/
static status_t
hvs6_show(display_info* info, uint32 index, int32 x, int32 y, uint32 width,
	uint32 height, uint32 bytesPerRow)
{
	if (!info->hasFirmwarePlane[index])
		return B_NOT_SUPPORTED;
	info->regionShown[index] = true;
	info->regionX[index] = x;
	info->regionY[index] = y;
	info->regionWidth[index] = width;
	info->regionHeight[index] = height;
	info->regionPitch[index] = bytesPerRow;
	hvs6_write_list(info, index);
	return B_OK;
}


/*!	The cursor on the BCM2712: a plane of its own in the list of the output
	under the hotspot. Moving it within an output rewrites its words in the
	list the compositor reads, as Linux's asynchronous updates do; showing,
	hiding it or moving it to another output writes new lists.
*/
static void
hvs6_update_cursor(display_info* info)
{
	for (uint32 i = 0; i < info->shared->output_count; i++) {
		if (!info->regionShown[i])
			continue;
		uint32 words[32];
		uint32 count = hvs6_cursor_plane(info, i, words);
		if (count != 0 && info->cursorIn[i]
			&& count == info->cursorWordCount[i]
			&& words[0] == info->cursorWords[i][0]) {
			// the same plane moved: the words that changed, in place
			volatile uint32* plane = hvs6_dlist(info,
				info->activeSlot[i] + info->cursorOffset[i]);
			for (uint32 k = 1; k < count; k++) {
				if (words[k] != info->cursorWords[i][k])
					plane[k] = words[k];
			}
			memcpy(info->cursorWords[i], words, count * 4);
		} else if (count != 0 || info->cursorIn[i])
			hvs6_write_list(info, i);
	}
}


/*!	The firmware's own list again: its frame buffer, the boot console. */
static void
hvs6_restore(display_info* info, uint32 index)
{
	if (info->hvsArea < 0 || !info->hasFirmwarePlane[index])
		return;
	hvs6_write(info, HVS6_DISP_LPTRS(index), info->firmwareHead[index]);
	info->regionShown[index] = false;
	info->cursorIn[index] = false;
}



//	#pragma mark - BCM2712 mode setting


/*	The firmware starts each output at a mode of its choosing (1080p, also
	on a 4K monitor) and ignores the mode options of config.txt on the Pi 5.
	The driver sets the mode the monitor's EDID prefers itself, the way
	Linux's vc4 driver does (vc4_crtc.c, vc4_hdmi.c, vc4_hdmi_phy.c:
	vc6_hdmi_phy_init()), keeping what the firmware set up that does not
	depend on the mode: the HDMI state machine and pixel clocks (648 and
	324 MHz from the firmware, enough for 594 MHz), the colour conversion,
	the compositor's output buffers (3840 pixels, 4 lines a channel), the
	pixel valves' two pixels a clock. Without scrambling (and the SCDC
	messages to the monitor it takes), up to 340 MHz: 4K at 30 Hz. */

// The HDMI block (vc4_hdmi_regs.h vc6_hdmi_hdmi*_fields)
#define HDMI6_FIFO_CTL					0x07c
#define  HDMI6_FIFO_CTL_MASTER_SLAVE_N	(1u << 0)
#define  HDMI6_FIFO_CTL_RECENTER		(1u << 6)
#define  HDMI6_FIFO_CTL_RECENTER_DONE	(1u << 14)
#define  HDMI6_FIFO_VALID_WRITE_MASK	0xefff
#define HDMI6_RAM_PACKET_CONFIG			0x0c4
#define HDMI6_SCHEDULER_CONTROL			0x0e8
#define  HDMI6_SCHEDULER_MODE_HDMI		(1u << 0)
#define  HDMI6_SCHEDULER_HDMI_ACTIVE	(1u << 1)
#define  HDMI6_SCHEDULER_IGNORE_VSYNC_PREDICTS (1u << 5)
#define  HDMI6_SCHEDULER_MANUAL_FORMAT	(1u << 15)
#define HDMI6_HORZA						0x0ec
#define HDMI6_HORZB						0x0f0
#define HDMI6_VERTA0					0x0f4
#define HDMI6_VERTB0					0x0f8
#define HDMI6_VERTA1					0x100
#define HDMI6_VERTB1					0x104
// "hd": HDMI_VID_CTL of HDMI 0 and 1
#define HD6_VID_CTL(n)					(0x44 + (n) * 4)
#define  HD6_VID_CTL_ENABLE				(1u << 31)
#define  HD6_VID_CTL_VSYNC_LOW			(1u << 28)
#define  HD6_VID_CTL_HSYNC_LOW			(1u << 27)
#define  HD6_VID_CTL_CLRRGB				(1u << 23)
#define  HD6_VID_CTL_BLANKPIX			(1u << 18)
#define  HD6_VID_CTL_BLANK_INSERT_EN	(1u << 16)
// the PHY
#define PHY6_RESET_CTL					0x000
#define PHY6_POWERUP_CTL				0x004
#define PHY6_CTL(n)						(0x008 + (n) * 4)	// lanes 0-2, clock
#define PHY6_PLL_REFCLK					0x01c
#define PHY6_PLL_POST_KDIV				0x028
#define PHY6_PLL_VCOCLK_DIV				0x02c
#define PHY6_PLL_CFG					0x044
#define PHY6_TMDS_CLK_WORD_SEL			0x054
#define PHY6_PLL_MISC(n)				(0x060 + (n) * 4)
#define PHY6_PLL_RESET_CTL				0x190
#define PHY6_PLL_POWERUP_CTL			0x194
// the rate manager
#define RM6_OFFSET						0x018
// the pixel valve (vc4_regs.h PV_*)
#define PV6_CONTROL						0x00
#define  PV6_CONTROL_EN					(1u << 0)
#define  PV6_CONTROL_FIFO_CLR			(1u << 1)
#define PV6_V_CONTROL					0x04
#define  PV6_VCONTROL_VIDEN				(1u << 0)
#define  PV6_VCONTROL_CONTINUOUS		(1u << 1)
#define PV6_VSYNCD_EVEN					0x08
#define PV6_HORZA						0x0c
#define PV6_HORZB						0x10
#define PV6_VERTA						0x14
#define PV6_VERTB						0x18
#define PV6_PIXELS_PER_CLOCK			2
	// as the firmware runs them for progressive modes

#define BCM2712_MAX_PIXEL_CLOCK			340000	// kHz, without scrambling
#define BCM2712_MAX_SCRAMBLED_CLOCK		600000	// kHz, what the PHY reaches
#define HDMI6_SCRAMBLER_CTL				0x1e4
#define  HDMI6_SCRAMBLER_ENABLE			(1u << 0)

// The DDC: Broadcom's "BSC" I2C controllers (i2c-brcmstb.c), polled,
// eight one-byte data registers
#define BSC_CHIP_ADDRESS				0x00
#define BSC_DATA_IN(n)					(0x04 + (n) * 4)
#define BSC_CNT							0x24
#define BSC_CTL							0x28
#define  BSC_CTL_READ					1
#define  BSC_CTL_97500_HZ				(1u << 4 | 1u << 7)
	// 390 kHz, divided by four
#define BSC_IIC_ENABLE					0x2c
#define  BSC_IIC_ENABLE_ENABLE			(1u << 0)
#define  BSC_IIC_ENABLE_INTRP			(1u << 1)
#define  BSC_IIC_ENABLE_NOACK			(1u << 2)
#define  BSC_IIC_ENABLE_NOSTOP			(1u << 4)
#define  BSC_IIC_ENABLE_RESTART			(1u << 6)
#define BSC_DATA_OUT(n)					(0x30 + (n) * 4)
#define BSC_CTLHI						0x50

// SCDC (HDMI 2.0): the monitor's registers at I2C address 0x54
#define SCDC_ADDRESS					0x54
#define SCDC_TMDS_CONFIG				0x20
#define  SCDC_SCRAMBLING_ENABLE			(1u << 0)
#define  SCDC_TMDS_CLOCK_RATIO_40		(1u << 1)
#define SCDC_SCRAMBLER_STATUS			0x21
#define AVI_INFOFRAME_PACKET			2		// type 0x82 - 0x80
#define PACKET_STRIDE					0x24

// The PHY's lane settings by TMDS rate: words for the three data lanes'
// and the clock lane's control registers (vc4_hdmi_phy.c's
// vc6_hdmi_phy_settings, packed).
static const struct {
	uint32	minRate, maxRate;
	uint32	lane[4];
} kPhy6Settings[] = {
	{ 0u, 222000000u, { 0x80828700, 0x80828700, 0x80828700, 0x80828700 } },
	{ 222000001u, 297000000u,
		{ 0xc0870000, 0xc0870000, 0xc0870000, 0xc0870800 } },
	{ 297000001u, 597000044u,
		{ 0x848f8700, 0x848f8700, 0x848f8700, 0x849f8f00 } },
};
static const uint32 kPhy6PllMisc[9] = {
	0x810c6000, 0x00b8c451, 0x46402e31, 0x00b8c005, 0x42410261, 0xcc021001,
	0xc8301c80, 0xb0804444, 0xf80f8000
};


static inline uint32
reg_read(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
reg_write(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
}


static status_t
map_page(const char* name, phys_addr_t physical, area_id& area,
	volatile uint8*& registers)
{
	phys_addr_t page = physical & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* address;
	area = map_physical_memory(name, page, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < 0)
		return area;
	registers = (volatile uint8*)address + (physical - page);
	return B_OK;
}


/*!	The registers of both outputs (Linux's bcm2712.dtsi: the HDMI nodes'
	"hdmi", "phy", "rm", "packet" and "hd" ranges, the pixel valves).
*/
static status_t
bcm2712_map_mode_registers(display_info* info)
{
	info->hdArea = -1;
	for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
		for (uint32 k = 0; k < 4; k++)
			info->modeAreas[i][k] = -1;
	}
	info->ddcArea = -1;
	volatile uint8* ddc;
	if (map_page("bcm2712 hdmi ddc", 0x107d508200ull, info->ddcArea, ddc)
			== B_OK) {
		info->ddcRegs[0] = ddc;
		info->ddcRegs[1] = ddc + 0x80;
	}
	status_t status = map_page("bcm2712 hdmi hd", 0x107c720000ull,
		info->hdArea, info->hdRegs);
	for (uint32 i = 0; status == B_OK && i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
		uint64 base = 0x107c700000ull + i * 0x5000;
		volatile uint8* page;
		status = map_page("bcm2712 hdmi", base + 0x1000, info->modeAreas[i][0],
			page);
		if (status != B_OK)
			break;
		info->hdmiRegs[i] = page + 0x400;
		info->phyRegs[i] = page + 0xd00;
		status = map_page("bcm2712 hdmi rm", base + 0x2000,
			info->modeAreas[i][1], info->rmRegs[i]);
		if (status == B_OK) {
			status = map_page("bcm2712 hdmi packets", base + 0x3800,
				info->modeAreas[i][2], info->packetRegs[i]);
		}
		if (status == B_OK) {
			status = map_page("bcm2712 pixel valve",
				0x107c410000ull + i * 0x1000, info->modeAreas[i][3],
				info->pvRegs[i]);
		}
	}
	return status;
}


static void
bcm2712_unmap_mode_registers(display_info* info)
{
	if (info->hdArea >= 0)
		delete_area(info->hdArea);
	if (info->ddcArea >= 0)
		delete_area(info->ddcArea);
	for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
		for (uint32 k = 0; k < 4; k++) {
			if (info->modeAreas[i][k] >= 0)
				delete_area(info->modeAreas[i][k]);
		}
	}
}


/*!	vc6_hdmi_phy_init(): the PLL to ten times the TMDS character rate
	times a divider, the lanes for the rate. */
static void
bcm2712_phy_init(display_info* info, uint32 index, uint64 tmdsRate)
{
	volatile uint8* phy = info->phyRegs[index];

	// a VCO between 8 and 12 GHz, the divider in the middle of the range
	uint32 divider = 0;
	while (tmdsRate * divider * 10 < 8000000000ull)
		divider++;
	uint32 minDivider = divider;
	while (tmdsRate * (divider + 1) * 10 < 12000000000ull)
		divider++;
	divider = minDivider + (divider - minDivider) / 2;
	uint64 vco = tmdsRate * divider * 10;

	uint32 setting = B_COUNT_OF(kPhy6Settings) - 1;
	for (uint32 i = 0; i < B_COUNT_OF(kPhy6Settings); i++) {
		if (tmdsRate >= kPhy6Settings[i].minRate
			&& tmdsRate <= kPhy6Settings[i].maxRate) {
			setting = i;
			break;
		}
	}

	reg_write(phy, PHY6_RESET_CTL, 0);
	reg_write(phy, PHY6_POWERUP_CTL, 0);
	for (uint32 i = 0; i < B_COUNT_OF(kPhy6PllMisc); i++)
		reg_write(phy, PHY6_PLL_MISC(i), kPhy6PllMisc[i]);
	// the 54 MHz crystal, CMOS
	reg_write(phy, PHY6_PLL_REFCLK, (1u << 13) | 54);
	reg_write(phy, PHY6_RESET_CTL, 0x7f);
	// the rate manager's offset: the VCO over the reference, 9.22
	uint64 offset = ((vco * 2) << 22) / 54000000ull >> 2;
	reg_write(info->rmRegs[index], RM6_OFFSET,
		(1u << 31) | ((uint32)offset & 0x7fffffff));
	reg_write(phy, PHY6_PLL_VCOCLK_DIV, (1u << 10) | divider);
	reg_write(phy, PHY6_PLL_CFG, 0);
	reg_write(phy, PHY6_PLL_POST_KDIV, (2u << 2) | 1);
	for (uint32 lane = 0; lane < 4; lane++)
		reg_write(phy, PHY6_CTL(lane), kPhy6Settings[setting].lane[lane]);
	reg_write(phy, PHY6_TMDS_CLK_WORD_SEL, tmdsRate >= 340000000 ? 3 : 0);
	// band gap, LDO, bias, the clock and data lanes
	reg_write(phy, PHY6_POWERUP_CTL, 0x1cf);
	reg_write(phy, PHY6_PLL_POWERUP_CTL, 1);
	reg_write(phy, PHY6_PLL_RESET_CTL,
		reg_read(phy, PHY6_PLL_RESET_CTL) & ~1u);
	reg_write(phy, PHY6_PLL_RESET_CTL,
		reg_read(phy, PHY6_PLL_RESET_CTL) | 1u);
	INFO("HDMI%" B_PRIu32 ": PHY for %" B_PRIu64 " Hz, VCO %" B_PRIu64
		" Hz (divider %" B_PRIu32 "), lane settings %" B_PRIu32 "\n", index,
		tmdsRate, vco, divider, setting);
}


/*!	The AVI InfoFrame the firmware left in the packet RAM, with the new
	mode's video code (and its checksum fixed). The packet RAM is off. */
static void
bcm2712_update_avi(display_info* info, uint32 index, uint16 videoCode)
{
	volatile uint8* packet = info->packetRegs[index]
		+ PACKET_STRIDE * AVI_INFOFRAME_PACKET;
	// bytes 0-2, 3-6, 7-9, ...: VIC is byte 7, the checksum byte 3
	uint32 word1 = reg_read(packet, 4);
	uint32 word2 = reg_read(packet, 8);
	if ((reg_read(packet, 0) & 0xff) != 0x82)
		return;
	uint8 oldCode = word2 & 0xff;
	uint8 checksum = (word1 & 0xff) + oldCode - (uint8)videoCode;
	reg_write(packet, 8, (word2 & ~0xffu) | (uint8)videoCode);
	reg_write(packet, 4, (word1 & ~0xffu) | checksum);
}


/*!	One I2C message on output \a index's DDC: \a length (at most 8) bytes
	written or read, \a flags the start and stop conditions. */
static status_t
bsc_transfer(display_info* info, uint32 index, uint8 address, bool read,
	uint8* data, uint32 length, uint32 flags)
{
	volatile uint8* bsc = info->ddcRegs[index];
	if (bsc == NULL || length == 0 || length > 8)
		return B_BAD_VALUE;

	reg_write(bsc, BSC_CTLHI, 0);
	reg_write(bsc, BSC_CTL, BSC_CTL_97500_HZ | (read ? BSC_CTL_READ : 0));
	reg_write(bsc, BSC_CHIP_ADDRESS, address << 1 | (read ? 1 : 0));
	reg_write(bsc, BSC_CNT, length);
	if (!read) {
		for (uint32 i = 0; i < length; i++)
			reg_write(bsc, BSC_DATA_IN(i), data[i]);
	}
	reg_write(bsc, BSC_IIC_ENABLE, flags | BSC_IIC_ENABLE_ENABLE);

	status_t status = B_OK;
	bigtime_t timeout = system_time() + 100000;
	while ((reg_read(bsc, BSC_IIC_ENABLE) & BSC_IIC_ENABLE_INTRP) == 0) {
		if (system_time() > timeout) {
			status = B_TIMED_OUT;
			break;
		}
		spin(20);
	}
	if (status == B_OK
		&& (reg_read(bsc, BSC_IIC_ENABLE) & BSC_IIC_ENABLE_NOACK) != 0) {
		status = B_DEV_NOT_READY;
	}
	if (status == B_OK && read) {
		for (uint32 i = 0; i < length; i++)
			data[i] = reg_read(bsc, BSC_DATA_OUT(i)) & 0xff;
	}
	reg_write(bsc, BSC_CNT, 0);
	reg_write(bsc, BSC_IIC_ENABLE, 0);
	return status;
}


static status_t
scdc_read(display_info* info, uint32 index, uint8 offset, uint8& value)
{
	// the offset without a stop, then the read after a repeated start
	status_t status = bsc_transfer(info, index, SCDC_ADDRESS, false, &offset,
		1, BSC_IIC_ENABLE_RESTART | BSC_IIC_ENABLE_NOSTOP);
	if (status == B_OK)
		status = bsc_transfer(info, index, SCDC_ADDRESS, true, &value, 1, 0);
	return status;
}


static status_t
scdc_write(display_info* info, uint32 index, uint8 offset, uint8 value)
{
	uint8 data[2] = { offset, value };
	return bsc_transfer(info, index, SCDC_ADDRESS, false, data, 2, 0);
}


/*!	HDMI 2.0 scrambling and the 1/40 TMDS clock ratio, in the monitor
	(SCDC) and in the encoder, for TMDS rates above 340 MHz (Linux's
	vc4_hdmi_enable_scrambling()). */
static void
bcm2712_set_scrambling(display_info* info, uint32 index, bool enable)
{
	volatile uint8* hdmi = info->hdmiRegs[index];
	if (!enable) {
		if (!info->scrambling[index])
			return;
		scdc_write(info, index, SCDC_TMDS_CONFIG, 0);
		reg_write(hdmi, HDMI6_SCRAMBLER_CTL,
			reg_read(hdmi, HDMI6_SCRAMBLER_CTL) & ~HDMI6_SCRAMBLER_ENABLE);
		info->scrambling[index] = false;
		return;
	}

	status_t status = scdc_write(info, index, SCDC_TMDS_CONFIG,
		SCDC_SCRAMBLING_ENABLE | SCDC_TMDS_CLOCK_RATIO_40);
	if (status != B_OK) {
		ERROR("HDMI%" B_PRIu32 ": the monitor's SCDC: %s\n", index,
			strerror(status));
	}
	reg_write(hdmi, HDMI6_SCRAMBLER_CTL,
		reg_read(hdmi, HDMI6_SCRAMBLER_CTL) | HDMI6_SCRAMBLER_ENABLE);
	info->scrambling[index] = true;
}


/*!	A monitor forgets the SCDC settings when it is switched off or sleeps:
	asked again once a second while it should scramble (Linux's
	vc4_hdmi_scrambling_wq()). */
static void
bcm2712_check_scrambling(display_info* info, uint32 index)
{
	if (!info->scrambling[index])
		return;
	uint8 status;
	if (scdc_read(info, index, SCDC_SCRAMBLER_STATUS, status) != B_OK
		|| (status & 1) != 0) {
		return;
	}
	INFO("HDMI%" B_PRIu32 ": the monitor stopped descrambling, on again\n",
		index);
	scdc_write(info, index, SCDC_TMDS_CONFIG,
		SCDC_SCRAMBLING_ENABLE | SCDC_TMDS_CLOCK_RATIO_40);
}


/*!	Sets output \a index to \a t (Linux's vc4_crtc_disable(), then
	vc4_crtc_atomic_enable() with the HDMI encoder's hooks). The compositor
	channel keeps its display list. */
static status_t
bcm2712_set_mode(display_info* info, uint32 index, const firmware_timing& t)
{
	volatile uint8* hdmi = info->hdmiRegs[index];
	volatile uint8* pv = info->pvRegs[index];
	volatile uint8* hd = info->hdRegs;
	if (hdmi == NULL || pv == NULL || hd == NULL)
		return B_NO_INIT;
	if (((t.hdisplay | t.hsync_start | t.hsync_end | t.htotal) & 1) != 0)
		return B_BAD_VALUE;
	uint64 tmdsRate = (uint64)t.clock * 1000;
	bool hsyncHigh = (t.flags & 1) != 0;
	bool vsyncHigh = (t.flags & 2) != 0;

	// off: the pixel valve's video, the packets, the picture, scrambling
	bcm2712_set_scrambling(info, index, false);
	reg_write(pv, PV6_V_CONTROL,
		reg_read(pv, PV6_V_CONTROL) & ~PV6_VCONTROL_VIDEN);
	spin(20000);
	uint32 packets = reg_read(hdmi, HDMI6_RAM_PACKET_CONFIG);
	reg_write(hdmi, HDMI6_RAM_PACKET_CONFIG, 0);
	reg_write(hd, HD6_VID_CTL(index), reg_read(hd, HD6_VID_CTL(index))
		| HD6_VID_CTL_CLRRGB | HD6_VID_CTL_BLANKPIX);
	spin(1000);
	uint32 pvControl = reg_read(pv, PV6_CONTROL) & ~PV6_CONTROL_EN;
	reg_write(pv, PV6_CONTROL, pvControl);
	reg_write(pv, PV6_CONTROL, pvControl | PV6_CONTROL_FIFO_CLR);
	uint32 control = hvs6_read(info, HVS6_DISP_CTRL0(index));
	hvs6_write(info, HVS6_DISP_CTRL0(index), control | HVS6_DISP_CTRL0_RESET);
	hvs6_write(info, HVS6_DISP_CTRL0(index),
		(control | HVS6_DISP_CTRL0_RESET) & ~HVS6_DISP_CTRL0_ENB);

	// the compositor's channel at the new size
	hvs6_write(info, HVS6_DISP_CTRL0(index), HVS6_DISP_CTRL0_RESET);
	hvs6_write(info, HVS6_DISP_CTRL1(index),
		hvs6_read(info, HVS6_DISP_CTRL1(index)) & ~HVS6_DISP_CTRL1_INTLACE);
	hvs6_write(info, HVS6_DISP_CTRL0(index), HVS6_DISP_CTRL0_ENB
		| (uint32)(t.hdisplay - 1) << 16 | (uint32)(t.vdisplay - 1));

	// the encoder: PHY, timings
	bcm2712_phy_init(info, index, tmdsRate);
	reg_write(hdmi, HDMI6_SCHEDULER_CONTROL,
		reg_read(hdmi, HDMI6_SCHEDULER_CONTROL)
			| HDMI6_SCHEDULER_MANUAL_FORMAT
			| HDMI6_SCHEDULER_IGNORE_VSYNC_PREDICTS);
	reg_write(hdmi, HDMI6_HORZA, (vsyncHigh ? 1u << 15 : 0)
		| (hsyncHigh ? 1u << 14 : 0) | t.hdisplay
		| (uint32)(t.hsync_start - t.hdisplay) << 16);
	reg_write(hdmi, HDMI6_HORZB, (uint32)(t.htotal - t.hsync_end) << 16
		| (uint32)(t.hsync_end - t.hsync_start));
	uint32 verta = (uint32)(t.vsync_end - t.vsync_start) << 24
		| (uint32)(t.vsync_start - t.vdisplay) << 16 | t.vdisplay;
	uint32 vertb = (uint32)(t.htotal >> 1) << 16
		| (uint32)(t.vtotal - t.vsync_end);
	reg_write(hdmi, HDMI6_VERTA0, verta);
	reg_write(hdmi, HDMI6_VERTA1, verta);
	reg_write(hdmi, HDMI6_VERTB0, t.vtotal - t.vsync_end);
	reg_write(hdmi, HDMI6_VERTB1, vertb);

	// the pixel valve, two pixels a clock
	const uint32 ppc = PV6_PIXELS_PER_CLOCK;
	reg_write(pv, PV6_HORZA, (uint32)(t.htotal - t.hsync_end) / ppc << 16
		| (uint32)(t.hsync_end - t.hsync_start) / ppc);
	reg_write(pv, PV6_HORZB, (uint32)(t.hsync_start - t.hdisplay) / ppc << 16
		| (uint32)t.hdisplay / ppc);
	reg_write(pv, PV6_V_CONTROL, PV6_VCONTROL_CONTINUOUS);
	reg_write(pv, PV6_VSYNCD_EVEN, 0);
	reg_write(pv, PV6_VERTA, (uint32)(t.vtotal - t.vsync_end) << 16
		| (uint32)(t.vsync_end - t.vsync_start));
	reg_write(pv, PV6_VERTB, (uint32)(t.vsync_start - t.vdisplay) << 16
		| t.vdisplay);
	reg_write(pv, PV6_CONTROL, pvControl | PV6_CONTROL_FIFO_CLR);
	reg_write(pv, PV6_CONTROL, pvControl | PV6_CONTROL_EN);
	reg_write(hdmi, HDMI6_FIFO_CTL, HDMI6_FIFO_CTL_MASTER_SLAVE_N);
	reg_write(pv, PV6_V_CONTROL,
		reg_read(pv, PV6_V_CONTROL) | PV6_VCONTROL_VIDEN);

	// on: the picture, HDMI mode, the packets
	uint32 vidControl = reg_read(hd, HD6_VID_CTL(index))
		& ~(HD6_VID_CTL_VSYNC_LOW | HD6_VID_CTL_HSYNC_LOW
			| HD6_VID_CTL_CLRRGB | HD6_VID_CTL_BLANKPIX);
	reg_write(hd, HD6_VID_CTL(index), vidControl | HD6_VID_CTL_ENABLE
		| HD6_VID_CTL_BLANK_INSERT_EN
		| (vsyncHigh ? 0 : HD6_VID_CTL_VSYNC_LOW)
		| (hsyncHigh ? 0 : HD6_VID_CTL_HSYNC_LOW));
	reg_write(hdmi, HDMI6_SCHEDULER_CONTROL,
		reg_read(hdmi, HDMI6_SCHEDULER_CONTROL) | HDMI6_SCHEDULER_MODE_HDMI);
	bigtime_t timeout = system_time() + 1000000;
	while ((reg_read(hdmi, HDMI6_SCHEDULER_CONTROL)
			& HDMI6_SCHEDULER_HDMI_ACTIVE) == 0) {
		if (system_time() > timeout) {
			ERROR("HDMI%" B_PRIu32 ": HDMI mode does not come on\n", index);
			break;
		}
		snooze(1000);
	}
	bcm2712_update_avi(info, index, t.video_id_code);
	reg_write(hdmi, HDMI6_RAM_PACKET_CONFIG, packets);

	// recenter the FIFO between the pixel valve and the encoder
	uint32 drift = reg_read(hdmi, HDMI6_FIFO_CTL) & HDMI6_FIFO_VALID_WRITE_MASK;
	for (int i = 0; i < 2; i++) {
		reg_write(hdmi, HDMI6_FIFO_CTL, drift & ~HDMI6_FIFO_CTL_RECENTER);
		reg_write(hdmi, HDMI6_FIFO_CTL, drift | HDMI6_FIFO_CTL_RECENTER);
		if (i == 0)
			snooze(1000);
	}
	spin(1000);
	if ((reg_read(hdmi, HDMI6_FIFO_CTL) & HDMI6_FIFO_CTL_RECENTER_DONE) == 0)
		ERROR("HDMI%" B_PRIu32 ": the FIFO did not recenter\n", index);

	if (tmdsRate > BCM2712_MAX_PIXEL_CLOCK * 1000ull)
		bcm2712_set_scrambling(info, index, true);

	INFO("HDMI%" B_PRIu32 ": %ux%u at %u Hz, %" B_PRIu32 " kHz (code %u)\n",
		index, t.hdisplay, t.vdisplay, t.vrefresh, t.clock, t.video_id_code);
	return B_OK;
}


/*!	The timing for output \a index: its EDID's preferred one the output can
	send, or the size set in the driver settings ("hdmi0 1280x720"). */
static bool
bcm2712_pick_timing(const rpi_display_output& output, uint32 index,
	firmware_timing& timing)
{
	uint16 width = 0, height = 0;
	void* settings = load_driver_settings("rpi_display");
	if (settings != NULL) {
		char key[8];
		snprintf(key, sizeof(key), "hdmi%" B_PRIu32, index);
		const char* value = get_driver_parameter(settings, key, NULL, NULL);
		if (value != NULL) {
			char* end;
			unsigned long w = strtoul(value, &end, 10);
			if (*end == 'x') {
				width = w;
				height = strtoul(end + 1, NULL, 10);
			}
		}
		unload_driver_settings(settings);
	}
	// above 340 MHz with scrambling, if the monitor can be told of it
	uint32 maxClock = BCM2712_MAX_PIXEL_CLOCK;
	uint32 maxTmds;
	bool scdc;
	if (sInfo->ddcRegs[index] != NULL
		&& hdmi_forum_capabilities(output.edid, output.edid_length, maxTmds,
			scdc) && scdc && maxTmds > maxClock) {
		maxClock = min_c(maxTmds, BCM2712_MAX_SCRAMBLED_CLOCK);
	}
	if (!preferred_edid_timing(output.edid, output.edid_length, timing,
			maxClock, width, height)) {
		return false;
	}
	// a detailed timing that is a CTA mode is named as one
	if (timing.video_id_code == 0 && (timing.flags & RPI_TIMING_DVI) == 0)
		timing.video_id_code = cta_code_of(timing);
	return true;
}


static status_t
connect_output_bcm2712(rpi_display_output& output)
{
	display_info* info = sInfo;
	uint32 index = output.id == 2 ? 0 : 1;
	// An output the firmware did not start at boot cannot be shown: there
	// is no mode setting here.
	if (!hvs6_read_firmware_plane(info, index))
		return B_DEV_NOT_READY;

	rpi_display_output connected = {};
	connected.id = output.id;
	read_edid(connected);

	// the EDID's mode, if it is not the firmware's
	uint32 control = hvs6_read(info, HVS6_DISP_CTRL0(index));
	firmware_timing timing;
	if (bcm2712_pick_timing(connected, index, timing)
		&& (timing.hdisplay != ((control >> 16) & 0x1fff) + 1
			|| timing.vdisplay != (control & 0x1fff) + 1
			|| (info->modeClock[index] != 0
				&& timing.clock != info->modeClock[index]))) {
		// the firmware's list (its frame buffer) until a layout comes
		bool shown = info->regionShown[index];
		hvs6_restore(info, index);
		if (bcm2712_set_mode(info, index, timing) == B_OK)
			info->modeClock[index] = timing.clock;
		if (shown)
			ERROR("HDMI%" B_PRIu32 ": mode changed under a layout\n", index);
		control = hvs6_read(info, HVS6_DISP_CTRL0(index));
	}
	connected.native_width = ((control >> 16) & 0x1fff) + 1;
	connected.native_height = (control & 0x1fff) + 1;
	connected.flags = RPI_DISPLAY_OUTPUT_CONNECTED;
	output = connected;
	INFO("HDMI%" B_PRIu32 ": %ux%u, %" B_PRIu32
		" bytes of EDID\n", index, output.native_width,
		output.native_height, output.edid_length);
	return B_OK;
}


static status_t
read_outputs_bcm2712(display_info* info)
{
	bool found = false;
	for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
		rpi_display_output& output = info->shared->outputs[i];
		if (connect_output_bcm2712(output) == B_OK)
			found = true;
	}
	return found ? B_OK : B_DEVICE_NOT_FOUND;
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
	// the Pi 4's VideoCore sees the first gigabyte; the BCM2712's
	// compositor all of it
	if (!info->bcm2712)
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
	if (info->bcm2712) {
		hvs6_update_cursor(info);
		return B_OK;
	}
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

			// fkms declares its cursor planes at the buffer's own pitch
			// (width == pitch / 4), the rest of the buffer transparent;
			// tight widths behind the stride read as lines. The canvas
			// mode follows that; the tight mode follows the cursor size.
			uint32 planePixels = info->cursorCanvas
				? RPI_DISPLAY_CURSOR_MAX : info->cursor.width;
			uint32 planeRows = info->cursorCanvas
				? RPI_DISPLAY_CURSOR_MAX : info->cursor.height;

			plane.image_type = info->cursorImageType;
			plane.layer = info->cursorLayer;
			plane.width = planePixels;
			plane.height = planeRows;
			plane.pitch = RPI_DISPLAY_CURSOR_MAX * 4;
			plane.vpitch = 0;
			plane.src_x = 0;
			plane.src_y = 0;
			plane.src_w = planePixels << 16;
			plane.src_h = planeRows << 16;
			plane.dst_x = (int16)(destX + (int32)((info->cursorX - output.x)
				* scaleX - info->cursor.hot_x * scaleX
				+ (info->cursorX >= output.x ? 0.5 : -0.5)));
			plane.dst_y = (int16)(destY + (int32)((info->cursorY - output.y)
				* scaleY - info->cursor.hot_y * scaleY
				+ (info->cursorY >= output.y ? 0.5 : -0.5)));
			int32 cursorWidth
				= (int32)(planePixels * scaleX + 0.5);
			int32 cursorHeight
				= (int32)(planeRows * scaleY + 0.5);
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
	if (status != B_OK) {
		ERROR("no frame buffer for %" B_PRIu32 "x%" B_PRIu32 ": %s\n",
			layout.width, layout.height, strerror(status));
		return status;
	}

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
		// (the BCM2712's planes are unscaled: the region, centered)
		int32 destX, destY;
		uint32 destWidth, destHeight;
		if (info->bcm2712) {
			destWidth = request.width;
			destHeight = request.height;
			status = hvs6_show(info, i, request.x, request.y, request.width,
				request.height, bytesPerRow);
			if (status != B_OK) {
				ERROR("output %" B_PRIu32 "'s plane: %s\n", i,
					strerror(status));
				return status;
			}
		} else {
			output_dest(output.native_width, output.native_height,
				request.width, request.height, &destX, &destY, &destWidth,
				&destHeight);

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
				ERROR("the firmware did not take output %" B_PRIu32 "'s "
					"plane: %s\n", i, strerror(status));
				return status;
			}
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
			if (!info->bcm2712) {
				// the cursor rides its own plane
				firmware_plane cursor = {};
				cursor.display = shared.outputs[i].id;
				cursor.plane_id = i * CURSOR_PLANES_PER_DISPLAY
					+ info->cursorPlaneOffset;
				set_plane(cursor);
			}
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


//	#pragma mark - copies by the DMA engine


/*!	The BCM2712's 40 bit DMA channels (Linux: bcm2835-dma.c, BCM2711_DMA40_*;
	the "dma40" controller of bcm2712.dtsi, channels 6 to 10 left to the OS,
	7 taken by the HDMI audio driver). A copy is a chain of control blocks in
	2D mode, one per rectangle: rows of XLENGTH bytes, YLENGTH of them, the
	strides added at each row's end. The engine does not snoop the CPU's
	caches: what the CPU drew into a source is written back first, and the
	shadows and the frame buffer are memory the CPU does not cache.
*/
#define DMA40_BASE					0x1000010000ull
#define DMA40_CHANNEL_SIZE			0x100
#define DMA40_CS					0x00
#define DMA40_CB					0x04
#define DMA40_DEBUG					0x0c
#define DMA40_CS_ACTIVE				(1u << 0)
#define DMA40_CS_END				(1u << 1)
#define DMA40_CS_INT				(1u << 2)
#define DMA40_CS_ERR				(1u << 10)
#define DMA40_CS_WAIT_FOR_WRITES	(1u << 28)
#define DMA40_CS_ABORT				(1u << 30)
#define DMA40_CS_HALT				(1u << 31)
#define DMA40_DEBUG_RESET			(1u << 23)
#define DMA40_TI_TDMODE				(1u << 1)
#define DMA40_TI_WAIT_RESP			(1u << 2)
#define DMA40_TI_WAIT_RD_RESP		(1u << 3)
#define DMA40_INFO_BURST(x)			(((uint32)(x) & 15) << 8)
#define DMA40_INFO_INC				(1u << 12)
#define DMA40_INFO_SIZE_32			(0u << 13)
#define DMA40_INFO_SIZE_128			(2u << 13)
#define DMA40_INFO_STRIDE(x)		(((uint32)(x) & 0xffff) << 16)

// Two engines a direction, each copying half of the rows: one engine
// moves about 1 GB/s, two together more. Filling sources and shadows;
// copying to the frame buffer.
static const uint32 kDmaEngines[2][2] = { { 6, 8 }, { 9, 10 } };
#define DMA_FILL					0
#define DMA_SCREEN					1
#define DMA_CHANNEL_COUNT			2
#define DMA_ENGINES					2
#define DMA_BLOCK_PAGES				4
#define DMA_MAX_BLOCKS				(DMA_BLOCK_PAGES * B_PAGE_SIZE / 32)
#define MAX_SOURCES					8
static const bigtime_t kDmaTimeout = 200000;
// bursts of 16 transfers: 4 and 8 copy a half and a fifth slower; waiting
// for each write's response makes no difference
#define DMA_BURST					15

struct dma40_cb {
	uint32	ti;
	uint32	src;
	uint32	srci;
	uint32	dst;
	uint32	dsti;
	uint32	len;
	uint32	next_cb;
	uint32	reserved;
};

struct dma_engine {
	area_id			registersArea;
	volatile uint8*	registers;
	area_id			blocksArea;
	dma40_cb*		blocks;
	phys_addr_t		blocksPhysical;
	uint32			blockCount;		// of the copy under way
	uint64			bytes;
};

// one direction's engines
struct display_dma_channel {
	mutex			lock;
	dma_engine		engines[DMA_ENGINES];
	bool			failed;
	// statistics, for the syslog now and then
	uint32			copies;
	uint64			bytes;
	bigtime_t		busy;
	bigtime_t		reported;
};

struct display_source {
	uint32			id;			// 0: free
	void*			owner;		// the cookie that made it
	bool			shadow;
	area_id			area;
	uint8*			address;
	phys_addr_t		physical;
	uint32			width;
	uint32			height;
	uint32			bytesPerRow;
	team_id			team;
	area_id			clone;
};


static void
dma_reset(dma_engine& engine)
{
	reg_write(engine.registers, DMA40_CS, DMA40_CS_HALT);
	spin(10);
	reg_write(engine.registers, DMA40_DEBUG, DMA40_DEBUG_RESET);
	spin(10);
	reg_write(engine.registers, DMA40_CS, DMA40_CS_INT | DMA40_CS_END);
}


static void
uninit_dma(display_info* info)
{
	if (info->dmaChannels != NULL) {
		for (uint32 i = 0; i < DMA_CHANNEL_COUNT; i++) {
			display_dma_channel& channel = info->dmaChannels[i];
			for (uint32 k = 0; k < DMA_ENGINES; k++) {
				dma_engine& engine = channel.engines[k];
				if (engine.registers != NULL)
					dma_reset(engine);
				if (engine.registersArea >= 0)
					delete_area(engine.registersArea);
				if (engine.blocksArea >= 0)
					delete_area(engine.blocksArea);
			}
			mutex_destroy(&channel.lock);
		}
		delete[] info->dmaChannels;
		info->dmaChannels = NULL;
	}
	if (info->sources != NULL) {
		for (uint32 i = 0; i < MAX_SOURCES; i++) {
			if (info->sources[i].id != 0)
				delete_area(info->sources[i].area);
		}
		delete[] info->sources;
		info->sources = NULL;
	}
}


static status_t
init_engine(dma_engine& engine, uint32 number)
{
	if (map_page("rpi display dma", DMA40_BASE + number * DMA40_CHANNEL_SIZE,
			engine.registersArea, engine.registers) != B_OK)
		return B_ERROR;

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	void* address;
	engine.blocksArea = create_area_etc(B_SYSTEM_TEAM, "rpi display dma blocks",
		DMA_BLOCK_PAGES * B_PAGE_SIZE, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &virtualRestrictions,
		&physicalRestrictions, &address);
	physical_entry entry;
	if (engine.blocksArea < 0
		|| get_memory_map(address, B_PAGE_SIZE, &entry, 1) != B_OK
		|| vm_set_area_memory_type(engine.blocksArea, entry.address,
			B_UNCACHED_MEMORY) != B_OK)
		return B_ERROR;
	engine.blocks = (dma40_cb*)address;
	engine.blocksPhysical = entry.address;
	dma_reset(engine);
	return B_OK;
}


/*!	The engines, their control blocks, and the source table; with any of it
	missing there are no copies (and the accelerant offers none). */
static void
init_dma(display_info* info)
{
	if (!info->bcm2712)
		return;
	// "device_copies false" in the driver's settings: the CPU copies, as
	// before; "device_copies test": the ioctls work, but the accelerant is
	// not told
	void* settings = load_driver_settings("rpi_display");
	const char* mode = get_driver_parameter(settings, "device_copies", "true",
		"true");
	bool enabled = strcmp(mode, "false") != 0;
	bool advertised = enabled && strcmp(mode, "test") != 0;
	unload_driver_settings(settings);
	if (!enabled)
		return;
	info->dmaChannels = new(std::nothrow) display_dma_channel[DMA_CHANNEL_COUNT];
	info->sources = new(std::nothrow) display_source[MAX_SOURCES];
	if (info->dmaChannels == NULL || info->sources == NULL) {
		delete[] info->dmaChannels;
		delete[] info->sources;
		info->dmaChannels = NULL;
		info->sources = NULL;
		return;
	}
	memset(info->sources, 0, sizeof(display_source) * MAX_SOURCES);
	info->nextSourceID = 1;

	bool ok = true;
	for (uint32 i = 0; i < DMA_CHANNEL_COUNT; i++) {
		display_dma_channel& channel = info->dmaChannels[i];
		memset(&channel, 0, sizeof(channel));
		mutex_init(&channel.lock, i == DMA_FILL ? "rpi display dma fill"
			: "rpi display dma screen");
		for (uint32 k = 0; k < DMA_ENGINES; k++) {
			channel.engines[k].registersArea = -1;
			channel.engines[k].blocksArea = -1;
			if (ok && init_engine(channel.engines[k], kDmaEngines[i][k]) != B_OK)
				ok = false;
		}
	}
	if (!ok) {
		ERROR("no DMA channels: the CPU copies to the screen\n");
		uninit_dma(info);
		return;
	}
	if (advertised)
		info->shared->flags |= RPI_DISPLAY_DEVICE_COPIES;
}


static display_source*
find_source(display_info* info, uint32 id)
{
	if (info->sources == NULL || id == 0)
		return NULL;
	for (uint32 i = 0; i < MAX_SOURCES; i++) {
		if (info->sources[i].id == id)
			return &info->sources[i];
	}
	return NULL;
}


/*!	Both channels, before the device lock (the order copies take them in):
	held while a source or the frame buffer is let go of, which a copy under
	way may still read or write. */
static void
lock_dma(display_info* info)
{
	if (info->dmaChannels == NULL)
		return;
	for (uint32 i = 0; i < DMA_CHANNEL_COUNT; i++)
		mutex_lock(&info->dmaChannels[i].lock);
}


static void
unlock_dma(display_info* info)
{
	if (info->dmaChannels == NULL)
		return;
	for (uint32 i = DMA_CHANNEL_COUNT; i-- > 0;)
		mutex_unlock(&info->dmaChannels[i].lock);
}


static void
delete_source(display_source& source)
{
	// the caller's mapping first: its pages stay with it otherwise
	if (source.clone >= 0)
		vm_delete_area(source.team, source.clone, true);
	delete_area(source.area);
	source.id = 0;
}


static status_t
create_source(display_info* info, void* owner, rpi_display_source& request)
{
	if (info->sources == NULL)
		return B_NOT_SUPPORTED;
	if (request.width == 0 || request.height == 0)
		return B_BAD_VALUE;
	display_source* source = NULL;
	for (uint32 i = 0; i < MAX_SOURCES; i++) {
		if (info->sources[i].id == 0) {
			source = &info->sources[i];
			break;
		}
	}
	if (source == NULL)
		return B_NO_MEMORY;

	// whole 64 byte lines a row: the CPU's cache lines, and the engine's
	// bursts start aligned
	const bool shadow = (request.flags & RPI_DISPLAY_SOURCE_SHADOW) != 0;
	uint32 bytesPerRow = ROUNDUP((uint32)request.width * 4, 64);
	size_t size = ROUNDUP((size_t)bytesPerRow * request.height, B_PAGE_SIZE);
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, shadow
			? "rpi display shadow" : "rpi display source", size, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &virtualRestrictions,
		&physicalRestrictions, &address);
	if (area < 0)
		return area;
	physical_entry entry;
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	// a shadow is the engine's alone: not cached, nothing to write back
	if (status == B_OK && shadow) {
		status = vm_set_area_memory_type(area, entry.address,
			B_WRITE_COMBINING_MEMORY);
	}
	if (status != B_OK) {
		delete_area(area);
		return status;
	}
	memset(address, 0, size);
	if (!shadow) {
		// nothing of the clear stays in the cache to be written back over
		// what the engine copies later
		for (addr_t line = (addr_t)address; line < (addr_t)address + size;
				line += 64)
			asm volatile("dc civac, %0" :: "r"(line) : "memory");
		asm volatile("dsb sy" ::: "memory");
	}

	area_id clone = -1;
	void* cloneAddress = NULL;
	if (!shadow) {
		clone = vm_clone_area(team_get_current_team_id(), "rpi display source",
			&cloneAddress, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, 0, area,
			true);
		if (clone < 0) {
			delete_area(area);
			return clone;
		}
	}

	source->id = info->nextSourceID++;
	if (info->nextSourceID == 0)
		info->nextSourceID = 1;
	source->owner = owner;
	source->shadow = shadow;
	source->area = area;
	source->address = (uint8*)address;
	source->physical = entry.address;
	source->width = request.width;
	source->height = request.height;
	source->bytesPerRow = bytesPerRow;
	source->team = team_get_current_team_id();
	source->clone = clone;

	request.id = source->id;
	request.bytes_per_row = bytesPerRow;
	request.area = clone;
	request.address = cloneAddress;
	INFO("%s %" B_PRIu32 ": %" B_PRIu32 "x%" B_PRIu32 " at %#" B_PRIxPHYSADDR
		"\n", shadow ? "shadow" : "source", source->id, source->width,
		source->height, source->physical);
	return B_OK;
}


/*!	One end of a copy: a source, or the frame buffer for
	RPI_DISPLAY_FRAME_BUFFER_ID. Locked. */
struct copy_end {
	phys_addr_t	physical;
	uint8*		address;		// where the CPU caches it, else NULL
	uint32		bytesPerRow;
	uint32		width;
	uint32		height;
};


static bool
copy_end_of(display_info* info, uint32 id, copy_end& end)
{
	if (id == RPI_DISPLAY_FRAME_BUFFER_ID) {
		if (info->bufferArea < 0)
			return false;
		end.physical = info->bufferAddress;
		end.address = NULL;
		end.bytesPerRow = info->shared->bytes_per_row;
		end.width = info->shared->width;
		end.height = info->shared->height;
		return end.bytesPerRow != 0;
	}
	display_source* source = find_source(info, id);
	if (source == NULL)
		return false;
	end.physical = source->physical;
	end.address = source->shadow ? NULL : source->address;
	end.bytesPerRow = source->bytesPerRow;
	end.width = source->width;
	end.height = source->height;
	return true;
}


/*!	Control blocks for rows [top, top + rows) of a rectangle, on one engine.
	Rows that do not start on 16 bytes: 32 bit transfers are some eight times
	slower than 128 bit ones, so only the up to three pixels on either side
	go that way (both ends have the same alignment: their rows are whole 64
	byte lines apart). */
static bool
add_blocks(dma_engine& engine, const copy_end& from, const copy_end& to,
	uint32 left, uint32 top, uint32 rowBytes, uint32 rows)
{
	uint64 source = from.physical + (uint64)top * from.bytesPerRow + left * 4;
	uint64 destination = to.physical + (uint64)top * to.bytesPerRow + left * 4;
	uint32 head = (16 - (uint32)(source & 15)) & 15;
	if ((destination & 15) != (source & 15) || head >= rowBytes)
		head = rowBytes;
	uint32 middle = (rowBytes - head) & ~15u;
	uint32 tail = rowBytes - head - middle;
	const uint32 parts[3][2] = {
		{ 0, head }, { head, middle }, { head + middle, tail }
	};
	for (uint32 k = 0; k < 3; k++) {
		uint32 offset = parts[k][0], width = parts[k][1];
		if (width == 0)
			continue;
		if (engine.blockCount == DMA_MAX_BLOCKS)
			return false;
		uint64 partSource = source + offset;
		uint64 partDestination = destination + offset;
		uint32 size = (partSource & 15) == 0 && (partDestination & 15) == 0
			&& (width & 15) == 0 ? DMA40_INFO_SIZE_128 : DMA40_INFO_SIZE_32;
		dma40_cb& block = engine.blocks[engine.blockCount];
		block.ti = DMA40_TI_TDMODE;
		block.src = (uint32)partSource;
		block.srci = (uint32)(partSource >> 32) | DMA40_INFO_INC | size
			| DMA40_INFO_BURST(DMA_BURST)
			| DMA40_INFO_STRIDE(from.bytesPerRow - width);
		block.dst = (uint32)partDestination;
		block.dsti = (uint32)(partDestination >> 32) | DMA40_INFO_INC | size
			| DMA40_INFO_BURST(DMA_BURST)
			| DMA40_INFO_STRIDE(to.bytesPerRow - width);
		// YLENGTH counts the rows after the first
		block.len = ((rows - 1) << 16) | width;
		block.next_cb = 0;
		block.reserved = 0;
		if (engine.blockCount > 0) {
			engine.blocks[engine.blockCount - 1].next_cb = (uint32)(
				(engine.blocksPhysical + engine.blockCount * sizeof(dma40_cb))
					>> 5);
		}
		engine.blockCount++;
	}
	engine.bytes += (uint64)rowBytes * rows;
	return true;
}


/*!	Copies the rectangles from one end to the other, and returns once the
	engines have written them. Called without the device lock. */
static status_t
dma_copy(display_info* info, display_dma_channel& channel,
	const rpi_display_copy& request, const uint16* rects)
{
	const uint32 count = request.count;
	if (count == 0)
		return B_OK;
	if (count > RPI_DISPLAY_MAX_COPY_RECTS)
		return B_BAD_VALUE;

	// The channel's lock keeps both ends: letting go of a source or the
	// frame buffer takes it first.
	MutexLocker locker(channel.lock);
	if (channel.failed)
		return B_ERROR;
	copy_end from, to;
	{
		MutexLocker deviceLocker(info->lock);
		if (request.from == RPI_DISPLAY_FRAME_BUFFER_ID
			|| !copy_end_of(info, request.from, from)
			|| !copy_end_of(info, request.to, to))
			return B_BAD_VALUE;
	}
	if (from.bytesPerRow > 0x7fff || to.bytesPerRow > 0x7fff)
		return B_BAD_VALUE;

	for (uint32 k = 0; k < DMA_ENGINES; k++) {
		channel.engines[k].blockCount = 0;
		channel.engines[k].bytes = 0;
	}
	uint64 bytes = 0;
	for (uint32 i = 0; i < count; i++) {
		const uint16* r = rects + i * 4;
		uint32 left = r[0], top = r[1], right = r[2], bottom = r[3];
		if (right < left || bottom < top || right >= from.width
			|| right >= to.width || bottom >= from.height
			|| bottom >= to.height)
			return B_BAD_VALUE;
		uint32 rowBytes = (right - left + 1) * 4;
		uint32 rows = bottom - top + 1;

		// what the CPU drew is written back to memory, where the engines
		// read
		if (from.address != NULL) {
			uint8* row = from.address + (size_t)top * from.bytesPerRow
				+ left * 4;
			for (uint32 y = 0; y < rows; y++, row += from.bytesPerRow) {
				for (addr_t line = (addr_t)row & ~(addr_t)63;
						line < (addr_t)row + rowBytes; line += 64)
					asm volatile("dc cvac, %0" :: "r"(line) : "memory");
			}
		}

		// a large one half each, small ones to the engine with less to do
		bool ok;
		if (rows >= 16 && (uint64)rowBytes * rows >= 64 * 1024) {
			uint32 half = rows / 2;
			ok = add_blocks(channel.engines[0], from, to, left, top, rowBytes,
					half)
				&& add_blocks(channel.engines[1], from, to, left, top + half,
					rowBytes, rows - half);
		} else {
			dma_engine& engine = channel.engines[0].bytes
				<= channel.engines[1].bytes ? channel.engines[0]
					: channel.engines[1];
			ok = add_blocks(engine, from, to, left, top, rowBytes, rows);
		}
		if (!ok)
			return B_BAD_VALUE;
		bytes += (uint64)rowBytes * rows;
	}
	asm volatile("dsb sy" ::: "memory");

	bigtime_t start = system_time();
	uint64 most = 0;
	for (uint32 k = 0; k < DMA_ENGINES; k++) {
		dma_engine& engine = channel.engines[k];
		if (engine.blockCount == 0)
			continue;
		reg_write(engine.registers, DMA40_CS, DMA40_CS_INT | DMA40_CS_END);
		reg_write(engine.registers, DMA40_CB,
			(uint32)(engine.blocksPhysical >> 5));
		reg_write(engine.registers, DMA40_CS, DMA40_CS_ACTIVE
			| DMA40_CS_WAIT_FOR_WRITES);
		if (engine.bytes > most)
			most = engine.bytes;
	}

	// about 1 GB/s an engine: sleep through most of it, then look often
	bigtime_t expected = most / 1000;
	if (expected > 200)
		snooze(expected - 100);
	bool failed = false;
	for (uint32 k = 0; k < DMA_ENGINES; k++) {
		dma_engine& engine = channel.engines[k];
		if (engine.blockCount == 0)
			continue;
		uint32 cs;
		while (true) {
			cs = reg_read(engine.registers, DMA40_CS);
			if ((cs & DMA40_CS_ACTIVE) == 0 || (cs & DMA40_CS_ERR) != 0)
				break;
			if (system_time() - start > kDmaTimeout)
				break;
			if (expected > 200)
				snooze(50);
			else
				spin(5);
		}
		if ((cs & DMA40_CS_ACTIVE) != 0 || (cs & DMA40_CS_ERR) != 0) {
			ERROR("DMA copy of %" B_PRIu32 " rectangles failed on channel %"
				B_PRIu32 " (cs %#" B_PRIx32 ", %" B_PRIdBIGTIME " us): the CPU "
				"copies from now on\n", count, kDmaEngines[&channel
					- info->dmaChannels][k], cs, system_time() - start);
			failed = true;
		}
	}
	bigtime_t elapsed = system_time() - start;
	if (failed) {
		for (uint32 k = 0; k < DMA_ENGINES; k++)
			dma_reset(channel.engines[k]);
		channel.failed = true;
		return B_ERROR;
	}

	channel.copies++;
	channel.bytes += bytes;
	channel.busy += elapsed;
	if (start - channel.reported > 60000000) {
		if (channel.reported != 0 && channel.busy > 0) {
			INFO("DMA %s: %" B_PRIu32 " copies, %" B_PRIu64 " MB in %"
				B_PRIdBIGTIME " ms (%" B_PRIu64 " MB/s)\n",
				&channel == &info->dmaChannels[DMA_FILL] ? "fill" : "screen",
				channel.copies, channel.bytes >> 20, channel.busy / 1000,
				channel.bytes / channel.busy);
		}
		channel.copies = 0;
		channel.bytes = 0;
		channel.busy = 0;
		channel.reported = start;
	}
	return B_OK;
}


static status_t
copy_rects(display_info* info, const rpi_display_copy& request,
	const uint16* rects)
{
	if (info->dmaChannels == NULL)
		return B_NOT_SUPPORTED;
	return dma_copy(info, info->dmaChannels[request.to
		== RPI_DISPLAY_FRAME_BUFFER_ID ? DMA_SCREEN : DMA_FILL], request,
		rects);
}


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
	info->hvsArea = -1;

	info->bcm2712 = is_bcm2712(info->node);
	if (info->bcm2712) {
		// the outputs' planes come from the compositor
		map_compositor(info);
		if (info->hvsArea < 0) {
			delete_area(info->sharedArea);
			return B_ERROR;
		}
		hvs6_upload_kernel(info);
		if (bcm2712_map_mode_registers(info) != B_OK) {
			// the firmware's modes only
			ERROR("cannot map the HDMI registers: no mode setting\n");
			bcm2712_unmap_mode_registers(info);
			info->hdRegs = NULL;
			for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
				info->hdmiRegs[i] = info->pvRegs[i] = NULL;
				info->ddcRegs[i] = NULL;
			}
		}
	}

	status_t status = read_outputs(info);
	if (status != B_OK) {
		ERROR("no display from the firmware: %s\n", strerror(status));
		if (info->hvsArea >= 0)
			delete_area(info->hvsArea);
		delete_area(info->sharedArea);
		return status;
	}

	// Room for the outputs side by side, taken while contiguous memory is
	// still easy to find.
	const rpi_display_shared_info& shared = *info->shared;
	uint32 width = 0, height = 0;
	uint32 widest = 0;
	for (uint32 i = 0; i < shared.output_count; i++) {
		width += shared.outputs[i].native_width;
		if (shared.outputs[i].native_width > widest)
			widest = shared.outputs[i].native_width;
		if (shared.outputs[i].native_height > height)
			height = shared.outputs[i].native_height;
	}
	// The BCM2712 scales: drawn at the densest output's scale, each region
	// can be as large as the largest panel.
	if (info->bcm2712)
		width = widest * shared.output_count;
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
	info->cursorCanvas = true;
	// The VC5 HDMI_HOTPLUG register, bit 0 = connected: offset 0x1a8 of
	// each HDMI block of the BCM2711, 0x1c8 of the BCM2712's (Linux's
	// bcm2711.dtsi and bcm2712.dtsi, vc4_hdmi_regs.h). These read-only maps
	// do not touch clocks, PHYs or interrupt state; those remain owned by
	// the VideoCore firmware.
	for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++) {
		void* registers;
		phys_addr_t page = info->bcm2712 ? 0x107c701000ull + i * 0x5000
			: 0xfef00000 + i * 0x5000;
		uint32 offset = info->bcm2712 ? 0x5c8 : 0x8a8;
		info->hpdAreas[i] = map_physical_memory("rpi HDMI hotplug", page,
			B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA, &registers);
		if (info->hpdAreas[i] >= 0)
			info->hpdRegisters[i] = (volatile uint32*)((uint8*)registers + offset);
		else
			ERROR("cannot map HDMI%" B_PRIu32 " hotplug register\n", i);
	}
	info->pollThread = spawn_kernel_thread(poll_outputs, "rpi HDMI hotplug",
		B_NORMAL_PRIORITY, info);
	if (info->pollThread >= 0)
		resume_thread(info->pollThread);
	else
		ERROR("cannot start HDMI hotplug thread\n");

	if (!info->bcm2712)
		map_compositor(info);
	if (info->hvsArea >= 0)
		info->retrace = new(std::nothrow) RetraceSemaphores;
	init_dma(info);
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
	if (info->bcm2712) {
		for (uint32 i = 0; i < RPI_DISPLAY_MAX_OUTPUTS; i++)
			hvs6_restore(info, i);
	}
	if (info->hvsArea >= 0)
		delete_area(info->hvsArea);
	if (info->bcm2712)
		bcm2712_unmap_mode_registers(info);
	uninit_dma(info);
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
// through a global (sInfo, above), there is only one.


static status_t
display_free(void* cookie)
{
	display_info* info = sInfo;
	// its sources, and the layout's frame buffer, go: no copy may be under
	// way into them
	lock_dma(info);
	MutexLocker locker(info->lock);
	if (info->notificationOwner == cookie) {
		info->notificationOwner = NULL;
		info->changePort = -1;
	}
	if (info->owner == cookie) {
		release_layout(info);
		info->owner = NULL;
	}
	if (info->sources != NULL) {
		for (uint32 i = 0; i < MAX_SOURCES; i++) {
			display_source& source = info->sources[i];
			if (source.id != 0 && source.owner == cookie) {
				// its team is going or gone: its mapping goes with it
				source.clone = -1;
				delete_source(source);
			}
		}
	}
	atomic_add(&info->openCount, -1);
	locker.Unlock();
	unlock_dma(info);
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
			// Mesa's V3D present asks with the size the info had before
			// flags were added (version 2): it gets that part.
			if (length != sizeof(rpi_display_shared_info)
				&& length != offsetof(rpi_display_shared_info, flags))
				return B_BAD_VALUE;
			MutexLocker locker(info->lock);
			return user_memcpy(buffer, info->shared, length);
		}

		case RPI_DISPLAY_CREATE_SOURCE:
		{
			rpi_display_source request;
			if (length != sizeof(request)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK)
				return B_BAD_VALUE;
			MutexLocker locker(info->lock);
			status_t status = create_source(info, cookie, request);
			if (status != B_OK)
				return status;
			if (user_memcpy(buffer, &request, sizeof(request)) != B_OK) {
				delete_source(*find_source(info, request.id));
				return B_BAD_ADDRESS;
			}
			return B_OK;
		}

		case RPI_DISPLAY_DELETE_SOURCE:
		{
			uint32 id;
			if (length != sizeof(id)
				|| user_memcpy(&id, buffer, sizeof(id)) != B_OK)
				return B_BAD_VALUE;
			// not while a copy reads or writes it
			lock_dma(info);
			MutexLocker locker(info->lock);
			display_source* source = find_source(info, id);
			status_t status = B_BAD_VALUE;
			if (source != NULL && source->owner == cookie) {
				delete_source(*source);
				status = B_OK;
			}
			locker.Unlock();
			unlock_dma(info);
			return status;
		}

		case RPI_DISPLAY_COPY:
		{
			rpi_display_copy request;
			if (length != sizeof(request)
				|| user_memcpy(&request, buffer, sizeof(request)) != B_OK
				|| request.count > RPI_DISPLAY_MAX_COPY_RECTS)
				return B_BAD_VALUE;
			uint16 rects[RPI_DISPLAY_MAX_COPY_RECTS * 4];
			if (user_memcpy(rects, request.rects,
					request.count * 4 * sizeof(uint16)) != B_OK)
				return B_BAD_ADDRESS;
			return copy_rects(info, request, rects);
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

			// the frame buffer may be replaced: not while a copy writes it
			lock_dma(info);
			MutexLocker locker(info->lock);
			if (info->owner != NULL && info->owner != cookie)
				status = B_BUSY;
			else
				status = set_layout(info, layout);
			if (status == B_OK)
				info->owner = cookie;
			locker.Unlock();
			unlock_dma(info);
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
			info->cursorCanvas = config.canvas != 0;
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
			false) != B_OK || (strcmp(compatible, "brcm,bcm2711-hvs") != 0
				&& strcmp(compatible, "brcm,bcm2712-hvs") != 0)) {
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
