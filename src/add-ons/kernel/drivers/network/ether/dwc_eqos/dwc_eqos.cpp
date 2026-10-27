/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Synopsys DesignWare Ethernet QoS ("GMAC 4.x/5.x"), as the Allwinner
	A733's GMAC ("gmac210", DWMAC 5.20) on the Radxa Cubie A7S, with its
	Maxio MAE0621A RGMII PHY on the controller's MDIO bus.

	One DMA channel and one queue in each direction, 256 descriptors each.
	The descriptors live in uncached memory; packets are copied to and from
	cacheable buffers with explicit cache maintenance, as the Pi's GENET
	driver does. The MAC passes the frames addressed to it, broadcasts and
	all multicast frames, or everything in promiscuous mode.

	The core follows U-Boot's dwc_eth_qos.c and Linux' stmmac (dwmac4);
	the Allwinner glue (clocks, resets, RGMII delay register, PHY reset
	line) follows U-Boot's dwc_eth_qos_sunxi.c and the A733 device trees. */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ByteOrder.h>
#include <KernelExport.h>
#include <bus/FDT.h>
#include <device_manager.h>
#include <net/if_media.h>
#include <sys/sockio.h>

#include <ethernet.h>
#include <kernel.h>
#include <lock.h>
#include <net_buffer.h>
#include <util/AutoLock.h>
#include <vm/vm.h>

#include "ether_driver.h"


//#define TRACE_EQOS
#ifdef TRACE_EQOS
#	define TRACE(x...) dprintf("dwc_eqos: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("dwc_eqos: " x)
#define ERROR(x...)	dprintf("dwc_eqos: " x)


#define EQOS_DRIVER_MODULE_NAME		"drivers/network/dwc_eqos/driver_v1"
#define EQOS_DEVICE_MODULE_NAME		"drivers/network/dwc_eqos/device_v1"
#define EQOS_DEVICE_ID_GENERATOR	"dwc_eqos/device_id"


// MAC
#define MAC_CONFIGURATION		0x0000
#define  MAC_CONFIG_RE			(1u << 0)
#define  MAC_CONFIG_TE			(1u << 1)
#define  MAC_CONFIG_DM			(1u << 13)
#define  MAC_CONFIG_FES			(1u << 14)
#define  MAC_CONFIG_PS			(1u << 15)
#define  MAC_CONFIG_JE			(1u << 16)
#define  MAC_CONFIG_JD			(1u << 17)
#define  MAC_CONFIG_WD			(1u << 19)
#define  MAC_CONFIG_ACS			(1u << 20)
#define  MAC_CONFIG_CST			(1u << 21)
#define  MAC_CONFIG_GPSLCE		(1u << 23)
#define MAC_PACKET_FILTER		0x0008
#define  PACKET_FILTER_PR		(1u << 0)
#define  PACKET_FILTER_PM		(1u << 4)
#define MAC_Q0_TX_FLOW_CTRL		0x0070
#define  TX_FLOW_CTRL_TFE		(1u << 1)
#define MAC_RX_FLOW_CTRL		0x0090
#define  RX_FLOW_CTRL_RFE		(1u << 0)
#define MAC_TXQ_PRTY_MAP0		0x0098
#define MAC_RXQ_CTRL0			0x00a0
#define  RXQ0_ENABLED_DCB		2
#define MAC_RXQ_CTRL1			0x00a4
#define  RXQ_CTRL1_MCBCQEN		(1u << 20)
#define MAC_RXQ_CTRL2			0x00a8
#define MAC_INTERRUPT_ENABLE	0x00b4
#define MAC_VERSION				0x0110
#define MAC_HW_FEATURE1			0x0120
#define MAC_MDIO_ADDRESS		0x0200
#define  MDIO_GB				(1u << 0)
#define  MDIO_GOC_READ			(3u << 2)
#define  MDIO_GOC_WRITE			(1u << 2)
#define  MDIO_CR_150_250		(4u << 8)
#define  MDIO_RDA_SHIFT			16
#define  MDIO_PA_SHIFT			21
#define MAC_MDIO_DATA			0x0204
#define MAC_ADDRESS0_HIGH		0x0300
#define MAC_ADDRESS0_LOW		0x0304
#define MMC_CONTROL				0x0700
#define MMC_RX_INTERRUPT_MASK	0x070c
#define MMC_TX_INTERRUPT_MASK	0x0710
#define MMC_IPC_RX_INTERRUPT_MASK 0x0800

// MTL
#define MTL_TXQ0_OPERATION_MODE	0x0d00
#define  TXQ_FTQ				(1u << 0)
#define  TXQ_TSF				(1u << 1)
#define  TXQ_TXQEN_ENABLED		(2u << 2)
#define  TXQ_TQS_SHIFT			16
#define  TXQ_TQS_MASK			(0x1ffu << 16)
#define MTL_TXQ0_DEBUG			0x0d08
#define MTL_TXQ0_QUANTUM_WEIGHT	0x0d18
#define MTL_RXQ0_OPERATION_MODE	0x0d30
#define  RXQ_RSF				(1u << 5)
#define  RXQ_EHFC				(1u << 7)
#define  RXQ_RFA_SHIFT			8
#define  RXQ_RFA_MASK			(0x3fu << 8)
#define  RXQ_RFD_SHIFT			14
#define  RXQ_RFD_MASK			(0x3fu << 14)
#define  RXQ_RQS_SHIFT			20
#define  RXQ_RQS_MASK			(0x3ffu << 20)

