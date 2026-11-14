/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Cadence GEM (Gigabit Ethernet MAC) in the Raspberry Pi 5's RP1 south
	bridge, with its Broadcom BCM54213PE RGMII PHY on the GEM's own MDIO bus.

	The RP1 sits behind PCIe. The loader translates the register address and
	turns the RP1's interrupt into a plain GIC SPI; since the Ethernet source
	is level triggered in the RP1, every handler run ends with an acknowledge
	at the RP1 ("airos,rp1-iack"), or the RP1 never signals again.

	One queue in each direction (queue 0), 256 descriptors each. The GEM is
	not cache coherent here: the descriptors live in uncached memory, packets
	are copied to and from cacheable buffers with explicit cache maintenance,
	as the Pi 4's GENET driver does. The MAC copies all frames (promiscuous);
	the network stack sorts them out. Further queues the design may have get
	descriptors that tie them off, so the DMA never walks into memory that
	is not ours.

	Register layout and bring-up order follow Linux' macb_main.c for the
	"raspberrypi,rp1-gem" configuration. */


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


//#define TRACE_GEM
#ifdef TRACE_GEM
#	define TRACE(x...) dprintf("rp1_gem: " x)
#else
#	define TRACE(x...) ;
#endif
#define INFO(x...)	dprintf("rp1_gem: " x)
#define ERROR(x...)	dprintf("rp1_gem: " x)


#define GEM_DRIVER_MODULE_NAME		"drivers/network/rp1_gem/driver_v1"
#define GEM_DEVICE_MODULE_NAME		"drivers/network/rp1_gem/device_v1"
#define GEM_DEVICE_ID_GENERATOR		"rp1_gem/device_id"


// registers
#define GEM_NCR					0x0000
#define  NCR_RE					(1 << 2)
#define  NCR_TE					(1 << 3)
#define  NCR_MPE				(1 << 4)
#define  NCR_CLRSTAT			(1 << 5)
#define  NCR_TSTART				(1 << 9)
#define GEM_NCFGR				0x0004
#define  NCFGR_SPD				(1 << 0)
#define  NCFGR_FD				(1 << 1)
#define  NCFGR_CAF				(1 << 4)
#define  NCFGR_BIG				(1 << 8)
#define  NCFGR_GBE				(1 << 10)
#define  NCFGR_RBOF_SHIFT		14
#define  NCFGR_DRFCS			(1 << 17)
#define  NCFGR_CLK_SHIFT		18
#define  NCFGR_CLK_DIV128		6
#define  NCFGR_DBW_SHIFT		21
#define GEM_NSR					0x0008
#define  NSR_IDLE				(1 << 2)
#define GEM_USRIO				0x000c
#define  USRIO_RGMII			(1 << 0)
#define GEM_DMACFG				0x0010
#define  DMACFG_FBLDO_MASK		0x1f
#define  DMACFG_FBLDO_INCR16	16
#define  DMACFG_ENDIA_DESC		(1 << 6)
#define  DMACFG_ENDIA_PKT		(1 << 7)
#define  DMACFG_RXBMS_FULL		(3 << 8)
#define  DMACFG_TXPBMS			(1 << 10)
#define  DMACFG_TXCOEN			(1 << 11)
#define  DMACFG_RXBS_SHIFT		16
#define  DMACFG_RXBS_MASK		(0xffu << DMACFG_RXBS_SHIFT)
#define  DMACFG_RXEXT			(1 << 28)
#define  DMACFG_TXEXT			(1 << 29)
#define  DMACFG_ADDR64			(1 << 30)
#define GEM_TSR					0x0014
#define  TSR_TGO				(1 << 3)
#define GEM_RBQP				0x0018
#define GEM_TBQP				0x001c
#define GEM_RSR					0x0020
#define GEM_ISR					0x0024
#define GEM_IER					0x0028
#define GEM_IDR					0x002c
#define GEM_IMR					0x0030
#define  INT_RCOMP				(1 << 1)
#define  INT_RXUBR				(1 << 2)
#define  INT_TXUBR				(1 << 3)
#define  INT_TUND				(1 << 4)
#define  INT_RLE				(1 << 5)
#define  INT_TXERR				(1 << 6)
#define  INT_TCOMP				(1 << 7)
#define  INT_ROVR				(1 << 10)
#define  INT_HRESP				(1 << 11)
#define  INT_TX_ERRORS			(INT_TUND | INT_RLE | INT_TXERR)
#define GEM_MAN					0x0034
#define  MAN_SOF_C22			(1u << 30)
#define  MAN_READ				(2u << 28)
#define  MAN_WRITE				(1u << 28)
#define  MAN_PHY_SHIFT			23
#define  MAN_REG_SHIFT			18
#define  MAN_CODE				(2u << 16)
#define GEM_PBUFRXCUT			0x0044
#define GEM_AMP					0x0054
#define  AMP_AR2R_SHIFT			0
#define  AMP_AW2W_SHIFT			8
#define  AMP_AW2B_FILL			(1 << 16)
#define GEM_INTMOD				0x005c
#define GEM_HRB					0x0080
#define GEM_HRT					0x0084
#define GEM_SA1B				0x0088
#define GEM_SA1T				0x008c
#define GEM_MID					0x00fc
#define GEM_DCFG1				0x0280
#define  DCFG1_IRQCOR			(1 << 23)
#define  DCFG1_DBWDEF_SHIFT		25
#define GEM_DCFG6				0x0294
#define  DCFG6_DAW64			(1 << 23)
// the other queues (1 to 7)
#define GEM_QUEUE_ISR(q)		(0x0400 + ((q) - 1) * 4)
#define GEM_QUEUE_TBQP(q)		(0x0440 + ((q) - 1) * 4)
#define GEM_QUEUE_RBQP(q)		(0x0480 + ((q) - 1) * 4)
#define GEM_QUEUE_RBQS(q)		(0x04a0 + ((q) - 1) * 4)
#define GEM_QUEUE_IDR(q)		(0x0620 + ((q) - 1) * 4)
// the upper halves of the descriptor ring addresses, shared by all queues
#define GEM_TBQPH				0x04c8
#define GEM_RBQPH				0x04d4

#define GEM_MAX_QUEUES			8

