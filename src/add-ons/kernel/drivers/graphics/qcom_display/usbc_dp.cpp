/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "usbc_dp.h"

#include "display_port.h"

#include <new>
#include <string.h>

#include <KernelExport.h>
#include <driver_settings.h>
#include <arch/atomic.h>


#define ERROR(x...)	dprintf("qcom_display: " x)
#define INFO(x...)	dprintf("qcom_display: " x)


namespace qcom {


// the USB-C connectors (gcc-sc8280xp.c, dispcc-sc8280xp.c,
// dpu_8_0_sc8280xp.h, dpu_hw_ctl.c, the device tree)
static const phys_addr_t kGccBase = 0x100000;
static const phys_addr_t kDispccBase = 0xaf00000;
static const phys_addr_t kTlmmBase = 0xf100000;
static const uint32 kMdp = 0x1000;

struct usbc_connector {
	const char*	name;
	uint32		controller;		// DisplayPort controller of MDSS 0
	phys_addr_t	controllerBase;
	// the combo PHY: its GDSC, aux/ref/com aux clocks, phy/common resets
	phys_addr_t	phyBase;
	uint32		gdsc;
	uint32		phyBranches[3];
	uint32		phyResets[2];
	// dispcc 0
	uint32		linkRcg, linkSource, linkBranch, linkIntfBranch;
	uint32		auxRcg, auxBranch;
	uint32		pixelRcg, pixelSource, pixelBranch;
	// the SBU mux (PI3USB102)
	uint32		sbuEnablePin, sbuSelectPin;
	// its DPU path: pipe, mixer, control path, timing engine
	uint32		pipe, mixer, control, interface;
	uint32		pipeFlush, mixerFlush;
	uint32		pipeFetch;		// CTL_FETCH_PIPE_ACTIVE bit
	uint32		layerShift;		// the pipe's field in CTL_LAYER
	uint32		layerExtShift;	// and in CTL_LAYER_EXT
	uint32		interfaceNumber;
	uint32		underrun;		// MDP interrupt bit
};

static const usbc_connector kConnectors[] = {
	{"DP-1", 0, 0xae90000, 0x88eb000, 0xf004, {0xf054, 0x8c02c, 0xf058},
		{0x50000, 0x4a008}, 0x21a4, 1, 0x205c, 0x2060, 0x2238, 0x206c,
		0x21d8, 2, 0x2070, 101, 164,
		kMdp + 0x4000, 0, 0, kMdp + 0x34000,	// VIG0, LM_0, CTL_0, INTF_0
		1u << 0, 1u << 6, 1u << 16, 0, 0, 0, 1u << 24},
	{"DP-2", 1, 0xae98000, 0x8903000, 0x10004, {0x10054, 0x8c010, 0x10058},
		{0x5000c, 0xb9020}, 0x2268, 2, 0x2084, 0x2088, 0x22d0, 0x20a0,
		0x2250, 4, 0x2078, 48, 47,
		kMdp + 0x6000, 1, 1, kMdp + 0x38000,	// VIG1, LM_1, CTL_1, INTF_4
		1u << 1, 1u << 7, 1u << 17, 3, 2, 4, 1u << 20},
};


// the combo PHY (phy-qcom-qmp-combo.c, qmp_combo_offsets_v5)
static const uint32 kPhyCom = 0x0000, kPhyTxA = 0x0400, kPhyTxB = 0x0a00,
	kPhyDpSerdes = 0x2000, kPhyDpPhy = 0x2200;

struct qmp_setting {
	uint16	offset;
	uint8	value;
};

static const qmp_setting kDpSerdes[] = {
	{0x184, 0x05}, {0x094, 0x3b}, {0x04c, 0x02}, {0x048, 0x0c},
	{0x050, 0x06}, {0x154, 0x30}, {0x058, 0x0f}, {0x084, 0x36},
	{0x088, 0x36}, {0x07c, 0x16}, {0x080, 0x16}, {0x074, 0x06},
	{0x078, 0x06}, {0x17c, 0x02}, {0x0ec, 0x3f}, {0x0f0, 0x00},
	{0x10c, 0x02}, {0x0cc, 0x00}, {0x00c, 0x0a}, {0x168, 0x0a},
	{0x108, 0x00}, {0x044, 0x17}, {0x174, 0x1f},
};

// per link rate (qmp_v4_dp_serdes_tbl_rbr, _hbr, _hbr2, _hbr3): HSCLK_SEL,
// DEC_START, DIV_FRAC_START2/3, LOCK_CMP1/2, LOCK_CMP_EN; and the PHY's VCO
// divider (qmp_v3_configure_dp_clocks)
static const uint16 kRateRegisters[] = {0x158, 0x0bc, 0x0d0, 0x0d4, 0x0ac,
	0x0b0, 0x0a4};
static const struct {
	uint32	rate;			// MHz of the link symbol clock
	uint8	values[7];
	uint8	vcoDivider;
	uint8	dpcd;			// LINK_BW_SET
} kRates[] = {
	{162, {0x05, 0x69, 0x80, 0x07, 0x6f, 0x08, 0x04}, 1, 0x06},
	{270, {0x03, 0x69, 0x80, 0x07, 0x0f, 0x0e, 0x08}, 1, 0x0a},
	{540, {0x01, 0x8c, 0x00, 0x0a, 0x1f, 0x1c, 0x08}, 2, 0x14},
	{810, {0x00, 0x69, 0x80, 0x07, 0x2f, 0x2a, 0x08}, 0, 0x1e},
};

static const qmp_setting kDpTx[] = {
	{0x80, 0x51}, {0x48, 0x1a}, {0xb0, 0x40}, {0x20, 0x00},
	{0xe4, 0xff}, {0x08, 0x0f}, {0x1c, 0x03}, {0xac, 0x0f},
	{0x54, 0x00}, {0x30, 0x11}, {0x34, 0x11}, {0xd8, 0x01},
};

enum {
	COM_PHY_MODE_CTRL = 0x00, COM_SW_RESET = 0x04, COM_POWER_DOWN_CTRL = 0x08,
	COM_SWI_CTRL = 0x0c, COM_TYPEC_CTRL = 0x10, COM_RESET_OVRD_CTRL = 0x1c,

	SERDES_RESETSM_CNTRL = 0x09c, SERDES_C_READY_STATUS = 0x178,
	SERDES_CMN_STATUS = 0x140, SERDES_BIAS_EN = 0x044,

	DP_PHY_CFG = 0x010, DP_PHY_CFG_1 = 0x014, DP_PHY_PD_CTL = 0x018,
	DP_PHY_MODE = 0x01c, DP_PHY_AUX_CFG0 = 0x020,
	DP_PHY_AUX_INTERRUPT_MASK = 0x054, DP_PHY_VCO_DIV = 0x070,
	DP_PHY_TX0_TX1_LANE_CTL = 0x078, DP_PHY_TX2_TX3_LANE_CTL = 0x09c,
	DP_PHY_STATUS = 0x0dc,

