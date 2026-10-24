/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "display_port.h"

#include "dp_tu.h"

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
	uint32		pixelClock;		// its pixel 0 RCG
} kControllers[] = {
	{0xae90000, 0x206c, 0x205c, 0x21d8},
	{0xae98000, 0x20a0, 0x2084, 0x2250},
	{0xae9a000, 0x20a4, 0x2090, 0x2208},
};
// an RCG: CMD (bit 0 updates the configuration), CFG, M, N, D
enum {
	RCG_CMD	= 0x0,
	RCG_CFG	= 0x4,
	RCG_M	= 0x8,
	RCG_N	= 0xc,
	RCG_D	= 0x10,
};
static const uint32 kRcgUpdate = 1u << 0;
static const uint32 kRcgModeMask = 3u << 12;
static const uint32 kRcgDualEdge = 2u << 12;
static const uint32 kRcgMndMask = 0xffff;
static const phys_addr_t kDispccBase = 0xaf00000;

// the controller's blocks (the device tree's "reg"): ahb, aux, link
static const uint32 kAhb = 0x000;
static const uint32 kAux = 0x200;
static const uint32 kLink = 0x400;

// ahb (dp_reg.h); each interrupt has a status bit, the ack bit above it and
// the mask bit above that
enum {
	REG_DP_INTR_STATUS	= kAhb + 0x020,
	REG_DP_INTR_STATUS2	= kAhb + 0x024,
};
static const uint32 kReadyForVideo = 1u << 0;
static const uint32 kIdlePatternSent = 1u << 3;
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

static const uint32 kPixelInterface = 0x1000;	// MMSS_DP_* of "p0"

// link
enum {
	REG_DP_STATE_CTRL			= kLink + 0x004,
	REG_DP_CONFIGURATION_CTRL	= kLink + 0x008,
	REG_DP_SOFTWARE_MVID		= kLink + 0x010,
	REG_DP_SOFTWARE_NVID		= kLink + 0x018,
	REG_DP_TOTAL_HOR_VER		= kLink + 0x01c,
	REG_DP_START_HOR_VER_FROM_SYNC = kLink + 0x020,
	REG_DP_HSYNC_VSYNC_WIDTH_POLARITY = kLink + 0x024,
	REG_DP_ACTIVE_HOR_VER		= kLink + 0x028,
	REG_DP_MISC1_MISC0			= kLink + 0x02c,
	REG_DP_VALID_BOUNDARY		= kLink + 0x030,
	REG_DP_VALID_BOUNDARY_2		= kLink + 0x034,
	REG_DP_MAINLINK_READY		= kLink + 0x040,
	REG_DP_TU					= kLink + 0x04c,
};
static const uint32 kStateSendVideo = 1u << 7;
static const uint32 kStatePushIdle = 1u << 8;
static const uint32 kMainlinkReadyForVideo = 1u << 0;

// DPCD
static const uint32 kDpcdLinkBandwidthSet = 0x100;
static const uint32 kNvidFixed = 0x8000;	// DP_LINK_CONSTANT_N_VALUE

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


/*!	The best fraction \a _numerator / \a _denominator of \a value / \a over
	with neither larger than \a max (Linux's rational_best_approximation()).
*/
static void
best_fraction(uint64 value, uint64 over, uint64 max, uint64* _numerator,
	uint64* _denominator)
{
	uint64 n = value, d = over;
	uint64 n0 = 0, d1 = 0, n1 = 1, d0 = 1;
	while (d != 0) {
		uint64 dp = d;
		uint64 a = n / d;
		d = n % d;
		n = dp;

		uint64 n2 = n0 + a * n1;
		uint64 d2 = d0 + a * d1;
		if (n2 > max || d2 > max) {
			// the largest semiconvergent, when it is the closer one
			uint64 t = UINT64_MAX;
			if (d1 != 0)
				t = (max - d0) / d1;
			if (n1 != 0 && (max - n0) / n1 < t)
				t = (max - n0) / n1;
			if (d1 == 0 || 2 * t > a || (2 * t == a && d0 * dp > d1 * d)) {
				n1 = n0 + t * n1;
				d1 = d0 + t * d1;
			}
			break;
		}
		n0 = n1;
		n1 = n2;
		d0 = d1;
		d1 = d2;
	}
	*_numerator = n1;
	*_denominator = d1;
}


