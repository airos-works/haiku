/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Sound output of the Raspberry Pi 5: the audio of its two HDMI outputs.

	The Pi 5 has no headphone jack and its firmware no sound service, so
	this driver works the BCM2712's HDMI controllers itself, on top of the
	video mode the firmware set (Linux: vc4_hdmi.c, bcm2835-dma.c):

	- the controller's MAI (multichannel audio interface) takes IEC 60958
	  subframes, one 32 bit word per channel and sample, from a FIFO that a
	  DMA channel of the SoC's 40 bit DMA controller keeps filled;
	- the controller sends them as audio sample packets, with the N and CTS
	  values from which the receiver regenerates the sample clock from the
	  pixel clock, and an audio InfoFrame;
	- the DMA channel runs a ring of control blocks, one per period, and
	  interrupts at the end of each one. A real-time thread converts the
	  Media Kit's 16 bit samples into subframes, two periods ahead.

	There is no volume in the hardware: the volume control scales the
	samples. */


#include <ByteOrder.h>
#include <Drivers.h>
#include <KernelExport.h>

#include <stdio.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <kernel.h>
#include <lock.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <hmulti_audio.h>
#include <rpi_firmware.h>


#define ERROR(x...)	dprintf("bcm2712_hdmi_audio: " x)
#define INFO(x...)	dprintf("bcm2712_hdmi_audio: " x)

#define DEVICE_NAME			"audio/hmulti/bcm2712_hdmi/0"

#define CHANNELS			2
#define PERIOD_FRAMES		960
	// 20 ms at 48 kHz
#define RING_PERIODS		4
#define LEAD_PERIODS		2
	// periods converted ahead of the one the DMA reads
#define USER_BUFFERS		2
#define USER_FRAME_SIZE		(CHANNELS * 2)
#define USER_BUFFER_BYTES	(PERIOD_FRAMES * USER_FRAME_SIZE)
#define PERIOD_BYTES		(PERIOD_FRAMES * CHANNELS * 4)
#define RING_OFFSET			B_PAGE_SIZE
	// the control blocks take the first page of the DMA area
#define DMA_AREA_SIZE		(RING_OFFSET + RING_PERIODS * PERIOD_BYTES)

#define HDMI_PORTS			2
#define FIRMWARE_GET_CLOCK_MEASURED	0x00030047
#define DMA_CHANNEL			7
	// one of the channels the device tree leaves to the OS (7 to 11)

#define MIN_GAIN			-60.0f
#define MAX_GAIN			0.0f


// The HDMI controller's registers (vc4_hdmi_regs.h, vc6_hdmi_hdmi*_fields):
// "hdmi" block
#define HDMI_MAI_CHANNEL_MAP		0x0a4
#define HDMI_MAI_CONFIG				0x0a8
#define HDMI_AUDIO_PACKET_CONFIG	0x0c0
#define HDMI_RAM_PACKET_CONFIG		0x0c4
#define HDMI_RAM_PACKET_STATUS		0x0cc
#define HDMI_CRP_CFG				0x0d0
#define HDMI_CTS_0					0x0d4
#define HDMI_CTS_1					0x0d8
#define HDMI_SCHEDULER_CONTROL		0x0e8
#define HDMI_HORZA					0x0ec
#define HDMI_HORZB					0x0f0
#define HDMI_VERTA0					0x0f4
#define HDMI_VERTB0					0x0f8
#define HDMI_HOTPLUG				0x1c8
// "hd" block, shared by both controllers: MAI_CTL is 0x10 or 0x30
#define HD_MAI_CTL					0x00
#define HD_MAI_THR					0x04
#define HD_MAI_FMT					0x08
#define HD_MAI_DATA					0x0c
#define HD_MAI_SMP					0x10
#define HD_FRAME_COUNT				0x60	// HDMI 0 only

#define MAI_CTL_RESET				(1u << 0)
#define MAI_CTL_ERRORF				(1u << 1)
#define MAI_CTL_ERRORE				(1u << 2)
#define MAI_CTL_ENABLE				(1u << 3)
#define MAI_CTL_CHNUM(n)			((uint32)(n) << 4)
#define MAI_CTL_FLUSH				(1u << 9)
#define MAI_CTL_EMPTY				(1u << 10)
#define MAI_CTL_FULL				(1u << 11)
#define MAI_CTL_WHOLSMP				(1u << 12)
#define MAI_CTL_CHALIGN				(1u << 13)
#define MAI_CTL_DLATE				(1u << 15)

#define MAI_CONFIG_BIT_REVERSE		(1u << 26)
#define MAI_CONFIG_FORMAT_REVERSE	(1u << 27)
#define MAI_FORMAT_PCM				2

#define AUDIO_PACKET_ZERO_ON_FLAT	(1u << 29)
#define AUDIO_PACKET_ZERO_ON_INACTIVE (1u << 24)
#define AUDIO_PACKET_B_FRAME_ID(x)	((uint32)(x) << 10)

#define RAM_PACKET_ENABLE			(1u << 16)
#define CRP_CFG_EXTERNAL_CTS_EN		(1u << 24)
#define SCHEDULER_MODE_HDMI			(1u << 0)
#define HOTPLUG_CONNECTED			(1u << 0)

#define PACKET_STRIDE				0x24
#define AUDIO_INFOFRAME_PACKET		4		// InfoFrame type 0x84 - 0x80

// the DVP's clock gates (clk-bcm2711-dvp.c): a set bit stops the clock
#define DVP_MISC_CONFIG				0x08
#define DVP_AUDIO_CLOCK_RATE		108000000
	// the gate's parent, a fixed 108 MHz clock

// The 40 bit DMA channels (bcm2835-dma.c, BCM2711_DMA40_*)
#define DMA40_CS					0x00
#define DMA40_CB					0x04
#define DMA40_DEBUG					0x0c
#define DMA40_TI					0x10
#define DMA40_SRC					0x14
#define DMA40_DEST					0x1c
#define DMA40_LEN					0x24
#define DMA40_NEXT_CB				0x28

#define DMA40_CS_ACTIVE				(1u << 0)
#define DMA40_CS_END				(1u << 1)
#define DMA40_CS_INT				(1u << 2)
#define DMA40_CS_PROT				(3u << 8)
#define DMA40_CS_ERR				(1u << 10)
#define DMA40_CS_FLAGS_MASK			((0xffu << 16) | (3u << 28))
	// QoS, panic QoS, wait for writes, disable debug
#define DMA40_CS_ABORT				(1u << 30)
#define DMA40_CS_HALT				(1u << 31)
#define DMA40_DEBUG_RESET			(1u << 23)

#define DMA40_TI_INTEN				(1u << 0)
#define DMA40_TI_WAIT_RESP			(1u << 2)
#define DMA40_TI_PER_MAP(x)			(((uint32)(x) & 31) << 9)
#define DMA40_TI_D_DREQ				(1u << 15)
#define DMA40_BURST_LEN(x)			(((uint32)(x) & 15) << 8)
#define DMA40_INC					(1u << 12)
#define DMA40_SIZE_128				(2u << 13)

