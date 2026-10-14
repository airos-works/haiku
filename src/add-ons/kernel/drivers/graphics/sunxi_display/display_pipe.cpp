/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "display_pipe.h"

#include <string.h>

#include <KernelExport.h>
#include <util/kernel_cpp.h>

#include "cadence_dp_phy.h"
#include "registers.h"


#define TRACE(x...)	dprintf("sunxi_display: " x)


namespace sunxi {


// #pragma mark - addresses


// clock controller (24 MHz reference for the PLL arithmetic)
static const phys_addr_t kPllVideo1		= 0x02002140;
static const phys_addr_t kPllDe			= 0x020022e0;
static const phys_addr_t kIommu1Clock	= 0x020025b4;
static const phys_addr_t kAhbGates		= 0x020025c0;	// keyed
static const phys_addr_t kMbusGates		= 0x020025e0;	// keyed
static const uint32 kAhbKey				= 0x010000ff;
static const uint32 kMbusKey			= 0x41055800;
static const phys_addr_t kDeClock		= 0x02002a00;
static const phys_addr_t kDeBus			= 0x02002a04;
static const phys_addr_t kDeSysReset	= 0x02002a74;
static const phys_addr_t kSerdesClock	= 0x020033c0;
static const phys_addr_t kSerdesReset	= 0x020033c4;
static const phys_addr_t kTconTv1Bus	= 0x0200360c;
static const phys_addr_t kEdpTvClock	= 0x02003640;
	// the pixel clock of TCON-TV1 and of the DP controller: one clock
static const phys_addr_t kEdpBus		= 0x0200364c;
static const phys_addr_t kDpssTop1		= 0x020036cc;
static const phys_addr_t kVideoOut1Reset = 0x020036ec;

static const phys_addr_t kPpuClock		= 0x070101ac;	// R-CCU
static const phys_addr_t kDcxoSerdes	= 0x0709016c;	// RTC
static const phys_addr_t kPck600		= 0x07060000;
static const uint32 kDomainDeSys		= 1;
static const uint32 kDomainVo1			= 10;

static const phys_addr_t kDeIommuBypass	= 0x03910804;	// IOMMU1, master 8

// display engine 3.5: top, video channels 0 and 1, disp0 blender, format,
// colour matrix. The frame buffer goes through video channel 1, whose
// scaler (VSU8) has the classic DE3 layout; channel 0 has a different one.
static const phys_addr_t kDeTop			= 0x05008000;
static const phys_addr_t kVch0			= 0x05100000;
static const phys_addr_t kVch0Overlay	= kVch0 + 0x1000;
static const phys_addr_t kVch1			= 0x05120000;
static const phys_addr_t kPlaneCsc		= kVch1 + 0x800;
static const phys_addr_t kPlaneOverlay	= kVch1 + 0x1000;
static const phys_addr_t kPlaneScaler	= kVch1 + 0x4000;
static const phys_addr_t kBlender0		= 0x05281000;
static const phys_addr_t kFormatter0	= 0x05285000;
static const phys_addr_t kColorMatrix0	= 0x05289000;

static const phys_addr_t kTconTop1		= 0x05510000;
static const phys_addr_t kTconTv1		= 0x05731000;

// DisplayPort transmitter
static const phys_addr_t kDp			= 0x05740000;
static const phys_addr_t kDpTop			= 0x05760000;
enum {
	DP_LINK_BW_SET			= 0x000,
	DP_LANE_COUNT_SET		= 0x004,
	DP_ENHANCED_FRAME		= 0x008,
	DP_TRAINING_PATTERN		= 0x00c,
	DP_SCRAMBLING_DISABLE	= 0x014,
	DP_CAPABILITY_CONFIG	= 0x01c,
	DP_TRANSMITTER_ENABLE	= 0x080,
	DP_SOFT_RESET			= 0x090,
	DP_INPUT_SOURCE			= 0x094,
	DP_CORE_ID				= 0x0fc,
	DP_AUX_COMMAND			= 0x100,
	DP_AUX_WRITE_FIFO		= 0x104,
	DP_AUX_ADDRESS			= 0x108,
	DP_AUX_CLOCK_DIVIDER	= 0x10c,
	DP_AUX_REPLY_TIMEOUT	= 0x110,
	DP_INTERRUPT_STATE		= 0x130,
	DP_AUX_REPLY_DATA		= 0x134,
	DP_AUX_REPLY_CODE		= 0x138,
	DP_INTERRUPT_CAUSE		= 0x140,
	DP_INTERRUPT_MASK		= 0x144,
	DP_AUX_REPLY_COUNT		= 0x148,
	DP_AUX_STATUS			= 0x14c,
	DP_GP_TIMER				= 0x158,
	DP_VIDEO_ENABLE			= 0x800,
	DP_SECOND_STREAM_ENABLE	= 0x804,
	DP_SECOND_DATA_WINDOW	= 0x808,
	DP_HTOTAL				= 0x820,
	DP_VTOTAL				= 0x824,
	DP_POLARITY				= 0x828,
	DP_HSYNC_WIDTH			= 0x82c,
	DP_VSYNC_WIDTH			= 0x830,
	DP_HRES					= 0x834,
	DP_VRES					= 0x838,
	DP_HSTART				= 0x83c,
	DP_VSTART				= 0x840,
	DP_MISC0				= 0x844,
	DP_MVID					= 0x84c,
	DP_TU_CONFIG			= 0x850,
	DP_NVID					= 0x854,
	DP_PIXEL_COUNT			= 0x858,
	DP_DATA_COUNT			= 0x85c,
	DP_INTERLACE			= 0x860,
};

enum {
	AUX_I2C_WRITE			= 0x0,
	AUX_I2C_READ			= 0x1,
	AUX_I2C_WRITE_MOT		= 0x4,
	AUX_I2C_READ_MOT		= 0x5,
	AUX_NATIVE_WRITE		= 0x8,
	AUX_NATIVE_READ			= 0x9,
};

// combo PHY 0 and the AUX/HPD PHY
static const phys_addr_t kSerdesTop		= 0x06c00000;
static const phys_addr_t kPhyLink		= 0x06c01000;
static const phys_addr_t kAuxPhy		= 0x06c01e00;
static const phys_addr_t kComboTop		= 0x06c06000;
static const phys_addr_t kPhyRegisters	= 0x06c80000;	// 16-bit words
enum {
	LINK_CTRL0	= 0x000,
	LINK_CTRL1	= 0x004,
	LINK_CTRL2	= 0x008,
	LINK_CTRL3	= 0x00c,
	LINK_STUS0	= 0x900,
};
#define PHY_WORD(n)	(kPhyRegisters + ((phys_addr_t)(n) << 1))


static const uint8 kEdidHeader[8]
	= { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };

// what is shown when the sink's EDID cannot be read or used (CEA VIC 16)
static const display_timing k1080p60 = {
	148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, true, true
};


/*	The channel's scaler (VSU8) stalls the pipe as programmed below: with
	it on, even at 1:1, TCON-TV1 underflows and the output is black. Until
	that is understood, regions other than the output's size are shown
	unscaled, and the accelerant offers no other sizes. */
static const bool kUseScaler = false;

/*	A 4-tap Lanczos (a = 2) filter in 32 phases, the taps at -1, 0, +1 and
	+2 in bytes 0..3, adding up to 64; generated for this driver, it matches
	the table Allwinner's scaler driver uses for enlarging. */
static const uint32 kLanczos2[32] = {
	0x00004000, 0x000140ff, 0x00033ffe, 0x00053efd,
	0x00063efc, 0x00083cfc, 0xff0a3cfb, 0xff0d39fb,
	0xff0f37fb, 0xff1135fb, 0xfe1433fb, 0xfe1631fb,
	0xfe192efb, 0xfd1c2cfb, 0xfd1f29fb, 0xfc2127fc,
	0xfc2424fc, 0xfc2721fc, 0xfb291ffd, 0xfb2c1cfd,
	0xfb2e19fe, 0xfb3116fe, 0xfb3314fe, 0xfb3511ff,
	0xfb370fff, 0xfb390dff, 0xfb3c0aff, 0xfc3c0800,
	0xfc3e0600, 0xfd3e0500, 0xfe3f0300, 0xff400100,
};


// #pragma mark - registers


DisplayPipe::DisplayPipe()
	:
	fMappingCount(0),
	fTconInterrupts(NULL),
	fInitialized(false),
	fEnabled(false),
	fFlipped(false),
	fLinkRate(0),
	fLanes(0),
	fWidth(0),
	fHeight(0)
{
	memset(fDpcd, 0, sizeof(fDpcd));
}


DisplayPipe::~DisplayPipe()
{
	for (int32 i = 0; i < fMappingCount; i++)
		delete_area(fMappings[i].area);
}


status_t
DisplayPipe::_Map(phys_addr_t physical, size_t size)
{
	if (fMappingCount == (int32)B_COUNT_OF(fMappings))
		return B_NO_MEMORY;
	Mapping& mapping = fMappings[fMappingCount];
	mapping.area = map_physical_memory("sunxi display registers", physical,
		size, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&mapping.address);
	if (mapping.area < 0)
		return mapping.area;
	mapping.physical = physical;
	mapping.size = size;
	fMappingCount++;
	return B_OK;
}


volatile void*
DisplayPipe::_Address(phys_addr_t physical) const
{
	for (int32 i = 0; i < fMappingCount; i++) {
		const Mapping& mapping = fMappings[i];
		if (physical >= mapping.physical
			&& physical < mapping.physical + mapping.size) {
			return mapping.address + (physical - mapping.physical);
		}
	}
	panic("sunxi_display: register %#" B_PRIxPHYSADDR " is not mapped",
		physical);
	return NULL;
}


uint32
DisplayPipe::_Read(phys_addr_t physical) const
{
	return *(volatile uint32*)_Address(physical);
}


void
DisplayPipe::_Write(phys_addr_t physical, uint32 value)
{
	*(volatile uint32*)_Address(physical) = value;
	memory_full_barrier();
}


void
DisplayPipe::_Update(phys_addr_t physical, uint32 clear, uint32 set)
{
	volatile uint32* address = (volatile uint32*)_Address(physical);
	*address = (*address & ~clear) | set;
	memory_full_barrier();
}


uint16
DisplayPipe::_Read16(phys_addr_t physical) const
{
	return *(volatile uint16*)_Address(physical);
}


void
DisplayPipe::_Write16(phys_addr_t physical, uint16 value)
{
	*(volatile uint16*)_Address(physical) = value;
	memory_full_barrier();
}


/*!	The AHB and MBUS master gates take a key with every write; the key bits
	read back as 0. As the BSP's clock driver writes them.
*/
void
DisplayPipe::_SetKeyedGate(phys_addr_t physical, uint32 key, uint32 bit)
{
	uint32 value = _Read(physical);
	if ((value & (1u << bit)) == 0)
		_Write(physical, value | key | (1u << bit));
}


// #pragma mark - power and clocks


/*!	Switches a PCK-600 power domain on (the BSP's sunxi_pd_init and
	sunxi_do_pmu_set_power_domain).
*/
status_t
DisplayPipe::_PowerOn(uint32 domain)
{
	phys_addr_t ppu = kPck600 + domain * 0x1000;
	if ((_Read(ppu + 0x8) & 0xf) == 0x8)
		return B_OK;

	_Write(ppu + 0x170, 0x001f1f1f);
	_Write(ppu + 0x174, 0x00001f1f);
	_Write(ppu + 0xc00, 0x08080808);
	_Write(ppu + 0xc04, 0x00000808);
	_Write(ppu + 0xc10, 0x00000008);

	_Update(ppu, 0xf, 0x8);
	bigtime_t deadline = system_time() + 10000;
	while ((_Read(ppu + 0x8) & 0xf) != 0x8) {
		if (system_time() > deadline) {
			TRACE("power domain %" B_PRIu32 " does not come up: %#" B_PRIx32
				"\n", domain, _Read(ppu + 0x8));
			return B_TIMED_OUT;
		}
		spin(10);
	}
	TRACE("power domain %" B_PRIu32 " on\n", domain);
	return B_OK;
}


/*!	Programs a PLL of the CCU's video layout (PLL_VIDEO1, PLL_DE): N in
	[15:8], the "4X" post divider in [22:20], the "3X" one in [18:16].
*/
static status_t
program_pll(volatile uint32* pll, uint32 n,
	uint32 divider4x, uint32 divider3x)
{
	*pll &= ~((1u << 27) | (1u << 26));
	uint32 value = *pll;
	value &= ~((0xffu << 8) | (7u << 20) | (7u << 16));
	value |= ((n - 1) << 8) | ((divider4x - 1) << 20)
		| ((divider3x - 1) << 16);
	*pll = value;
	if ((*pll & (1u << 31)) == 0) {
		*pll |= 1u << 30;
		*pll |= 1u << 31;
	}
	*pll &= ~(1u << 29);
	*pll |= 1u << 29;
	memory_full_barrier();

	int32 locked = 0;
	for (int32 i = 0; i < 100 && locked < 3; i++) {
		spin(3);
		locked = (*pll & (1u << 28)) != 0 ? locked + 1 : 0;
	}
	if (locked < 3)
		return B_TIMED_OUT;
	spin(20);
	*pll |= (1u << 27) | (1u << 26);
	memory_full_barrier();
	return B_OK;
}


status_t
DisplayPipe::_EnableDisplayEngineClocks()
{
	volatile uint32* pllDe = (volatile uint32*)_Address(kPllDe);
	if ((*pllDe & (1u << 31)) == 0) {
		// 24 MHz x 50 = 1200 MHz, "3X" output / 2 = 600 MHz for the DE
		status_t status = program_pll(pllDe, 50, 1, 2);
		if (status != B_OK) {
			TRACE("PLL_DE does not lock\n");
			return status;
		}
	}

	_Update(kDeSysReset, 0, 1u << 16);
	_Update(kDeBus, 0, 1u << 16);
	_SetKeyedGate(kAhbGates, kAhbKey, 5);
	_SetKeyedGate(kMbusGates, kMbusKey, 11);
	_Update(kDeBus, 0, 1u << 0);
	_Write(kDeClock, 0x80000000);
	return B_OK;
}


/*!	edp-tv = PLL_VIDEO1 "4X" / M / P, with PLL_VIDEO1 = 24 MHz x N / D4.
	The clock clocks both TCON-TV1 and the DP controller's pixel side.
*/
status_t
DisplayPipe::_SetPixelClock(uint32 kHz)
{
	uint32 n = 0, d4 = 0, m = 0, p = 0;
	if (kHz == 148500) {
		// what Linux uses: VCO 2376 MHz, / 8 = 297 MHz, / 2
		n = 99; d4 = 8; m = 2; p = 1;
	} else {
		// the closest clock, within 0.5 % (the DisplayPort stream's M/N
		// values are measured, so the display sees the clock as it is)
		uint64 bestError = (uint64)kHz * 5 / 1000 + 1;
		for (uint32 tryP = 1; tryP <= 32; tryP++) {
			for (uint32 tryM = 1; tryM <= 32; tryM++) {
				for (uint32 tryD4 = 1; tryD4 <= 8; tryD4++) {
					uint64 divider = (uint64)tryD4 * tryM * tryP;
					uint64 tryN = ((uint64)kHz * divider + 12000) / 24000;
					if (tryN < 53 || tryN > 105)
						continue;
					uint64 actual = tryN * 24000 / divider;
					uint64 error = actual > kHz ? actual - kHz : kHz - actual;
					if (error < bestError) {
						bestError = error;
						n = tryN; d4 = tryD4; m = tryM; p = tryP;
					}
				}
			}
		}
		if (n == 0)
			return B_BAD_VALUE;
	}

	volatile uint32* pll = (volatile uint32*)_Address(kPllVideo1);
	status_t status = program_pll(pll, n, d4,
		((*pll >> 16) & 7) + 1);
	if (status != B_OK) {
		TRACE("PLL_VIDEO1 does not lock\n");
		return status;
	}
	_Write(kEdpTvClock, (1u << 31) | (1u << 24) | ((p - 1) << 8) | (m - 1));
	TRACE("pixel clock %" B_PRIu32 " kHz: N %" B_PRIu32 " / %" B_PRIu32
		" / %" B_PRIu32 " / %" B_PRIu32 "\n", kHz, n, d4, m, p);
	return B_OK;
}


// #pragma mark - PHYs


status_t
DisplayPipe::_InitAuxPhy()
{
	_Update(kAuxPhy + 0x00,
		(0xfu << 24) | (3u << 20) | (3u << 16) | (3u << 12) | (1u << 8),
		(3u << 24) | (3u << 20) | (3u << 16) | (1u << 12) | (1u << 8));
	_Update(kAuxPhy + 0x04,
		(1u << 28) | (1u << 24) | (1u << 16) | (0x1fu << 8) | 0x1fu,
		(1u << 16) | (0xeu << 8) | 0xdu);
	_Write(kAuxPhy + 0x08, 0);
	_Update(kAuxPhy + 0x00, 0, 1u << 4);
	_Update(kAuxPhy + 0x00, 0, 1u << 5);
	_Update(kAuxPhy + 0x00, 0, 1u << 0);
	return B_OK;
}


void
DisplayPipe::_PhyAssert()
{
	_Write(kPhyLink + LINK_CTRL0, 0);
	_Write(kComboTop + 0x004, 0);
	_Write(kPhyLink + LINK_CTRL3, 0);
	snooze(1000);
}


static bool
wait_for(volatile uint32* reg, uint32 mask, bool set, int32 polls)
{
	for (int32 i = 0; i < polls; i++) {
		if (((*reg & mask) != 0) == set)
			return true;
		spin(5);
	}
	return false;
}


status_t
DisplayPipe::_PhyDeassert()
{
	_Update(kPhyLink + LINK_CTRL2, 0x3, 0);
	_Write(kPhyLink + LINK_CTRL0, 0x000);
	_Write(kPhyLink + LINK_CTRL0, 0x001);
	snooze(1000);
	_Write(kPhyLink + LINK_CTRL0, 0x101);
	snooze(1000);
	_Write(kPhyLink + LINK_CTRL0, 0x131);
	snooze(1000);
	_Update(kPhyLink + LINK_CTRL0, 1u << 12, fFlipped ? 1u << 12 : 0);
	snooze(1000);

	volatile uint32* status = (volatile uint32*)_Address(kPhyLink + LINK_STUS0);
	if (!wait_for(status, 1u << 0, true, 1000)) {
		TRACE("PHY: PMA common not ready, %#" B_PRIx32 "\n", *status);
		return B_TIMED_OUT;
	}
	_Write(kComboTop + 0x004, 0xff1);
	_Update(kPhyLink + LINK_CTRL3, 0, 1u << 0);
	if (!wait_for(status, 1u << 4, true, 1000)) {
		TRACE("PHY: no link clock, %#" B_PRIx32 "\n", *status);
		return B_TIMED_OUT;
	}
	snooze(1000);
	_Update(kPhyLink + LINK_CTRL3, 0x3fu << 4, 1u << 4);
	if (!wait_for(status, 0x3fu << 8, true, 100000)) {
		TRACE("PHY: power state not acknowledged, %#" B_PRIx32 "\n", *status);
		return B_TIMED_OUT;
	}
	return B_OK;
}


static void
write_table(volatile uint8* base, phys_addr_t physical,
	const cdns_dp_w16* table, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		*(volatile uint16*)(base + (table[i].addr - physical)) = table[i].val;
		memory_full_barrier();
	}
}


