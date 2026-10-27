/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SUNXI_THERMAL_THS_H
#define SUNXI_THERMAL_THS_H


#include <OS.h>


namespace sunxi_thermal {


/*	The Allwinner A733's thermal sensor controller (THS): five sensors (the
	big cluster, DRAM controller, NPU, little cluster, GPU), sampled by the
	hardware, read by polling. The eFuse calibration goes into the
	controller, which then reports calibrated codes. */
class Ths {
public:
	static	const int32			kSensorCount = 5;

								Ths();
								~Ths();

			// \a calibration: the three eFuse words of the factory
			// calibration, or NULL
			status_t			Init(phys_addr_t base,
									const uint32* calibration);

			// the calibrated code, B_BUSY until the first conversion
			status_t			ReadRaw(int32 sensor, uint32* _raw);
			status_t			Read(int32 sensor, int32* _milliCelsius);

	static	int32				RawToMilliCelsius(uint32 raw);

			uint32				CalibrationRegister(int32 index);
			bool				Calibrated() const { return fCalibrated; }

private:
			void				_Calibrate(const uint32* calibration);
			uint32				_Read(uint32 offset)
									{ return *(volatile uint32*)
										(fRegisters + offset); }
			void				_Write(uint32 offset, uint32 value)
									{ *(volatile uint32*)(fRegisters + offset)
										= value; }

			area_id				fArea;
			volatile uint8*		fRegisters;
			bool				fCalibrated;
};


}	// namespace sunxi_thermal


#endif	// SUNXI_THERMAL_THS_H
