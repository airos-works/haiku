/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "codec.h"

#include <string.h>

#include <KernelExport.h>

#include "dsp.h"


#define TRACE(x...) dprintf("qcom_audio: " x)


namespace qcom_audio {


// LPASS (the board's device tree)
static const phys_addr_t kRxMacro = 0x3200000;
static const phys_addr_t kRxBus = 0x3210000;
static const phys_addr_t kTxMacro = 0x3220000;
static const phys_addr_t kTxBus = 0x3330000;
static const phys_addr_t kVaMacro = 0x3370000;
static const phys_addr_t kLpiPins = 0x33c0000;
static const phys_addr_t kLpassCc = 0x33e0000;
static const phys_addr_t kTlmm = 0xf100000;
static const uint32 kCodecResetGpio = 106;

// the WCD9385's SoundWire devices: their unique IDs (sdw20217010d00)
static const uint32 kRxDeviceID = 4;
static const uint32 kTxDeviceID = 3;

// the PRM's resources: the LPASS macro and digital codec votes, the macros'
// clocks (q6prm.h; the device tree's LPASS_CLK_ID_RX_CORE_TX_MCLK,
// _RX_CORE_TX_2X_MCLK for the RX macro, _TX_CORE_MCLK and _TX_CORE_NPL_MCLK
// for the TX and VA macros)
static const uint32 kVoteMacro = 1, kVoteDcodec = 2;
static const uint32 kClocks[] = { 0x312, 0x313, 0x30c, 0x30d };
static const uint32 kMclkRate = 19200000;

// SoundWire controller registers (qcom.c, v1.3 to v1.7 layout)
enum {
	kSwrVersion = 0x00,
	kSwrConfig = 0x04,
	kSwrStatus = 0x14,
	kSwrInterruptStatus = 0x200,
	kSwrInterruptMask = 0x204,
	kSwrInterruptClear = 0x208,
	kSwrWriteCommand = 0x300,
	kSwrReadCommand = 0x304,
	kSwrFifoCommand = 0x308,
	kSwrFifoStatus = 0x30c,
	kSwrFifoConfig = 0x314,
	kSwrReadFifo = 0x318,
	kSwrEnumerator = 0x500,
	kSwrDeviceID1 = 0x530,
	kSwrDeviceID2 = 0x534,
	kSwrFrameControl = 0x101c,
	kSwrBusControl = 0x1044,
	kSwrMcpConfig = 0x1048,
	kSwrSlaveStatus = 0x1090,
};


static inline uint32
read32(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
write32(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
	memory_full_barrier();
}


static inline void
set32(volatile uint8* base, uint32 offset, uint32 bits)
{
	write32(base, offset, read32(base, offset) | bits);
}


static inline void
clear32(volatile uint8* base, uint32 offset, uint32 bits)
{
	write32(base, offset, read32(base, offset) & ~bits);
}


static area_id
map(const char* name, phys_addr_t address, size_t size, volatile uint8*& base)
{
	return map_physical_memory(name, address, size,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&base);
}


// #pragma mark - SoundWire


SoundWire::SoundWire(const char* label)
	:
	fLabel(label),
	fArea(-1),
	fBase(NULL),
	fCommandID(0)
{
}


SoundWire::~SoundWire()
{
	Stop();
}


/*!	As qcom_swrm_init(): 50 rows, 16 columns in bank 0, auto enumeration,
	the bus clock started; then the frames have to run.
*/
status_t
SoundWire::Start(phys_addr_t address)
{
	fArea = map("qcom audio soundwire", address, 0x2000, fBase);
	if (fArea < 0)
		return fArea;
	_Write(kSwrFrameControl, 7 | 1u << 3);
	_Write(kSwrEnumerator, 1);
	_Write(kSwrInterruptMask, 0x1ffff);
	uint32 config = _Read(kSwrMcpConfig);
	_Write(kSwrMcpConfig, (config & ~(0x1fu << 17)) | 0x1fu << 17);
	_Write(kSwrBusControl, 1u << 1);
	_Write(kSwrFifoConfig, 7 | 1u << 31);		// retries, continue
	_Write(kSwrConfig, 1);
	_Write(kSwrConfig, 2);
	_Write(kSwrInterruptClear, 0xffffffff);
	_Write(kSwrConfig, 3);
	for (int i = 0; i < 100; i++) {
		if ((_Read(kSwrStatus) & 1) != 0) {
			TRACE("%s SoundWire (version %#" B_PRIx32 ") runs\n", fLabel,
				_Read(kSwrVersion));
			return B_OK;
		}
		snooze(500);
	}
	TRACE("%s SoundWire: no frames (status %#" B_PRIx32 ")\n", fLabel,
		_Read(kSwrStatus));
	Stop();
	return B_ERROR;
}


void
SoundWire::Stop()
{
	if (fArea >= 0)
		delete_area(fArea);
	fArea = -1;
	fBase = NULL;
}


uint32
SoundWire::FindDevice(uint32 uniqueID, bigtime_t timeout)
{
	if (fBase == NULL)
		return 0;
	for (bigtime_t end = system_time() + timeout;; snooze(20000)) {
		uint32 status = _Read(kSwrSlaveStatus);
		for (uint32 device = 1; device < 12; device++) {
			if ((status >> (device * 2) & 3) == 0)
				continue;
			// qcom_swrm_enumerate(): DevId_0 to 5, the 48-bit address
			uint32 id1 = _Read(kSwrDeviceID1 + 8 * device);
			uint32 id2 = _Read(kSwrDeviceID2 + 8 * device);
			uint64 address = (id2 >> 8 & 0xff) | (id2 & 0xff) << 8
				| (id1 >> 24 & 0xff) << 16 | (uint64)(id1 >> 16 & 0xff) << 24
				| (uint64)(id1 >> 8 & 0xff) << 32
				| (uint64)(id1 & 0xff) << 40;
			uint32 manufacturer = address >> 24 & 0xffff;
			uint32 part = address >> 8 & 0xffff;
			if ((address >> 40 & 0xf) != uniqueID || manufacturer != 0x217
				|| part != 0x10d) {
				continue;
			}
			// the command FIFOs flushed, for a clean start
			_Write(kSwrFifoCommand, 1);
			_Write(kSwrInterruptClear, 0xffffffff);
			TRACE("%s SoundWire: the WCD938x's device with unique ID %"
				B_PRIu32 " is device %" B_PRIu32 "\n", fLabel, uniqueID,
				device);
			return device;
		}
		if (system_time() > end)
			return 0;
	}
}


//!	A register of \a device (qcom_swrm_cmd_fifo_rd_cmd()); -1 if unanswered.
int
SoundWire::Read(uint32 device, uint16 reg)
{
	if (fBase == NULL)
		return -1;
	fCommandID = fCommandID < 14 ? fCommandID + 1 : 0;
	// a byte (QCOM_SWRM_MAX_RD_LEN in the data field; 0 reads 17)
	uint32 command = reg | fCommandID << 16 | device << 20 | 1u << 24;
	_Write(kSwrReadCommand, command);
	spin(250);
	for (int attempt = 0; attempt < 3; attempt++) {
		bool available = false;
		for (int i = 0; i < 30 && !available; i++) {
			available = (_Read(kSwrFifoStatus) >> 16 & 0x1f) != 0;
			if (!available)
				spin(500);
		}
		if (!available)
			break;
		uint32 data = _Read(kSwrReadFifo);
		if ((data >> 8 & 0xf) == fCommandID)
			return data & 0xff;
		// a stale answer: flush and ask again
		spin(500);
		_Write(kSwrFifoCommand, 1);
		_Write(kSwrReadCommand, command);
		spin(250);
	}
	TRACE("%s SoundWire: read %#x of device %" B_PRIu32 " unanswered (FIFO "
		"%#" B_PRIx32 ", interrupts %#" B_PRIx32 ")\n", fLabel, reg, device,
		_Read(kSwrFifoStatus), _Read(kSwrInterruptStatus));
	return -1;
}


//!	qcom_swrm_cmd_fifo_wr_cmd(): no answer comes back.
status_t
SoundWire::Write(uint32 device, uint16 reg, uint8 value)
{
	if (fBase == NULL)
		return B_NO_INIT;
	// room in the write FIFO (swrm_wait_for_wr_fifo_avail())
	for (int i = 0; (_Read(kSwrFifoStatus) >> 8 & 0x1f) >= 8; i++) {
		if (i == 30)
			return B_TIMED_OUT;
		spin(500);
	}
	fCommandID = fCommandID < 14 ? fCommandID + 1 : 0;
	_Write(kSwrWriteCommand,
		reg | fCommandID << 16 | device << 20 | (uint32)value << 24);
	return B_OK;
}


uint32
SoundWire::_Read(uint32 offset) const
{
	return read32(fBase, offset);
}


void
SoundWire::_Write(uint32 offset, uint32 value)
{
	write32(fBase, offset, value);
}


// #pragma mark - Codec


Codec::Codec()
	:
	fRxArea(-1),
	fRx(NULL),
	fTxArea(-1),
	fTx(NULL),
	fVaArea(-1),
	fVa(NULL),
	fRxBus("RX"),
	fTxBus("TX"),
	fRxDevice(0),
	fTxDevice(0),
	fPowered(false)
{
}


Codec::~Codec()
{
	PowerDown();
}


status_t
Codec::PowerUp(Dsp& dsp)
{
	status_t status = dsp.Vote(kVoteMacro, true);
	if (status == B_OK)
		status = dsp.Vote(kVoteDcodec, true);
	for (size_t i = 0; status == B_OK && i < B_COUNT_OF(kClocks); i++)
		status = dsp.SetClock(kClocks[i], kMclkRate);
	if (status != B_OK) {
		TRACE("the DSP did not give us the LPASS macros' clocks: %s\n",
			strerror(status));
		return status;
	}

	status = _Map("qcom audio rx macro", kRxMacro, B_PAGE_SIZE, fRxArea, fRx);
	if (status == B_OK)
		status = _Map("qcom audio tx macro", kTxMacro, B_PAGE_SIZE, fTxArea, fTx);
	if (status == B_OK)
		status = _Map("qcom audio va macro", kVaMacro, B_PAGE_SIZE, fVaArea, fVa);
	if (status != B_OK) {
		PowerDown();
		return status;
	}
	_ClockMacros();
	_SetPins(3, 4);
	_SetPins(0, 1);

	// the TX bus' reset (lpasscc LPASS_AUDIO_SWR_TX_CGCR: 0xc010 bit 1)
	volatile uint8* lpassCc;
	area_id area = map("qcom audio lpasscc", kLpassCc, 0x12000, lpassCc);
	if (area >= 0) {
		set32(lpassCc, 0xc010, 2);
		spin(10);
		clear32(lpassCc, 0xc010, 2);
		delete_area(area);
	}

	status = fRxBus.Start(kRxBus);
	if (status == B_OK)
		status = fTxBus.Start(kTxBus);
	if (status != B_OK) {
		PowerDown();
		return status;
	}
	_ResetCodec();
	fRxDevice = fRxBus.FindDevice(kRxDeviceID, 1000000);
	fTxDevice = fTxBus.FindDevice(kTxDeviceID, 1000000);
	if (fRxDevice == 0 || fTxDevice == 0) {
		TRACE("the WCD9385 did not attach (RX device %" B_PRIu32 ", TX device %"
			B_PRIu32 ")\n", fRxDevice, fTxDevice);
		PowerDown();
		return B_DEV_NOT_READY;
	}

	// WCD938X_DIGITAL_CHIP_ID2/3 (0x010d), EFUSE_REG_0's variant (bits 4:1)
	int chip = Read(0x3403) | Read(0x3404) << 8;
	int variant = Read(0x34b0) >> 1 & 0xf;
	TRACE("WCD938x chip %#x, variant %d%s\n", chip, variant,
		variant == 5 ? " (WCD9385)" : "");
	if (chip != 0x010d) {
		PowerDown();
		return B_DEV_NOT_READY;
	}
	fPowered = true;
	return B_OK;
}


void
Codec::PowerDown()
{
	fRxBus.Stop();
	fTxBus.Stop();
	fRxDevice = fTxDevice = 0;
	area_id* areas[] = { &fRxArea, &fTxArea, &fVaArea };
	for (area_id* area : areas) {
		if (*area >= 0)
			delete_area(*area);
		*area = -1;
	}
	fRx = fTx = fVa = NULL;
	fPowered = false;
}


int
Codec::Read(uint16 reg)
{
	return fTxDevice != 0 ? fTxBus.Read(fTxDevice, reg) : -1;
}


status_t
Codec::Write(uint16 reg, uint8 value)
{
	return fTxDevice != 0 ? fTxBus.Write(fTxDevice, reg, value) : B_NO_INIT;
}


status_t
Codec::Update(uint16 reg, uint8 mask, uint8 value)
{
	int current = Read(reg);
	if (current < 0)
		return B_IO_ERROR;
	return Write(reg, (current & ~mask) | (value & mask));
}


status_t
Codec::_Map(const char* name, phys_addr_t address, size_t size,
	area_id& area, volatile uint8*& base)
{
	area = map(name, address, size, base);
	return area >= 0 ? B_OK : area;
}


/*!	The macros' clocks, as their drivers turn them on: the RX macro's MCLK,
	frame counter and SoundWire clock (lpass-rx-macro.c); the VA macro's
	frame sync generator, and its SoundWire clock, which drives the TX bus
	on this SoC (lpass-va-macro.c, sm8450_va_data: has_swr_master); the TX
	macro's MCLK and SoundWire clock (lpass-tx-macro.c).
*/
void
Codec::_ClockMacros()
{
	// CDC_RX_CLK_RST_CTRL_MCLK (MCLK, MCLK2), _FS_CNT, _SWR_CONTROL (in a
	// reset)
	set32(fRx, 0x100, 3);
	clear32(fRx, 0x104, 2);
	set32(fRx, 0x104, 1);
	set32(fRx, 0x108, 2);
	spin(100);
	clear32(fRx, 0x108, 2);
	set32(fRx, 0x108, 1);

	// CDC_VA_CLK_RST_CTRL_MCLK, _FS_CNT, CDC_VA_TOP_CSR_TOP_CFG0's frame
	// sync broadcast; SWR_MIC_CTL0-2 undivided; _SWR_CONTROL in a reset
	set32(fVa, 0x00, 1);
	set32(fVa, 0x04, 1);
	set32(fVa, 0x80, 2);
	for (uint32 reg = 0xd0; reg <= 0xd8; reg += 4)
		write32(fVa, reg, (read32(fVa, reg) & ~0xeeu) | 0xcc);
	set32(fVa, 0x08, 2);
	set32(fVa, 0x08, 1);
	spin(100);
	clear32(fVa, 0x08, 2);

	// CDC_TX_TOP_CSR_FREQ_MCLK, CDC_TX_CLK_RST_CTRL_MCLK, _FS_CNT,
	// _SWR_CONTROL (in a reset)
	set32(fTx, 0x90, 1);
	set32(fTx, 0x00, 1);
	set32(fTx, 0x04, 1);
	set32(fTx, 0x08, 2);
	set32(fTx, 0x08, 1);
	spin(100);
	clear32(fTx, 0x08, 2);
}


/*!	A SoundWire bus' pins on the LPASS LPI pin controller
	(pinctrl-sc8280xp-lpass-lpi.c: a page a pin; bits 1:0 pull, 5:2
	function, 8:6 drive strength): the clock pin without a pull, the data
	pins with bus hold, function 1, 2 mA.
*/
void
Codec::_SetPins(uint32 clockPin, uint32 firstDataPin)
{
	volatile uint8* pins;
	area_id area = map("qcom audio lpi pins", kLpiPins, 0x20000, pins);
	if (area < 0)
		return;
	write32(pins, clockPin * 0x1000, 1u << 2);
	write32(pins, firstDataPin * 0x1000, 2 | 1u << 2);
	write32(pins, (firstDataPin + 1) * 0x1000, 2 | 1u << 2);
	delete_area(area);
}


//!	The codec's reset line (TLMM GPIO 106, active low), pulsed.
void
Codec::_ResetCodec()
{
	volatile uint8* gpio;
	area_id area = map("qcom audio wcd reset",
		kTlmm + kCodecResetGpio * 0x1000, B_PAGE_SIZE, gpio);
	if (area < 0)
		return;
	uint32 config = read32(gpio, 0);
	uint32 io = read32(gpio, 4);
	TRACE("codec reset GPIO: config %#" B_PRIx32 ", in/out %#" B_PRIx32 "\n",
		config, io);
	if ((config & (1u << 9)) != 0 && (io & 2) != 0) {
		// the firmware let it out of reset already
		delete_area(area);
		return;
	}
	write32(gpio, 4, 0);
	write32(gpio, 0, 1u << 9);		// GPIO function, an output, low
	snooze(20000);
	write32(gpio, 4, 2);
	delete_area(area);
}


}	// namespace qcom_audio
