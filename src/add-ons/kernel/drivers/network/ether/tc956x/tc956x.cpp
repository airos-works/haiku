/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The Ethernet functions of the Toshiba TC9563/TC956x ("QPS615") PCIe
	switch, as on the Radxa Dragon Q8B: two PCIe functions (1179:0220) behind
	the switch's internal port, each a Synopsys XGMAC 3.01a with an XPCS and
	a PMA, wired by SGMII (2500BASE-X at 2.5 Gbit/s) to a Qualcomm QCA8081
	PHY on the MAC's own MDIO bus. The PHYs' reset lines are GPIOs of the
	chip.

	The firmware leaves the switch powered and configured, so this driver
	only does what Linux's tc956x_pci, dwmac-tc956x and stmmac (dwxgmac2)
	drivers do with the endpoint functions: it maps the chip's register
	space (BAR 4) and address translation (BAR 0), keeps its MCU in reset,
	routes the MAC's interrupts through the chip's MSI generator (one MSI
	per function) and runs one DMA channel and one queue in each direction.
	The MACs reach host memory through a window the chip translates at
	0x10_0000_0000, so every address given to them is offset by that.

	PCIe is cache coherent on the SC8280XP: descriptors and buffers are
	ordinary memory. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ByteOrder.h>
#include <Drivers.h>
#include <KernelExport.h>
#include <PCI.h>
#include <net/if_media.h>

#include <ethernet.h>
#include <kernel.h>
#include <lock.h>
#include <net_buffer.h>
#include <util/AutoLock.h>

#include "ether_driver.h"


//#define TRACE_TC956X
#ifdef TRACE_TC956X
#	define TRACE(x...) dprintf("tc956x: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("tc956x: " x)
#define ERROR(x...)	dprintf("tc956x: " x)


// FreeBSD's extended Ethernet subtype IFM_X(37), which ifconfig knows as
// 2500baseT; Haiku's <net/if_media.h> has none for 2.5 Gbit/s.
#ifndef IFM_2500_T
#	define IFM_2500_T			0x805
#endif

#define TOSHIBA_VENDOR_ID		0x1179
#define TC956X_DEVICE_ID		0x0220
#define MAX_DEVICES				2

// BARs of each function
#define BAR_BRIDGE_CONFIG		0		// address translation
#define BAR_SFR					4		// the chip's registers

// chip registers (SFR space)
#define NCID					0x0000
#define CLKCTRL0				0x1004
#define RSTCTRL0				0x1008
#define CLKCTRL1				0x100c
#define RSTCTRL1				0x1010
#define EMAC_CTL(mac)			((mac) != 0 ? 0x1074 : 0x1070)
#define  EMAC_SP_SEL_MASK		0xf
#define  SP_SEL_SGMII_2500M		4
#define  SP_SEL_SGMII_1000M		5
#define  SP_SEL_SGMII_100M		6
#define  SP_SEL_SGMII_10M		7
#define  EMAC_PHY_INF_SEL_MASK	(3u << 4)
#define  PCS_CLK_PHY			(1u << 4)
#define  EMAC_INV_SGM_SIG_DET	(1u << 6)
#define  EMAC_LPIHWCLKEN		(1u << 8)
#define  EMAC_INIT_DONE			(1u << 21)
#define GPIO_EN0				0x1208	// 0: output
#define GPIO_OUT0				0x1210
#define MSIGEN(mac)				((mac) != 0 ? 0xf100 : 0xf000)
#define  MSI_OUT_EN				0x0000
#define  MSI_MASK_CLR			0x000c
#define  MSI_INT_STS			0x0010
#define  HWIRQ_EVENT			2
#define  HWIRQ_TX0				3
#define  HWIRQ_RX0				11
#define EMAC(mac)				((mac) != 0 ? 0x48000 : 0x40000)

// resets and clocks: register 0 for the chip and MAC 0, register 1 for MAC 1
#define RESET_MCU				0
#define RESET_MCU1				1
#define RESET_INTC				4
#define RESET_UART0				16
#define RESET_MSIGEN			18
#define CLOCK_MCU				0
#define CLOCK_INTC				4
#define CLOCK_SRAM				13
#define CLOCK_UART0				16
#define CLOCK_MSIGEN			18
#define CLOCK_PLL				24
#define CLOCK_SGMII				25
#define CLOCK_REFCLK			26
#define MAC_RESET_MAC			7
#define MAC_RESET_PMA			30
#define MAC_RESET_XPCS			31
#define MAC_CLOCK_TX			7
#define MAC_CLOCK_RX			14
#define MAC_CLOCK_RMII			15		// MAC 1 only
#define MAC_CLOCK_125M			29
#define MAC_CLOCK_312_5M		30
#define MAC_CLOCK_ALL			31

// address translation of the MACs' bus to PCIe (BAR 0)
#define ATR_AXI4_SLV0			0x0800
#define ATR_ENTRY(i)			(ATR_AXI4_SLV0 + (i) * 0x20)
#define  ATR_SRC_ADDR_LO		0x00
#define  ATR_SRC_ADDR_HI		0x04
#define  ATR_TRSL_ADDR_LO		0x08
#define  ATR_TRSL_ADDR_HI		0x0c
#define  ATR_TRSL_PARAM			0x10
#define  ATR_IMPL				(1u << 0)
#define  ATR_SIZE_SHIFT			1
#define DMA_WINDOW				0x0000001000000000ULL
#define DMA_WINDOW_SIZE_BITS	35		// 2^36 bytes

// PMA, in the MAC's block
#define PMATOP					0x4000
#define PMA_CML_GL_PM_CFG0		0x01b8
#define PMA_HWT_REFCK_R_EN		0x1080
#define PMA_HWT_REFCK_TERM_EN	0x1090
#define PMA_HWT_REFCK_STRIDE	0x0014
#define PMA_COMM_CFG_0_1		0x1888
#define PMA_COMM_CFG_STRIDE		0x0008
#define PMA_COMM_CFG_VALUE		(0xf7u << 9 | 1u << 8 | 0x04u)

// XPCS, in the MAC's block, indirect: a page in the viewport register,
// then the low eight address bits, in 32 bit steps
#define XPCS					0x3a00
#define XPCS_VIEWPORT			(0xff << 2)
#define MMD_PMAPMD				1
#define MMD_PCS					3
#define MMD_AN					7
#define MMD_VEND2				31
#define PCS_CTRL2				7		// MMD 3
#define  PCS_CTRL2_TYPE_NONE	4		// a reserved type: not 10GBASE-R
#define PCS_STAT2				8
#define  PCS_STAT2_10GBR		(1u << 0)
#define VR_MII_DIG_CTRL1		0x8000
#define  DIG_CTRL1_MAC_AUTO_SW	(1u << 9)
#define  DIG_CTRL1_2G5_EN		(1u << 2)
#define VR_MII_AN_CTRL			0x8001
#define  AN_CTRL_TX_CONFIG		(1u << 3)
#define  AN_CTRL_PCS_MODE_MASK	(3u << 1)
#define  AN_CTRL_PCS_MODE_SGMII	(2u << 1)

