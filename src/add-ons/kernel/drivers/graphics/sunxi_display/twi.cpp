/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "twi.h"

#include <string.h>

#include <vm/vm.h>

#include "registers.h"


#define TRACE(x...) ;
#define ERROR(x...)	dprintf("sunxi_display: twi: " x)


// registers (Allwinner's layout of the Marvell controller)
#define TWI_ADDR			0x00
#define TWI_XADDR			0x04
#define TWI_DATA			0x08
#define TWI_CNTR			0x0c
#define TWI_STAT			0x10
#define TWI_CCR				0x14
#define TWI_SRST			0x18
#define TWI_EFR				0x1c
#define TWI_LCR				0x20

#define CNTR_ACK			(1u << 2)
#define CNTR_IFLG			(1u << 3)	// written as 1 to clear it (Allwinner)
#define CNTR_STOP			(1u << 4)
#define CNTR_START			(1u << 5)
#define CNTR_BUS_EN			(1u << 6)

#define STAT_START			0x08
#define STAT_REPEAT_START	0x10
#define STAT_WRITE_ADDR_ACK	0x18
#define STAT_WRITE_DATA_ACK	0x28
#define STAT_READ_ADDR_ACK	0x40
#define STAT_READ_DATA_ACK	0x50
#define STAT_READ_DATA_NACK	0x58
#define STAT_IDLE			0xf8


using namespace sunxi;


Twi::Twi()
	:
	fArea(-1),
	fRegisters(NULL)
{
}


Twi::~Twi()
{
	if (fArea >= 0)
		delete_area(fArea);
}


status_t
Twi::Init(phys_addr_t base, uint32 gateRegister, uint32 gateBit,
	uint32 clockDivider)
{
	fArea = map_physical_memory("sunxi_display twi", base, B_PAGE_SIZE,
		B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&fRegisters);
	if (fArea < 0)
		return fArea;

	status_t status = mmio_update(A733_R_CCU_BASE, gateRegister,
		0, (1u << gateBit) | (1u << (gateBit + 16)));
	if (status != B_OK)
		return status;
	spin(10);

	_Reset();

	// SCL = clock / (10 * (M + 1) * 2^N): the default M = 15, N = 2 stays
	// at or below 312 kHz for any bus clock up to 200 MHz (37.5 kHz from
	// 24 MHz).
	_Write(TWI_CCR, clockDivider);
	return B_OK;
}


void
Twi::_Reset()
{
	_Write(TWI_SRST, 1);
	for (int i = 0; i < 1000 && (_Read(TWI_SRST) & 1) != 0; i++)
		spin(10);
	_Write(TWI_EFR, 0);
	_Write(TWI_XADDR, 0);
	_Write(TWI_ADDR, 0);
	_Write(TWI_CNTR, CNTR_BUS_EN);
}


status_t
Twi::_Wait(uint32 expected, const char* what)
{
	bigtime_t timeout = system_time() + 20000;
	while ((_Read(TWI_CNTR) & CNTR_IFLG) == 0) {
		if (system_time() > timeout) {
			ERROR("timeout waiting for %s (status %#" B_PRIx32 ")\n", what,
				_Read(TWI_STAT));
			return B_TIMED_OUT;
		}
		spin(5);
	}
	uint32 status = _Read(TWI_STAT) & 0xff;
	if (status != expected) {
		TRACE("%s: status %#" B_PRIx32 ", expected %#" B_PRIx32 "\n", what,
			status, expected);
		return B_IO_ERROR;
	}
	return B_OK;
}


status_t
Twi::_Start(bool repeated)
{
	_Write(TWI_CNTR, CNTR_BUS_EN | CNTR_START | (repeated ? CNTR_IFLG : 0));
	return _Wait(repeated ? STAT_REPEAT_START : STAT_START, "start");
}


status_t
Twi::_SendAddress(uint8 address, bool read)
{
	_Write(TWI_DATA, address << 1 | (read ? 1 : 0));
	_Write(TWI_CNTR, CNTR_BUS_EN | CNTR_IFLG);
	return _Wait(read ? STAT_READ_ADDR_ACK : STAT_WRITE_ADDR_ACK, "address");
}


void
Twi::_Stop()
{
	_Write(TWI_CNTR, CNTR_BUS_EN | CNTR_STOP | CNTR_IFLG);
	bigtime_t timeout = system_time() + 10000;
	while ((_Read(TWI_CNTR) & CNTR_STOP) != 0 && system_time() < timeout)
		spin(5);
}


status_t
Twi::Read(uint8 address, uint8 reg, void* _data, size_t length)
{
	uint8* data = (uint8*)_data;
	status_t status = _Start(false);
	if (status == B_OK)
		status = _SendAddress(address, false);
	if (status == B_OK) {
		_Write(TWI_DATA, reg);
		_Write(TWI_CNTR, CNTR_BUS_EN | CNTR_IFLG);
		status = _Wait(STAT_WRITE_DATA_ACK, "register");
	}
	if (status == B_OK)
		status = _Start(true);
	if (status == B_OK)
		status = _SendAddress(address, true);
	for (size_t i = 0; status == B_OK && i < length; i++) {
		bool last = i == length - 1;
		_Write(TWI_CNTR, CNTR_BUS_EN | CNTR_IFLG | (last ? 0 : CNTR_ACK));
		status = _Wait(last ? STAT_READ_DATA_NACK : STAT_READ_DATA_ACK,
			"data");
		if (status == B_OK)
			data[i] = _Read(TWI_DATA);
	}
	_Stop();
	if (status != B_OK)
		_Reset();
	return status;
}


status_t
Twi::Write(uint8 address, uint8 reg, const void* _data, size_t length)
{
	const uint8* data = (const uint8*)_data;
	status_t status = _Start(false);
	if (status == B_OK)
		status = _SendAddress(address, false);
	if (status == B_OK) {
		_Write(TWI_DATA, reg);
		_Write(TWI_CNTR, CNTR_BUS_EN | CNTR_IFLG);
		status = _Wait(STAT_WRITE_DATA_ACK, "register");
	}
	for (size_t i = 0; status == B_OK && i < length; i++) {
		_Write(TWI_DATA, data[i]);
		_Write(TWI_CNTR, CNTR_BUS_EN | CNTR_IFLG);
		status = _Wait(STAT_WRITE_DATA_ACK, "data");
	}
	_Stop();
	if (status != B_OK)
		_Reset();
	return status;
}


status_t
Twi::Read16(uint8 address, uint8 reg, uint16& value)
{
	uint8 data[2];
	status_t status = Read(address, reg, data, 2);
	if (status == B_OK)
		value = data[0] | (uint16)data[1] << 8;
	return status;
}


status_t
Twi::Write16(uint8 address, uint8 reg, uint16 value)
{
	uint8 data[2] = {(uint8)value, (uint8)(value >> 8)};
	return Write(address, reg, data, 2);
}
