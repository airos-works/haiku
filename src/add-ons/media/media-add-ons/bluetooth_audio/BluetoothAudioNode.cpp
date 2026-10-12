/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "BluetoothAudioNode.h"

#include <new>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <Autolock.h>
#include <Buffer.h>
#include <TimeSource.h>

#include <bluetooth/bdaddrUtils.h>

#include "BluetoothAudioAddOn.h"


using namespace Bluetooth;


static const uint32 kSampleRate = 44100;
static const uint32 kChannels = 2;
static const size_t kRingFrames = kSampleRate;
	// one second
static const bigtime_t kProcessingLatency = 30000;
static const bigtime_t kDefaultSinkDelay = 150000;
static const bigtime_t kSuspendAfter = 5000000;
static const bigtime_t kDisconnectAfter = 60000000;
static const bigtime_t kRetryAfter = 10000000;
static const int16 kSilence = 8;
	// samples this small do not count as sound


BluetoothAudioNode::BluetoothAudioNode(BluetoothAudioAddOn* addOn,
	const char* name)
	:
	BMediaNode(name),
	BBufferConsumer(B_MEDIA_RAW_AUDIO),
	BMediaEventLooper(),
	fAddOn(addOn),
	fLatency(kProcessingLatency + kDefaultSinkDelay),
	fLock("bluetooth audio"),
	fSender(-1),
	fQuit(false),
	fRingFrames(kRingFrames),
	fRingRead(0),
	fRingFill(0),
	fSource(NULL),
	fLastConnectAttempt(0),
	fLastSound(0),
	fConnectFailed(false),
	fSinkChanged(false),
	fConnectNow(true)
{
	fRing = (int16*)malloc(fRingFrames * kChannels * sizeof(int16));
	fDataSem = create_sem(0, "bluetooth audio data");

	fInput.source = media_source::null;
	GetFormat(fInput.format);
	strlcpy(fInput.name, "Bluetooth audio", sizeof(fInput.name));
}


BluetoothAudioNode::~BluetoothAudioNode()
{
	fAddOn->NodeDeleted(this);
	Quit();

	fQuit = true;
	release_sem(fDataSem);
	if (fSender >= 0) {
		status_t result;
		wait_for_thread(fSender, &result);
	}
	_CloseStream();
	delete_sem(fDataSem);
	free(fRing);
}


/*static*/ void
BluetoothAudioNode::GetFormat(media_format& format)
{
	format.Clear();
	format.type = B_MEDIA_RAW_AUDIO;
	format.u.raw_audio.frame_rate = kSampleRate;
	format.u.raw_audio.channel_count = kChannels;
	format.u.raw_audio.format = media_raw_audio_format::B_AUDIO_SHORT;
	format.u.raw_audio.byte_order = B_MEDIA_HOST_ENDIAN;
	format.u.raw_audio.buffer_size = 2048 * kChannels * sizeof(int16);
}


void
BluetoothAudioNode::ConnectNow()
{
	fConnectNow = true;
	release_sem(fDataSem);
}


void
BluetoothAudioNode::SinkChanged()
{
	fSinkChanged = true;
	release_sem(fDataSem);
}


BMediaAddOn*
BluetoothAudioNode::AddOn(int32* _internalID) const
{
	if (_internalID != NULL)
		*_internalID = 0;
	return fAddOn;
}


status_t
BluetoothAudioNode::HandleMessage(int32 message, const void* data, size_t size)
{
	if (BBufferConsumer::HandleMessage(message, data, size) == B_OK)
		return B_OK;
	return BMediaNode::HandleMessage(message, data, size);
}


void
BluetoothAudioNode::NodeRegistered()
{
	fInput.destination.port = ControlPort();
	fInput.destination.id = 0;
	fInput.node = Node();

	SetPriority(B_REAL_TIME_PRIORITY);
	SetEventLatency(fLatency);
	Run();

	fSender = spawn_thread(&_SenderEntry, "bluetooth audio sender",
		B_URGENT_PRIORITY, this);
	if (fSender >= 0)
		resume_thread(fSender);
}


