/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_AIRTIME_MESSAGES_H
#define AIRTIME_AIRTIME_MESSAGES_H


#include <SupportDefs.h>


namespace airtime {

enum {
	// Transport
	kMsgTogglePlay			= 'tply',
	kMsgPlay				= 'play',
	kMsgPause				= 'paus',
	kMsgGoToStart			= 'gsta',
	kMsgGoToEnd				= 'gend',
	kMsgScanPress			= 'scnp',	// int32 "direction"
	kMsgScanRelease			= 'scnr',	// int32 "direction", int64 "held"
	kMsgFastForward			= 'ffwd',
	kMsgRewind				= 'rwnd',
	kMsgSeekTo				= 'seek',	// int64 "time", bool "final"
	kMsgSeekBy				= 'skby',	// int64 "delta"
	kMsgStepForward			= 'stpf',
	kMsgStepBackward		= 'stpb',
	kMsgGoToTime			= 'gtim',
	kMsgGoToTimeDone		= 'gtdn',	// int64 "time"
	kMsgChapter				= 'chap',	// int32 "index"
	kMsgSetRate				= 'rate',	// double "rate"
	kMsgFaster				= 'fast',
	kMsgSlower				= 'slow',
	kMsgToggleLoop			= 'loop',
	kMsgJKL					= 'jkl ',	// int32 "key": -1 J, 0 K, 1 L

	// Sound
	kMsgSetVolume			= 'volm',	// float "volume", bool "final"
	kMsgVolumeUp			= 'vol+',
	kMsgVolumeDown			= 'vol-',
	kMsgToggleMute			= 'mute',
	kMsgSelectAudio			= 'saud',	// int32 "index"
	kMsgCycleAudio			= 'caud',

	// Subtitles
	kMsgSelectSubtitle		= 'ssub',	// int32 "index", -1 off
	kMsgCycleSubtitle		= 'csub',
	kMsgToggleCaptions		= 'cc  ',
	kMsgAddSubtitleFile		= 'asub',
	kMsgSubtitleSize		= 'ssiz',	// float "scale"

	// Window and view
	kMsgToggleFullScreen	= 'fuls',
	kMsgExitFullScreen		= 'exfs',
	kMsgSize				= 'size',	// float "scale", 0 = fit screen
	kMsgToggleOnTop			= 'ontp',
	kMsgToggleHardware		= 'hwdc',
	kMsgToggleTimeDisplay	= 'tdsp',
	kMsgToggleSnap			= 'snap',
	kMsgToggleDevicePixels	= 'dpix',
	kMsgShowInspector		= 'insp',
	kMsgPulse				= 'puls',
	kMsgOpenFile			= 'open',
	kMsgOpenRecent			= 'orec',
	kMsgShowInTracker		= 'trak',
	kMsgMakeDefault			= 'mdef',
	kMsgControlsActivity	= 'cact',	// the mouse is over the controller
	kMsgVideoClicked		= 'vclk',	// int32 "clicks"
	kMsgInspectorClosed		= 'iclo',
	kMsgSettingsChanged		= 'setc'
};

}	// namespace airtime

#endif	// AIRTIME_AIRTIME_MESSAGES_H