// DMA
#define DMA_MODE				0x1000
#define  DMA_MODE_SWR			(1u << 0)
#define DMA_SYSBUS_MODE			0x1004
#define  SYSBUS_BLEN4			(1u << 1)
#define  SYSBUS_BLEN8			(1u << 2)
#define  SYSBUS_BLEN16			(1u << 3)
#define  SYSBUS_EAME			(1u << 11)
#define  SYSBUS_RD_OSR_SHIFT	16
#define DMA_CH0_CONTROL			0x1100
#define  CH_CONTROL_PBLX8		(1u << 16)
#define DMA_CH0_TX_CONTROL		0x1104
#define  TX_CONTROL_ST			(1u << 0)
#define  TX_CONTROL_OSP			(1u << 4)
#define  TX_CONTROL_TXPBL_SHIFT	16
#define DMA_CH0_RX_CONTROL		0x1108
#define  RX_CONTROL_SR			(1u << 0)
#define  RX_CONTROL_RBSZ_SHIFT	1
#define  RX_CONTROL_RBSZ_MASK	(0x3fffu << 1)
#define  RX_CONTROL_RXPBL_SHIFT	16
#define DMA_CH0_TXDESC_LIST_HIGH 0x1110
#define DMA_CH0_TXDESC_LIST_LOW	0x1114
#define DMA_CH0_RXDESC_LIST_HIGH 0x1118
#define DMA_CH0_RXDESC_LIST_LOW	0x111c
#define DMA_CH0_TXDESC_TAIL		0x1120
#define DMA_CH0_RXDESC_TAIL		0x1128
#define DMA_CH0_TXDESC_RING_LENGTH 0x112c
#define DMA_CH0_RXDESC_RING_LENGTH 0x1130
#define DMA_CH0_INTERRUPT_ENABLE 0x1134
#define DMA_CH0_STATUS			0x1160
#define  DMA_INT_TI				(1u << 0)
#define  DMA_INT_RI				(1u << 6)
#define  DMA_INT_RBU			(1u << 7)
#define  DMA_INT_FBE			(1u << 12)
#define  DMA_INT_AI				(1u << 14)
#define  DMA_INT_NI				(1u << 15)

// descriptors
#define DES3_OWN				(1u << 31)
#define DES3_IOC				(1u << 30)	// receive, read format
#define DES3_FD					(1u << 29)
#define DES3_LD					(1u << 28)
#define DES3_BUF1V				(1u << 24)	// receive, read format
#define DES3_ES					(1u << 15)	// receive, write-back format
#define DES3_LENGTH_MASK		0x7fff
#define DES2_IOC				(1u << 31)	// transmit

#define DESCRIPTOR_COUNT		256
#define BUFFER_SIZE				2048
#define MAX_FRAME_SIZE			1536
#define CACHE_LINE_SIZE			64

// Allwinner A733 glue: clock controller and the GMAC's own configuration
// register (the second "reg" of the node)
#define A733_CCU_BASE			0x02002000
#define A733_CCU_SIZE			0x2000
#define A733_CCU_MBUS_GATE		0x05e4
#define  MBUS_GATE_GMAC0		(1u << 11)
#define A733_CCU_GMAC0_PHY		0x1410
#define  GMAC0_PHY_CLOCK_ENABLE	(1u << 31)
#define A733_CCU_GMAC0_GATE		0x141c
#define  GMAC0_GATE_BUS			(1u << 0)
#define  GMAC0_RESET			((1u << 16) | (1u << 17))
#define GMAC210_CFG				0x0000
#define  CFG_ETCS_INT_GMII		2
#define  CFG_EPIT				(1u << 2)
#define  CFG_ERXDC_SHIFT		5
#define  CFG_ETXDC_L_SHIFT		10
#define  CFG_ETXDC_H_SHIFT		16

// the main GPIO controller: bank n at 0x80 + 0x80 * n
#define A733_PIO_BASE			0x02000000
#define A733_PIO_SIZE			0x1000
#define PIO_BANK(n)				(0x80 + 0x80 * (n))
#define PIO_CFG(pin)			(((pin) / 8) * 4)
#define PIO_DATA				0x10
#define PIO_BANK_H				7

// the SoC ID, for a MAC address of the board's own
#define A733_SID_BASE			0x03006000
#define A733_SID_SIZE			0x1000
#define SID_CHIP_ID				0x200

// PHY registers
#define MII_BMCR				0x00
#define  BMCR_RESET				0x8000
#define  BMCR_ANENABLE			0x1000
#define  BMCR_ANRESTART			0x0200
#define MII_BMSR				0x01
#define  BMSR_LSTATUS			0x0004
#define MII_PHYSID1				0x02
#define MII_PHYSID2				0x03
#define MII_ADVERTISE			0x04
#define MII_LPA					0x05
#define MII_CTRL1000			0x09
#define MII_STAT1000			0x0a


struct eqos_descriptor {
	uint32	des0;
	uint32	des1;
	uint32	des2;
	uint32	des3;
};


struct eqos_info {
	device_node*	node;
	uint64			registersBase;
	uint64			registersSize;
	uint64			configBase;		// Allwinner glue, 0 if none
	uint32			interrupt;
	uint8			macAddress[6];
	uint32			phyAddress;
	uint32			txDelay;		// units of 100 ps
	uint32			rxDelay;

	area_id			registerArea;
	volatile uint8*	registers;
	area_id			configArea;
	volatile uint8*	config;
	bool			interruptInstalled;

	area_id			descriptorArea;
	eqos_descriptor* rxRing;
	eqos_descriptor* txRing;
	phys_addr_t		rxRingAddress;
	phys_addr_t		txRingAddress;

	area_id			bufferArea;
	uint8*			buffers;
		// DESCRIPTOR_COUNT receive buffers, then as many for transmission
	phys_addr_t		buffersAddress;

	mutex			rxLock;
	mutex			txLock;
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
};


static device_manager_info* sDeviceManager;
static net_buffer_module_info* sBufferModule;


static inline uint32
read32(eqos_info* info, uint32 reg)
{
	return *(volatile uint32*)(info->registers + reg);
}


static inline void
write32(eqos_info* info, uint32 reg, uint32 value)
{
	*(volatile uint32*)(info->registers + reg) = value;
}


static inline void
set32(eqos_info* info, uint32 reg, uint32 bits)
{
	write32(info, reg, read32(info, reg) | bits);
}


static inline void
clear32(eqos_info* info, uint32 reg, uint32 bits)
{
	write32(info, reg, read32(info, reg) & ~bits);
}


