/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_USBC_DP_H
#define QCOM_USBC_DP_H


#include <OS.h>

#include "dpu.h"


namespace qcom {


class DisplayPort;
struct usbc_connector;


/*!	DisplayPort over a USB-C connector of the SC8280XP (connector 0: DP0,
	connector 1: DP1; each with its QMP USB43DP combo PHY and PI3USB102 SBU
	mux), and a DPU path to it (connector 0: VIG0, LM_0, CTL_0, INTF_0;
	connector 1: VIG1, LM_1, CTL_1, INTF_4) made from the path the firmware
	lit for the HDMI output (VIG2, LM_2, CTL_2, INTF_6), with its timing and
	format, on another region of the frame buffer
	(docs/dragon-q8b/USB-C-DISPLAYPORT.md).

	PowerUp() brings the PHY up in DisplayPort mode at RBR (four lanes, the
	normal orientation), DP0's link, AUX and pixel clocks and its AUX
	channel; Start() lights the path, then the link: the controller's
	mainlink, the link training with the sink (Linux' dp_ctrl.c: clock
	recovery on TPS1, equalization on TPS2/3, swing and pre-emphasis as the
	sink asks), the stream. Without a sink answering on AUX, the training
	is not done, the controller's training patterns are checked instead,
	and the stream runs untrained.
*/
class UsbcDisplayPort {
public:
								UsbcDisplayPort(uint32 connector);
								~UsbcDisplayPort();

			//! The plug's orientation and how many lanes are DisplayPort's
			//! (the port report: pin assignments C and E four, D two),
			//! before PowerUp().
			void				SetPortMode(bool reverse, uint32 lanes);
			bool				MatchesPort(bool reverse, uint32 lanes) const
									{ return fReverse == reverse
										&& fPhyLanes == lanes; }
			status_t			PowerUp();
			status_t			Start(phys_addr_t address, uint32 bytesPerRow,
									uint32 width, uint32 height,
									const display_timing& timing,
									bool wideBus);
			status_t			SetScanout(phys_addr_t address,
									uint32 bytesPerRow, uint32 width,
									uint32 height);
			void				Stop();
			void				LogState(const char* when);

			//! The sink's IRQ_HPD: its link status read, its IRQ vector
			//! cleared, the link trained again if it lost it.
			void				Attention();
			bool				Trained() const { return fTrained; }
			status_t			ReadEdid(uint8* edid, uint32* _length);

private:
			status_t			_Map(area_id& area, volatile uint8*& base,
									const char* name, phys_addr_t address,
									size_t size);
			status_t			_PowerPhy();
			status_t			_PowerUsb();
			status_t			_ConfigureRate(uint32 rate);
			status_t			_SetPhyLevels(uint32 swing,
									uint32 preEmphasis);
			status_t			_Clocks();
			void				_SetPixelClock();
			status_t			_InitPort();
			void				_DumpAgainstHdmi();
			void				_DumpBranch();
			void				_TestPattern(const display_timing& timing);
			void				_EnableAux();
			void				_EnableMainlink(uint32 lanes);
			status_t			_SendPattern(uint32 pattern);
			status_t			_SetLevels(uint32 swing, uint32 preEmphasis,
									uint32 lanes);
			status_t			_Train(uint32* _lanes);
			status_t			_TrainAt(uint32 rate, uint32 lanes,
									bool enhanced, bool tps3,
									bigtime_t crDelay, bigtime_t eqDelay);
			void				_Link(const display_timing& timing,
									bool wideBus);

			const usbc_connector& fConnector;
			uint32				fMixerBase;
			uint32				fControlBase;
			uint32				fLayer;			// CTL_LAYER(lm)
			uint32				fLayerExt;		// CTL_LAYER_EXT(lm)
			area_id				fGccArea;
			volatile uint8*		fGcc;
			area_id				fDispccArea;
			volatile uint8*		fDispcc;
			area_id				fPhyArea;
			volatile uint8*		fPhy;
			area_id				fControllerArea;
			volatile uint8*		fController;
			DisplayPort*		fPort;			// its AUX and stream
			area_id				fMdssArea;
			volatile uint8*		fMdss;
			uint32				fRate;			// MHz, the PHY's link rate
			display_timing		fTiming;		// the stream's, for a retraining
			bool				fWideBus;
			const uint32*		fFirmwarePhy;	// lab: its set-up as found
			bool				fReverse;		// the plug's orientation
			uint32				fPhyLanes;		// DisplayPort's lanes: 2 or 4
			uint32				fLanes;			// the trained link's
			bool				fTrained;
			uint32				fRetrainings;	// since the last trained link
			bool				fPowered;
			bool				fRunning;
};


}	// namespace qcom


#endif	// QCOM_USBC_DP_H