// descriptors: address, control/status, and with 64-bit addressing the
// address' upper half and a reserved word
#define DESC_ADDRESS			0
#define DESC_CONTROL			1
#define DESC_ADDRESS_HIGH		2
#define RX_USED					(1u << 0)
#define RX_WRAP					(1u << 1)
#define RX_LENGTH_MASK			0x0fff
#define RX_SOF					(1u << 14)
#define RX_EOF					(1u << 15)
#define TX_LAST					(1u << 15)
#define TX_WRAP					(1u << 30)
#define TX_USED					(1u << 31)

#define DESCRIPTOR_COUNT		256
#define MAX_DESCRIPTOR_SIZE		16
#define RING_SIZE				(DESCRIPTOR_COUNT * MAX_DESCRIPTOR_SIZE)
#define TIE_OFF_OFFSET			(2 * RING_SIZE)
#define DESCRIPTOR_AREA_SIZE	(4 * RING_SIZE)
	// two rings, then the tie-off descriptors; a power of two, aligned to
	// itself, so that it never crosses a 4 GB boundary (the rings share
	// the upper halves of their addresses)
#define BUFFER_SIZE				2048
#define RX_OFFSET				2
	// two bytes in front of each received frame align the IP header
#define MAX_FRAME_SIZE			1536
#define CACHE_LINE_SIZE			64

// RP1 MSI-X configuration: acknowledges a level triggered interrupt
#define RP1_MSIX_CFG_IACK		(1 << 2)

// PHY registers (Broadcom BCM54xx)
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
#define MII_BCM54XX_AUX_CTL		0x18
#define MII_BCM54XX_SHD			0x1c


struct gem_info {
	device_node*	node;
	uint64			registersBase;
	uint64			registersSize;
	uint32			interrupt;
	uint64			interruptAckAddress;
	uint8			macAddress[6];
	uint32			phyAddress;
	bool			rxDelayInPhy;
	bool			txDelayInPhy;
	uint8			ar2rMaxPipe;
	uint8			aw2wMaxPipe;
	bool			aw2bFill;

	area_id			registerArea;
	volatile uint8*	registers;
	area_id			interruptAckArea;
	volatile uint32* interruptAck;
	bool			interruptInstalled;

	uint32			revision;
	uint32			queueMask;
	bool			isrClearOnWrite;
	uint32			busWidth;
	uint32			descriptorSize;	// 8, or 16 with 64-bit addressing

	area_id			descriptorArea;
	uint8*			descriptors;
	phys_addr_t		descriptorsAddress;
	uint8*			rxRing;
	uint8*			txRing;
	phys_addr_t		rxRingAddress;
	phys_addr_t		txRingAddress;

	area_id			bufferArea;
	uint8*			buffers;
		// DESCRIPTOR_COUNT receive buffers, then as many for transmission
	phys_addr_t		buffersAddress;

	mutex			rxLock;
	mutex			txLock;
	spinlock		lock;
		// NCR's read-modify-writes, and the TX interrupt bookkeeping below
	sem_id			rxSemaphore;
	sem_id			txSemaphore;
	uint32			rxIndex;	// next descriptor to look at
	uint32			txIndex;	// next descriptor to fill
	uint32			txCleaned;	// oldest descriptor the controller may own
	bool			txWaiting;	// a writer waits for a free descriptor
	bool			txKickPending;
		// a TSTART may have been dropped; look again when TX stops

	int32			openCount;
	bool			nonblocking;

	thread_id		linkThread;
	sem_id			linkThreadSemaphore;
	bool			stopping;
	int32			errorStatus;	// DMA errors for the link thread
	sem_id			linkChangeSemaphore;
	bool			linkUp;
	uint32			linkSpeed;	// Mbit/s
	bool			fullDuplex;
};


static device_manager_info* sDeviceManager;
static net_buffer_module_info* sBufferModule;


static inline uint32
read32(gem_info* info, uint32 reg)
{
	return *(volatile uint32*)(info->registers + reg);
}


static inline void
write32(gem_info* info, uint32 reg, uint32 value)
{
	*(volatile uint32*)(info->registers + reg) = value;
}


/*!	Orders writes to the uncached descriptors as the controller sees them
	(Linux' dma_wmb()).
*/
static inline void
dma_write_barrier()
{
	asm volatile("dmb oshst" : : : "memory");
}


/*!	Orders reads from the uncached descriptors (Linux' dma_rmb()). */
static inline void
dma_read_barrier()
{
	asm volatile("dmb oshld" : : : "memory");
}


