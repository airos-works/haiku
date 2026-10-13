/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef BLUETOOTH_AUDIO_ADD_ON_H
#define BLUETOOTH_AUDIO_ADD_ON_H


#include <Locker.h>
#include <MediaAddOn.h>
#include <MediaNode.h>
#include <String.h>

#include <bluetooth/bluetooth.h>


class BluetoothAudioNode;
class SettingsWatcher;


// Publishes the Bluetooth speaker or headset chosen in the Bluetooth
// preferences as an audio output. The choice lives in a settings file; the
// add-on follows it and tells the media server when it changes.
class BluetoothAudioAddOn : public BMediaAddOn {
public:
								BluetoothAudioAddOn(image_id image);
	virtual						~BluetoothAudioAddOn();

	virtual	status_t			InitCheck(const char** _failureText);
	virtual	int32				CountFlavors();
	virtual	status_t			GetFlavorAt(int32 index,
									const flavor_info** _info);
	virtual	BMediaNode*			InstantiateNodeFor(const flavor_info* info,
									BMessage* config, status_t* _error);
	virtual	status_t			GetConfigurationFor(BMediaNode* node,
									BMessage* message);
	virtual	bool				WantsAutoStart();
	virtual	status_t			AutoStart(int index, BMediaNode** _node,
									int32* _internalID, bool* _hasMore);

			void				SettingsChanged();
			void				NodeDeleted(BluetoothAudioNode* node);
			void				MakeDefaultOutput();

private:
			bool				_ReadSettings(bdaddr_t& address,
									BString& name);
			void				_MakeDefaultOutput();
			void				_RestoreOutput();

private:
			BLocker				fLock;
			flavor_info			fFlavor;
			media_format		fFormat;
			BString				fFlavorName;
			bool				fHasSink;
			bdaddr_t			fSink;
			SettingsWatcher*	fWatcher;
			BluetoothAudioNode*	fNode;
			media_node			fPreviousOutput;
			bool				fStarted;
			bool				fPinned;
			bool				fConnectOnCreate;
};


#endif	// BLUETOOTH_AUDIO_ADD_ON_H
