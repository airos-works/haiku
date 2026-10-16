/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MEDIA_MESSAGES_H
#define MEDIA_MESSAGES_H


#include <SupportDefs.h>


// sent to the window
enum {
	kMsgShowPage			= 'mSPg',	// "key" string
	kMsgSetDefault			= 'mSDf',	// "role", "addon", "flavor" int32
	kMsgSetOutputChannel	= 'mSCh',	// "input" raw media_input
	kMsgRestartServices		= 'mRst',
	kMsgStartServices		= 'mStr',
	kMsgSidebarSelection	= 'mSel',
	kMsgRescan				= 'mRsc',
	kMsgRestartProgress		= 'mRpr',	// "stage" int32, "text" string
	kMsgRestartDone			= 'mRdn'
};


#endif	// MEDIA_MESSAGES_H
