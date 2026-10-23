/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	xHCI controllers described by a flattened device tree, such as the
	DesignWare USB3 cores (DWC3) of the Rockchip RK3588 and the Allwinner
	A733. On the RK3588 the firmware (EDK2) powers the PHYs, enables the
	clocks and puts the cores in host mode; this attachment checks that
	state, completes the host mode setup where needed and runs the
	controller with uncached DMA memory. On the A733 U-Boot leaves the core
	unpowered, so its power domain, clocks, reset and USB 2 PHY are brought
	up here.
*/


#include <stdio.h>

#include <KernelExport.h>
#include <driver_settings.h>

#include "usb_fdt.h"
#include "xhci.h"


extern device_manager_info* gDeviceManager;
extern usb_for_controller_interface* gUSB;

#define XHCI_FDT_DRIVER "busses/usb/xhci/fdt/driver_v1"
#define XHCI_FDT_BUS "busses/usb/xhci/fdt/device_v1"

// DesignWare USB3 global registers, relative to the xHCI register base
#define DWC3_GCTL					0xc110
#define DWC3_GCTL_PRTCAPDIR_MASK	(3 << 12)
#define DWC3_GCTL_PRTCAPDIR_HOST	(1 << 12)
#define DWC3_GSNPSID				0xc120
#define DWC3_GLOBALS_END			0xc200

// Rockchip RK3588 clock gates (CRU_CLKGATE_CON42; a set bit stops the clock)
#define RK3588_CRU_BASE				0xfd7c0000
#define RK3588_CRU_CLKGATE_CON42	0x8a8
// USB GRF: a DWC3 core's USB 3 port behind the USBDP PHY (Linux
// phy-rockchip-usbdp.c, rk_udphy_u3_port_disable()); a write sets the bits
// whose mask is in the upper half
#define RK3588_USB_GRF_BASE			0xfd5ac000
#define RK3588_U3_PORT_DISABLED		0x0188

// Allwinner A733 (sun60i): the USB-C port's DWC3 core ("usbc2"), from the
// BSP's ccu-sun60iw2.c, pck600_domains.c, dwc3-sunxi-plat.c and
// phy-sunxi-plat.c, and the registers of a running Radxa Debian
// (cubie/evidence/usb3-debian-dump.txt).
#define A733_DWC3_BASE				0x06a00000
#define A733_CCU_BASE				0x02002000
#define A733_R_CCU_BASE				0x07010000
#define A733_PCK600_BASE			0x07060000
#define A733_USB2_PHY_BASE			0x06b00000
// R_CCU: the PCK-600's bus clock
#define A733_R_CCU_PPU				0x01ac
// PCK-600 power policy units: the USB2 domain (pd_usb2) holds the core,
// the PCIE domain the NSI bus master shared by USB 3 and PCIe ("usb_pcie")
#define A733_PPU_PCIE				7
#define A733_PPU_USB2				8
#define A733_PPU_POLICY				0x000
#define A733_PPU_STATUS				0x008
#define A733_PPU_ON					0x8
// CCU
#define A733_CCU_MBUS_GATES			0x05e0	// b28 serdes, with key
#define A733_CCU_MBUS_GATES_KEY		0x41055800
#define A733_CCU_AHB_GATES			0x05c0	// b8 serdes, b9 usb-sys-ahb-gate, with key
#define A733_CCU_AHB_GATES_KEY		0x10000ff
// The SerDes subsystem (the USB 3/DisplayPort combo PHY) holds the USB 3
// block too: its registers answer only once the subsystem's bus reset is
// released and its top register enables it, which the display driver does
// later in the boot (sunxi_display, DisplayPipe::Init()).
#define A733_CCU_SERDES_CLOCK		0x13c0	// b31 gate, b24 mux (pll-peri0-600m), M
#define A733_CCU_SERDES_RESET		0x13c4	// b16 reset (1: released)
#define A733_RTC_BASE				0x07090000
#define A733_RTC_DCXO_SERDES		0x016c	// b4 DCXO to the SerDes
#define A733_SERDES_TOP_BASE		0x06c00000
#define A733_SERDES_PCIE_GATING		0x004	// b16-b18 (on in a running Debian)
#define A733_SERDES_USB3_CONTROL	0x008	// SUBSYS_USB3P1_BGR
#define A733_SERDES_USB3_HCLK		(1u << 16)
#define A733_SERDES_USB3_ACLK		(1u << 17)
#define A733_SERDES_USB2_PHY_RSTN	(1u << 4)
// The core's PIPE interface clocked from the CCU's usb2-u2-pipe (120 MHz)
// instead of the combo PHY, which carries DisplayPort (the BSP's
// USB3_U2_ONLY mode); the UTMI interface runs from the USB 2 PHY, whose
// PLL locks after a reset pulse once it is powered.
#define A733_SERDES_USB3_PIPE_INTERNAL	(1u << 20)
#define A733_SERDES_USB3_UTMI_INTERNAL	(1u << 21)
#define A733_PHY_STATUS				0x24
#define A733_PHY_STATUS_LOCKED		(1u << 3)
#define A733_SERDES_DEBUG_CONTROL	0x0f0	// b29 set in a running Debian
#define A733_CCU_USB2_U3_UTMI		0x1360	// b31 gate, [26:24] mux (1: pll-peri0-300m), [4:0] M
#define A733_CCU_USB2_U2_PIPE		0x1364	// b31 gate, [26:24] mux (1: pll-peri0-480m), [4:0] M
#define DWC3_DCTL					0xc704
#define DWC3_DCTL_CSFTRST			(1u << 30)
#define A733_CCU_MSI_LITE2			0x05a4	// b0 gate, b16/b17 resets
#define A733_CCU_USB_REF			0x1340	// b31 gate, [26:24] mux (0: 24 MHz)
#define A733_CCU_USB2_U2_REF		0x1348	// b31 gate, [26:24] mux
#define A733_CCU_USB2_SUSPEND		0x1350	// b31 gate, b24 mux (1: 24 MHz), [4:0] M
#define A733_CCU_USB2_MF			0x1354	// b31 gate, [26:24] mux (0: 24 MHz), [4:0] M
#define A733_CCU_USB2_RESET			0x135c	// b16 reset (1: released)
#define A733_CCU_RES_DCAP			0x1a00	// b3 res-dcap-24m (USB 2 PHY)
// the PCIe side of the shared USB 3/PCIe block (on in a running Debian)
#define A733_CCU_ITS_PCIE			0x0574	// b1 its-pcie0-aclk, b16 reset
#define A733_CCU_PCIE_AUX			0x1380	// b31 gate (24 MHz)
#define A733_CCU_PCIE_AXI_SLV		0x1384	// b31 gate, [26:24] mux (2: pll-peri0-400m)
#define A733_CCU_PCIE_RESET			0x138c	// b16 power-up reset, b17 bus reset
// USB 2 PHY ("u2_base")
#define A733_PHY_CTRL				0x10
#define A733_PHY_CTRL_OTGDISABLE	(1u << 10)
#define A733_PHY_CTRL_VBUSVLDEXT	(1u << 5)
#define A733_PHY_CTRL_SIDDQ			(1u << 3)
#define A733_PHY_TUNE				0x18
#define A733_PHY_TUNE_PARAM			0x143338d6	// aw,phy_tune_param
// DWC3 PHY configuration (relative to the xHCI register base)
#define DWC3_GUSB2PHYCFG0			0xc200
#define DWC3_GUSB2PHYCFG_SUSPHY		(1u << 6)
#define DWC3_GUSB2PHYCFG_ENBLSLPM	(1u << 8)
#define DWC3_GUSB3PIPECTL0			0xc2c0
#define DWC3_GUSB3PIPECTL_SUSPHY	(1u << 17)
#define DWC3_GLOBALS_MAPPED			0xc300