static void
dma_buffer_for_cpu(const void* buffer, size_t size)
{
	addr_t end = ((addr_t)buffer + size + CACHE_LINE_SIZE - 1)
		& ~(addr_t)(CACHE_LINE_SIZE - 1);
	for (addr_t line = (addr_t)buffer & ~(addr_t)(CACHE_LINE_SIZE - 1);
			line < end; line += CACHE_LINE_SIZE) {
		asm volatile("dc ivac, %0" : : "r" (line) : "memory");
	}
	asm volatile("dsb sy" : : : "memory");
}


static void
dma_buffer_for_device(const void* buffer, size_t size)
{
	addr_t end = ((addr_t)buffer + size + CACHE_LINE_SIZE - 1)
		& ~(addr_t)(CACHE_LINE_SIZE - 1);
	for (addr_t line = (addr_t)buffer & ~(addr_t)(CACHE_LINE_SIZE - 1);
			line < end; line += CACHE_LINE_SIZE) {
		asm volatile("dc cvac, %0" : : "r" (line) : "memory");
	}
	asm volatile("dsb sy" : : : "memory");
}


/*!	Maps \a size bytes of physical memory at \a base for a moment, and
	calls \a function with the mapping.
*/
template<typename Function>
static status_t
with_registers(const char* name, phys_addr_t base, size_t size,
	Function function)
{
	volatile uint8* registers;
	area_id area = map_physical_memory(name, base, size,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&registers);
	if (area < 0)
		return area;
	function(registers);
	delete_area(area);
	return B_OK;
}


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


//	#pragma mark - Allwinner glue


/*!	Clocks on and resets released for the A733's GMAC0, the RGMII delays
	and interface mode set, and the PHY taken out of reset (PH16, active
	low, on the Cubie A7S).
*/
static void
sunxi_gmac_power_up(eqos_info* info)
{
	with_registers("dwc_eqos clocks", A733_CCU_BASE, A733_CCU_SIZE,
		[](volatile uint8* ccu) {
			mmio_write(ccu, A733_CCU_MBUS_GATE,
				mmio_read(ccu, A733_CCU_MBUS_GATE) | MBUS_GATE_GMAC0);
			mmio_write(ccu, A733_CCU_GMAC0_PHY,
				mmio_read(ccu, A733_CCU_GMAC0_PHY) | GMAC0_PHY_CLOCK_ENABLE);
			mmio_write(ccu, A733_CCU_GMAC0_GATE,
				mmio_read(ccu, A733_CCU_GMAC0_GATE) | GMAC0_GATE_BUS
					| GMAC0_RESET);
			memory_full_barrier();
		});
	spin(10);

	if (info->config != NULL) {
		uint32 config = CFG_EPIT | CFG_ETCS_INT_GMII
			| (info->rxDelay & 0x1f) << CFG_ERXDC_SHIFT
			| (info->txDelay & 0x7) << CFG_ETXDC_L_SHIFT
			| ((info->txDelay >> 3) & 0x3) << CFG_ETXDC_H_SHIFT;
		mmio_write(info->config, GMAC210_CFG, config);
	}

	// PHY reset: output, low for 10 ms, high, then 150 ms to come up
	with_registers("dwc_eqos gpio", A733_PIO_BASE, A733_PIO_SIZE,
		[](volatile uint8* pio) {
			const uint32 pin = 16;
			uint32 bank = PIO_BANK(PIO_BANK_H);
			uint32 cfg = mmio_read(pio, bank + PIO_CFG(pin));
			uint32 shift = (pin % 8) * 4;
			mmio_write(pio, bank + PIO_DATA,
				mmio_read(pio, bank + PIO_DATA) & ~(1u << pin));
			mmio_write(pio, bank + PIO_CFG(pin),
				(cfg & ~(0xfu << shift)) | (1u << shift));
			memory_full_barrier();
			snooze(10000);
			mmio_write(pio, bank + PIO_DATA,
				mmio_read(pio, bank + PIO_DATA) | (1u << pin));
			memory_full_barrier();
		});
	snooze(150000);
}


/*!	The MAC address U-Boot gives the board when its EEPROM has none,
	derived from the SoC's ID (dwc_eth_qos_sunxi.c).
*/
static bool
sunxi_mac_address(uint8* address)
{
	uint32 chipId[4] = {};
	if (with_registers("dwc_eqos sid", A733_SID_BASE, A733_SID_SIZE,
			[&](volatile uint8* sid) {
				for (int i = 0; i < 4; i++)
					chipId[i] = mmio_read(sid, SID_CHIP_ID + 4 * i);
			}) != B_OK) {
		return false;
	}
	if (chipId[2] == 0 && chipId[3] == 0)
		return false;

	uint32 seed = chipId[3];
	address[0] = ((chipId[2] >> 8) & 0xff & ~0x01) | 0x02;
	address[1] = chipId[2] & 0xff;
	address[2] = (seed >> 24) & 0xff;
	address[3] = (seed >> 16) & 0xff;
	address[4] = (seed >> 8) & 0xff;
	address[5] = seed & 0xff;
	return true;
}


//	#pragma mark - PHY


static status_t
mdio_wait(eqos_info* info)
{
	bigtime_t timeout = system_time() + 20000;
	while ((read32(info, MAC_MDIO_ADDRESS) & MDIO_GB) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		spin(10);
	}
	return B_OK;
}


static int32
mdio_read(eqos_info* info, uint32 reg)
{
	if (mdio_wait(info) != B_OK)
		return B_TIMED_OUT;
	write32(info, MAC_MDIO_ADDRESS, info->phyAddress << MDIO_PA_SHIFT
		| reg << MDIO_RDA_SHIFT | MDIO_CR_150_250 | MDIO_GOC_READ | MDIO_GB);
	if (mdio_wait(info) != B_OK)
		return B_TIMED_OUT;
	return read32(info, MAC_MDIO_DATA) & 0xffff;
}


static status_t
mdio_write(eqos_info* info, uint32 reg, uint16 value)
{
	if (mdio_wait(info) != B_OK)
		return B_TIMED_OUT;
	write32(info, MAC_MDIO_DATA, value);
	write32(info, MAC_MDIO_ADDRESS, info->phyAddress << MDIO_PA_SHIFT
		| reg << MDIO_RDA_SHIFT | MDIO_CR_150_250 | MDIO_GOC_WRITE | MDIO_GB);
	return mdio_wait(info);
}


