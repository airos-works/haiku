/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SUNXI_DISPLAY_TWI_H
#define SUNXI_DISPLAY_TWI_H


/*	A polled I2C master on one of the A733's TWI controllers (Marvell
	mv64xxx-style register interface, Allwinner layout: Linux'
	i2c-mv64xxx.c). Used for the USB Type-C port controller (R_TWI1) and,
	through the sunxi_regulator driver's PMIC module, for the PMIC (R_TWI0).
	Not thread safe: the caller serializes. */


#include <KernelExport.h>
#include <OS.h>


namespace sunxi {


class Twi {
public:
								Twi();
								~Twi();

			// \a gate is a register of the R clock controller with the bus
			// gate at \a gateBit and the reset at \a gateBit + 16;
			// \a clockDivider is the CCR value (SCL = 24 MHz /
			// (10 * (M + 1) * 2^N) with M in bits 6:3, N in bits 2:0), by
			// default 37.5 kHz (at most 312 kHz from any bus clock)
			status_t			Init(phys_addr_t base, uint32 gateRegister,
									uint32 gateBit,
									uint32 clockDivider = kSlowClock);

	static	const uint32		kSlowClock = 15 << 3 | 2;

			status_t			Read(uint8 address, uint8 reg, void* data,
									size_t length);
			status_t			Write(uint8 address, uint8 reg,
									const void* data, size_t length);

			status_t			Read8(uint8 address, uint8 reg, uint8& value)
									{ return Read(address, reg, &value, 1); }
			status_t			Read16(uint8 address, uint8 reg,
									uint16& value);
			status_t			Write8(uint8 address, uint8 reg, uint8 value)
									{ return Write(address, reg, &value, 1); }
			status_t			Write16(uint8 address, uint8 reg,
									uint16 value);

private:
			uint32				_Read(uint32 reg)
									{ return *(volatile uint32*)(fRegisters + reg); }
			void				_Write(uint32 reg, uint32 value)
									{ *(volatile uint32*)(fRegisters + reg) = value; }

			status_t			_Wait(uint32 expected, const char* what);
			status_t			_Start(bool repeated);
			status_t			_SendAddress(uint8 address, bool read);
			void				_Stop();
			void				_Reset();

			area_id				fArea;
			volatile uint8*		fRegisters;
};


}	// namespace sunxi


#endif	// SUNXI_DISPLAY_TWI_H