status_t
BluetoothAudioNode::AcceptFormat(const media_destination& destination,
	media_format* format)
{
	if (destination != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;
	if (format->type == B_MEDIA_NO_TYPE)
		format->type = B_MEDIA_RAW_AUDIO;
	if (format->type != B_MEDIA_RAW_AUDIO)
		return B_MEDIA_BAD_FORMAT;

	media_raw_audio_format& raw = format->u.raw_audio;
	const media_raw_audio_format& wildcard
		= media_raw_audio_format::wildcard;
	if ((raw.frame_rate != wildcard.frame_rate && raw.frame_rate != kSampleRate)
		|| (raw.channel_count != wildcard.channel_count
			&& raw.channel_count != kChannels)
		|| (raw.format != wildcard.format
			&& raw.format != media_raw_audio_format::B_AUDIO_SHORT)
		|| (raw.byte_order != wildcard.byte_order
			&& raw.byte_order != B_MEDIA_HOST_ENDIAN)) {
		GetFormat(*format);
		return B_MEDIA_BAD_FORMAT;
	}

	raw.frame_rate = kSampleRate;
	raw.channel_count = kChannels;
	raw.format = media_raw_audio_format::B_AUDIO_SHORT;
	raw.byte_order = B_MEDIA_HOST_ENDIAN;
	if (raw.buffer_size == wildcard.buffer_size)
		raw.buffer_size = fInput.format.u.raw_audio.buffer_size;
	return B_OK;
}


status_t
BluetoothAudioNode::GetNextInput(int32* cookie, media_input* _input)
{
	if (*cookie != 0)
		return B_BAD_INDEX;
	*_input = fInput;
	(*cookie)++;
	return B_OK;
}


void
BluetoothAudioNode::DisposeInputCookie(int32 cookie)
{
}


void
BluetoothAudioNode::BufferReceived(BBuffer* buffer)
{
	if (buffer->Header()->destination != fInput.destination.id) {
		buffer->Recycle();
		return;
	}

	media_timed_event event(buffer->Header()->start_time,
		BTimedEventQueue::B_HANDLE_BUFFER, buffer,
		BTimedEventQueue::B_RECYCLE_BUFFER);
	if (EventQueue()->AddEvent(event) != B_OK)
		buffer->Recycle();
}


void
BluetoothAudioNode::ProducerDataStatus(const media_destination& destination,
	int32 status, bigtime_t performanceTime)
{
}


status_t
BluetoothAudioNode::GetLatencyFor(const media_destination& destination,
	bigtime_t* _latency, media_node_id* _timeSource)
{
	if (destination != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;
	*_latency = EventLatency() + SchedulingLatency();
	*_timeSource = TimeSource()->ID();
	return B_OK;
}


status_t
BluetoothAudioNode::Connected(const media_source& producer,
	const media_destination& destination, const media_format& format,
	media_input* _input)
{
	if (destination != fInput.destination)
		return B_MEDIA_BAD_DESTINATION;

	fInput.source = producer;
	fInput.format = format;
	*_input = fInput;
	return B_OK;
}


void
BluetoothAudioNode::Disconnected(const media_source& producer,
	const media_destination& destination)
{
	if (destination != fInput.destination || producer != fInput.source)
		return;
	fInput.source = media_source::null;
	GetFormat(fInput.format);
}


status_t
BluetoothAudioNode::FormatChanged(const media_source& producer,
	const media_destination& consumer, int32 changeTag,
	const media_format& format)
{
	return B_MEDIA_BAD_FORMAT;
}


void
BluetoothAudioNode::HandleEvent(const media_timed_event* event,
	bigtime_t lateness, bool realTimeEvent)
{
	if (event->type != BTimedEventQueue::B_HANDLE_BUFFER)
		return;

	// The mixer starts and stops us with itself; whatever it sends is
	// meant to be heard.
	BBuffer* buffer = (BBuffer*)event->pointer;
	const size_t frames = buffer->SizeUsed() / (kChannels * sizeof(int16));
	_Queue((const int16*)buffer->Data(), frames);
	buffer->Recycle();
}


void
BluetoothAudioNode::_Queue(const int16* samples, size_t frames)
{
	bool sound = false;
	for (size_t i = 0; i < frames * kChannels; i++) {
		if (samples[i] > kSilence || samples[i] < -kSilence) {
			sound = true;
			break;
		}
	}

	BAutolock _(fLock);
	if (sound)
		fLastSound = system_time();
	for (size_t i = 0; i < frames; i++) {
		size_t write = (fRingRead + fRingFill) % fRingFrames;
		memcpy(fRing + write * kChannels, samples + i * kChannels,
			kChannels * sizeof(int16));
		if (fRingFill < fRingFrames)
			fRingFill++;
		else
			fRingRead = (fRingRead + 1) % fRingFrames;
				// a full ring drops the oldest audio
	}
	release_sem(fDataSem);
}


/*static*/ status_t
BluetoothAudioNode::_SenderEntry(void* cookie)
{
	((BluetoothAudioNode*)cookie)->_Sender();
	return B_OK;
}


void
BluetoothAudioNode::_CloseStream()
{
	A2dpSource* source = fSource;
	fSource = NULL;
	if (source != NULL) {
		source->Disconnect();
		delete source;
	}
}


/*!	Connects to the speaker of the setting and opens a stream (which can
	take seconds, and may need the user to confirm pairing).
*/
bool
BluetoothAudioNode::_Connect()
{
	const bigtime_t now = system_time();
	{
		if (fConnectFailed && now - fLastConnectAttempt < kRetryAfter)
			return false;
		fLastConnectAttempt = now;
		_CloseStream();

		bdaddr_t address;
		BString name;
		if (GetAudioSink(address, name) != B_OK) {
			fConnectFailed = true;
			return false;
		}

		fSource = new(std::nothrow) A2dpSource(address);
		if (fSource == NULL)
			return false;
		fSource->SetPreferredSampleRate(kSampleRate);
		status_t status = fSource->Connect();
		if (status == B_OK
			&& fSource->Configuration().sampleRate != kSampleRate) {
			syslog(LOG_ERR, "bluetooth audio: %s only takes %" B_PRIu32
				" Hz\n", name.String(), fSource->Configuration().sampleRate);
			status = B_NOT_SUPPORTED;
		}
		if (status == B_OK)
			status = fEncoder.SetTo(fSource->Configuration());
		if (status != B_OK) {
			syslog(LOG_ERR, "bluetooth audio: cannot connect to %s: %s\n",
				name.String(), fSource->LastError());
			_CloseStream();
			fConnectFailed = true;
			return false;
		}
		fConnectFailed = false;
		syslog(LOG_INFO, "bluetooth audio: connected to %s, %" B_PRIu32
			" kbit/s SBC\n", name.String(),
			fSource->Configuration().BitRate() / 1000);

		const bigtime_t delay = fSource->SinkDelay();
		const bigtime_t latency = kProcessingLatency
			+ (delay > 0 ? delay : kDefaultSinkDelay);
		if (latency != fLatency) {
			fLatency = latency;
			SetEventLatency(latency);
			if (fInput.source != media_source::null)
				SendLatencyChange(fInput.source, fInput.destination,
					EventLatency() + SchedulingLatency());
		}
	}
	return true;
}


/*!	Gets the stream going when there is sound to play. Right after the
	setting changed, connects without waiting for sound, so that the
	speaker shows it is in use.
*/
bool
BluetoothAudioNode::_EnsureStream()
{
	if (fSource != NULL && fSource->IsStreaming())
		return true;

	bigtime_t lastSound;
	{
		BAutolock _(fLock);
		lastSound = fLastSound;
	}
	const bool sound = system_time() - lastSound <= 1000000;
	if (!sound && !fConnectNow)
		return false;

	if ((fSource == NULL || !fSource->IsOpen()) && !_Connect()) {
		fConnectNow = false;
		return false;
	}
	fConnectNow = false;
	if (!sound)
		return false;

	if (fSource->Start() != B_OK) {
		syslog(LOG_ERR, "bluetooth audio: cannot start the stream: %s\n",
			fSource->LastError());
		_CloseStream();
		return false;
	}

	fEncoder.Reset();
	{
		// Audio that piled up while connecting is late now; keep a little
		// of it to fill the sink's buffer.
		BAutolock _(fLock);
		const size_t keep = kSampleRate / 10;
		if (fRingFill > keep) {
			fRingRead = (fRingRead + fRingFill - keep) % fRingFrames;
			fRingFill = keep;
		}
	}
	return true;
}


void
BluetoothAudioNode::_Sender()
{
	int16 frame[16 * 8 * 2];
	uint8 packet[15 * 520];

	while (!fQuit) {
		acquire_sem_etc(fDataSem, 1, B_RELATIVE_TIMEOUT, 200000);
		if (fQuit)
			break;

		if (fSinkChanged) {
			fSinkChanged = false;
			_CloseStream();
			fConnectFailed = false;
			fConnectNow = true;
		}

		const bigtime_t now = system_time();
		bigtime_t lastSound;
		{
			BAutolock _(fLock);
			lastSound = fLastSound;
		}

		if (fSource != NULL && now - lastSound > kDisconnectAfter) {
			syslog(LOG_INFO, "bluetooth audio: silent for a minute, "
				"letting the speaker go\n");
			_CloseStream();
		} else if (fSource != NULL && fSource->IsStreaming()
			&& now - lastSound > kSuspendAfter) {
			fSource->Suspend();
		}

		if (!_EnsureStream()) {
			BAutolock _(fLock);
			fRingRead = (fRingRead + fRingFill) % fRingFrames;
			fRingFill = 0;
			continue;
		}

		const SbcConfiguration& config = fSource->Configuration();
		const uint32 frameSamples = fEncoder.FrameSamples();
		const uint32 perPacket = fSource->MaxFramesPerPacket();
		const size_t frameSize = fEncoder.FrameSize();
		if (perPacket == 0)
			continue;

		while (!fQuit) {
			{
				BAutolock _(fLock);
				if (fRingFill < perPacket * frameSamples)
					break;
				for (uint32 f = 0; f < perPacket; f++) {
					for (uint32 i = 0; i < frameSamples; i++) {
						const int16* in = fRing + fRingRead * kChannels;
						if (config.Channels() == 1)
							frame[i] = (in[0] + in[1]) / 2;
						else {
							frame[2 * i] = in[0];
							frame[2 * i + 1] = in[1];
						}
						fRingRead = (fRingRead + 1) % fRingFrames;
						fRingFill--;
					}
					fEncoder.Encode(frame, packet + f * frameSize);
				}
			}

			status_t status = fSource->SendFrames(packet, perPacket);
			if (status == B_NOT_ALLOWED) {
				// The sink suspended or closed the stream; start again, or
				// connect again, with the next sound.
				if (!fSource->IsOpen()) {
					syslog(LOG_INFO, "bluetooth audio: the speaker closed "
						"the stream\n");
					_CloseStream();
				}
				break;
			}
		}
	}
}
