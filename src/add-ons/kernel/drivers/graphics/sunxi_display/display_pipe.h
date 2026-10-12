/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SUNXI_DISPLAY_DISPLAY_PIPE_H
#define SUNXI_DISPLAY_DISPLAY_PIPE_H


/*	The A733's DisplayPort output path, as the Cubie A7S wires it: display
	engine 3.5 (disp0, video channel 0) -> TCON-TV1 (through TCON TOP1) ->
	the DisplayPort transmitter -> combo PHY 0 -> the USB-C port, pin
	assignment C (four lanes). The TCPC (typec.h) negotiates the alternate
	mode and reports hot plug; this class does everything after that.

	Register-level recipe: evidence/linux-display/SPEC.md of the lab, worked
	out from Allwinner's BSP and the registers of the board while Linux
	showed a picture. All addresses below are physical. */


#include <OS.h>


namespace sunxi {


// TCON-TV1's interrupt: GIC SPI 58, level (Allwinner's device tree)
static const uint32 kTconTv1Interrupt = 32 + 58;


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


class DisplayPipe {
public:
								DisplayPipe();
								~DisplayPipe();

			// once: power, clocks, IOMMU bypass, PHYs, controller
			status_t			Init();

			// The alternate mode is up and the sink has HPD high: read the
			// DPCD and the EDID, and pick the sink's preferred timing.
			status_t			Discover(bool flipped, uint8* edid,
									uint32* edidLength,
									display_timing& timing);

			// Shows B_RGB32 memory at \a address (below 4 GiB) in the
			// given timing; SetScanout() then scales any region to it.
			status_t			Enable(const display_timing& timing,
									phys_addr_t address, uint32 bytesPerRow);
			status_t			SetScanout(phys_addr_t address,
									uint32 bytesPerRow, uint32 width,
									uint32 height);
			void				Disable();
			// whether the sink still has the link: clock recovery, channel
			// equalization and symbol lock on every lane, lanes aligned
			bool				LinkOk();
			// From the interrupt handler: whether TCON-TV1 started a
			// vertical blank; clears its flag if so.
			bool				AcknowledgeBlank();
			void				DumpState(const char* when);

			// lab access to the registers of the mapped blocks
			status_t			DebugRegister(phys_addr_t address,
									uint32& value, bool write);

			// the fastest pixel clock (kHz) the sink's link carries at the
			// rates this driver trains (HBR; HBR2 is untried)
			uint32				MaxPixelClock() const;

			bool				Enabled() const { return fEnabled; }
			uint32				LinkRate() const { return fLinkRate; }
			uint32				Lanes() const { return fLanes; }

private:
			struct Mapping {
				phys_addr_t	physical;
				size_t		size;
				area_id		area;
				uint8*		address;
			};

			status_t			_Map(phys_addr_t physical, size_t size);
			volatile void*		_Address(phys_addr_t physical) const;
			uint32				_Read(phys_addr_t physical) const;
			void				_Write(phys_addr_t physical, uint32 value);
			void				_Update(phys_addr_t physical, uint32 clear,
									uint32 set);
			uint16				_Read16(phys_addr_t physical) const;
			void				_Write16(phys_addr_t physical, uint16 value);
			void				_SetKeyedGate(phys_addr_t physical,
									uint32 key, uint32 bit);

			status_t			_PowerOn(uint32 domain);
			status_t			_EnableDisplayEngineClocks();
			status_t			_SetPixelClock(uint32 kHz);
			status_t			_InitAuxPhy();
			void				_InitController();

			void				_PhyAssert();
			status_t			_PhyDeassert();
			status_t			_PhyConfigure(uint32 rate);
			void				_PhySetLevels(uint32 rate,
									const uint8* swing,
									const uint8* preEmphasis);

			status_t			_AuxWaitReply();
			status_t			_AuxTransfer(uint32 request, uint32 address,
									uint8* data, size_t length);
			status_t			_DpcdRead(uint32 address, uint8* data,
									size_t length);
			status_t			_DpcdWrite(uint32 address, const uint8* data,
									size_t length);
			status_t			_ReadEdid(uint8* edid, uint32* length);

			status_t			_TrainLink(uint32 rate);
			void				_SetVideo(const display_timing& timing,
									uint32 rate);
			void				_InitDisplayEngine(const display_timing& timing,
									phys_addr_t address, uint32 bytesPerRow);
			void				_InitTcon(const display_timing& timing);

			Mapping				fMappings[16];
			int32				fMappingCount;
			volatile uint32*	fTconInterrupts;
			bool				fInitialized;
			bool				fEnabled;
			bool				fFlipped;
			uint8				fDpcd[16];
			uint32				fLinkRate;		// 10 kHz units: 162000...
			uint32				fLanes;
			uint16				fWidth;
			uint16				fHeight;
};


}	// namespace sunxi


#endif	// SUNXI_DISPLAY_DISPLAY_PIPE_H