/*!	The BSP's phy_configure(): USB/DP configuration for the orientation,
	four lanes, the rate, swing and pre-emphasis level 0.
*/
status_t
DisplayPipe::_PhyConfigure(uint32 rate)
{
	volatile uint8* phy = (volatile uint8*)_Address(kPhyRegisters);
#define TABLE(t) write_table(phy, kPhyRegisters, t, B_COUNT_OF(t))

	_PhyAssert();
	_Update(kPhyLink + LINK_CTRL2, 0x3, 0);
	if (fFlipped)
		TABLE(kCfgUsbDp_Reverse_A);
	else
		TABLE(kCfgUsbDp_Normal_A);
	_Write(kPhyLink + LINK_CTRL1, 0x01100001);
	if (fFlipped)
		TABLE(kCfgUsbDp_Reverse_B);
	else
		TABLE(kCfgUsbDp_Normal_B);
	_Update(kPhyLink + LINK_CTRL0, 1u << 12, fFlipped ? 1u << 12 : 0);
	if (fFlipped)
		TABLE(kCfgUsbDp_Reverse_C);
	else
		TABLE(kCfgUsbDp_Normal_C);
	status_t status = _PhyDeassert();
	if (status != B_OK)
		return status;

	// four lanes
	_Write16(PHY_WORD(0x4011), 0x0100);

	_PhyAssert();
	_Update(kPhyLink + LINK_CTRL2, 0x3, 0);
	_Write(kPhyLink + LINK_CTRL1, 0x01100001);
	if (rate == 540000)
		TABLE(kRate5400);
	else if (rate == 270000)
		TABLE(kRate2700);
	else
		TABLE(kRate1620);
#undef TABLE
	status = _PhyDeassert();
	if (status != B_OK)
		return status;

	static const uint8 kZero[4] = {};
	_PhySetLevels(rate, kZero, kZero);
	return B_OK;
}