	TX_EMP_POST1_LVL = 0x0c, TX_DRV_LVL = 0x14, TX_TRANSCEIVER_BIAS = 0x48,
	TX_HIGHZ_DRVR_EN = 0x4c, TX_POL_INV = 0x50,
};

// the DPU (dpu_8_0_sc8280xp.h): the firmware's HDMI path and the new one
static const phys_addr_t kMdssBase = 0xae00000;
static const size_t kMdssSize = 0x90000;
static const uint32 kVig2 = kMdp + 0x8000;
static const uint32 kMixers = kMdp + 0x44000, kLm2 = kMixers + 0x2000;
static const uint32 kControls = kMdp + 0x15000, kCtl2 = kControls + 0x2000;
static const uint32 kIntf6 = kMdp + 0x3a000;
static const uint32 kInterruptStatus = kMdp + 0x14;
static const uint32 kInterruptClear = kMdp + 0x18;

enum {
	CTL_LAYER_LM2 = 0x08, CTL_TOP = 0x14,
	CTL_FLUSH = 0x18, CTL_LAYER_EXT_LM2 = 0x48,
	CTL_INTF_ACTIVE = 0xf4, CTL_FETCH_PIPE_ACTIVE = 0xfc,
	CTL_INTF_FLUSH = 0x110,

	SSPP_SRC_SIZE = 0x00, SSPP_SRC_XY = 0x08, SSPP_OUT_SIZE = 0x0c,
	SSPP_OUT_XY = 0x10, SSPP_SRC0_ADDR = 0x14, SSPP_SRC_YSTRIDE0 = 0x24,

	LM_OUT_SIZE = 0x04,

	INTF_TIMING_ENGINE_EN = 0x000, INTF_FRAME_LINE_COUNT_EN = 0x0a8,
	INTF_FRAME_COUNT = 0x0ac, INTF_PANEL_FORMAT = 0x090,
	INTF_PROG_FETCH_START = 0x170, INTF_MUX = 0x25c,
};
static const uint32 kFlushCtl = 1u << 17, kFlushIntf = 1u << 31;
// the HDMI path's timing registers, which INTF_0 takes
static const uint32 kIntfTiming[] = {0x004, 0x008, 0x00c, 0x014, 0x01c, 0x024,
	0x02c, 0x034, 0x03c, 0x040, 0x04c, 0x050, 0x060, 0x064, 0x068};


static inline uint32
rd(volatile uint8* base, uint32 offset)
{
	return *(volatile uint32*)(base + offset);
}


static inline void
wr(volatile uint8* base, uint32 offset, uint32 value)
{
	*(volatile uint32*)(base + offset) = value;
	memory_full_barrier();
}


static bool
wait_bits(volatile uint8* base, uint32 offset, uint32 mask, uint32 value)
{
	for (int i = 0; i < 1000; i++) {
		if ((rd(base, offset) & mask) == value)
			return true;
		spin(10);
	}
	return false;
}


//!	A branch on; whether it then runs (CBCR bit 31: off).
static bool
branch_on(volatile uint8* cc, uint32 offset)
{
	wr(cc, offset, rd(cc, offset) | 1);
	return wait_bits(cc, offset, 1u << 31, 0);
}


UsbcDisplayPort::UsbcDisplayPort(uint32 connector)
	:
	fConnector(kConnectors[connector < B_COUNT_OF(kConnectors)
		? connector : 0]),
	fMixerBase(kMixers + 0x1000 * fConnector.mixer),
	fControlBase(kControls + 0x1000 * fConnector.control),
	fLayer(4 * fConnector.mixer),
	fLayerExt(0x40 + 4 * fConnector.mixer),
	fGccArea(-1), fGcc(NULL), fDispccArea(-1), fDispcc(NULL), fPhyArea(-1),
	fPhy(NULL), fControllerArea(-1), fController(NULL), fPort(NULL),
	fMdssArea(-1), fMdss(NULL), fRate(0), fWideBus(false),
	fReverse(false), fPhyLanes(4), fLanes(4), fTrained(false), fRetrainings(0), fPowered(false), fRunning(false)
{
}


UsbcDisplayPort::~UsbcDisplayPort()
{
	Stop();
	delete fPort;
	area_id areas[] = {fGccArea, fDispccArea, fPhyArea, fControllerArea,
		fMdssArea};
	for (area_id area : areas) {
		if (area >= 0)
			delete_area(area);
	}
}


status_t
UsbcDisplayPort::_Map(area_id& area, volatile uint8*& base, const char* name,
	phys_addr_t address, size_t size)
{
	area = map_physical_memory(name, address, size,
		B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&base);
	return area < 0 ? area : B_OK;
}


/*!	The combo PHY in USB3 + DisplayPort mode at RBR, four lanes, the normal
	orientation (qmp_combo_com_init(), qmp_v4_dp_aux_init(),
	qmp_combo_dp_power_on(), qmp_v4_configure_dp_phy()). Its supplies (L9D,
	L4D) are the USB PHY's, which the firmware has on. */
status_t
UsbcDisplayPort::_PowerPhy()
{
	volatile uint8* com = fPhy + kPhyCom;
	volatile uint8* serdes = fPhy + kPhyDpSerdes;
	volatile uint8* dp = fPhy + kPhyDpPhy;

	// With two lanes USB 3 runs beside DisplayPort on the same PHY: if the
	// firmware (or the USB side) has the common block up in USB 3 mode, in
	// our orientation, it is left alone, as Linux' qmp_combo_com_init() runs
	// once for both halves; resetting it drops the USB 3 link.
	uint32 gdsc = rd(fGcc, fConnector.gdsc);
	bool usbRunning = fPhyLanes == 2 && (gdsc & (1u << 31)) != 0
		&& (rd(com, COM_PHY_MODE_CTRL) & 1) != 0
		&& (rd(com, COM_SW_RESET) & 1) == 0
		&& (rd(com, COM_TYPEC_CTRL) & 3) == (1u << 1 | (fReverse ? 1 : 0));
	if (usbRunning) {
		INFO("%s: USB 3 runs on the PHY: its common block kept\n",
			fConnector.name);
		wr(com, COM_PHY_MODE_CTRL, 3);
	} else {
		if ((gdsc & 1) != 0)
			wr(fGcc, fConnector.gdsc, gdsc & ~1u);
		if (!wait_bits(fGcc, fConnector.gdsc, 1u << 31, 1u << 31)) {
			ERROR("%s: its PHY's GDSC does not come on\n", fConnector.name);
			return B_ERROR;
		}
		for (uint32 branch : fConnector.phyBranches)
			branch_on(fGcc, branch);
		for (uint32 reset : fConnector.phyResets)
			wr(fGcc, reset, rd(fGcc, reset) | 1);
		spin(200);
		for (uint32 reset : fConnector.phyResets)
			wr(fGcc, reset, rd(fGcc, reset) & ~1u);
		spin(200);

		wr(com, COM_POWER_DOWN_CTRL, rd(com, COM_POWER_DOWN_CTRL) | 1);
		wr(com, COM_RESET_OVRD_CTRL, rd(com, COM_RESET_OVRD_CTRL) | 0xf);
		// software port select, the orientation; DisplayPort only with
		// four lanes, USB 3 beside it with two
		wr(com, COM_TYPEC_CTRL, 1u << 1 | (fReverse ? 1 : 0));
		wr(com, COM_PHY_MODE_CTRL, fPhyLanes == 4 ? 2 : 3);
		wr(com, COM_RESET_OVRD_CTRL, rd(com, COM_RESET_OVRD_CTRL) & ~0xfu);
		wr(com, COM_SWI_CTRL, rd(com, COM_SWI_CTRL) & ~3u);
		wr(com, COM_SW_RESET, rd(com, COM_SW_RESET) & ~1u);
	}
	for (uint32 branch : fConnector.phyBranches)
		branch_on(fGcc, branch);

	// the AUX block
	wr(dp, DP_PHY_PD_CTL, 0x01 | 0x02 | 0x04 | 0x20 | 0x40);
	wr(serdes, SERDES_BIAS_EN, 0x17);
	static const uint8 kAux[] = {0x00, 0x13, 0xa4, 0x00, 0x0a, 0x26, 0x0a,
		0x03, 0xb7, 0x03};
	for (uint32 i = 0; i < sizeof(kAux); i++)
		wr(dp, DP_PHY_AUX_CFG0 + 4 * i, kAux[i]);
	wr(dp, DP_PHY_AUX_INTERRUPT_MASK, 0x1f);

	return _ConfigureRate(162);
}


/*!	The DP PLL and TX at \a rate (162, 270, 540 or 810 MHz: RBR to HBR3), the
	lanes as _PowerPhy() leaves them (qmp_combo_dp_power_on(),
	qmp_v456_configure_dp_phy(), qmp_v4_configure_dp_phy()). */
status_t
UsbcDisplayPort::_ConfigureRate(uint32 rate)
{
	int32 index = -1;
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
		if (kRates[i].rate == rate)
			index = i;
	}
	if (index < 0)
		return B_BAD_VALUE;
	volatile uint8* serdes = fPhy + kPhyDpSerdes;
	volatile uint8* dp = fPhy + kPhyDpPhy;
	volatile uint8* tx[2] = {fPhy + kPhyTxA, fPhy + kPhyTxB};

