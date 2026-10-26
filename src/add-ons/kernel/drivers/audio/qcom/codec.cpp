/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "codec.h"

#include <stdio.h>
#include <string.h>

#include <KernelExport.h>
#include <driver_settings.h>

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
	kSwrFrameControl = 0x101c,		// + 0x40 a bank
	kSwrBusControl = 0x1044,
	kSwrMcpConfig = 0x1048,
	kSwrMcpStatus = 0x104c,			// bit 0: the bank in use
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


status_t
SoundWire::_Broadcast(uint16 reg, uint8 value)
{
	_Write(kSwrInterruptClear, 1u << 10);
	// device 15, command ID 15 (SWR_BROADCAST_CMD_ID)
	_Write(kSwrWriteCommand, reg | 0xfu << 16 | 0xfu << 20 | (uint32)value << 24);
	// SWRM_INTERRUPT_STATUS_SPECIAL_CMD_ID_FINISHED
	for (int i = 0; i < 100; i++) {
		if ((_Read(kSwrInterruptStatus) & (1u << 10)) != 0) {
			_Write(kSwrInterruptClear, 1u << 10);
			return B_OK;
		}
		snooze(100);
	}
	return B_TIMED_OUT;
}


/*!	sdw_bank_switch() with qcom_swrm_pre_bank_switch(): the frame shape in
	the other bank's frame control, then the devices' SCP_FrameCtrl of that
	bank written by broadcast, which switches all of them at the end of the
	frame.
*/
status_t
SoundWire::_BankSwitch()
{
	uint32 next = (_Read(kSwrMcpStatus) & 1) ^ 1;
	_Write(kSwrFrameControl + 0x40 * next, 7 | 1u << 3);
	// (whether the controller says the broadcast finished or not, the bank
	// in use tells)
	_Broadcast(next != 0 ? 0x70 : 0x60, 7 | 1u << 3);
	status_t status = B_OK;
	for (int i = 0; (_Read(kSwrMcpStatus) & 1) != next; i++) {
		if (i == 100) {
			status = B_TIMED_OUT;
			break;
		}
		snooze(100);
	}
	if (status != B_OK)
		TRACE("%s SoundWire: no bank switch: %s\n", fLabel, strerror(status));
	return status;
}


//!	One bank's transport and port parameters, for the controller and device.
void
SoundWire::_ProgramPorts(uint32 device, const SoundWirePort* ports,
	const uint8* channelMasks, uint32 portCount, uint32 bank, bool enable)
{
	for (uint32 i = 0; i < portCount; i++) {
		const SoundWirePort& port = ports[i];
		uint32 n = i + 1;
		uint32 base = 0x100 * (n - 1) + 0x40 * bank;
		if (channelMasks[i] == 0)
			continue;
		// the controller: qcom_swrm_transport_params(), _port_params(),
		// _port_enable() (SWRM_DP_PORT_CTRL_BANK and the rest)
		uint32 control = port.offset1 << 8 | port.offset2 << 16
			| port.sampleInterval;
		if (enable)
			control |= (uint32)channelMasks[i] << 24;
		_Write(0x1124 + base, control);
		if (port.lane != 0xff)
			_Write(0x1128 + base, port.lane);
		if (port.blockGroupCount != 0xff)
			_Write(0x1130 + base, port.blockGroupCount);
		_Write(0x1134 + base, port.hstart != 0xff && port.hstop != 0xff
			? port.hstop << 4 | port.hstart : 0xf << 4);
		if (port.blockPackingMode != 0xff)
			_Write(0x1138 + base, port.blockPackingMode);
		if (port.wordLength != 0xff)
			_Write(0x112c + 0x100 * (n - 1), port.wordLength);

		// the device (sdw_program_slave_port_params(); its ports are
		// "simple" ones with lane control): isochronous, normal data
		uint16 dpn = 0x100 * n;
		uint16 bankOffset = bank != 0 ? 0x10 : 0;
		int portControl = Read(device, dpn + 0x02);
		if (portControl >= 0)
			Write(device, dpn + 0x02, portControl & 0xf0);
		if (port.wordLength != 0xff)
			Write(device, dpn + 0x03, port.wordLength);
		Write(device, dpn + 0x22 + bankOffset, port.sampleInterval);
		Write(device, dpn + 0x24 + bankOffset, port.offset1);
		Write(device, dpn + 0x28 + bankOffset,
			port.lane != 0xff ? port.lane : 0);
		Write(device, dpn + 0x20 + bankOffset,
			enable ? channelMasks[i] : 0);
	}
}