static void
dma_buffer_for_cpu(const void* buffer, size_t size)
{
	// The GEM does not snoop the ARM caches. RX slots are cache-line
	// aligned and only read by the CPU, so no dirty CPU bytes can be lost.
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


static inline volatile uint32*
rx_descriptor(gem_info* info, uint32 index)
{
	return (volatile uint32*)(info->rxRing + index * info->descriptorSize);
}


static inline volatile uint32*
tx_descriptor(gem_info* info, uint32 index)
{
	return (volatile uint32*)(info->txRing + index * info->descriptorSize);
}


static inline phys_addr_t
rx_buffer_address(gem_info* info, uint32 index)
{
	return info->buffersAddress + (uint64)index * BUFFER_SIZE;
}


static inline phys_addr_t
tx_buffer_address(gem_info* info, uint32 index)
{
	return info->buffersAddress
		+ (uint64)(DESCRIPTOR_COUNT + index) * BUFFER_SIZE;
}


//	#pragma mark - PHY


static status_t
mdio_wait(gem_info* info)
{
	bigtime_t timeout = system_time() + 20000;
	while ((read32(info, GEM_NSR) & NSR_IDLE) == 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(10);
	}
	return B_OK;
}


static int32
mdio_read(gem_info* info, uint32 reg)
{
	if (mdio_wait(info) != B_OK)
		return B_TIMED_OUT;
	write32(info, GEM_MAN, MAN_SOF_C22 | MAN_READ
		| info->phyAddress << MAN_PHY_SHIFT | reg << MAN_REG_SHIFT | MAN_CODE);
	if (mdio_wait(info) != B_OK)
		return B_TIMED_OUT;
	return read32(info, GEM_MAN) & 0xffff;
}


static status_t
mdio_write(gem_info* info, uint32 reg, uint16 value)
{
	if (mdio_wait(info) != B_OK)
		return B_TIMED_OUT;
	write32(info, GEM_MAN, MAN_SOF_C22 | MAN_WRITE
		| info->phyAddress << MAN_PHY_SHIFT | reg << MAN_REG_SHIFT | MAN_CODE
		| value);
	return mdio_wait(info);
}


static bool
phy_present(gem_info* info)
{
	int32 id1 = mdio_read(info, MII_PHYSID1);
	int32 id2 = mdio_read(info, MII_PHYSID2);
	return id1 >= 0 && id2 >= 0 && id1 != 0xffff && (id1 != 0 || id2 != 0);
}


static status_t
phy_init(gem_info* info)
{
	if (!phy_present(info)) {
		// The device tree says address 1; look around before giving up.
		uint32 expected = info->phyAddress;
		for (info->phyAddress = 0; info->phyAddress < 32;
				info->phyAddress++) {
			if (info->phyAddress != expected && phy_present(info))
				break;
		}
		if (info->phyAddress == 32) {
			info->phyAddress = expected;
			ERROR("no PHY on the MDIO bus\n");
			return B_DEVICE_NOT_FOUND;
		}
	}
	INFO("PHY %04" B_PRIx32 ":%04" B_PRIx32 " at MDIO address %" B_PRIu32 "\n",
		mdio_read(info, MII_PHYSID1), mdio_read(info, MII_PHYSID2),
		info->phyAddress);

	mdio_write(info, MII_BMCR, BMCR_RESET);
	bigtime_t timeout = system_time() + 500000;
	while ((mdio_read(info, MII_BMCR) & BMCR_RESET) != 0) {
		if (system_time() > timeout)
			return B_TIMED_OUT;
		snooze(1000);
	}

	// RGMII clock delays ("phy-mode"; "rgmii-id" on the Pi 5: both in the
	// PHY): the receive clock delay in auxiliary control, shadow 7, bit 8;
	// the transmit clock delay in shadow register 0x1c, selector 3, bit 9.
	mdio_write(info, MII_BCM54XX_AUX_CTL, 0x7007);
	int32 value = mdio_read(info, MII_BCM54XX_AUX_CTL);
	if (value >= 0) {
		if (info->rxDelayInPhy)
			value |= 1 << 8;
		else
			value &= ~(1 << 8);
		mdio_write(info, MII_BCM54XX_AUX_CTL, 0x8000 | (value & 0x7ff8) | 0x7);
	}

	mdio_write(info, MII_BCM54XX_SHD, 0x03 << 10);
	value = mdio_read(info, MII_BCM54XX_SHD);
	if (value >= 0) {
		value &= 0x3ff;
		if (info->txDelayInPhy)
			value |= 1 << 9;
		else
			value &= ~(1 << 9);
		mdio_write(info, MII_BCM54XX_SHD, 0x8000 | 0x03 << 10 | value);
	}

	// Advertise everything up to 1000BASE-T full duplex. No pause: the MAC
	// is not set up to honour pause frames (Linux runs it without, too).
	mdio_write(info, MII_ADVERTISE, 0x0001 | 0x01e0);
	mdio_write(info, MII_CTRL1000, 0x0200);
	mdio_write(info, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);
	return B_OK;
}


//	#pragma mark - controller


static uint32
gem_network_config(gem_info* info, uint32 speed, bool fullDuplex)
{
	// MDC: the RP1's clk_sys (pclk) runs at up to 200 MHz; / 128 keeps MDC
	// below 2.5 MHz up to 320 MHz.
	uint32 config = NCFGR_CLK_DIV128 << NCFGR_CLK_SHIFT
		| RX_OFFSET << NCFGR_RBOF_SHIFT | NCFGR_DRFCS | NCFGR_BIG | NCFGR_CAF
		| info->busWidth << NCFGR_DBW_SHIFT;
	if (speed == 1000)
		config |= NCFGR_GBE;
	else if (speed == 100)
		config |= NCFGR_SPD;
	if (fullDuplex)
		config |= NCFGR_FD;
	return config;
}


/*!	Returns whether the transmitter is still running. If it stopped while
	the last frame queued is not sent yet (a TSTART can get lost while it
	runs), it is started again. Called with the lock held; the writer may
	be queueing a frame meanwhile, but a TSTART too many only makes the idle
	transmitter read a used descriptor and stop again.
*/
static bool
tx_kick_locked(gem_info* info)
{
	if ((read32(info, GEM_TSR) & TSR_TGO) != 0)
		return true;

	uint32 last = (*(volatile uint32*)&info->txIndex + DESCRIPTOR_COUNT - 1)
		% DESCRIPTOR_COUNT;
	dma_read_barrier();
	if ((tx_descriptor(info, last)[DESC_CONTROL] & TX_USED) == 0) {
		TRACE("restarting the transmitter, last frame at %" B_PRIu32 "\n",
			last);
		write32(info, GEM_NCR, read32(info, GEM_NCR) | NCR_TSTART);
	}
	return false;
}


static void
clear_interrupts(gem_info* info)
{
	// The status registers clear on read, or, in some designs, on write.
	read32(info, GEM_ISR);
	if (info->isrClearOnWrite)
		write32(info, GEM_ISR, 0xffffffff);
	for (uint32 queue = 1; queue < GEM_MAX_QUEUES; queue++) {
		if ((info->queueMask & (1 << queue)) == 0)
			continue;
		read32(info, GEM_QUEUE_ISR(queue));
		if (info->isrClearOnWrite)
			write32(info, GEM_QUEUE_ISR(queue), 0xffffffff);
	}
}


static int32
gem_interrupt(void* data)
{
	gem_info* info = (gem_info*)data;
	int32 result = B_UNHANDLED_INTERRUPT;

	uint32 status = read32(info, GEM_ISR);
	if (status != 0 && info->isrClearOnWrite)
		write32(info, GEM_ISR, status);
	// IMR: a set bit is a masked interrupt
	uint32 active = status & ~read32(info, GEM_IMR);

	if (active != 0) {
		result = B_HANDLED_INTERRUPT;

		// The reader drains RX with the interrupt masked.
		if ((active & INT_RCOMP) != 0) {
			write32(info, GEM_IDR, INT_RCOMP);
			release_sem_etc(info->rxSemaphore, 1, B_DO_NOT_RESCHEDULE);
			result = B_INVOKE_SCHEDULER;
		}

		// TX interrupts are on while a writer waits for descriptors, or
		// while a TSTART may have been lost.
		if ((active & (INT_TCOMP | INT_TXUBR)) != 0) {
			SpinLocker locker(info->lock);
			if (!tx_kick_locked(info))
				info->txKickPending = false;
			if (!info->txKickPending && !info->txWaiting)
				write32(info, GEM_IDR, INT_TCOMP | INT_TXUBR);
			bool wake = info->txWaiting;
			locker.Unlock();

			if (wake) {
				release_sem_etc(info->txSemaphore, 1, B_DO_NOT_RESCHEDULE);
				result = B_INVOKE_SCHEDULER;
			}
		}

		// The DMA stops on these; the link thread starts it over.
		if ((active & (INT_TX_ERRORS | INT_HRESP)) != 0) {
			write32(info, GEM_IDR, INT_TX_ERRORS | INT_HRESP);
			atomic_or(&info->errorStatus,
				(int32)(active & (INT_TX_ERRORS | INT_HRESP)));
			release_sem_etc(info->linkThreadSemaphore, 1, B_DO_NOT_RESCHEDULE);
			result = B_INVOKE_SCHEDULER;
		}
	}

	// The RP1 sends the next message for its level triggered source only
	// after this.
	if (info->interruptAck != NULL)
		*info->interruptAck = RP1_MSIX_CFG_IACK;

	return result;
}


/*!	Turns the receiver, the transmitter and all interrupts off. */
static void
gem_stop(gem_info* info)
{
	{
		InterruptsSpinLocker locker(info->lock);
		// keep the management port: the PHY is still to be talked to
		write32(info, GEM_NCR, NCR_MPE);
		info->txKickPending = false;
	}

	write32(info, GEM_IDR, 0xffffffff);
	for (uint32 queue = 1; queue < GEM_MAX_QUEUES; queue++) {
		if ((info->queueMask & (1 << queue)) != 0)
			write32(info, GEM_QUEUE_IDR(queue), 0xffffffff);
	}
	clear_interrupts(info);

	write32(info, GEM_TSR, 0xffffffff);
	write32(info, GEM_RSR, 0xffffffff);
}


/*!	Gives all receive descriptors to the controller, takes all transmit
	descriptors from it, and points the queues at the rings. The receiver
	and the transmitter must be off: only then does the controller take new
	queue pointers.
*/
static void
gem_init_rings(gem_info* info)
{
	bool address64 = info->descriptorSize == 16;

	for (uint32 i = 0; i < DESCRIPTOR_COUNT; i++) {
		phys_addr_t address = rx_buffer_address(info, i);
		volatile uint32* descriptor = rx_descriptor(info, i);
		if (address64)
			descriptor[DESC_ADDRESS_HIGH] = (uint32)(address >> 32);
		descriptor[DESC_CONTROL] = 0;
		dma_write_barrier();
		descriptor[DESC_ADDRESS] = (uint32)address
			| (i == DESCRIPTOR_COUNT - 1 ? RX_WRAP : 0);
	}

	for (uint32 i = 0; i < DESCRIPTOR_COUNT; i++) {
		phys_addr_t address = tx_buffer_address(info, i);
		volatile uint32* descriptor = tx_descriptor(info, i);
		if (address64)
			descriptor[DESC_ADDRESS_HIGH] = (uint32)(address >> 32);
		descriptor[DESC_ADDRESS] = (uint32)address;
		descriptor[DESC_CONTROL] = TX_USED
			| (i == DESCRIPTOR_COUNT - 1 ? TX_WRAP : 0);
	}

	// The tie-offs: rings of one descriptor that is always the software's,
	// for the queues this driver does not use.
	volatile uint32* rxTieOff
		= (volatile uint32*)(info->descriptors + TIE_OFF_OFFSET);
	volatile uint32* txTieOff = (volatile uint32*)(info->descriptors
		+ TIE_OFF_OFFSET + MAX_DESCRIPTOR_SIZE);
	phys_addr_t rxTieOffAddress = info->descriptorsAddress + TIE_OFF_OFFSET;
	phys_addr_t txTieOffAddress = rxTieOffAddress + MAX_DESCRIPTOR_SIZE;
	rxTieOff[DESC_ADDRESS] = RX_USED | RX_WRAP;
	rxTieOff[DESC_CONTROL] = 0;
	txTieOff[DESC_ADDRESS] = 0;
	txTieOff[DESC_CONTROL] = TX_USED | TX_WRAP;
	if (address64) {
		rxTieOff[DESC_ADDRESS_HIGH] = 0;
		txTieOff[DESC_ADDRESS_HIGH] = 0;
	}
	memory_full_barrier();

	info->rxIndex = 0;
	info->txIndex = 0;
	info->txCleaned = 0;

	// one upper half for all queues: everything is in one area that does
	// not cross a 4 GB boundary
	if (address64) {
		write32(info, GEM_RBQPH, (uint32)(info->descriptorsAddress >> 32));
		write32(info, GEM_TBQPH, (uint32)(info->descriptorsAddress >> 32));
	}
	write32(info, GEM_RBQP, (uint32)info->rxRingAddress);
	write32(info, GEM_TBQP, (uint32)info->txRingAddress);

	for (uint32 queue = 1; queue < GEM_MAX_QUEUES; queue++) {
		if ((info->queueMask & (1 << queue)) == 0)
			continue;
		write32(info, GEM_QUEUE_RBQS(queue), BUFFER_SIZE / 64);
		write32(info, GEM_QUEUE_RBQP(queue), (uint32)rxTieOffAddress);
		write32(info, GEM_QUEUE_TBQP(queue), (uint32)txTieOffAddress);
	}
}


/*!	Starts the receiver and the transmitter over with fresh rings, with
	the MAC set to \a speed, or leaves them off when \a up is \c false.
	The link state is set under the locks so that the writers see it
	together with the rings.
*/
static void
gem_restart(gem_info* info, bool up, uint32 speed, bool fullDuplex)
{
	MutexLocker txLocker(info->txLock);
	MutexLocker rxLocker(info->rxLock);

	{
		InterruptsSpinLocker locker(info->lock);
		write32(info, GEM_NCR,
			read32(info, GEM_NCR) & ~(NCR_RE | NCR_TE | NCR_TSTART));
		info->txKickPending = false;
	}
	write32(info, GEM_IDR, INT_TX_ERRORS | INT_HRESP);

	// Let a frame in flight finish its way into memory before the rings
	// change under it.
	bigtime_t timeout = system_time() + 10000;
	while ((read32(info, GEM_TSR) & TSR_TGO) != 0 && system_time() < timeout)
		snooze(100);
	snooze(2000);

	gem_init_rings(info);
	write32(info, GEM_TSR, 0xffffffff);
	write32(info, GEM_RSR, 0xffffffff);
	write32(info, GEM_NCFGR, gem_network_config(info, speed, fullDuplex));

	info->linkUp = up;
	info->linkSpeed = up ? speed : 0;
	info->fullDuplex = up && fullDuplex;

	if (up) {
		write32(info, GEM_IER, INT_TX_ERRORS | INT_HRESP);
		InterruptsSpinLocker locker(info->lock);
		write32(info, GEM_NCR, read32(info, GEM_NCR) | NCR_RE | NCR_TE);
	}

	// a writer waiting for descriptors finds them all free now
	release_sem_etc(info->txSemaphore, 1, B_DO_NOT_RESCHEDULE);
}


static void
update_link(gem_info* info)
{
	// the link bit latches a failure: read twice
	mdio_read(info, MII_BMSR);
	int32 status = mdio_read(info, MII_BMSR);
	bool up = status >= 0 && status != 0xffff	// no PHY answers
		&& (status & BMSR_LSTATUS) != 0;

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

	// Turning the transmitter off resets its queue pointer to the start of
	// the ring: the rings start over with every link change.
	gem_restart(info, up, speed, fullDuplex);

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
	gem_info* info = (gem_info*)data;
	while (!info->stopping) {
		update_link(info);

		int32 errors = atomic_get_and_set(&info->errorStatus, 0);
		if (errors != 0 && info->linkUp) {
			ERROR("DMA error (interrupt status %#" B_PRIx32 "), restarting\n",
				(uint32)errors);
			gem_restart(info, true, info->linkSpeed, info->fullDuplex);
		}

		if (info->linkUp) {
			// in case a lost TSTART went unnoticed
			InterruptsSpinLocker locker(info->lock);
			tx_kick_locked(info);
		}

		acquire_sem_etc(info->linkThreadSemaphore, 1, B_RELATIVE_TIMEOUT,
			1000000);
	}
	return B_OK;
}


static void
gem_start(gem_info* info)
{
	gem_stop(info);

	// clear the statistics
	write32(info, GEM_NCR, NCR_MPE | NCR_CLRSTAT);

	info->linkUp = false;
	info->linkSpeed = 0;
	info->fullDuplex = false;
	info->txWaiting = false;
	write32(info, GEM_NCFGR, gem_network_config(info, 0, false));

	// RGMII, as Linux' default USRIO for "raspberrypi,rp1-gem"
	write32(info, GEM_USRIO, USRIO_RGMII);

	uint32 dmaConfig = read32(info, GEM_DMACFG);
	dmaConfig &= ~(DMACFG_RXBS_MASK | DMACFG_FBLDO_MASK | DMACFG_ENDIA_DESC
		| DMACFG_ENDIA_PKT | DMACFG_TXCOEN | DMACFG_RXEXT | DMACFG_TXEXT
		| DMACFG_ADDR64);
	dmaConfig |= (BUFFER_SIZE / 64) << DMACFG_RXBS_SHIFT | DMACFG_FBLDO_INCR16
		| DMACFG_TXPBMS | DMACFG_RXBMS_FULL;
	if (info->descriptorSize == 16)
		dmaConfig |= DMACFG_ADDR64;
	write32(info, GEM_DMACFG, dmaConfig);

	// AXI pipelining, only where the device tree says so
	uint32 amp = read32(info, GEM_AMP);
	if (info->aw2bFill)
		amp |= AMP_AW2B_FILL;
	if (info->aw2wMaxPipe != 0) {
		amp = (amp & ~(0xffu << AMP_AW2W_SHIFT))
			| (uint32)info->aw2wMaxPipe << AMP_AW2W_SHIFT;
	}
	if (info->ar2rMaxPipe != 0) {
		amp = (amp & ~(0xffu << AMP_AR2R_SHIFT))
			| (uint32)info->ar2rMaxPipe << AMP_AR2R_SHIFT;
	}
	write32(info, GEM_AMP, amp);

	// interrupt moderation: 50 us in either direction (800 ns units)
	write32(info, GEM_INTMOD, 62 << 16 | 62);
	// store and forward, no cut-through
	write32(info, GEM_PBUFRXCUT, 0);

	const uint8* mac = info->macAddress;
	write32(info, GEM_SA1B, mac[0] | mac[1] << 8 | mac[2] << 16
		| (uint32)mac[3] << 24);
	write32(info, GEM_SA1T, mac[4] | mac[5] << 8);
	for (uint32 i = 1; i < 4; i++) {
		write32(info, GEM_SA1B + i * 8, 0);
		write32(info, GEM_SA1T + i * 8, 0);
	}
	write32(info, GEM_HRB, 0);
	write32(info, GEM_HRT, 0);

	gem_init_rings(info);

	// The receiver and the transmitter stay off until the link is up.
	phy_init(info);
}


static status_t
gem_receive(gem_info* info, net_buffer** _buffer)
{
	while (true) {
		MutexLocker locker(info->rxLock);

		uint32 index = info->rxIndex;
		volatile uint32* descriptor = rx_descriptor(info, index);
		uint32 address = descriptor[DESC_ADDRESS];
		if ((address & RX_USED) != 0) {
			dma_read_barrier();
			uint32 status = descriptor[DESC_CONTROL];
			uint32 length = status & RX_LENGTH_MASK;
			uint8* data = info->buffers + (uint64)index * BUFFER_SIZE;

			net_buffer* buffer = NULL;
			bool good = (status & (RX_SOF | RX_EOF)) == (RX_SOF | RX_EOF)
				&& length >= ETHER_HEADER_LENGTH
				&& RX_OFFSET + length <= BUFFER_SIZE;
			if (good) {
				dma_buffer_for_cpu(data, RX_OFFSET + length);
				buffer = sBufferModule->create(0);
				if (buffer != NULL && sBufferModule->append(buffer,
						data + RX_OFFSET, length) != B_OK) {
					sBufferModule->free(buffer);
					buffer = NULL;
				}
			} else {
				TRACE("dropped frame, status %#" B_PRIx32 "\n", status);
			}

			// back to the controller: the status first, then the address
			// word, whose cleared used bit hands the descriptor over
			phys_addr_t bufferAddress = rx_buffer_address(info, index);
			if (info->descriptorSize == 16)
				descriptor[DESC_ADDRESS_HIGH] = (uint32)(bufferAddress >> 32);
			descriptor[DESC_CONTROL] = 0;
			dma_write_barrier();
			descriptor[DESC_ADDRESS] = (uint32)bufferAddress
				| (index == DESCRIPTOR_COUNT - 1 ? RX_WRAP : 0);
			info->rxIndex = (index + 1) % DESCRIPTOR_COUNT;

			if (buffer != NULL) {
				*_buffer = buffer;
				return B_OK;
			}
			continue;
		}

		if (info->nonblocking)
			return B_WOULD_BLOCK;

		// Arm, then recheck: a frame can arrive after the empty-ring test
		// while the interrupt is still masked. Never sleep with it pending.
		// (The status register is not cleared here: reading it would also
		// clear what the interrupt handler still has to see.)
		write32(info, GEM_IER, INT_RCOMP);
		read32(info, GEM_IMR);
			// the enable has reached the controller before the recheck
		dma_read_barrier();
		if ((rx_descriptor(info, info->rxIndex)[DESC_ADDRESS] & RX_USED)
				!= 0) {
			write32(info, GEM_IDR, INT_RCOMP);
			continue;
		}
		locker.Unlock();

		status_t status = acquire_sem_etc(info->rxSemaphore, 1,
			B_CAN_INTERRUPT, 0);
		if (status != B_OK)
			return status;
	}
}


/*!	Forgets the descriptors the controller has sent. */
static void
clean_tx(gem_info* info)
{
	while (info->txCleaned != info->txIndex) {
		if ((tx_descriptor(info, info->txCleaned)[DESC_CONTROL] & TX_USED)
				== 0) {
			break;
		}
		info->txCleaned = (info->txCleaned + 1) % DESCRIPTOR_COUNT;
	}
	dma_read_barrier();
}


static inline uint32
tx_pending(gem_info* info)
{
	return (info->txIndex + DESCRIPTOR_COUNT - info->txCleaned)
		% DESCRIPTOR_COUNT;
}


static status_t
gem_send(gem_info* info, net_buffer* buffer)
{
	size_t length = buffer->size;
	if (length > MAX_FRAME_SIZE)
		return B_BAD_DATA;

	MutexLocker locker(info->txLock);
	while (true) {
		if (!info->linkUp) {
			// The transmitter is off without a link; nothing would go out.
			locker.Unlock();
			sBufferModule->free(buffer);
			return B_OK;
		}

		// One descriptor always stays the software's: it stops the
		// transmitter behind the last frame.
		clean_tx(info);
		if (tx_pending(info) < DESCRIPTOR_COUNT - 1)
			break;

		if (info->nonblocking)
			return B_WOULD_BLOCK;

		{
			InterruptsSpinLocker spinLocker(info->lock);
			info->txWaiting = true;
			write32(info, GEM_IER, INT_TCOMP | INT_TXUBR);
			tx_kick_locked(info);
		}
		clean_tx(info);
		if (tx_pending(info) < DESCRIPTOR_COUNT - 1) {
			InterruptsSpinLocker spinLocker(info->lock);
			info->txWaiting = false;
			continue;
		}
		locker.Unlock();

		status_t status = acquire_sem_etc(info->txSemaphore, 1,
			B_CAN_INTERRUPT | B_RELATIVE_TIMEOUT, 100000);
		locker.Lock();
		{
			InterruptsSpinLocker spinLocker(info->lock);
			info->txWaiting = false;
		}
		if (status != B_OK && status != B_TIMED_OUT)
			return status;
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

	// The address words first, then the control word, whose cleared used
	// bit hands the descriptor over. The controller appends the FCS.
	phys_addr_t address = tx_buffer_address(info, index);
	volatile uint32* descriptor = tx_descriptor(info, index);
	if (info->descriptorSize == 16)
		descriptor[DESC_ADDRESS_HIGH] = (uint32)(address >> 32);
	descriptor[DESC_ADDRESS] = (uint32)address;
	dma_write_barrier();
	descriptor[DESC_CONTROL] = length | TX_LAST
		| (index == DESCRIPTOR_COUNT - 1 ? TX_WRAP : 0);
	info->txIndex = (index + 1) % DESCRIPTOR_COUNT;

	// the descriptor in memory before the controller is told about it
	memory_full_barrier();
	{
		InterruptsSpinLocker spinLocker(info->lock);
		bool running = (read32(info, GEM_TSR) & TSR_TGO) != 0;
		write32(info, GEM_NCR, read32(info, GEM_NCR) | NCR_TSTART);
		if (running) {
			// A TSTART while the transmitter runs can get lost, should it
			// have read past this descriptor already: look again when it
			// stops, and right now, in case it stopped before the
			// interrupt was on.
			info->txKickPending = true;
			write32(info, GEM_IER, INT_TCOMP | INT_TXUBR);
			if (!tx_kick_locked(info))
				info->txKickPending = false;
		}
	}
	locker.Unlock();

	sBufferModule->free(buffer);
	return B_OK;
}


//	#pragma mark - device


static status_t
allocate_dma_memory(gem_info* info)
{
	// Without 64-bit addressing the controller reaches the low 4 GB only.
	physical_address_restrictions physicalRestrictions = {};
	if (info->descriptorSize != 16)
		physicalRestrictions.high_address = (phys_addr_t)1 << 32;
	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address_specification = B_ANY_KERNEL_ADDRESS;

	// the descriptors, uncached
	physicalRestrictions.alignment = DESCRIPTOR_AREA_SIZE;
	void* descriptors;
	info->descriptorArea = create_area_etc(B_SYSTEM_TEAM, "rp1_gem descriptors",
		DESCRIPTOR_AREA_SIZE, B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &virtualRestrictions,
		&physicalRestrictions, &descriptors);
	if (info->descriptorArea < 0)
		return info->descriptorArea;

	physical_entry entry;
	get_memory_map(descriptors, DESCRIPTOR_AREA_SIZE, &entry, 1);
	memset(descriptors, 0, DESCRIPTOR_AREA_SIZE);
	for (addr_t line = (addr_t)descriptors;
			line < (addr_t)descriptors + DESCRIPTOR_AREA_SIZE;
			line += CACHE_LINE_SIZE) {
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	}
	memory_full_barrier();
	status_t status = vm_set_area_memory_type(info->descriptorArea,
		entry.address, B_WRITE_COMBINING_MEMORY);
	if (status != B_OK) {
		delete_area(info->descriptorArea);
		return status;
	}
	info->descriptors = (uint8*)descriptors;
	info->descriptorsAddress = entry.address;
	info->rxRing = info->descriptors;
	info->txRing = info->descriptors + RING_SIZE;
	info->rxRingAddress = entry.address;
	info->txRingAddress = entry.address + RING_SIZE;

	// The packets, cacheable. Buffer boundaries are also cache-line
	// boundaries so RX invalidation cannot discard another slot's data.
	size_t size = 2 * DESCRIPTOR_COUNT * BUFFER_SIZE;
	physicalRestrictions.alignment = 0;
	info->bufferArea = create_area_etc(B_SYSTEM_TEAM, "rp1_gem buffers", size,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, (void**)&info->buffers);
	if (info->bufferArea < 0) {
		delete_area(info->descriptorArea);
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
	return B_OK;
}


static bool
valid_mac_address(const uint8* address)
{
	// neither all zeros nor multicast
	return (address[0] & 1) == 0 && (address[0] | address[1] | address[2]
		| address[3] | address[4] | address[5]) != 0;
}


static status_t
gem_init_device(void* _info, void** _cookie)
{
	gem_info* info = (gem_info*)_info;

	info->registerArea = map_physical_memory("rp1_gem registers",
		info->registersBase, info->registersSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&info->registers);
	if (info->registerArea < 0)
		return info->registerArea;

	// module ID 2 and up: a GEM rather than the older MACB
	info->revision = read32(info, GEM_MID);
	if (((info->revision >> 16) & 0xfff) < 2) {
		ERROR("not a GEM (module ID %#" B_PRIx32 ")\n", info->revision);
		delete_area(info->registerArea);
		return B_NOT_SUPPORTED;
	}

	uint32 config1 = read32(info, GEM_DCFG1);
	uint32 config6 = read32(info, GEM_DCFG6);
	info->isrClearOnWrite = (config1 & DCFG1_IRQCOR) == 0;
	switch ((config1 >> DCFG1_DBWDEF_SHIFT) & 0x7) {
		case 4:
			info->busWidth = 2;	// 128 bits
			break;
		case 2:
			info->busWidth = 1;	// 64 bits
			break;
		default:
			info->busWidth = 0;	// 32 bits
			break;
	}
	// queue 0 always exists; bits 1 to 7 tell of the others
	info->queueMask = 1 | (config6 & 0xfe);
	info->descriptorSize = (config6 & DCFG6_DAW64) != 0 ? 16 : 8;

	status_t status = allocate_dma_memory(info);
	if (status != B_OK) {
		delete_area(info->registerArea);
		return status;
	}

	if (!valid_mac_address(info->macAddress)) {
		// what the firmware may have left in the first address filter
		uint32 bottom = read32(info, GEM_SA1B);
		uint32 top = read32(info, GEM_SA1T);
		uint8 address[6] = {(uint8)bottom, (uint8)(bottom >> 8),
			(uint8)(bottom >> 16), (uint8)(bottom >> 24), (uint8)top,
			(uint8)(top >> 8)};
		if (!valid_mac_address(address)) {
			// locally administered, so that the board at least gets on
			// the net
			const uint8 fallback[6] = {0x02, 0xa1, 0x05, 0x00, 0x00, 0x05};
			memcpy(address, fallback, 6);
		}
		memcpy(info->macAddress, address, 6);
	}

	info->interruptAckArea = -1;
	info->interruptAck = NULL;
	if (info->interruptAckAddress != 0) {
		phys_addr_t page = info->interruptAckAddress
			& ~(phys_addr_t)(B_PAGE_SIZE - 1);
		void* mapping;
		info->interruptAckArea = map_physical_memory("rp1_gem interrupt ack",
			page, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &mapping);
		if (info->interruptAckArea >= 0) {
			info->interruptAck = (volatile uint32*)((uint8*)mapping
				+ (info->interruptAckAddress - page));
		} else
			ERROR("cannot map the RP1 interrupt acknowledge register\n");
	}

	mutex_init(&info->rxLock, "rp1_gem rx");
	mutex_init(&info->txLock, "rp1_gem tx");
	B_INITIALIZE_SPINLOCK(&info->lock);
	info->rxSemaphore = -1;
	info->txSemaphore = -1;
	info->linkThreadSemaphore = -1;
	info->linkChangeSemaphore = -1;
	info->linkThread = -1;

	gem_stop(info);

	INFO("GEM %#" B_PRIx32 " at %#" B_PRIx64 ", interrupt %" B_PRIu32
		"%s, queues %#" B_PRIx32 ", %s addressing, bus width %d, "
		"%02x:%02x:%02x:%02x:%02x:%02x\n", info->revision,
		info->registersBase, info->interrupt,
		info->interruptAck != NULL ? " (RP1 acknowledged)" : "",
		info->queueMask, info->descriptorSize == 16 ? "64-bit" : "32-bit",
		32 << info->busWidth, info->macAddress[0], info->macAddress[1],
		info->macAddress[2], info->macAddress[3], info->macAddress[4],
		info->macAddress[5]);
	TRACE("descriptors at %#" B_PRIxPHYSADDR ", buffers at %#"
		B_PRIxPHYSADDR "\n", info->descriptorsAddress, info->buffersAddress);

	*_cookie = info;
	return B_OK;
}


static void
gem_uninit_device(void* cookie)
{
	gem_info* info = (gem_info*)cookie;

	gem_stop(info);
	mutex_destroy(&info->rxLock);
	mutex_destroy(&info->txLock);
	delete_area(info->bufferArea);
	delete_area(info->descriptorArea);
	if (info->interruptAckArea >= 0)
		delete_area(info->interruptAckArea);
	delete_area(info->registerArea);
}


static status_t
gem_open(void* _info, const char* path, int openMode, void** _cookie)
{
	gem_info* info = (gem_info*)_info;

	if (atomic_add(&info->openCount, 1) != 0) {
		atomic_add(&info->openCount, -1);
		return B_BUSY;
	}

	info->nonblocking = (openMode & O_NONBLOCK) != 0;
	info->stopping = false;
	info->errorStatus = 0;
	info->rxSemaphore = create_sem(0, "rp1_gem rx");
	info->txSemaphore = create_sem(0, "rp1_gem tx");
	info->linkThreadSemaphore = create_sem(0, "rp1_gem link");

	status_t status = install_io_interrupt_handler(info->interrupt,
		gem_interrupt, info, 0);
	if (status != B_OK) {
		delete_sem(info->rxSemaphore);
		delete_sem(info->txSemaphore);
		delete_sem(info->linkThreadSemaphore);
		atomic_add(&info->openCount, -1);
		return status;
	}
	info->interruptInstalled = true;

	gem_start(info);

	info->linkThread = spawn_kernel_thread(link_thread, "rp1_gem link",
		B_NORMAL_PRIORITY, info);
	resume_thread(info->linkThread);

	*_cookie = info;
	return B_OK;
}


static status_t
gem_close(void* cookie)
{
	gem_info* info = (gem_info*)cookie;

	info->stopping = true;
	release_sem(info->linkThreadSemaphore);
	status_t result;
	wait_for_thread(info->linkThread, &result);
	info->linkThread = -1;

	gem_stop(info);
	info->linkUp = false;

	if (info->interruptInstalled) {
		remove_io_interrupt_handler(info->interrupt, gem_interrupt, info);
		info->interruptInstalled = false;
	}

	delete_sem(info->rxSemaphore);
	delete_sem(info->txSemaphore);
	delete_sem(info->linkThreadSemaphore);
	info->rxSemaphore = info->txSemaphore = info->linkThreadSemaphore = -1;
	return B_OK;
}


static status_t
gem_free(void* cookie)
{
	gem_info* info = (gem_info*)cookie;
	atomic_add(&info->openCount, -1);
	return B_OK;
}


static status_t
gem_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	gem_info* info = (gem_info*)cookie;

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
			// the controller copies all frames anyway
			return B_OK;

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
			return gem_send(info, (net_buffer*)buffer);

		case ETHER_RECEIVE_NET_BUFFER:
			if (buffer == NULL || length == 0)
				return B_BAD_DATA;
			if (!IS_KERNEL_ADDRESS(buffer))
				return B_BAD_ADDRESS;
			return gem_receive(info, (net_buffer**)buffer);
	}

	return B_DEV_INVALID_IOCTL;
}


//	#pragma mark - driver


static float
gem_supports_device(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0) {
		return 0.0f;
	}

	// Only the RP1's GEM: other "cdns,macb" have other glue.
	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, "fdt/compatible", &compatible,
			false) != B_OK
		|| strcmp(compatible, "raspberrypi,rp1-gem") != 0) {
		return 0.0f;
	}

	return 1.0f;
}


