/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */


#include "LabSpeaker.h"

#include <string.h>

#include <Buffer.h>
#include <TimeSource.h>


LabSpeaker::LabSpeaker(BMediaAddOn* addOn, int32 id)
	:
	BMediaNode("Virtual speaker (lab)"),
	BBufferConsumer(B_MEDIA_RAW_AUDIO),
	BMediaEventLooper(),
	fAddOn(addOn),
	fID(id)
{
	AddNodeKind(B_PHYSICAL_OUTPUT);
	_InitInput();
}


LabSpeaker::~LabSpeaker()
{
	Quit();
}


void
LabSpeaker::NodeRegistered()
{
	_InitInput();
	SetPriority(B_URGENT_DISPLAY_PRIORITY);
	Run();
}


BMediaAddOn*
LabSpeaker::AddOn(int32* _internalID) const
{
	if (_internalID != NULL)
		*_internalID = fID;
	return fAddOn;
}


status_t
LabSpeaker::AcceptFormat(const media_destination& dest, media_format* format)
{
	if (dest != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;
	if (format == NULL)
		return B_BAD_VALUE;
	if (format->type != B_MEDIA_RAW_AUDIO
		&& format->type != B_MEDIA_UNKNOWN_TYPE) {
		return B_MEDIA_BAD_FORMAT;
	}

	// whatever the producer likes, within raw audio
	format->type = B_MEDIA_RAW_AUDIO;
	if (format->u.raw_audio.frame_rate == 0)
		format->u.raw_audio.frame_rate = 48000;
	if (format->u.raw_audio.channel_count == 0)
		format->u.raw_audio.channel_count = 2;
	if (format->u.raw_audio.format == 0)
		format->u.raw_audio.format = media_raw_audio_format::B_AUDIO_FLOAT;
	if (format->u.raw_audio.byte_order == 0)
		format->u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
	if (format->u.raw_audio.buffer_size == 0) {
		format->u.raw_audio.buffer_size = 1024
			* (format->u.raw_audio.format
				& media_raw_audio_format::B_AUDIO_SIZE_MASK)
			* format->u.raw_audio.channel_count;
	}
	return B_OK;
}


status_t
LabSpeaker::GetNextInput(int32* cookie, media_input* _input)
{
	if (_input == NULL)
		return B_BAD_VALUE;
	if (++(*cookie) > 1)
		return B_BAD_INDEX;

	*_input = fInput;
	return B_OK;
}


void
LabSpeaker::DisposeInputCookie(int32 cookie)
{
}


void
LabSpeaker::BufferReceived(BBuffer* buffer)
{
	media_timed_event event(buffer->Header()->start_time,
		BTimedEventQueue::B_HANDLE_BUFFER, buffer,
		BTimedEventQueue::B_RECYCLE_BUFFER);
	EventQueue()->AddEvent(event);
}


void
LabSpeaker::ProducerDataStatus(const media_destination& forWhom, int32 status,
	bigtime_t atPerformanceTime)
{
}


status_t
LabSpeaker::GetLatencyFor(const media_destination& forWhom,
	bigtime_t* _latency, media_node_id* _timesource)
{
	if (forWhom != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;

	*_latency = 5000;
	*_timesource = TimeSource()->ID();
	return B_OK;
}


status_t
LabSpeaker::Connected(const media_source& producer,
	const media_destination& where, const media_format& withFormat,
	media_input* _input)
{
	if (where != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;

	SetEventLatency(5000);
	fInput.source = producer;
	fInput.format = withFormat;
	*_input = fInput;
	return B_OK;
}


void
LabSpeaker::Disconnected(const media_source& producer,
	const media_destination& where)
{
	_InitInput();
}


status_t
LabSpeaker::FormatChanged(const media_source& producer,
	const media_destination& consumer, int32 changeTag,
	const media_format& format)
{
	fInput.format = format;
	return B_OK;
}


status_t
LabSpeaker::HandleMessage(int32 message, const void* data, size_t size)
{
	if (BBufferConsumer::HandleMessage(message, data, size) == B_OK
		|| BMediaEventLooper::HandleMessage(message, data, size) == B_OK)
		return B_OK;

	return BMediaNode::HandleMessage(message, data, size);
}


void
LabSpeaker::HandleEvent(const media_timed_event* event, bigtime_t lateness,
	bool realTimeEvent)
{
	switch (event->type) {
		case BTimedEventQueue::B_HANDLE_BUFFER:
		{
			// the sound goes nowhere
			BBuffer* buffer = const_cast<BBuffer*>((BBuffer*)event->pointer);
			if (buffer != NULL)
				buffer->Recycle();
			break;
		}

		case BTimedEventQueue::B_STOP:
			EventQueue()->FlushEvents(0, BTimedEventQueue::B_ALWAYS, true,
				BTimedEventQueue::B_HANDLE_BUFFER);
			break;

		default:
			break;
	}
}


void
LabSpeaker::_InitInput()
{
	fInput.source = media_source::null;
	fInput.destination.port = ControlPort();
	fInput.destination.id = 0;
	fInput.node = Node();
	fInput.format.type = B_MEDIA_RAW_AUDIO;
	fInput.format.u.raw_audio = media_raw_audio_format::wildcard;
	strcpy(fInput.name, "output");
}
