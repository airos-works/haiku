/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_USBC_DP_H
#define QCOM_USBC_DP_H


#include <OS.h>


namespace qcom {


/*!	DisplayPort over USB-C connector 0 of the SC8280XP (DP0, its QMP USB43DP
	combo PHY, the PI3USB102 SBU mux), and a second DPU path to it: VIG0,
	LM_0, CTL_0 and INTF_0, made from the path the firmware lit for the
	HDMI output (VIG2, LM_2, CTL_2, INTF_6), with its timing and format, on
	another region of the frame buffer (docs/dragon-q8b/USB-C-DISPLAYPORT.md).

	PowerUp() brings the PHY up in DisplayPort mode at RBR (four lanes, the
	normal orientation), DP0's link, AUX and pixel clocks and its AUX
	channel; Start() lights the path. The DisplayPort controller's link
	training and mainlink are not done yet: the path scans out, nothing is
	sent to a monitor.
*/
class UsbcDisplayPort {
public:
								UsbcDisplayPort();
								~UsbcDisplayPort();

			status_t			PowerUp();
			status_t			Start(phys_addr_t address, uint32 bytesPerRow,
									uint32 width, uint32 height);
			status_t			SetScanout(phys_addr_t address,
									uint32 bytesPerRow, uint32 width,
									uint32 height);
			void				Stop();
			void				LogState(const char* when);

private:
			status_t			_Map(area_id& area, volatile uint8*& base,
									const char* name, phys_addr_t address,
									size_t size);
			status_t			_PowerPhy();
			status_t			_Clocks();
			void				_EnableAux();

			area_id				fGccArea;
			volatile uint8*		fGcc;
			area_id				fDispccArea;
			volatile uint8*		fDispcc;
			area_id				fPhyArea;
			volatile uint8*		fPhy;
			area_id				fControllerArea;
			volatile uint8*		fController;
			area_id				fMdssArea;
			volatile uint8*		fMdss;
			bool				fPowered;
			bool				fRunning;
};


}	// namespace qcom


#endif	// QCOM_USBC_DP_H