static status_t
gem_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{.string = "Raspberry Pi RP1 Ethernet (Cadence GEM)"}},
		{}
	};

	return sDeviceManager->register_node(parent, GEM_DRIVER_MODULE_NAME,
		attrs, NULL, NULL);
}


/*!	Reads a one byte device tree property ("[08]"), or a cell. */
static uint8
fdt_u8(fdt_device_module_info* fdt, fdt_device* device, const char* name)
{
	int length;
	const uint8* property = (const uint8*)fdt->get_prop(device, name, &length);
	if (property == NULL)
		return 0;
	if (length == 1)
		return property[0];
	if (length == 4)
		return property[3];
	return 0;
}


static status_t
gem_init_driver(device_node* node, void** _cookie)
{
	device_node* parent = sDeviceManager->get_parent_node(node);
	fdt_device_module_info* fdt;
	fdt_device* device;
	status_t status = sDeviceManager->get_driver(parent,
		(driver_module_info**)&fdt, (void**)&device);
	sDeviceManager->put_node(parent);
	if (status != B_OK)
		return status;

	gem_info* info = (gem_info*)calloc(1, sizeof(gem_info));
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

	// the loader's: where the RP1 wants its level triggered interrupt
	// acknowledged
	int length;
	const void* property = fdt->get_prop(device, "airos,rp1-iack", &length);
	if (property != NULL && length == 8) {
		uint64 address;
		memcpy(&address, property, sizeof(address));
		info->interruptAckAddress = B_BENDIAN_TO_HOST_INT64(address);
	}

	// The firmware fills in the address; if it does not, init_device
	// looks at the controller.
	property = fdt->get_prop(device, "local-mac-address", &length);
	if (property != NULL && length == 6)
		memcpy(info->macAddress, property, 6);

	// "rgmii-id": the PHY delays both clocks; "rgmii": the board does
	property = fdt->get_prop(device, "phy-mode", &length);
	if (property != NULL && length > 0) {
		const char* mode = (const char*)property;
		info->rxDelayInPhy = strcmp(mode, "rgmii-id") == 0
			|| strcmp(mode, "rgmii-rxid") == 0;
		info->txDelayInPhy = strcmp(mode, "rgmii-id") == 0
			|| strcmp(mode, "rgmii-txid") == 0;
	} else {
		// what the Pi 5 has
		info->rxDelayInPhy = true;
		info->txDelayInPhy = true;
	}

	info->ar2rMaxPipe = fdt_u8(fdt, device, "cdns,ar2r-max-pipe");
	info->aw2wMaxPipe = fdt_u8(fdt, device, "cdns,aw2w-max-pipe");
	info->aw2bFill = fdt->get_prop(device, "cdns,use-aw2b-fill", &length)
		!= NULL;

	// The Pi 5 has the PHY at address 1 (the "reg" of the node that
	// "phy-handle" names); phy_init() looks further if it is not there.
	info->phyAddress = 1;

	*_cookie = info;
	return B_OK;
}