	for (const qmp_setting& setting : kDpSerdes)
		wr(serdes, setting.offset, setting.value);
	for (size_t i = 0; i < B_COUNT_OF(kRateRegisters); i++)
		wr(serdes, kRateRegisters[i], kRates[index].values[i]);
	fRate = rate;
	for (volatile uint8* lane : tx) {
		for (const qmp_setting& setting : kDpTx)
			wr(lane, setting.offset, setting.value);
		wr(lane, TX_DRV_LVL, 0x27);
		wr(lane, TX_EMP_POST1_LVL, 0x20);
	}
	_SetPhyLevels(0, 0);

	// qmp_combo_configure_dp_mode(): the lanes in use powered (lanes 0/1
	// for the reverse orientation, 2/3 for the normal one, all four)
	wr(dp, DP_PHY_CFG_1, 0x0f);
	uint32 powerDown = 0x01 | 0x04 | 0x20 | 0x40;
	if (fPhyLanes == 4 || fReverse)
		powerDown |= 0x08;
	if (fPhyLanes == 4 || !fReverse)
		powerDown |= 0x10;
	wr(dp, DP_PHY_PD_CTL, powerDown);
	wr(dp, DP_PHY_MODE, fReverse ? 0x4c : 0x5c);
	wr(dp, DP_PHY_AUX_CFG0 + 4, 0x13);
	wr(dp, DP_PHY_AUX_CFG0 + 8, 0xa4);
	wr(dp, DP_PHY_TX0_TX1_LANE_CTL, 0x05);
	wr(dp, DP_PHY_TX2_TX3_LANE_CTL, 0x05);
	wr(dp, DP_PHY_VCO_DIV, kRates[index].vcoDivider);
	wr(dp, DP_PHY_CFG, 0x01);
	wr(dp, DP_PHY_CFG, 0x05);
	wr(dp, DP_PHY_CFG, 0x01);
	wr(dp, DP_PHY_CFG, 0x09);
	wr(serdes, SERDES_RESETSM_CNTRL, 0x20);
	if (!wait_bits(serdes, SERDES_C_READY_STATUS, 1, 1)
		|| !wait_bits(serdes, SERDES_CMN_STATUS, 3, 3)) {
		ERROR("%s: the PHY's PLL does not lock at %" B_PRIu32 " MHz (cmn %#"
			B_PRIx32 ")\n", fConnector.name, rate,
			rd(serdes, SERDES_CMN_STATUS));
		return B_ERROR;
	}
	wr(dp, DP_PHY_CFG, 0x19);
	if (!wait_bits(dp, DP_PHY_STATUS, 3, 3))
		return B_ERROR;
	// qmp_v4_configure_dp_phy(): the TX blocks' drivers and biases for the
	// lanes in use
	uint32 bias[2] = {0x3f, 0x3f};
	if (fPhyLanes == 2) {
		bias[0] = fReverse ? 0x3f : 0x15;
		bias[1] = fReverse ? 0x15 : 0x3f;
	}
	for (int i = 0; i < 2; i++) {
		wr(tx[i], TX_HIGHZ_DRVR_EN, 0x10);
		wr(tx[i], TX_TRANSCEIVER_BIAS, bias[i]);
	}
	wr(dp, DP_PHY_CFG, 0x18);
	snooze(2000);
	wr(dp, DP_PHY_CFG, 0x19);
	if (!wait_bits(dp, DP_PHY_STATUS, 2, 2))
		return B_ERROR;
	for (volatile uint8* lane : tx) {
		wr(lane, TX_POL_INV, 0x0a);
		wr(lane, TX_DRV_LVL, 0x27);
		wr(lane, TX_EMP_POST1_LVL, 0x20);
	}
	return B_OK;
}