status_t
SoundWire::SetPorts(uint32 device, const SoundWirePort* ports,
	const uint8* channelMasks, uint32 portCount, bool enable)
{
	if (fBase == NULL)
		return B_NO_INIT;
	if (!enable) {
		// sdw_disable_stream(): channels off in the other bank, switch;
		// the ports unprepared
		uint32 next = (_Read(kSwrMcpStatus) & 1) ^ 1;
		_ProgramPorts(device, ports, channelMasks, portCount, next, false);
		status_t status = _BankSwitch();
		for (uint32 i = 0; i < portCount; i++) {
			if (channelMasks[i] != 0)
				Write(device, 0x100 * (i + 1) + 0x05, 0);
		}
		return status;
	}

	// sdw_prepare_stream(): the parameters in the other bank, switch, the
	// device's ports prepared (simple channel prepare state machines: no
	// waiting)
	uint32 next = (_Read(kSwrMcpStatus) & 1) ^ 1;
	_ProgramPorts(device, ports, channelMasks, portCount, next, false);
	status_t status = _BankSwitch();
	if (status != B_OK)
		return status;
	for (uint32 i = 0; i < portCount; i++) {
		if (channelMasks[i] != 0)
			Write(device, 0x100 * (i + 1) + 0x05, channelMasks[i]);
	}
	// sdw_enable_stream(): again in the other bank with the channels on,
	// switch
	next ^= 1;
	_ProgramPorts(device, ports, channelMasks, portCount, next, true);
	status = _BankSwitch();
	TRACE("%s SoundWire: ports %s (bank %" B_PRIu32 ", port 1 control %#"
		B_PRIx32 ")\n", fLabel, status == B_OK ? "running" : "not running",
		_Read(kSwrMcpStatus) & 1,
		_Read(0x1124 + 0x40 * (_Read(kSwrMcpStatus) & 1)));
	return status;
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
	fRxStride(0x80),
	fRxStride2(0xc),
	fPowered(false),
	fHeadphones(false)
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

	// the macros' version (va_macro_set_lpass_codec_version()): from 2.5 on
	// the RX interpolators' registers are 0xc0 apart
	uint32 core0 = read32(fVa, 0xc0), core1 = read32(fVa, 0xc4),
		core2 = read32(fVa, 0xc8);
	bool v25 = core0 == 2 && core1 == 0xf && core2 >= 0x50;
	fRxStride = v25 ? 0xc0 : 0x80;
	fRxStride2 = v25 ? 0 : 0xc;
	TRACE("LPASS codec core ID %#" B_PRIx32 " %#" B_PRIx32 " %#" B_PRIx32
		"\n", core0, core1, core2);
	// rx_macro_component_probe(): the DSM output delay, the DC coefficient
	for (uint32 interpolator = 0; interpolator < 3; interpolator++) {
		_RxUpdate(_RxPath(interpolator, 0x434), 7, 2);
		_RxUpdate(0x410 + fRxStride * interpolator, 3, 2);
	}

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
	_InitCodec();
	fPowered = true;
	return B_OK;
}