// XGMAC
#define MAC_TX_CONFIG			0x0000
#define  TX_CONFIG_TE			(1u << 0)
#define  TX_CONFIG_JD			(1u << 16)
#define  TX_CONFIG_SS_MASK		(7u << 29)
#define  TX_CONFIG_SS_2500		(2u << 29)	// GMII
#define  TX_CONFIG_SS_1000		(3u << 29)
#define  TX_CONFIG_SS_100		(4u << 29)
#define  TX_CONFIG_SS_10		(7u << 29)
#define MAC_RX_CONFIG			0x0004
#define  RX_CONFIG_RE			(1u << 0)
#define  RX_CONFIG_ACS			(1u << 1)
#define  RX_CONFIG_CST			(1u << 2)
#define MAC_PACKET_FILTER		0x0008
#define  FILTER_PR				(1u << 0)
#define  FILTER_PM				(1u << 4)
#define MAC_Q0_TX_FLOW_CTRL		0x0070
#define  TX_FLOW_TFE			(1u << 1)
#define MAC_RX_FLOW_CTRL		0x0090
#define  RX_FLOW_RFE			(1u << 0)
#define MAC_RXQ_CTRL0			0x00a0
#define  RXQ0_ENABLED_DCB		2
#define MAC_INT_EN				0x00b4
#define MAC_VERSION				0x0110
#define MAC_HW_FEATURE0			0x011c
#define MAC_HW_FEATURE1			0x0120
#define MAC_MDIO_ADDR			0x0200
#define MAC_MDIO_DATA			0x0204
#define  MDIO_READ				(3u << 16)
#define  MDIO_WRITE				(1u << 16)
#define  MDIO_SADDR				(1u << 18)
#define  MDIO_BUSY				(1u << 22)
#define MAC_MDIO_C22P			0x0220
#define MAC_ADDR0_HIGH			0x0300
#define  ADDR_AE				(1u << 31)
#define MAC_ADDR0_LOW			0x0304
#define MMC_CONTROL				0x0800
#define MMC_RX_INT_EN			0x080c
#define MMC_TX_INT_EN			0x0810
#define MMC_TX_FPE_INT_MASK		0x0a04
#define MMC_RX_FPE_INT_MASK		0x0a24
#define MMC_RX_IPC_INT_MASK		0x0a5c
#define MTL_TXQ0_OPMODE			0x1100
#define  TXQ_TSF				(1u << 1)
#define  TXQ_TXQEN_ENABLED		(2u << 2)
#define  TXQ_TQS_SHIFT			16
#define MTL_TC0_QUANTUM_WEIGHT	0x1118
#define MTL_RXQ0_OPMODE			0x1140
#define  RXQ_RSF				(1u << 5)
#define  RXQ_EHFC				(1u << 7)
#define  RXQ_RQS_SHIFT			16
#define MTL_RXQ0_FLOW_CONTROL	0x1150
#define  RXQ_RFD_SHIFT			17
#define  RXQ_RFA_SHIFT			1
#define DMA_MODE				0x3000
#define  DMA_MODE_SWR			(1u << 0)
#define  DMA_MODE_INTM_MODE1	(1u << 12)
#define  DMA_MODE_INTM_MASK		(3u << 12)
#define DMA_SYSBUS_MODE			0x3004
#define  SYSBUS_UNDEF			(1u << 0)
#define  SYSBUS_BLEN_ALL		(0x7fu << 1)	// 4 to 256 beats
#define  SYSBUS_EAME			(1u << 11)
#define  SYSBUS_EN_LPI			(1u << 15)
#define  SYSBUS_RD_OSR_SHIFT	16
#define  SYSBUS_WR_OSR_SHIFT	24
#define DMA_TX_EDMA_CTRL		0x3040
#define DMA_RX_EDMA_CTRL		0x3044
#define  EDMA_DPS				0x3fffffffu
#define DMA_CH0_CONTROL			0x3100
#define  CH_CONTROL_PBLX8		(1u << 16)
#define DMA_CH0_TX_CONTROL		0x3104
#define  TX_CONTROL_ST			(1u << 0)
#define  TX_CONTROL_TXPBL_SHIFT	16
#define DMA_CH0_RX_CONTROL		0x3108
#define  RX_CONTROL_SR			(1u << 0)
#define  RX_CONTROL_RBSZ_SHIFT	1
#define  RX_CONTROL_RXPBL_SHIFT	16
#define DMA_CH0_TXDESC_HADDR	0x3110
#define DMA_CH0_TXDESC_LADDR	0x3114
#define DMA_CH0_RXDESC_HADDR	0x3118
#define DMA_CH0_RXDESC_LADDR	0x311c
#define DMA_CH0_TXDESC_TAIL		0x3124
#define DMA_CH0_RXDESC_TAIL		0x312c
#define DMA_CH0_TXDESC_RING_LEN	0x3130
#define DMA_CH0_RXDESC_RING_LEN	0x3134
#define  RING_LEN_OWRQ_3		(3u << 24)	// XGMAC 3.01a erratum
#define DMA_CH0_INT_EN			0x3138
#define DMA_CH0_STATUS			0x3160
#define  DMA_INT_TI				(1u << 0)
#define  DMA_INT_RI				(1u << 6)
#define  DMA_INT_RBU			(1u << 7)
#define  DMA_INT_FBE			(1u << 12)
#define  DMA_INT_AI				(1u << 14)
#define  DMA_INT_NI				(1u << 15)

// descriptors
#define DES3_OWN				(1u << 31)
#define DES3_IOC				(1u << 30)	// receive, read format
#define DES3_FD					(1u << 29)	// transmit
#define DES3_LD					(1u << 28)
#define DES3_CTXT				(1u << 30)	// receive, write-back format
#define DES3_ES					(1u << 15)	// receive, write-back format
#define DES3_RX_LENGTH_MASK		0x3fff
#define DES2_IOC				(1u << 31)	// transmit

#define DESCRIPTOR_COUNT		256
#define BUFFER_SIZE				2048
#define MAX_FRAME_SIZE			1536
#define QUEUE_FIFO_SIZE			8192		// of 46 KiB each way, as Linux

// PHY: Qualcomm QCA8081
#define PHY_ADDRESS				0x1c	// on both MDIO buses of the Dragon
#define MII_BMCR				0x00
#define  BMCR_RESET				0x8000
#define  BMCR_SPEED100			0x2000
#define  BMCR_ANENABLE			0x1000
#define  BMCR_ANRESTART			0x0200
#define  BMCR_FULLDPLX			0x0100
#define  BMCR_SPEED1000			0x0040
#define MII_BMSR				0x01
#define  BMSR_LSTATUS			0x0004
#define MII_PHYSID1				0x02
#define MII_PHYSID2				0x03
#define MII_ADVERTISE			0x04
#define MII_CTRL1000			0x09
#define PHY_SPECIFIC_STATUS		0x11
#define  SS_DUPLEX				(1u << 13)
#define  SS_RESOLVED			(1u << 11)
#define  SS_SPEED_SHIFT			7
#define  SS_SPEED_MASK			7
#define AN_10GBT_CTRL			32		// MMD 7
#define  ADV_2500				0x0080
#define PHY_CNT_CTRL			0x8029	// MMD 7
#define  PHY_CNT_CRC_CHECK		(1u << 0)
#define PHY_CNT_RX_HIGH			0x802a
#define PHY_CNT_RX_LOW			0x802b
#define PHY_CNT_RX_ERRORS		0x802c
#define PHY_CNT_TX_HIGH			0x802d
#define PHY_CNT_TX_LOW			0x802e
#define PHY_CNT_TX_ERRORS		0x802f
#define QCA8081_SERDES_FIFO_CTRL 0x9072	// MMD 1 of the PHY address + 1
#define  SERDES_FIFO_RSTN		(1u << 11)


struct tc956x_descriptor {
	uint32	des0;
	uint32	des1;
	uint32	des2;
	uint32	des3;
};


struct tc956x_chip {
	phys_addr_t		sfrBase;		// function 0's
	phys_addr_t		bridgeBase;
	area_id			sfrArea;
	volatile uint8*	sfr;
	area_id			bridgeArea;
	volatile uint8*	bridge;
	uint8			revision;
	bool			initialized;
};


struct tc956x_device {
	pci_info		pci;
	uint8			mac;		// 0 or 1: the PCI function
	char			name[32];

	area_id			sfrArea;	// this function's own window
	volatile uint8*	sfr;
	volatile uint8*	emac;
	volatile uint8*	msigen;
	uint32			vector;
	bool			msi;
	bool			interruptInstalled;

	uint8			macAddress[6];

	area_id			descriptorArea;
	tc956x_descriptor* rxRing;
	tc956x_descriptor* txRing;
	phys_addr_t		rxRingAddress;
	phys_addr_t		txRingAddress;

	area_id			bufferArea;
	uint8*			buffers;
		// DESCRIPTOR_COUNT receive buffers, then as many for transmission
	phys_addr_t		buffersAddress;

	mutex			rxLock;
	mutex			txLock;
	mutex			mdioLock;
	sem_id			rxSemaphore;
	sem_id			txSemaphore;
	uint32			rxIndex;	// next descriptor the controller completes
	uint32			txIndex;	// next free descriptor
	uint32			txCleaned;	// oldest descriptor the controller may own

	int32			openCount;
	bool			nonblocking;
	bool			promiscuous;

	thread_id		linkThread;
	bool			stopping;
	sem_id			linkChangeSemaphore;
	bool			linkUp;
	uint32			linkSpeed;	// Mbit/s
	bool			fullDuplex;
	int32			interrupts;
};


int32 api_version = B_CUR_DRIVER_API_VERSION;

static pci_module_info* sPCI;
static net_buffer_module_info* sBufferModule;
static tc956x_chip sChip;
static tc956x_device* sDevices[MAX_DEVICES];
static int32 sDeviceCount;
static char* sDeviceNames[MAX_DEVICES + 1];


static inline uint32
mmio_read(volatile uint8* base, uint32 reg)
{
	return *(volatile uint32*)(base + reg);
}


static inline void
mmio_write(volatile uint8* base, uint32 reg, uint32 value)
{
	*(volatile uint32*)(base + reg) = value;
}


static inline uint32
read32(tc956x_device* device, uint32 reg)
{
	return mmio_read(device->emac, reg);
}


static inline void
write32(tc956x_device* device, uint32 reg, uint32 value)
{
	mmio_write(device->emac, reg, value);
}


static inline void
set32(tc956x_device* device, uint32 reg, uint32 bits)
{
	write32(device, reg, read32(device, reg) | bits);
}


static inline void
clear32(tc956x_device* device, uint32 reg, uint32 bits)
{
	write32(device, reg, read32(device, reg) & ~bits);
}


static inline uint64
dma_address(phys_addr_t address)
{
	return (uint64)address + DMA_WINDOW;
}


//	#pragma mark - chip


static void
chip_reset_clock(bool reset, bool register0, bool set, uint32 bit)
{
	uint32 reg = reset ? (register0 ? RSTCTRL0 : RSTCTRL1)
		: (register0 ? CLKCTRL0 : CLKCTRL1);
	uint32 value = mmio_read(sChip.sfr, reg);
	if (set)
		value |= 1u << bit;
	else
		value &= ~(1u << bit);
	mmio_write(sChip.sfr, reg, value);
}


