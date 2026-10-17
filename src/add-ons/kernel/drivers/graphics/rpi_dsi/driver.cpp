/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Raspberry Pi 4's DSI display connector as an auxiliary display.

	The firmware drives the HDMI outputs and keeps the compositor (HVS)
	for them; it does not know this panel. This driver takes the third
	compositor channel, the second pixel valve and the DSI1 block for itself:
	a display list with one plane (a frame buffer of the panel's size), the
	pixel valve's timings, the DSI PHY and its clocks (PLLD's DSI1 channel,
	the escape and pixel clocks of the clock manager), the way Linux's vc4
	driver programs them. The panel is a plain video-mode panel that needs
	no commands (the Waveshare 3.5" DSI LCD (E) by default; others through
	the driver's settings file).

	/dev/auxdisplay/rpi_dsi/0 speaks the auxdisplay protocol
	(headers/os/drivers/auxdisplay.h): two buffers, present one. */


#include <new>
#include <stdlib.h>
#include <string.h>

#include <bus/FDT.h>
#include <device_manager.h>
#include <driver_settings.h>
#include <Drivers.h>
#include <GraphicsDefs.h>
#include <KernelExport.h>

#include <lock.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include <rpi_dsi.h>
#include <rpi_firmware.h>


#define RPI_DSI_DRIVER_MODULE_NAME	"drivers/graphics/rpi_dsi/driver_v1"
#define RPI_DSI_DEVICE_MODULE_NAME	"drivers/graphics/rpi_dsi/device_v1"

#define ERROR(x...)	dprintf("rpi_dsi: " x)
#define INFO(x...)	dprintf("rpi_dsi: " x)

#define BIT(n)					(1u << (n))
#define FIELD(value, shift)		((uint32)(value) << (shift))
#define DIV_ROUND_UP(n, d)		(((n) + (d) - 1) / (d))

// The VideoCore sees the ARM's first gigabyte at this bus address.
#define VC_BUS_OFFSET			0xc0000000

// the firmware's power domains: the device tree binding's index plus one
// (Linux's raspberrypi-power driver does that)
#define RPI_FIRMWARE_GET_DOMAIN_STATE	0x00030030
#define RPI_FIRMWARE_SET_DOMAIN_STATE	0x00038030
#define RPI_POWER_DOMAIN_DSI1			(18 + 1)
#define XOSC_RATE				54000000	// the BCM2711's crystal

// ---- clock manager (brcm,bcm2711-cprman)
#define CM_PASSWORD				0x5a000000
#define CM_PLLD					0x10c
#define CM_PLLD_LOADDSI1		BIT(2)
#define CM_PLLD_HOLDDSI1		BIT(3)
#define CM_LOCK					0x114
#define CM_LOCK_FLOCKD			BIT(11)
#define CM_DSI1ECTL				0x158
#define CM_DSI1EDIV				0x15c
#define CM_DSI1PCTL				0x160
#define CM_DSI1PDIV				0x164
#define CM_SRC_MASK				0xf
#define CM_SRC_OSC				1
#define CM_SRC_PLLD_PER			6
#define CM_SRC_DSI1_BYTE		8
#define CM_ENABLE				BIT(4)
#define CM_GATE					BIT(6)
#define CM_BUSY					BIT(7)
#define CM_FRAC					BIT(9)
#define CM_DIV_FRAC_BITS		12
#define A2W_PLLD_ANA1			0x1054
#define A2W_PLLD_CTRL			0x1140
#define A2W_PLL_CTRL_PWRDN		BIT(16)
#define A2W_PLL_CTRL_NDIV_MASK	0x3ff
#define A2W_PLL_CTRL_PDIV_SHIFT	12
#define A2W_PLL_CTRL_PDIV_MASK	0x7
#define A2W_PLLD_FRAC			0x1240
#define A2W_PLL_FRAC_BITS		20
#define A2W_PLLD_DSI0			0x1340
#define A2W_PLLD_CORE			0x1440
#define A2W_PLLD_PER			0x1540
#define A2W_PLLD_DSI1			0x1640
#define A2W_PLL_CHANNEL_DISABLE	BIT(8)
#define A2W_PLL_DIV_MASK		0xff

// ---- DSI1 (brcm,bcm2711-dsi1)
#define DSI1_CTRL				0x00
#define DSI_CTRL_HS_CLKC_BYTE	0
#define DSI_CTRL_RX_LPDT_EOT_DISABLE	BIT(13)
#define DSI_CTRL_LPDT_EOT_DISABLE	BIT(12)
#define DSI_CTRL_HSDT_EOT_DISABLE	BIT(11)
#define DSI_CTRL_SOFT_RESET_CFG	BIT(10)
#define DSI_CTRL_CAL_BYTE		BIT(9)
#define DSI_CTRL_CLR_LDF		BIT(7)
#define DSI1_CTRL_CLR_RXF		BIT(6)
#define DSI1_CTRL_CLR_PDF		BIT(5)
#define DSI1_CTRL_CLR_CDF		BIT(4)
#define DSI1_CTRL_EN			BIT(0)
#define DSI1_CTRL_RESET_FIFOS	(DSI_CTRL_CLR_LDF | DSI1_CTRL_CLR_RXF \
									| DSI1_CTRL_CLR_PDF | DSI1_CTRL_CLR_CDF)
#define DSI1_TXPKT1C			0x04
#define DSI1_TXPKT1H			0x08
#define DSI1_DISP0_CTRL			0x28
#define DSI_DISP0_PIX_CLK_DIV_SHIFT	13
#define DSI_DISP0_LP_STOP_CTRL_SHIFT	11
#define DSI_DISP0_LP_STOP_PERFRAME	2
#define DSI_DISP0_ST_END		BIT(4)
#define DSI_DISP0_PFORMAT_SHIFT	2
#define DSI_PFORMAT_RGB565		0
#define DSI_PFORMAT_RGB666_PACKED	1
#define DSI_PFORMAT_RGB666		2
#define DSI_PFORMAT_RGB888		3
#define DSI_DISP0_ENABLE		BIT(0)
#define DSI1_DISP1_CTRL			0x2c
#define DSI1_DISP1_PFORMAT_32BIT_LE	2
#define DSI1_DISP1_PFORMAT_SHIFT	1
#define DSI_DISP1_ENABLE		BIT(0)
#define DSI1_INT_STAT			0x30
#define DSI1_INT_EN				0x34
#define DSI1_STAT				0x38
#define DSI1_HSTX_TO_CNT		0x3c
#define DSI1_LPRX_TO_CNT		0x40
#define DSI1_TA_TO_CNT			0x44
#define DSI1_PR_TO_CNT			0x48
#define DSI1_PHYC				0x4c
#define DSI1_PHYC_ESC_CLK_LPDT_SHIFT	20
#define DSI1_PHYC_HS_CLK_CONTINUOUS	BIT(18)
#define DSI1_PHYC_CLANE_ENABLE	BIT(16)
#define DSI_PHYC_DLANE3_ENABLE	BIT(12)
#define DSI_PHYC_DLANE2_ENABLE	BIT(8)
#define DSI_PHYC_DLANE1_ENABLE	BIT(4)
#define DSI_PHYC_DLANE0_ENABLE	BIT(0)
#define DSI1_HS_CLT0			0x50
#define DSI1_HS_CLT1			0x54
#define DSI1_HS_CLT2			0x58
#define DSI1_HS_DLT3			0x5c
#define DSI1_HS_DLT4			0x60
#define DSI1_HS_DLT5			0x64
#define DSI1_HS_DLT6			0x68
#define DSI1_HS_DLT7			0x6c
#define DSI1_PHY_AFEC0			0x70
#define DSI1_PHY_AFEC0_IDR_DLANE3_SHIFT	29
#define DSI1_PHY_AFEC0_IDR_DLANE2_SHIFT	26
#define DSI1_PHY_AFEC0_IDR_DLANE1_SHIFT	23
#define DSI1_PHY_AFEC0_IDR_DLANE0_SHIFT	20
#define DSI1_PHY_AFEC0_IDR_CLANE_SHIFT	17
#define DSI1_PHY_AFEC0_LATCH_ULPS	BIT(14)
#define DSI1_PHY_AFEC0_RESET	BIT(13)
#define DSI1_PHY_AFEC0_PD		BIT(12)
#define DSI1_PHY_AFEC0_PD_BG	BIT(11)
#define DSI1_PHY_AFEC0_PD_DLANE1	BIT(10)
#define DSI1_PHY_AFEC0_PD_DLANE2	BIT(9)
#define DSI1_PHY_AFEC0_PD_DLANE3	BIT(8)
#define DSI_PHY_AFEC0_PTATADJ_SHIFT	4
#define DSI_PHY_AFEC0_CTATADJ_SHIFT	0
#define DSI1_PHY_AFEC1			0x74
#define DSI1_ID					0x8c
#define DSI_ID_VALUE			0x00647369