DisplayPort::DisplayPort(uint32 index)
	:
	fIndex(index),
	fArea(-1),
	fBase(NULL),
	fClockArea(-1),
	fClocks(NULL),
	fLinkRate(0)
{
}


DisplayPort::~DisplayPort()
{
	if (fArea >= 0)
		delete_area(fArea);
	if (fClockArea >= 0)
		delete_area(fClockArea);
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
	// with the pixel interface ("p0", at 0x1000)
	fArea = map_physical_memory("qcom dp", kControllers[fIndex].base, 0x2000,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fBase);
	if (fArea < 0)
		return fArea;
	if ((_Read(REG_DP_AUX_CTRL) & kAuxEnable) == 0) {
		ERROR("DP %" B_PRIu32 ": AUX is off\n", fIndex);
		return B_DEVICE_NOT_FOUND;
	}
	uint32 rcg = kControllers[fIndex].pixelClock;
	fClockArea = map_physical_memory("qcom dp pixel clock",
		kDispccBase + (rcg & ~(B_PAGE_SIZE - 1)), B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fClocks);
	if (fClockArea < 0)
		return fClockArea;
	fClocks += rcg & (B_PAGE_SIZE - 1);
	return B_OK;
}


uint32
DisplayPort::LinkRate()
{
	if (fLinkRate != 0 || !HotPlug())
		return fLinkRate;
	uint8 bandwidth;
	if (ReadDpcd(kDpcdLinkBandwidthSet, &bandwidth, 1) == B_OK) {
		// in units of 0.27 Gbit/s: 6, 10, 20, 30
		switch (bandwidth) {
			case 0x06: fLinkRate = 162; break;
			case 0x0a: fLinkRate = 270; break;
			case 0x14: fLinkRate = 540; break;
			case 0x1e: fLinkRate = 810; break;
		}
	}
	return fLinkRate;
}


uint32
DisplayPort::MaxPixelClock()
{
	// 8b/10b: each lane carries a byte per link symbol clock
	return LinkRate() * 1000 * Lanes() / 3;
}


uint32
DisplayPort::PixelClock(bool wideBus)
{
	uint32 linkRate = LinkRate();
	if (fClocks == NULL || linkRate == 0)
		return 0;
	uint32 divider = linkRate == 810 ? 6 : linkRate == 540 ? 4 : 2;
	uint64 parent = (uint64)linkRate * 1000 * 10 / divider;
	uint32 config = *(volatile uint32*)(fClocks + RCG_CFG);
	uint64 rate = parent;
	if ((config & kRcgModeMask) == kRcgDualEdge) {
		uint32 m = *(volatile uint32*)(fClocks + RCG_M) & kRcgMndMask;
		uint32 n = ((~*(volatile uint32*)(fClocks + RCG_N)) & kRcgMndMask) + m;
		if (m == 0 || n == 0)
			return 0;
		rate = parent * m / n;
	}
	// a pre-divider other than 1 (CFG bits 0-4 = 2 * divider - 1) is not
	// what this driver sets
	if ((config & 0x1f) > 1)
		return 0;
	return wideBus ? rate * 2 : rate;
}


