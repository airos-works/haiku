/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef NEARBY_SCANNER_H
#define NEARBY_SCANNER_H


#include <Handler.h>
#include <String.h>

#include "LEAdvertisingData.h"


namespace Bluetooth {
class DiscoveryAgent;
class DiscoveryListener;
class LocalDevice;
}


class ScannerListener {
public:
	virtual						~ScannerListener() {}

	virtual	void				LEAdvertisement(const uint8 address[6],
									uint8 addressType, uint8 eventType,
									const LEAdvertisingData& advertising,
									bool hasRSSI, int8 rssi) = 0;
	// The name is the one included in the inquiry result, if any.
	virtual	void				ClassicDeviceFound(const uint8 address[6],
									uint32 deviceClass, const BString& name,
									uint8 pageRepetitionMode,
									uint16 clockOffset) = 0;
	virtual	void				ClassicInquiryFinished() = 0;
	virtual	void				ScannerStatusChanged() = 0;
	// Everything is stopped after Pause(); the controller is free for a
	// connection.
	virtual	void				ScannerPaused() = 0;
};


// Keeps discovery going while the window is visible: a continuous LE scan,
// with a Classic inquiry every 30 seconds. The scanner stops cleanly for
// pairing (Pause/Resume) and retries when the server is busy or fails.
class NearbyScanner : public BHandler {
public:
								NearbyScanner(ScannerListener* listener);
	virtual						~NearbyScanner();

			void				SetAdapter(Bluetooth::LocalDevice* device);

			void				Start();
			void				Stop();
			bool				IsRunning() const { return fEnabled; }

			void				Pause();
			void				Resume();
			void				HoldInquiry(bigtime_t duration);
									// no Classic inquiry for this long
			bool				IsPaused() const { return fPaused; }

			bool				IsSearching() const;
			BString				StatusText() const;

	virtual	void				MessageReceived(BMessage* message);

private:
			enum le_state {
				LE_IDLE,
				LE_REQUESTED,
				LE_STARTING,
				LE_ACTIVE,
				LE_STOPPING
			};

			void				_Begin();
			void				_Halt();
			void				_StartLE();
			void				_StopLE();
			void				_SendLEStop();
			void				_LEStopped();
			void				_LEFailed(int32 status, bigtime_t retryDelay);
			void				_ScheduleInquiry(bigtime_t delay);
			void				_StartInquiry();
			void				_EndInquiry();
			void				_NotifyPausedIfIdle();
			void				_SetStatus(int32 status);
			void				_Timer(uint32 what, int32 generation,
									bigtime_t delay);
			int32				_HCIID() const;

			ScannerListener*	fListener;
			Bluetooth::LocalDevice* fDevice;
			Bluetooth::DiscoveryAgent* fAgent;
			Bluetooth::DiscoveryListener* fInquiryListener;

			bool				fEnabled;
			bool				fPaused;
			bool				fPauseNotified;
			le_state			fLEState;
			int32				fLEGeneration;
			bool				fInquiryActive;
			bigtime_t			fInquiryNotBefore;
			int32				fInquiryGeneration;
			int32				fFailures;
			int32				fStatus;
};


#endif	// NEARBY_SCANNER_H