static void
mac_reset(tc956x_device* device, uint32 bit, bool assert)
{
	chip_reset_clock(true, device->mac == 0, assert, bit);
}


static void
mac_clock(tc956x_device* device, uint32 bit, bool enable)
{
	chip_reset_clock(false, device->mac == 0, enable, bit);
}


static phys_addr_t
bar_address(const pci_info& info, int bar)
{
	phys_addr_t address = info.u.h0.base_registers[bar];
	if ((info.u.h0.base_register_flags[bar] & PCI_address_type)
			== PCI_address_type_64) {
		address |= (phys_addr_t)info.u.h0.base_registers[bar + 1] << 32;
	}
	return address;
}


/*!	What tc956x_pci does with function 0: keep the MCU and the blocks this
	driver does not use in reset, let the MACs reach PCIe through the
	translation window, and take the MSI generator out of reset.
*/
static status_t
chip_init(const pci_info& function0)
{
	if (sChip.initialized)
		return B_OK;

	sChip.sfrBase = bar_address(function0, BAR_SFR);
	sChip.bridgeBase = bar_address(function0, BAR_BRIDGE_CONFIG);
	sChip.sfrArea = map_physical_memory("tc956x registers", sChip.sfrBase,
		function0.u.h0.base_register_sizes[BAR_SFR], B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&sChip.sfr);
	if (sChip.sfrArea < 0)
		return sChip.sfrArea;
	sChip.bridgeArea = map_physical_memory("tc956x bridge",
		sChip.bridgeBase, function0.u.h0.base_register_sizes[BAR_BRIDGE_CONFIG],
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&sChip.bridge);
	if (sChip.bridgeArea < 0) {
		delete_area(sChip.sfrArea);
		return sChip.bridgeArea;
	}

	INFO("chip %#" B_PRIx32 ", clocks %#" B_PRIx32 "/%#" B_PRIx32 ", resets %#"
		B_PRIx32 "/%#" B_PRIx32 ", EMAC control %#" B_PRIx32 "/%#" B_PRIx32 "\n",
		mmio_read(sChip.sfr, NCID), mmio_read(sChip.sfr, CLKCTRL0),
		mmio_read(sChip.sfr, CLKCTRL1), mmio_read(sChip.sfr, RSTCTRL0),
		mmio_read(sChip.sfr, RSTCTRL1), mmio_read(sChip.sfr, EMAC_CTL(0)),
		mmio_read(sChip.sfr, EMAC_CTL(1)));

	chip_reset_clock(true, true, true, RESET_MCU);
	chip_reset_clock(true, true, true, RESET_MCU1);
	chip_reset_clock(true, true, true, RESET_INTC);
	chip_reset_clock(true, true, true, RESET_UART0);
	chip_reset_clock(false, true, false, CLOCK_MCU);
	chip_reset_clock(false, true, false, CLOCK_SRAM);
	chip_reset_clock(false, true, false, CLOCK_PLL);
	chip_reset_clock(false, true, false, CLOCK_SGMII);
	chip_reset_clock(false, true, false, CLOCK_REFCLK);
	chip_reset_clock(false, true, false, CLOCK_INTC);
	chip_reset_clock(false, true, false, CLOCK_UART0);
	chip_reset_clock(true, true, true, RESET_MSIGEN);
	chip_reset_clock(false, true, false, CLOCK_MSIGEN);

	sChip.revision = mmio_read(sChip.sfr, NCID) & 0xff;

	// AXI 0x10_0000_0000 - 0x1f_ffff_ffff to PCIe 0x0 - 0xf_ffff_ffff
	volatile uint8* bridge = sChip.bridge;
	mmio_write(bridge, ATR_ENTRY(0) + ATR_SRC_ADDR_LO, (uint32)DMA_WINDOW
		| DMA_WINDOW_SIZE_BITS << ATR_SIZE_SHIFT | ATR_IMPL);
	mmio_write(bridge, ATR_ENTRY(0) + ATR_SRC_ADDR_HI,
		(uint32)(DMA_WINDOW >> 32));
	mmio_write(bridge, ATR_ENTRY(0) + ATR_TRSL_ADDR_LO, 0);
	mmio_write(bridge, ATR_ENTRY(0) + ATR_TRSL_ADDR_HI, 0);
	mmio_write(bridge, ATR_ENTRY(0) + ATR_TRSL_PARAM, 0);
	for (int i = 1; i < 4; i++) {
		mmio_write(bridge, ATR_ENTRY(i) + ATR_SRC_ADDR_LO, 0);
		mmio_write(bridge, ATR_ENTRY(i) + ATR_SRC_ADDR_HI, 0);
		mmio_write(bridge, ATR_ENTRY(i) + ATR_TRSL_ADDR_LO, 0);
		mmio_write(bridge, ATR_ENTRY(i) + ATR_TRSL_ADDR_HI, 0);
		mmio_write(bridge, ATR_ENTRY(i) + ATR_TRSL_PARAM, 0);
	}

	TRACE("translation %#" B_PRIx32 " %#" B_PRIx32 " -> %#" B_PRIx32 " %#"
		B_PRIx32 ", parameters %#" B_PRIx32 "\n",
		mmio_read(bridge, ATR_ENTRY(0) + ATR_SRC_ADDR_LO),
		mmio_read(bridge, ATR_ENTRY(0) + ATR_SRC_ADDR_HI),
		mmio_read(bridge, ATR_ENTRY(0) + ATR_TRSL_ADDR_LO),
		mmio_read(bridge, ATR_ENTRY(0) + ATR_TRSL_ADDR_HI),
		mmio_read(bridge, ATR_ENTRY(0) + ATR_TRSL_PARAM));

	chip_reset_clock(false, true, true, CLOCK_MSIGEN);
	chip_reset_clock(true, true, false, RESET_MSIGEN);

	sChip.initialized = true;
	return B_OK;
}


static void
chip_uninit()
{
	if (!sChip.initialized)
		return;
	chip_reset_clock(true, true, true, RESET_MSIGEN);
	chip_reset_clock(false, true, false, CLOCK_MSIGEN);
	delete_area(sChip.bridgeArea);
	delete_area(sChip.sfrArea);
	sChip.initialized = false;
}


/*!	The PHY reset lines are GPIO 0 (MAC 0) and 1 (MAC 1), active low. */
static void
phy_hardware_reset(tc956x_device* device)
{
	uint32 mask = 1u << device->mac;
	mmio_write(sChip.sfr, GPIO_OUT0, mmio_read(sChip.sfr, GPIO_OUT0) & ~mask);
	mmio_write(sChip.sfr, GPIO_EN0, mmio_read(sChip.sfr, GPIO_EN0) & ~mask);
	snooze(11000);
	mmio_write(sChip.sfr, GPIO_OUT0, mmio_read(sChip.sfr, GPIO_OUT0) | mask);
	snooze(70000);
}


//	#pragma mark - MAC glue


/*!	Switches the PMA to the chip's reference clock and waits for the MAC to
	finish its initialization.
*/
static void
pma_init(tc956x_device* device)
{
	mac_reset(device, MAC_RESET_PMA, true);

	volatile uint8* pma = device->emac + PMATOP;
	mmio_write(pma, PMA_CML_GL_PM_CFG0, 0);
	for (uint32 i = 0; i < 5; i++) {
		mmio_write(pma, PMA_HWT_REFCK_R_EN + i * PMA_HWT_REFCK_STRIDE, 0);
		mmio_write(pma, PMA_HWT_REFCK_TERM_EN + i * PMA_HWT_REFCK_STRIDE, 0);
		mmio_write(pma, PMA_COMM_CFG_0_1 + i * PMA_COMM_CFG_STRIDE,
			PMA_COMM_CFG_VALUE);
	}

	mac_reset(device, MAC_RESET_PMA, false);

	bigtime_t timeout = system_time() + 1000000;
	while ((mmio_read(sChip.sfr, EMAC_CTL(device->mac)) & EMAC_INIT_DONE)
			== 0) {
		if (system_time() > timeout) {
			ERROR("%s: the MAC does not finish its initialization (EMAC "
				"control %#" B_PRIx32 ")\n", device->name,
				mmio_read(sChip.sfr, EMAC_CTL(device->mac)));
			return;
		}
		snooze(50);
	}
}


static void
mac_select_speed(tc956x_device* device, uint32 speed)
{
	uint32 select = speed == 2500 ? SP_SEL_SGMII_2500M
		: speed == 1000 ? SP_SEL_SGMII_1000M
		: speed == 100 ? SP_SEL_SGMII_100M : SP_SEL_SGMII_10M;

	uint32 reg = EMAC_CTL(device->mac);
	uint32 value = mmio_read(sChip.sfr, reg);
	value |= EMAC_LPIHWCLKEN;
	value &= ~(EMAC_INV_SGM_SIG_DET | EMAC_PHY_INF_SEL_MASK | EMAC_SP_SEL_MASK);
	value |= PCS_CLK_PHY | select;
	mmio_write(sChip.sfr, reg, value);
}


static void
mac_disable(tc956x_device* device)
{
	mac_reset(device, MAC_RESET_MAC, true);
	mac_reset(device, MAC_RESET_PMA, true);
	mac_reset(device, MAC_RESET_XPCS, true);
	mac_clock(device, MAC_CLOCK_ALL, false);
	mac_clock(device, MAC_CLOCK_RX, false);
	mac_clock(device, MAC_CLOCK_TX, false);
	if (device->mac != 0)
		mac_clock(device, MAC_CLOCK_RMII, false);
}