struct xhci_fdt_info {
	device_node* node;
	xhci_platform_info platform;
};


static const char* const kCompatible[] = {
	"snps,dwc3", "generic-xhci", "xhci-platform", NULL
};


/*!	Reading an unclocked RK3588 block stalls the bus with an SError, so the
	DWC3 registers are touched only when the firmware left the controller's
	bus clocks running.
*/
static bool
rk3588_dwc3_clocks_running(phys_addr_t base, const char* name)
{
	static const struct {
		phys_addr_t base;
		uint32 gates;
	} kControllers[] = {
		// aclk_usb_root, aclk_usb, aclk_usb3otg0
		{ 0xfc000000, (1 << 0) | (1 << 2) | (1 << 4) },
		// aclk_usb_root, aclk_usb, aclk_usb3otg1
		{ 0xfc400000, (1 << 0) | (1 << 2) | (1 << 7) },
	};

	uint32 gates = 0;
	for (size_t i = 0; i < B_COUNT_OF(kControllers); i++) {
		if (kControllers[i].base == base)
			gates = kControllers[i].gates;
	}
	if (gates == 0) {
		dprintf("xhci: %s: unknown RK3588 DWC3 instance %#" B_PRIxPHYSADDR "\n",
			name, base);
		return false;
	}

	void* address;
	area_id area = map_physical_memory("RK3588 CRU (xhci)", RK3588_CRU_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address);
	if (area < 0)
		return false;
	uint32 value = *(volatile uint32*)((uint8*)address + RK3588_CRU_CLKGATE_CON42);
	delete_area(area);
	if ((value & gates) != 0) {
		dprintf("xhci: %s: bus clock gated by the firmware (CLKGATE_CON42 %#"
			B_PRIx32 ")\n", name, value);
		return false;
	}
	return true;
}


/*!	Disconnects the USB 3 port of an RK3588 DWC3 core from its USBDP PHY, the
	way Linux does when the PHY carries only DisplayPort. The controller then
	sees no SuperSpeed port at all, and hubs and devices run at high speed.
*/
static void
rk3588_dwc3_disable_u3_port(phys_addr_t base, const char* name)
{
	uint32 offset;
	if (base == 0xfc000000)
		offset = 0x1c;
	else if (base == 0xfc400000)
		offset = 0x34;
	else
		return;

	void* address;
	area_id area = map_physical_memory("RK3588 USB GRF (xhci)",
		RK3588_USB_GRF_BASE, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < 0)
		return;
	volatile uint32* control = (volatile uint32*)((uint8*)address + offset);
	uint32 previous = *control & 0xffff;
	*control = 0xffff0000 | RK3588_U3_PORT_DISABLED;
	dprintf("xhci: %s: USB 3 port disabled in the USB GRF (0x%04" B_PRIx32
		" -> 0x%04" B_PRIx32 ")\n", name, previous, *control & 0xffff);
	delete_area(area);
}


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