/*!	DP0's clocks: the link clock from the PHY, the AUX clock from XO, the
	pixel clock from the PHY's VCO divider (810 MHz at RBR) at 74.25 MHz
	(M/N 11/120), what the HDMI path's wide-bus 1080p timing needs for 60 Hz.
*/
status_t
UsbcDisplayPort::_Clocks()
{
	uint32 cfg = rd(fDispcc, fConnector.linkRcg + 4);
	wr(fDispcc, fConnector.linkRcg + 4, (cfg & ~(7u << 8 | 0x1fu))
		| fConnector.linkSource << 8);
	wr(fDispcc, fConnector.linkRcg, rd(fDispcc, fConnector.linkRcg) | 1);
	wait_bits(fDispcc, fConnector.linkRcg, 1, 0);
	wr(fDispcc, fConnector.auxRcg + 4, 0);
	wr(fDispcc, fConnector.auxRcg, rd(fDispcc, fConnector.auxRcg) | 1);
	wait_bits(fDispcc, fConnector.auxRcg, 1, 0);

	_SetPixelClock();

	if (!branch_on(fDispcc, fConnector.linkBranch)
		|| !branch_on(fDispcc, fConnector.auxBranch)
		|| !branch_on(fDispcc, fConnector.linkIntfBranch)
		|| !branch_on(fDispcc, fConnector.pixelBranch)) {
		ERROR("%s: its clocks do not run (link %#" B_PRIx32 " aux %#" B_PRIx32
			" pixel %#" B_PRIx32 ")\n", fConnector.name,
			rd(fDispcc, fConnector.linkBranch),
			rd(fDispcc, fConnector.auxBranch),
			rd(fDispcc, fConnector.pixelBranch));
		return B_ERROR;
	}
	return B_OK;
}


/*!	The pixel clock at 74.25 MHz from the PHY's VCO divider clock, which
	runs at 810 MHz at RBR and at 1350 MHz at HBR to HBR3
	(qmp_v4_configure_dp_clocks()): M/N 11/120 or 11/200. */
void
UsbcDisplayPort::_SetPixelClock()
{
	const uint32 m = 11, n = fRate == 162 ? 120 : 200, d = n;
	wr(fDispcc, fConnector.pixelRcg + 8, m);
	wr(fDispcc, fConnector.pixelRcg + 0xc, ~(n - m) & 0xffff);
	wr(fDispcc, fConnector.pixelRcg + 0x10, ~d & 0xffff);
	// dual edge
	wr(fDispcc, fConnector.pixelRcg + 4,
		fConnector.pixelSource << 8 | 2u << 12);
	wr(fDispcc, fConnector.pixelRcg, rd(fDispcc, fConnector.pixelRcg) | 1);
	wait_bits(fDispcc, fConnector.pixelRcg, 1, 0);
}


//!	The controller's AUX channel (msm_dp_aux): reset, limits, enabled.
void
UsbcDisplayPort::_EnableAux()
{
	const uint32 auxControl = 0x200 + 0x30;
	wr(fController, auxControl, rd(fController, auxControl) | 2);
	snooze(1000);
	wr(fController, auxControl, rd(fController, auxControl) & ~2u);
	wr(fController, 0x200 + 0x3c, 0xffff);		// timeout count
	wr(fController, 0x200 + 0x40, 0xffff);		// limits
	wr(fController, auxControl, rd(fController, auxControl) | 1);
}


void
UsbcDisplayPort::SetPortMode(bool reverse, uint32 lanes)
{
	fReverse = reverse;
	fPhyLanes = lanes == 2 ? 2 : 4;
}


status_t
UsbcDisplayPort::PowerUp()
{
	if (fPowered)
		return B_OK;
	status_t status = _Map(fGccArea, fGcc, "qcom usbc gcc", kGccBase, 0x1f4200);
	if (status == B_OK) {
		status = _Map(fDispccArea, fDispcc, "qcom usbc dispcc", kDispccBase,
			0x20000);
	}
	if (status == B_OK)
		status = _Map(fPhyArea, fPhy, "qcom usbc phy", fConnector.phyBase,
			0x4000);
	if (status == B_OK) {
		status = _Map(fControllerArea, fController, "qcom usbc dp",
			fConnector.controllerBase, 0x1000);
	}
	if (status == B_OK)
		status = _Map(fMdssArea, fMdss, "qcom usbc mdss", kMdssBase, kMdssSize);
	if (status != B_OK)
		return status;

	// the SBU mux on (gpio-sbu-mux: enable active low), select set for the
	// reverse orientation
	const uint32 kPins[] = {fConnector.sbuEnablePin, fConnector.sbuSelectPin};
	for (uint32 pin : kPins) {
		volatile uint8* tlmm;
		area_id area;
		if (_Map(area, tlmm, "qcom usbc sbu", kTlmmBase + pin * 0x1000,
				B_PAGE_SIZE) != B_OK)
			return B_NO_MEMORY;
		bool high = pin == fConnector.sbuSelectPin && fReverse;
		wr(tlmm, 4, high ? 2 : 0);
		wr(tlmm, 0, 1u << 9);		// GPIO, output, 2 mA
		delete_area(area);
	}

	status = _PowerPhy();
	if (status == B_OK)
		status = _Clocks();
	if (status != B_OK)
		return status;
	_EnableAux();
	fPowered = true;
	INFO("%s: PHY at RBR (%" B_PRIu32 " lanes, %s orientation), clocks and "
		"AUX on\n", fConnector.name, fPhyLanes, fReverse ? "reverse" : "normal");
	return B_OK;
}


/*!	VIG0 -> LM_0 -> CTL_0 -> INTF_0 as the firmware's VIG2 -> LM_2 -> CTL_2
	-> INTF_6, fetching \a width x \a height at \a address. */