// ---- pixel valve 1 (brcm,bcm2711-pixelvalve1)
#define PV_CONTROL				0x00
#define PV_CONTROL_FORMAT_SHIFT	21
#define PV_CONTROL_FORMAT_DSIV_24	4
#define PV_CONTROL_FIFO_LEVEL_SHIFT	15
#define PV_CONTROL_CLR_AT_START	BIT(14)
#define PV_CONTROL_TRIGGER_UNDERFLOW	BIT(13)
#define PV_CONTROL_WAIT_HSTART	BIT(12)
#define PV_CONTROL_CLK_SELECT_DSI	0
#define PV_CONTROL_CLK_SELECT_SHIFT	2
#define PV_CONTROL_FIFO_CLR		BIT(1)
#define PV_CONTROL_EN			BIT(0)
#define PV_V_CONTROL			0x04
#define PV_VCONTROL_DSI			BIT(3)
#define PV_VCONTROL_CONTINUOUS	BIT(1)
#define PV_VCONTROL_VIDEN		BIT(0)
#define PV_VSYNCD_EVEN			0x08
#define PV_HORZA				0x0c
#define PV_HORZB				0x10
#define PV_VERTA				0x14
#define PV_VERTB				0x18
#define PV_INTEN				0x24
#define PV_INTSTAT				0x28
#define PV_STAT					0x2c
#define PV_HACT_ACT				0x30
#define PV_MUX_CFG				0x34
#define PV_MUX_CFG_RGB_PIXEL_MUX_MODE_NO_SWAP	8
#define PV_MUX_CFG_RGB_PIXEL_MUX_MODE_SHIFT		2
#define PV1_FIFO_DEPTH			64
#define HVS_FIFO_LATENCY_PIX	6

// ---- HVS (brcm,bcm2711-hvs)
#define SCALER_DISPCTRL			0x00
#define SCALER_DISPCTRL_ENABLE	BIT(31)
#define SCALER_DISPCTRL_DSP3_MUX_SHIFT	18
#define SCALER_DISPCTRL_DSP3_MUX_MASK	(3u << 18)
#define SCALER_DISPSTAT			0x04
#define SCALER_DISPECTRL		0x0c
#define SCALER_DISPDITHER		0x14
#define SCALER_DISPEOLN			0x18
#define SCALER_DISPLIST0		0x20
#define SCALER_DISPLISTX(x)		(SCALER_DISPLIST0 + (x) * 4)
#define SCALER_DISPCTRL0		0x40
#define SCALER_DISPCTRLX(x)		(SCALER_DISPCTRL0 + (x) * 0x10)
#define SCALER_DISPCTRLX_ENABLE	BIT(31)
#define SCALER_DISPCTRLX_RESET	BIT(30)
#define SCALER5_DISPCTRLX_WIDTH_SHIFT	16
#define SCALER5_DISPCTRLX_HEIGHT_SHIFT	0
#define SCALER_DISPBKGND0		0x44
#define SCALER_DISPBKGNDX(x)	(SCALER_DISPBKGND0 + (x) * 0x10)
#define SCALER5_DISPBKGND_BCK2BCK	BIT(31)
#define SCALER_DISPBKGND_INTERLACE	BIT(30)
#define SCALER_DISPBKGND_GAMMA	BIT(29)
#define SCALER_DISPBKGND_FILL	BIT(24)
#define SCALER_DISPSTAT0		0x48
#define SCALER_DISPSTATX(x)		(SCALER_DISPSTAT0 + (x) * 0x10)
#define SCALER_DISPSTATX_MODE_SHIFT	30
#define SCALER_DISPBASE0		0x4c
#define SCALER_DISPBASEX(x)		(SCALER_DISPBASE0 + (x) * 0x10)
#define SCALER5_DLIST_START		0x4000
#define SCALER_DLIST_WORDS		4096
#define HVS_BOOTLOADER_DLIST_END	32

// display list words
#define SCALER_CTL0_END			BIT(31)
#define SCALER_CTL0_VALID		BIT(30)
#define SCALER_CTL0_SIZE_SHIFT	24
#define SCALER5_CTL0_UNITY		BIT(15)
#define SCALER_CTL0_ORDER_SHIFT	13
#define SCALER5_CTL0_ALPHA_EXPAND	BIT(12)
#define SCALER5_CTL0_RGB_EXPAND	BIT(11)
#define HVS_PIXEL_FORMAT_RGBA8888	7
#define HVS_PIXEL_ORDER_ARGB	2
#define SCALER5_POS0_START_Y_SHIFT	16
#define SCALER5_CTL2_ALPHA_MODE_FIXED	1
#define SCALER5_CTL2_ALPHA_MODE_SHIFT	30
#define SCALER5_CTL2_ALPHA_SHIFT	4
#define SCALER5_POS2_HEIGHT_SHIFT	16
#define SCALER_DLIST_CONTEXT	0xc0c0c0c0

#define HVS_CHANNEL				2	// the third FIFO; the firmware uses 0 and 1
#define HVS_OUTPUT_DSI1			3	// DSP3: pixel valve 1 (DSI1 / SMI)
#define DISPLAY_LIST_WORDS		9	// one plane and the end word
#define BUFFER_COUNT			2


struct timing {
	uint32	width, height;
	uint32	hfront, hsync, hback;
	uint32	vfront, vsync, vback;
	uint32	clock;		// kHz
	uint32	lanes;
};

// The Waveshare 3.5" DSI LCD (E): 640x480, one lane, no commands, no
// backlight control (the values of its Linux overlay).
static const timing kDefaultPanel = {
	640, 480, 48, 32, 80, 3, 4, 13, 24000, 1
};

struct registers {
	area_id			area;
	volatile uint8*	base;
	phys_addr_t		physical;
	size_t			size;
};

struct dsi_info {
	device_node*	node;
	mutex			lock;
	registers		dsi;
	registers		pixelValve;
	registers		hvs;
	registers		clocks;
	registers		i2c;		// BSC0, the connector's I2C (GPIO 44/45)
	registers		gpio;

	char			name[64];
	timing			panel;
	timing			mode;		// stretched for the integer PLL divider
	uint32			steps;		// highest enable step done
	uint32			pllDivider;
	uint32			hsClock;
	uint32			escapeClock;
	uint32			escapeNs;	// one escape clock period
	uint32			displayList;	// word offset

	area_id			bufferArea;
	uint8*			buffer;
	phys_addr_t		bufferAddress;
	size_t			bufferSize;	// one buffer
	uint32			bytesPerRow;
	uint32			shown;

	uint32			initialHVS[32];
	uint32			initialPV[16];
	uint32			initialDSI[36];
};


static device_manager_info* sDeviceManager;
static rpi_firmware_module_info* sFirmware;
static dsi_info* sInfo;


//	#pragma mark - power


static status_t
set_domain_power(uint32 domain, bool on)
{
	uint32 request[2] = { domain, on ? 1u : 0u };
	status_t status = sFirmware->property(RPI_FIRMWARE_SET_DOMAIN_STATE,
		request, sizeof(request));
	if (status != B_OK)
		return status;
	uint32 state[2] = { domain, 0 };
	if (sFirmware->property(RPI_FIRMWARE_GET_DOMAIN_STATE, state,
			sizeof(state)) == B_OK && (state[1] != 0) != on) {
		ERROR("power domain %" B_PRIu32 " stays %s\n", domain,
			state[1] != 0 ? "on" : "off");
		return B_ERROR;
	}
	INFO("power domain %" B_PRIu32 " %s\n", domain, on ? "on" : "off");
	return B_OK;
}


//	#pragma mark - registers