/*!	Switches a PCK-600 power domain on (Allwinner's sunxi_pd_init delays,
	then the policy; a policy already saying ON while the domain is off is
	taken back first so that the PPU sees a request, as the GPU driver does
	it) and waits for it.
*/
static status_t
sun60i_power_domain_on(volatile uint8* pck, uint32 domain, const char* name)
{
	volatile uint8* ppu = pck + domain * 0x1000;
	write32(ppu, 0x170, 0x001f1f1f);
	write32(ppu, 0x174, 0x00001f1f);
	write32(ppu, 0xc00, 0x08080808);
	write32(ppu, 0xc04, 0x00000808);
	write32(ppu, 0xc10, 0x00000008);
	if ((read32(ppu, A733_PPU_POLICY) & 0xf) == A733_PPU_ON) {
		write32(ppu, A733_PPU_POLICY, read32(ppu, A733_PPU_POLICY) & ~0xfu);
		spin(10);
	}
	write32(ppu, A733_PPU_POLICY,
		(read32(ppu, A733_PPU_POLICY) & ~0xfu) | A733_PPU_ON);
	bigtime_t deadline = system_time() + 10000;
	while ((read32(ppu, A733_PPU_STATUS) & 0xf) != A733_PPU_ON) {
		if (system_time() > deadline) {
			dprintf("xhci: %s: power domain %" B_PRIu32 " does not come up "
				"(status %#" B_PRIx32 ")\n", name, domain,
				read32(ppu, A733_PPU_STATUS));
			return B_TIMED_OUT;
		}
		spin(10);
	}
	return B_OK;
}


