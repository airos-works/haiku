/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "usbc_dp.h"

#include "display_port.h"

#include <new>
#include <string.h>

#include <KernelExport.h>
#include <arch/atomic.h>


#define ERROR(x...)	dprintf("qcom_display: " x)
#define INFO(x...)	dprintf("qcom_display: " x)


namespace qcom {


// USB-C connector 0 (gcc-sc8280xp.c, dispcc-sc8280xp.c, the device tree)
static const phys_addr_t kGccBase = 0x100000;
static const uint32 kGdsc = 0xf004;						// USB30_PRIM_GDSC
static const uint32 kPhyBranches[] = {0xf054, 0x8c02c, 0xf058};
	// aux, ref (usb4 eud clkref), com aux
static const uint32 kPhyResets[] = {0x50000, 0x4a008};
	// GCC_USB3_PHY_PRIM_BCR, GCC_USB4_DP_PHY_PRIM_BCR
static const phys_addr_t kPhyBase = 0x88eb000;
static const phys_addr_t kDispccBase = 0xaf00000;
static const uint32 kLinkRcg = 0x21a4, kLinkSource = 1, kLinkBranch = 0x205c;
static const uint32 kLinkIntfBranch = 0x2060;
static const uint32 kAuxRcg = 0x2238, kAuxBranch = 0x206c;
static const uint32 kPixelRcg = 0x21d8, kPixelSource = 2, kPixelBranch = 0x2070;
static const phys_addr_t kControllerBase = 0xae90000;
static const phys_addr_t kTlmmBase = 0xf100000;
static const uint32 kSbuEnablePin = 101, kSbuSelectPin = 164;

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

// RBR (qmp_v4_dp_serdes_tbl_rbr)
static const qmp_setting kDpSerdesRbr[] = {
	{0x158, 0x05}, {0x0bc, 0x69}, {0x0d0, 0x80}, {0x0d4, 0x07},
	{0x0ac, 0x6f}, {0x0b0, 0x08}, {0x0a4, 0x04},
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
static const uint32 kMdp = 0x1000;
static const uint32 kVig0 = kMdp + 0x4000, kVig2 = kMdp + 0x8000;
static const uint32 kLm0 = kMdp + 0x44000, kLm2 = kMdp + 0x46000;
static const uint32 kCtl0 = kMdp + 0x15000, kCtl2 = kMdp + 0x17000;
static const uint32 kIntf0 = kMdp + 0x34000, kIntf6 = kMdp + 0x3a000;
static const uint32 kInterruptStatus = kMdp + 0x14;
static const uint32 kInterruptClear = kMdp + 0x18;
static const uint32 kUnderrunIntf0 = 1u << 24;

enum {
	CTL_LAYER_LM0 = 0x00, CTL_LAYER_LM2 = 0x08, CTL_TOP = 0x14,
	CTL_FLUSH = 0x18, CTL_LAYER_EXT_LM0 = 0x40, CTL_LAYER_EXT_LM2 = 0x48,
	CTL_INTF_ACTIVE = 0xf4, CTL_FETCH_PIPE_ACTIVE = 0xfc,
	CTL_INTF_FLUSH = 0x110,

	SSPP_SRC_SIZE = 0x00, SSPP_SRC_XY = 0x08, SSPP_OUT_SIZE = 0x0c,
	SSPP_OUT_XY = 0x10, SSPP_SRC0_ADDR = 0x14, SSPP_SRC_YSTRIDE0 = 0x24,

	LM_OUT_SIZE = 0x04,

	INTF_TIMING_ENGINE_EN = 0x000, INTF_FRAME_LINE_COUNT_EN = 0x0a8,
	INTF_FRAME_COUNT = 0x0ac,
};
static const uint32 kFlushVig0 = 1u << 0, kFlushLm0 = 1u << 6;
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


UsbcDisplayPort::UsbcDisplayPort()
	:
	fGccArea(-1), fGcc(NULL), fDispccArea(-1), fDispcc(NULL), fPhyArea(-1),
	fPhy(NULL), fControllerArea(-1), fController(NULL), fPort(NULL),
	fMdssArea(-1), fMdss(NULL), fPowered(false), fRunning(false)
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
	uint32 gdsc = rd(fGcc, kGdsc);
	if ((gdsc & 1) != 0)
		wr(fGcc, kGdsc, gdsc & ~1u);
	if (!wait_bits(fGcc, kGdsc, 1u << 31, 1u << 31)) {
		ERROR("DP-1: USB30_PRIM_GDSC does not come on\n");
		return B_ERROR;
	}
	for (uint32 branch : kPhyBranches)
		branch_on(fGcc, branch);
	for (uint32 reset : kPhyResets)
		wr(fGcc, reset, rd(fGcc, reset) | 1);
	spin(200);
	for (uint32 reset : kPhyResets)
		wr(fGcc, reset, rd(fGcc, reset) & ~1u);
	spin(200);

	volatile uint8* com = fPhy + kPhyCom;
	volatile uint8* serdes = fPhy + kPhyDpSerdes;
	volatile uint8* dp = fPhy + kPhyDpPhy;
	volatile uint8* tx[2] = {fPhy + kPhyTxA, fPhy + kPhyTxB};

	wr(com, COM_POWER_DOWN_CTRL, rd(com, COM_POWER_DOWN_CTRL) | 1);
	wr(com, COM_RESET_OVRD_CTRL, rd(com, COM_RESET_OVRD_CTRL) | 0xf);
	wr(com, COM_TYPEC_CTRL, 1u << 1);		// software port select, normal
	wr(com, COM_PHY_MODE_CTRL, 3);			// USB3 + DP
	wr(com, COM_RESET_OVRD_CTRL, rd(com, COM_RESET_OVRD_CTRL) & ~0xfu);
	wr(com, COM_SWI_CTRL, rd(com, COM_SWI_CTRL) & ~3u);
	wr(com, COM_SW_RESET, rd(com, COM_SW_RESET) & ~1u);

	// the AUX block
	wr(dp, DP_PHY_PD_CTL, 0x01 | 0x02 | 0x04 | 0x20 | 0x40);
	wr(serdes, SERDES_BIAS_EN, 0x17);
	static const uint8 kAux[] = {0x00, 0x13, 0xa4, 0x00, 0x0a, 0x26, 0x0a,
		0x03, 0xb7, 0x03};
	for (uint32 i = 0; i < sizeof(kAux); i++)
		wr(dp, DP_PHY_AUX_CFG0 + 4 * i, kAux[i]);
	wr(dp, DP_PHY_AUX_INTERRUPT_MASK, 0x1f);

	for (const qmp_setting& setting : kDpSerdes)
		wr(serdes, setting.offset, setting.value);
	for (const qmp_setting& setting : kDpSerdesRbr)
		wr(serdes, setting.offset, setting.value);
	for (volatile uint8* lane : tx) {
		for (const qmp_setting& setting : kDpTx)
			wr(lane, setting.offset, setting.value);
		wr(lane, TX_DRV_LVL, 0x28 | 0x20);	// swing, pre-emphasis level 0
		wr(lane, TX_EMP_POST1_LVL, 0x20 | 0x20);
	}

	wr(dp, DP_PHY_CFG_1, 0x0f);
	wr(dp, DP_PHY_PD_CTL, 0x01 | 0x04 | 0x20 | 0x40 | 0x08 | 0x10);
	wr(dp, DP_PHY_MODE, 0x5c);
	wr(dp, DP_PHY_AUX_CFG0 + 4, 0x13);
	wr(dp, DP_PHY_AUX_CFG0 + 8, 0xa4);
	wr(dp, DP_PHY_TX0_TX1_LANE_CTL, 0x05);
	wr(dp, DP_PHY_TX2_TX3_LANE_CTL, 0x05);
	wr(dp, DP_PHY_VCO_DIV, 0x1);			// RBR
	wr(dp, DP_PHY_CFG, 0x01);
	wr(dp, DP_PHY_CFG, 0x05);
	wr(dp, DP_PHY_CFG, 0x01);
	wr(dp, DP_PHY_CFG, 0x09);
	wr(serdes, SERDES_RESETSM_CNTRL, 0x20);
	if (!wait_bits(serdes, SERDES_C_READY_STATUS, 1, 1)
		|| !wait_bits(serdes, SERDES_CMN_STATUS, 3, 3)) {
		ERROR("DP-1: the PHY's PLL does not lock (cmn %#" B_PRIx32 ")\n",
			rd(serdes, SERDES_CMN_STATUS));
		return B_ERROR;
	}
	wr(dp, DP_PHY_CFG, 0x19);
	if (!wait_bits(dp, DP_PHY_STATUS, 3, 3))
		return B_ERROR;
	for (volatile uint8* lane : tx) {
		wr(lane, TX_HIGHZ_DRVR_EN, 0x10);
		wr(lane, TX_TRANSCEIVER_BIAS, 0x3f);
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
	uint32 cfg = rd(fDispcc, kLinkRcg + 4);
	wr(fDispcc, kLinkRcg + 4, (cfg & ~(7u << 8 | 0x1fu)) | kLinkSource << 8);
	wr(fDispcc, kLinkRcg, rd(fDispcc, kLinkRcg) | 1);
	wait_bits(fDispcc, kLinkRcg, 1, 0);
	wr(fDispcc, kAuxRcg + 4, 0);
	wr(fDispcc, kAuxRcg, rd(fDispcc, kAuxRcg) | 1);
	wait_bits(fDispcc, kAuxRcg, 1, 0);

	const uint32 m = 11, n = 120, d = n;
	wr(fDispcc, kPixelRcg + 8, m);
	wr(fDispcc, kPixelRcg + 0xc, ~(n - m) & 0xffff);
	wr(fDispcc, kPixelRcg + 0x10, ~d & 0xffff);
	wr(fDispcc, kPixelRcg + 4, kPixelSource << 8 | 2u << 12);	// dual edge
	wr(fDispcc, kPixelRcg, rd(fDispcc, kPixelRcg) | 1);
	wait_bits(fDispcc, kPixelRcg, 1, 0);

	if (!branch_on(fDispcc, kLinkBranch) || !branch_on(fDispcc, kAuxBranch)
		|| !branch_on(fDispcc, kLinkIntfBranch)
		|| !branch_on(fDispcc, kPixelBranch)) {
		ERROR("DP-1: its clocks do not run (link %#" B_PRIx32 " aux %#" B_PRIx32
			" pixel %#" B_PRIx32 ")\n", rd(fDispcc, kLinkBranch),
			rd(fDispcc, kAuxBranch), rd(fDispcc, kPixelBranch));
		return B_ERROR;
	}
	return B_OK;
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


status_t
UsbcDisplayPort::PowerUp()
{
	if (fPowered)
		return B_OK;
	status_t status = _Map(fGccArea, fGcc, "qcom dp1 gcc", kGccBase, 0x1f4200);
	if (status == B_OK) {
		status = _Map(fDispccArea, fDispcc, "qcom dp1 dispcc", kDispccBase,
			0x20000);
	}
	if (status == B_OK)
		status = _Map(fPhyArea, fPhy, "qcom dp1 phy", kPhyBase, 0x4000);
	if (status == B_OK) {
		status = _Map(fControllerArea, fController, "qcom dp1", kControllerBase,
			0x1000);
	}
	if (status == B_OK)
		status = _Map(fMdssArea, fMdss, "qcom dp1 mdss", kMdssBase, kMdssSize);
	if (status != B_OK)
		return status;

	// the SBU mux on, the normal orientation (gpio-sbu-mux: enable active
	// low; select set for the reverse one)
	static const uint32 kPins[] = {kSbuEnablePin, kSbuSelectPin};
	for (uint32 pin : kPins) {
		volatile uint8* tlmm;
		area_id area;
		if (_Map(area, tlmm, "qcom dp1 sbu", kTlmmBase + pin * 0x1000,
				B_PAGE_SIZE) != B_OK)
			return B_NO_MEMORY;
		wr(tlmm, 4, 0);				// low
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
	INFO("DP-1: PHY at RBR, clocks and AUX on (no link training yet)\n");
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
		ERROR("DP-1: the firmware's path is not VIG2 -> CTL_2 -> INTF_6\n");
		return B_NOT_SUPPORTED;
	}
	if ((rd(fMdss, kCtl0 + CTL_FETCH_PIPE_ACTIVE) & ~(1u << 16)) != 0
		|| (rd(fMdss, kCtl0 + CTL_INTF_ACTIVE) & ~1u) != 0) {
		ERROR("DP-1: CTL_0 is in use\n");
		return B_BUSY;
	}

	for (uint32 offset = 0; offset < 0x200; offset += 4)
		wr(fMdss, kVig0 + offset, rd(fMdss, kVig2 + offset));
	for (uint32 offset = 0xa00; offset < 0xb00; offset += 4)
		wr(fMdss, kVig0 + offset, rd(fMdss, kVig2 + offset));
	for (uint32 offset = 0; offset < 0x100; offset += 4)
		wr(fMdss, kLm0 + offset, rd(fMdss, kLm2 + offset));

	// VIG2's stage in LM_2's layer (bits 8:6, extension bit 4) as VIG0's
	// (bits 2:0, extension bit 0), and the border out bit (dpu_hw_ctl.c)
	uint32 layer2 = rd(fMdss, kCtl2 + CTL_LAYER_LM2);
	uint32 layerExt2 = rd(fMdss, kCtl2 + CTL_LAYER_EXT_LM2);
	wr(fMdss, kCtl0 + CTL_TOP, rd(fMdss, kCtl2 + CTL_TOP));
	wr(fMdss, kCtl0 + CTL_LAYER_LM0, (layer2 & (1u << 24)) | ((layer2 >> 6) & 7));
	wr(fMdss, kCtl0 + CTL_LAYER_EXT_LM0, (layerExt2 >> 4) & 1);
	wr(fMdss, kCtl0 + CTL_INTF_ACTIVE, 1u << 0);
	wr(fMdss, kCtl0 + CTL_FETCH_PIPE_ACTIVE, 1u << 16);

	for (uint32 offset : kIntfTiming)
		wr(fMdss, kIntf0 + offset, rd(fMdss, kIntf6 + offset));
	wr(fMdss, kIntf0 + INTF_FRAME_LINE_COUNT_EN, 3);

	status_t status = SetScanout(address, bytesPerRow, width, height);
	if (status != B_OK)
		return status;
	wr(fMdss, kCtl0 + CTL_INTF_FLUSH, 1u << 0);
	wr(fMdss, kCtl0 + CTL_FLUSH, kFlushVig0 | kFlushLm0 | kFlushCtl
		| kFlushIntf);
	wr(fMdss, kInterruptClear, kUnderrunIntf0);
	wr(fMdss, kIntf0 + INTF_TIMING_ENGINE_EN, 1);
	fRunning = true;
	LogState("DP-1 started");
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
static const uint8 kSwing[4][4] = {
	{0x28, 0x2f, 0x36, 0x3f}, {0x31, 0x3e, 0x3f, 0xff},
	{0x36, 0x3f, 0xff, 0xff}, {0x3f, 0xff, 0xff, 0xff}};
static const uint8 kPreEmphasis[4][4] = {
	{0x20, 0x2d, 0x34, 0x3a}, {0x20, 0x2e, 0x35, 0xff},
	{0x20, 0x2e, 0xff, 0xff}, {0x24, 0xff, 0xff, 0xff}};


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


//!	The PHY's levels on both TX blocks, and the sink told (DPCD 0x103).
status_t
UsbcDisplayPort::_SetLevels(uint32 swing, uint32 preEmphasis, uint32 lanes)
{
	if (swing > 3 || preEmphasis > 3 || kSwing[swing][preEmphasis] == 0xff)
		return B_BAD_VALUE;
	static const uint32 kLanes[] = {kPhyTxA, kPhyTxB};
	for (uint32 lane : kLanes) {
		wr(fPhy + lane, TX_DRV_LVL, kSwing[swing][preEmphasis] | 0x20);
		wr(fPhy + lane, TX_EMP_POST1_LVL, kPreEmphasis[swing][preEmphasis]
			| 0x20);
	}
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
	uint32 lanes = caps[2] & 0x1f;
	if (lanes != 1 && lanes != 2 && lanes != 4)
		return B_BAD_DATA;
	bool enhanced = (caps[2] & 0x80) != 0, tps3 = (caps[2] & 0x40) != 0;
	// the read interval for the status (TRAINING_AUX_RD_INTERVAL)
	uint32 interval = caps[14] & 0x7f;
	bigtime_t crDelay = interval == 0 ? 100 : interval * 4000;
	bigtime_t eqDelay = interval == 0 ? 400 : interval * 4000;
	INFO("DP-1: sink DPCD %#x, max rate %#x, %" B_PRIu32 " lanes%s%s\n",
		caps[0], caps[1], lanes, enhanced ? ", enhanced framing" : "",
		tps3 ? ", TPS3" : "");

	_EnableMainlink(lanes);
	uint8 link[2] = {0x06, (uint8)(lanes | (enhanced ? 0x80 : 0))};	// RBR
	uint8 coding[2] = {0, 1};			// no down spread, 8b/10b
	if (fPort->WriteDpcd(0x100, link, 2) != B_OK
		|| fPort->WriteDpcd(0x107, coding, 2) != B_OK)
		return B_IO_ERROR;

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
		INFO("DP-1: training %s: %s, swing %" B_PRIu32 " pre-emphasis %"
			B_PRIu32 "\n", phase == 1 ? "clock recovery" : "equalization",
			strerror(status), swing, preEmphasis);
	}
	uint8 off = 0;
	fPort->WriteDpcd(0x102, &off, 1);
	wr(fController, DP_STATE_CTRL, 0);
	*_lanes = lanes;
	return status;
}


/*!	The link and the stream: trained with the sink if one answers; if not,
	the controller's training patterns checked, and the stream started on
	an untrained link (where it goes nowhere), so that path is known too. */
void
UsbcDisplayPort::_Link(const display_timing& timing, bool wideBus)
{
	if (fPort == NULL) {
		fPort = new(std::nothrow) DisplayPort(0);
		if (fPort == NULL || fPort->Init() != B_OK) {
			ERROR("DP-1: no AUX channel\n");
			delete fPort;
			fPort = NULL;
			return;
		}
	}
	uint32 lanes = 4;
	status_t status = _Train(&lanes);
	if (status != B_OK) {
		INFO("DP-1: link not trained (%s)\n", strerror(status));
		_EnableMainlink(4);
		lanes = 4;
		for (uint32 pattern = 1; pattern <= 3; pattern++) {
			INFO("DP-1: training pattern %" B_PRIu32 ": %s\n", pattern,
				_SendPattern(pattern) == B_OK ? "sent" : "not sent");
		}
		wr(fController, DP_STATE_CTRL, 0);
	}
	fPort->SetLinkRate(162);
	status = fPort->SetStream(timing, wideBus);
	if (status == B_OK)
		status = fPort->StartStream();
	INFO("DP-1: stream on %" B_PRIu32 " lanes at RBR: %s (mainlink ready %#"
		B_PRIx32 ")\n", lanes, strerror(status),
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
	wr(fMdss, kVig0 + SSPP_SRC_SIZE, sizes);
	wr(fMdss, kVig0 + SSPP_SRC_XY, 0);
	wr(fMdss, kVig0 + SSPP_OUT_SIZE, sizes);
	wr(fMdss, kVig0 + SSPP_OUT_XY, 0);
	wr(fMdss, kVig0 + SSPP_SRC0_ADDR, (uint32)address);
	wr(fMdss, kVig0 + SSPP_SRC_YSTRIDE0, bytesPerRow);
	if (fRunning)
		wr(fMdss, kCtl0 + CTL_FLUSH, kFlushVig0);
	return B_OK;
}


void
UsbcDisplayPort::Stop()
{
	if (!fRunning || fMdss == NULL)
		return;
	wr(fMdss, kIntf0 + INTF_TIMING_ENGINE_EN, 0);
	wr(fMdss, kCtl0 + CTL_LAYER_LM0, 0);
	wr(fMdss, kCtl0 + CTL_LAYER_EXT_LM0, 0);
	wr(fMdss, kCtl0 + CTL_INTF_ACTIVE, 0);
	wr(fMdss, kCtl0 + CTL_FETCH_PIPE_ACTIVE, 0);
	wr(fMdss, kCtl0 + CTL_FLUSH, kFlushVig0 | kFlushLm0 | kFlushCtl);
	fRunning = false;
}


void
UsbcDisplayPort::LogState(const char* when)
{
	if (fMdss == NULL)
		return;
	wr(fMdss, kInterruptClear, kUnderrunIntf0);
	uint32 frames = rd(fMdss, kIntf0 + INTF_FRAME_COUNT);
	snooze(100000);
	frames = rd(fMdss, kIntf0 + INTF_FRAME_COUNT) - frames;
	INFO("%s: INTF_0 %s, %" B_PRIu32 " frames in 100 ms, flush %#" B_PRIx32
		", underrun %s, VIG0 %#" B_PRIx32 " at %#" B_PRIx32 " stride %#"
		B_PRIx32 "\n", when,
		(rd(fMdss, kIntf0 + INTF_TIMING_ENGINE_EN) & 1) != 0 ? "on" : "off",
		frames, rd(fMdss, kCtl0 + CTL_FLUSH),
		(rd(fMdss, kInterruptStatus) & kUnderrunIntf0) != 0 ? "yes" : "no",
		rd(fMdss, kVig0 + SSPP_SRC_SIZE), rd(fMdss, kVig0 + SSPP_SRC0_ADDR),
		rd(fMdss, kVig0 + SSPP_SRC_YSTRIDE0));
}


}	// namespace qcom