/*!	The pixel RCG at \a rate kHz. Its parent is the PHY's link clock divided
	for the pixel path, fixed by the link rate (msm_dp_ctrl_config_msa()'s
	dispcc_input_rate); M/N do the rest.
*/
status_t
DisplayPort::_SetPixelClock(uint32 rate)
{
	uint32 linkRate = LinkRate();
	uint32 divider = linkRate == 810 ? 6 : linkRate == 540 ? 4 : 2;
	uint64 parent = (uint64)linkRate * 1000 * 10 / divider;
	if (linkRate == 0 || rate == 0 || rate > parent)
		return B_BAD_VALUE;

	uint64 m, n;
	best_fraction(rate, parent, kRcgMndMask, &m, &n);
	if (m == 0 || n == 0)
		return B_BAD_VALUE;

	uint32 config = *(volatile uint32*)(fClocks + RCG_CFG) & ~kRcgModeMask;
	if (m != n) {
		uint32 d = n;
		uint32 twiceNMinusM = (n - m) * 2;
		if (d < m)
			d = m;
		if (d > twiceNMinusM)
			d = twiceNMinusM;
		*(volatile uint32*)(fClocks + RCG_M) = m;
		*(volatile uint32*)(fClocks + RCG_N) = ~(n - m) & kRcgMndMask;
		*(volatile uint32*)(fClocks + RCG_D) = ~d & kRcgMndMask;
		config |= kRcgDualEdge;
	}
	*(volatile uint32*)(fClocks + RCG_CFG) = config;
	*(volatile uint32*)(fClocks + RCG_CMD) |= kRcgUpdate;
	bigtime_t timeout = system_time() + 10000;
	while ((*(volatile uint32*)(fClocks + RCG_CMD) & kRcgUpdate) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(1);
	}
	return B_OK;
}


/*!	Sends the idle pattern instead of the stream, for the stream to be
	retimed.
*/
status_t
DisplayPort::StopStream()
{
	if (fBase == NULL)
		return B_NO_INIT;
	_Write(REG_DP_INTR_STATUS2, kIdlePatternSent << 1);
	_Write(REG_DP_STATE_CTRL, kStatePushIdle);
	bigtime_t timeout = system_time() + 50000;
	while ((_Read(REG_DP_INTR_STATUS2) & kIdlePatternSent) == 0) {
		if (system_time() > timeout) {
			ERROR("DP %" B_PRIu32 ": no idle pattern (status %#" B_PRIx32
				")\n", fIndex, _Read(REG_DP_INTR_STATUS2));
			return B_TIMED_OUT;
		}
		spin(20);
	}
	_Write(REG_DP_INTR_STATUS2, kIdlePatternSent << 1);
	return B_OK;
}