static void
mac_enable(tc956x_device* device)
{
	mac_clock(device, MAC_CLOCK_125M, false);
	mac_clock(device, MAC_CLOCK_312_5M, false);
	mac_disable(device);

	mac_clock(device, MAC_CLOCK_TX, true);
	mac_clock(device, MAC_CLOCK_RX, true);
	mac_clock(device, MAC_CLOCK_ALL, true);
	if (device->mac != 0)
		mac_clock(device, MAC_CLOCK_RMII, true);

	mac_select_speed(device, 2500);
	mac_reset(device, MAC_RESET_MAC, false);
	pma_init(device);
	mac_reset(device, MAC_RESET_XPCS, false);
}



//	#pragma mark - XPCS


static uint16
xpcs_read(tc956x_device* device, uint32 mmd, uint32 reg)
{
	uint32 address = mmd << 16 | reg;
	volatile uint8* xpcs = device->emac + XPCS;
	mmio_write(xpcs, XPCS_VIEWPORT, address >> 8);
	return mmio_read(xpcs, (address & 0xff) << 2) & 0xffff;
}


static void
xpcs_write(tc956x_device* device, uint32 mmd, uint32 reg, uint16 value)
{
	uint32 address = mmd << 16 | reg;
	volatile uint8* xpcs = device->emac + XPCS;
	mmio_write(xpcs, XPCS_VIEWPORT, address >> 8);
	mmio_write(xpcs, (address & 0xff) << 2, value);
}


static void
xpcs_modify(tc956x_device* device, uint32 mmd, uint32 reg, uint16 mask,
	uint16 value)
{
	xpcs_write(device, mmd, reg,
		(xpcs_read(device, mmd, reg) & ~mask) | (value & mask));
}


/*!	An XPCS that can do 10GBASE-R starts in it, and its 1G/2.5G (MII)
	registers then read as zero: select a reserved PCS type to get out of it
	(Linux' xpcs_config_operating_mode()), then reset the MII side.
*/
static status_t
xpcs_reset(tc956x_device* device)
{
	if ((xpcs_read(device, MMD_PCS, PCS_STAT2) & PCS_STAT2_10GBR) != 0)
		xpcs_write(device, MMD_PCS, PCS_CTRL2, PCS_CTRL2_TYPE_NONE);

	xpcs_write(device, MMD_VEND2, MII_BMCR, BMCR_RESET);
	bigtime_t timeout = system_time() + 600000;
	while ((xpcs_read(device, MMD_VEND2, MII_BMCR) & BMCR_RESET) != 0) {
		if (system_time() > timeout) {
			ERROR("%s: the XPCS does not come out of reset\n", device->name);
			return B_TIMED_OUT;
		}
		snooze(1000);
	}
	if ((xpcs_read(device, MMD_PCS, PCS_STAT2) & PCS_STAT2_10GBR) != 0)
		xpcs_write(device, MMD_PCS, PCS_CTRL2, PCS_CTRL2_TYPE_NONE);
	INFO("%s: XPCS %04x:%04x, MII control %#x status %#x\n", device->name,
		xpcs_read(device, MMD_PCS, MII_PHYSID1),
		xpcs_read(device, MMD_PCS, MII_PHYSID2),
		xpcs_read(device, MMD_VEND2, MII_BMCR),
		xpcs_read(device, MMD_VEND2, MII_BMSR));
	return B_OK;
}


/*!	Configures the XPCS for what the PHY's side of the SGMII link runs at:
	2500BASE-X without negotiation at 2.5 Gbit/s, otherwise SGMII with the
	speed set from the PHY's state.
*/
static void
xpcs_link_up(tc956x_device* device, uint32 speed, bool fullDuplex)
{
	if (speed == 2500) {
		xpcs_modify(device, MMD_VEND2, VR_MII_DIG_CTRL1,
			DIG_CTRL1_2G5_EN | DIG_CTRL1_MAC_AUTO_SW, DIG_CTRL1_2G5_EN);
		xpcs_modify(device, MMD_VEND2, MII_BMCR,
			BMCR_ANENABLE | BMCR_SPEED1000 | BMCR_SPEED100, BMCR_SPEED1000);
		return;
	}

	xpcs_modify(device, MMD_VEND2, MII_BMCR, BMCR_ANENABLE, 0);
	xpcs_modify(device, MMD_VEND2, VR_MII_AN_CTRL,
		AN_CTRL_PCS_MODE_MASK | AN_CTRL_TX_CONFIG, AN_CTRL_PCS_MODE_SGMII);
	xpcs_modify(device, MMD_VEND2, VR_MII_DIG_CTRL1,
		DIG_CTRL1_2G5_EN | DIG_CTRL1_MAC_AUTO_SW, 0);

	// Negotiation stays enabled: without it, the TC956x's XPCS passes no
	// frames at 1 Gbit/s and below (Linux' "Preserve BMCR_ANENABLE during
	// link up" for this chip).
	uint16 control = BMCR_ANENABLE | (speed == 1000 ? BMCR_SPEED1000
		: speed == 100 ? BMCR_SPEED100 : 0);
	if (fullDuplex)
		control |= BMCR_FULLDPLX;
	xpcs_write(device, MMD_VEND2, MII_BMCR, control);
}


//	#pragma mark - MDIO and PHY


static status_t
mdio_wait(tc956x_device* device)
{
	bigtime_t timeout = system_time() + 20000;
	while ((read32(device, MAC_MDIO_DATA) & MDIO_BUSY) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(10);
	}
	return B_OK;
}


static int32
mdio_access(tc956x_device* device, uint32 address, uint32 command)
{
	MutexLocker locker(device->mdioLock);
	if (mdio_wait(device) != B_OK)
		return B_TIMED_OUT;
	write32(device, MAC_MDIO_ADDR, address);
	write32(device, MAC_MDIO_DATA, command | MDIO_BUSY);
	if (mdio_wait(device) != B_OK)
		return B_TIMED_OUT;
	return read32(device, MAC_MDIO_DATA) & 0xffff;
}


static void
mdio_clause(tc956x_device* device, uint32 phy, bool clause22)
{
	uint32 ports = read32(device, MAC_MDIO_C22P);
	if (clause22)
		ports |= 1u << phy;
	else
		ports &= ~(1u << phy);
	write32(device, MAC_MDIO_C22P, ports);
}


static int32
mdio_read(tc956x_device* device, uint32 reg)
{
	mdio_clause(device, PHY_ADDRESS, true);
	return mdio_access(device, PHY_ADDRESS << 16 | (reg & 0x1f), MDIO_READ);
}


static status_t
mdio_write(tc956x_device* device, uint32 reg, uint16 value)
{
	mdio_clause(device, PHY_ADDRESS, true);
	int32 result = mdio_access(device, PHY_ADDRESS << 16 | (reg & 0x1f),
		MDIO_SADDR | MDIO_WRITE | value);
	return result < 0 ? result : B_OK;
}


static int32
mdio_read_mmd(tc956x_device* device, uint32 phy, uint32 mmd, uint32 reg)
{
	mdio_clause(device, phy, false);
	return mdio_access(device, mmd << 21 | phy << 16 | (reg & 0xffff),
		MDIO_READ);
}


static status_t
mdio_write_mmd(tc956x_device* device, uint32 phy, uint32 mmd, uint32 reg,
	uint16 value)
{
	mdio_clause(device, phy, false);
	int32 result = mdio_access(device, mmd << 21 | phy << 16 | (reg & 0xffff),
		MDIO_WRITE | value);
	return result < 0 ? result : B_OK;
}


