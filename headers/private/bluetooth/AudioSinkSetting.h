/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _BLUETOOTH_AUDIO_SINK_SETTING_H_
#define _BLUETOOTH_AUDIO_SINK_SETTING_H_


#include <Path.h>
#include <String.h>

#include <bluetooth/bluetooth.h>


namespace Bluetooth {


// The speaker or headset the Bluetooth audio output plays to, kept in
// ~/config/settings/bluetooth_audio. SetAudioSink(NULL) forgets it.
status_t	GetAudioSink(bdaddr_t& address, BString& name);
status_t	SetAudioSink(const bdaddr_t* address, const char* name);
status_t	GetAudioSinkSettingsPath(BPath& path);


} // namespace Bluetooth


#endif	// _BLUETOOTH_AUDIO_SINK_SETTING_H_
