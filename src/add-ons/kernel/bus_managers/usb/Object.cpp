/*
 * Copyright 2006-2020, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Michael Lotz <mmlr@mlotz.ch>
 */

#include "usb_private.h"


Object::Object(Stack *stack, BusManager *bus)
	:	fParent(NULL),
		fBusManager(bus),
		fStack(stack),
		fUSBID(fStack->GetUSBID(this))
{
}


Object::Object(Object *parent)
	:	fParent(parent),
		fBusManager(parent->GetBusManager()),
		fStack(parent->GetStack()),
		fUSBID(fStack->GetUSBID(this))
{
}


Object::~Object()
{
	PutUSBID();
}


void
Object::PutUSBID(bool waitForIdle)
{
	if (fUSBID != UINT32_MAX) {
		fStack->PutUSBID(this);
		fUSBID = UINT32_MAX;
	}

	if (waitForIdle)
		WaitForIdle();
}


void
Object::WaitForIdle()
{
	// The host controller can retire an isochronous transfer several frames
	// after it has been canceled. Two milliseconds is not enough for that path.
	int32 retries = 5000;
	while (CountReferences() != 1 && retries-- > 0)
		snooze(100);

	// Test the reference count, not the retry counter. `retries--` is a post
	// decrement that is skipped entirely when the && short-circuits, so an
	// object that went idle on the final retry leaves retries at 0 and the
	// old condition panicked on it despite the wait having succeeded.
	if (CountReferences() != 1)
		panic("USB object did not become idle! @! calling -m usb");
}


status_t
Object::SetFeature(uint16 selector)
{
	// to be implemented in subclasses
	TRACE_ERROR("set feature called\n");
	return B_ERROR;
}


status_t
Object::ClearFeature(uint16 selector)
{
	// to be implemented in subclasses
	TRACE_ERROR("clear feature called\n");
	return B_ERROR;
}


status_t
Object::GetStatus(uint16 *status)
{
	// to be implemented in subclasses
	TRACE_ERROR("get status called\n");
	return B_ERROR;
}
