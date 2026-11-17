/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TEST_STUB_MESSAGE_H
#define _TEST_STUB_MESSAGE_H

#include <SupportDefs.h>

class BMessage {
public:
	BMessage(uint32 what) : what(what) {}
	status_t AddInt32(const char*, int32) { return B_OK; }
	status_t AddString(const char*, const char*) { return B_OK; }

	uint32 what;
};

#endif