static inline uint32
read32(const registers& regs, uint32 offset)
{
	return *(volatile uint32*)(regs.base + offset);
}


static inline void
write32(const registers& regs, uint32 offset, uint32 value)
{
	*(volatile uint32*)(regs.base + offset) = value;
}


static inline void
barrier()
{
	__asm__ __volatile__("dsb sy" ::: "memory");
}


static inline uint32
cm_read(dsi_info* info, uint32 offset)
{
	return read32(info->clocks, offset);
}


static inline void
cm_write(dsi_info* info, uint32 offset, uint32 value)
{
	write32(info->clocks, offset, CM_PASSWORD | value);
}


/*!	Finds the device tree node whose \a attribute ("fdt/compatible" or
	"fdt/name") is \a value and maps its registers. */
static status_t
map_block_by(const char* attribute, const char* value, const char* name,
	registers& regs)
{
	device_node* root = sDeviceManager->get_root_node();
	if (root == NULL)
		return B_DEVICE_NOT_FOUND;

	const char* compatible = value;
	device_attr attributes[] = {
		{ attribute, B_STRING_TYPE, { .string = value } },
		{}
	};
	device_node* node = NULL;
	if (sDeviceManager->find_child_node(root, attributes, &node) != B_OK)
		node = NULL;
	sDeviceManager->put_node(root);
	if (node == NULL) {
		ERROR("no %s in the device tree\n", compatible);
		return B_DEVICE_NOT_FOUND;
	}

	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(node,
		(driver_module_info**)&fdt, (void**)&device);
	uint64 base = 0, size = 0;
	if (status == B_OK && !fdt->get_reg(device, 0, &base, &size))
		status = B_BAD_DATA;
	sDeviceManager->put_node(node);
	if (status != B_OK)
		return status;

	phys_addr_t page = base & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	void* address;
	regs.area = map_physical_memory(name, page,
		ROUNDUP(base + size - page, B_PAGE_SIZE), B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (regs.area < 0) {
		ERROR("cannot map %s: %s\n", name, strerror(regs.area));
		return regs.area;
	}
	regs.base = (volatile uint8*)address + (base - page);
	regs.physical = base;
	regs.size = size;
	return B_OK;
}


static status_t
map_block(const char* compatible, const char* name, registers& regs)
{
	return map_block_by("fdt/compatible", compatible, name, regs);
}


static void
unmap_block(registers& regs)
{
	if (regs.area >= 0)
		delete_area(regs.area);
	regs.area = -1;
	regs.base = NULL;
}


//	#pragma mark - clocks


/*!	PLLD's rate from its registers (the BCM2711 has no feedback prediv). */
static uint32
plld_rate(dsi_info* info)
{
	uint32 control = cm_read(info, A2W_PLLD_CTRL);
	uint32 ndiv = control & A2W_PLL_CTRL_NDIV_MASK;
	uint32 pdiv = (control >> A2W_PLL_CTRL_PDIV_SHIFT) & A2W_PLL_CTRL_PDIV_MASK;
	uint32 frac = cm_read(info, A2W_PLLD_FRAC) & ((1u << A2W_PLL_FRAC_BITS) - 1);
	if ((control & A2W_PLL_CTRL_PWRDN) != 0 || pdiv == 0)
		return 0;
	uint64 rate = ((uint64)XOSC_RATE * (((uint64)ndiv << A2W_PLL_FRAC_BITS)
		+ frac)) >> A2W_PLL_FRAC_BITS;
	return (uint32)(rate / pdiv);
}


static uint32
pll_channel_rate(dsi_info* info, uint32 offset, uint32 pll)
{
	uint32 value = cm_read(info, offset);
	if ((value & A2W_PLL_CHANNEL_DISABLE) != 0)
		return 0;
	uint32 divider = value & A2W_PLL_DIV_MASK;
	if (divider == 0)
		divider = 256;
	return pll / divider;
}


static status_t
wait_not_busy(dsi_info* info, uint32 control)
{
	for (int i = 0; i < 10000; i++) {
		if ((cm_read(info, control) & CM_BUSY) == 0)
			return B_OK;
		spin(10);
	}
	ERROR("clock at 0x%" B_PRIx32 " stays busy\n", control);
	return B_TIMED_OUT;
}


/*!	A clock manager clock: off, source and divider (12.12 with the given
	fractional bits) set, on. */
static status_t
set_cm_clock(dsi_info* info, uint32 control, uint32 divReg, uint32 source,
	uint32 divider, uint32 fracBits)
{
	uint32 value = cm_read(info, control);
	if ((value & CM_ENABLE) != 0) {
		cm_write(info, control, value & ~CM_ENABLE);
		status_t status = wait_not_busy(info, control);
		if (status != B_OK)
			return status;
	}
	divider &= ~((1u << (CM_DIV_FRAC_BITS - fracBits)) - 1);
	value = source & CM_SRC_MASK;
	if ((divider & ((1u << CM_DIV_FRAC_BITS) - 1)) != 0)
		value |= CM_FRAC;
	cm_write(info, control, value);
	cm_write(info, divReg, divider);
	barrier();
	cm_write(info, control, value | CM_ENABLE | CM_GATE);
	return B_OK;
}


static void
stop_cm_clock(dsi_info* info, uint32 control)
{
	cm_write(info, control, cm_read(info, control) & ~CM_ENABLE);
	wait_not_busy(info, control);
}


/*!	Step 1: the DSI bit clock from PLLD's DSI1 channel (integer divider),
	the mode stretched to keep the refresh rate, and the escape clock. */
static status_t
enable_clocks(dsi_info* info)
{
	uint32 plld = plld_rate(info);
	if (plld == 0) {
		ERROR("PLLD is off\n");
		return B_DEVICE_NOT_FOUND;
	}

	// bits per pixel over the lanes: how many PHY clocks one pixel takes
	uint32 divider = 24 / info->panel.lanes;
	uint64 wanted = (uint64)info->panel.clock * 1000 * divider;
	uint32 pllDivider = (uint32)(plld / wanted);
	if (pllDivider < 1)
		pllDivider = 1;
	if (pllDivider > 255)
		pllDivider = 255;
	uint32 hsClock = plld / pllDivider;
	uint32 pixelClock = hsClock / divider;

	// the faster pixel clock is made up for with a wider front porch
	timing mode = info->panel;
	uint32 htotal = mode.width + mode.hfront + mode.hsync + mode.hback;
	uint32 newTotal = (uint32)((uint64)htotal * pixelClock
		/ ((uint64)info->panel.clock * 1000));
	mode.hfront += newTotal - htotal;
	mode.clock = pixelClock / 1000;
	info->mode = mode;
	info->pllDivider = pllDivider;
	info->hsClock = hsClock;

	// the PLL channel
	cm_write(info, A2W_PLLD_DSI1, pllDivider);
	uint32 cm = cm_read(info, CM_PLLD);
	cm_write(info, CM_PLLD, cm | CM_PLLD_LOADDSI1);
	cm_write(info, CM_PLLD, cm & ~CM_PLLD_LOADDSI1);
	cm_write(info, A2W_PLLD_DSI1,
		cm_read(info, A2W_PLLD_DSI1) & ~A2W_PLL_CHANNEL_DISABLE);
	cm_write(info, CM_PLLD, cm_read(info, CM_PLLD) & ~CM_PLLD_HOLDDSI1);
	barrier();

	// the escape clock, 100 MHz from PLLD_PER where that fits its 4-bit
	// divider, else the crystal
	uint32 per = pll_channel_rate(info, A2W_PLLD_PER, plld);
	uint32 source = CM_SRC_PLLD_PER;
	uint64 div = per != 0 ? (((uint64)per << CM_DIV_FRAC_BITS) / 100000000) : 0;
	if (per == 0 || div < (1u << CM_DIV_FRAC_BITS) || div >= (16u << CM_DIV_FRAC_BITS)) {
		source = CM_SRC_OSC;
		per = XOSC_RATE;
		div = 1u << CM_DIV_FRAC_BITS;
	}
	status_t status = set_cm_clock(info, CM_DSI1ECTL, CM_DSI1EDIV, source,
		(uint32)div, 8);
	if (status != B_OK)
		return status;
	div &= ~0xfull;
	info->escapeClock = (uint32)(((uint64)per << CM_DIV_FRAC_BITS) / div);
	info->escapeNs = DIV_ROUND_UP(1000000000u, info->escapeClock);

	INFO("PLLD %" B_PRIu32 " MHz / %" B_PRIu32 " = DSI clock %" B_PRIu32
		" MHz, pixel clock %" B_PRIu32 " kHz (panel %" B_PRIu32 " kHz, "
		"%" B_PRIu32 "x%" B_PRIu32 ", front porch %" B_PRIu32 "), escape "
		"%" B_PRIu32 " MHz\n", plld / 1000000, pllDivider,
		hsClock / 1000000, mode.clock, info->panel.clock, mode.width,
		mode.height, mode.hfront, info->escapeClock / 1000000);
	return B_OK;
}


static void
disable_clocks(dsi_info* info)
{
	stop_cm_clock(info, CM_DSI1PCTL);
	stop_cm_clock(info, CM_DSI1ECTL);
	cm_write(info, CM_PLLD, cm_read(info, CM_PLLD) | CM_PLLD_HOLDDSI1);
	cm_write(info, A2W_PLLD_DSI1,
		cm_read(info, A2W_PLLD_DSI1) | A2W_PLL_CHANNEL_DISABLE);
}


//	#pragma mark - DSI


/*!	HS timings are in byte clocks: rounded up to a multiple of 8 unit
	intervals. */
static uint32
hs_timing(uint32 uiNs, uint32 ns, uint32 ui)
{
	return ROUNDUP(ui + DIV_ROUND_UP(ns, uiNs), 8);
}


static uint32
esc_timing(dsi_info* info, uint32 ns)
{
	return DIV_ROUND_UP(ns, info->escapeNs);
}


/*!	Step 2: the DSI block, its PHY and the pixel clock, as Linux's
	vc4_dsi_bridge_pre_enable for port 1. */
static status_t
enable_phy(dsi_info* info)
{
	const registers& dsi = info->dsi;
	uint32 lanes = info->panel.lanes;

	// the block's power domain, as Linux's runtime PM does first
	status_t status = set_domain_power(RPI_POWER_DOMAIN_DSI1, true);
	if (status != B_OK)
		return status;

	if (read32(dsi, DSI1_ID) != DSI_ID_VALUE) {
		ERROR("DSI1 does not answer (id 0x%" B_PRIx32 ")\n",
			read32(dsi, DSI1_ID));
		return B_DEVICE_NOT_FOUND;
	}

	// Reset the block and its FIFOs; it must be enabled for that.
	write32(dsi, DSI1_CTRL, DSI_CTRL_SOFT_RESET_CFG | DSI1_CTRL_RESET_FIFOS);
	write32(dsi, DSI1_CTRL, DSI1_CTRL_EN | DSI_CTRL_HSDT_EOT_DISABLE
		| DSI_CTRL_RX_LPDT_EOT_DISABLE);
	write32(dsi, DSI1_STAT, read32(dsi, DSI1_STAT));

	// the analog front end: bias, lanes we have, held in reset
	uint32 afec0 = FIELD(7, DSI_PHY_AFEC0_PTATADJ_SHIFT)
		| FIELD(7, DSI_PHY_AFEC0_CTATADJ_SHIFT)
		| FIELD(6, DSI1_PHY_AFEC0_IDR_CLANE_SHIFT)
		| FIELD(6, DSI1_PHY_AFEC0_IDR_DLANE0_SHIFT)
		| FIELD(6, DSI1_PHY_AFEC0_IDR_DLANE1_SHIFT)
		| FIELD(6, DSI1_PHY_AFEC0_IDR_DLANE2_SHIFT)
		| FIELD(6, DSI1_PHY_AFEC0_IDR_DLANE3_SHIFT);
	if (lanes < 4)
		afec0 |= DSI1_PHY_AFEC0_PD_DLANE3;
	if (lanes < 3)
		afec0 |= DSI1_PHY_AFEC0_PD_DLANE2;
	if (lanes < 2)
		afec0 |= DSI1_PHY_AFEC0_PD_DLANE1;
	afec0 |= DSI1_PHY_AFEC0_RESET;
	write32(dsi, DSI1_PHY_AFEC0, afec0);
	write32(dsi, DSI1_PHY_AFEC1, 0);
	barrier();
	snooze(1000);

	// The pixel clock to the pixel valve is the PHY's byte clock; the
	// block divides it down to pixels with PIX_CLK_DIV.
	status = set_cm_clock(info, CM_DSI1PCTL, CM_DSI1PDIV,
		CM_SRC_DSI1_BYTE, 1u << CM_DIV_FRAC_BITS, 0);
	if (status != B_OK)
		return status;

	// one unit interval in ns; the clock is DDR
	uint32 uiNs = DIV_ROUND_UP(500000000u, info->hsClock);
	uint32 lpx = esc_timing(info, 60);

	write32(dsi, DSI1_HS_CLT0, FIELD(hs_timing(uiNs, 262, 0), 18)
		| FIELD(hs_timing(uiNs, 0, 8), 9) | hs_timing(uiNs, 38, 0));
	write32(dsi, DSI1_HS_CLT1, FIELD(hs_timing(uiNs, 60, 0), 9)
		| hs_timing(uiNs, 60, 52));
	write32(dsi, DSI1_HS_CLT2, hs_timing(uiNs, 1000000, 0));
	write32(dsi, DSI1_HS_DLT3, FIELD(hs_timing(uiNs, 100, 0), 18)
		| FIELD(hs_timing(uiNs, 105, 6), 9) | hs_timing(uiNs, 40, 4));
	uint32 trail = hs_timing(uiNs, 0, 8);
	if (hs_timing(uiNs, 60, 4) > trail)
		trail = hs_timing(uiNs, 60, 4);
	write32(dsi, DSI1_HS_DLT4, FIELD(trail, 9)
		| hs_timing(uiNs, lpx * info->escapeNs, 0));
	// STOP after power-up for 5 ms, as the firmware does
	write32(dsi, DSI1_HS_DLT5, hs_timing(uiNs, 5 * 1000 * 1000, 0));
	write32(dsi, DSI1_HS_DLT6, FIELD(lpx * 5, 24) | FIELD(lpx, 16)
		| FIELD(lpx * 4, 8) | lpx);
	write32(dsi, DSI1_HS_DLT7, esc_timing(info, 1000000));

	write32(dsi, DSI1_PHYC, DSI_PHYC_DLANE0_ENABLE
		| (lanes >= 2 ? DSI_PHYC_DLANE1_ENABLE : 0)
		| (lanes >= 3 ? DSI_PHYC_DLANE2_ENABLE : 0)
		| (lanes >= 4 ? DSI_PHYC_DLANE3_ENABLE : 0)
		| DSI1_PHYC_CLANE_ENABLE | DSI1_PHYC_HS_CLK_CONTINUOUS
		| FIELD(lpx - 1, DSI1_PHYC_ESC_CLK_LPDT_SHIFT));

	write32(dsi, DSI1_CTRL, read32(dsi, DSI1_CTRL) | DSI_CTRL_CAL_BYTE);

	write32(dsi, DSI1_HSTX_TO_CNT, 0);
	write32(dsi, DSI1_LPRX_TO_CNT, 0xffffff);
	write32(dsi, DSI1_TA_TO_CNT, 100000);
	write32(dsi, DSI1_PR_TO_CNT, 100000);

	// command payloads through the pixel FIFO (unused by a plain panel)
	write32(dsi, DSI1_DISP1_CTRL,
		FIELD(DSI1_DISP1_PFORMAT_32BIT_LE, DSI1_DISP1_PFORMAT_SHIFT)
		| DSI_DISP1_ENABLE);

	// the front end out of reset
	write32(dsi, DSI1_PHY_AFEC0,
		read32(dsi, DSI1_PHY_AFEC0) & ~DSI1_PHY_AFEC0_RESET);
	barrier();

	// video mode, set up but not enabled
	write32(dsi, DSI1_DISP0_CTRL,
		FIELD(24 / lanes, DSI_DISP0_PIX_CLK_DIV_SHIFT)
		| FIELD(DSI_PFORMAT_RGB888, DSI_DISP0_PFORMAT_SHIFT)
		| FIELD(DSI_DISP0_LP_STOP_PERFRAME, DSI_DISP0_LP_STOP_CTRL_SHIFT)
		| DSI_DISP0_ST_END);
	barrier();
	return B_OK;
}


static void
disable_phy(dsi_info* info)
{
	const registers& dsi = info->dsi;
	write32(dsi, DSI1_DISP0_CTRL,
		read32(dsi, DSI1_DISP0_CTRL) & ~DSI_DISP0_ENABLE);
	write32(dsi, DSI1_CTRL, DSI_CTRL_SOFT_RESET_CFG | DSI1_CTRL_RESET_FIFOS);
	write32(dsi, DSI1_PHY_AFEC0, DSI1_PHY_AFEC0_RESET | DSI1_PHY_AFEC0_PD
		| DSI1_PHY_AFEC0_PD_BG | DSI1_PHY_AFEC0_PD_DLANE1
		| DSI1_PHY_AFEC0_PD_DLANE2 | DSI1_PHY_AFEC0_PD_DLANE3);
	barrier();
	set_domain_power(RPI_POWER_DOMAIN_DSI1, false);
}


//	#pragma mark - HVS


static inline uint32
dlist_read(dsi_info* info, uint32 word)
{
	return read32(info->hvs, SCALER5_DLIST_START + word * 4);
}


static inline void
dlist_write(dsi_info* info, uint32 word, uint32 value)
{
	write32(info->hvs, SCALER5_DLIST_START + word * 4, value);
}


/*!	How many words the list at \a start occupies, up to its end word. */
static uint32
dlist_length(dsi_info* info, uint32 start)
{
	for (uint32 i = 0; start + i < SCALER_DLIST_WORDS; i++) {
		if ((dlist_read(info, start + i) & SCALER_CTL0_END) != 0)
			return i + 1;
	}
	return SCALER_DLIST_WORDS - start;
}


/*!	Step 3: the display list and the compositor channel, its output muxed to
	the pixel valve. The firmware's lists live at the start of the list
	memory; ours is at the end. */
static status_t
enable_hvs(dsi_info* info)
{
	const registers& hvs = info->hvs;

	if ((read32(hvs, SCALER_DISPCTRL) & SCALER_DISPCTRL_ENABLE) == 0) {
		ERROR("the compositor is off\n");
		return B_DEVICE_NOT_FOUND;
	}
	if ((read32(hvs, SCALER_DISPCTRLX(HVS_CHANNEL)) & SCALER_DISPCTRLX_ENABLE)
			!= 0 && info->steps < RPI_DSI_STEP_HVS) {
		ERROR("compositor channel %d is in use\n", HVS_CHANNEL);
		return B_BUSY;
	}
	uint32 base = read32(hvs, SCALER_DISPBASEX(HVS_CHANNEL));
	if ((base >> 16) <= (base & 0xffff)) {
		ERROR("compositor channel %d has no output buffer (0x%" B_PRIx32
			")\n", HVS_CHANNEL, base);
		return B_DEVICE_NOT_FOUND;
	}

	// the firmware's lists must not reach ours
	for (uint32 channel = 0; channel < 2; channel++) {
		uint32 start = read32(hvs, SCALER_DISPLISTX(channel));
		if (start >= SCALER_DLIST_WORDS)
			continue;
		if (start + dlist_length(info, start) > info->displayList) {
			ERROR("the firmware's display list %" B_PRIu32 " at %" B_PRIu32
				" reaches ours at %" B_PRIu32 "\n", channel, start,
				info->displayList);
			return B_BUSY;
		}
	}

	// one plane, unscaled, our first buffer
	uint32 list = info->displayList;
	dlist_write(info, list + 0, SCALER_CTL0_VALID
		| FIELD(DISPLAY_LIST_WORDS - 1, SCALER_CTL0_SIZE_SHIFT)
		| FIELD(HVS_PIXEL_ORDER_ARGB, SCALER_CTL0_ORDER_SHIFT)
		| HVS_PIXEL_FORMAT_RGBA8888 | SCALER5_CTL0_UNITY
		| SCALER5_CTL0_ALPHA_EXPAND | SCALER5_CTL0_RGB_EXPAND);
	dlist_write(info, list + 1, 0);		// position 0,0
	dlist_write(info, list + 2,
		FIELD(SCALER5_CTL2_ALPHA_MODE_FIXED, SCALER5_CTL2_ALPHA_MODE_SHIFT)
		| FIELD(0xfff, SCALER5_CTL2_ALPHA_SHIFT));
	dlist_write(info, list + 3,
		FIELD(info->panel.height, SCALER5_POS2_HEIGHT_SHIFT)
		| info->panel.width);
	dlist_write(info, list + 4, SCALER_DLIST_CONTEXT);
	dlist_write(info, list + 5,
		(uint32)(info->bufferAddress + info->shown * info->bufferSize)
			| VC_BUS_OFFSET);
	dlist_write(info, list + 6, SCALER_DLIST_CONTEXT);
	dlist_write(info, list + 7, info->bytesPerRow);
	dlist_write(info, list + 8, SCALER_CTL0_END);
	barrier();

	write32(hvs, SCALER_DISPLISTX(HVS_CHANNEL), list);

	// the channel: reset, then on with the panel's size
	write32(hvs, SCALER_DISPCTRLX(HVS_CHANNEL), 0);
	write32(hvs, SCALER_DISPCTRLX(HVS_CHANNEL), SCALER_DISPCTRLX_RESET);
	write32(hvs, SCALER_DISPCTRLX(HVS_CHANNEL), 0);
	write32(hvs, SCALER_DISPCTRLX(HVS_CHANNEL), SCALER_DISPCTRLX_ENABLE
		| FIELD(info->panel.width, SCALER5_DISPCTRLX_WIDTH_SHIFT)
		| FIELD(info->panel.height, SCALER5_DISPCTRLX_HEIGHT_SHIFT));
	uint32 background = read32(hvs, SCALER_DISPBKGNDX(HVS_CHANNEL));
	background &= ~(SCALER5_DISPBKGND_BCK2BCK | SCALER_DISPBKGND_GAMMA
		| SCALER_DISPBKGND_INTERLACE);
	write32(hvs, SCALER_DISPBKGNDX(HVS_CHANNEL), background);

	// output 3 (pixel valve 1) takes this channel
	uint32 control = read32(hvs, SCALER_DISPCTRL);
	control = (control & ~SCALER_DISPCTRL_DSP3_MUX_MASK)
		| FIELD(HVS_CHANNEL, SCALER_DISPCTRL_DSP3_MUX_SHIFT);
	write32(hvs, SCALER_DISPCTRL, control);
	barrier();
	return B_OK;
}


static void
disable_hvs(dsi_info* info)
{
	const registers& hvs = info->hvs;
	uint32 control = read32(hvs, SCALER_DISPCTRL);
	control = (control & ~SCALER_DISPCTRL_DSP3_MUX_MASK)
		| FIELD(3, SCALER_DISPCTRL_DSP3_MUX_SHIFT);
	write32(hvs, SCALER_DISPCTRL, control);
	if ((read32(hvs, SCALER_DISPCTRLX(HVS_CHANNEL)) & SCALER_DISPCTRLX_ENABLE)
			!= 0) {
		write32(hvs, SCALER_DISPCTRLX(HVS_CHANNEL), SCALER_DISPCTRLX_RESET);
		write32(hvs, SCALER_DISPCTRLX(HVS_CHANNEL), 0);
	}
	barrier();
}


//	#pragma mark - pixel valve


/*!	Step 4: the pixel valve's timings, running. */
static status_t
enable_pixel_valve(dsi_info* info)
{
	const registers& pv = info->pixelValve;
	const timing& mode = info->mode;

	write32(pv, PV_CONTROL, read32(pv, PV_CONTROL) & ~PV_CONTROL_EN);
	write32(pv, PV_CONTROL, read32(pv, PV_CONTROL) | PV_CONTROL_FIFO_CLR);

	write32(pv, PV_HORZA, FIELD(mode.hback, 16) | mode.hsync);
	write32(pv, PV_HORZB, FIELD(mode.hfront, 16) | mode.width);
	write32(pv, PV_V_CONTROL, PV_VCONTROL_CONTINUOUS | PV_VCONTROL_DSI);
	write32(pv, PV_VSYNCD_EVEN, 0);
	write32(pv, PV_VERTA, FIELD(mode.vback, 16) | mode.vsync);
	write32(pv, PV_VERTB, FIELD(mode.vfront, 16) | mode.height);
	write32(pv, PV_HACT_ACT, mode.width);
	write32(pv, PV_MUX_CFG, FIELD(PV_MUX_CFG_RGB_PIXEL_MUX_MODE_NO_SWAP,
		PV_MUX_CFG_RGB_PIXEL_MUX_MODE_SHIFT));

	uint32 fifoLevel = PV1_FIFO_DEPTH - 3 * HVS_FIFO_LATENCY_PIX;
	write32(pv, PV_CONTROL, PV_CONTROL_FIFO_CLR
		| FIELD(fifoLevel, PV_CONTROL_FIFO_LEVEL_SHIFT)
		| FIELD(PV_CONTROL_FORMAT_DSIV_24, PV_CONTROL_FORMAT_SHIFT)
		| PV_CONTROL_CLR_AT_START | PV_CONTROL_TRIGGER_UNDERFLOW
		| PV_CONTROL_WAIT_HSTART
		| FIELD(PV_CONTROL_CLK_SELECT_DSI, PV_CONTROL_CLK_SELECT_SHIFT));
	barrier();
	write32(pv, PV_CONTROL, read32(pv, PV_CONTROL) | PV_CONTROL_EN);
	write32(pv, PV_V_CONTROL, read32(pv, PV_V_CONTROL) | PV_VCONTROL_VIDEN);
	barrier();
	return B_OK;
}


static void
disable_pixel_valve(dsi_info* info)
{
	const registers& pv = info->pixelValve;
	write32(pv, PV_V_CONTROL, read32(pv, PV_V_CONTROL) & ~PV_VCONTROL_VIDEN);
	for (int i = 0; i < 100; i++) {
		if ((read32(pv, PV_V_CONTROL) & PV_VCONTROL_VIDEN) == 0)
			break;
		spin(100);
	}
	write32(pv, PV_CONTROL, read32(pv, PV_CONTROL) & ~PV_CONTROL_EN);
	write32(pv, PV_CONTROL, read32(pv, PV_CONTROL) | PV_CONTROL_FIFO_CLR);
	barrier();
}


//	#pragma mark - the sequence


/*!	Step 5: pixels go out. */
static status_t
enable_video(dsi_info* info)
{
	const registers& dsi = info->dsi;
	write32(dsi, DSI1_DISP0_CTRL,
		read32(dsi, DSI1_DISP0_CTRL) | DSI_DISP0_ENABLE);
	barrier();
	return B_OK;
}


static status_t
run_step(dsi_info* info, uint32 step)
{
	if (step == RPI_DSI_STEP_OFF) {
		if (info->steps >= RPI_DSI_STEP_VIDEO) {
			write32(info->dsi, DSI1_DISP0_CTRL,
				read32(info->dsi, DSI1_DISP0_CTRL) & ~DSI_DISP0_ENABLE);
		}
		if (info->steps >= RPI_DSI_STEP_PIXEL_VALVE)
			disable_pixel_valve(info);
		if (info->steps >= RPI_DSI_STEP_HVS)
			disable_hvs(info);
		if (info->steps >= RPI_DSI_STEP_PHY)
			disable_phy(info);
		if (info->steps >= RPI_DSI_STEP_CLOCKS)
			disable_clocks(info);
		info->steps = 0;
		INFO("off\n");
		return B_OK;
	}

	if (step < 1 || step > RPI_DSI_STEP_VIDEO)
		return B_BAD_VALUE;
	if (step <= info->steps)
		return B_OK;
	if (step != info->steps + 1)
		return B_BAD_VALUE;

	status_t status;
	switch (step) {
		case RPI_DSI_STEP_CLOCKS:
			status = enable_clocks(info);
			break;
		case RPI_DSI_STEP_PHY:
			status = enable_phy(info);
			break;
		case RPI_DSI_STEP_HVS:
			status = enable_hvs(info);
			break;
		case RPI_DSI_STEP_PIXEL_VALVE:
			status = enable_pixel_valve(info);
			break;
		case RPI_DSI_STEP_VIDEO:
		default:
			status = enable_video(info);
			break;
	}
	if (status != B_OK) {
		ERROR("step %" B_PRIu32 " failed: %s\n", step, strerror(status));
		return status;
	}
	info->steps = step;
	if (step == RPI_DSI_STEP_VIDEO) {
		INFO("%s on: %" B_PRIu32 "x%" B_PRIu32 "\n", info->name,
			info->panel.width, info->panel.height);
	}
	return B_OK;
}


static status_t
set_power(dsi_info* info, bool on)
{
	if (!on)
		return run_step(info, RPI_DSI_STEP_OFF);
	for (uint32 step = info->steps + 1; step <= RPI_DSI_STEP_VIDEO; step++) {
		status_t status = run_step(info, step);
		if (status != B_OK) {
			run_step(info, RPI_DSI_STEP_OFF);
			return status;
		}
	}
	return B_OK;
}


static status_t
present(dsi_info* info, uint32 index)
{
	if (index >= BUFFER_COUNT)
		return B_BAD_VALUE;
	info->shown = index;
	if (info->steps >= RPI_DSI_STEP_HVS) {
		dlist_write(info, info->displayList + 5,
			(uint32)(info->bufferAddress + index * info->bufferSize)
				| VC_BUS_OFFSET);
		barrier();
	}
	return B_OK;
}


//	#pragma mark - touch controller probe


// the BCM2835 BSC (I2C) controller
#define BSC_C					0x00
#define BSC_C_I2CEN				BIT(15)
#define BSC_C_ST				BIT(7)
#define BSC_C_CLEAR				(BIT(4) | BIT(5))
#define BSC_C_READ				BIT(0)
#define BSC_S					0x04
#define BSC_S_CLKT				BIT(9)
#define BSC_S_ERR				BIT(8)
#define BSC_S_RXD				BIT(5)
#define BSC_S_TXD				BIT(4)
#define BSC_S_DONE				BIT(1)
#define BSC_DLEN				0x08
#define BSC_A					0x0c
#define BSC_FIFO				0x10
#define BSC_DIV					0x14
#define GOODIX_REG_ID			0x8140


static status_t
bsc_wait_done(const registers& bsc, uint32* _status)
{
	for (int i = 0; i < 2000; i++) {
		uint32 status = read32(bsc, BSC_S);
		if ((status & BSC_S_DONE) != 0) {
			*_status = status;
			write32(bsc, BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
			if ((status & (BSC_S_ERR | BSC_S_CLKT)) != 0)
				return B_IO_ERROR;
			return B_OK;
		}
		spin(10);
	}
	*_status = read32(bsc, BSC_S);
	write32(bsc, BSC_C, BSC_C_I2CEN | BSC_C_CLEAR);
	return B_TIMED_OUT;
}


/*!	Reads \a length bytes from a 16-bit register of an I2C device, as the
	Goodix touch controllers expect: the register address written, then a
	read. The connector's pins are switched to the BSC first. */
static status_t
touch_probe(dsi_info* info, rpi_dsi_touch_probe& probe)
{
	if (info->i2c.base == NULL || info->gpio.base == NULL)
		return B_DEVICE_NOT_FOUND;
	const registers& bsc = info->i2c;

	// GPIO 44/45 to ALT1 (SDA0/SCL0), pulled up
	uint32 fsel = read32(info->gpio, 0x10);		// FSEL4: pins 40 to 49
	fsel = (fsel & ~((7u << 12) | (7u << 15))) | (5u << 12) | (5u << 15);
	write32(info->gpio, 0x10, fsel);
	uint32 pull = read32(info->gpio, 0xec);		// PUP_PDN_CNTRL2: pins 32 to 47
	pull = (pull & ~((3u << 24) | (3u << 26))) | (1u << 24) | (1u << 26);
	write32(info->gpio, 0xec, pull);
	barrier();

	if (read32(bsc, BSC_DIV) == 0 || read32(bsc, BSC_DIV) > 0x8000)
		write32(bsc, BSC_DIV, 2500);		// slow and safe
	write32(bsc, BSC_C, BSC_C_I2CEN | BSC_C_CLEAR);
	write32(bsc, BSC_S, BSC_S_CLKT | BSC_S_ERR | BSC_S_DONE);
	write32(bsc, BSC_A, probe.address & 0x7f);
	write32(bsc, BSC_DLEN, 2);
	write32(bsc, BSC_FIFO, GOODIX_REG_ID >> 8);
	write32(bsc, BSC_FIFO, GOODIX_REG_ID & 0xff);
	write32(bsc, BSC_C, BSC_C_I2CEN | BSC_C_ST);
	status_t status = bsc_wait_done(bsc, &probe.controller_status);
	if (status != B_OK)
		return status;

	write32(bsc, BSC_DLEN, sizeof(probe.id));
	write32(bsc, BSC_C, BSC_C_I2CEN | BSC_C_ST | BSC_C_READ);
	status = bsc_wait_done(bsc, &probe.controller_status);
	for (size_t i = 0; i < sizeof(probe.id); i++) {
		if ((read32(bsc, BSC_S) & BSC_S_RXD) == 0)
			break;
		probe.id[i] = read32(bsc, BSC_FIFO) & 0xff;
	}
	return status;
}


//	#pragma mark - setup


static void
read_settings(dsi_info* info)
{
	info->panel = kDefaultPanel;
	strlcpy(info->name, "Waveshare 3.5\" DSI LCD (E)", sizeof(info->name));
	info->displayList = SCALER_DLIST_WORDS - 16;

	void* handle = load_driver_settings("rpi_dsi");
	if (handle == NULL)
		return;

	struct { const char* key; uint32* value; } keys[] = {
		{ "width", &info->panel.width }, { "height", &info->panel.height },
		{ "hfront", &info->panel.hfront }, { "hsync", &info->panel.hsync },
		{ "hback", &info->panel.hback }, { "vfront", &info->panel.vfront },
		{ "vsync", &info->panel.vsync }, { "vback", &info->panel.vback },
		{ "clock", &info->panel.clock }, { "lanes", &info->panel.lanes },
		{ "display_list", &info->displayList },
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		const char* value = get_driver_parameter(handle, keys[i].key, NULL,
			NULL);
		if (value != NULL && atoi(value) > 0)
			*keys[i].value = atoi(value);
	}
	const char* name = get_driver_parameter(handle, "name", NULL, NULL);
	if (name != NULL)
		strlcpy(info->name, name, sizeof(info->name));
	unload_driver_settings(handle);

	timing& panel = info->panel;
	if (panel.width < 16 || panel.width > 4096 || panel.height < 16
		|| panel.height > 4096 || panel.lanes < 1 || panel.lanes > 4
		|| panel.clock < 1000 || panel.clock > 500000
		|| info->displayList < HVS_BOOTLOADER_DLIST_END
		|| info->displayList + DISPLAY_LIST_WORDS > SCALER_DLIST_WORDS) {
		ERROR("settings out of range, using the default panel\n");
		info->panel = kDefaultPanel;
		info->displayList = SCALER_DLIST_WORDS - 16;
	}
}


/*!	The frame buffers: contiguous, where the VideoCore reaches them,
	write-combining, black. */
static status_t
create_buffers(dsi_info* info)
{
	info->bytesPerRow = info->panel.width * 4;
	info->bufferSize = ROUNDUP((size_t)info->bytesPerRow * info->panel.height,
		B_PAGE_SIZE);
	size_t size = info->bufferSize * BUFFER_COUNT;

	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = 1ull << 30;
	void* address;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "rpi dsi frame buffers",
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

	info->bufferArea = area;
	info->buffer = (uint8*)address;
	info->bufferAddress = entry.address;
	return B_OK;
}


static void
snapshot_registers(dsi_info* info)
{
	for (uint32 i = 0; i < 32; i++)
		info->initialHVS[i] = read32(info->hvs, i * 4);
	for (uint32 i = 0; i < 16; i++)
		info->initialPV[i] = read32(info->pixelValve, i * 4);
	for (uint32 i = 0; i < 36; i++)
		info->initialDSI[i] = read32(info->dsi, i * 4);
}


//	#pragma mark - device


static status_t
dsi_init_device(void* _info, void** _cookie)
{
	dsi_info* info = (dsi_info*)_info;
	*_cookie = info;
	return B_OK;
}


static void
dsi_uninit_device(void* cookie)
{
}


static status_t
dsi_open(void* _info, const char* path, int openMode, void** _cookie)
{
	*_cookie = _info;
	return B_OK;
}


static status_t
dsi_close(void* cookie)
{
	return B_OK;
}


static status_t
dsi_free(void* cookie)
{
	return B_OK;
}


static status_t
dsi_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	dsi_info* info = (dsi_info*)cookie;
	MutexLocker locker(info->lock);

	switch (op) {
		case AUX_DISPLAY_GET_INFO:
		{
			aux_display_info auxInfo = {};
			auxInfo.version = AUX_DISPLAY_API_VERSION;
			auxInfo.flags = AUX_DISPLAY_FLAG_BUFFERS | AUX_DISPLAY_FLAG_POWER
				| (info->steps >= RPI_DSI_STEP_VIDEO ? AUX_DISPLAY_FLAG_ON : 0);
			strlcpy(auxInfo.name, info->name, sizeof(auxInfo.name));
			auxInfo.width = info->panel.width;
			auxInfo.height = info->panel.height;
			auxInfo.bytes_per_row = info->bytesPerRow;
			auxInfo.color_space = B_RGB32;
			auxInfo.buffer_count = BUFFER_COUNT;
			auxInfo.buffer_size = info->bufferSize;
			uint64 total = (uint64)(info->mode.width + info->mode.hfront
				+ info->mode.hsync + info->mode.hback) * (info->mode.height
				+ info->mode.vfront + info->mode.vsync + info->mode.vback);
			if (total != 0 && info->mode.clock != 0) {
				auxInfo.refresh_rate = (uint32)((uint64)info->mode.clock
					* 1000 * 1000 / total);
			}
			return user_memcpy(buffer, &auxInfo, sizeof(auxInfo));
		}

		case AUX_DISPLAY_SET_POWER:
		{
			uint32 on;
			if (length < sizeof(on))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&on, buffer, sizeof(on));
			if (status != B_OK)
				return status;
			return set_power(info, on != 0);
		}

		case AUX_DISPLAY_SET_BACKLIGHT:
			return B_NOT_SUPPORTED;

		case AUX_DISPLAY_CLONE_BUFFER:
		{
			aux_display_buffer request;
			if (length < sizeof(request))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&request, buffer, sizeof(request));
			if (status != B_OK)
				return status;
			if (request.index >= BUFFER_COUNT)
				return B_BAD_VALUE;

			// The buffers are one area; the clone covers both, the caller
			// gets the address of the one asked for.
			void* address = NULL;
			area_id area = vm_clone_area(team_get_current_team_id(),
				"rpi dsi frame buffer", &address, B_ANY_ADDRESS,
				B_READ_AREA | B_WRITE_AREA, 0, info->bufferArea, true);
			if (area < 0)
				return area;
			request.area = area;
			request.address = (uint8*)address
				+ request.index * info->bufferSize;
			request.size = info->bufferSize;
			status = user_memcpy(buffer, &request, sizeof(request));
			if (status != B_OK)
				vm_delete_area(team_get_current_team_id(), area, true);
			return status;
		}

		case AUX_DISPLAY_PRESENT:
		{
			uint32 index;
			if (length < sizeof(index))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&index, buffer, sizeof(index));
			if (status != B_OK)
				return status;
			return present(info, index);
		}

		case AUX_DISPLAY_WAIT_TOUCH:
			return B_NOT_SUPPORTED;

		case RPI_DSI_GET_STATE:
		{
			rpi_dsi_state state = {};
			state.steps_done = info->steps;
			memcpy(&state.panel, &info->panel, sizeof(rpi_dsi_timing));
			memcpy(&state.mode, &info->mode, sizeof(rpi_dsi_timing));
			state.plld_rate = plld_rate(info);
			state.plld_per_rate = pll_channel_rate(info, A2W_PLLD_PER,
				state.plld_rate);
			state.hs_clock = info->hsClock;
			state.escape_clock = info->escapeClock;
			state.pll_divider = info->pllDivider;
			state.display_list = info->displayList;
			state.channel = HVS_CHANNEL;
			state.buffer_address = info->bufferAddress;
			state.buffer_size = info->bufferSize;
			state.buffer_count = BUFFER_COUNT;
			state.shown = info->shown;
			memcpy(state.initial_hvs, info->initialHVS, sizeof(state.initial_hvs));
			memcpy(state.initial_pv, info->initialPV, sizeof(state.initial_pv));
			memcpy(state.initial_dsi, info->initialDSI, sizeof(state.initial_dsi));
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case RPI_DSI_RUN_STEP:
		{
			uint32 step;
			if (length < sizeof(step))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&step, buffer, sizeof(step));
			if (status != B_OK)
				return status;
			return run_step(info, step);
		}

		case RPI_DSI_READ_REGISTER:
		case RPI_DSI_WRITE_REGISTER:
		{
			rpi_dsi_register request;
			if (length < sizeof(request))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&request, buffer, sizeof(request));
			if (status != B_OK)
				return status;
			const registers* block;
			switch (request.block) {
				case RPI_DSI_BLOCK_DSI: block = &info->dsi; break;
				case RPI_DSI_BLOCK_PIXEL_VALVE: block = &info->pixelValve; break;
				case RPI_DSI_BLOCK_HVS: block = &info->hvs; break;
				case RPI_DSI_BLOCK_CLOCKS: block = &info->clocks; break;
				case RPI_DSI_BLOCK_GPIO: block = &info->gpio; break;
				case RPI_DSI_BLOCK_I2C: block = &info->i2c; break;
				default: return B_BAD_VALUE;
			}
			if (block->base == NULL)
				return B_DEVICE_NOT_FOUND;
			// the whole mapped page: DSI1's id sits right behind its reg size
			if ((request.offset & 3) != 0
				|| request.offset + 4 > ROUNDUP(block->size, B_PAGE_SIZE)) {
				return B_BAD_VALUE;
			}
			if (op == RPI_DSI_WRITE_REGISTER) {
				if (block == &info->clocks)
					cm_write(info, request.offset, request.value);
				else
					write32(*block, request.offset, request.value);
				barrier();
			}
			request.value = read32(*block, request.offset);
			return user_memcpy(buffer, &request, sizeof(request));
		}

		case RPI_DSI_TOUCH_PROBE:
		{
			rpi_dsi_touch_probe probe;
			if (length < sizeof(probe))
				return B_BAD_VALUE;
			status_t status = user_memcpy(&probe, buffer, sizeof(probe));
			if (status != B_OK)
				return status;
			memset(probe.id, 0, sizeof(probe.id));
			probe.status = touch_probe(info, probe);
			return user_memcpy(buffer, &probe, sizeof(probe));
		}

		case RPI_DSI_READ_DISPLAY_LIST:
		{
			if (length < SCALER_DLIST_WORDS * 4)
				return B_BAD_VALUE;
			uint32* words = new(std::nothrow) uint32[SCALER_DLIST_WORDS];
			if (words == NULL)
				return B_NO_MEMORY;
			for (uint32 i = 0; i < SCALER_DLIST_WORDS; i++)
				words[i] = dlist_read(info, i);
			status_t status = user_memcpy(buffer, words,
				SCALER_DLIST_WORDS * 4);
			delete[] words;
			return status;
		}
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver


