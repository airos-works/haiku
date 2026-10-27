/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SUNXI_DISPLAY_REGISTERS_H
#define SUNXI_DISPLAY_REGISTERS_H


/*	Allwinner A733 addresses and small register helpers shared by the parts
	of the display driver. */


#include <KernelExport.h>
#include <OS.h>

#include <vm/vm.h>


namespace sunxi {


// clock controllers
static const phys_addr_t A733_CCU_BASE		= 0x02002000;
static const phys_addr_t A733_R_CCU_BASE	= 0x07010000;

// pin controllers: the main one (banks A..K, 0x80 each after 0x80) and the
// R one (banks L, M, 0x30 each)
static const phys_addr_t A733_PIO_BASE		= 0x02000000;
static const phys_addr_t A733_R_PIO_BASE	= 0x07025000;

// TWI controller of the USB Type-C port controller (S_TWI1), its gate and
// reset in the R clock controller
static const phys_addr_t A733_R_TWI1_BASE	= 0x07084000;
static const uint32 A733_R_CCU_TWI_GATE		= 0x19c;
static const uint32 A733_R_CCU_TWI1_BIT		= 1;


/*!	Maps the page with \a reg of the block at \a base for the call, and
	writes (value & ~clear) | set back.
*/
static inline status_t
mmio_update(phys_addr_t base, uint32 reg, uint32 clear, uint32 set)
{
	phys_addr_t page = (base + reg) & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	volatile uint8* mapping;
	area_id area = map_physical_memory("sunxi_display register", page,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&mapping);
	if (area < 0)
		return area;
	volatile uint32* address
		= (volatile uint32*)(mapping + ((base + reg) & (B_PAGE_SIZE - 1)));
	*address = (*address & ~clear) | set;
	memory_full_barrier();
	delete_area(area);
	return B_OK;
}


static inline status_t
mmio_read(phys_addr_t base, uint32 reg, uint32& value)
{
	phys_addr_t page = (base + reg) & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	volatile uint8* mapping;
	area_id area = map_physical_memory("sunxi_display register", page,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA,
		(void**)&mapping);
	if (area < 0)
		return area;
	value = *(volatile uint32*)(mapping + ((base + reg) & (B_PAGE_SIZE - 1)));
	delete_area(area);
	return B_OK;
}


// pins of the R pin controller: PL0 is pin 0 of bank 0
enum {
	PIN_FUNCTION_INPUT	= 0,
	PIN_FUNCTION_OUTPUT	= 1,
	PIN_FUNCTION_IRQ	= 0xe,
	PIN_FUNCTION_OFF	= 0xf
};

#define R_PIO_BANK_SIZE		0x30
#define R_PIO_CFG(pin)		(((pin) / 8) * 4)
#define R_PIO_DATA			0x10

static inline status_t
r_pin_set_function(uint32 bank, uint32 pin, uint32 function)
{
	uint32 shift = (pin % 8) * 4;
	return mmio_update(A733_R_PIO_BASE, bank * R_PIO_BANK_SIZE
		+ R_PIO_CFG(pin), 0xfu << shift, function << shift);
}


static inline status_t
r_pin_set(uint32 bank, uint32 pin, bool high)
{
	return mmio_update(A733_R_PIO_BASE, bank * R_PIO_BANK_SIZE + R_PIO_DATA,
		high ? 0 : 1u << pin, high ? 1u << pin : 0);
}


static inline bool
r_pin_get(uint32 bank, uint32 pin)
{
	uint32 value = 0;
	mmio_read(A733_R_PIO_BASE, bank * R_PIO_BANK_SIZE + R_PIO_DATA, value);
	return (value & (1u << pin)) != 0;
}


}	// namespace sunxi


#endif	// SUNXI_DISPLAY_REGISTERS_H
