/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_DPU_H
#define QCOM_DPU_H


#include <OS.h>


namespace qcom {


struct display_timing {
	uint32	pixel_clock;		// kHz
	uint16	h_display;
	uint16	h_sync_start;
	uint16	h_sync_end;
	uint16	h_total;
	uint16	v_display;
	uint16	v_sync_start;
	uint16	v_sync_end;
	uint16	v_total;
	bool	h_sync_positive;
	bool	v_sync_positive;
};


/*!	MDSS 0's display processing unit (DPU 8.0, Linux's dpu_8_0_sc8280xp.h).

	So far it takes over the path the firmware left lit: a source pipe
	fetching the firmware's frame buffer, through a layer mixer and a control
	path, to the timing engine (INTF) of one DisplayPort controller. Init()
	finds it, Timing() is its mode, and SetScanout() points the pipe at
	another buffer, or another region of one, from the next frame on. The
	MDSS's stream bypasses the SMMU, so the pipe fetches physical addresses.
*/
class Dpu {
public:
								Dpu();
								~Dpu();

			status_t			Init();
				// B_DEVICE_NOT_FOUND: nothing lit, or the block is off

			uint32				Interface() const { return fInterface; }
				// INTF number, 6 for the HDMI output
			const display_timing& Timing() const { return fTiming; }

			status_t			SetScanout(phys_addr_t address,
									uint32 bytesPerRow, uint32 width,
									uint32 height);

			// the interface's vertical blank interrupt
			void				EnableBlankInterrupt(bool enable);
			bool				AcknowledgeBlank();
				// whether it was the interface's

			status_t			DebugRegister(uint64 address, uint32& value,
									bool write);

private:
			uint32				_Read(uint32 offset) const
									{ return *(volatile uint32*)(fMdss
										+ offset); }
			void				_Write(uint32 offset, uint32 value)
									{ *(volatile uint32*)(fMdss + offset)
										= value; }
			bool				_PoweredUp();
			status_t			_FindPath();
			void				_ReadTiming();

			area_id				fArea;
			volatile uint8*		fMdss;
			uint32				fInterface;
			uint32				fInterfaceBase;
			uint32				fControlBase;
			uint32				fPipeBase;
			uint32				fPipeFlush;
			uint32				fMixerFlush;
			uint32				fBlankBit;
			display_timing		fTiming;
};


}	// namespace qcom


#endif	// QCOM_DPU_H