void
DisplayPipe::_PhySetLevels(uint32 rate, const uint8* swing,
	const uint8* preEmphasis)
{
	const cdns_dp_level (*table)[4] = rate == 540000 ? kLevel_5g4
		: rate == 270000 ? kLevel_2g7 : kLevel_1g62;

	for (uint32 lane = 0; lane < 4; lane++)
		_Write16(PHY_WORD(0x41e7 + lane * 0x200), 1);
	spin(10);
	for (uint32 lane = 0; lane < 4; lane++)
		_Write16(PHY_WORD(0x4040 + lane * 0x200), 0x08a4);
	for (uint32 lane = 0; lane < 4; lane++) {
		const cdns_dp_level& level = table[swing[lane] & 3][preEmphasis[lane] & 3];
		_Write16(PHY_WORD(0x40c6 + lane * 0x200), level.txdrv[lane]);
		_Write16(PHY_WORD(0x4050 + lane * 0x200), level.mgnfs[lane]);
		_Write16(PHY_WORD(0x404c + lane * 0x200), level.cpost[lane]);
	}
	spin(10);
	for (uint32 lane = 0; lane < 4; lane++)
		_Write16(PHY_WORD(0x41e7 + lane * 0x200), 0);
}


// #pragma mark - DisplayPort controller and AUX


void
DisplayPipe::_InitController()
{
	_Write(kDpTop + 0x00, 1);
	_Write(kDpTop + 0x04, 1);
	_Write(kDpTop + 0x18, 1);
	_Update(kDp + DP_GP_TIMER, 1u << 31, 0);
	_Update(kDp + DP_INTERRUPT_MASK, 0x7f, 0);
	_Update(kDp + DP_CAPABILITY_CONFIG, 1u << 1, 0);
	_Write(kDp + DP_AUX_CLOCK_DIVIDER, 200);
	_Write(kDp + DP_AUX_REPLY_TIMEOUT, 440);
	_Update(kDp + DP_INPUT_SOURCE, 0x7, 0x1);
	_Update(kDp + DP_TRANSMITTER_ENABLE, 0, 1);
	snooze(1000);
}


