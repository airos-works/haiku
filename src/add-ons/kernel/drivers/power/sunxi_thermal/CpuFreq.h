/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SUNXI_THERMAL_CPU_FREQ_H
#define SUNXI_THERMAL_CPU_FREQ_H


#include <OS.h>

#include <sunxi_pmic.h>


namespace sunxi_thermal {


struct OperatingPoint {
	uint32		kilohertz;
	uint32		microvolts;
};


/*	One cluster of the A733: the cores that share a PLL and a supply rail
	(the six Cortex-A55s on DCDC5, the two Cortex-A76s on DCDC3). */
struct Cluster {
	static	const int32			kMaxOperatingPoints = 24;
	static	const int32			kMaxCpus = 8;

			char				name[16];
			uint32				pllOffset;		// in the CPU PLL block
			int32				cpus[kMaxCpus];
			int32				cpuCount;

			OperatingPoint		points[kMaxOperatingPoints];
			int32				pointCount;		// ascending

			uint32				dcdc;			// the PMIC rail
			uint32				floorMicrovolts;	// what else on the rail
												// needs (the DSU)

			int32				current;		// index into points, -1
												// before the first change
			uint32				microvolts;		// the rail now
			uint32				bootKilohertz;
			uint32				bootMicrovolts;

			uint32				transitions;
			uint32				errors;

			uint32				CurrentKilohertz() const
									{ return current >= 0
										? points[current].kilohertz
										: bootKilohertz; }
			// the highest point at or below the frequency (0 if none)
			int32				IndexAtOrBelow(uint32 kilohertz) const;
			// the lowest point at or above it (the top one if none)
			int32				IndexAtOrAbove(uint32 kilohertz) const;
};


/*	The CPU clusters' frequency and voltage, with the operating points of
	the device tree (the BSP's, chosen by the chip's speed grade). The
	PLL changes ramp (the PLLs' linear frequency modulation), so the cores
	keep running through them, as in Linux. */
class CpuFreq {
public:
	static	const int32			kMaxClusters = 2;

								CpuFreq();
								~CpuFreq();

			status_t			Init(const void* fdt,
									sunxi_pmic_module_info* pmic);

			int32				ClusterCount() const { return fClusterCount; }
			Cluster&			ClusterAt(int32 index)
									{ return fClusters[index]; }
			const char*			SpeedGrade() const { return fSpeedGrade; }

			// Voltage up before the frequency, down after it.
			status_t			SetPoint(Cluster& cluster, int32 index);

			// The PLL as the registers have it.
			uint32				PllKilohertz(uint32 pllOffset);
			uint32				DsuKilohertz()
									{ return PllKilohertz(kDsuPll); }

			// Times a loop of dependent adds on one of the cluster's cores.
			uint32				MeasureMegahertz(const Cluster& cluster);

			void				DumpRegisters(const char* when);

			// the last changes, for the kernel debugger ("sunxi_thermal")
			void				DumpTrace();

			// margins: the time a raised voltage gets beyond the PMIC's
			// nominal slew (percent of it), and whether a PLL change waits
			// for its frequency ramp to end
			uint32				voltageSettlePercent;
			bool				waitPllRamp;

private:
	static	const uint32		kDsuPll = 0x3000;

			status_t			_ParseClusters(const void* fdt);
			status_t			_ParseTable(const void* fdt, int table,
									OperatingPoint* points, int32* _count);
			status_t			_ReadSpeedGrade(const void* fdt, int table);
			uint32				_RailOf(const void* fdt, int node,
									const char* property);
			void				_ComputeDsuFloor(const void* fdt);
			void				_InitPll(uint32 pllOffset);
			status_t			_SetPll(uint32 pllOffset, uint32 kilohertz);
			status_t			_WaitClear(uint32 offset, uint32 bit,
									bigtime_t timeout);

			uint32				_Read(uint32 offset)
									{ return *(volatile uint32*)
										(fPll + offset); }
			void				_Write(uint32 offset, uint32 value);

			sunxi_pmic_module_info* fPmic;
			area_id				fPllArea;
			volatile uint8*		fPll;
			uint32				fParentHz;
			char				fSpeedGrade[16];

			Cluster				fClusters[kMaxClusters];
			int32				fClusterCount;

			// a ring of the last changes' steps
	static	const int32			kTraceSize = 64;
			struct TraceEntry {
				bigtime_t		time;
				const char*		step;
				const char*		cluster;
				uint32			kilohertz;
				uint32			microvolts;
			};
			void				_Trace(const Cluster& cluster,
									const char* step, uint32 kilohertz,
									uint32 microvolts);
			TraceEntry			fTrace[kTraceSize];
			int32				fTraceNext;
};


}	// namespace sunxi_thermal


#endif	// SUNXI_THERMAL_CPU_FREQ_H