// the "fake" bits of the device tree's DMA cell (bcm2835-dma.c)
#define DREQ_WIDE_SOURCE			(1u << 24)
#define DREQ_NO_WAIT_RESP			(1u << 27)
#define DREQ_BURST					(1u << 30)

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


// mixer controls
enum {
	CONTROL_GROUP = 1024,
	CONTROL_VOLUME_LEFT,
	CONTROL_VOLUME_RIGHT,
	CONTROL_DESTINATION,
	CONTROL_DESTINATION_FIRST
};

static const char* const kDestinations[HDMI_PORTS] = { "HDMI 0", "HDMI 1" };


struct hdmi_port {
	bool			present;
	area_id			hdmiArea;
	volatile uint8*	hdmi;
	area_id			packetArea;
	volatile uint8*	packet;
	uint32			maiOffset;		// of MAI_CTL in the "hd" block
	uint32			dreq;			// the device tree's DMA cell
};

struct audio_device {
	mutex			lock;
	int32			openCount;

	hdmi_port		ports[HDMI_PORTS];
	area_id			hdArea;
	volatile uint8*	hd;
	uint64			hdPhysical;
	area_id			dvpArea;
	volatile uint8*	dvp;
	area_id			dmaRegistersArea;
	volatile uint8*	dma;
	uint32			dmaInterrupt;
	bool			d0;

	area_id			dmaArea;
	uint8*			dmaBuffer;
	phys_addr_t		dmaPhysical;

	area_id			bufferArea;
	uint8*			buffers[USER_BUFFERS];

	sem_id			periodSem;
	sem_id			readySem;
	thread_id		thread;
	bool			running;
	bool			interruptInstalled;
	int32			periodsDone;		// by the DMA, counted in its interrupt
	int32			dmaErrors;

	spinlock		timeLock;
	int32			playingBuffer;
	bigtime_t		realTime;
	uint64			framesCount;
	bigtime_t		periodTime;			// of the last DMA interrupt

	uint32			rate;
	float			gain;
	uint32			gainFactor;			// Q16
	uint32			destination;
	uint32			activePort;
	uint8			channelStatus[24];
};


int32 api_version = B_CUR_DRIVER_API_VERSION;

static device_manager_info* sDeviceManager;
static audio_device sDevice;
static const char* sDeviceNames[] = { DEVICE_NAME, NULL };


