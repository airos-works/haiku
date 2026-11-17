/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _TEST_STUB_MESSENGER_H
#define _TEST_STUB_MESSENGER_H

#include <Message.h>

class BMessenger {
public:
	bool IsValid() const { return false; }
	status_t SendMessage(BMessage*) const { return B_OK; }
};

#endif