/*!	Powers the A733's USB-C DWC3 core: PCK-600 power domains, bus gates,
	the 24 MHz reference, suspend and "mf" clocks, the SerDes subsystem
	that houses the block, the controller reset, and the USB 2 PHY out of
	power-down with the BSP's tuning and through a reset pulse until its PLL
	locks. The combo PHY stays with DisplayPort: the core's PIPE interface
	runs from the CCU's substitute clock instead, so the controller has
	the USB 2 port only. Nothing of the core is read before its domain,
	clocks and SerDes enable are up: an unpowered A733 block reads as zeros
	or hangs the bus.
*/
static status_t
sun60i_dwc3_power_up(phys_addr_t base, const char* name)
{
	if (base != A733_DWC3_BASE)
		return B_NOT_SUPPORTED;
	dprintf("xhci: %s: powering up the A733 DWC3 core\n", name);

	void* address;
	area_id rccuArea = map_physical_memory("a733 r_ccu (xhci)",
		A733_R_CCU_BASE, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (rccuArea < 0)
		return rccuArea;
	volatile uint8* rccu = (volatile uint8*)address;
	area_id pckArea = map_physical_memory("a733 pck-600 (xhci)",
		A733_PCK600_BASE, 0xb000, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (pckArea < 0) {
		delete_area(rccuArea);
		return pckArea;
	}
	volatile uint8* pck = (volatile uint8*)address;
	area_id rtcArea = map_physical_memory("a733 rtc (xhci)", A733_RTC_BASE,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (rtcArea < 0) {
		delete_area(pckArea);
		delete_area(rccuArea);
		return rtcArea;
	}
	volatile uint8* rtc = (volatile uint8*)address;
	area_id ccuArea = map_physical_memory("a733 ccu (xhci)", A733_CCU_BASE,
		0x2000, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		&address);
	if (ccuArea < 0) {
		delete_area(rtcArea);
		delete_area(pckArea);
		delete_area(rccuArea);
		return ccuArea;
	}
	volatile uint8* ccu = (volatile uint8*)address;

	// the power domains (the PCK-600's bus clock first)
	write32(rccu, A733_R_CCU_PPU, read32(rccu, A733_R_CCU_PPU) | 1);
	uint32 pcieBefore = read32(pck + A733_PPU_PCIE * 0x1000, A733_PPU_STATUS);
	uint32 usb2Before = read32(pck + A733_PPU_USB2 * 0x1000, A733_PPU_STATUS);
	status_t status = sun60i_power_domain_on(pck, A733_PPU_PCIE, name);
	if (status == B_OK)
		status = sun60i_power_domain_on(pck, A733_PPU_USB2, name);

	if (status == B_OK) {
		// bus gates: the USB system AHB master (keyed register) and
		// MSI-lite 2 with its resets
		if ((read32(ccu, A733_CCU_AHB_GATES) & (1u << 9)) == 0) {
			write32(ccu, A733_CCU_AHB_GATES, read32(ccu, A733_CCU_AHB_GATES)
				| A733_CCU_AHB_GATES_KEY | (1u << 9));
		}
		write32(ccu, A733_CCU_MSI_LITE2, read32(ccu, A733_CCU_MSI_LITE2)
			| (1u << 0) | (1u << 16) | (1u << 17));
		// clocks, all from the 24 MHz oscillator as the BSP sets them
		write32(ccu, A733_CCU_USB_REF, 1u << 31);
		write32(ccu, A733_CCU_USB2_U2_REF, 1u << 31);
		write32(ccu, A733_CCU_USB2_SUSPEND, (1u << 31) | (1u << 24));
		write32(ccu, A733_CCU_USB2_MF, 1u << 31);
		// the SerDes subsystem: DCXO reference, 100 MHz configuration
		// clock, bus gates, reset and its top enable (as the display driver
		// does it; its later writes only set the same bits)
		write32(rtc, A733_RTC_DCXO_SERDES, read32(rtc, A733_RTC_DCXO_SERDES)
			| (1u << 4) | (1u << 5));
		if ((read32(ccu, A733_CCU_SERDES_CLOCK) & (1u << 31)) == 0)
			write32(ccu, A733_CCU_SERDES_CLOCK, 0x81000005);
		write32(ccu, A733_CCU_SERDES_RESET, read32(ccu, A733_CCU_SERDES_RESET)
			| (1u << 16));
		if ((read32(ccu, A733_CCU_AHB_GATES) & (1u << 8)) == 0) {
			write32(ccu, A733_CCU_AHB_GATES, read32(ccu, A733_CCU_AHB_GATES)
				| A733_CCU_AHB_GATES_KEY | (1u << 8));
		}
		if ((read32(ccu, A733_CCU_MBUS_GATES) & (1u << 28)) == 0) {
			write32(ccu, A733_CCU_MBUS_GATES, read32(ccu, A733_CCU_MBUS_GATES)
				| A733_CCU_MBUS_GATES_KEY | (1u << 28));
		}
		area_id topArea = map_physical_memory("a733 serdes top (xhci)",
			A733_SERDES_TOP_BASE, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
		if (topArea >= 0) {
			volatile uint8* top = (volatile uint8*)address;
			write32(top, A733_SERDES_PCIE_GATING,
				read32(top, A733_SERDES_PCIE_GATING) | 0x70000);
			write32(top, A733_SERDES_DEBUG_CONTROL,
				read32(top, A733_SERDES_DEBUG_CONTROL) | (1u << 29));
			write32(top, A733_SERDES_USB3_CONTROL,
				read32(top, A733_SERDES_USB3_CONTROL) | A733_SERDES_USB3_HCLK
					| A733_SERDES_USB3_ACLK | A733_SERDES_USB2_PHY_RSTN
					| A733_SERDES_USB3_PIPE_INTERNAL);
			delete_area(topArea);
		}
		// the substitute PIPE clock: 480 MHz / 4
		write32(ccu, A733_CCU_USB2_U2_PIPE, (1u << 31) | (1u << 24) | 3);
		spin(100);
		// the controller through a reset, now that it is clocked
		write32(ccu, A733_CCU_USB2_RESET, read32(ccu, A733_CCU_USB2_RESET)
			& ~(1u << 16));
		spin(100);
		write32(ccu, A733_CCU_USB2_RESET, read32(ccu, A733_CCU_USB2_RESET)
			| (1u << 16));
		// the PHY's clock
		write32(ccu, A733_CCU_RES_DCAP, read32(ccu, A733_CCU_RES_DCAP)
			| (1u << 3));
		// the PCIe side's AXI slave clock, aux clock, ITS clock and resets,
		// as the running Debian has them
		write32(ccu, A733_CCU_PCIE_AXI_SLV, (1u << 31) | (2u << 24));
		write32(ccu, A733_CCU_PCIE_AUX, 1u << 31);
		write32(ccu, A733_CCU_ITS_PCIE, read32(ccu, A733_CCU_ITS_PCIE)
			| (1u << 1) | (1u << 16));
		write32(ccu, A733_CCU_PCIE_RESET, read32(ccu, A733_CCU_PCIE_RESET)
			| (1u << 16) | (1u << 17));
		spin(100);
	}
	uint32 ahbGates = read32(ccu, A733_CCU_AHB_GATES);
	uint32 mbusGates = read32(ccu, A733_CCU_MBUS_GATES);
	uint32 msiLite = read32(ccu, A733_CCU_MSI_LITE2);
	uint32 usbReset = read32(ccu, A733_CCU_USB2_RESET);
	uint32 usbRef = read32(ccu, A733_CCU_USB_REF);
	uint32 usbSuspend = read32(ccu, A733_CCU_USB2_SUSPEND);
	uint32 usbMf = read32(ccu, A733_CCU_USB2_MF);
	uint32 pcieStatus = read32(pck + A733_PPU_PCIE * 0x1000, A733_PPU_STATUS);
	uint32 usb2Status = read32(pck + A733_PPU_USB2 * 0x1000, A733_PPU_STATUS);
	delete_area(ccuArea);
	delete_area(rtcArea);
	delete_area(pckArea);
	delete_area(rccuArea);
	if (status != B_OK)
		return status;

	// the USB 2 PHY: out of SIDDQ (power-down), external VBUS valid and
	// OTG disabled as phy-sunxi-plat.c (v1) does, then the tuning
	area_id phyArea = map_physical_memory("a733 usb2 phy (xhci)",
		A733_USB2_PHY_BASE, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (phyArea < 0)
		return phyArea;
	volatile uint8* phy = (volatile uint8*)address;
	uint32 phyControl = read32(phy, A733_PHY_CTRL);
	phyControl |= A733_PHY_CTRL_OTGDISABLE | A733_PHY_CTRL_VBUSVLDEXT;
	phyControl &= ~A733_PHY_CTRL_SIDDQ;
	write32(phy, A733_PHY_CTRL, phyControl);
	write32(phy, A733_PHY_TUNE, A733_PHY_TUNE_PARAM);
	phyControl = read32(phy, A733_PHY_CTRL);
	// a reset pulse now that it is powered, then its PLL locks
	bigtime_t phyWaited = system_time();
	uint32 phyStatus = 0;
	area_id topArea = map_physical_memory("a733 serdes top (xhci)",
		A733_SERDES_TOP_BASE, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (topArea >= 0) {
		volatile uint8* top = (volatile uint8*)address;
		write32(top, A733_SERDES_USB3_CONTROL,
			read32(top, A733_SERDES_USB3_CONTROL)
				& ~A733_SERDES_USB2_PHY_RSTN);
		spin(100);
		write32(top, A733_SERDES_USB3_CONTROL,
			read32(top, A733_SERDES_USB3_CONTROL)
				| A733_SERDES_USB2_PHY_RSTN);
		delete_area(topArea);
		bigtime_t phyDeadline = system_time() + 1000000;
		while (((phyStatus = read32(phy, A733_PHY_STATUS))
				& A733_PHY_STATUS_LOCKED) == 0
			&& system_time() < phyDeadline) {
			snooze(1000);
		}
	}
	phyWaited = system_time() - phyWaited;
	delete_area(phyArea);
	if ((phyStatus & A733_PHY_STATUS_LOCKED) == 0) {
		dprintf("xhci: %s: the USB 2 PHY does not lock (status %#" B_PRIx32
			")\n", name, phyStatus);
		return B_TIMED_OUT;
	}

	// the core's PHY interfaces: no PHY suspend, no sleep (the BSP's
	// dis_u2_susphy, dis_u3_susphy and dis_enblslpm quirks)
	area_id coreArea = map_physical_memory("DWC3 PHY config (xhci)", base,
		DWC3_GLOBALS_MAPPED, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (coreArea < 0)
		return coreArea;
	volatile uint8* core = (volatile uint8*)address;
	// give the core a moment to answer after the reset
	bigtime_t waited = system_time();
	bigtime_t deadline = waited + 100000;
	while ((read32(core, DWC3_GSNPSID) >> 16) == 0 && system_time() < deadline)
		spin(100);
	waited = system_time() - waited;
	// a core soft reset (Linux does it for a dual-role core before host
	// mode); it completes only with both interface clocks running, like
	// the xHCI reset after it
	write32(core, DWC3_DCTL, read32(core, DWC3_DCTL) | DWC3_DCTL_CSFTRST);
	deadline = system_time() + 100000;
	while ((read32(core, DWC3_DCTL) & DWC3_DCTL_CSFTRST) != 0
		&& system_time() < deadline) {
		spin(100);
	}
	uint32 deviceControl = read32(core, DWC3_DCTL);
	uint32 usb2Config = read32(core, DWC3_GUSB2PHYCFG0);
	write32(core, DWC3_GUSB2PHYCFG0, usb2Config
		& ~(DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM));
	uint32 usb3Config = read32(core, DWC3_GUSB3PIPECTL0);
	write32(core, DWC3_GUSB3PIPECTL0, usb3Config & ~DWC3_GUSB3PIPECTL_SUSPHY);
	dprintf("xhci: %s: A733 DWC3 up (GSNPSID %#" B_PRIx32 " after %" B_PRId64
		" us, PHY locked after %" B_PRId64 " us, DCTL %#" B_PRIx32 "): PPU PCIE "
		"%#" B_PRIx32 " (was %#"
		B_PRIx32 "), USB2 %#" B_PRIx32 " (was %#" B_PRIx32 "), AHB gates %#"
		B_PRIx32 ", MBUS gates %#" B_PRIx32 ", MSI-lite %#" B_PRIx32
		", ref %#" B_PRIx32 ", suspend %#" B_PRIx32 ", mf %#" B_PRIx32
		", reset %#" B_PRIx32 ", PHY control %#" B_PRIx32 ", GUSB2PHYCFG %#"
		B_PRIx32 " -> %#" B_PRIx32 ", GUSB3PIPECTL %#" B_PRIx32 " -> %#"
		B_PRIx32 "\n", name, read32(core, DWC3_GSNPSID), waited, phyWaited,
		deviceControl, pcieStatus,
		pcieBefore, usb2Status, usb2Before,
		ahbGates, mbusGates, msiLite, usbRef, usbSuspend, usbMf, usbReset,
		phyControl, usb2Config, read32(core, DWC3_GUSB2PHYCFG0), usb3Config,
		read32(core, DWC3_GUSB3PIPECTL0));
	delete_area(coreArea);
	return B_OK;
}


/*!	Checks a DesignWare USB3 core and leaves it in host mode.
	\a _brokenPortDisable is set for cores whose Port Disable does not work.
*/
static status_t
dwc3_prepare_host(phys_addr_t base, size_t size, const char* name,
	bool* _brokenPortDisable)
{
	if (size < DWC3_GLOBALS_END)
		return B_BAD_VALUE;

	void* address;
	area_id area = map_physical_memory("DWC3 globals (xhci)", base,
		DWC3_GLOBALS_END, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < 0)
		return area;
	volatile uint32* registers = (volatile uint32*)address;

	status_t status = B_OK;
	uint32 id = registers[DWC3_GSNPSID / 4];
	uint32 product = id >> 16;
	if (product != 0x5533 && product != 0x3331 && product != 0x3332) {
		dprintf("xhci: %s: not a DesignWare USB3 core (GSNPSID %#" B_PRIx32
			")\n", name, id);
		status = B_NOT_SUPPORTED;
	} else {
		uint32 control = registers[DWC3_GCTL / 4];
		if ((control & DWC3_GCTL_PRTCAPDIR_MASK) != DWC3_GCTL_PRTCAPDIR_HOST) {
			// The firmware drives every RK3588 port as a host. An OTG port
			// left in device or OTG mode is switched over the same way Linux
			// dwc3_set_prtcap() does it.
			registers[DWC3_GCTL / 4] = (control & ~DWC3_GCTL_PRTCAPDIR_MASK)
				| DWC3_GCTL_PRTCAPDIR_HOST;
			dprintf("xhci: %s: switched the DWC3 core to host mode (GCTL %#"
				B_PRIx32 ")\n", name, control);
			snooze(1000);
		}
		// Linux "quirk-broken-port-ped": DWC_usb3 3.00a and earlier.
		*_brokenPortDisable = product == 0x5533 && (id & 0xffff) <= 0x300a;
		dprintf("xhci: %s: DWC3 core %#" B_PRIx32 ", GCTL %#" B_PRIx32 "\n",
			name, id, registers[DWC3_GCTL / 4]);
	}

	delete_area(area);
	return status;
}


/*!	The SC8280XP's DWC3 cores, as the Qualcomm UEFI leaves them. Reading an
	unpowered or unclocked core is a fatal bus error, so look at the global
	clock controller first. Their DMA goes through the apps SMMU (MMU-500,
	"iommus" in the device tree): accept a core only if the SMMU passes its
	stream through untranslated.
*/
static status_t
sc8280xp_dwc3_check(phys_addr_t base, const char* name)
{
	static const struct {
		phys_addr_t base;
		uint32 gccBlock;
		uint16 stream;
	} kControllers[] = {
		{ 0xa600000, 0x0f000, 0x820 },	// usb_0 (primary)
		{ 0xa800000, 0x10000, 0x860 },	// usb_1 (secondary)
		{ 0xa400000, 0xab000, 0x800 },	// usb_2 (multiport)
	};
	static const phys_addr_t kGccBase = 0x100000;
	static const phys_addr_t kAppsSmmuBase = 0x15000000;

	uint32 gccBlock = 0, stream = 0;
	for (size_t i = 0; i < B_COUNT_OF(kControllers); i++) {
		if (kControllers[i].base == base) {
			gccBlock = kControllers[i].gccBlock;
			stream = kControllers[i].stream;
		}
	}
	if (gccBlock == 0) {
		dprintf("xhci: %s: unknown SC8280XP DWC3 instance %#" B_PRIxPHYSADDR "\n",
			name, base);
		return B_NOT_SUPPORTED;
	}

	void* address;
	area_id area = map_physical_memory("SC8280XP GCC (xhci)", kGccBase + gccBlock,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address);
	if (area < 0)
		return area;
	uint32 gdsc = *(volatile uint32*)((uint8*)address + 0x004);
	uint32 master = *(volatile uint32*)((uint8*)address + 0x010);
	delete_area(area);
	dprintf("xhci: %s: gdsc %#" B_PRIx32 ", master clock %#" B_PRIx32 "\n", name,
		gdsc, master);
	if ((gdsc & (1u << 31)) == 0 || (master & (1u << 31)) != 0) {
		dprintf("xhci: %s: left powered down by the firmware\n", name);
		return B_NOT_SUPPORTED;
	}

	area = map_physical_memory("SC8280XP apps SMMU (xhci)", kAppsSmmuBase,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address);
	if (area < 0)
		return area;
	volatile uint32* smmu = (volatile uint32*)address;
	uint32 control = smmu[0];
	uint32 groups = smmu[0x20 / 4] & 0xff;
	bool extendedIds = (control & (1u << 3)) != 0;
	int32 type = -1;
	for (uint32 i = 0; i < groups && i < 256; i++) {
		uint32 smr = smmu[0x800 / 4 + i];
		uint32 s2cr = smmu[0xc00 / 4 + i];
		bool valid = extendedIds ? (s2cr & (1u << 28)) != 0 : (smr & (1u << 31)) != 0;
		uint32 id = smr & (extendedIds ? 0xffff : 0x7fff);
		uint32 mask = (smr >> 16) & (extendedIds ? 0xffff : 0x7fff);
		if (valid && ((stream ^ id) & ~mask) == 0) {
			type = (s2cr >> 16) & 3;
			dprintf("xhci: %s: SMMU stream %#" B_PRIx32 " group %" B_PRIu32
				" SMR %#" B_PRIx32 " S2CR %#" B_PRIx32 "\n", name, stream, i, smr,
				s2cr);
			break;
		}
	}
	uint32 idr1 = smmu[0x24 / 4];
	delete_area(area);
	dprintf("xhci: %s: SMMU sCR0 %#" B_PRIx32 ", %" B_PRIu32 " groups, sIDR1 %#"
		B_PRIx32 "\n", name, control, groups, idr1);

	// A translating context bank whose translation is off passes DMA through.
	int32 contextBank = -1;
	if (type == 0) {
		size_t pageSize = (idr1 & (1u << 31)) != 0 ? 65536 : 4096;
		size_t pages = 1u << (((idr1 >> 28) & 7) + 1);
		// the s2cr of the matched group holds the context bank index
		area = map_physical_memory("SC8280XP apps SMMU (xhci)", kAppsSmmuBase,
			B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address);
		if (area >= 0) {
			smmu = (volatile uint32*)address;
			for (uint32 i = 0; i < groups && i < 256; i++) {
				uint32 smr = smmu[0x800 / 4 + i];
				if ((smr & (1u << 31)) != 0
					&& ((stream ^ (smr & 0x7fff)) & ~((smr >> 16) & 0x7fff)) == 0) {
					contextBank = smmu[0xc00 / 4 + i] & 0xff;
					break;
				}
			}
			delete_area(area);
		}
		if (contextBank >= 0) {
			phys_addr_t gr1 = kAppsSmmuBase + pageSize;
			phys_addr_t bank = kAppsSmmuBase + pages * pageSize
				+ contextBank * pageSize;
			uint32 cbar = 0, sctlr = 0, tcr = 0;
			uint64 ttbr0 = 0;
			area = map_physical_memory("SC8280XP SMMU GR1 (xhci)", gr1, B_PAGE_SIZE,
				B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address);
			if (area >= 0) {
				cbar = ((volatile uint32*)address)[contextBank];
				delete_area(area);
			}
			area = map_physical_memory("SC8280XP SMMU bank (xhci)", bank,
				B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA, &address);
			if (area >= 0) {
				volatile uint32* cb = (volatile uint32*)address;
				sctlr = cb[0];
				tcr = cb[0x30 / 4];
				ttbr0 = cb[0x20 / 4] | ((uint64)cb[0x24 / 4] << 32);
				delete_area(area);
			}
			dprintf("xhci: %s: context bank %" B_PRId32 " at %#" B_PRIxPHYSADDR
				": CBAR %#" B_PRIx32 " SCTLR %#" B_PRIx32 " TCR %#" B_PRIx32
				" TTBR0 %#" B_PRIx64 "\n", name, contextBank, bank, cbar, sctlr,
				tcr, ttbr0);
			if ((sctlr & 1) == 0)
				type = 1;
		}
	}

	bool bypass = (control & 1) != 0	// CLIENTPD: the SMMU is off
		|| type == 1
		|| (type < 0 && (control & (1u << 10)) == 0);	// unmatched pass
	if (!bypass) {
		dprintf("xhci: %s: DMA is translated or faulted by the SMMU\n", name);
		return B_NOT_SUPPORTED;
	}
	return B_OK;
}


static float
supports_fdt(device_node* parent)
{
	fdt_device_module_info* fdt;
	fdt_device* device;
	if (!usb_fdt_get_device(gDeviceManager, parent, kCompatible, &fdt, &device))
		return 0;
	// An OTG core is used as a host; a peripheral-only one is not ours.
	if (usb_fdt_has_string(fdt, device, "dr_mode", "peripheral"))
		return 0;
	// A core behind a USB-C connector takes its role, and its port its
	// power, from a Type-C controller that nothing here drives yet. On the
	// ROCK 5 ITX running that core (usb@fc000000) as a host froze the
	// system early in the boot.
	if (fdt->get_prop(device, "usb-role-switch", NULL) != NULL)
		return 0;
	return 0.8f;
}


static status_t
register_fdt(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "XHCI FDT" } },
		{}
	};
	return gDeviceManager->register_node(parent, XHCI_FDT_DRIVER, attrs, NULL,
		NULL);
}


static status_t
init_fdt(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	usb_fdt_resources resources;
	if (status == B_OK)
		status = usb_fdt_get_resources(gDeviceManager, parent, "xhci", &resources,
			true);
	if (status != B_OK) {
		gDeviceManager->put_node(parent);
		return status;
	}

	const char* name = fdt->get_name(device);
	xhci_platform_info platform = {};
	platform.register_base = resources.register_base;
	platform.register_size = resources.register_size;
	platform.interrupt = resources.interrupt;
	platform.dma_coherent = resources.dma_coherent;

	// The Qualcomm glue is the core's parent node.
	bool qualcomm = false;
	device_node* glue = gDeviceManager->get_parent_node(parent);
	if (glue != NULL) {
		fdt_device_module_info* glueFDT;
		fdt_device* glueDevice;
		const char* bus;
		if (gDeviceManager->get_attr_string(glue, B_DEVICE_BUS, &bus, false) == B_OK
			&& strcmp(bus, "fdt") == 0
			&& gDeviceManager->get_driver(glue, (driver_module_info**)&glueFDT,
				(void**)&glueDevice) == B_OK) {
			qualcomm = usb_fdt_has_string(glueFDT, glueDevice, "compatible",
					"qcom,sc8280xp-dwc3")
				|| usb_fdt_has_string(glueFDT, glueDevice, "compatible",
					"qcom,sc8280xp-dwc3-mp");
		}
		gDeviceManager->put_node(glue);
	}
	if (resources.iommu && !qualcomm) {
		dprintf("xhci: unsupported IOMMU for %s\n", name);
		status = B_NOT_SUPPORTED;
	}
	if (status == B_OK && qualcomm)
		status = sc8280xp_dwc3_check(resources.register_base, name);

	if (status == B_OK && usb_fdt_has_string(fdt, device, "compatible", "snps,dwc3")) {
		// With the GL3523 on the ROCK 5 ITX's USB 3 link the system soon
		// stopped taking input and starting programs, so these cores' USB 3
		// ports stay off unless the "xhci" driver settings ask for them with
		// "dwc3_superspeed true". USB 3 devices then run at high speed.
		void* settings = load_driver_settings("xhci");
		platform.usb2_only = !get_driver_boolean_parameter(settings,
			"dwc3_superspeed", false, true);
		unload_driver_settings(settings);

		if (usb_fdt_has_string(fdt, device, "compatible", "rockchip,rk3588-dwc3")
			&& !rk3588_dwc3_clocks_running(platform.register_base, name)) {
			status = B_NOT_SUPPORTED;
		}
		if (usb_fdt_has_string(fdt, device, "compatible",
				"allwinner,sun60i-a733-dwc3")) {
			// the USB 3 lanes carry DisplayPort: USB 2 only, always
			platform.usb2_only = true;
			status = sun60i_dwc3_power_up(platform.register_base, name);
		}
		if (status == B_OK) {
			status = dwc3_prepare_host(platform.register_base,
				platform.register_size, name, &platform.broken_port_disable);
		}
		if (status == B_OK && platform.usb2_only
			&& usb_fdt_has_string(fdt, device, "compatible",
				"rockchip,rk3588-dwc3")) {
			rk3588_dwc3_disable_u3_port(platform.register_base, name);
		}
	}
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	xhci_fdt_info* info = new(std::nothrow) xhci_fdt_info;
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;
	info->platform = platform;
	*cookie = info;
	dprintf("xhci: FDT %s, registers %#" B_PRIxPHYSADDR ", IRQ %" B_PRIu32
		", DMA %s%s%s; retaining firmware PHY/clock configuration\n", name,
		platform.register_base, platform.interrupt,
		platform.dma_coherent ? "coherent" : "noncoherent",
		platform.broken_port_disable ? ", no port disable" : "",
		platform.usb2_only ? ", USB 2 only" : "");
	return B_OK;
}


static void
uninit_fdt(void* cookie)
{
	delete (xhci_fdt_info*)cookie;
}


static status_t
register_fdt_children(void* cookie)
{
	xhci_fdt_info* info = (xhci_fdt_info*)cookie;
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, { .string = "XHCI Controller" } },
		{ B_DEVICE_FIXED_CHILD, B_STRING_TYPE,
			{ .string = USB_FOR_CONTROLLER_MODULE_NAME } },
		{}
	};
	return gDeviceManager->register_node(info->node, XHCI_FDT_BUS, attrs, NULL,
		NULL);
}


static status_t
init_fdt_bus(device_node* node, void** cookie)
{
	device_node* parent = gDeviceManager->get_parent_node(node);
	xhci_fdt_info* info;
	status_t status = gDeviceManager->get_driver(parent, NULL, (void**)&info);
	gDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	Stack* stack;
	status = gUSB->get_stack((void**)&stack);
	if (status != B_OK)
		return status;

	XHCI* xhci = new(std::nothrow) XHCI(NULL, NULL, NULL, stack, node,
		&info->platform);
	if (xhci == NULL)
		return B_NO_MEMORY;
	status = xhci->InitCheck();
	if (status == B_OK)
		status = xhci->Start();
	if (status != B_OK) {
		delete xhci;
		return status;
	}
	*cookie = xhci;
	return B_OK;
}


static void
uninit_fdt_bus(void* cookie)
{
	delete (XHCI*)cookie;
}


driver_module_info gXHCIFDTDriver = {
	{ XHCI_FDT_DRIVER, 0, NULL },
	supports_fdt,
	register_fdt,
	init_fdt,
	uninit_fdt,
	register_fdt_children,
	NULL,
	NULL
};

usb_bus_interface gXHCIFDTBus = {
	{
		{ XHCI_FDT_BUS, 0, NULL },
		NULL, NULL,
		init_fdt_bus,
		uninit_fdt_bus,
		NULL, NULL, NULL
	}
};
