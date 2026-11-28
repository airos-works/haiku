/*	
 * Copyright (c) 2000-2008, Ingo Weinhold <ingo_weinhold@gmx.de>,
 * Copyright (c) 2000-2008, Stephan Aßmus <superstippi@gmx.de>,
 * All Rights Reserved. Distributed under the terms of the MIT license.
 */
#include <Message.h>

#include "MessageEvent.h"


MessageEvent::MessageEvent(bigtime_t time, BHandler* handler, uint32 command)
	: Event(time),
	  AbstractLOAdapter(handler),
	  fMessage(command)
{
}


MessageEvent::MessageEvent(bigtime_t time, BHandler* handler,
		const BMessage& message)
	: Event(time),
	  AbstractLOAdapter(handler),
	  fMessage(message)
{
}


MessageEvent::MessageEvent(bigtime_t time, const BMessenger& messenger)
	: Event(time),
	  AbstractLOAdapter(messenger)
{
}


MessageEvent::MessageEvent(bigtime_t time, const BMessenger& messenger,
		const BMessage& message)
	: Event(time),
	  AbstractLOAdapter(messenger),
	  fMessage(message)
{
}


MessageEvent::~MessageEvent()
{
}


void
MessageEvent::Execute()
{
	BMessage msg(fMessage);
	msg.AddInt64("time", Time());
	DeliverMessage(msg);
}