status_t
UsbcDisplayPort::Start(phys_addr_t address, uint32 bytesPerRow, uint32 width,
	uint32 height, const display_timing& timing, bool wideBus)
{
	if (!fPowered)
		return B_NO_INIT;
	if ((rd(fMdss, kCtl2 + CTL_INTF_ACTIVE) & (1u << 6)) == 0
		|| (rd(fMdss, kCtl2 + CTL_FETCH_PIPE_ACTIVE) & (1u << 18)) == 0
		|| (rd(fMdss, kIntf6 + INTF_TIMING_ENGINE_EN) & 1) == 0) {
		ERROR("%s: the firmware's path is not VIG2 -> CTL_2 -> INTF_6\n",
			fConnector.name);
		return B_NOT_SUPPORTED;
	}
	uint32 interfaceBit = 1u << fConnector.interfaceNumber;
	if ((rd(fMdss, fControlBase + CTL_FETCH_PIPE_ACTIVE)
			& ~fConnector.pipeFetch) != 0
		|| (rd(fMdss, fControlBase + CTL_INTF_ACTIVE) & ~interfaceBit) != 0) {
		ERROR("%s: its control path is in use\n", fConnector.name);
		return B_BUSY;
	}

	for (uint32 offset = 0; offset < 0x200; offset += 4)
		wr(fMdss, fConnector.pipe + offset, rd(fMdss, kVig2 + offset));
	for (uint32 offset = 0xa00; offset < 0xb00; offset += 4)
		wr(fMdss, fConnector.pipe + offset, rd(fMdss, kVig2 + offset));
	for (uint32 offset = 0; offset < 0x100; offset += 4)
		wr(fMdss, fMixerBase + offset, rd(fMdss, kLm2 + offset));

	// VIG2's stage in LM_2's layer (bits 8:6, extension bit 4) as VIG0's
	// (bits 2:0, extension bit 0), and the border out bit (dpu_hw_ctl.c)
	uint32 layer2 = rd(fMdss, kCtl2 + CTL_LAYER_LM2);
	uint32 layerExt2 = rd(fMdss, kCtl2 + CTL_LAYER_EXT_LM2);
	wr(fMdss, fControlBase + CTL_TOP, rd(fMdss, kCtl2 + CTL_TOP));
	wr(fMdss, fControlBase + fLayer, (layer2 & (1u << 24))
		| ((layer2 >> 6) & 7) << fConnector.layerShift);
	wr(fMdss, fControlBase + fLayerExt,
		((layerExt2 >> 4) & 1) << fConnector.layerExtShift);
	wr(fMdss, fControlBase + CTL_INTF_ACTIVE, interfaceBit);
	wr(fMdss, fControlBase + CTL_FETCH_PIPE_ACTIVE, fConnector.pipeFetch);

	for (uint32 offset : kIntfTiming)
		wr(fMdss, fConnector.interface + offset, rd(fMdss, kIntf6 + offset));
	wr(fMdss, fConnector.interface + INTF_FRAME_LINE_COUNT_EN, 3);
	// the panel format and programmable fetch as the HDMI path's, and the
	// INTF fed by our mixer's ping-pong block (LM_n -> PINGPONG_n;
	// dpu_hw_intf_bind_pingpong_blk()): after a reset INTF_MUX says none,
	// and the timing engine runs frames without pixels
	wr(fMdss, fConnector.interface + INTF_PANEL_FORMAT,
		rd(fMdss, kIntf6 + INTF_PANEL_FORMAT));
	wr(fMdss, fConnector.interface + INTF_PROG_FETCH_START,
		rd(fMdss, kIntf6 + INTF_PROG_FETCH_START));
	// two the firmware sets that Linux does not name (0x08c 0x11, 0x1c4
	// 0xa00 on its output)
	wr(fMdss, fConnector.interface + 0x08c, rd(fMdss, kIntf6 + 0x08c));
	wr(fMdss, fConnector.interface + 0x1c4, rd(fMdss, kIntf6 + 0x1c4));
	wr(fMdss, fConnector.interface + INTF_MUX,
		(rd(fMdss, kIntf6 + INTF_MUX) & ~0xfu) | fConnector.mixer);

	status_t status = SetScanout(address, bytesPerRow, width, height);
	if (status != B_OK)
		return status;
	wr(fMdss, fControlBase + CTL_INTF_FLUSH, interfaceBit);
	wr(fMdss, fControlBase + CTL_FLUSH, fConnector.pipeFlush
		| fConnector.mixerFlush | kFlushCtl | kFlushIntf);
	wr(fMdss, kInterruptClear, fConnector.underrun);
	wr(fMdss, fConnector.interface + INTF_TIMING_ENGINE_EN, 1);
	fRunning = true;
	LogState("started");
	_Link(timing, wideBus);
	return B_OK;
}


// the controller's link block (dp_reg.h), from its base
enum {
	DP_MAINLINK_CTRL = 0x400, DP_STATE_CTRL = 0x404,
	DP_CONFIGURATION_CTRL = 0x408, DP_LANE_MAPPING = 0x438,
	DP_MAINLINK_READY = 0x440,
};
// the PHY's swing and pre-emphasis for RBR and HBR, by level
// (qmp_dp_v5_voltage_swing_hbr_rbr, qmp_dp_v5_pre_emphasis_hbr_rbr)
static const uint8 kSwingLow[4][4] = {
	{0x28, 0x2f, 0x36, 0x3f}, {0x31, 0x3e, 0x3f, 0xff},
	{0x36, 0x3f, 0xff, 0xff}, {0x3f, 0xff, 0xff, 0xff}};
static const uint8 kPreEmphasisLow[4][4] = {
	{0x20, 0x2d, 0x34, 0x3a}, {0x20, 0x2e, 0x35, 0xff},
	{0x20, 0x2e, 0xff, 0xff}, {0x24, 0xff, 0xff, 0xff}};
// for HBR2 and HBR3 (qmp_dp_v5_voltage_swing_hbr3_hbr2, _pre_emphasis_)
static const uint8 kSwingHigh[4][4] = {
	{0x22, 0x32, 0x36, 0x3a}, {0x29, 0x39, 0x3f, 0xff},
	{0x30, 0x3f, 0xff, 0xff}, {0x3f, 0xff, 0xff, 0xff}};
static const uint8 kPreEmphasisHigh[4][4] = {
	{0x20, 0x2c, 0x35, 0x3b}, {0x22, 0x2e, 0x36, 0xff},
	{0x22, 0x31, 0xff, 0xff}, {0x24, 0xff, 0xff, 0xff}};


//!	msm_dp_ctrl_config_ctrl(), msm_dp_ctrl_lane_mapping(),
//	msm_dp_ctrl_mainlink_enable(): 8 bits a component, RGB.
void
UsbcDisplayPort::_EnableMainlink(uint32 lanes)
{
	uint32 config = 2u << 13		// LSCLK = link clock / 4
		| 1u << 8					// 8 bpc
		| (lanes - 1) << 4
		| 0x40						// enhanced framing
		| 0x04 | 0x02 | 0x01;		// progressive, static Mvid, sync clock
	wr(fController, DP_CONFIGURATION_CTRL, config);
	wr(fController, DP_LANE_MAPPING, 0 | 1u << 2 | 2u << 4 | 3u << 6);
	uint32 control = rd(fController, DP_MAINLINK_CTRL) & ~3u;
	wr(fController, DP_MAINLINK_CTRL, control);
	wr(fController, DP_MAINLINK_CTRL, control | 2);			// reset
	wr(fController, DP_MAINLINK_CTRL, control);
	wr(fController, DP_MAINLINK_CTRL, control | 1 | 0x02000000);
		// enable, frame boundary select
}