status_t
DisplayPipe::_AuxWaitReply()
{
	bigtime_t deadline = system_time() + 5000;
	while (true) {
		if ((_Read(kDp + DP_INTERRUPT_STATE) & (1u << 3)) != 0)
			return B_TIMED_OUT;
		uint32 status = _Read(kDp + DP_AUX_STATUS);
		if ((status & (1u << 3)) != 0)
			return B_IO_ERROR;
		if ((status & (1u << 0)) != 0)
			return B_OK;
		if (system_time() > deadline)
			return B_TIMED_OUT;
		spin(10);
	}
}


/*!	One AUX request of up to 16 bytes, retried up to seven times on NACK,
	DEFER, timeouts and errors, 500 us apart (DP CTS 4.2.1.1).
*/
status_t
DisplayPipe::_AuxTransfer(uint32 request, uint32 address, uint8* data,
	size_t length)
{
	if (length > 16)
		return B_BAD_VALUE;
	bool isWrite = request == AUX_NATIVE_WRITE || request == AUX_I2C_WRITE
		|| request == AUX_I2C_WRITE_MOT;

	status_t status = B_ERROR;
	for (int32 attempt = 0; attempt < 8; attempt++) {
		if (attempt > 0)
			snooze(520);

		bigtime_t deadline = system_time() + 5000;
		while ((_Read(kDp + DP_AUX_STATUS) & (1u << 2)) != 0
			&& system_time() < deadline) {
			spin(10);
		}
		// polled: clear what the interrupt handler would have
		_Read(kDp + DP_INTERRUPT_CAUSE);

		_Write(kDp + DP_AUX_ADDRESS, address);
		if (isWrite) {
			for (size_t i = 0; i < length; i++)
				_Write(kDp + DP_AUX_WRITE_FIFO, data[i]);
		}
		uint32 command = request << 8;
		if (length == 0)
			command |= 1u << 12;	// address only
		else
			command |= length - 1;
		_Write(kDp + DP_AUX_COMMAND, command);

		status = _AuxWaitReply();
		if (status != B_OK)
			continue;
		uint32 code = _Read(kDp + DP_AUX_REPLY_CODE) & 0xf;
		if (code != 0) {
			status = code == 2 || code == 8 ? B_BUSY : B_IO_ERROR;
			continue;
		}
		if (!isWrite) {
			uint32 count = _Read(kDp + DP_AUX_REPLY_COUNT);
			for (uint32 i = 0; i < count && i < 16; i++) {
				uint8 byte = _Read(kDp + DP_AUX_REPLY_DATA) & 0xff;
				if (i < length)
					data[i] = byte;
			}
			if (count < length) {
				status = B_IO_ERROR;
				continue;
			}
		}
		return B_OK;
	}
	return status;
}


status_t
DisplayPipe::_DpcdRead(uint32 address, uint8* data, size_t length)
{
	while (length > 0) {
		size_t chunk = length > 16 ? 16 : length;
		status_t status = _AuxTransfer(AUX_NATIVE_READ, address, data, chunk);
		if (status != B_OK)
			return status;
		address += chunk;
		data += chunk;
		length -= chunk;
	}
	return B_OK;
}


status_t
DisplayPipe::_DpcdWrite(uint32 address, const uint8* data, size_t length)
{
	while (length > 0) {
		size_t chunk = length > 16 ? 16 : length;
		uint8 buffer[16];
		memcpy(buffer, data, chunk);
		status_t status = _AuxTransfer(AUX_NATIVE_WRITE, address, buffer,
			chunk);
		if (status != B_OK)
			return status;
		address += chunk;
		data += chunk;
		length -= chunk;
	}
	return B_OK;
}


/*!	EDID blocks 0 and 1 over I2C-over-AUX (address 0x50), as the BSP reads
	them: offset write, then eight 16-byte reads, the last without MOT.
*/
status_t
DisplayPipe::_ReadEdid(uint8* edid, uint32* _length)
{
	uint32 blocks = 1;
	for (uint32 block = 0; block < blocks && block < 2; block++) {
		uint8 offset = block * 128;
		status_t status = _AuxTransfer(AUX_I2C_WRITE_MOT, 0x50, &offset, 1);
		if (status != B_OK)
			return status;
		for (uint32 chunk = 0; chunk < 8; chunk++) {
			status = _AuxTransfer(chunk == 7 ? AUX_I2C_READ : AUX_I2C_READ_MOT,
				0x50, edid + block * 128 + chunk * 16, 16);
			if (status != B_OK)
				return status;
		}
		if (block == 0) {
			if (memcmp(edid, kEdidHeader, sizeof(kEdidHeader)) != 0)
				return B_BAD_DATA;
			blocks = 1 + edid[126];
		}
	}
	*_length = blocks >= 2 ? 256 : 128;
	return B_OK;
}


static bool
parse_detailed_timing(const uint8* d, display_timing& timing)
{
	uint32 pixelClock = (d[0] | (d[1] << 8)) * 10;
	if (pixelClock == 0 || (d[17] & 0x80) != 0)
		return false;	// a descriptor, not a timing, or interlaced

	uint32 hActive = d[2] | ((d[4] & 0xf0) << 4);
	uint32 hBlank = d[3] | ((d[4] & 0x0f) << 8);
	uint32 vActive = d[5] | ((d[7] & 0xf0) << 4);
	uint32 vBlank = d[6] | ((d[7] & 0x0f) << 8);
	uint32 hSyncOffset = d[8] | ((d[11] & 0xc0) << 2);
	uint32 hSyncWidth = d[9] | ((d[11] & 0x30) << 4);
	uint32 vSyncOffset = (d[10] >> 4) | ((d[11] & 0x0c) << 2);
	uint32 vSyncWidth = (d[10] & 0x0f) | ((d[11] & 0x03) << 4);
	if (hActive == 0 || vActive == 0 || hSyncWidth == 0 || vSyncWidth == 0)
		return false;

	timing.pixel_clock = pixelClock;
	timing.h_display = hActive;
	timing.h_sync_start = hActive + hSyncOffset;
	timing.h_sync_end = timing.h_sync_start + hSyncWidth;
	timing.h_total = hActive + hBlank;
	timing.v_display = vActive;
	timing.v_sync_start = vActive + vSyncOffset;
	timing.v_sync_end = timing.v_sync_start + vSyncWidth;
	timing.v_total = vActive + vBlank;
	// digital separate sync carries the polarities, others are negative
	bool separate = (d[17] & 0x18) == 0x18;
	timing.v_sync_positive = separate && (d[17] & 0x04) != 0;
	timing.h_sync_positive = separate && (d[17] & 0x02) != 0;
	return timing.h_sync_end <= timing.h_total
		&& timing.v_sync_end <= timing.v_total;
}


// #pragma mark - link training


static uint32
link_rate_code(uint32 rate)
{
	return rate == 540000 ? 0x14 : rate == 270000 ? 0x0a : 0x06;
}


static uint8
training_lane_set(uint8 swing, uint8 preEmphasis)
{
	return swing | (preEmphasis << 3) | (swing == 3 ? 0x04 : 0)
		| (preEmphasis == 3 ? 0x20 : 0);
}