static inline uint32
read32(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
write32(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
}


//	#pragma mark - the device tree


struct fdt_node_info {
	fdt_device_module_info*	module;
	fdt_device*				device;
	device_node*			node;
};


/*!	Finds the device tree node whose "compatible" is \a compatible and,
	if \a address is not 0, whose first register range starts there. The
	caller puts \a info.node. */
static bool
find_fdt_node(const char* compatible, uint64 address, fdt_node_info& info)
{
	device_attr attributes[] = {
		{ "fdt/compatible", B_STRING_TYPE, { .string = compatible } },
		{}
	};
	device_node* root = sDeviceManager->get_root_node();
	device_node* node = NULL;
	bool found = false;
	while (!found
		&& sDeviceManager->find_child_node(root, attributes, &node) == B_OK) {
		uint64 base, size;
		if (sDeviceManager->get_driver(node, (driver_module_info**)&info.module,
				(void**)&info.device) != B_OK
			|| !info.module->get_reg(info.device, 0, &base, &size)
			|| (address != 0 && base != address)) {
			continue;
		}
		found = true;
	}
	sDeviceManager->put_node(root);
	info.node = found ? node : NULL;
	return found;
}


static status_t
map_registers(const char* name, uint64 physical, size_t size, area_id& area,
	volatile uint8*& registers)
{
	uint64 base = ROUNDDOWN(physical, B_PAGE_SIZE);
	size_t mapSize = ROUNDUP(physical + size, B_PAGE_SIZE) - base;
	void* address;
	area = map_physical_memory(name, base, mapSize,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < 0)
		return area;
	registers = (volatile uint8*)address + (physical - base);
	return B_OK;
}


static status_t
find_port(audio_device* device, uint32 index)
{
	hdmi_port& port = device->ports[index];
	char compatible[32];
	snprintf(compatible, sizeof(compatible), "brcm,bcm2712-hdmi%" B_PRIu32,
		index);

	fdt_node_info info;
	if (!find_fdt_node(compatible, 0, info))
		return B_ENTRY_NOT_FOUND;

	// reg-names: hdmi, dvp, phy, rm, packet, metadata, csc, cec, hd
	uint64 hdmi, hdmiSize, packet, packetSize, hd, hdSize;
	bool valid = info.module->get_reg(info.device, 0, &hdmi, &hdmiSize)
		&& info.module->get_reg(info.device, 4, &packet, &packetSize)
		&& info.module->get_reg(info.device, 8, &hd, &hdSize);

	// dmas = <&dma40 cell>: the cell carries the DREQ and the channel flags
	int length;
	const uint32* dmas = (const uint32*)info.module->get_prop(info.device,
		"dmas", &length);
	if (dmas != NULL && length >= 8)
		port.dreq = B_BENDIAN_TO_HOST_INT32(dmas[1]);
	sDeviceManager->put_node(info.node);
	if (!valid || port.dreq == 0)
		return B_BAD_DATA;

	status_t status = map_registers("bcm2712 hdmi", hdmi, 0x300,
		port.hdmiArea, port.hdmi);
	if (status == B_OK) {
		status = map_registers("bcm2712 hdmi packets", packet, 0x200,
			port.packetArea, port.packet);
	}
	if (status != B_OK)
		return status;

	// the "hd" block is the same for both
	if (device->hd == NULL) {
		device->hdPhysical = hd;
		status = map_registers("bcm2712 hdmi hd", hd, 0x100, device->hdArea,
			device->hd);
		if (status != B_OK)
			return status;
	}
	port.maiOffset = index == 0 ? 0x10 : 0x30;
	port.present = true;
	return B_OK;
}


static status_t
find_dvp(audio_device* device)
{
	// the clock controller with the HDMI audio clocks' gates
	fdt_node_info info;
	if (!find_fdt_node("brcm,brcm2711-dvp", 0, info))
		return B_ENTRY_NOT_FOUND;
	uint64 base, size;
	bool valid = info.module->get_reg(info.device, 0, &base, &size);
	sDeviceManager->put_node(info.node);
	if (!valid)
		return B_BAD_DATA;
	return map_registers("bcm2712 dvp", base, 0x10, device->dvpArea,
		device->dvp);
}


static status_t
find_dma(audio_device* device)
{
	fdt_node_info info;
	if (!find_fdt_node("brcm,bcm2712-dma", 0x1000010600ull, info))
		return B_ENTRY_NOT_FOUND;

	// "dma6" to "dma11"
	uint64 interrupt = 0;
	bool valid = info.module->get_interrupt(info.device, DMA_CHANNEL - 6,
		NULL, &interrupt);
	sDeviceManager->put_node(info.node);
	if (!valid || interrupt == 0)
		return B_BAD_DATA;
	device->dmaInterrupt = (uint32)interrupt;

	// the channel's registers, 0x100 bytes per channel from channel 0 on
	return map_registers("bcm2712 dma40",
		0x1000010000ull + DMA_CHANNEL * 0x100, 0x100,
		device->dmaRegistersArea, device->dma);
}


static void
unmap_hardware(audio_device* device)
{
	for (uint32 i = 0; i < HDMI_PORTS; i++) {
		hdmi_port& port = device->ports[i];
		if (port.hdmiArea >= 0)
			delete_area(port.hdmiArea);
		if (port.packetArea >= 0)
			delete_area(port.packetArea);
		port.hdmiArea = port.packetArea = -1;
		port.present = false;
	}
	area_id* areas[] = { &device->hdArea, &device->dvpArea,
		&device->dmaRegistersArea, &device->dmaArea };
	for (size_t i = 0; i < B_COUNT_OF(areas); i++) {
		if (*areas[i] >= 0)
			delete_area(*areas[i]);
		*areas[i] = -1;
	}
}


//	#pragma mark - the HDMI controller


static inline uint32
hdmi_read(audio_device* device, uint32 offset)
{
	return read32(device->ports[device->activePort].hdmi, offset);
}


static inline void
hdmi_write(audio_device* device, uint32 offset, uint32 value)
{
	write32(device->ports[device->activePort].hdmi, offset, value);
}


static inline uint32
mai_read(audio_device* device, uint32 offset)
{
	return read32(device->hd,
		device->ports[device->activePort].maiOffset + offset);
}


static inline void
mai_write(audio_device* device, uint32 offset, uint32 value)
{
	write32(device->hd, device->ports[device->activePort].maiOffset + offset,
		value);
}


static bool
port_connected(audio_device* device, uint32 index)
{
	hdmi_port& port = device->ports[index];
	return port.present
		&& (read32(port.hdmi, HDMI_HOTPLUG) & HOTPLUG_CONNECTED) != 0
		&& (read32(port.hdmi, HDMI_SCHEDULER_CONTROL) & SCHEDULER_MODE_HDMI)
			!= 0;
}


/*!	The pixel clock of the mode the firmware set, in Hz. The firmware
	measures the clock of HDMI 0 for us. Otherwise it is estimated from the
	timings in the controller's registers (in which the firmware leaves the
	vertical front porch 0) and the frame rate measured with the frame
	counter (HDMI 0 only), rounded to a whole or a 1000/1001 rate. */
static uint64
pixel_clock(audio_device* device)
{
	if (device->activePort == 0) {
		rpi_firmware_module_info* firmware;
		if (get_module(RPI_FIRMWARE_MODULE_NAME, (module_info**)&firmware)
				== B_OK) {
			uint32 data[2] = { RPI_FIRMWARE_CLOCK_PIXEL, 0 };
			status_t status = firmware->property(FIRMWARE_GET_CLOCK_MEASURED,
				data, sizeof(data));
			put_module(RPI_FIRMWARE_MODULE_NAME);
			if (status == B_OK && data[1] > 20000000) {
				// to the kHz, as the modes have it
				uint64 clock = (data[1] + 500) / 1000 * 1000;
				INFO("HDMI 0: pixel clock %" B_PRIu32 " Hz measured by the"
					" firmware\n", data[1]);
				return clock;
			}
		}
	}

	uint32 horza = hdmi_read(device, HDMI_HORZA);
	uint32 horzb = hdmi_read(device, HDMI_HORZB);
	uint32 verta = hdmi_read(device, HDMI_VERTA0);
	uint32 vertb = hdmi_read(device, HDMI_VERTB0);
	uint32 htotal = (horza & 0x3fff) + ((horza >> 16) & 0x1fff)
		+ (horzb & 0x7ff) + ((horzb >> 16) & 0x7ff);
	uint32 vtotal = (verta & 0x1fff) + ((verta >> 16) & 0x7f)
		+ ((verta >> 24) & 0x1f) + (vertb & 0x1ff);

	// 1000 times the frame rate
	uint64 milliHertz = 60000;
	if (device->activePort == 0) {
		// time 30 frames, from one frame change to another
		uint32 count = read32(device->hd, HD_FRAME_COUNT);
		bigtime_t start = 0, end = 0;
		bigtime_t timeout = system_time() + 1000000;
		int32 frames = -1;
		while (frames < 30 && system_time() < timeout) {
			uint32 now = read32(device->hd, HD_FRAME_COUNT);
			if (now == count) {
				spin(50);
				continue;
			}
			count = now;
			if (++frames == 0)
				start = system_time();
			else
				end = system_time();
		}
		if (frames == 30 && end > start) {
			uint64 measured = 30ull * 1000000000ull / (end - start);
			uint64 whole = (measured + 500) / 1000 * 1000;
			uint64 ntsc = whole * 1000 / 1001;
			milliHertz = (measured > ntsc ? measured - ntsc : ntsc - measured)
				< (measured > whole ? measured - whole : whole - measured)
				? ntsc : whole;
			INFO("frame rate %" B_PRIu64 ".%03" B_PRIu64 " Hz measured\n",
				measured / 1000, measured % 1000);
		} else
			ERROR("the frame counter does not count; assuming 60 Hz\n");
	}

	uint64 clock = (uint64)htotal * vtotal * milliHertz / 1000;
	INFO("HDMI %" B_PRIu32 ": %" B_PRIu32 " x %" B_PRIu32 " total at %"
		B_PRIu64 ".%03" B_PRIu64 " Hz: pixel clock %" B_PRIu64 " Hz\n",
		device->activePort, htotal, vtotal, milliHertz / 1000,
		milliHertz % 1000, clock);
	return clock;
}


static uint32
sample_rate_code(uint32 rate)
{
	return rate == 44100 ? 8 : 9;
}


static status_t
write_audio_infoframe(audio_device* device)
{
	// type 0x84, version 1, length 10; PB1: 2 channels, coding and size
	// from the stream; the rest 0 (front left and right)
	uint8 frame[14] = { 0x84, 0x01, 0x0a, 0, 0x01 };
	uint8 sum = 0;
	for (size_t i = 0; i < sizeof(frame); i++)
		sum += frame[i];
	frame[3] = (uint8)(0x100 - sum);

	hdmi_port& port = device->ports[device->activePort];
	uint32 bit = 1u << AUDIO_INFOFRAME_PACKET;
	uint32 config = hdmi_read(device, HDMI_RAM_PACKET_CONFIG);
	if ((config & RAM_PACKET_ENABLE) == 0) {
		config |= RAM_PACKET_ENABLE;
		hdmi_write(device, HDMI_RAM_PACKET_CONFIG, config);
	}
	hdmi_write(device, HDMI_RAM_PACKET_CONFIG, config & ~bit);
	bigtime_t timeout = system_time() + 100000;
	while ((hdmi_read(device, HDMI_RAM_PACKET_STATUS) & bit) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(100);
	}

	// seven bytes in two words: 3 in the first, 4 in the second
	uint32 offset = PACKET_STRIDE * AUDIO_INFOFRAME_PACKET;
	uint32 end = offset + PACKET_STRIDE;
	for (size_t i = 0; i < sizeof(frame); i += 7) {
		write32(port.packet, offset,
			frame[i] | frame[i + 1] << 8 | frame[i + 2] << 16);
		offset += 4;
		write32(port.packet, offset, frame[i + 3] | frame[i + 4] << 8
			| frame[i + 5] << 16 | (uint32)frame[i + 6] << 24);
		offset += 4;
	}
	for (; offset < end; offset += 4)
		write32(port.packet, offset, 0);

	hdmi_write(device, HDMI_RAM_PACKET_CONFIG,
		hdmi_read(device, HDMI_RAM_PACKET_CONFIG) | bit);
	timeout = system_time() + 100000;
	while ((hdmi_read(device, HDMI_RAM_PACKET_STATUS) & bit) == 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(100);
	}
	return B_OK;
}


static void
stop_audio_infoframe(audio_device* device)
{
	hdmi_write(device, HDMI_RAM_PACKET_CONFIG,
		hdmi_read(device, HDMI_RAM_PACKET_CONFIG)
			& ~(1u << AUDIO_INFOFRAME_PACKET));
}


static uint32
gcd(uint32 a, uint32 b)
{
	while (b != 0) {
		uint32 t = a % b;
		a = b;
		b = t;
	}
	return a;
}


/*!	Sets the MAI and the packets up for 16 bit stereo PCM at
	\a device->rate (vc4_hdmi_audio_startup() and _prepare()). */
static status_t
start_hdmi(audio_device* device)
{
	uint32 rate = device->rate;

	// the audio clock gate of this port
	uint32 gate = 1u << (3 + device->activePort);
	uint32 misc = read32(device->dvp, DVP_MISC_CONFIG);
	if ((misc & gate) != 0)
		write32(device->dvp, DVP_MISC_CONFIG, misc & ~gate);

	mai_write(device, HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH
		| MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF);

	// the sample clock: 108 MHz * m / n
	uint32 divisor = gcd(DVP_AUDIO_CLOCK_RATE, rate);
	uint32 n = DVP_AUDIO_CLOCK_RATE / divisor;
	uint32 m = rate / divisor;
	if (m > 256 || n > 0xffffff)
		return B_BAD_VALUE;
	mai_write(device, HD_MAI_SMP, n << 8 | (m - 1));

	mai_write(device, HD_MAI_CTL, MAI_CTL_CHNUM(CHANNELS) | MAI_CTL_WHOLSMP
		| MAI_CTL_CHALIGN | MAI_CTL_ENABLE);
	mai_write(device, HD_MAI_FMT, sample_rate_code(rate) << 8
		| MAI_FORMAT_PCM << 16);

	// DREQ and panic thresholds; the D0 stepping moved the fields
	if (device->d0) {
		mai_write(device, HD_MAI_THR, 0x10u << 23 | 0x10u << 15 | 0x1cu << 7
			| 0x1cu);
	} else {
		mai_write(device, HD_MAI_THR, 0x10u << 24 | 0x10u << 16 | 0x1cu << 8
			| 0x1cu);
	}

	uint32 channelMask = (1u << CHANNELS) - 1;
	hdmi_write(device, HDMI_MAI_CONFIG, MAI_CONFIG_BIT_REVERSE
		| MAI_CONFIG_FORMAT_REVERSE | channelMask);
	uint32 channelMap = 0;
	for (uint32 i = 0; i < CHANNELS; i++)
		channelMap |= i << (4 * i);
	hdmi_write(device, HDMI_MAI_CHANNEL_MAP, channelMap);
	// block start: preamble 8, as written into the subframes
	hdmi_write(device, HDMI_AUDIO_PACKET_CONFIG, AUDIO_PACKET_ZERO_ON_FLAT
		| AUDIO_PACKET_ZERO_ON_INACTIVE | AUDIO_PACKET_B_FRAME_ID(8)
		| channelMask);

	// audio clock regeneration: the HDMI specification's recommended N,
	// CTS = f_pixel N / 128 fs
	uint32 audioN = rate == 44100 ? 6272 : 128 * rate / 1000;
	uint64 cts = (pixel_clock(device) * audioN + 64ull * rate)
		/ (128ull * rate);
	hdmi_write(device, HDMI_CRP_CFG, CRP_CFG_EXTERNAL_CTS_EN | audioN);
	hdmi_write(device, HDMI_CTS_0, (uint32)cts);
	hdmi_write(device, HDMI_CTS_1, (uint32)cts);
	INFO("HDMI %" B_PRIu32 ": %" B_PRIu32 " Hz, N %" B_PRIu32 ", CTS %"
		B_PRIu64 ", MAI n %" B_PRIu32 " m %" B_PRIu32 "\n",
		device->activePort, rate, audioN, cts, n, m);

	status_t status = write_audio_infoframe(device);
	if (status != B_OK)
		ERROR("the audio InfoFrame: %s\n", strerror(status));
	return B_OK;
}


static void
stop_hdmi(audio_device* device)
{
	mai_write(device, HD_MAI_CTL, MAI_CTL_DLATE | MAI_CTL_ERRORE
		| MAI_CTL_ERRORF);
	stop_audio_infoframe(device);
	mai_write(device, HD_MAI_CTL, MAI_CTL_RESET);
	mai_write(device, HD_MAI_CTL, MAI_CTL_ERRORF);
	mai_write(device, HD_MAI_CTL, MAI_CTL_FLUSH);
}


//	#pragma mark - DMA


static int32
dma_interrupt(void* data)
{
	audio_device* device = (audio_device*)data;
	uint32 cs = read32(device->dma, DMA40_CS);
	if ((cs & DMA40_CS_INT) == 0)
		return B_UNHANDLED_INTERRUPT;

	// acknowledge, keeping the channel running
	uint32 flags = device->ports[device->activePort].dreq
		& DMA40_CS_FLAGS_MASK;
	write32(device->dma, DMA40_CS, DMA40_CS_INT | DMA40_CS_ACTIVE
		| DMA40_CS_PROT | flags);
	if ((cs & DMA40_CS_ERR) != 0)
		atomic_add(&device->dmaErrors, 1);

	acquire_spinlock(&device->timeLock);
	atomic_add(&device->periodsDone, 1);
	device->periodTime = system_time();
	release_spinlock(&device->timeLock);

	release_sem_etc(device->periodSem, 1, B_DO_NOT_RESCHEDULE);
	return B_INVOKE_SCHEDULER;
}


static void
reset_dma(audio_device* device)
{
	write32(device->dma, DMA40_CS, DMA40_CS_HALT);
	spin(10);
	write32(device->dma, DMA40_DEBUG, DMA40_DEBUG_RESET);
	spin(10);
	write32(device->dma, DMA40_CS, DMA40_CS_INT | DMA40_CS_END);
}


static void
start_dma(audio_device* device)
{
	hdmi_port& port = device->ports[device->activePort];
	uint32 dreq = port.dreq;
	uint64 destination = device->hdPhysical + port.maiOffset + HD_MAI_DATA;

	uint32 ti = DMA40_TI_D_DREQ | DMA40_TI_PER_MAP(dreq & 31) | DMA40_TI_INTEN;
	if ((dreq & DREQ_NO_WAIT_RESP) == 0)
		ti |= DMA40_TI_WAIT_RESP;
	uint32 burst = (dreq & DREQ_BURST) != 0 ? DMA40_BURST_LEN(3) : 0;
	uint32 srcInfo = DMA40_INC | burst
		| ((dreq & DREQ_WIDE_SOURCE) != 0 ? DMA40_SIZE_128 : 0);

	dma40_cb* blocks = (dma40_cb*)device->dmaBuffer;
	for (uint32 i = 0; i < RING_PERIODS; i++) {
		uint64 source = device->dmaPhysical + RING_OFFSET + i * PERIOD_BYTES;
		uint64 next = device->dmaPhysical
			+ ((i + 1) % RING_PERIODS) * sizeof(dma40_cb);
		blocks[i].ti = ti;
		blocks[i].src = (uint32)source;
		blocks[i].srci = (uint32)(source >> 32) | srcInfo;
		blocks[i].dst = (uint32)destination;
		blocks[i].dsti = (uint32)(destination >> 32) | burst;
		blocks[i].len = PERIOD_BYTES;
		blocks[i].next_cb = (uint32)(next >> 5);
		blocks[i].reserved = 0;
	}
	memory_full_barrier();

	reset_dma(device);
	write32(device->dma, DMA40_CB, (uint32)(device->dmaPhysical >> 5));
	write32(device->dma, DMA40_CS, DMA40_CS_ACTIVE | DMA40_CS_PROT
		| (dreq & DMA40_CS_FLAGS_MASK));
}


//	#pragma mark - playback


static void
make_channel_status(audio_device* device)
{
	// IEC 60958 consumer format: PCM, no copyright, original; sample rate;
	// 16 bit words
	memset(device->channelStatus, 0, sizeof(device->channelStatus));
	device->channelStatus[0] = 0x04;
	device->channelStatus[1] = 0x82;
	device->channelStatus[3] = device->rate == 44100 ? 0x00 : 0x02;
	device->channelStatus[4] = 0x02;
}


/*!	One period of 16 bit stereo into IEC 60958 subframes as the MAI takes
	them (alsa-lib's iec958 plug-in): sample in bits 12 to 27, channel
	status in bit 30, even parity of bits 4 to 30 in bit 31, the preamble
	in bits 0 to 3, 8 at the start of each block of 192 frames. */
static void
convert_period(audio_device* device, const int16* samples, uint32* out,
	uint32& counter)
{
	uint32 factor = device->gainFactor;
	for (uint32 frame = 0; frame < PERIOD_FRAMES; frame++) {
		bool status = (device->channelStatus[counter >> 3]
			>> (counter & 7)) & 1;
		for (uint32 channel = 0; channel < CHANNELS; channel++) {
			int32 sample = samples[frame * CHANNELS + channel];
			if (factor != 0x10000)
				sample = (int32)(((int64)sample * factor) >> 16);
			uint32 data = ((uint32)sample & 0xffff) << 12;
			if (status)
				data |= 1u << 30;
			if (__builtin_parity(data))
				data |= 1u << 31;
			if (channel != 0)
				data |= 0x04;
			else
				data |= counter == 0 ? 0x08 : 0x02;
			out[frame * CHANNELS + channel] = data;
		}
		if (++counter == 192)
			counter = 0;
	}
}


static uint32*
ring_period(audio_device* device, uint32 index)
{
	return (uint32*)(device->dmaBuffer + RING_OFFSET + index * PERIOD_BYTES);
}


static int32
playback_thread(void* data)
{
	audio_device* device = (audio_device*)data;
	static const int16 kSilence[PERIOD_FRAMES * CHANNELS] = {};
	uint32 counter = 0;
	uint32 written = 0;
	int32 index = 0;
	int32 waits = 0;

	// silence in the period the DMA starts with
	convert_period(device, kSilence, ring_period(device, 0), counter);
	written = 1;
	start_dma(device);

	while (device->running) {
		int32 done = atomic_get(&device->periodsDone);
		if ((int32)written - done >= LEAD_PERIODS) {
			status_t status = acquire_sem_etc(device->periodSem, 1,
				B_RELATIVE_TIMEOUT, 200000);
			if (status == B_TIMED_OUT && device->running) {
				ERROR("no DMA interrupt for 200 ms: CS %#" B_PRIx32
					", CB %#" B_PRIx32 ", debug %#" B_PRIx32 ", MAI_CTL %#"
					B_PRIx32 "\n", read32(device->dma, DMA40_CS),
					read32(device->dma, DMA40_CB),
					read32(device->dma, DMA40_DEBUG),
					mai_read(device, HD_MAI_CTL));
				if (++waits == 5)
					break;
			}
			continue;
		}
		waits = 0;
		if (done > (int32)written) {
			// the thread fell behind: the DMA played stale periods
			written = done + 1;
		}

		convert_period(device, (const int16*)device->buffers[index],
			ring_period(device, written % RING_PERIODS), counter);
		written++;

		InterruptsSpinLocker locker(device->timeLock);
		device->playingBuffer = index;
		device->realTime = device->periodTime != 0
			? device->periodTime : system_time();
		device->framesCount = (uint64)device->periodsDone * PERIOD_FRAMES;
		locker.Unlock();
		release_sem_etc(device->readySem, 1, B_DO_NOT_RESCHEDULE);

		index = (index + 1) % USER_BUFFERS;
	}

	reset_dma(device);
	return 0;
}


static status_t
start_playback(audio_device* device)
{
	MutexLocker locker(device->lock);
	if (device->running)
		return B_OK;

	// the chosen output, or the first connected one
	uint32 port = device->destination;
	if (!port_connected(device, port)) {
		for (port = 0; port < HDMI_PORTS; port++) {
			if (port_connected(device, port))
				break;
		}
		if (port == HDMI_PORTS) {
			ERROR("no HDMI output with an HDMI sink\n");
			return B_DEV_NOT_READY;
		}
	}
	device->activePort = port;
	make_channel_status(device);

	status_t status = start_hdmi(device);
	if (status != B_OK)
		return status;

	if (!device->interruptInstalled) {
		status = install_io_interrupt_handler(device->dmaInterrupt,
			dma_interrupt, device, 0);
		if (status != B_OK) {
			stop_hdmi(device);
			return status;
		}
		device->interruptInstalled = true;
	}

	while (acquire_sem_etc(device->readySem, 1, B_RELATIVE_TIMEOUT, 0)
			== B_OK) {
	}
	while (acquire_sem_etc(device->periodSem, 1, B_RELATIVE_TIMEOUT, 0)
			== B_OK) {
	}
	atomic_set(&device->periodsDone, 0);
	device->periodTime = 0;
	device->framesCount = 0;
	device->realTime = 0;
	device->running = true;
	device->thread = spawn_kernel_thread(playback_thread, "bcm2712 hdmi audio",
		B_REAL_TIME_PRIORITY, device);
	if (device->thread < 0) {
		device->running = false;
		stop_hdmi(device);
		return device->thread;
	}
	resume_thread(device->thread);
	return B_OK;
}


static void
stop_playback(audio_device* device)
{
	MutexLocker locker(device->lock);
	if (!device->running)
		return;

	device->running = false;
	release_sem(device->periodSem);
	status_t result;
	wait_for_thread(device->thread, &result);

	stop_hdmi(device);
	if (device->interruptInstalled) {
		remove_io_interrupt_handler(device->dmaInterrupt, dma_interrupt,
			device);
		device->interruptInstalled = false;
	}
	if (device->dmaErrors != 0) {
		ERROR("%" B_PRId32 " DMA errors\n", device->dmaErrors);
		device->dmaErrors = 0;
	}
}


static status_t
allocate_dma_buffer(audio_device* device)
{
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	device->dmaArea = create_area_etc(B_SYSTEM_TEAM, "bcm2712 hdmi audio dma",
		ROUNDUP(DMA_AREA_SIZE, B_PAGE_SIZE), B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &virtualRestrictions,
		&physicalRestrictions, (void**)&device->dmaBuffer);
	if (device->dmaArea < 0)
		return device->dmaArea;

	physical_entry entry;
	status_t status = get_memory_map(device->dmaBuffer, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	device->dmaPhysical = entry.address;

	// the DMA reads behind the caches' back: keep the ring out of them
	for (addr_t line = (addr_t)device->dmaBuffer;
			line < (addr_t)device->dmaBuffer + DMA_AREA_SIZE; line += 64) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	memory_full_barrier();
	return vm_set_area_memory_type(device->dmaArea, device->dmaPhysical,
		B_WRITE_COMBINING_MEMORY);
}


static void
free_buffers(audio_device* device)
{
	if (device->bufferArea >= 0) {
		delete_area(device->bufferArea);
		device->bufferArea = -1;
	}
}


//	#pragma mark - multi audio


static const multi_channel_info kChannels[] = {
	{ 0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0 },
	{ 1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0 },
	{ 2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		0 },
	{ 3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		0 }
};


static status_t
get_description(multi_description* userData)
{
	multi_description description;
	if (user_memcpy(&description, userData, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;

	description.interface_version = B_CURRENT_INTERFACE_VERSION;
	description.interface_minimum = B_CURRENT_INTERFACE_VERSION;
	strlcpy(description.friendly_name, "Raspberry Pi 5 HDMI sound",
		sizeof(description.friendly_name));
	strlcpy(description.vendor_info, "Broadcom",
		sizeof(description.vendor_info));
	description.output_channel_count = CHANNELS;
	description.input_channel_count = 0;
	description.output_bus_channel_count = CHANNELS;
	description.input_bus_channel_count = 0;
	description.aux_bus_channel_count = 0;
	description.output_rates = B_SR_44100 | B_SR_48000;
	description.input_rates = 0;
	description.max_cvsr_rate = 0;
	description.min_cvsr_rate = 0;
	description.output_formats = B_FMT_16BIT;
	description.input_formats = 0;
	description.lock_sources = B_MULTI_LOCK_INTERNAL;
	description.timecode_sources = 0;
	description.interface_flags = B_MULTI_INTERFACE_PLAYBACK;
	description.start_latency = 40000;
	description.control_panel[0] = 0;

	if (user_memcpy(userData, &description, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;
	if ((size_t)description.request_channel_count >= B_COUNT_OF(kChannels)
		&& user_memcpy(description.channels, kChannels, sizeof(kChannels))
			!= B_OK) {
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


static status_t
get_enabled_channels(multi_channel_enable* userData)
{
	multi_channel_enable data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;

	uint8 bits = 0x3;
	if (!IS_USER_ADDRESS(data.enable_bits)
		|| user_memcpy(data.enable_bits, &bits, 1) != B_OK) {
		return B_BAD_ADDRESS;
	}
	data.lock_source = B_MULTI_LOCK_INTERNAL;
	return user_memcpy(userData, &data, sizeof(data));
}


static status_t
get_global_format(audio_device* device, multi_format_info* userData)
{
	multi_format_info data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;

	data.output_latency = 0;
	data.input_latency = 0;
	data.timecode_kind = 0;
	data.input.rate = 0;
	data.input.cvsr = 0;
	data.input.format = 0;
	data.output.rate = device->rate == 44100 ? B_SR_44100 : B_SR_48000;
	data.output.cvsr = 0;
	data.output.format = B_FMT_16BIT;
	return user_memcpy(userData, &data, sizeof(data));
}


static status_t
set_global_format(audio_device* device, multi_format_info* userData)
{
	multi_format_info data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;

	uint32 rate;
	if (data.output.rate == B_SR_48000)
		rate = 48000;
	else if (data.output.rate == B_SR_44100)
		rate = 44100;
	else
		return B_BAD_VALUE;
	if (data.output.format != B_FMT_16BIT)
		return B_BAD_VALUE;

	if (rate != device->rate) {
		stop_playback(device);
		device->rate = rate;
	}
	return B_OK;
}


static status_t
get_buffers(audio_device* device, multi_buffer_list* userData)
{
	multi_buffer_list data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;
	if (data.request_playback_buffers < USER_BUFFERS
		|| data.request_playback_channels < CHANNELS
		|| !IS_USER_ADDRESS(data.playback_buffers)) {
		return B_BAD_VALUE;
	}

	buffer_desc* userBuffers[USER_BUFFERS];
	if (user_memcpy(userBuffers, data.playback_buffers, sizeof(userBuffers))
			!= B_OK) {
		return B_BAD_ADDRESS;
	}

	stop_playback(device);
	free_buffers(device);

	void* address;
	device->bufferArea = create_area("bcm2712 hdmi audio buffers", &address,
		B_ANY_KERNEL_ADDRESS,
		ROUNDUP(USER_BUFFERS * USER_BUFFER_BYTES, B_PAGE_SIZE), B_FULL_LOCK,
		B_READ_AREA | B_WRITE_AREA | B_KERNEL_READ_AREA
			| B_KERNEL_WRITE_AREA);
	if (device->bufferArea < 0)
		return device->bufferArea;
	memset(address, 0, USER_BUFFERS * USER_BUFFER_BYTES);

	for (uint32 i = 0; i < USER_BUFFERS; i++) {
		device->buffers[i] = (uint8*)address + i * USER_BUFFER_BYTES;

		buffer_desc descriptors[CHANNELS];
		for (uint32 channel = 0; channel < CHANNELS; channel++) {
			descriptors[channel].base = (char*)device->buffers[i]
				+ channel * 2;
			descriptors[channel].stride = USER_FRAME_SIZE;
		}
		if (!IS_USER_ADDRESS(userBuffers[i])
			|| user_memcpy(userBuffers[i], descriptors, sizeof(descriptors))
				!= B_OK) {
			free_buffers(device);
			return B_BAD_ADDRESS;
		}
	}

	data.flags = B_MULTI_BUFFER_PLAYBACK;
	data.return_playback_buffers = USER_BUFFERS;
	data.return_playback_channels = CHANNELS;
	data.return_playback_buffer_size = PERIOD_FRAMES;
	data.return_record_buffers = 0;
	data.return_record_channels = 0;
	data.return_record_buffer_size = 0;
	if (user_memcpy(userData, &data, sizeof(data)) != B_OK) {
		free_buffers(device);
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


static status_t
buffer_exchange(audio_device* device, multi_buffer_info* userData)
{
	if (device->bufferArea < 0)
		return B_NO_INIT;

	status_t status = start_playback(device);
	if (status != B_OK)
		return status;

	status = acquire_sem_etc(device->readySem, 1,
		B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 1000000);
	if (status != B_OK)
		return status;

	multi_buffer_info info;
	if (user_memcpy(&info, userData, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;

	InterruptsSpinLocker locker(device->timeLock);
	info.playback_buffer_cycle = device->playingBuffer;
	info.played_real_time = device->realTime;
	info.played_frames_count = device->framesCount;
	locker.Unlock();
	info.flags = B_MULTI_BUFFER_PLAYBACK;

	return user_memcpy(userData, &info, sizeof(info));
}


static uint32
gain_factor(float gain)
{
	// 10^(gain / 20) in Q16, without the maths library
	if (gain >= 0)
		return 0x10000;
	float factor = 1.0f;
	float step = 0.891250938f;	// -1 dB
	int32 steps = (int32)(-gain + 0.5f);
	for (int32 i = 0; i < steps; i++)
		factor *= step;
	return (uint32)(factor * 65536.0f);
}


static status_t
list_mix_controls(multi_mix_control_info* userData)
{
	multi_mix_control_info info;
	if (user_memcpy(&info, userData, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;

	multi_mix_control controls[4 + HDMI_PORTS];
	memset(controls, 0, sizeof(controls));
	uint32 count = 0;

	multi_mix_control* control = &controls[count++];
	control->id = CONTROL_GROUP;
	control->flags = B_MULTI_MIX_GROUP;
	control->string = S_OUTPUT;

	control = &controls[count++];
	control->id = CONTROL_VOLUME_LEFT;
	control->parent = CONTROL_GROUP;
	control->flags = B_MULTI_MIX_GAIN;
	control->gain.min_gain = MIN_GAIN;
	control->gain.max_gain = MAX_GAIN;
	control->gain.granularity = 1.0f;
	control->string = S_VOLUME;

	// one volume for both channels: the right one follows the left
	control = &controls[count++];
	*control = controls[count - 2];
	control->id = CONTROL_VOLUME_RIGHT;
	control->master = CONTROL_VOLUME_LEFT;

	control = &controls[count++];
	control->id = CONTROL_DESTINATION;
	control->parent = CONTROL_GROUP;
	control->flags = B_MULTI_MIX_MUX;
	strlcpy(control->name, "Output", sizeof(control->name));

	for (uint32 i = 0; i < HDMI_PORTS; i++) {
		control = &controls[count++];
		control->id = CONTROL_DESTINATION_FIRST + i;
		control->parent = CONTROL_DESTINATION;
		control->flags = B_MULTI_MIX_MUX_VALUE;
		strlcpy(control->name, kDestinations[i], sizeof(control->name));
	}

	if ((uint32)info.control_count < count)
		count = info.control_count;
	if (count > 0 && (!IS_USER_ADDRESS(info.controls)
			|| user_memcpy(info.controls, controls,
				count * sizeof(multi_mix_control)) != B_OK)) {
		return B_BAD_ADDRESS;
	}
	info.control_count = count;
	return user_memcpy(userData, &info, sizeof(info));
}


static status_t
get_or_set_mix(audio_device* device, multi_mix_value_info* userData, bool set)
{
	multi_mix_value_info info;
	if (user_memcpy(&info, userData, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;
	if (info.item_count < 0 || info.item_count > 32
		|| !IS_USER_ADDRESS(info.values)) {
		return B_BAD_VALUE;
	}

	bool outputChanged = false;
	for (int32 i = 0; i < info.item_count; i++) {
		multi_mix_value value;
		if (user_memcpy(&value, &info.values[i], sizeof(value)) != B_OK)
			return B_BAD_ADDRESS;

		switch (value.id) {
			case CONTROL_VOLUME_LEFT:
			case CONTROL_VOLUME_RIGHT:
				if (set) {
					float gain = value.gain;
					if (gain < MIN_GAIN)
						gain = MIN_GAIN;
					if (gain > MAX_GAIN)
						gain = MAX_GAIN;
					device->gain = gain;
					device->gainFactor = gain_factor(gain);
				} else
					value.gain = device->gain;
				break;

			case CONTROL_DESTINATION:
				if (set) {
					if (value.mux >= HDMI_PORTS)
						return B_BAD_VALUE;
					outputChanged |= value.mux != device->destination;
					device->destination = value.mux;
				} else
					value.mux = device->destination;
				break;

			default:
				continue;
		}

		if (!set && user_memcpy(&info.values[i], &value, sizeof(value))
				!= B_OK) {
			return B_BAD_ADDRESS;
		}
	}

	// the next buffer exchange starts on the new output
	if (outputChanged)
		stop_playback(device);
	return B_OK;
}


//	#pragma mark - device


static status_t
audio_open(const char* name, uint32 flags, void** _cookie)
{
	audio_device* device = &sDevice;

	MutexLocker locker(device->lock);
	if (device->openCount > 0)
		return B_BUSY;

	device->openCount++;
	*_cookie = device;
	return B_OK;
}


static status_t
audio_close(void* cookie)
{
	stop_playback((audio_device*)cookie);
	return B_OK;
}


static status_t
audio_free(void* cookie)
{
	audio_device* device = (audio_device*)cookie;

	stop_playback(device);

	MutexLocker locker(device->lock);
	free_buffers(device);
	device->openCount--;
	return B_OK;
}


static status_t
audio_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	audio_device* device = (audio_device*)cookie;

	switch (op) {
		case B_MULTI_GET_DESCRIPTION:
			return get_description((multi_description*)buffer);
		case B_MULTI_GET_ENABLED_CHANNELS:
			return get_enabled_channels((multi_channel_enable*)buffer);
		case B_MULTI_SET_ENABLED_CHANNELS:
			return B_OK;
		case B_MULTI_GET_GLOBAL_FORMAT:
			return get_global_format(device, (multi_format_info*)buffer);
		case B_MULTI_SET_GLOBAL_FORMAT:
			return set_global_format(device, (multi_format_info*)buffer);
		case B_MULTI_GET_BUFFERS:
			return get_buffers(device, (multi_buffer_list*)buffer);
		case B_MULTI_BUFFER_EXCHANGE:
			return buffer_exchange(device, (multi_buffer_info*)buffer);
		case B_MULTI_BUFFER_FORCE_STOP:
			stop_playback(device);
			return B_OK;
		case B_MULTI_LIST_MIX_CONTROLS:
			return list_mix_controls((multi_mix_control_info*)buffer);
		case B_MULTI_GET_MIX:
			return get_or_set_mix(device, (multi_mix_value_info*)buffer,
				false);
		case B_MULTI_SET_MIX:
			return get_or_set_mix(device, (multi_mix_value_info*)buffer,
				true);

		case B_MULTI_GET_EVENT_INFO:
		case B_MULTI_SET_EVENT_INFO:
		case B_MULTI_GET_EVENT:
		case B_MULTI_GET_CHANNEL_FORMATS:
		case B_MULTI_SET_CHANNEL_FORMATS:
		case B_MULTI_LIST_MIX_CHANNELS:
		case B_MULTI_LIST_MIX_CONNECTIONS:
		case B_MULTI_SET_BUFFERS:
		case B_MULTI_SET_START_TIME:
			return B_NOT_SUPPORTED;
	}

	return B_DEV_INVALID_IOCTL;
}


static status_t
audio_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_IO_ERROR;
}


static status_t
audio_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_IO_ERROR;
}


static device_hooks sHooks = {
	audio_open,
	audio_close,
	audio_free,
	audio_control,
	audio_read,
	audio_write
};


status_t
init_hardware()
{
	return B_OK;
}


status_t
init_driver()
{
	status_t status = get_module(B_DEVICE_MANAGER_MODULE_NAME,
		(module_info**)&sDeviceManager);
	if (status != B_OK)
		return status;

	audio_device* device = &sDevice;
	memset(device, 0, sizeof(*device));
	for (uint32 i = 0; i < HDMI_PORTS; i++)
		device->ports[i].hdmiArea = device->ports[i].packetArea = -1;
	device->hdArea = device->dvpArea = device->dmaRegistersArea = -1;
	device->dmaArea = device->bufferArea = -1;

	// a Raspberry Pi 5 (BCM2712) only
	uint32 found = 0;
	for (uint32 i = 0; i < HDMI_PORTS; i++) {
		if (find_port(device, i) == B_OK)
			found++;
	}
	if (found == 0) {
		unmap_hardware(device);
		put_module(B_DEVICE_MANAGER_MODULE_NAME);
		return B_ERROR;
	}

	fdt_node_info info;
	device->d0 = find_fdt_node("brcm,bcm2712d0-pinctrl", 0, info);
	if (device->d0)
		sDeviceManager->put_node(info.node);

	status = find_dvp(device);
	if (status != B_OK)
		ERROR("the DVP clock controller: %s\n", strerror(status));
	if (status == B_OK) {
		status = find_dma(device);
		if (status != B_OK)
			ERROR("the DMA channel: %s\n", strerror(status));
	}
	if (status == B_OK)
		status = allocate_dma_buffer(device);
	if (status != B_OK) {
		unmap_hardware(device);
		put_module(B_DEVICE_MANAGER_MODULE_NAME);
		return status;
	}

	for (uint32 i = 0; i < HDMI_PORTS; i++) {
		hdmi_port& port = device->ports[i];
		if (!port.present)
			continue;
		INFO("HDMI %" B_PRIu32 ": hotplug %#" B_PRIx32 ", scheduler %#"
			B_PRIx32 ", RAM packets %#" B_PRIx32 "/%#" B_PRIx32 ", CRP %#"
			B_PRIx32 ", CTS %" B_PRIu32 ", MAI_CTL %#" B_PRIx32 ", DREQ cell %#"
			B_PRIx32 "\n", i, read32(port.hdmi, HDMI_HOTPLUG),
			read32(port.hdmi, HDMI_SCHEDULER_CONTROL),
			read32(port.hdmi, HDMI_RAM_PACKET_CONFIG),
			read32(port.hdmi, HDMI_RAM_PACKET_STATUS),
			read32(port.hdmi, HDMI_CRP_CFG), read32(port.hdmi, HDMI_CTS_0),
			read32(device->hd, port.maiOffset + HD_MAI_CTL), port.dreq);
	}
	INFO("DVP misc %#" B_PRIx32 ", DMA channel %d CS %#" B_PRIx32 " debug %#"
		B_PRIx32 ", interrupt %" B_PRIu32 ", %s stepping\n",
		read32(device->dvp, DVP_MISC_CONFIG), DMA_CHANNEL,
		read32(device->dma, DMA40_CS), read32(device->dma, DMA40_DEBUG),
		device->dmaInterrupt, device->d0 ? "D0" : "C0");

	mutex_init(&device->lock, "bcm2712 hdmi audio");
	device->timeLock = B_SPINLOCK_INITIALIZER;
	device->rate = 48000;
	device->gain = 0;
	device->gainFactor = 0x10000;
	device->destination = 0;
	device->periodSem = create_sem(0, "bcm2712 hdmi audio period");
	device->readySem = create_sem(0, "bcm2712 hdmi audio buffer");
	if (device->periodSem < 0 || device->readySem < 0) {
		delete_sem(device->periodSem);
		delete_sem(device->readySem);
		mutex_destroy(&device->lock);
		unmap_hardware(device);
		put_module(B_DEVICE_MANAGER_MODULE_NAME);
		return B_NO_MORE_SEMS;
	}

	return B_OK;
}


void
uninit_driver()
{
	audio_device* device = &sDevice;
	delete_sem(device->periodSem);
	delete_sem(device->readySem);
	mutex_destroy(&device->lock);
	unmap_hardware(device);
	put_module(B_DEVICE_MANAGER_MODULE_NAME);
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sHooks;
}