//!	Training pattern \a pattern (1..4) on the link, the controller saying
//	it sends it (msm_dp_ctrl_set_pattern_state_bit()).
status_t
UsbcDisplayPort::_SendPattern(uint32 pattern)
{
	wr(fController, DP_STATE_CTRL, 1u << (pattern - 1));
	uint32 ready = 1u << (pattern - 1 + 3);
	for (int i = 0; i < 500; i++) {
		if ((rd(fController, DP_MAINLINK_READY) & ready) != 0)
			return B_OK;
		spin(10);
	}
	return B_TIMED_OUT;
}


//!	The PHY's levels on both TX blocks (qmp_combo_configure_dp_swing()).
status_t
UsbcDisplayPort::_SetPhyLevels(uint32 swing, uint32 preEmphasis)
{
	bool high = fRate > 270;
	const uint8 (*swings)[4] = high ? kSwingHigh : kSwingLow;
	const uint8 (*emphases)[4] = high ? kPreEmphasisHigh : kPreEmphasisLow;
	if (swing > 3 || preEmphasis > 3 || swings[swing][preEmphasis] == 0xff)
		return B_BAD_VALUE;
	static const uint32 kLanes[] = {kPhyTxA, kPhyTxB};
	for (uint32 lane : kLanes) {
		wr(fPhy + lane, TX_DRV_LVL, swings[swing][preEmphasis] | 0x20);
		wr(fPhy + lane, TX_EMP_POST1_LVL, emphases[swing][preEmphasis] | 0x20);
	}
	return B_OK;
}


//!	The PHY's levels, and the sink told (DPCD 0x103).
status_t
UsbcDisplayPort::_SetLevels(uint32 swing, uint32 preEmphasis, uint32 lanes)
{
	status_t status = _SetPhyLevels(swing, preEmphasis);
	if (status != B_OK)
		return status;
	uint8 set[4];
	for (uint32 i = 0; i < 4; i++) {
		set[i] = swing | preEmphasis << 3
			| (swing == 3 ? 1u << 2 : 0) | (preEmphasis == 3 ? 1u << 5 : 0);
	}
	return fPort->WriteDpcd(0x103, set, lanes);
}


/*!	Link training at RBR with the sink (msm_dp_ctrl_link_train()): its
	capabilities, the link set, clock recovery on TPS1, channel equalization
	on TPS2 or TPS3, the swing and pre-emphasis the sink asks for. */
status_t
UsbcDisplayPort::_Train(uint32* _lanes)
{
	uint8 caps[16];
	status_t status = fPort->ReadDpcd(0x000, caps, sizeof(caps));
	if (status != B_OK)
		return status;
	// the sink (or branch device) in D0 (msm_dp_link_psm_config(),
	// DP_SET_POWER)
	uint8 power;
	if (fPort->ReadDpcd(0x600, &power, 1) == B_OK && (power & 7) != 1) {
		power = (power & ~7u) | 1;
		fPort->WriteDpcd(0x600, &power, 1);
		snooze(1000);
	}
	uint32 lanes = min_c(caps[2] & 0x1fu, fPhyLanes);
	if (lanes != 1 && lanes != 2 && lanes != 4)
		return B_BAD_DATA;
	bool enhanced = (caps[2] & 0x80) != 0, tps3 = (caps[2] & 0x40) != 0;
	// the read interval for the status (TRAINING_AUX_RD_INTERVAL)
	uint32 interval = caps[14] & 0x7f;
	bigtime_t crDelay = interval == 0 ? 100 : interval * 4000;
	bigtime_t eqDelay = interval == 0 ? 400 : interval * 4000;
	INFO("%s: sink DPCD %#x, max rate %#x, %" B_PRIu32 " lanes%s%s\n",
		fConnector.name,
		caps[0], caps[1], lanes, enhanced ? ", enhanced framing" : "",
		tps3 ? ", TPS3" : "");
	*_lanes = lanes;

	// the sink's highest rate first, lower ones when that does not train
	status = B_NOT_SUPPORTED;
	for (int32 i = B_COUNT_OF(kRates) - 1; i >= 0; i--) {
		if (kRates[i].dpcd > caps[1])
			continue;
		// HBR3 needs TPS3 at least; TPS4 is not used
		if (kRates[i].rate == 810 && !tps3)
			continue;
		if (fRate != kRates[i].rate) {
			status = _ConfigureRate(kRates[i].rate);
			if (status != B_OK)
				continue;
		}
		status = _TrainAt(kRates[i].rate, lanes, enhanced, tps3, crDelay,
			eqDelay);
		if (status == B_OK)
			return B_OK;
	}
	return status;
}


status_t
UsbcDisplayPort::_TrainAt(uint32 rate, uint32 lanes, bool enhanced, bool tps3,
	bigtime_t crDelay, bigtime_t eqDelay)
{
	uint8 bandwidth = 0;
	for (size_t i = 0; i < B_COUNT_OF(kRates); i++) {
		if (kRates[i].rate == rate)
			bandwidth = kRates[i].dpcd;
	}
	_EnableMainlink(lanes);
	uint8 link[2] = {bandwidth, (uint8)(lanes | (enhanced ? 0x80 : 0))};
	uint8 coding[2] = {0, 1};			// no down spread, 8b/10b
	if (fPort->WriteDpcd(0x100, link, 2) != B_OK
		|| fPort->WriteDpcd(0x107, coding, 2) != B_OK)
		return B_IO_ERROR;

	status_t status = B_OK;
	uint32 swing = 0, preEmphasis = 0;
	bool done = false;
	for (int phase = 1; phase <= 2 && status == B_OK; phase++) {
		uint32 pattern = phase == 1 ? 1 : tps3 ? 3 : 2;
		status = _SendPattern(pattern);
		uint8 set = pattern | 0x20;		// scrambling off
		if (status == B_OK)
			status = fPort->WriteDpcd(0x102, &set, 1);
		if (status == B_OK)
			status = _SetLevels(swing, preEmphasis, lanes);
		done = false;
		for (int attempt = 0; attempt < 6 && status == B_OK; attempt++) {
			snooze(phase == 1 ? crDelay : eqDelay);
			uint8 lane[6];
			status = fPort->ReadDpcd(0x202, lane, sizeof(lane));
			if (status != B_OK)
				break;
			// per lane: clock recovery bit 0, equalization 1, symbol lock 2
			uint32 need = phase == 1 ? 0x1 : 0x7;
			done = phase == 1 || (lane[2] & 1) != 0;	// interlane align
			for (uint32 i = 0; i < lanes; i++) {
				if (((lane[i / 2] >> (4 * (i % 2))) & need) != need)
					done = false;
			}
			if (done)
				break;
			// what the sink asks for: the highest of the lanes
			uint32 wantSwing = 0, wantPre = 0;
			for (uint32 i = 0; i < lanes; i++) {
				uint8 adjust = lane[4 + i / 2] >> (4 * (i % 2));
				wantSwing = max_c(wantSwing, (uint32)(adjust & 3));
				wantPre = max_c(wantPre, (uint32)((adjust >> 2) & 3));
			}
			if (wantSwing != swing || wantPre != preEmphasis) {
				swing = wantSwing;
				preEmphasis = wantPre;
				status = _SetLevels(swing, preEmphasis, lanes);
			}
		}
		if (status == B_OK && !done)
			status = B_TIMED_OUT;
		INFO("%s: training at %" B_PRIu32 " MHz, %s: %s, swing %" B_PRIu32
			" pre-emphasis %" B_PRIu32 "\n", fConnector.name, rate,
			phase == 1 ? "clock recovery" : "equalization", strerror(status),
			swing, preEmphasis);
	}
	uint8 off = 0;
	fPort->WriteDpcd(0x102, &off, 1);
	wr(fController, DP_STATE_CTRL, 0);
	return status;
}


