/*
 * Copyright 2008-09, Oliver Ruiz Dorantes, <oliver.ruiz.dorantes_at_gmail.com>
 * Copyright 2026, Haiku, Inc.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef BLUETOOTH_WINDOW_H
#define BLUETOOTH_WINDOW_H


#include <Messenger.h>
#include <String.h>
#include <Window.h>

#include <map>
#include <vector>

#include "DeviceModel.h"
#include "NearbyScanner.h"


class BButton;
class BCheckBox;
class BMenuField;
class BPopUpMenu;
class BStringView;
class BTextView;
class DeviceListView;
class HeaderIconView;

namespace Bluetooth {
class LocalDevice;
}


class BluetoothWindow : public BWindow, private ScannerListener {
public:
								BluetoothWindow();
	virtual						~BluetoothWindow();

	virtual	bool				QuitRequested();
	virtual	void				MessageReceived(BMessage* message);
	virtual	void				Minimize(bool minimize);

private:
			struct Adapter {
				Bluetooth::LocalDevice*	device;
				int32			id;
				BString			name;
				uint8			address[6];
			};

			enum operation_type {
				OPERATION_NONE,
				OPERATION_LE_PAIR,
				OPERATION_CLASSIC_CONNECT
			};

			enum operation_state {
				OPERATION_IDLE,
				OPERATION_WAITING,
					// for the scanner to release the controller
				OPERATION_RUNNING
			};

	// ScannerListener
	virtual	void				LEAdvertisement(const uint8 address[6],
									uint8 addressType, uint8 eventType,
									const LEAdvertisingData& advertising,
									bool hasRSSI, int8 rssi);
	virtual	void				ClassicDeviceFound(const uint8 address[6],
									uint32 deviceClass, const BString& name,
									uint8 pageRepetitionMode,
									uint16 clockOffset);
	virtual	void				ClassicInquiryFinished();
	virtual	void				ScannerStatusChanged();
	virtual	void				ScannerPaused();

			// Service and adapters
			void				_ServerStateChanged(bool running);
			void				_StartServices();
			void				_StopServices();
			void				_ProbeAdapters();
			void				_AdaptersProbed(BMessage* message);
			void				_SelectAdapter(int32 index);
			const Adapter*		_CurrentAdapter() const;
			int32				_HCIID() const;
			void				_UpdateHeader();
			void				_UpdateScanning();

			// My devices
			void				_LoadPaired();
			void				_PairedLoaded(BMessage* message);
			void				_ClearPaired();
			void				_UpdatePairedItem(PairedDevice* device);
			void				_UpdatePairedButtons();
			PairedDevice*		_SelectedPaired() const;
			void				_ConnectPaired();
			void				_DisconnectPaired();
			void				_ReleaseAudioSink(const uint8 address[6]);
			void				_RemovePaired();
			void				_ConnectionEvent(BMessage* message);
			void				_LearnName(device_key key,
									const BString& name, device_kind kind);

			// Nearby devices
			NearbyDevice*		_Nearby(const uint8 address[6]);
			void				_UpdateNearbyItem(NearbyDevice* device);
			void				_LinkCompanions(NearbyDevice* device);
			void				_UpdateAllNearby();
			void				_ClearNearby();
			void				_ExpireNearby();
			void				_UpdateNearbyButtons();
			NearbyDevice*		_SelectedNearby() const;
			void				_UpdateScanStatus();
			void				_StartNameLookup();
			void				_NameResult(BMessage* message);

			// Connecting and pairing
			void				_ConnectNearby();
			void				_BeginOperation(operation_type type,
									device_key key, const BString& name,
									device_kind kind);
			void				_RunOperation();
			void				_PairingProgress(BMessage* message);
			void				_LEPairDone(BMessage* message);
			void				_ClassicConnectSent(BMessage* message);
			void				_ClassicConnectFinished(bool success,
									uint8 status);
			void				_EndOperation();
			BString				_OperationProgressText() const;

			void				_ShowStatus(const BString& title,
									const BString& text, bool showLog);
			void				_HideStatus();

			void				_OpenAdvanced();

			// Header
			HeaderIconView*		fIconView;
			BStringView*		fTitleView;
			BStringView*		fSubtitleView;
			BPopUpMenu*			fAdapterMenu;
			BMenuField*			fAdapterField;
			BButton*			fServiceButton;

			// My devices
			DeviceListView*		fPairedList;
			BButton*			fConnectButton;
			BButton*			fDisconnectButton;
			BButton*			fRemoveButton;

			// Nearby devices
			DeviceListView*		fNearbyList;
			BStringView*		fScanStatusView;
			BCheckBox*			fShowUnnamed;
			BButton*			fPairButton;

			// Status area
			BView*				fStatusView;
			BStringView*		fStatusTitle;
			BTextView*			fStatusText;
			BButton*			fShowLogButton;
			BButton*			fDismissButton;

			NearbyScanner*		fScanner;
			BMessenger			fAdvancedWindow;
			BMessageRunner*		fTickRunner;

			bool				fServerRunning;
			bool				fRestartPending;
			bool				fProbing;
			bigtime_t			fProbeStarted;
			int32				fProbeGeneration;
			std::vector<Adapter> fAdapters;
			int32				fAdapterIndex;

			std::map<device_key, PairedDevice*> fPaired;
			int32				fPairedGeneration;

			std::map<device_key, NearbyDevice*> fNearby;
			bigtime_t			fSearchingSince;

			bool				fNameLookupActive;
			bigtime_t			fNameLookupStarted;

			DeviceNameCache		fNames;

			operation_type		fOperation;
			operation_state		fOperationState;
			device_key			fOperationKey;
			BString				fOperationName;
			device_kind			fOperationKind;
			bool				fOperationFromPaired;
			uint8				fOperationAddressType;
			uint32				fOperationClass;
			uint8				fOperationRepetitionMode;
			uint16				fOperationClockOffset;
			int32				fOperationStage;
			BString				fOperationDetail;
			bigtime_t			fOperationStarted;

			bool				fMinimized;
};


#endif	// BLUETOOTH_WINDOW_H
