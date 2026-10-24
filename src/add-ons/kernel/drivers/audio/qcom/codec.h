/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef QCOM_AUDIO_CODEC_H
#define QCOM_AUDIO_CODEC_H


#include <OS.h>


namespace qcom_audio {


class Dsp;


/*!	A data port's transport, as the device tree gives it per port of a
	bus (qcom,ports-*; 0xff: not used). */
struct SoundWirePort {
	uint8	sampleInterval;		// low byte, less one
	uint8	offset1;
	uint8	offset2;
	uint8	hstart;
	uint8	hstop;
	uint8	wordLength;			// bits less one
	uint8	blockPackingMode;
	uint8	lane;
	uint8	blockGroupCount;
};


/*!	A SoundWire bus controller of the LPASS ("qcom,soundwire-v1.6.0",
	Linux' drivers/soundwire/qcom.c): its frames, the devices the hardware
	enumerator numbered, register access to them through the command FIFOs.
*/
class SoundWire {
public:
								SoundWire(const char* label);
								~SoundWire();

			status_t			Start(phys_addr_t address);
			void				Stop();

			//! The device with this unique ID, once it attached; 0 if none.
			uint32				FindDevice(uint32 uniqueID,
									bigtime_t timeout);
			int					Read(uint32 device, uint16 reg);
			status_t			Write(uint32 device, uint16 reg, uint8 value);

			//! Runs ports (1-based, each with its channel mask) between the
			//! controller and \a device's ports of the same numbers, as
			//! sdw_prepare_stream() and sdw_enable_stream() do; with
			//! \a enable false, stops them.
			status_t			SetPorts(uint32 device,
									const SoundWirePort* ports,
									const uint8* channelMasks,
									uint32 portCount, bool enable);

private:
			status_t			_Broadcast(uint16 reg, uint8 value);
			status_t			_BankSwitch();
			void				_ProgramPorts(uint32 device,
									const SoundWirePort* ports,
									const uint8* channelMasks,
									uint32 portCount, uint32 bank,
									bool enable);

			uint32				_Read(uint32 offset) const;
			void				_Write(uint32 offset, uint32 value);

			const char*			fLabel;
			area_id				fArea;
			volatile uint8*		fBase;
			uint32				fCommandID;
};


/*!	The board's analog audio: the LPASS RX, TX and VA macros, the two
	SoundWire buses and the WCD9385 codec on them (its RX device on the RX
	bus, its TX device, through which its registers are reached, on the TX
	bus).
*/
class Codec {
public:
								Codec();
								~Codec();

			status_t			PowerUp(Dsp& dsp);
			void				PowerDown();

			//! The RX macro's headphone path: the codec DMA's RX0 and RX1
			//! through interpolators 0 and 1 (HPHL, HPHR) with class H.
			status_t			EnableHeadphonePath(uint32 sampleRate);
			void				DisableHeadphonePath();

			//! The WCD9385's side: its RX device's HPH and class H ports on
			//! the RX bus, then its headphone DACs and amplifiers (class H,
			//! HiFi), at \a gain (0: 0 dB, to 20: -30 dB in 1.5 dB steps).
			status_t			EnableHeadphones(uint8 gain);
			void				DisableHeadphones();

			//! The WCD9385's registers (WCD938X_* in Linux' wcd938x.h).
			int					Read(uint16 reg);
			status_t			Write(uint16 reg, uint8 value);
			status_t			Update(uint16 reg, uint8 mask, uint8 value);

private:
			status_t			_Map(const char* name, phys_addr_t address,
									size_t size, area_id& area,
									volatile uint8*& base);
			void				_ClockMacros();
			void				_SetPins(uint32 clockPin, uint32 firstDataPin);
			void				_ResetCodec();
			void				_InitCodec();
			void				_ClassH(bool enable);
			uint32				_RxPath(uint32 interpolator,
									uint32 offset) const;
			void				_RxUpdate(uint32 offset, uint32 mask,
									uint32 value);

			area_id				fRxArea;
			volatile uint8*		fRx;
			area_id				fTxArea;
			volatile uint8*		fTx;
			area_id				fVaArea;
			volatile uint8*		fVa;
			SoundWire			fRxBus;
			SoundWire			fTxBus;
			uint32				fRxDevice;
			uint32				fTxDevice;
			uint32				fRxStride;		// the interpolators' spacing
			uint32				fRxStride2;
			bool				fPowered;
			bool				fHeadphones;
};


}	// namespace qcom_audio


#endif	// QCOM_AUDIO_CODEC_H