/*!	The link and the stream: trained with the sink if one answers; if not,
	the controller's training patterns checked, and the stream started on
	an untrained link (where it goes nowhere), so that path is known too. */
/*!	Lab ("usbc_tpg true"): the controller's own test pattern, checkered
	rectangles (msm_dp_panel_tpg_enable()): on the screen, the link and its
	main stream attributes are right, whatever the DPU sends. */
void
UsbcDisplayPort::_TestPattern(const display_timing& timing)
{
	volatile uint8* p0;
	area_id area = map_physical_memory("qcom usbc dp tpg",
		fConnector.controllerBase + 0x1000, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&p0);
	if (area < 0)
		return;
	uint32 hPeriod = timing.h_total, vPeriod = timing.v_total;
	uint32 vStart = (timing.v_total - timing.v_sync_start) * hPeriod
		+ timing.h_total - timing.h_sync_start;
	uint32 vEnd = (vPeriod - (timing.v_sync_start - timing.v_display))
		* hPeriod - 1 - (timing.h_sync_start - timing.h_display);
	uint32 hStartX = timing.h_total - timing.h_sync_start;
	uint32 hEndX = hPeriod - (timing.h_sync_start - timing.h_display) - 1;
	wr(p0, 0x18, hPeriod << 16 | (timing.h_sync_end - timing.h_sync_start));
	wr(p0, 0x1c, vPeriod * hPeriod);
	wr(p0, 0x24, (timing.v_sync_end - timing.v_sync_start) * hPeriod);
	wr(p0, 0x20, 0);
	wr(p0, 0x28, 0);
	wr(p0, 0x4c, hEndX << 16 | hStartX);
	wr(p0, 0x50, 0);
	wr(p0, 0x2c, vStart);
	wr(p0, 0x34, vEnd);
	static const uint32 kZeroed[] = {0x30, 0x38, 0x3c, 0x40, 0x44, 0x48, 0x58};
	for (uint32 offset : kZeroed)
		wr(p0, offset, 0);
	wr(p0, 0x60, 0x100);		// checkered rectangles
	wr(p0, 0x64, 1 | 4);		// 8 bits, RGB
	wr(p0, 0x00, 1);			// BIST
	wr(p0, 0x10, 1);			// its timing engine
	INFO("%s: test pattern on\n", fConnector.name);
	delete_area(area);
}


/*!	Lab: the controller's link and pixel interface registers next to the
	HDMI output's (DP2, which the firmware set up and which works), the
	ones that differ. */
void
UsbcDisplayPort::_DumpAgainstHdmi()
{
	volatile uint8* ours;
	volatile uint8* hdmi;
	area_id oursArea = map_physical_memory("qcom usbc dp dump",
		fConnector.controllerBase, 0x2000, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA, (void**)&ours);
	area_id hdmiArea = map_physical_memory("qcom hdmi dp dump", 0xae9a000,
		0x2000, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, (void**)&hdmi);
	if (oursArea >= 0 && hdmiArea >= 0) {
		static const struct { uint32 start, end; } kRanges[] = {
			{0x400, 0x5fc}, {0x1000, 0x10fc}};
		for (const auto& range : kRanges) {
			for (uint32 offset = range.start; offset <= range.end;
					offset += 4) {
				uint32 a = rd(ours, offset), b = rd(hdmi, offset);
				if (a != b) {
					INFO("%s: %#05" B_PRIx32 ": %#010" B_PRIx32 " (HDMI %#010"
						B_PRIx32 ")\n", fConnector.name, offset, a, b);
				}
			}
		}
	}
	if (oursArea >= 0)
		delete_area(oursArea);
	if (hdmiArea >= 0)
		delete_area(hdmiArea);
	// our INTF against the HDMI output's (INTF_6)
	if (fMdss != NULL) {
		for (uint32 offset = 0; offset < 0x280; offset += 4) {
			uint32 a = rd(fMdss, fConnector.interface + offset);
			uint32 b = rd(fMdss, kMdp + 0x3a000 + offset);
			if (a != b) {
				INFO("%s: INTF +%#05" B_PRIx32 ": %#010" B_PRIx32 " (INTF_6 %#010"
					B_PRIx32 ")\n", fConnector.name, offset, a, b);
			}
		}
	}
}


void
UsbcDisplayPort::Attention()
{
	if (fPort == NULL || !fRunning)
		return;
	// DPCD 0x200: sink count, the device service IRQ vector, lane 0/1 and
	// 2/3 status, alignment, sink status
	uint8 status[6];
	if (fPort->ReadDpcd(0x200, status, sizeof(status)) != B_OK) {
		INFO("%s: attention, but no AUX answer\n", fConnector.name);
		return;
	}
	if (status[1] != 0)
		fPort->WriteDpcd(0x201, &status[1], 1);
	// each lane in use: clock recovery, equalization, symbol lock
	bool locked = (status[4] & 1) != 0;
	for (uint32 i = 0; i < fLanes; i++) {
		if (((status[2 + i / 2] >> (4 * (i % 2))) & 7) != 7)
			locked = false;
	}
	locked = locked && fTrained;
	INFO("%s: attention: sink count %#x, IRQ %#x, lanes %#x %#x, align %#x, "
		"sink %#x: %s\n", fConnector.name, status[0], status[1], status[2],
		status[3], status[4], status[5],
		locked ? "link fine" : "training again");
	if (!locked && fRetrainings < 3) {
		fRetrainings++;
		_Link(fTiming, fWideBus);
	}
}


