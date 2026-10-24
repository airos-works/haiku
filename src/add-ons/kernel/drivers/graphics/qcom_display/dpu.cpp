/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "dpu.h"

#include <KernelExport.h>


#define ERROR(x...)	dprintf("qcom_display: " x)
#define INFO(x...)	dprintf("qcom_display: " x)


namespace qcom {


// MDSS 0, with the DPU ("mdp") at +0x1000
static const phys_addr_t kMdssBase = 0xae00000;
static const size_t kMdssSize = 0x90000;
static const uint32 kMdp = 0x1000;

// What must be on before the MDSS may be read: GCC's display AHB branch,
// then dispcc 0's MDSS power domain and its AHB and MDP clocks. Reading an
// unclocked block hangs the SoC.
static const phys_addr_t kGccBase = 0x100000;
static const uint32 kGccDisplayAhb = 0x27004;
static const phys_addr_t kDispccBase = 0xaf00000;
static const uint32 kDispccMdssGdsc = 0x3000;
static const uint32 kDispccAhb = 0x20bc;
static const uint32 kDispccMdp = 0x200c;

// DPU interrupt registers (dpu_hwio.h), MDP_SSPP_TOP0_INTR
static const uint32 kInterruptEnable = kMdp + 0x10;
static const uint32 kInterruptStatus = kMdp + 0x14;
static const uint32 kInterruptClear = kMdp + 0x18;

// control paths (CTL_0..5)
static const uint32 kControls = kMdp + 0x15000;
static const uint32 kControlCount = 6;
enum {
	CTL_FLUSH			= 0x018,
	CTL_INTF_FLUSH		= 0x110,
	CTL_INTF_ACTIVE		= 0x0f4,
	CTL_FETCH_PIPE_ACTIVE = 0x0fc,
};
static const uint32 kControlFlush = 1u << 17;
static const uint32 kInterfaceFlush = 1u << 31;

static const uint32 kMixers = kMdp + 0x44000;
enum {
	LM_OUT_SIZE			= 0x04,
};

// layer mixers: CTL_LAYER(lm) and their flush bits
static const uint32 kMixerLayer[] = {0x00, 0x04, 0x08, 0x0c, 0x10, 0x24};
static const uint32 kMixerFlush[] = {1u << 6, 1u << 7, 1u << 8, 1u << 9,
	1u << 10, 1u << 20};

// source pipes: base, CTL_FETCH_PIPE_ACTIVE bit, flush bit
static const struct {
	uint32	base;
	uint32	fetch;
	uint32	flush;
	const char* name;
} kPipes[] = {
	{kMdp + 0x4000, 1u << 16, 1u << 0, "VIG0"},
	{kMdp + 0x6000, 1u << 17, 1u << 1, "VIG1"},
	{kMdp + 0x8000, 1u << 18, 1u << 2, "VIG2"},
	{kMdp + 0xa000, 1u << 19, 1u << 18, "VIG3"},
	{kMdp + 0x24000, 1u << 0, 1u << 11, "DMA0"},
	{kMdp + 0x26000, 1u << 1, 1u << 12, "DMA1"},
	{kMdp + 0x28000, 1u << 2, 1u << 24, "DMA2"},
	{kMdp + 0x2a000, 1u << 3, 1u << 25, "DMA3"},
};
enum {
	SSPP_SRC_SIZE		= 0x00,
	SSPP_SRC_XY			= 0x08,
	SSPP_OUT_SIZE		= 0x0c,
	SSPP_OUT_XY			= 0x10,
	SSPP_SRC0_ADDR		= 0x14,
	SSPP_SRC_YSTRIDE0	= 0x24,
};

// the DisplayPort timing engines: INTF number and vertical blank bit
static const struct {
	uint32	number;
	uint32	blank;
} kInterfaces[] = {
	{0, 1u << 25}, {4, 1u << 21}, {5, 1u << 23}, {6, 1u << 17},
};
static const uint32 kInterfaceBase = kMdp + 0x34000;
static const uint32 kInterfaceStride = 0x1000;
enum {
	INTF_TIMING_ENGINE_EN	= 0x000,
	INTF_HSYNC_CTL			= 0x008,
	INTF_VSYNC_PERIOD_F0	= 0x00c,
	INTF_VSYNC_PULSE_WIDTH_F0 = 0x014,
	INTF_DISPLAY_V_START_F0	= 0x01c,
	INTF_DISPLAY_V_END_F0	= 0x024,
	INTF_CONFIG				= 0x004,
	INTF_ACTIVE_V_START_F0	= 0x02c,
	INTF_ACTIVE_V_END_F0	= 0x034,
	INTF_DISPLAY_HCTL		= 0x03c,
	INTF_ACTIVE_HCTL		= 0x040,
	INTF_BORDER_COLOR		= 0x044,
	INTF_HSYNC_SKEW			= 0x04c,
	INTF_POLARITY_CTL		= 0x050,
	INTF_CONFIG2			= 0x060,
	INTF_DISPLAY_DATA_HCTL	= 0x064,
	INTF_ACTIVE_DATA_HCTL	= 0x068,
	INTF_FRAME_LINE_COUNT_EN = 0x0a8,
	INTF_FRAME_COUNT		= 0x0ac,
};
static const uint32 kDataBusWiden = 1u << 0;
static const uint32 kDataHctlEnable = 1u << 4;
static const uint32 kActiveHEnable = 1u << 29;
static const uint32 kActiveVEnable = 1u << 30;
static const uint32 kProgrammableFetch = 1u << 31;


/*!	Bit 31 of a clock controller register: a branch's CBCR reads it set
	while the clock is off, a GDSC while the domain is on.
*/
static bool
status_bit(phys_addr_t base, uint32 offset)
{
	volatile uint32* reg;
	area_id area = map_physical_memory("qcom display clock check",
		base + (offset & ~(B_PAGE_SIZE - 1)), B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA, (void**)&reg);
	if (area < 0)
		return false;
	uint32 value = reg[(offset & (B_PAGE_SIZE - 1)) / 4];
	delete_area(area);
	return (value & (1u << 31)) != 0;
}


Dpu::Dpu()
	:
	fArea(-1),
	fMdss(NULL),
	fInterface(0),
	fInterfaceBase(0),
	fControlBase(0),
	fPipeBase(0),
	fPipeFlush(0),
	fMixerBase(0),
	fMixerFlush(0),
	fWideBus(false),
	fBlankBit(0),
	fTiming()
{
}


Dpu::~Dpu()
{
	if (fArea >= 0) {
		EnableBlankInterrupt(false);
		delete_area(fArea);
	}
}


bool
Dpu::_PoweredUp()
{
	// GCC's branch first: dispcc's registers sit behind it
	return !status_bit(kGccBase, kGccDisplayAhb)
		&& status_bit(kDispccBase, kDispccMdssGdsc)
		&& !status_bit(kDispccBase, kDispccAhb)
		&& !status_bit(kDispccBase, kDispccMdp);
}


status_t
Dpu::Init()
{
	if (!_PoweredUp()) {
		ERROR("MDSS 0 is not powered or clocked; left alone\n");
		return B_DEVICE_NOT_FOUND;
	}

	fArea = map_physical_memory("qcom mdss", kMdssBase, kMdssSize,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fMdss);
	if (fArea < 0)
		return fArea;

	INFO("MDSS %#" B_PRIx32 ", DPU %#" B_PRIx32 "\n", _Read(0x0),
		_Read(kMdp + 0x0));
	return _FindPath();
}


/*!	The timing engine that runs, the control path that drives it, and the
	source pipe that path fetches.
*/
status_t
Dpu::_FindPath()
{
	for (size_t i = 0; i < B_COUNT_OF(kInterfaces); i++) {
		uint32 base = kInterfaceBase + kInterfaceStride * kInterfaces[i].number;
		if ((_Read(base + INTF_TIMING_ENGINE_EN) & 1) == 0)
			continue;
		for (uint32 control = 0; control < kControlCount; control++) {
			uint32 controlBase = kControls + 0x1000 * control;
			if ((_Read(controlBase + CTL_INTF_ACTIVE)
					& (1u << kInterfaces[i].number)) == 0) {
				continue;
			}
			uint32 fetch = _Read(controlBase + CTL_FETCH_PIPE_ACTIVE);
			int32 pipe = -1;
			for (size_t p = 0; p < B_COUNT_OF(kPipes); p++) {
				if ((fetch & kPipes[p].fetch) != 0) {
					pipe = p;
					break;
				}
			}
			int32 mixer = -1;
			for (size_t m = 0; m < B_COUNT_OF(kMixerLayer); m++) {
				if (_Read(controlBase + kMixerLayer[m]) != 0) {
					mixer = m;
					break;
				}
			}
			if (pipe < 0 || mixer < 0)
				continue;

			fInterface = kInterfaces[i].number;
			fInterfaceBase = base;
			fControlBase = controlBase;
			fPipeBase = kPipes[pipe].base;
			fPipeFlush = kPipes[pipe].flush;
			fMixerBase = kMixers + 0x1000 * mixer;
			fMixerFlush = kMixerFlush[mixer];
			fBlankBit = kInterfaces[i].blank;
			_ReadTiming();
			if (pipe < 4) {
				// a VIG pipe's QSEED3 scaler (+0xa00): OP_MODE, source and
				// destination sizes
				uint32 scaler = fPipeBase + 0xa00;
				INFO("%s scaler: op %#" B_PRIx32 " src %#" B_PRIx32 " dst %#"
					B_PRIx32 " phase step %#" B_PRIx32 " %#" B_PRIx32 "\n",
					kPipes[pipe].name, _Read(scaler + 0x04),
					_Read(scaler + 0x40), _Read(scaler + 0x48),
					_Read(scaler + 0x10), _Read(scaler + 0x14));
			}
			INFO("the firmware's path: %s -> LM_%" B_PRId32 " -> CTL_%" B_PRIu32
				" -> INTF_%" B_PRIu32 ", %ux%u, %" B_PRIu32 " kHz, buffer %#"
				B_PRIx32 "\n", kPipes[pipe].name, mixer, control, fInterface,
				fTiming.h_display, fTiming.v_display, fTiming.pixel_clock,
				_Read(fPipeBase + SSPP_SRC0_ADDR));
			return B_OK;
		}
	}
	ERROR("no lit display path\n");
	return B_DEVICE_NOT_FOUND;
}


/*!	The mode back from the timing engine's registers, which
	dpu_hw_intf_setup_timing_engine() wrote for DisplayPort: the front porches
	folded into the back porches, and with the wide data bus every horizontal
	value halved. The refresh rate is counted (INTF_FRAME_COUNT).
*/
void
Dpu::_ReadTiming()
{
	fWideBus = (_Read(fInterfaceBase + INTF_CONFIG2) & kDataBusWiden) != 0;
	uint32 scale = fWideBus ? 2 : 1;
	uint32 hsync = _Read(fInterfaceBase + INTF_HSYNC_CTL);
	uint32 hPeriod = hsync >> 16;
	uint32 hPulse = hsync & 0xffff;
	// for DisplayPort the display window is the active one; the firmware
	// leaves INTF_ACTIVE_HCTL 0
	uint32 activeH = _Read(fInterfaceBase + INTF_ACTIVE_HCTL);
	if (activeH == 0)
		activeH = _Read(fInterfaceBase + INTF_DISPLAY_HCTL);
	uint32 hStart = activeH & 0xffff;
	uint32 hEnd = activeH >> 16;
	uint32 vTotal = hPeriod != 0
		? _Read(fInterfaceBase + INTF_VSYNC_PERIOD_F0) / hPeriod : 0;
	uint32 vPulse = hPeriod != 0
		? _Read(fInterfaceBase + INTF_VSYNC_PULSE_WIDTH_F0) / hPeriod : 0;
	uint32 vStart = hPeriod != 0
		? _Read(fInterfaceBase + INTF_DISPLAY_V_START_F0) / hPeriod : 0;
	uint32 vEnd = hPeriod != 0
		? _Read(fInterfaceBase + INTF_DISPLAY_V_END_F0) / hPeriod : 0;
	uint32 polarity = _Read(fInterfaceBase + INTF_POLARITY_CTL);

	display_timing& timing = fTiming;
	timing.h_total = hPeriod * scale;
	timing.h_display = (hEnd - hStart + 1) * scale;
	uint32 hFront = (hPeriod - 1 - hEnd) * scale;
	timing.h_sync_start = timing.h_display + hFront;
	timing.h_sync_end = timing.h_sync_start + hPulse * scale;
	timing.v_total = vTotal;
	timing.v_display = vEnd >= vStart ? vEnd - vStart + 1 : 0;
	uint32 vFront = vTotal - 1 - vEnd;
	timing.v_sync_start = timing.v_display + vFront;
	timing.v_sync_end = timing.v_sync_start + vPulse;
	// polarity 1 = active low
	timing.h_sync_positive = (polarity & 1) == 0;
	timing.v_sync_positive = (polarity & 2) == 0;

	// frames counted over about a quarter of a second, from a frame's start
	_Write(fInterfaceBase + INTF_FRAME_LINE_COUNT_EN, 0x3);
	uint32 frames = 0;
	bigtime_t start = 0, end = 0;
	uint32 count = _Read(fInterfaceBase + INTF_FRAME_COUNT);
	bigtime_t timeout = system_time() + 500000;
	while (system_time() < timeout) {
		uint32 now = _Read(fInterfaceBase + INTF_FRAME_COUNT);
		if (now != count) {
			if (start == 0)
				start = system_time();
			else {
				frames++;
				end = system_time();
				if (end - start > 250000)
					break;
			}
			count = now;
		}
		spin(50);
	}
	if (frames > 0 && end > start) {
		uint64 hz1000 = (uint64)frames * 1000000000ull / (end - start);
		// pixel clock in kHz: totals times the rate
		timing.pixel_clock = (uint32)((uint64)timing.h_total * timing.v_total
			* hz1000 / 1000000);
	} else {
		// no frames counted: assume 60 Hz
		timing.pixel_clock = timing.h_total * timing.v_total * 60 / 1000;
	}
}


status_t
Dpu::SetScanout(phys_addr_t address, uint32 bytesPerRow, uint32 width,
	uint32 height)
{
	if (fMdss == NULL || fPipeBase == 0)
		return B_NO_INIT;
	// the pipe fetches 32-bit addresses; it does not scale here, so the
	// region is no larger than the mode
	if (address + (phys_addr_t)bytesPerRow * height > (1ull << 32)
		|| width == 0 || height == 0 || width > fTiming.h_display
		|| height > fTiming.v_display || bytesPerRow < width * 4
		|| bytesPerRow > 0xffff || (address & 0xf) != 0) {
		return B_BAD_VALUE;
	}

	uint32 size = height << 16 | width;
	_Write(fPipeBase + SSPP_SRC_SIZE, size);
	_Write(fPipeBase + SSPP_SRC_XY, 0);
	_Write(fPipeBase + SSPP_OUT_SIZE, size);
	_Write(fPipeBase + SSPP_OUT_XY, 0);
	_Write(fPipeBase + SSPP_SRC0_ADDR, (uint32)address);
	_Write(fPipeBase + SSPP_SRC_YSTRIDE0, bytesPerRow);
	// the pipe takes it at the next vertical blank
	_Write(fControlBase + CTL_FLUSH, fPipeFlush | fMixerFlush | kControlFlush);
	return B_OK;
}


void
Dpu::StopTiming()
{
	if (fMdss == NULL || fInterfaceBase == 0)
		return;
	_Write(fInterfaceBase + INTF_TIMING_ENGINE_EN, 0);
	// let the frame under way end (Linux waits a frame's time and more)
	uint32 rate = fTiming.h_total != 0 && fTiming.v_total != 0
		? (uint32)((uint64)fTiming.pixel_clock * 1000 / fTiming.h_total
			/ fTiming.v_total) : 60;
	snooze(2 * 1000000 / (rate > 0 ? rate : 60));
}


/*!	The timing engine's registers for \a timing, as
	dpu_encoder_phys_vid_setup_timing_engine() and
	dpu_hw_intf_setup_timing_engine() write them for DisplayPort: the front
	porches folded into the back porches, with the wide data bus every
	horizontal value halved, the window shifted to the bottom right. The
	mixer and, with SetScanout(), the pipe follow.
*/
status_t
Dpu::SetTiming(const display_timing& timing)
{
	if (fMdss == NULL || fInterfaceBase == 0)
		return B_NO_INIT;
	uint32 divider = fWideBus ? 2 : 1;
	if (timing.h_display % divider != 0 || timing.h_display == 0
		|| timing.v_display == 0 || timing.h_total <= timing.h_display
		|| timing.v_total <= timing.v_display) {
		return B_BAD_VALUE;
	}

	uint32 width = timing.h_display / divider;
	uint32 hPulse = (timing.h_sync_end - timing.h_sync_start) / divider;
	uint32 hBack = (timing.h_total - timing.h_sync_end
		+ timing.h_sync_start - timing.h_display) / divider;
	uint32 height = timing.v_display;
	uint32 vPulse = timing.v_sync_end - timing.v_sync_start;
	uint32 vBack = timing.v_total - timing.v_sync_end + timing.v_sync_start
		- timing.v_display;

	uint32 hPeriod = hPulse + hBack + width;
	uint32 vPeriod = vPulse + vBack + height;
	uint32 displayVStart = (vPulse + vBack) * hPeriod;
	uint32 displayVEnd = vPeriod * hPeriod - 1;
	uint32 hStartX = hBack + hPulse;

	// DisplayPort: the active window from the start of the data on
	displayVStart += hPulse + hBack;
	uint32 activeHEnd = hStartX + width - 1;
	uint32 activeHctl = activeHEnd << 16 | hStartX;
	uint32 activeVStart = displayVStart;
	uint32 activeVEnd = activeVStart + height * hPeriod - 1;

	// polarity 1 = active low
	uint32 polarity = (timing.v_sync_positive ? 0 : 2)
		| (timing.h_sync_positive ? 0 : 1);

	uint32 config = _Read(fInterfaceBase + INTF_CONFIG) | kActiveHEnable
		| kActiveVEnable;
	config &= ~kProgrammableFetch;
	uint32 config2 = _Read(fInterfaceBase + INTF_CONFIG2) | kDataHctlEnable;
	if (fWideBus)
		config2 |= kDataBusWiden;

	_Write(fInterfaceBase + INTF_HSYNC_CTL, hPeriod << 16 | hPulse);
	_Write(fInterfaceBase + INTF_VSYNC_PERIOD_F0, vPeriod * hPeriod);
	_Write(fInterfaceBase + INTF_VSYNC_PULSE_WIDTH_F0, vPulse * hPeriod);
	_Write(fInterfaceBase + INTF_DISPLAY_HCTL, activeHctl);
	_Write(fInterfaceBase + INTF_DISPLAY_V_START_F0, displayVStart);
	_Write(fInterfaceBase + INTF_DISPLAY_V_END_F0, displayVEnd);
	_Write(fInterfaceBase + INTF_ACTIVE_HCTL, activeHctl);
	_Write(fInterfaceBase + INTF_ACTIVE_V_START_F0, activeVStart);
	_Write(fInterfaceBase + INTF_ACTIVE_V_END_F0, activeVEnd);
	_Write(fInterfaceBase + INTF_BORDER_COLOR, 0);
	_Write(fInterfaceBase + INTF_HSYNC_SKEW, 0);
	_Write(fInterfaceBase + INTF_POLARITY_CTL, polarity);
	_Write(fInterfaceBase + INTF_FRAME_LINE_COUNT_EN, 0x3);
	_Write(fInterfaceBase + INTF_CONFIG, config);
	_Write(fInterfaceBase + INTF_CONFIG2, config2);
	_Write(fInterfaceBase + INTF_DISPLAY_DATA_HCTL,
		(hStartX + width - 1) << 16 | hStartX);
	_Write(fInterfaceBase + INTF_ACTIVE_DATA_HCTL, 0);

	_Write(fMixerBase + LM_OUT_SIZE,
		(uint32)timing.v_display << 16 | timing.h_display);
	// the pipe no larger than the mixer until SetScanout() gives it a region
	uint32 size = _Read(fPipeBase + SSPP_SRC_SIZE);
	uint32 pipeWidth = min_c(size & 0xffff, (uint32)timing.h_display);
	uint32 pipeHeight = min_c(size >> 16, (uint32)timing.v_display);
	size = pipeHeight << 16 | pipeWidth;
	_Write(fPipeBase + SSPP_SRC_SIZE, size);
	_Write(fPipeBase + SSPP_OUT_SIZE, size);
	fTiming = timing;
	return B_OK;
}


void
Dpu::StartTiming()
{
	if (fMdss == NULL || fInterfaceBase == 0)
		return;
	// mixer, pipe and timing engine together
	_Write(fControlBase + CTL_INTF_FLUSH, 1u << fInterface);
	_Write(fControlBase + CTL_FLUSH, fPipeFlush | fMixerFlush | kControlFlush
		| kInterfaceFlush);
	_Write(fInterfaceBase + INTF_TIMING_ENGINE_EN, 1);
}


void
Dpu::LogState(const char* when)
{
	if (fMdss == NULL || fInterfaceBase == 0)
		return;
	// INTF_6's underrun bit is the one below its vertical blank's
	uint32 underrun = fBlankBit >> 1;
	_Write(kInterruptClear, underrun);
	uint32 frames = _Read(fInterfaceBase + INTF_FRAME_COUNT);
	snooze(100000);
	frames = _Read(fInterfaceBase + INTF_FRAME_COUNT) - frames;
	INFO("%s: INTF_%" B_PRIu32 " %s, %" B_PRIu32 " frames in 100 ms, line %"
		B_PRIu32 ", underrun %s; LM %#" B_PRIx32 ", pipe %#" B_PRIx32 " -> %#"
		B_PRIx32 " stride %#" B_PRIx32 "\n", when, fInterface,
		(_Read(fInterfaceBase + INTF_TIMING_ENGINE_EN) & 1) != 0
			? "on" : "off", frames, _Read(fInterfaceBase + 0xb0),
		(_Read(kInterruptStatus) & underrun) != 0 ? "yes" : "no",
		_Read(fMixerBase + LM_OUT_SIZE), _Read(fPipeBase + SSPP_SRC_SIZE),
		_Read(fPipeBase + SSPP_OUT_SIZE), _Read(fPipeBase + SSPP_SRC_YSTRIDE0));
}


void
Dpu::EnableBlankInterrupt(bool enable)
{
	if (fMdss == NULL || fBlankBit == 0)
		return;
	uint32 mask = _Read(kInterruptEnable);
	if (enable) {
		_Write(kInterruptClear, fBlankBit);
		mask |= fBlankBit;
	} else
		mask &= ~fBlankBit;
	_Write(kInterruptEnable, mask);
}


bool
Dpu::AcknowledgeBlank()
{
	if (fMdss == NULL || (_Read(kInterruptStatus) & fBlankBit) == 0)
		return false;
	_Write(kInterruptClear, fBlankBit);
	return true;
}


status_t
Dpu::DebugRegister(uint64 address, uint32& value, bool write)
{
	if (fMdss == NULL || address < kMdssBase
		|| address + 4 > kMdssBase + kMdssSize || (address & 3) != 0) {
		return B_BAD_VALUE;
	}
	uint32 offset = address - kMdssBase;
	if (write)
		_Write(offset, value);
	else
		value = _Read(offset);
	return B_OK;
}


}	// namespace qcom
