/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "display_port.h"

#include <string.h>

#include <KernelExport.h>


#define ERROR(x...)	dprintf("qcom_display: " x)
#define INFO(x...)	dprintf("qcom_display: " x)


namespace qcom {


// MDSS 0's DisplayPort controllers and the dispcc 0 branches in front of
// their AUX and link blocks
static const struct {
	phys_addr_t	base;
	uint32		auxClock;
	uint32		linkClock;
} kControllers[] = {
	{0xae90000, 0x206c, 0x205c},
	{0xae98000, 0x20a0, 0x2084},
	{0xae9a000, 0x20a4, 0x2090},
};
static const phys_addr_t kDispccBase = 0xaf00000;

// the controller's blocks (the device tree's "reg"): ahb, aux, link
static const uint32 kAhb = 0x000;
static const uint32 kAux = 0x200;
static const uint32 kLink = 0x400;

// ahb (dp_reg.h); each interrupt has a status bit, the ack bit above it and
// the mask bit above that
enum {
	REG_DP_INTR_STATUS	= kAhb + 0x020,
};
static const uint32 kAuxDone = 1u << 3;
static const uint32 kAuxWrongAddress = 1u << 6;
static const uint32 kAuxTimeout = 1u << 9;
static const uint32 kAuxNackDefer = 1u << 12;
static const uint32 kAuxWrongCount = 1u << 15;
static const uint32 kAuxI2cNack = 1u << 18;
static const uint32 kAuxI2cDefer = 1u << 21;
static const uint32 kAuxError = 1u << 27;
static const uint32 kAuxStatus = kAuxDone | kAuxWrongAddress | kAuxTimeout
	| kAuxNackDefer | kAuxWrongCount | kAuxI2cNack | kAuxI2cDefer | kAuxError;

// aux
enum {
	REG_DP_DP_HPD_INT_STATUS	= kAux + 0x004,
	REG_DP_AUX_CTRL				= kAux + 0x030,
	REG_DP_AUX_DATA				= kAux + 0x034,
	REG_DP_AUX_TRANS_CTRL		= kAux + 0x038,
	REG_DP_PHY_AUX_INTERRUPT_CLEAR = kAux + 0x04c,
	REG_DP_PHY_AUX_INTERRUPT_STATUS = kAux + 0x0bc,
};
static const uint32 kHotPlugConnected = 0x40000000;
static const uint32 kAuxEnable = 1u << 0;
static const uint32 kAuxDataRead = 1u << 0;
static const uint32 kAuxDataIndexWrite = 1u << 31;
static const uint32 kTransI2c = 1u << 8;
static const uint32 kTransGo = 1u << 9;
static const uint32 kTransNoSendAddress = 1u << 10;
static const uint32 kTransNoSendStop = 1u << 11;

// link
enum {
	REG_DP_CONFIGURATION_CTRL	= kLink + 0x008,
	REG_DP_TOTAL_HOR_VER		= kLink + 0x01c,
	REG_DP_START_HOR_VER_FROM_SYNC = kLink + 0x020,
	REG_DP_HSYNC_VSYNC_WIDTH_POLARITY = kLink + 0x024,
	REG_DP_ACTIVE_HOR_VER		= kLink + 0x028,
};

static const uint32 kEdidAddress = 0x50;


static bool
branch_on(uint32 offset)
{
	volatile uint32* reg;
	area_id area = map_physical_memory("qcom dp clock check",
		kDispccBase + (offset & ~(B_PAGE_SIZE - 1)), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, (void**)&reg);
	if (area < 0)
		return false;
	uint32 value = reg[(offset & (B_PAGE_SIZE - 1)) / 4];
	delete_area(area);
	// a CBCR reads bit 31 set while the clock is off
	return (value & (1u << 31)) == 0;
}


DisplayPort::DisplayPort(uint32 index)
	:
	fIndex(index),
	fArea(-1),
	fBase(NULL)
{
}


DisplayPort::~DisplayPort()
{
	if (fArea >= 0)
		delete_area(fArea);
}


status_t
DisplayPort::Init()
{
	if (fIndex >= B_COUNT_OF(kControllers))
		return B_BAD_VALUE;
	// the caller checked MDSS 0's power and AHB clock
	if (!branch_on(kControllers[fIndex].auxClock)
		|| !branch_on(kControllers[fIndex].linkClock)) {
		return B_DEVICE_NOT_FOUND;
	}
	fArea = map_physical_memory("qcom dp", kControllers[fIndex].base, 0x1000,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fBase);
	if (fArea < 0)
		return fArea;
	if ((_Read(REG_DP_AUX_CTRL) & kAuxEnable) == 0) {
		ERROR("DP %" B_PRIu32 ": AUX is off\n", fIndex);
		return B_DEVICE_NOT_FOUND;
	}
	return B_OK;
}


bool
DisplayPort::HotPlug()
{
	return fBase != NULL
		&& (_Read(REG_DP_DP_HPD_INT_STATUS) & kHotPlugConnected) != 0;
}


uint32
DisplayPort::Lanes()
{
	return fBase != NULL
		? ((_Read(REG_DP_CONFIGURATION_CTRL) >> 4) & 3) + 1 : 0;
}


/*!	One AUX request of up to 16 bytes natively, 128 over I2C, as Linux's
	msm_dp_aux_transfer() makes it; \a stop ends an I2C transaction.
*/
status_t
DisplayPort::_Transfer(bool native, bool read, uint32 address, uint8* buffer,
	size_t size, bool stop)
{
	if (fBase == NULL)
		return B_NO_INIT;
	if (size == 0 || size > (native ? 16 : 128))
		return B_BAD_VALUE;

	for (int attempt = 0; attempt < 5; attempt++) {
		// the request: address and length, then the data of a write
		uint8 header[4];
		header[0] = ((address >> 16) & 0xf) | (read ? 0x10 : 0);
		header[1] = address >> 8;
		header[2] = address;
		header[3] = size - 1;
		size_t length = 4 + (read ? 0 : size);
		for (size_t i = 0; i < length; i++) {
			uint32 data = (i < 4 ? header[i] : buffer[i - 4]) << 8;
			if (i == 0)
				data |= kAuxDataIndexWrite;
			_Write(REG_DP_AUX_DATA, data);
		}

		_Write(REG_DP_AUX_TRANS_CTRL, 0);
		_Read(REG_DP_PHY_AUX_INTERRUPT_STATUS);
		_Write(REG_DP_PHY_AUX_INTERRUPT_CLEAR, 0x1f);
		_Write(REG_DP_PHY_AUX_INTERRUPT_CLEAR, 0x9f);
		_Write(REG_DP_PHY_AUX_INTERRUPT_CLEAR, 0);
		// acknowledge what an earlier request left
		_Write(REG_DP_INTR_STATUS, (_Read(REG_DP_INTR_STATUS) & kAuxStatus)
			<< 1);

		uint32 control = kTransGo;
		if (!native) {
			control |= kTransI2c | kTransNoSendAddress;
			if (!stop)
				control |= kTransNoSendStop;
		}
		_Write(REG_DP_AUX_TRANS_CTRL, control);

		uint32 status = 0;
		bigtime_t timeout = system_time() + 250000;
		while (system_time() < timeout) {
			status = _Read(REG_DP_INTR_STATUS) & kAuxStatus;
			if (status != 0)
				break;
			spin(20);
		}
		_Write(REG_DP_INTR_STATUS, status << 1);
		_Write(REG_DP_AUX_TRANS_CTRL, _Read(REG_DP_AUX_TRANS_CTRL) & ~kTransGo);

		if (status == 0)
			return B_TIMED_OUT;
		if ((status & (kAuxNackDefer | kAuxI2cDefer)) != 0) {
			snooze(1000);
			continue;
		}
		if ((status & kAuxDone) == 0 || (status & ~kAuxDone) != 0)
			return B_IO_ERROR;

		if (read) {
			_Write(REG_DP_AUX_DATA, kAuxDataIndexWrite | kAuxDataRead);
			// the first byte is the reply code
			_Read(REG_DP_AUX_DATA);
			for (size_t i = 0; i < size; i++) {
				uint32 data = _Read(REG_DP_AUX_DATA);
				buffer[i] = (data >> 8) & 0xff;
				if (((data >> 16) & 0xff) != i)
					return B_IO_ERROR;
			}
		}
		return B_OK;
	}
	return B_BUSY;
}


status_t
DisplayPort::ReadDpcd(uint32 address, uint8* buffer, size_t size)
{
	while (size > 0) {
		size_t chunk = size > 16 ? 16 : size;
		status_t status = _Transfer(true, true, address, buffer, chunk, true);
		if (status != B_OK)
			return status;
		address += chunk;
		buffer += chunk;
		size -= chunk;
	}
	return B_OK;
}


/*!	The sink's EDID over I2C on AUX (DDC through the board's HDMI bridge):
	the base block and, when it names one, the first extension.
*/
status_t
DisplayPort::ReadEdid(uint8* edid, uint32* _length)
{
	uint32 length = 0;
	for (uint32 block = 0; block < 2; block++) {
		uint8 offset = block * 128;
		status_t status = _Transfer(false, false, kEdidAddress, &offset, 1,
			false);
		if (status == B_OK) {
			status = _Transfer(false, true, kEdidAddress, edid + block * 128,
				128, true);
		}
		if (status != B_OK) {
			if (block == 0)
				return status;
			break;
		}
		length += 128;
		if (block == 0) {
			static const uint8 kHeader[] = {0, 0xff, 0xff, 0xff, 0xff, 0xff,
				0xff, 0};
			if (memcmp(edid, kHeader, sizeof(kHeader)) != 0)
				return B_BAD_DATA;
			if (edid[126] == 0)
				break;
		}
	}
	*_length = length;
	return B_OK;
}


/*!	The mode the stream carries, from its main stream attributes. */
status_t
DisplayPort::StreamTiming(display_timing& timing)
{
	if (fBase == NULL)
		return B_NO_INIT;
	uint32 total = _Read(REG_DP_TOTAL_HOR_VER);
	uint32 start = _Read(REG_DP_START_HOR_VER_FROM_SYNC);
	uint32 sync = _Read(REG_DP_HSYNC_VSYNC_WIDTH_POLARITY);
	uint32 active = _Read(REG_DP_ACTIVE_HOR_VER);
	uint32 hTotal = total & 0xffff, vTotal = total >> 16;
	uint32 hStart = start & 0xffff, vStart = start >> 16;
	uint32 hSync = sync & 0x7fff, vSync = (sync >> 16) & 0x7fff;
	uint32 hActive = active & 0xffff, vActive = active >> 16;
	if (hTotal == 0 || vTotal == 0 || hActive == 0 || vActive == 0
		|| hStart + hActive > hTotal || vStart + vActive > vTotal) {
		return B_BAD_DATA;
	}

	// start = sync width + back porch; the front porch is the rest
	timing.h_display = hActive;
	timing.h_sync_start = hTotal - hStart;
	timing.h_sync_end = timing.h_sync_start + hSync;
	timing.h_total = hTotal;
	timing.v_display = vActive;
	timing.v_sync_start = vTotal - vStart;
	timing.v_sync_end = timing.v_sync_start + vSync;
	timing.v_total = vTotal;
	// a polarity bit set means active low
	timing.h_sync_positive = (sync & (1u << 15)) == 0;
	timing.v_sync_positive = (sync & (1u << 31)) == 0;
	return B_OK;
}


}	// namespace qcom