/*!	Full link training as the BSP does it (edp_full_link_train): clock
	recovery with TPS1, equalization with TPS3 (or TPS2), adjusting swing
	and pre-emphasis as the sink asks; then the pattern off and scrambling
	on.
*/
status_t
DisplayPipe::_TrainLink(uint32 rate)
{
	uint8 swing[4] = {};
	uint8 preEmphasis[4] = {};

	_Write(kDp + DP_SCRAMBLING_DISABLE, 1);
	status_t status = _PhyConfigure(rate);
	if (status != B_OK)
		return status;
	_Write(kDp + DP_LINK_BW_SET, link_rate_code(rate));
	_Write(kDp + DP_LANE_COUNT_SET, fLanes);

	uint8 laneCount = 0;
	status = _DpcdRead(0x101, &laneCount, 1);
	if (status != B_OK) {
		TRACE("link: DPCD 0x101 unreadable: %s\n", strerror(status));
		return status;
	}
	uint8 linkSet[2] = { (uint8)link_rate_code(rate),
		(uint8)((laneCount & ~0x1f) | fLanes) };
	status = _DpcdWrite(0x100, linkSet, 2);
	if (status != B_OK)
		return status;
	uint8 laneSet[4] = {};
	status = _DpcdWrite(0x103, laneSet, 4);
	if (status != B_OK)
		return status;

	_Update(kDp + DP_SOFT_RESET, 0, 0x10);
	snooze(5000);
	_Update(kDp + DP_TRANSMITTER_ENABLE, 0, 1);

	// clock recovery
	_Write(kDp + DP_TRAINING_PATTERN, 1);
	snooze(2500);
	uint8 pattern = 0x21;
	status = _DpcdWrite(0x102, &pattern, 1);
	if (status != B_OK)
		return status;

	bool done = false;
	int32 tries = 0;
	for (int32 loop = 0; loop < 20 && !done; loop++) {
		snooze(1000);
		uint8 laneStatus[6];
		status = _DpcdRead(0x202, laneStatus, 6);
		if (status != B_OK)
			return status;

		done = true;
		for (uint32 lane = 0; lane < fLanes; lane++) {
			if (((laneStatus[lane / 2] >> (4 * (lane & 1))) & 0x1) == 0)
				done = false;
		}
		if (done)
			break;

		bool changed = false;
		for (uint32 lane = 0; lane < fLanes; lane++) {
			if (swing[lane] == 3) {
				TRACE("link: clock recovery fails at the highest swing\n");
				return B_ERROR;
			}
			uint8 adjust = laneStatus[4 + lane / 2] >> (4 * (lane & 1));
			uint8 newSwing = adjust & 3;
			uint8 newPre = (adjust >> 2) & 3;
			changed |= newSwing != swing[lane];
			swing[lane] = newSwing;
			preEmphasis[lane] = newPre;
			laneSet[lane] = training_lane_set(newSwing, newPre);
		}
		if (changed)
			tries = 0;
		else if (++tries >= 5)
			break;
		_PhySetLevels(rate, swing, preEmphasis);
		_DpcdWrite(0x103, laneSet, fLanes);
	}
	if (!done) {
		TRACE("link: no clock recovery\n");
		return B_ERROR;
	}

	// channel equalization
	bool tps3 = (fDpcd[2] & 0x40) != 0;
	_Write(kDp + DP_TRAINING_PATTERN, tps3 ? 3 : 2);
	snooze(2500);
	pattern = tps3 ? 0x23 : 0x22;
	status = _DpcdWrite(0x102, &pattern, 1);
	if (status != B_OK)
		return status;

	uint32 interval = fDpcd[0x0e] & 0x7f;
	bigtime_t equalizationDelay = interval == 0 ? 400 : interval * 4000;
	done = false;
	for (int32 loop = 0; loop < 5 && !done; loop++) {
		snooze(equalizationDelay);
		uint8 laneStatus[6];
		status = _DpcdRead(0x202, laneStatus, 6);
		if (status != B_OK)
			return status;

		done = (laneStatus[2] & 0x1) != 0;
		for (uint32 lane = 0; lane < fLanes; lane++) {
			if (((laneStatus[lane / 2] >> (4 * (lane & 1))) & 0x7) != 0x7)
				done = false;
		}
		if (done)
			break;

		for (uint32 lane = 0; lane < fLanes; lane++) {
			uint8 adjust = laneStatus[4 + lane / 2] >> (4 * (lane & 1));
			swing[lane] = adjust & 3;
			preEmphasis[lane] = (adjust >> 2) & 3;
			laneSet[lane] = training_lane_set(swing[lane], preEmphasis[lane]);
		}
		_PhySetLevels(rate, swing, preEmphasis);
		_DpcdWrite(0x103, laneSet, fLanes);
	}

	// the pattern off, scrambling on
	_Write(kDp + DP_TRAINING_PATTERN, 0);
	snooze(2500);
	pattern = 0;
	_DpcdWrite(0x102, &pattern, 1);
	snooze(equalizationDelay);
	_Write(kDp + DP_SCRAMBLING_DISABLE, 0);

	if (!done) {
		TRACE("link: no channel equalization\n");
		return B_ERROR;
	}
	TRACE("link trained: %" B_PRIu32 " lanes at %" B_PRIu32 " Mbit/s, swing %u"
		" pre-emphasis %u\n", fLanes, rate / 100, swing[0], preEmphasis[0]);
	return B_OK;
}


// #pragma mark - video


void
DisplayPipe::_SetVideo(const display_timing& timing, uint32 rate)
{
	if ((fDpcd[2] & 0x80) != 0) {
		uint8 value;
		if (_DpcdRead(0x101, &value, 1) == B_OK) {
			value |= 0x80;
			_DpcdWrite(0x101, &value, 1);
		}
		_Write(kDp + DP_ENHANCED_FRAME, 1);
	}

	uint32 pixelClock = timing.pixel_clock;
	_Update(kDp + DP_MVID, 0xffffff, pixelClock & 0xffffff);
	_Update(kDp + DP_NVID, 0xffffff, rate & 0xffffff);
	_Update(kDp + DP_MISC0, 0xff, 0x20);	// RGB, 8 bits, asynchronous clock
	uint32 bytesPerLine = (timing.h_display * 24 + 7) / 8;
	_Write(kDp + DP_DATA_COUNT, (bytesPerLine + fLanes - 1) / fLanes);
	_Write(kDp + DP_INTERLACE, 0);

	uint32 hSync = timing.h_sync_end - timing.h_sync_start;
	uint32 hBackPorch = timing.h_total - timing.h_sync_end;
	uint32 vSync = timing.v_sync_end - timing.v_sync_start;
	uint32 vBackPorch = timing.v_total - timing.v_sync_end;
	_Write(kDp + DP_HTOTAL, timing.h_total);
	_Write(kDp + DP_HSYNC_WIDTH, hSync);
	_Write(kDp + DP_HRES, timing.h_display);
	_Write(kDp + DP_HSTART, hSync + hBackPorch);
	_Write(kDp + DP_VTOTAL, timing.v_total);
	_Write(kDp + DP_VSYNC_WIDTH, vSync);
	_Write(kDp + DP_VRES, timing.v_display);
	_Write(kDp + DP_VSTART, vSync + vBackPorch);
	_Write(kDp + DP_POLARITY, (timing.h_sync_positive ? 1 : 0)
		| (timing.v_sync_positive ? 2 : 0));

	// secondary data window and transfer unit, after the timings
	uint32 hStart = _Read(kDp + DP_HSTART);
	_Write(kDp + DP_SECOND_DATA_WINDOW,
		(uint64)hStart * (rate / 2) / pixelClock);
	uint32 bandwidth = fLanes * rate / 1000;
	uint32 symbols1000 = (uint64)pixelClock * 8 * 24 / bandwidth;
	uint32 symbols = symbols1000 / 1000;
	uint32 fraction = (symbols1000 % 1000) * 16 / 1000;
	_Write(kDp + DP_TU_CONFIG, (fraction << 24) | (symbols << 16) | 64);
	_Update(kDp + DP_PIXEL_COUNT, 0x7, 1);

	_Update(kDp + DP_SOFT_RESET, 0, 0x1);
	snooze(5000);
	_Update(kDp + DP_VIDEO_ENABLE, 0, 1);
	_Update(kDp + DP_SECOND_STREAM_ENABLE, 0, 1);
}


