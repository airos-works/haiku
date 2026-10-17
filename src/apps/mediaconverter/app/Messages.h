/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MESSAGES_H
#define MESSAGES_H


enum {
	kMsgAddFiles				= 'adfl',
	kMsgRemoveFiles				= 'rmfl',
	kMsgClearFinished			= 'clfn',
	kMsgQueueSelection			= 'qsel',
	kMsgQueueInvoked			= 'qinv',

	kMsgSettingsChanged			= 'stch',
	kMsgPresetChosen			= 'prst',
	kMsgChooseFolder			= 'chfd',
	kMsgFolderChosen			= 'fdch',
	kMsgSameFolder				= 'smfd',

	kMsgConvert					= 'cnvt',
	kMsgCpuWarningAnswer		= 'cpwa',
	kMsgStop					= 'stop',
	kMsgShowHardware			= 'shhw',
	kMsgToggleCpuWarning		= 'tgcw',
	kMsgShowInTracker			= 'shtr',
	kMsgPlayOutput				= 'plyo',
	kMsgDiscTitleChosen			= 'dttl',

	// from the worker thread
	kMsgJobStarted				= 'jbst',
	kMsgJobProgress				= 'jbpr',
	kMsgJobFinished				= 'jbfn',
	kMsgQueueFinished			= 'qfin',
	kMsgSourceProbed			= 'sprb',
	kMsgPreviewReady			= 'prvw',
	kMsgHardwareTested			= 'hwts',
};


#endif	// MESSAGES_H