static status_t
phy_init(eqos_info* info)
{
	int32 id1 = mdio_read(info, MII_PHYSID1);
	int32 id2 = mdio_read(info, MII_PHYSID2);
	if (id1 < 0 || id2 < 0 || id1 == 0xffff || (id1 == 0 && id2 == 0)) {
		ERROR("no PHY at MDIO address %" B_PRIu32 " (%" B_PRId32 ", %"
			B_PRId32 ")\n", info->phyAddress, id1, id2);
		return B_DEVICE_NOT_FOUND;
	}
	INFO("PHY %04" B_PRIx32 ":%04" B_PRIx32 " at MDIO address %" B_PRIu32 "\n",
		id1, id2, info->phyAddress);

	mdio_write(info, MII_BMCR, BMCR_RESET);
	bigtime_t timeout = system_time() + 500000;
	while ((mdio_read(info, MII_BMCR) & BMCR_RESET) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(1000);
	}

	// The RGMII clock delays are the MAC's (the configuration register);
	// the PHY's straps leave its own off on this board.
	// advertise everything up to 1000BASE-T full duplex, with pause
	mdio_write(info, MII_ADVERTISE, 0x0001 | 0x01e0 | 0x0400);
	mdio_write(info, MII_CTRL1000, 0x0200);
	mdio_write(info, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
	return B_OK;
}


static void
update_link(eqos_info* info)
{
	// the link bit latches a failure: read twice
	mdio_read(info, MII_BMSR);
	int32 status = mdio_read(info, MII_BMSR);
	bool up = status >= 0 && (status & BMSR_LSTATUS) != 0;

	uint32 speed = 0;
	bool fullDuplex = false;
	if (up) {
		int32 status1000 = mdio_read(info, MII_STAT1000);
		int32 partner = mdio_read(info, MII_LPA);
		int32 own = mdio_read(info, MII_ADVERTISE);
		if (status1000 < 0 || partner < 0 || own < 0)
			return;

		uint32 common = partner & own;
		if ((status1000 & 0x0800) != 0) {
			speed = 1000;
			fullDuplex = true;
		} else if ((common & 0x0100) != 0) {
			speed = 100;
			fullDuplex = true;
		} else if ((common & 0x0080) != 0) {
			speed = 100;
		} else if ((common & 0x0040) != 0) {
			speed = 10;
			fullDuplex = true;
		} else
			speed = 10;
	}

	if (up == info->linkUp && speed == info->linkSpeed
		&& fullDuplex == info->fullDuplex) {
		return;
	}

	if (up) {
		// tell the MAC what the PHY negotiated
		uint32 config = read32(info, MAC_CONFIGURATION);
		config &= ~(MAC_CONFIG_PS | MAC_CONFIG_FES | MAC_CONFIG_DM);
		if (speed == 100)
			config |= MAC_CONFIG_PS | MAC_CONFIG_FES;
		else if (speed == 10)
			config |= MAC_CONFIG_PS;
		if (fullDuplex)
			config |= MAC_CONFIG_DM;
		write32(info, MAC_CONFIGURATION, config);
	}

	info->linkUp = up;
	info->linkSpeed = speed;
	info->fullDuplex = fullDuplex;

	if (up) {
		INFO("link up, %" B_PRIu32 " Mbit/s %s duplex\n", speed,
			fullDuplex ? "full" : "half");
	} else
		INFO("link down\n");

	if (info->linkChangeSemaphore >= 0)
		release_sem_etc(info->linkChangeSemaphore, 1, B_DO_NOT_RESCHEDULE);
}


static status_t
link_thread(void* data)
{
	eqos_info* info = (eqos_info*)data;
	while (!info->stopping) {
		update_link(info);
		snooze(1000000);
	}
	return B_OK;
}


//	#pragma mark - controller


static int32
eqos_interrupt(void* data)
{
	eqos_info* info = (eqos_info*)data;

	uint32 status = read32(info, DMA_CH0_STATUS);
	uint32 enabled = read32(info, DMA_CH0_INTERRUPT_ENABLE);
	if ((status & enabled & (DMA_INT_RI | DMA_INT_TI | DMA_INT_FBE)) == 0) {
		if (status != 0)
			write32(info, DMA_CH0_STATUS, status);
		return B_UNHANDLED_INTERRUPT;
	}

	write32(info, DMA_CH0_STATUS, status);

	if ((status & DMA_INT_FBE) != 0)
		ERROR("fatal bus error, DMA status %#" B_PRIx32 "\n", status);

	// The reader drains RX with the interrupt off. TX completion
	// interrupts are only needed when a writer runs out of descriptors.
	uint32 handled = status & enabled & (DMA_INT_RI | DMA_INT_TI);
	write32(info, DMA_CH0_INTERRUPT_ENABLE, enabled & ~handled);
	if ((handled & DMA_INT_RI) != 0)
		release_sem_etc(info->rxSemaphore, 1, B_DO_NOT_RESCHEDULE);
	if ((handled & DMA_INT_TI) != 0)
		release_sem_etc(info->txSemaphore, 1, B_DO_NOT_RESCHEDULE);

	return B_INVOKE_SCHEDULER;
}


static inline phys_addr_t
rx_descriptor_address(eqos_info* info, uint32 index)
{
	return info->rxRingAddress + index * sizeof(eqos_descriptor);
}


static inline phys_addr_t
tx_descriptor_address(eqos_info* info, uint32 index)
{
	return info->txRingAddress + index * sizeof(eqos_descriptor);
}


static void
give_rx_descriptor(eqos_info* info, uint32 index)
{
	phys_addr_t address = info->buffersAddress + (uint64)index * BUFFER_SIZE;
	eqos_descriptor& descriptor = info->rxRing[index];
	descriptor.des0 = (uint32)address;
	descriptor.des1 = (uint32)(address >> 32);
	descriptor.des2 = 0;
	memory_write_barrier();
	descriptor.des3 = DES3_OWN | DES3_IOC | DES3_BUF1V;
}


static void
eqos_stop(eqos_info* info)
{
	write32(info, DMA_CH0_INTERRUPT_ENABLE, 0);

	clear32(info, DMA_CH0_TX_CONTROL, TX_CONTROL_ST);
	for (int i = 0; i < 10000; i++) {
		uint32 debug = read32(info, MTL_TXQ0_DEBUG);
		if (((debug >> 1) & 3) != 1 && (debug & (1 << 4)) == 0)
			break;
		spin(10);
	}
	clear32(info, MAC_CONFIGURATION, MAC_CONFIG_TE | MAC_CONFIG_RE);
	clear32(info, DMA_CH0_RX_CONTROL, RX_CONTROL_SR);
	write32(info, DMA_CH0_STATUS, 0xffffffff);
}


static status_t
eqos_start(eqos_info* info)
{
	// software reset of the whole controller
	set32(info, DMA_MODE, DMA_MODE_SWR);
	bigtime_t timeout = system_time() + 500000;
	while ((read32(info, DMA_MODE) & DMA_MODE_SWR) != 0) {
		if (system_time() > timeout) {
			ERROR("the software reset does not finish (no RX clock from "
				"the PHY?)\n");
			return B_TIMED_OUT;
		}
		snooze(100);
	}

	// no MMC counter interrupts
	write32(info, MMC_RX_INTERRUPT_MASK, 0xffffffff);
	write32(info, MMC_TX_INTERRUPT_MASK, 0xffffffff);
	write32(info, MMC_IPC_RX_INTERRUPT_MASK, 0xffffffff);
	write32(info, MAC_INTERRUPT_ENABLE, 0);

	// MTL: store and forward, all of the FIFOs for the one queue
	uint32 feature1 = read32(info, MAC_HW_FEATURE1);
	uint32 txFifo = 128u << ((feature1 >> 6) & 0x1f);
	uint32 rxFifo = 128u << (feature1 & 0x1f);
	uint32 tqs = txFifo / 256 - 1;
	uint32 rqs = rxFifo / 256 - 1;

	write32(info, MTL_TXQ0_OPERATION_MODE, TXQ_TSF | TXQ_TXQEN_ENABLED
		| (tqs << TXQ_TQS_SHIFT));
	write32(info, MTL_TXQ0_QUANTUM_WEIGHT, 0x10);

	uint32 rxMode = RXQ_RSF | (rqs << RXQ_RQS_SHIFT);
	if (rqs >= 4096 / 256 - 1) {
		// flow control: pause at "full minus 4 KiB" etc. (U-Boot's values)
		uint32 rfd = 0x6, rfa;
		if (rqs == 4096 / 256 - 1) {
			rfd = 0x3;
			rfa = 0x1;
		} else if (rqs == 8192 / 256 - 1)
			rfa = 0xa;
		else if (rqs == 16384 / 256 - 1)
			rfa = 0x12;
		else
			rfa = 0x1e;
		rxMode |= RXQ_EHFC | rfd << RXQ_RFD_SHIFT | rfa << RXQ_RFA_SHIFT;
	}
	write32(info, MTL_RXQ0_OPERATION_MODE, rxMode);

	// MAC
	write32(info, MAC_RXQ_CTRL0, RXQ0_ENABLED_DCB);
	set32(info, MAC_RXQ_CTRL1, RXQ_CTRL1_MCBCQEN);
	write32(info, MAC_PACKET_FILTER,
		info->promiscuous ? PACKET_FILTER_PR : PACKET_FILTER_PM);
	write32(info, MAC_Q0_TX_FLOW_CTRL, 0xffffu << 16 | TX_FLOW_CTRL_TFE);
	write32(info, MAC_TXQ_PRTY_MAP0, 0);
	write32(info, MAC_RXQ_CTRL2, 0);
	write32(info, MAC_RX_FLOW_CTRL, RX_FLOW_CTRL_RFE);

	uint32 config = read32(info, MAC_CONFIGURATION);
	config &= ~(MAC_CONFIG_GPSLCE | MAC_CONFIG_WD | MAC_CONFIG_JD
		| MAC_CONFIG_JE | MAC_CONFIG_TE | MAC_CONFIG_RE);
	config |= MAC_CONFIG_CST | MAC_CONFIG_ACS | MAC_CONFIG_DM;
	write32(info, MAC_CONFIGURATION, config);

	const uint8* mac = info->macAddress;
	write32(info, MAC_ADDRESS0_HIGH, mac[5] << 8 | mac[4]);
	write32(info, MAC_ADDRESS0_LOW,
		mac[3] << 24 | mac[2] << 16 | mac[1] << 8 | mac[0]);

	// DMA
	write32(info, DMA_SYSBUS_MODE, 2 << SYSBUS_RD_OSR_SHIFT | SYSBUS_EAME
		| SYSBUS_BLEN16 | SYSBUS_BLEN8 | SYSBUS_BLEN4);
	write32(info, DMA_CH0_CONTROL, CH_CONTROL_PBLX8);

	uint32 pbl = tqs + 1 > 32 ? 32 : tqs + 1;
	write32(info, DMA_CH0_TX_CONTROL, TX_CONTROL_OSP
		| pbl << TX_CONTROL_TXPBL_SHIFT);
	write32(info, DMA_CH0_RX_CONTROL, 8 << RX_CONTROL_RXPBL_SHIFT
		| (BUFFER_SIZE << RX_CONTROL_RBSZ_SHIFT & RX_CONTROL_RBSZ_MASK));

	// the rings
	memset(info->txRing, 0, DESCRIPTOR_COUNT * sizeof(eqos_descriptor));
	for (uint32 i = 0; i < DESCRIPTOR_COUNT; i++)
		give_rx_descriptor(info, i);
	for (uint32 i = 0; i < DESCRIPTOR_COUNT; i++) {
		dma_buffer_for_cpu(info->buffers + (uint64)i * BUFFER_SIZE,
			BUFFER_SIZE);
	}
	memory_full_barrier();
	info->rxIndex = 0;
	info->txIndex = 0;
	info->txCleaned = 0;

	write32(info, DMA_CH0_TXDESC_LIST_HIGH, (uint32)(info->txRingAddress >> 32));
	write32(info, DMA_CH0_TXDESC_LIST_LOW, (uint32)info->txRingAddress);
	write32(info, DMA_CH0_TXDESC_RING_LENGTH, DESCRIPTOR_COUNT - 1);
	write32(info, DMA_CH0_RXDESC_LIST_HIGH, (uint32)(info->rxRingAddress >> 32));
	write32(info, DMA_CH0_RXDESC_LIST_LOW, (uint32)info->rxRingAddress);
	write32(info, DMA_CH0_RXDESC_RING_LENGTH, DESCRIPTOR_COUNT - 1);

	// nothing to send; everything to receive into (the tail points past
	// the last descriptor the controller may use)
	write32(info, DMA_CH0_TXDESC_TAIL, (uint32)info->txRingAddress);
	write32(info, DMA_CH0_RXDESC_TAIL,
		(uint32)rx_descriptor_address(info, DESCRIPTOR_COUNT - 1));

	write32(info, DMA_CH0_STATUS, 0xffffffff);
	write32(info, DMA_CH0_INTERRUPT_ENABLE, DMA_INT_NI | DMA_INT_AI
		| DMA_INT_FBE | DMA_INT_RI);

	set32(info, DMA_CH0_TX_CONTROL, TX_CONTROL_ST);
	set32(info, DMA_CH0_RX_CONTROL, RX_CONTROL_SR);

	info->linkUp = false;
	info->linkSpeed = 0;
	phy_init(info);

	set32(info, MAC_CONFIGURATION, MAC_CONFIG_TE | MAC_CONFIG_RE);
	return B_OK;
}


static status_t
eqos_receive(eqos_info* info, net_buffer** _buffer)
{
	while (true) {
		MutexLocker locker(info->rxLock);

		uint32 index = info->rxIndex;
		eqos_descriptor& descriptor = info->rxRing[index];
		uint32 status = descriptor.des3;
		if ((status & DES3_OWN) == 0) {
			memory_read_barrier();
			uint32 length = status & DES3_LENGTH_MASK;
			uint8* data = info->buffers + (uint64)index * BUFFER_SIZE;

			net_buffer* buffer = NULL;
			bool good = (status & (DES3_FD | DES3_LD)) == (DES3_FD | DES3_LD)
				&& (status & DES3_ES) == 0
				&& length > ETHER_HEADER_LENGTH && length <= BUFFER_SIZE;
			if (good) {
				dma_buffer_for_cpu(data, length);
				buffer = sBufferModule->create(0);
				if (buffer != NULL
					&& sBufferModule->append(buffer, data, length) != B_OK) {
					sBufferModule->free(buffer);
					buffer = NULL;
				}
			} else {
				TRACE("dropped frame, status %#" B_PRIx32 "\n", status);
			}

			// back to the controller
			give_rx_descriptor(info, index);
			info->rxIndex = (index + 1) % DESCRIPTOR_COUNT;
			memory_full_barrier();
			write32(info, DMA_CH0_RXDESC_TAIL,
				(uint32)rx_descriptor_address(info, index));

			if (buffer != NULL) {
				*_buffer = buffer;
				return B_OK;
			}
			continue;
		}

		if (info->nonblocking)
			return B_WOULD_BLOCK;

		// Arm, then recheck: a frame can arrive after the empty-ring test
		// while the interrupt is still off. Never sleep with it pending.
		write32(info, DMA_CH0_STATUS, DMA_INT_RI);
		set32(info, DMA_CH0_INTERRUPT_ENABLE, DMA_INT_RI);
		if ((info->rxRing[info->rxIndex].des3 & DES3_OWN) == 0) {
			clear32(info, DMA_CH0_INTERRUPT_ENABLE, DMA_INT_RI);
			continue;
		}
		locker.Unlock();

		status_t result = acquire_sem_etc(info->rxSemaphore, 1,
			B_CAN_INTERRUPT, 0);
		if (result != B_OK)
			return result;
	}
}


/*!	Forgets the descriptors the controller has sent. */
static void
clean_tx(eqos_info* info)
{
	while (info->txCleaned != info->txIndex
		&& (info->txRing[info->txCleaned].des3 & DES3_OWN) == 0) {
		info->txCleaned = (info->txCleaned + 1) % DESCRIPTOR_COUNT;
	}
}


static status_t
eqos_send(eqos_info* info, net_buffer* buffer)
{
	size_t length = buffer->size;
	if (length > MAX_FRAME_SIZE)
		return B_BAD_DATA;

	MutexLocker locker(info->txLock);
	while (true) {
		clean_tx(info);
		uint32 pending = (info->txIndex + DESCRIPTOR_COUNT - info->txCleaned)
			% DESCRIPTOR_COUNT;
		if (pending < DESCRIPTOR_COUNT - 1)
			break;

		if (info->nonblocking)
			return B_WOULD_BLOCK;

		write32(info, DMA_CH0_STATUS, DMA_INT_TI);
		set32(info, DMA_CH0_INTERRUPT_ENABLE, DMA_INT_TI);
		clean_tx(info);
		if ((info->txIndex + DESCRIPTOR_COUNT - info->txCleaned)
				% DESCRIPTOR_COUNT < DESCRIPTOR_COUNT - 1) {
			continue;
		}
		locker.Unlock();

		status_t status = acquire_sem_etc(info->txSemaphore, 1,
			B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 100000);
		if (status != B_OK && status != B_TIMED_OUT)
			return status;
		locker.Lock();
	}

	uint32 index = info->txIndex;
	uint8* data = info->buffers
		+ (uint64)(DESCRIPTOR_COUNT + index) * BUFFER_SIZE;
	if (sBufferModule->read(buffer, 0, data, length) != B_OK)
		return B_BAD_DATA;
	if (length < 60) {
		memset(data + length, 0, 60 - length);
		length = 60;
	}
	dma_buffer_for_device(data, length);

	phys_addr_t address = info->buffersAddress
		+ (uint64)(DESCRIPTOR_COUNT + index) * BUFFER_SIZE;
	eqos_descriptor& descriptor = info->txRing[index];
	descriptor.des0 = (uint32)address;
	descriptor.des1 = (uint32)(address >> 32);
	descriptor.des2 = length;
	memory_write_barrier();
	descriptor.des3 = DES3_OWN | DES3_FD | DES3_LD | length;

	info->txIndex = (index + 1) % DESCRIPTOR_COUNT;
	memory_full_barrier();
	write32(info, DMA_CH0_TXDESC_TAIL,
		(uint32)tx_descriptor_address(info, info->txIndex));
	locker.Unlock();

	sBufferModule->free(buffer);
	return B_OK;
}


//	#pragma mark - device


static status_t
eqos_init_device(void* _info, void** _cookie)
{
	eqos_info* info = (eqos_info*)_info;

	info->registerArea = map_physical_memory("dwc_eqos registers",
		info->registersBase, info->registersSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&info->registers);
	if (info->registerArea < 0)
		return info->registerArea;

	info->configArea = -1;
	if (info->configBase != 0) {
		info->configArea = map_physical_memory("dwc_eqos glue",
			info->configBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&info->config);
		if (info->configArea < 0) {
			delete_area(info->registerArea);
			return info->configArea;
		}
		sunxi_gmac_power_up(info);
	}

	uint32 version = read32(info, MAC_VERSION);
	if ((version & 0xff) < 0x40) {
		ERROR("not an Ethernet QoS core (version %#" B_PRIx32 ")\n", version);
		if (info->configArea >= 0)
			delete_area(info->configArea);
		delete_area(info->registerArea);
		return B_NOT_SUPPORTED;
	}

	// the descriptors, uncached
	size_t ringSize = DESCRIPTOR_COUNT * sizeof(eqos_descriptor);
	void* rings;
	info->descriptorArea = create_area("dwc_eqos descriptors", &rings,
		B_ANY_KERNEL_ADDRESS, 2 * ringSize, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (info->descriptorArea < 0) {
		if (info->configArea >= 0)
			delete_area(info->configArea);
		delete_area(info->registerArea);
		return info->descriptorArea;
	}
	physical_entry entry;
	get_memory_map(rings, 2 * ringSize, &entry, 1);
	memset(rings, 0, 2 * ringSize);
	for (addr_t line = (addr_t)rings; line < (addr_t)rings + 2 * ringSize;
			line += CACHE_LINE_SIZE) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	memory_full_barrier();
	vm_set_area_memory_type(info->descriptorArea, entry.address,
		B_WRITE_COMBINING_MEMORY);
	info->rxRing = (eqos_descriptor*)rings;
	info->txRing = (eqos_descriptor*)((uint8*)rings + ringSize);
	info->rxRingAddress = entry.address;
	info->txRingAddress = entry.address + ringSize;

	// the packets, cacheable; slots are cache line aligned
	size_t size = 2 * DESCRIPTOR_COUNT * BUFFER_SIZE;
	info->bufferArea = create_area("dwc_eqos buffers", (void**)&info->buffers,
		B_ANY_KERNEL_ADDRESS, size, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (info->bufferArea < 0) {
		delete_area(info->descriptorArea);
		if (info->configArea >= 0)
			delete_area(info->configArea);
		delete_area(info->registerArea);
		return info->bufferArea;
	}
	get_memory_map(info->buffers, size, &entry, 1);
	info->buffersAddress = entry.address;
	memset(info->buffers, 0, size);
	for (addr_t line = (addr_t)info->buffers;
			line < (addr_t)info->buffers + size; line += CACHE_LINE_SIZE) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	memory_full_barrier();

	mutex_init(&info->rxLock, "dwc_eqos rx");
	mutex_init(&info->txLock, "dwc_eqos tx");
	info->rxSemaphore = -1;
	info->txSemaphore = -1;
	info->linkChangeSemaphore = -1;
	info->linkThread = -1;

	INFO("Ethernet QoS %#" B_PRIx32 " at %#" B_PRIx64 ", interrupt %"
		B_PRIu32 ", %02x:%02x:%02x:%02x:%02x:%02x, delays tx %" B_PRIu32
		" rx %" B_PRIu32 "\n", version, info->registersBase, info->interrupt,
		info->macAddress[0], info->macAddress[1], info->macAddress[2],
		info->macAddress[3], info->macAddress[4], info->macAddress[5],
		info->txDelay, info->rxDelay);

	*_cookie = info;
	return B_OK;
}


static void
eqos_uninit_device(void* cookie)
{
	eqos_info* info = (eqos_info*)cookie;

	eqos_stop(info);
	mutex_destroy(&info->rxLock);
	mutex_destroy(&info->txLock);
	delete_area(info->bufferArea);
	delete_area(info->descriptorArea);
	if (info->configArea >= 0)
		delete_area(info->configArea);
	delete_area(info->registerArea);
}


static status_t
eqos_open(void* _info, const char* path, int openMode, void** _cookie)
{
	eqos_info* info = (eqos_info*)_info;

	if (atomic_add(&info->openCount, 1) != 0) {
		atomic_add(&info->openCount, -1);
		return B_BUSY;
	}

	info->nonblocking = (openMode & O_NONBLOCK) != 0;
	info->stopping = false;
	info->rxSemaphore = create_sem(0, "dwc_eqos rx");
	info->txSemaphore = create_sem(0, "dwc_eqos tx");

	status_t status = install_io_interrupt_handler(info->interrupt,
		eqos_interrupt, info, 0);
	if (status != B_OK) {
		delete_sem(info->rxSemaphore);
		delete_sem(info->txSemaphore);
		atomic_add(&info->openCount, -1);
		return status;
	}
	info->interruptInstalled = true;

	status = eqos_start(info);
	if (status != B_OK) {
		remove_io_interrupt_handler(info->interrupt, eqos_interrupt, info);
		info->interruptInstalled = false;
		delete_sem(info->rxSemaphore);
		delete_sem(info->txSemaphore);
		atomic_add(&info->openCount, -1);
		return status;
	}

	info->linkThread = spawn_kernel_thread(link_thread, "dwc_eqos link",
		B_NORMAL_PRIORITY, info);
	resume_thread(info->linkThread);

	*_cookie = info;
	return B_OK;
}


static status_t
eqos_close(void* cookie)
{
	eqos_info* info = (eqos_info*)cookie;

	info->stopping = true;
	status_t result;
	wait_for_thread(info->linkThread, &result);
	info->linkThread = -1;

	eqos_stop(info);

	if (info->interruptInstalled) {
		remove_io_interrupt_handler(info->interrupt, eqos_interrupt, info);
		info->interruptInstalled = false;
	}

	delete_sem(info->rxSemaphore);
	delete_sem(info->txSemaphore);
	info->rxSemaphore = info->txSemaphore = -1;
	return B_OK;
}


static status_t
eqos_free(void* cookie)
{
	eqos_info* info = (eqos_info*)cookie;
	atomic_add(&info->openCount, -1);
	return B_OK;
}


static status_t
eqos_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	eqos_info* info = (eqos_info*)cookie;

	switch (op) {
		case ETHER_GETADDR:
			return user_memcpy(buffer, info->macAddress,
				sizeof(info->macAddress));

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
			info->promiscuous = value != 0;
			write32(info, MAC_PACKET_FILTER,
				info->promiscuous ? PACKET_FILTER_PR : PACKET_FILTER_PM);
			return B_OK;
		}

		case ETHER_NONBLOCK:
		{
			int32 value;
			if (user_memcpy(&value, buffer, sizeof(value)) != B_OK)
				return B_BAD_ADDRESS;
			info->nonblocking = value != 0;
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
			info->linkChangeSemaphore = semaphore;
			return B_OK;
		}

		case ETHER_GET_LINK_STATE:
		{
			ether_link_state_t state;
			state.media = IFM_ETHER;
			if (info->linkUp) {
				state.media |= IFM_ACTIVE
					| (info->fullDuplex ? IFM_FULL_DUPLEX : IFM_HALF_DUPLEX)
					| (info->linkSpeed == 1000 ? IFM_1000_T
						: info->linkSpeed == 100 ? IFM_100_TX : IFM_10_T);
			}
			state.speed = (uint64)info->linkSpeed * 1000000;
			state.quality = 1000;
			return user_memcpy(buffer, &state, sizeof(state));
		}

		case ETHER_SEND_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return eqos_send(info, (net_buffer*)buffer);

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return eqos_receive(info, (net_buffer**)buffer);
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver


static const char* kCompatible[] = {
	"allwinner,sun60i-a733-gmac210",
	"allwinner,sunxi-gmac-210",
};


static float
eqos_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK) {
		return 0.0f;
	}

	for (size_t i = 0; i < B_COUNT_OF(kCompatible); i++) {
		if (strcmp(compatible, kCompatible[i]) == 0)
			return 1.0f;
	}
	return 0.0f;
}


static status_t
eqos_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "Synopsys Ethernet QoS"}},
		{}
	};

	return sDeviceManager->register_node(parent, EQOS_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


static uint32
get_cell(fdt_device_module_info* fdt, fdt_device* device, const char* name,
	uint32 defaultValue)
{
	int length;
	const uint32* property = (const uint32*)fdt->get_prop(device, name,
		&length);
	if (property == NULL || length != 4)
		return defaultValue;
	return B_BENDIAN_TO_HOST_INT32(*property);
}


static status_t
eqos_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	sDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	eqos_info* info = (eqos_info*)calloc(1, sizeof(eqos_info));
	if (info == NULL)
		return B_NO_MEMORY;
	info->node = node;

	uint64 interrupt;
	if (!fdt->get_reg(device, 0, &info->registersBase, &info->registersSize)
		|| !fdt->get_interrupt(device, 0, NULL, &interrupt)) {
		free(info);
		return B_BAD_DATA;
	}
	info->interrupt = interrupt;

	uint64 configSize;
	if (!fdt->get_reg(device, 1, &info->configBase, &configSize))
		info->configBase = 0;

	// RGMII delays in 100 ps steps: U-Boot's and Linux' properties, then
	// the BSP's, then the Cubie A7S's values
	uint32 delay = get_cell(fdt, device, "tx-internal-delay-ps", 0);
	info->txDelay = delay != 0 ? delay / 100
		: get_cell(fdt, device, "tx-delay", 12);
	delay = get_cell(fdt, device, "rx-internal-delay-ps", 0);
	info->rxDelay = delay != 0 ? delay / 100
		: get_cell(fdt, device, "rx-delay", 10);

	int length;
	const void* property = fdt->get_prop(device, "local-mac-address", &length);
	if (property == NULL || length != 6)
		property = fdt->get_prop(device, "mac-address", &length);
	if (property != NULL && length == 6
		&& memcmp(property, "\0\0\0\0\0\0", 6) != 0) {
		memcpy(info->macAddress, property, 6);
	} else if (!sunxi_mac_address(info->macAddress)) {
		// locally administered, so that the board at least gets on the net
		const uint8 fallback[6] = {0x02, 0xa7, 0x33, 0x00, 0x00, 0x01};
		memcpy(info->macAddress, fallback, 6);
	}

	// The Cubie A7S has its PHY at address 1 (the "reg" of the node that
	// "phy-handle" names).
	info->phyAddress = 1;

	*_cookie = info;
	return B_OK;
}


static void
eqos_uninit_driver(void* cookie)
{
	free(cookie);
}


static status_t
eqos_register_child_devices(void* cookie)
{
	eqos_info* info = (eqos_info*)cookie;

	int32 id = sDeviceManager->create_id(EQOS_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "net/dwc_eqos/%" B_PRId32, id);

	return sDeviceManager->publish_device(info->node, name,
		EQOS_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{NET_BUFFER_MODULE_NAME, (module_info**)&sBufferModule},
	{}
};

static device_module_info sEqosDevice = {
	{
		EQOS_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	eqos_init_device,
	eqos_uninit_device,
	NULL,	// removed
	eqos_open,
	eqos_close,
	eqos_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	eqos_control,
	NULL,	// select
	NULL,	// deselect
};

static driver_module_info sEqosDriver = {
	{
		EQOS_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	eqos_supports_device,
	eqos_register_device,
	eqos_init_driver,
	eqos_uninit_driver,
	eqos_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
};

module_info* modules[] = {
	(module_info*)&sEqosDriver,
	(module_info*)&sEqosDevice,
	NULL
};