/*!	Display engine 3.5, disp0: one full screen layer on video channel 0,
	the blender passing it through, out to TCON-TV1 (mux index 5). Direct
	register writes; the BSP's register command queue is not used.
*/
void
DisplayPipe::_InitDisplayEngine(const display_timing& timing,
	phys_addr_t address, uint32 bytesPerRow)
{
	uint32 size = ((uint32)(timing.v_display - 1) << 16)
		| (timing.h_display - 1);

	_Update(kDeTop + 0x004, 0, 0x00010001);
	_Update(kDeTop + 0x000, 0, 0x00010001);
	_Update(kDeTop + 0x008, 0, 1u << 4);
	_Update(kDeTop + 0x008, 0, 1u << 0);
	_Update(kDeTop + 0x080, 0xffff0000, 2048u << 16);
	_Write(kDeTop + 0x050, 0x2000);
	_Update(kDeTop + 0x100, (1u << 4) | (1u << 6) | (1u << 7) | (3u << 16),
		1u << 0);
	_Write(kDeTop + 0x10c, 1);
	_Write(kDeTop + 0x108, size);

	uint32 mux = _Read(kDeTop + 0x010);
	for (uint32 nibble = 1; nibble < 8; nibble++) {
		if (((mux >> (nibble * 4)) & 0xf) == 5)
			mux |= 0xfu << (nibble * 4);
	}
	_Write(kDeTop + 0x010, (mux & ~0xfu) | 5);
	_Write(kDeTop + 0x030, 0x0000a810);
	_Write(kDeTop + 0x028, 0);
	_Write(kDeTop + 0x02c, 0);
	_Write(kDeTop + 0x00c, 0);

	// blender: pipe 0 from port 0, opaque, size of the output
	_Write(kBlender0 + 0x088, 0);
	_Write(kBlender0 + 0x08c, size);
	_Write(kBlender0 + 0x0fc, 0);
	_Write(kBlender0 + 0x004, 0xff000000);
	_Update(kBlender0 + 0x080, 0xf, 1);	// pipe 0 from port 1: vch1
	_Write(kBlender0 + 0x084, 0);
	_Write(kBlender0 + 0x090, 0x03010301);
	_Write(kBlender0 + 0x008, size);
	_Write(kBlender0 + 0x00c, 0);
	_Write(kBlender0 + 0x000, 0x101);
	_Write(kFormatter0, 0);

	// identity color conversions, as Linux leaves them
	static const uint32 kIdentity[] = {
		0x20000, 0, 0, 0, 0, 0x20000, 0, 0, 0, 0, 0x20000, 0, 0
	};
	_Write(kPlaneCsc, 1);
	for (uint32 i = 1; i < 4; i++)
		_Write(kPlaneCsc + i * 4, 0);
	for (uint32 i = 0; i < B_COUNT_OF(kIdentity); i++)
		_Write(kPlaneCsc + 0x10 + i * 4, kIdentity[i]);
	static const uint32 kMatrix[] = {
		0x20000, 0, 0, 0x10000, 0, 0x20000, 0, 0x10000, 0, 0, 0x20000, 0x10000
	};
	_Write(kColorMatrix0, 1);
	_Write(kColorMatrix0 + 0x4, size);
	_Write(kColorMatrix0 + 0x8, 0);
	_Write(kColorMatrix0 + 0xc, 0);
	for (uint32 i = 0; i < B_COUNT_OF(kMatrix); i++)
		_Write(kColorMatrix0 + 0x10 + i * 4, kMatrix[i]);

	// video channel 0 shows nothing (Linux and firmware may have used it)
	for (uint32 layer = 0; layer < 4; layer++)
		_Write(kVch0Overlay + layer * 0x30, 0);

	// the layer: XRGB8888 (B_RGB32), global alpha, RGB on a video channel
	_Write(kPlaneOverlay + 0x008, 0);
	_Write(kPlaneOverlay + 0x030, 0);
	_Write(kPlaneOverlay + 0x060, 0);
	_Write(kPlaneOverlay + 0x090, 0);
	_Write(kVch1 + 0x5000, 0);	// AFBC decoder
	_Write(kVch1 + 0x5400, 0);	// tiled frame buffer decoder
	SetScanout(address, bytesPerRow, timing.h_display, timing.v_display);
	_Write(kPlaneOverlay + 0x000, 0xff008403);
}


/*!	TCON-TV1 (tcon_tv_init and tcon_tv_cfg of the BSP, sun60iw2), fed by
	the display engine, out to the DP controller through TCON TOP1.
*/
void
DisplayPipe::_InitTcon(const display_timing& timing)
{
	_Update(kTconTv1 + 0x090, 1u << 31, 0);
	_Update(kTconTv1 + 0x000, 1u << 31, 0);
	_Write(kTconTv1 + 0x004, 0);
	_Update(kTconTv1 + 0x000, 0, 1u << 31);
	for (uint32 offset = 0x330; offset <= 0x340; offset += 4)
		_Write(kTconTv1 + offset, 0);

	uint32 hSync = timing.h_sync_end - timing.h_sync_start;
	uint32 hBackPorch = timing.h_total - timing.h_sync_end;
	uint32 vSync = timing.v_sync_end - timing.v_sync_start;
	uint32 vBackPorch = timing.v_total - timing.v_sync_end;

	_Write(kTconTv1 + 0x098, (uint32)timing.v_total * 2);
	_Write(kTconTv1 + 0x09c, ((uint32)(timing.h_display - 1) << 16)
		| (timing.v_display - 1));
	_Write(kTconTv1 + 0x0a0, ((uint32)(timing.h_total - 1) << 16)
		| (hSync + hBackPorch - 1));
	_Write(kTconTv1 + 0x0a4, ((uint32)timing.v_total * 2 << 16)
		| (vSync + vBackPorch - 1));
	_Write(kTconTv1 + 0x0a8, ((hSync - 1) << 16) | (vSync - 1));
	_Write(kTconTv1 + 0x088, (timing.v_sync_positive ? 1u << 24 : 0)
		| (timing.h_sync_positive ? 1u << 25 : 0));

	_Write(kTconTv1 + 0x300, 0);
	_Write(kTconTv1 + 0x304, ((uint32)timing.v_total + 1) << 12);
	_Write(kTconTv1 + 0x308, (uint32)timing.v_total << 12);
	_Write(kTconTv1 + 0x30c, 0);

	uint32 startDelay = timing.v_total - timing.v_display - 5;
	if (startDelay > 31)
		startDelay = 31;
	_Write(kTconTv1 + 0x090, startDelay << 4);
	// the display engine as the source, not a test pattern; this register
	// does not come up 0 (the BSP only writes it for its colour bars)
	_Write(kTconTv1 + 0x040, 0);
	_Update(kTconTv1 + 0x088, 0, 1u << 26);
	_Write(kTconTv1 + 0x08c, 0x0fffffff);
	_Update(kTconTv1 + 0x000, 3u << 4, 0);	// one pixel per clock

	_Update(kTconTop1 + 0x000, 0, 1u << 5);		// TV1 to the DP controller
	_Update(kTconTop1 + 0x020, 0, 1u << 21);	// TV1 clock

	_Update(kTconTv1 + 0x090, 0, 1u << 31);
	_Update(kTconTv1 + 0x004, 0, 1u << 30);
}


// #pragma mark - public