static void
gem_uninit_driver(void* cookie)
{
	free(cookie);
}


static status_t
gem_register_child_devices(void* cookie)
{
	gem_info* info = (gem_info*)cookie;

	int32 id = sDeviceManager->create_id(GEM_DEVICE_ID_GENERATOR);
	if (id < 0)
		return id;

	char name[64];
	snprintf(name, sizeof(name), "net/rp1_gem/%" B_PRId32, id);

	return sDeviceManager->publish_device(info->node, name,
		GEM_DEVICE_MODULE_NAME);
}


module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{NET_BUFFER_MODULE_NAME, (module_info**)&sBufferModule},
	{}
};

static device_module_info sGemDevice = {
	{
		GEM_DEVICE_MODULE_NAME,
		0,
		NULL
	},
	gem_init_device,
	gem_uninit_device,
	NULL,	// removed
	gem_open,
	gem_close,
	gem_free,
	NULL,	// read
	NULL,	// write
	NULL,	// io
	gem_control,
	NULL,	// select
	NULL,	// deselect
};

static driver_module_info sGemDriver = {
	{
		GEM_DRIVER_MODULE_NAME,
		0,
		NULL
	},
	gem_supports_device,
	gem_register_device,
	gem_init_driver,
	gem_uninit_driver,
	gem_register_child_devices,
	NULL,	// rescan
	NULL,	// removed
	NULL,	// suspend
	NULL,	// resume
};

module_info* modules[] = {
	(module_info*)&sGemDriver,
	(module_info*)&sGemDevice,
	NULL
};
