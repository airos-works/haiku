/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "Ths.h"

#include <KernelExport.h>
#include <vm/vm.h>


#define TRACE(x...)	dprintf("sunxi_thermal: " x)


namespace sunxi_thermal {


// The clock controller's THS and GPADC registers (A733 user manual; the
// values Radxa's Linux leaves them at)
static const phys_addr_t kCcu = 0x02002000;
enum {
	CCU_GPADC_CLOCK		= 0x0fc0,	// b31 gate, [26:24] source (0 = 24 MHz)
	CCU_GPADC_BUS		= 0x0fc4,	// b0 bus gate, b16 reset deassert
	CCU_THS_BUS			= 0x0fe4	// b0 bus gate, b16 reset deassert
};

// THS registers (the H616 family's layout)
enum {
	THS_CONTROL			= 0x00,	// [31:16] clock divider, [15:0] acquire time
	THS_ENABLE			= 0x04,	// one bit per sensor
	THS_PERIOD			= 0x08,	// [31:12] sampling period
	THS_DATA_INTERRUPT	= 0x20,
	THS_FILTER			= 0x30,	// b2 enable, [1:0] type
	THS_CALIBRATION		= 0xa0,	// 12 bits per sensor, two per word
	THS_DATA			= 0xc0	// one word per sensor
};

static const uint32 kControl = 479 << 16 | 47;
static const uint32 kPeriod = 28 << 12;
static const uint32 kFilter = 1 << 2 | 1;
static const uint32 kAllSensors = (1u << Ths::kSensorCount) - 1;

// Code to temperature, two lines meeting at code 1769 (Allwinner's numbers
// for the A733: millidegrees = (code - offset) * slope)
static const int32 kKneeCode = 1769;
static const int32 kOffsetAbove = 2822;	// codes above the knee (colder)
static const int32 kSlopeAbove = -62;
static const int32 kOffsetBelow = 2835;
static const int32 kSlopeBelow = -59;

static const uint32 kCalibrationNeutral = 0x800;


static inline void
ccu_update(volatile uint8* ccu, uint32 offset, uint32 clear, uint32 set)
{
	volatile uint32* reg = (volatile uint32*)(ccu + offset);
	*reg = (*reg & ~clear) | set;
	memory_full_barrier();
}


Ths::Ths()
	:
	fArea(-1),
	fRegisters(NULL),
	fCalibrated(false)
{
}


Ths::~Ths()
{
	if (fArea >= 0)
		delete_area(fArea);
}


status_t
Ths::Init(phys_addr_t base, const uint32* calibration)
{
	volatile uint8* ccu;
	area_id ccuArea = map_physical_memory("sunxi_thermal ccu", kCcu,
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&ccu);
	if (ccuArea < 0)
		return ccuArea;

	// the 24 MHz sampling clock, then out of reset and the bus clocks
	ccu_update(ccu, CCU_GPADC_CLOCK, 0x7u << 24 | 0x1f, 1u << 31);
	ccu_update(ccu, CCU_GPADC_BUS, 0, 1u << 16 | 1);
	ccu_update(ccu, CCU_THS_BUS, 0, 1u << 16 | 1);
	spin(10);
	delete_area(ccuArea);

	fArea = map_physical_memory("sunxi_thermal ths", base & ~(B_PAGE_SIZE - 1),
		B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&fRegisters);
	if (fArea < 0)
		return fArea;
	fRegisters += base & (B_PAGE_SIZE - 1);

	if (calibration != NULL)
		_Calibrate(calibration);

	_Write(THS_CONTROL, kControl);
	_Write(THS_FILTER, kFilter);
	_Write(THS_PERIOD, kPeriod);
	_Write(THS_DATA_INTERRUPT, kAllSensors);
	_Write(THS_ENABLE, kAllSensors);
	memory_full_barrier();
	return B_OK;
}


/*static*/ int32
Ths::RawToMilliCelsius(uint32 raw)
{
	int32 code = (int32)raw;
	if (code > kKneeCode)
		return (code - kOffsetAbove) * kSlopeAbove;
	return (code - kOffsetBelow) * kSlopeBelow;
}


status_t
Ths::ReadRaw(int32 sensor, uint32* _raw)
{
	if (fRegisters == NULL)
		return B_NO_INIT;
	if (sensor < 0 || sensor >= kSensorCount)
		return B_BAD_INDEX;
	uint32 raw = _Read(THS_DATA + sensor * 4) & 0xfff;
	if (raw == 0)
		return B_BUSY;
	*_raw = raw;
	return B_OK;
}


status_t
Ths::Read(int32 sensor, int32* _milliCelsius)
{
	uint32 raw;
	status_t status = ReadRaw(sensor, &raw);
	if (status == B_OK)
		*_milliCelsius = RawToMilliCelsius(raw);
	return status;
}


uint32
Ths::CalibrationRegister(int32 index)
{
	return fRegisters != NULL ? _Read(THS_CALIBRATION + index * 4) : 0;
}


/*!	The factory calibration (eFuse): the test temperature in 0.1 degrees in
	bits 11:0 of the first word, then each sensor's code at that temperature,
	12 bits each, packed from bit 12 of the first word on. A sensor's
	correction is the difference in codes, at the colder slope, from the
	neutral 0x800 of the controller.
*/
void
Ths::_Calibrate(const uint32* words)
{
	uint32 testTemperature = words[0] & 0xfff;
	if (testTemperature == 0) {
		TRACE("no factory calibration\n");
		return;
	}

	uint64 bits = (uint64)words[0] | (uint64)words[1] << 32;
	for (int32 sensor = 0; sensor < kSensorCount; sensor++) {
		uint32 shift = 12 + sensor * 12;
		uint32 code;
		if (shift + 12 <= 64)
			code = (bits >> shift) & 0xfff;
		else {
			// the fifth sensor straddles the second and third words
			code = ((words[1] >> 28) | (words[2] << 4)) & 0xfff;
		}
		int32 delta = ((int32)testTemperature * 100
			- RawToMilliCelsius(code)) / kSlopeAbove;
		uint32 value = kCalibrationNeutral - delta;
		if ((value & ~0xfffu) != 0) {
			TRACE("sensor %" B_PRId32 ": calibration %#" B_PRIx32
				" out of range\n", sensor, value);
			continue;
		}
		uint32 offset = THS_CALIBRATION + (sensor / 2) * 4;
		uint32 fieldShift = (sensor % 2) * 16;
		_Write(offset, (_Read(offset) & ~(0xfffu << fieldShift))
			| value << fieldShift);
		fCalibrated = true;
	}
}


}	// namespace sunxi_thermal