status_t
DisplayPipe::Init()
{
	static const struct { phys_addr_t base; size_t size; } kBlocks[] = {
		{ 0x02002000, 0x2000 },		// CCU
		{ 0x07010000, 0x1000 },		// R-CCU
		{ 0x07090000, 0x1000 },		// RTC (DCXO gates)
		{ kPck600, 0xb000 },		// power domains 0..10
		{ 0x03910000, 0x1000 },		// IOMMU1
		{ kDeTop, 0x1000 },
		{ kVch0, 0x6000 },
		{ kVch1, 0x6000 },
		{ 0x05280000, 0xa000 },		// disp0
		{ kTconTop1, 0x1000 },
		{ kTconTv1, 0x1000 },
		{ kDp, 0x1000 },
		{ kDpTop, 0x1000 },
		{ kSerdesTop, 0x2000 },		// subsystem top, PHY0 link, AUX PHY
		{ kComboTop, 0x2000 },
		{ kPhyRegisters, 0x20000 },
	};
	for (size_t i = 0; i < B_COUNT_OF(kBlocks); i++) {
		status_t status = _Map(kBlocks[i].base, kBlocks[i].size);
		if (status != B_OK)
			return status;
	}

	_Update(kPpuClock, 0, 1);
	status_t status = _PowerOn(kDomainDeSys);
	if (status == B_OK)
		status = _PowerOn(kDomainVo1);
	if (status != B_OK)
		return status;

	status = _EnableDisplayEngineClocks();
	if (status != B_OK)
		return status;

	// the display engine reads physical addresses: IOMMU bypass for it
	_Update(kIommu1Clock, 0, 0x00010007);
	_SetKeyedGate(kMbusGates, kMbusKey, 1);
	_Write(kDeIommuBypass, 1);

	// TCON TOP1, TCON-TV1, the DP controller and their shared pixel clock
	_Update(kVideoOut1Reset, 0, 1u << 16);
	_Update(kDpssTop1, 0, 1u << 16);
	_SetKeyedGate(kAhbGates, kAhbKey, 4);
	_Update(kDpssTop1, 0, 1u << 0);
	_Update(kTconTv1Bus, 0, 1u << 16);
	_Update(kTconTv1Bus, 0, 1u << 0);
	_Update(kEdpBus, 0, 1u << 16);
	_Update(kEdpBus, 0, 1u << 0);
	status = _SetPixelClock(148500);
	if (status != B_OK)
		return status;

	// the combo PHY: 26 MHz DCXO reference, 100 MHz configuration clock
	_Update(kDcxoSerdes, 0, 1u << 4);
	_Write(kSerdesClock, 0x81000005);
	_Update(kSerdesReset, 0, 1u << 16);
	_SetKeyedGate(kAhbGates, kAhbKey, 8);
	_SetKeyedGate(kMbusGates, kMbusKey, 28);
	_Update(kSerdesTop + 0x008, 0, 0x30000);
	snooze(10000);

	_InitAuxPhy();
	_Write(kComboTop + 0xc24, 0x3210);	// lane remap
	for (uint32 lane = 0; lane < 4; lane++) {
		phys_addr_t word = PHY_WORD(0xf000 + lane * 0x100);
		_Write16(word, _Read16(word) | 0x100);	// lane invert
	}

	_InitController();
	TRACE("DP controller %#" B_PRIx32 ", AUX PHY %#" B_PRIx32 " %#" B_PRIx32
		"\n", _Read(kDp + DP_CORE_ID), _Read(kAuxPhy), _Read(kAuxPhy + 4));
	// TCON-TV1 is clocked from here on, so its registers can be read when
	// it interrupts
	fTconInterrupts = (volatile uint32*)_Address(kTconTv1 + 0x004);
	fInitialized = true;
	return B_OK;
}


status_t
DisplayPipe::Discover(bool flipped, uint8* edid, uint32* _edidLength,
	display_timing& timing)
{
	if (!fInitialized)
		return B_NO_INIT;
	fFlipped = flipped;
	// The AUX channel arrives through the connector's SBU pins, which a
	// flipped plug swaps: the PHY's connection direction has to be right
	// before the sink is spoken to, not only when the main link is
	// configured (_PhyConfigure()).
	_Update(kPhyLink + LINK_CTRL0, 1u << 12, fFlipped ? 1u << 12 : 0);

	status_t status = _DpcdRead(0, fDpcd, sizeof(fDpcd));
	if (status == B_OK && (fDpcd[0] == 0 || fDpcd[1] == 0))
		status = B_BAD_DATA;
	if (status != B_OK) {
		TRACE("no DPCD: %s\n", strerror(status));
		return status;
	}
	TRACE("sink: DPCD %x.%x, up to %u lanes at %u Mbit/s%s%s\n",
		fDpcd[0] >> 4, fDpcd[0] & 0xf, fDpcd[2] & 0x1f, fDpcd[1] * 270,
		(fDpcd[2] & 0x40) != 0 ? ", TPS3" : "",
		(fDpcd[5] & 0x01) != 0 ? ", branch device" : "");

	*_edidLength = 0;
	status = _ReadEdid(edid, _edidLength);
	if (status != B_OK)
		TRACE("no EDID: %s\n", strerror(status));

	// the preferred timing if it is one this path can show
	timing = k1080p60;
	display_timing preferred;
	if (*_edidLength >= 128 && parse_detailed_timing(edid + 54, preferred)) {
		if (preferred.pixel_clock <= 200000 && preferred.h_display <= 4096
			&& preferred.v_display <= 4096) {
			timing = preferred;
		} else {
			TRACE("preferred mode %ux%u at %" B_PRIu32 " kHz is out of reach,"
				" showing 1920x1080\n", preferred.h_display,
				preferred.v_display, preferred.pixel_clock);
		}
	}
	return B_OK;
}


status_t
DisplayPipe::Enable(const display_timing& timing, phys_addr_t address,
	uint32 bytesPerRow)
{
	if (!fInitialized)
		return B_NO_INIT;

	fLanes = fDpcd[2] & 0x1f;
	if (fLanes >= 4)
		fLanes = 4;
	else if (fLanes >= 2)
		fLanes = 2;
	else
		fLanes = 1;
	// HBR first, Linux's working state; HBR2 when the mode needs more. A
	// lane carries rate x 10 kbit/s, 8 of every 10 bits are payload.
	fLinkRate = fDpcd[1] >= 0x0a ? 270000 : 162000;
	uint64 needed = (uint64)timing.pixel_clock * 24;
	if ((uint64)fLanes * fLinkRate * 8 < needed && fDpcd[1] >= 0x14)
		fLinkRate = 540000;
	if ((uint64)fLanes * fLinkRate * 8 < needed) {
		TRACE("%" B_PRIu32 " kHz needs more than the link carries\n",
			timing.pixel_clock);
		return B_NOT_SUPPORTED;
	}

	fWidth = timing.h_display;
	fHeight = timing.v_display;
	_InitDisplayEngine(timing, address, bytesPerRow);

	status_t status = _SetPixelClock(timing.pixel_clock);
	if (status != B_OK) {
		TRACE("no pixel clock of %" B_PRIu32 " kHz\n", timing.pixel_clock);
		return status;
	}
	_InitTcon(timing);

	// Allwinner resets the DP controller before every enable on this SoC;
	// the shared pixel clock stays on for the TCON.
	_Update(kEdpBus, (1u << 16) | (1u << 0), 0);
	snooze(50000);
	_Update(kEdpBus, 0, 1u << 16);
	_Update(kEdpBus, 0, 1u << 0);
	_InitController();
	snooze(10000);

	status = _TrainLink(fLinkRate);
	if (status != B_OK)
		return status;
	_SetVideo(timing, fLinkRate);

	fEnabled = true;

	snooze(50000);
	uint8 laneStatus[3] = {};
	_DpcdRead(0x202, laneStatus, 3);
	TRACE("%ux%u at %" B_PRIu32 " kHz on: TCON %#" B_PRIx32 ", measured MVID"
		" %#" B_PRIx32 ", lanes %02x %02x %02x\n", timing.h_display,
		timing.v_display, timing.pixel_clock, _Read(kTconTv1 + 0x0fc),
		_Read(kDp + DP_MVID), laneStatus[0], laneStatus[1], laneStatus[2]);
	return B_OK;
}