/*!	The stream's clock, attributes and transfer units for \a timing on the
	trained link (msm_dp_ctrl_on_stream() without the training). With the
	wide data bus the pixel clock runs at half the pixel rate.
*/
status_t
DisplayPort::SetStream(const display_timing& timing, bool wideBus)
{
	uint32 linkRate = LinkRate();
	uint32 lanes = Lanes();
	if (linkRate == 0 || lanes == 0)
		return B_NO_INIT;
	if (timing.pixel_clock > MaxPixelClock())
		return B_BAD_VALUE;

	status_t status = _SetPixelClock(wideBus
		? timing.pixel_clock / 2 : timing.pixel_clock);
	if (status != B_OK) {
		ERROR("DP %" B_PRIu32 ": pixel clock %" B_PRIu32 " kHz: %s\n", fIndex,
			timing.pixel_clock, strerror(status));
		return status;
	}

	// main stream attributes: totals, start from the syncs (sync and back
	// porch), sync widths and polarities, active size
	uint32 hStart = timing.h_total - timing.h_sync_start;
	uint32 vStart = timing.v_total - timing.v_sync_start;
	_Write(REG_DP_TOTAL_HOR_VER, (uint32)timing.v_total << 16 | timing.h_total);
	_Write(REG_DP_START_HOR_VER_FROM_SYNC, vStart << 16 | hStart);
	_Write(REG_DP_HSYNC_VSYNC_WIDTH_POLARITY,
		(timing.v_sync_positive ? 0 : 1u << 31)
		| (uint32)(timing.v_sync_end - timing.v_sync_start) << 16
		| (timing.h_sync_positive ? 0 : 1u << 15)
		| (timing.h_sync_end - timing.h_sync_start));
	_Write(REG_DP_ACTIVE_HOR_VER,
		(uint32)timing.v_display << 16 | timing.h_display);
	// MISC0 (msm_dp_ctrl_configure_source_params()): 8 bits a component,
	// the synchronous clock, the colorimetry as it is; the pixel
	// interface's data bus widened for two pixels a clock
	// (msm_dp_panel_timing_cfg(); the firmware sets both on its output)
	uint32 misc = _Read(REG_DP_MISC1_MISC0) & ~(7u << 5);
	_Write(REG_DP_MISC1_MISC0, misc | 1u << 5 | 1);
	// no DSC: its DTO off (msm_dp_panel_clear_dsc_dto(); after a reset
	// the USB-C controllers have it at 2)
	_Write(kPixelInterface + 0x7c, 0);
	uint32 intfConfig = _Read(kPixelInterface + 0x14);
	_Write(kPixelInterface + 0x14,
		wideBus ? intfConfig | 1u << 4 : intfConfig & ~(1u << 4));

	// Mvid/Nvid, as msm_dp_ctrl_config_msa()
	uint32 divider = linkRate == 810 ? 6 : linkRate == 540 ? 4 : 2;
	uint64 input = (uint64)linkRate * 1000 * 10 / divider;
	uint64 m, n;
	best_fraction(timing.pixel_clock, input, kRcgMndMask, &m, &n);
	uint32 mvid = m * 5;
	uint32 nvid = n;
	if (nvid < kNvidFixed) {
		uint32 factor = kNvidFixed / nvid;
		mvid *= factor;
		nvid *= factor;
	}
	if (linkRate == 540)
		nvid *= 2;
	else if (linkRate == 810)
		nvid *= 3;
	_Write(REG_DP_SOFTWARE_MVID, mvid);
	_Write(REG_DP_SOFTWARE_NVID, nvid);

	uint32 tu, valid, valid2;
	status = qcom_dp_calc_tu(linkRate, timing.pixel_clock, timing.h_display,
		timing.h_total - timing.h_display, lanes, 24, &tu, &valid, &valid2);
	if (status != B_OK)
		return status;
	_Write(REG_DP_VALID_BOUNDARY, valid);
	_Write(REG_DP_TU, tu);
	_Write(REG_DP_VALID_BOUNDARY_2, valid2);
	INFO("DP %" B_PRIu32 ": %ux%u at %" B_PRIu32 " kHz on %" B_PRIu32
		" lanes at %" B_PRIu32 " MHz: M/N %" B_PRIu32 "/%" B_PRIu32 ", TU %#"
		B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32 "\n", fIndex, timing.h_display,
		timing.v_display, timing.pixel_clock, lanes, linkRate, mvid, nvid, tu,
		valid, valid2);
	return B_OK;
}


/*!	The stream again, once the timing engine runs. */
status_t
DisplayPort::StartStream()
{
	if (fBase == NULL)
		return B_NO_INIT;
	_Write(REG_DP_INTR_STATUS2, kReadyForVideo << 1);
	_Write(REG_DP_STATE_CTRL, kStateSendVideo);
	bigtime_t timeout = system_time() + 100000;
	while ((_Read(REG_DP_INTR_STATUS2) & kReadyForVideo) == 0
		&& (_Read(REG_DP_MAINLINK_READY) & kMainlinkReadyForVideo) == 0) {
		if (system_time() > timeout) {
			ERROR("DP %" B_PRIu32 ": no video (ready %#" B_PRIx32 ")\n", fIndex,
				_Read(REG_DP_MAINLINK_READY));
			return B_TIMED_OUT;
		}
		spin(50);
	}
	_Write(REG_DP_INTR_STATUS2, kReadyForVideo << 1);

	// what the sink says of the link (sink count, lane status, alignment,
	// sink status)
	uint8 status[6];
	if (ReadDpcd(0x200, status, sizeof(status)) == B_OK) {
		INFO("DP %" B_PRIu32 ": sink count %#x, lanes %#x %#x, align %#x, "
			"sink %#x\n", fIndex, status[0], status[2], status[3], status[4],
			status[5]);
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
DisplayPort::WriteDpcd(uint32 address, const uint8* buffer, size_t size)
{
	while (size > 0) {
		size_t chunk = size > 16 ? 16 : size;
		status_t status = _Transfer(true, false, address, (uint8*)buffer,
			chunk, true);
		if (status != B_OK)
			return status;
		address += chunk;
		buffer += chunk;
		size -= chunk;
	}
	return B_OK;
}


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