static float
dsi_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return -1.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK || strcmp(compatible, "brcm,bcm2711-dsi1") != 0) {
		return -1.0f;
	}

	return 1.0f;
}


static status_t
dsi_register_device(device_node* parent)
{
	device_attr attributes[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Raspberry Pi DSI display" } },
		{}
	};

	return sDeviceManager->register_node(parent, RPI_DSI_DRIVER_MODULE_NAME,
		attributes, NULL, NULL);
}


static status_t
dsi_init_driver(device_node* node, void** _cookie)
{
	dsi_info* info = (dsi_info*)calloc(1, sizeof(dsi_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->dsi.area = info->pixelValve.area = info->hvs.area
		= info->clocks.area = info->i2c.area = info->gpio.area = -1;
	info->bufferArea = -1;

	status_t status = map_block("brcm,bcm2711-dsi1", "rpi dsi1", info->dsi);
	if (status == B_OK) {
		status = map_block("brcm,bcm2711-pixelvalve1", "rpi pixel valve 1",
			info->pixelValve);
	}
	if (status == B_OK)
		status = map_block("brcm,bcm2711-hvs", "rpi hvs", info->hvs);
	if (status == B_OK)
		status = map_block("brcm,bcm2711-cprman", "rpi clocks", info->clocks);
	if (status == B_OK) {
		// optional: the connector's I2C for the touch controller
		info->i2c.area = info->gpio.area = -1;
		if (map_block_by("fdt/name", "i2c@7e205000", "rpi bsc0", info->i2c)
				!= B_OK
			|| map_block("brcm,bcm2711-gpio", "rpi gpio (dsi)", info->gpio)
				!= B_OK) {
			unmap_block(info->i2c);
			unmap_block(info->gpio);
			info->i2c.base = info->gpio.base = NULL;
		}
	}
	if (status == B_OK) {
		read_settings(info);
		status = create_buffers(info);
		if (status != B_OK)
			ERROR("no memory for the frame buffers: %s\n", strerror(status));
	}
	if (status != B_OK) {
		unmap_block(info->dsi);
		unmap_block(info->pixelValve);
		unmap_block(info->hvs);
		unmap_block(info->clocks);
		free(info);
		return status;
	}

	snapshot_registers(info);
	mutex_init(&info->lock, "rpi dsi");
	uint32 domain[2] = { RPI_POWER_DOMAIN_DSI1, 0 };
	sFirmware->property(RPI_FIRMWARE_GET_DOMAIN_STATE, domain, sizeof(domain));
	INFO("DSI1 power domain is %s\n", domain[1] != 0 ? "on" : "off");
	INFO("%s, %" B_PRIu32 "x%" B_PRIu32 " on %" B_PRIu32 " lane(s); DSI1 id "
		"0x%" B_PRIx32 ", HVS control 0x%" B_PRIx32 ", lists %" B_PRIu32
		"/%" B_PRIu32 ", channel %d base 0x%" B_PRIx32 "; PLLD %" B_PRIu32
		" MHz\n", info->name, info->panel.width, info->panel.height,
		info->panel.lanes, info->initialDSI[DSI1_ID / 4],
		info->initialHVS[SCALER_DISPCTRL / 4],
		info->initialHVS[SCALER_DISPLISTX(0) / 4],
		info->initialHVS[SCALER_DISPLISTX(1) / 4], HVS_CHANNEL,
		info->initialHVS[SCALER_DISPBASEX(HVS_CHANNEL) / 4],
		plld_rate(info) / 1000000);

	sInfo = info;
	*_cookie = info;
	return B_OK;
}


static void
dsi_uninit_driver(void* cookie)
{
	dsi_info* info = (dsi_info*)cookie;
	if (info->steps != 0)
		run_step(info, RPI_DSI_STEP_OFF);
	mutex_destroy(&info->lock);
	if (info->bufferArea >= 0)
		delete_area(info->bufferArea);
	unmap_block(info->i2c);
	unmap_block(info->gpio);
	unmap_block(info->dsi);
	unmap_block(info->pixelValve);
	unmap_block(info->hvs);
	unmap_block(info->clocks);
	free(info);
	sInfo = NULL;
}


static status_t
dsi_register_child_devices(void* cookie)
{
	dsi_info* info = (dsi_info*)cookie;
	return sDeviceManager->publish_device(info->node, RPI_DSI_DEVICE,
		RPI_DSI_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{RPI_FIRMWARE_MODULE_NAME, (module_info**)&sFirmware},
	{}
};

static device_module_info sDsiDevice = {
	{
		RPI_DSI_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	dsi_init_device,
	dsi_uninit_device,
	NULL,	// removed
	dsi_open,
	dsi_close,
	dsi_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	dsi_control,
	NULL,	// select
	NULL,	// deselect
};

static driver_module_info sDsiDriver = {
	{
		RPI_DSI_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	dsi_supports_device,
	dsi_register_device,
	dsi_init_driver,
	dsi_uninit_driver,
	dsi_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sDsiDriver,
	(module_info*)&sDsiDevice,
	NULL
};