/*!	Shows \a width x \a height pixels at \a address on the whole output,
	through the channel's scaler when that is not the output's size.
*/
status_t
DisplayPipe::SetScanout(phys_addr_t address, uint32 bytesPerRow,
	uint32 width, uint32 height)
{
	if (fWidth == 0 || fHeight == 0 || width == 0 || height == 0
		|| width > 4096 || height > 4096)
		return B_BAD_VALUE;

	uint32 size = ((height - 1) << 16) | (width - 1);
	uint32 outputSize = ((uint32)(fHeight - 1) << 16) | (fWidth - 1);
	_Write(kPlaneOverlay + 0x004, size);
	_Write(kPlaneOverlay + 0x0e8, size);
	_Write(kPlaneOverlay + 0x00c, bytesPerRow);
	_Write(kPlaneOverlay + 0x018, (uint32)address);
	_Update(kPlaneOverlay + 0x0d0, 0xff, (uint32)(address >> 32) & 0xff);

	if (width == fWidth && height == fHeight) {
		_Write(kPlaneScaler, 0);
		return B_OK;
	}
	if (!kUseScaler) {
		// shown 1:1 from the top left corner, the rest black
		_Write(kPlaneScaler, 0);
		return B_OK;
	}

	// VSU8: steps in 4.19 fixed point, stored from bit 1; RGB, so the
	// "chroma" path takes the same sizes and steps
	uint32 horizontalStep = (uint32)(((uint64)width << 19) / fWidth) << 1;
	uint32 verticalStep = (uint32)(((uint64)height << 19) / fHeight) << 1;
	_Write(kPlaneScaler + 0x10, 0);		// scale mode: RGB
	_Write(kPlaneScaler + 0x40, outputSize);
	_Write(kPlaneScaler + 0x44, 0xff);	// global alpha
	_Write(kPlaneScaler + 0x80, size);
	_Write(kPlaneScaler + 0x88, horizontalStep);
	_Write(kPlaneScaler + 0x8c, verticalStep);
	_Write(kPlaneScaler + 0x90, 0);
	_Write(kPlaneScaler + 0x98, 0);
	_Write(kPlaneScaler + 0xc0, size);
	_Write(kPlaneScaler + 0xc8, horizontalStep);
	_Write(kPlaneScaler + 0xcc, verticalStep);
	_Write(kPlaneScaler + 0xd0, 0);
	_Write(kPlaneScaler + 0xd8, 0);
	for (uint32 i = 0; i < 32; i++) {
		_Write(kPlaneScaler + 0x200 + i * 4, kLanczos2[i]);
		_Write(kPlaneScaler + 0x400 + i * 4, kLanczos2[i]);
		_Write(kPlaneScaler + 0x600 + i * 4, kLanczos2[i]);
	}
	_Write(kPlaneScaler, (1u << 4) | 1);	// coefficients ready, enable
	return B_OK;
}


status_t
DisplayPipe::DebugRegister(phys_addr_t address, uint32& value, bool write)
{
	if ((address & 3) != 0)
		return B_BAD_VALUE;
	for (int32 i = 0; i < fMappingCount; i++) {
		const Mapping& mapping = fMappings[i];
		if (address < mapping.physical
			|| address + 4 > mapping.physical + mapping.size) {
			continue;
		}
		if (address >= kPhyRegisters
			&& address < kPhyRegisters + 0x20000) {
			// the PHY's register file takes 16-bit accesses only
			if (write)
				_Write16(address, (uint16)value);
			else
				value = _Read16(address);
		} else if (write)
			_Write(address, value);
		else
			value = _Read(address);
		return B_OK;
	}
	return B_BAD_ADDRESS;
}


void
DisplayPipe::DumpState(const char* when)
{
	TRACE("%s: overlay %#" B_PRIx32 " size %#" B_PRIx32 " pitch %#" B_PRIx32
		" addr %#" B_PRIx32 " win %#" B_PRIx32 "; scaler %#" B_PRIx32 " out %#"
		B_PRIx32 " in %#" B_PRIx32 " steps %#" B_PRIx32 "/%#" B_PRIx32
		"; blender %#" B_PRIx32 " route %#" B_PRIx32 " in %#" B_PRIx32
		"; RTMX status %#" B_PRIx32 "; TCON %#" B_PRIx32 "\n", when,
		_Read(kPlaneOverlay), _Read(kPlaneOverlay + 0x004),
		_Read(kPlaneOverlay + 0x00c), _Read(kPlaneOverlay + 0x018),
		_Read(kPlaneOverlay + 0x0e8), _Read(kPlaneScaler),
		_Read(kPlaneScaler + 0x40), _Read(kPlaneScaler + 0x80),
		_Read(kPlaneScaler + 0x88), _Read(kPlaneScaler + 0x8c),
		_Read(kBlender0), _Read(kBlender0 + 0x080), _Read(kBlender0 + 0x008),
		_Read(kDeTop + 0x104), _Read(kTconTv1 + 0x0fc));
}


/*!	GINT0: interrupt enables in bits 31..16, flags in 15..0; _InitTcon()
	enables the vertical blank one (bit 30), flagged in bit 14. Writing 0
	clears a flag, writing 1 leaves it as it is, so the other flags are
	written back as they were read.
*/
bool
DisplayPipe::AcknowledgeBlank()
{
	if (fTconInterrupts == NULL)
		return false;
	uint32 interrupts = *fTconInterrupts;
	if ((interrupts & (1u << 14)) == 0)
		return false;
	*fTconInterrupts = interrupts & ~(1u << 14);
	return true;
}


bool
DisplayPipe::LinkOk()
{
	if (!fEnabled)
		return false;
	uint8 status[3];
	if (_DpcdRead(0x202, status, 3) != B_OK)
		return false;
	for (uint32 lane = 0; lane < fLanes; lane++) {
		uint8 bits = (status[lane / 2] >> ((lane & 1) * 4)) & 0x7;
		if (bits != 0x7)
			return false;
	}
	return (status[2] & 0x1) != 0;
}


uint32
DisplayPipe::MaxPixelClock() const
{
	uint32 lanes = fDpcd[2] & 0x1f;
	if (lanes >= 4)
		lanes = 4;
	else if (lanes >= 2)
		lanes = 2;
	else
		lanes = 1;
	uint32 rate = fDpcd[1] >= 0x0a ? 270000 : 162000;
	// a lane carries rate x 10 kbit/s, 8 of every 10 bits are payload, 24
	// bits a pixel
	return lanes * rate / 3;
}


void
DisplayPipe::Disable()
{
	if (!fEnabled)
		return;
	_Update(kDp + DP_VIDEO_ENABLE, 1, 0);
	uint8 value;
	if (_DpcdRead(0x101, &value, 1) == B_OK) {
		value &= ~0x80;
		_DpcdWrite(0x101, &value, 1);
	}
	_Update(kTconTv1 + 0x090, 1u << 31, 0);
	_Update(kTconTv1 + 0x004, 1u << 30, 0);
	_Update(kTconTv1 + 0x000, 1u << 31, 0);
	_Update(kTconTop1 + 0x000, 1u << 5, 0);
	_Update(kTconTop1 + 0x020, 1u << 21, 0);
	_Update(kPlaneOverlay, 1, 0);
	_Write(kPlaneScaler, 0);
	fEnabled = false;
}


}	// namespace sunxi
