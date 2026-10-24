/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_DISPLAY_PORT_H
#define QCOM_DISPLAY_PORT_H


#include "dpu.h"


namespace qcom {


/*!	One of MDSS 0's DisplayPort controllers (Linux's drm/msm/dp): its AUX
	channel and what it sends. So far only what a link the firmware trained
	tells: hot plug, the sink's DPCD and EDID (I2C over AUX), and the mode
	in the stream's MSA registers.

	AUX transfers are polled: the controller's interrupts reach the CPU
	through MDSS 0's, which the driver shares with the DPU's vertical blank,
	so their masks stay off.
*/
class DisplayPort {
public:
								DisplayPort(uint32 index);
								~DisplayPort();

			status_t			Init();
				// B_DEVICE_NOT_FOUND: its clocks are off

			bool				HotPlug();
			status_t			ReadDpcd(uint32 address, uint8* buffer,
									size_t size);
			status_t			WriteDpcd(uint32 address, const uint8* buffer,
									size_t size);
			status_t			ReadEdid(uint8* edid, uint32* _length);
				// 128 or 256 bytes
			status_t			StreamTiming(display_timing& timing);
				// h_display etc., not the pixel clock
			uint32				Lanes();
			uint32				LinkRate();
			void				SetLinkRate(uint32 rate)
									{ fLinkRate = rate; }
				// for a link the driver trained: its MHz, as LinkRate()
				// MHz of the link symbol clock (162, 270, 540, 810), from
				// the sink's DPCD: the rate the firmware trained
			uint32				MaxPixelClock();
				// kHz the link carries at 24 bits per pixel
			uint32				PixelClock(bool wideBus);
				// kHz, from the pixel RCG's M/N; 0 if it cannot tell

			// a new mode on the trained link: StopStream(), the timing
			// engine reprogrammed, StartStream()
			status_t			StopStream();
			status_t			SetStream(const display_timing& timing,
									bool wideBus);
			status_t			StartStream();

private:
			uint32				_Read(uint32 offset) const
									{ return *(volatile uint32*)(fBase
										+ offset); }
			void				_Write(uint32 offset, uint32 value)
									{ *(volatile uint32*)(fBase + offset)
										= value; }
			status_t			_SetPixelClock(uint32 rate);
			status_t			_Transfer(bool native, bool read,
									uint32 address, uint8* buffer,
									size_t size, bool stop);

			uint32				fIndex;
			area_id				fArea;
			volatile uint8*		fBase;
			area_id				fClockArea;
			volatile uint8*		fClocks;	// the pixel RCG's page of dispcc
			uint32				fLinkRate;
};


}	// namespace qcom


#endif	// QCOM_DISPLAY_PORT_H