void
Codec::PowerDown()
{
	DisableHeadphones();
	DisableHeadphonePath();
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


/*!	As the RX macro's DAPM path from AIF1 to HPHL_OUT and HPHR_OUT powers
	up (lpass-rx-macro.c, with the UCM's headphone settings: RX INT0/1_1
	MIX1 INP0 = RX0/RX1, RX INT0/1 DEM MUX = CLSH_DSM_OUT), then unmuted.
*/
status_t
Codec::EnableHeadphonePath(uint32 sampleRate)
{
	if (!fPowered)
		return B_NO_INIT;
	// sr_val_tbl
	uint32 rate = sampleRate == 44100 ? 9 : sampleRate == 96000 ? 5 : 4;

	// the interpolators' first mixer inputs: RX0 (5) and RX1 (6)
	_RxUpdate(0x180, 0xf, 5);
	_RxUpdate(0x188, 0xf, 6);
	// class H for both (CDC_RX_CLSH_*: its clock, K1 for 16 ohm, the HPH
	// minimum PA voltage, the decay rate)
	_RxUpdate(0x200, 1, 1);
	write32(fRx, 0x220, 0xc0);
	_RxUpdate(0x21c, 0xf, 0);
	_RxUpdate(0x20c, 0x3f, 0x1c);
	_RxUpdate(0x208, 7, 0);
	for (uint32 interpolator = 0; interpolator < 2; interpolator++) {
		uint32 control = 0x400 + fRxStride * interpolator;
		// the DEM input: class H's DSM output, with its look-ahead delay
		_RxUpdate(0x408 + fRxStride * interpolator, 1, 1);
		_RxUpdate(0x404 + fRxStride * interpolator, 1u << 3, 1u << 3);
		_RxUpdate(control, 0xf, rate);
		// rx_macro_enable_interp_clk(): muted, the DSM clock, the HPF
		// cut-off, the compander's LUT bypassed, class H on
		_RxUpdate(control, 1u << 4, 1u << 4);
		_RxUpdate(_RxPath(interpolator, 0x440), 1, 1);
		_RxUpdate(0x40c + fRxStride * interpolator, 3, 3);
		_RxUpdate(interpolator == 0 ? 0x28 : 0x3c, 1u << 7, 1u << 7);
		_RxUpdate(0x404 + fRxStride * interpolator, 1u << 6, 1u << 6);
		// the digital volume (0 dB) written again, as POST_PMU does
		write32(fRx, 0x414 + fRxStride * interpolator,
			read32(fRx, 0x414 + fRxStride * interpolator));
		// rx_macro_digital_mute(): unmuted, the path's clock on
		_RxUpdate(control, 1u << 4, 0);
		_RxUpdate(0x418 + fRxStride * interpolator, 1u << 4, 0);
		_RxUpdate(control, 1u << 5, 1u << 5);
	}
	TRACE("RX macro: headphone path on (RX0/RX1 path control %#" B_PRIx32
		" %#" B_PRIx32 ")\n", read32(fRx, 0x400), read32(fRx, 0x400 + fRxStride));
	return B_OK;
}


void
Codec::DisableHeadphonePath()
{
	if (fRx == NULL)
		return;
	for (uint32 interpolator = 0; interpolator < 2; interpolator++) {
		uint32 control = 0x400 + fRxStride * interpolator;
		_RxUpdate(control, 1u << 4, 1u << 4);
		_RxUpdate(_RxPath(interpolator, 0x440), 1, 0);
		_RxUpdate(control, 1u << 5, 0);
		// reset, back to 48 kHz
		_RxUpdate(control, 1u << 6, 1u << 6);
		_RxUpdate(control, 1u << 6, 0);
		_RxUpdate(control, 0xf, 4);
		_RxUpdate(0x404 + fRxStride * interpolator, 1u << 6, 0);
	}
	_RxUpdate(0x200, 1, 0);
	_RxUpdate(0x180, 0xf, 0);
	_RxUpdate(0x188, 0xf, 0);
}


// the WCD938x's registers (wcd938x.h)
enum {
	kWcdAnaBias = 0x3001,
	kWcdRxSupplies = 0x3008,
	kWcdAnaHph = 0x3009,
	kWcdBiasVbgFineAdjust = 0x3029,
	kWcdClassHMode2 = 0x3098,
	kWcdClassHMode3 = 0x3099,
	kWcdFlybackVnegControl1 = 0x30a5,
	kWcdFlybackVnegControl4 = 0x30a8,
	kWcdFlybackVnegDacControl2 = 0x30af,
	kWcdRxBiasHphLowPower = 0x30bf,
	kWcdRxBiasFlybackBuffer = 0x30c7,
	kWcdHphPaControl1 = 0x30d1,
	kWcdHphLeft = 0x30d3,
	kWcdHphRight = 0x30d6,
	kWcdHphRdacClockControl1 = 0x30d9,
	kWcdHphRefBufferUhqa = 0x30dd,
	kWcdHphSurge = 0x30e2,
	kWcdSleepControl = 0x3103,
	kWcdAuxPa = 0x3128,
	kWcdLdoRxTxConfig = 0x312a,
	kWcdRdacGainControl = 0x3132,
	kWcdRdacHd2Left = 0x3133,
	kWcdRdacHd2Right = 0x3136,
	kWcdHphTimer1 = 0x313a,
	kWcdPaMisc2 = 0x3138,
	kWcdRdacHd2LeftNew = 0x3140,
	kWcdRdacHd2RightNew = 0x3141,
	kWcdAnaClockControl = 0x3408,
	kWcdDigitalClockControl = 0x3409,
	kWcdRx0Control = 0x340d,
	kWcdCompControl0 = 0x3414,
	kWcdHphGainControl = 0x344e,
	kWcdPdmWatchdogLeft = 0x3465,
	kWcdPdmWatchdogRight = 0x3466,
};

// the WCD9385's RX device's ports on the RX bus (the device tree's
// qcom,ports-* of soundwire@3210000; wcd938x_sdw_ch_info: HPH on port 1,
// left and right, class H on port 2)
static const SoundWirePort kRxPorts[] = {
	{ 0x03, 0x00, 0x00, 0xff, 0xff, 0x01, 0xff, 0x01, 0xff },
	{ 0x1f, 0x00, 0x00, 0x03, 0x06, 0x07, 0xff, 0x00, 0xff },
};
static const uint8 kHeadphoneChannels[] = { 0x3, 0x1 };


//!	wcd938x_io_init(): its sleep and bias set up once after the reset.
void
Codec::_InitCodec()
{
	Update(kWcdSleepControl, 0x0e, 0x0e);
	Update(kWcdSleepControl, 0x80, 0x80);
	snooze(1000);
	Update(kWcdSleepControl, 0x40, 0x40);
	snooze(1000);
	Update(kWcdLdoRxTxConfig, 0x10, 0x00);
	Update(kWcdBiasVbgFineAdjust, 0xf0, 0x80);
	Update(kWcdAnaBias, 0x80, 0x80);
	Update(kWcdAnaBias, 0x40, 0x40);
	snooze(10000);
	Update(kWcdAnaBias, 0x40, 0x00);
	Update(kWcdRdacGainControl, 0xf0, 0x00);
	Update(kWcdRdacHd2LeftNew, 0x1f, 0x15);
	Update(kWcdRdacHd2RightNew, 0x1f, 0x15);
	Update(kWcdHphRefBufferUhqa, 0xc0, 0x80);
	Update(kWcdHphSurge, 0xc0, 0xc0);
}


/*!	wcd-clsh-v2.c's v3 state machine for the headphones in class H HiFi:
	the buck and the flyback (VPOS, VNEG) on, in HiFi mode.
*/
void
Codec::_ClassH(bool enable)
{
	if (enable) {
		Update(kWcdRxSupplies, 0x02, 0x00);			// buck regulator mode
		Update(kWcdRxSupplies, 0x04, 0x04);			// flyback HiFi
		Update(kWcdFlybackVnegControl4, 0xf0, 0x80);
		Update(kWcdFlybackVnegDacControl2, 0xe0, 0xa0);	// force IQ
		snooze(100);
		Update(kWcdClassHMode3, 0x02, 0x02);
		Update(kWcdClassHMode2, 0xff, 0x1c);
		Update(kWcdFlybackVnegControl1, 0xe0, 0xe0);	// the flyback on
		Update(kWcdRxSupplies, 1u << 6, 1u << 6);
		snooze(100);
		Update(kWcdFlybackVnegDacControl2, 0xe0, 0xe0);
		snooze(500);
		Update(kWcdRxBiasFlybackBuffer, 0x0f, 0x0a);	// its current
		Update(kWcdRxBiasFlybackBuffer, 0xf0, 0xa0);
		snooze(100);
		Update(kWcdRxSupplies, 0x08, 0x08);			// buck HiFi
		Update(kWcdRxSupplies, 1u << 7, 1u << 7);		// the buck on
		snooze(500);
		Update(kWcdClassHMode3, 0x02, 0x00);
		Update(kWcdClassHMode2, 0xff, 0x3a);
		snooze(500);
		Update(kWcdAnaHph, 0x0c, 0x08);				// HPH HiFi
	} else {
		Update(kWcdAnaHph, 0x0c, 0x00);
		Update(kWcdFlybackVnegControl1, 0xe0, 0xe0);
		Update(kWcdRxSupplies, 1u << 6, 0);
		snooze(100);
		Update(kWcdFlybackVnegDacControl2, 0xe0, 0xe0);
		snooze(500);
		Update(kWcdRxSupplies, 1u << 7, 0);
		snooze(500);
		Update(kWcdClassHMode2, 0xff, 0x3a);
		snooze(500);
		Update(kWcdPaMisc2, 0x20, 0x00);
		Update(kWcdRxBiasHphLowPower, 0xf0, 0x80);
		Update(kWcdHphPaControl1, 0x0e, 0x06);
		Update(kWcdRxSupplies, 0x04, 0x00);
		Update(kWcdFlybackVnegControl4, 0xf0, 0x70);
		Update(kWcdRxSupplies, 0x08, 0x00);
	}
}


/*!	The WCD938x's DAPM path IN1_HPHL/IN2_HPHR to HPHL/HPHR powered up in
	DAPM's order: the RX clocks (RXCLK), the DACs (RDAC1/2), the
	amplifiers (HPHL/R PGA) with class H; without the compander.
*/
status_t
Codec::EnableHeadphones(uint8 gain)
{
	if (!fPowered)
		return B_NO_INIT;
	if (fHeadphones)
		return B_OK;
	status_t status = fRxBus.SetPorts(fRxDevice, kRxPorts, kHeadphoneChannels,
		B_COUNT_OF(kRxPorts), true);
	if (status != B_OK)
		return status;

	// RXCLK: the analog RX clock, the RX bias, no DEM dither, its divider
	Update(kWcdAnaClockControl, 1, 1);
	Update(kWcdRxSupplies, 1, 1);
	for (uint16 reg = kWcdRx0Control; reg < kWcdRx0Control + 3; reg++)
		Update(reg, 1u << 6, 0);
	Update(kWcdAnaClockControl, 2, 2);
	Update(kWcdAuxPa, 1u << 4, 1u << 4);

	// RDAC1/2 before power up: the RX digital clocks, the HPH inputs, no
	// chopper clock
	Update(kWcdDigitalClockControl, 3, 3);
	Update(kWcdHphGainControl, 3u << 2, 3u << 2);
	Update(kWcdHphRdacClockControl1, 1u << 7, 0);

	// HPHL/R PGA before power up: class H, the references, the PDM
	// watchdogs
	_ClassH(true);
	Update(kWcdAnaHph, 3u << 4, 3u << 4);
	snooze(100);
	Update(kWcdPdmWatchdogLeft, 7, 3);
	Update(kWcdPdmWatchdogRight, 7, 3);
	// the gain from the register, then the amplifiers on
	Update(kWcdHphLeft, 0x1f, gain);
	Update(kWcdHphRight, 0x1f, gain);
	Update(kWcdAnaHph, 3u << 6, 3u << 6);

	// RDAC1/2 after power up: the HD2 divider, no compander
	Update(kWcdRdacHd2Left, 0x1f, 0x02);
	Update(kWcdRdacHd2Right, 0x1f, 0x02);
	Update(kWcdCompControl0, 3, 0);
	Update(kWcdHphLeft, 1u << 5, 1u << 5);
	Update(kWcdHphRight, 1u << 5, 1u << 5);

	// HPHL/R PGA after power up: 20 ms without the compander, then the
	// auto chop timer
	snooze(20000);
	Update(kWcdHphTimer1, 1u << 1, 1u << 1);
	fHeadphones = true;
	TRACE("WCD938x headphones on (HPH %#x, RX supplies %#x)\n",
		Read(kWcdAnaHph), Read(kWcdRxSupplies));
	return B_OK;
}


void
Codec::DisableHeadphones()
{
	if (!fHeadphones)
		return;
	// the amplifiers off after 20 ms (without the compander), then the
	// rest in the reverse order
	snooze(20000);
	Update(kWcdAnaHph, 3u << 6, 0);
	snooze(21000);
	Update(kWcdPdmWatchdogLeft, 7, 0);
	Update(kWcdPdmWatchdogRight, 7, 0);
	Update(kWcdAnaHph, 3u << 4, 0);
	_ClassH(false);
	Update(kWcdRdacHd2Left, 0x1f, 0x01);
	Update(kWcdRdacHd2Right, 0x1f, 0x01);
	Update(kWcdHphGainControl, 3u << 2, 0);
	Update(kWcdDigitalClockControl, 3, 0);
	Update(kWcdRxSupplies, 3u << 6, 0);
	Update(kWcdRxSupplies, 1, 0);
	Update(kWcdAnaClockControl, 3, 0);
	fRxBus.SetPorts(fRxDevice, kRxPorts, kHeadphoneChannels,
		B_COUNT_OF(kRxPorts), false);
	fHeadphones = false;
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


//!	An RX interpolator's register in the block whose spacing changes after
//!	interpolator 1 (CDC_RX_RXn_RX_PATH_SEC7, _DSM_CTL).
uint32
Codec::_RxPath(uint32 interpolator, uint32 offset) const
{
	return offset + fRxStride * interpolator
		+ (interpolator > 1 ? fRxStride2 : 0);
}


void
Codec::_RxUpdate(uint32 offset, uint32 mask, uint32 value)
{
	write32(fRx, offset, (read32(fRx, offset) & ~mask) | (value & mask));
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


namespace qcom_audio {


/*	The TX macro as Linux has it while recording from the headset input
	(regmap of 3220000.txmacro during arecord on this board, the UCM's TX DEC0
	MUX SWR_MIC, TX SMIC MUX0 ADC1, TX_AIF1_CAP Mixer DEC0, TX_DEC0 Volume
	110): its MCLK, frame sync counter and SoundWire clock; the MCLK
	frequency, the I2S and SoundWire microphone controls; decimator 0's
	input (SWR_MIC, ADC1), its path configuration and volume; its path last
	(enabled, 48 kHz). Where its reset values differ from the defaults
	Linux' regcache_sync() writes once the MCLK is on (SWR_CTRL, the
	SoundWire analog microphone controls, decimator 0's SEC0), Linux'. */
static const struct { uint16 offset; uint8 value; } kTxMacroMic[] = {
	{0x000, 0x01}, {0x004, 0x01}, {0x008, 0x01}, {0x090, 0x01},
	{0x088, 0x00}, {0x0d0, 0x00}, {0x0d4, 0x00}, {0x410, 0x00},
	{0x0a4, 0x0c}, {0x0c0, 0x0e}, {0x0c4, 0x0e}, {0x0c8, 0x0e},
	{0x0cc, 0x0e}, {0x100, 0x02}, {0x104, 0x01}, {0x404, 0x10},
	{0x408, 0x0b}, {0x40c, 0x1a}, {0x418, 0x01}, {0x41c, 0x3c},
	{0x420, 0x20}, {0x42c, 0x0a}, {0x400, 0x24},
};


// the WCD9385's TX device's port on the TX bus (the device tree's
// qcom,ports-* of soundwire@3330000: port 1, sample interval 1, offset 1,
// lane 0; wcd938x_sdw_ch_info: ADC2 is its second channel)
static const SoundWirePort kTxPorts[] = {
	{ 0x01, 0x01, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0xff },
};
static const uint8 kHeadsetMicChannels[] = { 0x2 };

/*	The WCD938x's TX side as Linux sets it up (its registers while recording
	that differ from ours, but the headphone path's, status and interrupts):
	TX channel 2 (ADC2), the headset detection (MBHC: its mechanism, button
	thresholds, plug detection, the microphone bias it gates), the
	microphone bias test controls, the TX reference and front end currents,
	the TX analog clocks, the request and DMIC controls and the TX request
	feedback. */
static const struct { uint16 reg; uint8 value; } kWcdTxInit[] = {
	{0x3014, 0x8d}, {0x301a, 0x18}, {0x301b, 0x30}, {0x301c, 0x48},
	{0x301d, 0xa0}, {0x301e, 0xa0}, {0x301f, 0xa0}, {0x3020, 0xa0},
	{0x3021, 0xa0}, {0x306b, 0xfa}, {0x306e, 0xfa}, {0x3071, 0xfa},
	{0x3074, 0xfa}, {0x3081, 0x0a}, {0x3089, 0x0a}, {0x308f, 0xfa},
	{0x3120, 0x82}, {0x3121, 0x06}, {0x3122, 0xe6}, {0x3124, 0x03},
	{0x31b1, 0x08}, {0x31dd, 0xd5}, {0x31de, 0xca}, {0x31df, 0x08},
	{0x31e1, 0x14}, {0x3417, 0x1f}, {0x3457, 0x02}, {0x345b, 0x06},
	{0x34d0, 0x55}, {0x34d1, 0x44}, {0x34d2, 0x11}, {0x34d3, 0x00},
	{0x34d4, 0x00}, {0x300f, 0x0a},
};

/*	The WCD938x registers Linux changes when recording starts from the
	headset input (idle against recording, the same boot): the microphone
	bias, the TX channel, the analog and digital clocks, the ADC power. */
static const struct { uint16 reg; uint8 value; } kWcdHeadsetMic[] = {
	{0x3408, 0x18}, {0x3409, 0xf0}, {0x3410, 0x30}, {0x3015, 0x89},
	{0x3023, 0x50},
};


status_t
Codec::EnableHeadsetMic(uint32 sampleRate)
{
	if (!fPowered)
		return B_NO_INIT;
	for (const auto& setting : kWcdTxInit)
		Write(setting.reg, setting.value);
	for (const auto& setting : kWcdHeadsetMic) {
		int before = Read(setting.reg);
		Write(setting.reg, setting.value);
		TRACE("WCD938x %#x: %#x -> %#x\n", setting.reg, before, Read(setting.reg));
	}
	status_t portStatus = fTxBus.SetPorts(fTxDevice, kTxPorts,
		kHeadsetMicChannels, B_COUNT_OF(kTxPorts), true);
	if (portStatus != B_OK)
		TRACE("TX SoundWire: ports: %s\n", strerror(portStatus));
	char changed[256] = "";
	size_t length = 0;
	for (const auto& setting : kTxMacroMic) {
		uint32 value = read32(fTx, setting.offset);
		if (value != setting.value && length + 16 < sizeof(changed)) {
			length += snprintf(changed + length, sizeof(changed) - length,
				" %03x:%" B_PRIx32 ">%x", setting.offset, value, setting.value);
		}
		write32(fTx, setting.offset, setting.value);
	}
	TRACE("TX macro: decimator 0 from the SoundWire microphone on, set:%s\n",
		changed);
	// lab ("dump_macros"): the TX and VA macros' registers, as Linux'
	// regmap prints them, to compare
	void* settings = load_driver_settings("qcom_audio");
	bool dump = get_driver_boolean_parameter(settings, "dump_macros", false,
		true);
	unload_driver_settings(settings);
	if (dump) {
		volatile uint8* macros[2] = {fTx, fVa};
		const char* names[2] = {"tx", "va"};
		for (int m = 0; m < 2; m++) {
			for (uint32 offset = 0; offset < 0x800; offset += 0x40) {
				char line[200];
				size_t at = 0;
				for (uint32 i = 0; i < 0x40; i += 4) {
					at += snprintf(line + at, sizeof(line) - at, " %" B_PRIx32,
						read32(macros[m], offset + i));
				}
				TRACE("dump %s %03" B_PRIx32 ":%s\n", names[m], offset, line);
			}
		}
		// the WCD938x's registers (through its TX device)
		for (uint32 reg = 0x3000; reg < 0x3500; reg += 16) {
			char line[100];
			size_t at = 0;
			for (uint32 i = 0; i < 16; i++)
				at += snprintf(line + at, sizeof(line) - at, " %x", Read(reg + i));
			TRACE("dump wcd %04" B_PRIx32 ":%s\n", reg, line);
		}
	}
	return B_OK;
}


void
Codec::DisableHeadsetMic()
{
	if (fTx == NULL)
		return;
	fTxBus.SetPorts(fTxDevice, kTxPorts, kHeadsetMicChannels,
		B_COUNT_OF(kTxPorts), false);
	// decimator 0's path off, its input disconnected
	write32(fTx, 0x400, 0x04);
	write32(fTx, 0x100, 0);
	write32(fTx, 0x104, 0);
}


}	// namespace qcom_audio