void
UsbcDisplayPort::_Link(const display_timing& timing, bool wideBus)
{
	fTiming = timing;
	fWideBus = wideBus;
	if (fPort == NULL) {
		fPort = new(std::nothrow) DisplayPort(fConnector.controller);
		if (fPort == NULL || fPort->Init() != B_OK) {
			ERROR("%s: no AUX channel\n", fConnector.name);
			delete fPort;
			fPort = NULL;
			return;
		}
	}
	uint32 lanes = fPhyLanes;
	status_t status = _Train(&lanes);
	fTrained = status == B_OK;
	if (fTrained)
		fRetrainings = 0;
	if (status != B_OK) {
		INFO("%s: link not trained (%s)\n", fConnector.name, strerror(status));
		// what can be known without a sink: the PLL at each rate
		for (size_t i = 1; i < B_COUNT_OF(kRates); i++) {
			INFO("%s: PLL at %" B_PRIu32 " MHz: %s\n", fConnector.name,
				kRates[i].rate,
				_ConfigureRate(kRates[i].rate) == B_OK ? "locked" : "no lock");
		}
		_ConfigureRate(162);
		_EnableMainlink(fPhyLanes);
		lanes = fPhyLanes;
		for (uint32 pattern = 1; pattern <= 3; pattern++) {
			INFO("%s: training pattern %" B_PRIu32 ": %s\n", fConnector.name,
				pattern,
				_SendPattern(pattern) == B_OK ? "sent" : "not sent");
		}
		wr(fController, DP_STATE_CTRL, 0);
	}
	fLanes = lanes;
	_SetPixelClock();
	fPort->SetLinkRate(fRate);
	status = fPort->SetStream(timing, wideBus);
	if (status == B_OK)
		status = fPort->StartStream();
	// lab: the registers the firmware sets on its output (DP2) that Linux
	// does not name or leaves at their reset values, copied
	{
		volatile uint8* hdmi;
		area_id hdmiArea = map_physical_memory("qcom hdmi dp copy", 0xae9a000,
			0x2000, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, (void**)&hdmi);
		volatile uint8* ours;
		area_id oursArea = map_physical_memory("qcom usbc dp copy",
			fConnector.controllerBase, 0x2000, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&ours);
		if (hdmiArea >= 0 && oursArea >= 0) {
			static const uint32 kCopied[] = {0x450, 0x454, 0x580, 0x1084};
			for (uint32 offset : kCopied)
				wr(ours, offset, rd(hdmi, offset));
		}
		if (hdmiArea >= 0)
			delete_area(hdmiArea);
		if (oursArea >= 0)
			delete_area(oursArea);
	}
	_DumpAgainstHdmi();
	void* settings = load_driver_settings("qcom_display");
	bool testPattern = get_driver_boolean_parameter(settings, "usbc_tpg",
		false, true);
	unload_driver_settings(settings);
	if (testPattern)
		_TestPattern(timing);
	uint8 sinkStatus[6] = {};
	snooze(200000);
	fPort->ReadDpcd(0x200, sinkStatus, sizeof(sinkStatus));
	INFO("%s: 200 ms later: sink count %#x, lanes %#x, align %#x, sink "
		"status %#x\n", fConnector.name, sinkStatus[0], sinkStatus[2],
		sinkStatus[4], sinkStatus[5]);
	INFO("%s: stream on %" B_PRIu32 " lanes at %" B_PRIu32 " MHz: %s "
		"(mainlink ready %#" B_PRIx32 ")\n", fConnector.name, lanes, fRate,
		strerror(status),
		rd(fController, DP_MAINLINK_READY));
}


status_t
UsbcDisplayPort::SetScanout(phys_addr_t address, uint32 bytesPerRow,
	uint32 width, uint32 height)
{
	if (fMdss == NULL)
		return B_NO_INIT;
	// the firmware's mode is the one INTF_0 runs: the region must fill it
	uint32 size = rd(fMdss, kLm2 + LM_OUT_SIZE);
	if (width != (size & 0xffff) || height != size >> 16
		|| address + (phys_addr_t)bytesPerRow * height > (1ull << 32)) {
		return B_BAD_VALUE;
	}
	uint32 sizes = height << 16 | width;
	wr(fMdss, fConnector.pipe + SSPP_SRC_SIZE, sizes);
	wr(fMdss, fConnector.pipe + SSPP_SRC_XY, 0);
	wr(fMdss, fConnector.pipe + SSPP_OUT_SIZE, sizes);
	wr(fMdss, fConnector.pipe + SSPP_OUT_XY, 0);
	wr(fMdss, fConnector.pipe + SSPP_SRC0_ADDR, (uint32)address);
	wr(fMdss, fConnector.pipe + SSPP_SRC_YSTRIDE0, bytesPerRow);
	if (fRunning)
		wr(fMdss, fControlBase + CTL_FLUSH, fConnector.pipeFlush);
	return B_OK;
}


void
UsbcDisplayPort::Stop()
{
	if (!fRunning || fMdss == NULL)
		return;
	wr(fMdss, fConnector.interface + INTF_TIMING_ENGINE_EN, 0);
	wr(fMdss, fControlBase + fLayer, 0);
	wr(fMdss, fControlBase + fLayerExt, 0);
	wr(fMdss, fControlBase + CTL_INTF_ACTIVE, 0);
	wr(fMdss, fControlBase + CTL_FETCH_PIPE_ACTIVE, 0);
	wr(fMdss, fControlBase + CTL_FLUSH, fConnector.pipeFlush
		| fConnector.mixerFlush | kFlushCtl);
	fRunning = false;
}


void
UsbcDisplayPort::LogState(const char* when)
{
	if (fMdss == NULL)
		return;
	wr(fMdss, kInterruptClear, fConnector.underrun);
	uint32 frames = rd(fMdss, fConnector.interface + INTF_FRAME_COUNT);
	snooze(100000);
	frames = rd(fMdss, fConnector.interface + INTF_FRAME_COUNT) - frames;
	INFO("%s %s: timing engine %s, %" B_PRIu32 " frames in 100 ms, flush %#"
		B_PRIx32
		", underrun %s, pipe %#" B_PRIx32 " at %#" B_PRIx32 " stride %#"
		B_PRIx32 "\n", fConnector.name, when,
		(rd(fMdss, fConnector.interface + INTF_TIMING_ENGINE_EN) & 1) != 0
			? "on" : "off",
		frames, rd(fMdss, fControlBase + CTL_FLUSH),
		(rd(fMdss, kInterruptStatus) & fConnector.underrun) != 0 ? "yes" : "no",
		rd(fMdss, fConnector.pipe + SSPP_SRC_SIZE),
		rd(fMdss, fConnector.pipe + SSPP_SRC0_ADDR),
		rd(fMdss, fConnector.pipe + SSPP_SRC_YSTRIDE0));
}


}	// namespace qcom