static status_t
phy_init(tc956x_device* device)
{
	int32 id1 = mdio_read(device, MII_PHYSID1);
	int32 id2 = mdio_read(device, MII_PHYSID2);
	if (id1 < 0 || id2 < 0 || id1 == 0xffff || (id1 == 0 && id2 == 0)) {
		ERROR("%s: no PHY at MDIO address %d (%" B_PRId32 ", %" B_PRId32 ")\n",
			device->name, PHY_ADDRESS, id1, id2);
		return B_DEVICE_NOT_FOUND;
	}
	INFO("%s: PHY %04" B_PRIx32 ":%04" B_PRIx32 "\n", device->name, id1, id2);

	mdio_write(device, MII_BMCR, BMCR_RESET);
	bigtime_t timeout = system_time() + 500000;
	while ((mdio_read(device, MII_BMCR) & BMCR_RESET) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(1000);
	}

	// everything up to 2500BASE-T full duplex, with pause
	mdio_write(device, MII_ADVERTISE, 0x0001 | 0x01e0 | 0x0400);
	mdio_write(device, MII_CTRL1000, 0x0200);
	int32 control = mdio_read_mmd(device, PHY_ADDRESS, MMD_AN, AN_10GBT_CTRL);
	if (control >= 0) {
		mdio_write_mmd(device, PHY_ADDRESS, MMD_AN, AN_10GBT_CTRL,
			control | ADV_2500);
	}
	mdio_write(device, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

	// packet counters with CRC checks
	int32 counters = mdio_read_mmd(device, PHY_ADDRESS, MMD_AN, PHY_CNT_CTRL);
	if (counters >= 0) {
		mdio_write_mmd(device, PHY_ADDRESS, MMD_AN, PHY_CNT_CTRL,
			counters | PHY_CNT_CRC_CHECK);
	}
	return B_OK;
}


static void
update_link(tc956x_device* device)
{
	// the link bit latches a failure: read twice
	mdio_read(device, MII_BMSR);
	int32 status = mdio_read(device, MII_BMSR);
	int32 specific = mdio_read(device, PHY_SPECIFIC_STATUS);
	bool up = status >= 0 && (status & BMSR_LSTATUS) != 0
		&& specific >= 0 && (specific & SS_RESOLVED) != 0;

	uint32 speed = 0;
	bool fullDuplex = false;
	if (up) {
		switch ((specific >> SS_SPEED_SHIFT) & SS_SPEED_MASK) {
			case 0:
				speed = 10;
				break;
			case 1:
				speed = 100;
				break;
			case 2:
				speed = 1000;
				break;
			case 4:
				speed = 2500;
				break;
			default:
				up = false;
				break;
		}
		fullDuplex = (specific & SS_DUPLEX) != 0;
	}

	if (up == device->linkUp && speed == device->linkSpeed
		&& fullDuplex == device->fullDuplex) {
		return;
	}

	if (up) {
		// what phylink does at link up: the PCS, the MAC's speed, the
		// glue's speed with a new PMA initialization
		uint32 config = read32(device, MAC_TX_CONFIG) & ~TX_CONFIG_SS_MASK;
		config |= speed == 2500 ? TX_CONFIG_SS_2500
			: speed == 1000 ? TX_CONFIG_SS_1000
			: speed == 100 ? TX_CONFIG_SS_100 : TX_CONFIG_SS_10;
		write32(device, MAC_TX_CONFIG, config);
		mac_select_speed(device, speed);
		pma_init(device);
		xpcs_link_up(device, speed, fullDuplex);
	}

	// The PHY's SGMII FIFO is held in reset while the link is down.
	int32 fifo = mdio_read_mmd(device, PHY_ADDRESS + 1, MMD_PMAPMD,
		QCA8081_SERDES_FIFO_CTRL);
	if (fifo >= 0) {
		mdio_write_mmd(device, PHY_ADDRESS + 1, MMD_PMAPMD,
			QCA8081_SERDES_FIFO_CTRL, up ? (fifo | SERDES_FIFO_RSTN)
				: (fifo & ~SERDES_FIFO_RSTN));
	}

	device->linkUp = up;
	device->linkSpeed = speed;
	device->fullDuplex = fullDuplex;

	if (up) {
		INFO("%s: link up, %" B_PRIu32 " Mbit/s %s duplex\n", device->name,
			speed, fullDuplex ? "full" : "half");
	} else
		INFO("%s: link down\n", device->name);

	if (device->linkChangeSemaphore >= 0)
		release_sem_etc(device->linkChangeSemaphore, 1, B_DO_NOT_RESCHEDULE);
}


#ifdef TRACE_TC956X
static void
dump_state(tc956x_device* device)
{
	INFO("%s: DMA status %#" B_PRIx32 " enable %#" B_PRIx32 " mode %#" B_PRIx32
		" sysbus %#" B_PRIx32 ", TX control %#" B_PRIx32 " RX control %#"
		B_PRIx32 ", MSI status %#" B_PRIx32 " enable %#" B_PRIx32 "\n",
		device->name, read32(device, DMA_CH0_STATUS),
		read32(device, DMA_CH0_INT_EN), read32(device, DMA_MODE),
		read32(device, DMA_SYSBUS_MODE), read32(device, DMA_CH0_TX_CONTROL),
		read32(device, DMA_CH0_RX_CONTROL),
		mmio_read(device->msigen, MSI_INT_STS),
		mmio_read(device->msigen, MSI_OUT_EN));
	INFO("%s: MMC tx %" B_PRIu32 " frames %" B_PRIu32 " bytes, underflow %"
		B_PRIu32 "; rx %" B_PRIu32 " frames %" B_PRIu32 " bytes, CRC %"
		B_PRIu32 ", overflow %" B_PRIu32 "; MAC tx %#" B_PRIx32 " rx %#"
		B_PRIx32 "\n", device->name, read32(device, MMC_CONTROL + 0x1c),
		read32(device, MMC_CONTROL + 0x14), read32(device, MMC_CONTROL + 0x7c),
		read32(device, MMC_CONTROL + 0x100), read32(device, MMC_CONTROL + 0x108),
		read32(device, MMC_CONTROL + 0x128), read32(device, MMC_CONTROL + 0x190),
		read32(device, MAC_TX_CONFIG), read32(device, MAC_RX_CONFIG));
	INFO("%s: tx ring %" B_PRIu32 "/%" B_PRIu32 " des3 %#" B_PRIx32 " %#"
		B_PRIx32 ", rx %" B_PRIu32 " des3 %#" B_PRIx32 ", XPCS BMCR %#x BMSR %#x"
		" dig %#x an %#x status %#x, EMAC control %#" B_PRIx32 "\n",
		device->name, device->txCleaned, device->txIndex,
		device->txRing[0].des3, device->txRing[1].des3, device->rxIndex,
		device->rxRing[device->rxIndex].des3,
		xpcs_read(device, MMD_VEND2, MII_BMCR),
		xpcs_read(device, MMD_VEND2, MII_BMSR),
		xpcs_read(device, MMD_VEND2, VR_MII_DIG_CTRL1),
		xpcs_read(device, MMD_VEND2, VR_MII_AN_CTRL),
		xpcs_read(device, MMD_VEND2, 0x8002),
		mmio_read(sChip.sfr, EMAC_CTL(device->mac)));

	INFO("%s: %" B_PRId32 " interrupts, MAC interrupt status %#" B_PRIx32
		", MTL %#" B_PRIx32 "\n", device->name, device->interrupts,
		read32(device, 0x00b0), read32(device, 0x1020));
	INFO("%s: PHY rx %" B_PRId32 ":%" B_PRId32 " errors %" B_PRId32 ", tx %"
		B_PRId32 ":%" B_PRId32 " errors %" B_PRId32 ", status %#" B_PRIx32
		", SerDes FIFO %#" B_PRIx32 "\n", device->name,
		mdio_read_mmd(device, PHY_ADDRESS, MMD_AN, PHY_CNT_RX_HIGH),
		mdio_read_mmd(device, PHY_ADDRESS, MMD_AN, PHY_CNT_RX_LOW),
		mdio_read_mmd(device, PHY_ADDRESS, MMD_AN, PHY_CNT_RX_ERRORS),
		mdio_read_mmd(device, PHY_ADDRESS, MMD_AN, PHY_CNT_TX_HIGH),
		mdio_read_mmd(device, PHY_ADDRESS, MMD_AN, PHY_CNT_TX_LOW),
		mdio_read_mmd(device, PHY_ADDRESS, MMD_AN, PHY_CNT_TX_ERRORS),
		mdio_read(device, PHY_SPECIFIC_STATUS),
		mdio_read_mmd(device, PHY_ADDRESS + 1, MMD_PMAPMD,
			QCA8081_SERDES_FIFO_CTRL));
}
#endif


static status_t
link_thread(void* data)
{
	tc956x_device* device = (tc956x_device*)data;
#ifdef TRACE_TC956X
	int32 upSeconds = 0;
#endif
	while (!device->stopping) {
		update_link(device);
#ifdef TRACE_TC956X
		upSeconds = device->linkUp ? upSeconds + 1 : 0;
		if (upSeconds == 3 || upSeconds == 10)
			dump_state(device);
#endif
		snooze(1000000);
	}
	return B_OK;
}


//	#pragma mark - controller


static int32
tc956x_interrupt(void* data)
{
	tc956x_device* device = (tc956x_device*)data;
	device->interrupts++;

	uint32 sources = mmio_read(device->msigen, MSI_INT_STS);
	uint32 status = read32(device, DMA_CH0_STATUS);
	uint32 enabled = read32(device, DMA_CH0_INT_EN);

	int32 result = B_UNHANDLED_INTERRUPT;
	if (status != 0) {
		write32(device, DMA_CH0_STATUS, status);

		if ((status & DMA_INT_FBE) != 0) {
			ERROR("%s: fatal bus error, DMA status %#" B_PRIx32 "\n",
				device->name, status);
		}

		// The reader drains RX with the interrupt off. TX completion
		// interrupts are only needed when a writer runs out of descriptors.
		uint32 handled = status & enabled & (DMA_INT_RI | DMA_INT_TI);
		write32(device, DMA_CH0_INT_EN, enabled & ~handled);
		if ((handled & DMA_INT_RI) != 0)
			release_sem_etc(device->rxSemaphore, 1, B_DO_NOT_RESCHEDULE);
		if ((handled & DMA_INT_TI) != 0)
			release_sem_etc(device->txSemaphore, 1, B_DO_NOT_RESCHEDULE);
		result = handled != 0 ? B_INVOKE_SCHEDULER : B_HANDLED_INTERRUPT;
	} else if (sources != 0)
		result = B_HANDLED_INTERRUPT;

	// The MSI is an edge: re-arm the generator, which sends another one if
	// a source is still asserted.
	mmio_write(device->msigen, MSI_MASK_CLR, 1);
	return result;
}


static inline phys_addr_t
rx_descriptor_address(tc956x_device* device, uint32 index)
{
	return device->rxRingAddress + index * sizeof(tc956x_descriptor);
}


static inline phys_addr_t
tx_descriptor_address(tc956x_device* device, uint32 index)
{
	return device->txRingAddress + index * sizeof(tc956x_descriptor);
}


static void
give_rx_descriptor(tc956x_device* device, uint32 index)
{
	uint64 address = dma_address(device->buffersAddress
		+ (uint64)index * BUFFER_SIZE);
	tc956x_descriptor& descriptor = device->rxRing[index];
	descriptor.des0 = (uint32)address;
	descriptor.des1 = (uint32)(address >> 32);
	descriptor.des2 = 0;
	memory_write_barrier();
	descriptor.des3 = DES3_OWN | DES3_IOC;
}


static void
tc956x_stop(tc956x_device* device)
{
	mmio_write(device->msigen, MSI_OUT_EN, 0);
	write32(device, DMA_CH0_INT_EN, 0);

	clear32(device, DMA_CH0_TX_CONTROL, TX_CONTROL_ST);
	snooze(1000);
	clear32(device, MAC_TX_CONFIG, TX_CONFIG_TE);
	clear32(device, MAC_RX_CONFIG, RX_CONFIG_RE);
	clear32(device, DMA_CH0_RX_CONTROL, RX_CONTROL_SR);
	write32(device, DMA_CH0_STATUS, 0xffffffff);
}


static status_t
tc956x_start(tc956x_device* device)
{
	mmio_write(device->msigen, MSI_OUT_EN, 0);

	mac_enable(device);
	phy_hardware_reset(device);

	uint32 version = read32(device, MAC_VERSION);
	INFO("%s: XGMAC %#" B_PRIx32 ", features %#" B_PRIx32 " %#" B_PRIx32
		", EMAC control %#" B_PRIx32 ", XPCS %04x:%04x\n", device->name,
		version, read32(device, MAC_HW_FEATURE0),
		read32(device, MAC_HW_FEATURE1),
		mmio_read(sChip.sfr, EMAC_CTL(device->mac)),
		xpcs_read(device, MMD_VEND2, MII_PHYSID1),
		xpcs_read(device, MMD_VEND2, MII_PHYSID2));
	if (version == 0 || version == 0xffffffff)
		return B_DEVICE_NOT_FOUND;

	// software reset of the DMA (and MAC)
	write32(device, DMA_MODE, read32(device, DMA_MODE) | DMA_MODE_SWR);
	bigtime_t timeout = system_time() + 100000;
	while ((read32(device, DMA_MODE) & DMA_MODE_SWR) != 0) {
		if (system_time() > timeout) {
			ERROR("%s: the DMA software reset does not finish\n",
				device->name);
			return B_TIMED_OUT;
		}
		snooze(100);
	}

	xpcs_reset(device);

	// no MMC counter interrupts
	write32(device, MMC_RX_INT_EN, 0);
	write32(device, MMC_TX_INT_EN, 0);
	write32(device, MMC_TX_FPE_INT_MASK, 0xffffffff);
	write32(device, MMC_RX_FPE_INT_MASK, 0xffffffff);
	write32(device, MMC_RX_IPC_INT_MASK, 0xffffffff);
	write32(device, MAC_INT_EN, 0);

	// DMA: the channel's interrupts on their own lines of the MSI generator
	uint32 mode = read32(device, DMA_MODE) & ~DMA_MODE_INTM_MASK;
	write32(device, DMA_MODE, mode | DMA_MODE_INTM_MODE1);
	write32(device, DMA_SYSBUS_MODE, SYSBUS_EAME | SYSBUS_EN_LPI
		| 31u << SYSBUS_WR_OSR_SHIFT | 31u << SYSBUS_RD_OSR_SHIFT
		| SYSBUS_UNDEF | SYSBUS_BLEN_ALL);
	write32(device, DMA_TX_EDMA_CTRL, EDMA_DPS);
	write32(device, DMA_RX_EDMA_CTRL, EDMA_DPS);

	write32(device, DMA_CH0_CONTROL, CH_CONTROL_PBLX8);
	write32(device, DMA_CH0_TX_CONTROL, 32u << TX_CONTROL_TXPBL_SHIFT);
	write32(device, DMA_CH0_RX_CONTROL, 32u << RX_CONTROL_RXPBL_SHIFT
		| BUFFER_SIZE << RX_CONTROL_RBSZ_SHIFT);

	// MTL: store and forward, one queue each way
	uint32 queueSize = QUEUE_FIFO_SIZE / 256 - 1;
	write32(device, MTL_TXQ0_OPMODE, TXQ_TSF | TXQ_TXQEN_ENABLED
		| queueSize << TXQ_TQS_SHIFT);
	write32(device, MTL_TC0_QUANTUM_WEIGHT, 0x10);
	write32(device, MTL_RXQ0_FLOW_CONTROL,
		0x7u << RXQ_RFD_SHIFT | 0x4u << RXQ_RFA_SHIFT);
	write32(device, MTL_RXQ0_OPMODE, RXQ_RSF | RXQ_EHFC
		| queueSize << RXQ_RQS_SHIFT);

	// MAC
	write32(device, MAC_RXQ_CTRL0, RXQ0_ENABLED_DCB);
	write32(device, MAC_PACKET_FILTER,
		device->promiscuous ? FILTER_PR : FILTER_PM);
	write32(device, MAC_Q0_TX_FLOW_CTRL, 0xffffu << 16 | TX_FLOW_TFE);
	write32(device, MAC_RX_FLOW_CTRL, RX_FLOW_RFE);
	write32(device, MAC_TX_CONFIG, TX_CONFIG_JD | TX_CONFIG_SS_2500);
	write32(device, MAC_RX_CONFIG, RX_CONFIG_ACS | RX_CONFIG_CST);

	const uint8* mac = device->macAddress;
	write32(device, MAC_ADDR0_HIGH, ADDR_AE | mac[5] << 8 | mac[4]);
	write32(device, MAC_ADDR0_LOW,
		mac[3] << 24 | mac[2] << 16 | mac[1] << 8 | mac[0]);

	// the rings
	memset(device->txRing, 0, DESCRIPTOR_COUNT * sizeof(tc956x_descriptor));
	for (uint32 i = 0; i < DESCRIPTOR_COUNT; i++)
		give_rx_descriptor(device, i);
	memory_full_barrier();
	device->rxIndex = 0;
	device->txIndex = 0;
	device->txCleaned = 0;

	uint64 txRing = dma_address(device->txRingAddress);
	uint64 rxRing = dma_address(device->rxRingAddress);
	write32(device, DMA_CH0_TXDESC_HADDR, (uint32)(txRing >> 32));
	write32(device, DMA_CH0_TXDESC_LADDR, (uint32)txRing);
	write32(device, DMA_CH0_TXDESC_RING_LEN, DESCRIPTOR_COUNT - 1);
	write32(device, DMA_CH0_RXDESC_HADDR, (uint32)(rxRing >> 32));
	write32(device, DMA_CH0_RXDESC_LADDR, (uint32)rxRing);
	write32(device, DMA_CH0_RXDESC_RING_LEN,
		RING_LEN_OWRQ_3 | (DESCRIPTOR_COUNT - 1));

	// nothing to send; everything to receive into (the tail points past
	// the last descriptor the controller may use)
	write32(device, DMA_CH0_TXDESC_TAIL, (uint32)txRing);
	write32(device, DMA_CH0_RXDESC_TAIL,
		(uint32)dma_address(rx_descriptor_address(device,
			DESCRIPTOR_COUNT - 1)));

	write32(device, DMA_CH0_STATUS, 0xffffffff);
	write32(device, DMA_CH0_INT_EN, DMA_INT_NI | DMA_INT_AI | DMA_INT_FBE
		| DMA_INT_RI);

	set32(device, DMA_CH0_TX_CONTROL, TX_CONTROL_ST);
	set32(device, DMA_CH0_RX_CONTROL, RX_CONTROL_SR);

	// Only channel 0's lines: the common one (MAC and MTL events) would
	// need its sources handled, or it asserts forever.
	mmio_write(device->msigen, MSI_OUT_EN, 1u << HWIRQ_TX0 | 1u << HWIRQ_RX0);
	mmio_write(device->msigen, MSI_MASK_CLR, 1);

	device->linkUp = false;
	device->linkSpeed = 0;
	status_t status = phy_init(device);
	if (status != B_OK)
		return status;

	set32(device, MAC_TX_CONFIG, TX_CONFIG_TE);
	set32(device, MAC_RX_CONFIG, RX_CONFIG_RE);
	return B_OK;
}


static status_t
tc956x_receive(tc956x_device* device, net_buffer** _buffer)
{
	while (true) {
		MutexLocker locker(device->rxLock);

		uint32 index = device->rxIndex;
		tc956x_descriptor& descriptor = device->rxRing[index];
		uint32 status = descriptor.des3;
		if ((status & DES3_OWN) == 0) {
			memory_read_barrier();
			uint32 length = status & DES3_RX_LENGTH_MASK;
			uint8* data = device->buffers + (uint64)index * BUFFER_SIZE;

			net_buffer* buffer = NULL;
			bool good = (status & (DES3_CTXT | DES3_LD)) == DES3_LD
				&& (status & DES3_ES) == 0
				&& length > ETHER_HEADER_LENGTH && length <= BUFFER_SIZE;
			if (good) {
				buffer = sBufferModule->create(0);
				if (buffer != NULL
					&& sBufferModule->append(buffer, data, length) != B_OK) {
					sBufferModule->free(buffer);
					buffer = NULL;
				}
			} else {
				TRACE("%s: dropped frame, status %#" B_PRIx32 "\n",
					device->name, status);
			}

			// back to the controller
			give_rx_descriptor(device, index);
			device->rxIndex = (index + 1) % DESCRIPTOR_COUNT;
			memory_full_barrier();
			write32(device, DMA_CH0_RXDESC_TAIL,
				(uint32)dma_address(rx_descriptor_address(device, index)));

			if (buffer != NULL) {
				*_buffer = buffer;
				return B_OK;
			}
			continue;
		}

		if (device->nonblocking)
			return B_WOULD_BLOCK;

		// Arm, then recheck: a frame can arrive after the empty-ring test
		// while the interrupt is still off. Never sleep with it pending.
		write32(device, DMA_CH0_STATUS, DMA_INT_RI);
		set32(device, DMA_CH0_INT_EN, DMA_INT_RI);
		if ((device->rxRing[device->rxIndex].des3 & DES3_OWN) == 0) {
			clear32(device, DMA_CH0_INT_EN, DMA_INT_RI);
			continue;
		}
		locker.Unlock();

		status_t result = acquire_sem_etc(device->rxSemaphore, 1,
			B_CAN_INTERRUPT, 0);
		if (result != B_OK)
			return result;
	}
}


/*!	Forgets the descriptors the controller has sent. */
static void
clean_tx(tc956x_device* device)
{
	while (device->txCleaned != device->txIndex
		&& (device->txRing[device->txCleaned].des3 & DES3_OWN) == 0) {
		device->txCleaned = (device->txCleaned + 1) % DESCRIPTOR_COUNT;
	}
}


static status_t
tc956x_send(tc956x_device* device, net_buffer* buffer)
{
	size_t length = buffer->size;
	if (length > MAX_FRAME_SIZE)
		return B_BAD_DATA;

	MutexLocker locker(device->txLock);
	while (true) {
		clean_tx(device);
		uint32 pending = (device->txIndex + DESCRIPTOR_COUNT
			- device->txCleaned) % DESCRIPTOR_COUNT;
		if (pending < DESCRIPTOR_COUNT - 1)
			break;

		if (device->nonblocking)
			return B_WOULD_BLOCK;

		write32(device, DMA_CH0_STATUS, DMA_INT_TI);
		set32(device, DMA_CH0_INT_EN, DMA_INT_TI);
		clean_tx(device);
		if ((device->txIndex + DESCRIPTOR_COUNT - device->txCleaned)
				% DESCRIPTOR_COUNT < DESCRIPTOR_COUNT - 1) {
			continue;
		}
		locker.Unlock();

		status_t status = acquire_sem_etc(device->txSemaphore, 1,
			B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 100000);
		if (status != B_OK && status != B_TIMED_OUT)
			return status;
		locker.Lock();
	}

	uint32 index = device->txIndex;
	uint8* data = device->buffers
		+ (uint64)(DESCRIPTOR_COUNT + index) * BUFFER_SIZE;
	if (sBufferModule->read(buffer, 0, data, length) != B_OK)
		return B_BAD_DATA;
	if (length < 60) {
		memset(data + length, 0, 60 - length);
		length = 60;
	}

	uint64 address = dma_address(device->buffersAddress
		+ (uint64)(DESCRIPTOR_COUNT + index) * BUFFER_SIZE);
	tc956x_descriptor& descriptor = device->txRing[index];
	descriptor.des0 = (uint32)address;
	descriptor.des1 = (uint32)(address >> 32);
	descriptor.des2 = length;
	memory_write_barrier();
	descriptor.des3 = DES3_OWN | DES3_FD | DES3_LD | length;

	device->txIndex = (index + 1) % DESCRIPTOR_COUNT;
	memory_full_barrier();
	write32(device, DMA_CH0_TXDESC_TAIL,
		(uint32)dma_address(tx_descriptor_address(device, device->txIndex)));
	locker.Unlock();

	sBufferModule->free(buffer);
	return B_OK;
}


//	#pragma mark - device hooks


static status_t
tc956x_open(const char* name, uint32 flags, void** _cookie)
{
	tc956x_device* device = NULL;
	for (int32 i = 0; i < sDeviceCount; i++) {
		if (strcmp(sDevices[i]->name, name) == 0)
			device = sDevices[i];
	}
	if (device == NULL)
		return B_ENTRY_NOT_FOUND;

	if (atomic_add(&device->openCount, 1) != 0) {
		atomic_add(&device->openCount, -1);
		return B_BUSY;
	}

	device->nonblocking = (flags & O_NONBLOCK) != 0;
	device->stopping = false;
	device->rxSemaphore = create_sem(0, "tc956x rx");
	device->txSemaphore = create_sem(0, "tc956x tx");

	status_t status = install_io_interrupt_handler(device->vector,
		tc956x_interrupt, device, 0);
	if (status == B_OK) {
		device->interruptInstalled = true;
		status = tc956x_start(device);
	}
	if (status != B_OK) {
		tc956x_stop(device);
		if (device->interruptInstalled) {
			remove_io_interrupt_handler(device->vector, tc956x_interrupt,
				device);
			device->interruptInstalled = false;
		}
		delete_sem(device->rxSemaphore);
		delete_sem(device->txSemaphore);
		device->rxSemaphore = device->txSemaphore = -1;
		atomic_add(&device->openCount, -1);
		return status;
	}

	device->linkThread = spawn_kernel_thread(link_thread, "tc956x link",
		B_NORMAL_PRIORITY, device);
	resume_thread(device->linkThread);

	*_cookie = device;
	return B_OK;
}


static status_t
tc956x_close(void* cookie)
{
	tc956x_device* device = (tc956x_device*)cookie;

	device->stopping = true;
	status_t result;
	wait_for_thread(device->linkThread, &result);
	device->linkThread = -1;

	tc956x_stop(device);
	mac_disable(device);

	if (device->interruptInstalled) {
		remove_io_interrupt_handler(device->vector, tc956x_interrupt, device);
		device->interruptInstalled = false;
	}

	delete_sem(device->rxSemaphore);
	delete_sem(device->txSemaphore);
	device->rxSemaphore = device->txSemaphore = -1;
	device->linkUp = false;
	return B_OK;
}


static status_t
tc956x_free(void* cookie)
{
	tc956x_device* device = (tc956x_device*)cookie;
	atomic_add(&device->openCount, -1);
	return B_OK;
}


static status_t
tc956x_read(void* cookie, off_t position, void* buffer, size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
tc956x_write(void* cookie, off_t position, const void* buffer,
	size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
tc956x_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	tc956x_device* device = (tc956x_device*)cookie;

	switch (op) {
		case ETHER_GETADDR:
			return user_memcpy(buffer, device->macAddress,
				sizeof(device->macAddress));

		case ETHER_INIT:
			return B_OK;

		case ETHER_GETFRAMESIZE:
		{
			uint32 frameSize = ETHER_MAX_FRAME_SIZE;
			return user_memcpy(buffer, &frameSize, sizeof(frameSize));
		}

		case ETHER_SETPROMISC:
		{
			int32 value;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			device->promiscuous = value != 0;
			write32(device, MAC_PACKET_FILTER,
				device->promiscuous ? FILTER_PR : FILTER_PM);
			return B_OK;
		}

		case ETHER_NONBLOCK:
		{
			int32 value;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			device->nonblocking = value != 0;
			return B_OK;
		}

		case ETHER_ADDMULTI:
		case ETHER_REMMULTI:
			// all multicast frames pass anyway
			return B_OK;

		case ETHER_SET_LINK_STATE_SEM:
		{
			sem_id semaphore;
			if (user_memcpy(&semaphore, buffer, sizeof(semaphore)) != B_OK)
				return B_BAD_ADDRESS;
			device->linkChangeSemaphore = semaphore;
			return B_OK;
		}

		case ETHER_GET_LINK_STATE:
		{
			ether_link_state_t state;
			state.media = IFM_ETHER;
			if (device->linkUp) {
				state.media |= IFM_ACTIVE
					| (device->fullDuplex ? IFM_FULL_DUPLEX : IFM_HALF_DUPLEX)
					| (device->linkSpeed == 2500 ? IFM_2500_T
						: device->linkSpeed == 1000 ? IFM_1000_T
						: device->linkSpeed == 100 ? IFM_100_TX : IFM_10_T);
			}
			state.speed = (uint64)device->linkSpeed * 1000000;
			state.quality = 1000;
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case ETHER_SEND_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return tc956x_send(device, (net_buffer*)buffer);

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return tc956x_receive(device, (net_buffer**)buffer);
	}

	return B_DEV_INVALID_IOCTL;
}


static device_hooks sDeviceHooks = {
	tc956x_open,
	tc956x_close,
	tc956x_free,
	tc956x_control,
	tc956x_read,
	tc956x_write,
};


//	#pragma mark - driver


/*!	Lets the switch's ports pass the functions' memory requests (DMA and
	MSIs) upstream, as Linux' pci_set_master() does for the bridges above a
	device: the firmware leaves bus mastering off on them, and they enforce
	it. (pci_info's bus numbers are global, a bridge's bus range is the
	domain's own, so the ports are found by their IDs. The SC8280XP's root
	ports pass DMA with it off too, and are left alone.)
*/
static void
enable_upstream_bridges()
{
	pci_info info;
	for (int32 i = 0; sPCI->get_nth_pci_info(i, &info) == B_OK; i++) {
		if ((info.header_type & PCI_header_type_mask)
				!= PCI_header_type_PCI_to_PCI_bridge
			|| info.vendor_id != TOSHIBA_VENDOR_ID) {
			continue;
		}
		uint16 command = sPCI->read_pci_config(info.bus, info.device,
			info.function, PCI_command, 2);
		uint16 wanted = command | PCI_command_master | PCI_command_memory;
		INFO("bridge %u.%u.%u (%04x:%04x): command %#x%s\n", info.bus,
			info.device, info.function, info.vendor_id, info.device_id,
			command, command != wanted ? ", enabling bus mastering" : "");
		if (command != wanted) {
			sPCI->write_pci_config(info.bus, info.device, info.function,
				PCI_command, 2, wanted);
		}
	}
}


static status_t
device_init(tc956x_device* device)
{
	device->sfrArea = map_physical_memory("tc956x function registers",
		bar_address(device->pci, BAR_SFR),
		device->pci.u.h0.base_register_sizes[BAR_SFR], B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&device->sfr);
	if (device->sfrArea < 0)
		return device->sfrArea;
	device->emac = device->sfr + EMAC(device->mac);
	device->msigen = device->sfr + MSIGEN(device->mac);

	// bus master, memory decoding, here and upstream
	const pci_info& pci = device->pci;
	if (device->mac == 0)
		enable_upstream_bridges();
	uint16 command = sPCI->read_pci_config(pci.bus, pci.device, pci.function,
		PCI_command, 2);
	sPCI->write_pci_config(pci.bus, pci.device, pci.function, PCI_command, 2,
		command | PCI_command_master | PCI_command_memory);

	// one MSI, which the chip's MSI generator sends
	if (sPCI->get_msi_count(pci.bus, pci.device, pci.function) < 1
		|| sPCI->configure_msi(pci.bus, pci.device, pci.function, 1,
			&device->vector) != B_OK
		|| sPCI->enable_msi(pci.bus, pci.device, pci.function) != B_OK) {
		ERROR("%s: no MSI\n", device->name);
		delete_area(device->sfrArea);
		return B_NOT_SUPPORTED;
	}
	device->msi = true;

	size_t ringSize = DESCRIPTOR_COUNT * sizeof(tc956x_descriptor);
	void* rings;
	device->descriptorArea = create_area("tc956x descriptors", &rings,
		B_ANY_KERNEL_ADDRESS, 2 * ringSize, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (device->descriptorArea < 0) {
		delete_area(device->sfrArea);
		return device->descriptorArea;
	}
	physical_entry entry;
	get_memory_map(rings, 2 * ringSize, &entry, 1);
	memset(rings, 0, 2 * ringSize);
	device->rxRing = (tc956x_descriptor*)rings;
	device->txRing = (tc956x_descriptor*)((uint8*)rings + ringSize);
	device->rxRingAddress = entry.address;
	device->txRingAddress = entry.address + ringSize;

	size_t size = 2 * DESCRIPTOR_COUNT * BUFFER_SIZE;
	device->bufferArea = create_area("tc956x buffers",
		(void**)&device->buffers, B_ANY_KERNEL_ADDRESS, size, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (device->bufferArea < 0) {
		delete_area(device->descriptorArea);
		delete_area(device->sfrArea);
		return device->bufferArea;
	}
	get_memory_map(device->buffers, size, &entry, 1);
	device->buffersAddress = entry.address;

	if (dma_address(device->buffersAddress + size) >= DMA_WINDOW * 2
		|| dma_address(device->rxRingAddress + 2 * ringSize)
			>= DMA_WINDOW * 2) {
		ERROR("%s: memory beyond the chip's 64 GiB window\n", device->name);
		delete_area(device->bufferArea);
		delete_area(device->descriptorArea);
		delete_area(device->sfrArea);
		return B_NO_MEMORY;
	}

	mutex_init(&device->rxLock, "tc956x rx");
	mutex_init(&device->txLock, "tc956x tx");
	mutex_init(&device->mdioLock, "tc956x mdio");
	device->rxSemaphore = -1;
	device->txSemaphore = -1;
	device->linkChangeSemaphore = -1;
	device->linkThread = -1;

	// TODO: the board's addresses are in the I2C EEPROM the device tree
	// names ("nvmem-cells"); until there is a driver for it, a locally
	// administered one per MAC.
	const uint8 address[6] = {0x02, 0x11, 0x79, 0x02, 0x20,
		(uint8)device->mac};
	memcpy(device->macAddress, address, 6);

	INFO("%s: function %u.%u.%u, MSI vector %" B_PRIu32 ", revision %#x, "
		"%02x:%02x:%02x:%02x:%02x:%02x\n", device->name, pci.bus, pci.device,
		pci.function, device->vector, sChip.revision, device->macAddress[0],
		device->macAddress[1], device->macAddress[2], device->macAddress[3],
		device->macAddress[4], device->macAddress[5]);
	return B_OK;
}


static void
device_uninit(tc956x_device* device)
{
	const pci_info& pci = device->pci;
	if (device->msi) {
		sPCI->disable_msi(pci.bus, pci.device, pci.function);
		sPCI->unconfigure_msi(pci.bus, pci.device, pci.function);
	}
	mutex_destroy(&device->rxLock);
	mutex_destroy(&device->txLock);
	mutex_destroy(&device->mdioLock);
	delete_area(device->bufferArea);
	delete_area(device->descriptorArea);
	delete_area(device->sfrArea);
}


status_t
init_hardware()
{
	pci_module_info* pci;
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&pci) != B_OK)
		return B_ERROR;

	status_t status = B_DEVICE_NOT_FOUND;
	pci_info info;
	for (int32 i = 0; pci->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (info.vendor_id == TOSHIBA_VENDOR_ID
			&& info.device_id == TC956X_DEVICE_ID) {
			status = B_OK;
			break;
		}
	}

	put_module(B_PCI_MODULE_NAME);
	return status;
}


status_t
init_driver()
{
	if (get_module(B_PCI_MODULE_NAME, (module_info**)&sPCI) != B_OK)
		return B_ERROR;
	if (get_module(NET_BUFFER_MODULE_NAME, (module_info**)&sBufferModule)
			!= B_OK) {
		put_module(B_PCI_MODULE_NAME);
		return B_ERROR;
	}

	// Function 0 carries the chip's shared registers: set the chip up first.
	pci_info infos[MAX_DEVICES];
	int32 count = 0;
	pci_info info;
	for (int32 i = 0; count < MAX_DEVICES
			&& sPCI->get_nth_pci_info(i, &info) == B_OK; i++) {
		if (info.vendor_id == TOSHIBA_VENDOR_ID
			&& info.device_id == TC956X_DEVICE_ID && info.function < 2) {
			infos[count++] = info;
		}
	}

	status_t status = B_DEVICE_NOT_FOUND;
	for (int32 i = 0; i < count; i++) {
		if (infos[i].function == 0)
			status = chip_init(infos[i]);
	}
	if (status != B_OK) {
		ERROR("no usable function 0\n");
		put_module(NET_BUFFER_MODULE_NAME);
		put_module(B_PCI_MODULE_NAME);
		return status;
	}

	for (int32 i = 0; i < count; i++) {
		tc956x_device* device = (tc956x_device*)calloc(1,
			sizeof(tc956x_device));
		if (device == NULL)
			break;
		device->pci = infos[i];
		device->mac = infos[i].function;
		snprintf(device->name, sizeof(device->name), "net/tc956x/%" B_PRId32,
			sDeviceCount);
		if (device_init(device) != B_OK) {
			free(device);
			continue;
		}
		sDeviceNames[sDeviceCount] = device->name;
		sDevices[sDeviceCount++] = device;
	}
	sDeviceNames[sDeviceCount] = NULL;

	if (sDeviceCount == 0) {
		chip_uninit();
		put_module(NET_BUFFER_MODULE_NAME);
		put_module(B_PCI_MODULE_NAME);
		return B_DEVICE_NOT_FOUND;
	}
	return B_OK;
}


void
uninit_driver()
{
	for (int32 i = 0; i < sDeviceCount; i++) {
		device_uninit(sDevices[i]);
		free(sDevices[i]);
		sDevices[i] = NULL;
	}
	sDeviceCount = 0;
	chip_uninit();
	put_module(NET_BUFFER_MODULE_NAME);
	put_module(B_PCI_MODULE_NAME);
}


const char**
publish_devices()
{
	return (const char**)sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return &sDeviceHooks;
}
