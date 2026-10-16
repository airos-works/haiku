/*
 *	Driver for USB Audio Device Class devices.
 *	Copyright (c) 2009-13 S.Zharski <imker@gmx.li>
 *	Distributed under the terms of the MIT license.
 */


#include "Stream.h"

#include <stdint.h>
#include <string.h>

#include <kernel.h>
#include <usb/USB_audio.h>

#include "Device.h"
#include "Driver.h"
#include "IsoPacketSizes.h"
#include "Settings.h"


// One isochronous packet per service interval of the endpoint: per 1 ms
// frame at full speed, per 125 us microframe (times 2^(bInterval - 1)) at
// high speed.
//
// A full-speed device behind a high-speed hub is scheduled on EHCI's
// 128-frame periodic list (siTDs), and so are a high-speed device's iTDs:
// kSamplesBufferCount buffers of this many milliseconds must fit in it with
// room for the scheduling threshold.
static const uint32 kMaxBufferMilliseconds = 48;

// The highest sampling rate offered for a USB Audio 2.0 device (they go up
// to 768 kHz, and the media node takes the highest it is offered).
static const uint32 kMaxUAC2Rate = 192000;

// Feedback transfers cover this much time each.
static const bigtime_t kFeedbackTransferTime = 8000;


Stream::Stream(Device* device, size_t interface, usb_interface_list* List)
	:
	AudioStreamingInterface(&device->AudioControl(), interface, List),
	fDevice(device),
	fStatus(B_NO_INIT),
	fStreamEndpoint(0),
	fIsRunning(false),
	fArea(-1),
	fKernelArea(-1),
	fAreaSize(0),
	fDescriptors(NULL),
	fDescriptorsCount(0),
	fCurrentBuffer(0),
	fSamplesCount(0),
	fFramesPerSecond(1000),
	fPacketsPerSecond(1000),
	fPacketShift(0),
	fPacketsPerBuffer(0),
	fMaxPacketsPerBuffer(0),
	fSamplesPerBuffer(0),
	fFrameSize(0),
	fMaxPacketFrames(0),
	fUseFeedback(false),
	fFeedbackEndpoint(0),
	fFeedbackPacketSize(0),
	fFeedbackPackets(0),
	fFeedbackSlot(0),
	fFeedbackFailures(0),
	fFeedbackFrame(0),
	fNominalFeedback(0),
	fFeedbackShift(INT32_MIN),
	fFeedbackFrames(0),
	fFeedbackLogged(false),
	fPhase(0),
	fRealTime(0),
	fStartingFrame(0),
	fProcessedBuffers(0),
	fInsideNotify(0)
{
}


Stream::~Stream()
{
	delete_area(fArea);
	delete_area(fKernelArea);
	delete[] fDescriptors;
}


status_t
Stream::_ChooseAlternate()
{
	// lookup alternate with maximal (ch * 100 + resolution)
	uint16 maxChxRes = 0;
	for (int i = 0; i < fAlternates.Count(); i++) {
		if (fAlternates[i]->Interface() == 0) {
			TRACE(INF, "Ignore alternate %d - zero interface description.\n", i);
			continue;
		}

		if (fAlternates[i]->Format() == 0) {
			TRACE(INF, "Ignore alternate %d - zero format description.\n", i);
			continue;
		}

		if (fAlternates[i]->Endpoint() == 0) {
			TRACE(INF, "Ignore alternate %d - no audio endpoint description.\n",
				i);
			continue;
		}

		if (fAlternates[i]->Format()->fFormatType
				!= USB_AUDIO_FORMAT_TYPE_I) {
			TRACE(ERR, "Ignore alternate %d - format type %#02x "
				"is not supported.\n", i, fAlternates[i]->Format()->fFormatType);
			continue;
		}

		switch (fAlternates[i]->Interface()->fFormatTag) {
			case USB_AUDIO_FORMAT_PCM:
			case USB_AUDIO_FORMAT_PCM8:
			case USB_AUDIO_FORMAT_IEEE_FLOAT:
		//	case USB_AUDIO_FORMAT_ALAW:
		//	case USB_AUDIO_FORMAT_MULAW:
				break;
			default:
				TRACE(ERR, "Ignore alternate %d - format %#04x is not "
					"supported.\n", i, fAlternates[i]->Interface()->fFormatTag);
			continue;
		}

		TypeIFormatDescriptor* format
			= static_cast<TypeIFormatDescriptor*>(fAlternates[i]->Format());

		if (format->fNumChannels > 2) {
			TRACE(ERR, "Ignore alternate %d - channel count %d "
				"is not supported.\n", i, format->fNumChannels);
			continue;
		}

		if (fAlternates[i]->Interface()->fFormatTag == USB_AUDIO_FORMAT_PCM) {
			switch(format->fBitResolution) {
				default:
				TRACE(ERR, "Ignore alternate %d - bit resolution %d "
					"is not supported.\n", i, format->fBitResolution);
					continue;
				case 8: case 16: case 18: case 20: case 24: case 32:
					break;
			}

			switch(format->fSubframeSize) {
				case 1: case 2: case 4:
					// The USB-Audio 1.0 specification also allows 3 bytes per subframe, but the
					// multi-audio resampler will write 4 bytes in that case, overwriting things
					// in the neighboring bytes.
					// It can be allowed after https://dev.haiku-os.org/ticket/20267 is resolved.
					break;
				default:
					TRACE(ERR, "Ignore alternate %d - subframe size %d is not supported.\n", i,
						format->fSubframeSize);
					continue;
			}
		}

		uint16 chxRes = format->fNumChannels * 100 + format->fBitResolution;
		if (chxRes > maxChxRes) {
			maxChxRes = chxRes;
			fActiveAlternate = i;
		}
	}

	if (maxChxRes <= 0) {
		TRACE(ERR, "No compatible alternate found. "
			"Stream initialization failed.\n");
		return B_NO_INIT;
	}

	const ASEndpointDescriptor* endpoint
		= fAlternates[fActiveAlternate]->Endpoint();
	fIsInput = (endpoint->fEndpointAddress & USB_ENDPOINT_ADDR_DIR_IN)
		== USB_ENDPOINT_ADDR_DIR_IN;

	if (fIsInput)
		fCurrentBuffer = (size_t)-1;

	TRACE(INF, "Alternate %d EP:%x selected for %s!\n",
		fActiveAlternate, endpoint->fEndpointAddress,
		fIsInput ? "recording" : "playback");

	return B_OK;
}


status_t
Stream::Init()
{
	fStatus = _ChooseAlternate();
	return fStatus;
}


void
Stream::OnRemove()
{
	// the transfer callback schedule traffic - so we must ensure that we are
	// not inside the callback anymore before returning, as we would otherwise
	// violate the promise not to use any of the pipes after returning from the
	// removed callback
	while (atomic_get(&fInsideNotify) != 0)
		snooze(100);

	gUSBModule->cancel_queued_transfers(fStreamEndpoint);
	if (fFeedbackEndpoint != 0)
		gUSBModule->cancel_queued_transfers(fFeedbackEndpoint);
}


bool
Stream::_IsHighSpeed()
{
	usb_device_speed_args args;
	args.device = fDevice->fDevice;
	args.speed = 0;
	if (gUSBModule->usb_ioctl(B_USB_GET_DEVICE_SPEED, &args, sizeof(args))
			== B_OK) {
		return args.speed >= B_USB_DEVICE_SPEED_HIGH;
	}

	// USB Audio 2.0 devices are high-speed ones
	return IsUAC2();
}


status_t
Stream::_SetupBuffers()
{
	// allocate buffer for worst (maximal size) case
	TypeIFormatDescriptor* format = static_cast<TypeIFormatDescriptor*>(
		fAlternates[fActiveAlternate]->Format());
	ASEndpointDescriptor* endpoint = fAlternates[fActiveAlternate]->Endpoint();

	uint32 samplingRate = fAlternates[fActiveAlternate]->GetSamplingRate();
	uint32 sampleSize = format->fNumChannels * format->fSubframeSize;
	if (samplingRate == 0 || sampleSize == 0) {
		TRACE(ERR, "no sampling rate (%" B_PRIu32 ") or sample size (%"
			B_PRIu32 ")!\n", samplingRate, sampleSize);
		return B_BAD_VALUE;
	}

	// isochronous endpoints are serviced every 2^(bInterval - 1) frames or
	// microframes
	uint32 interval = endpoint->fInterval;
	if (interval < 1)
		interval = 1;
	if (interval > 16)
		interval = 16;
	fFramesPerSecond = _IsHighSpeed() ? 8000 : 1000;
	fPacketShift = interval - 1;
	fPacketsPerSecond = fFramesPerSecond >> fPacketShift;
	if (fPacketsPerSecond == 0)
		fPacketsPerSecond = 1;
	uint32 maxPackets = kMaxBufferMilliseconds * fPacketsPerSecond / 1000;
	if (maxPackets == 0)
		maxPackets = 1;

	// Packets carry rate / fPacketsPerSecond samples on average: 48 for
	// 48 kHz, 44 or 45 for 44.1 kHz at 1000 packets per second. Each buffer
	// is whole repeats of that pattern (IsoPacketSizes.h).
	uint32 packetsPerBuffer = usb_audio_packets::packets_per_buffer(
		samplingRate, fPacketsPerSecond, kSamplesBufferSize, maxPackets);
	uint32 samplesPerBuffer = usb_audio_packets::samples_per_buffer(
		samplingRate, fPacketsPerSecond, packetsPerBuffer);
	size_t maxPacketSize = usb_audio_packets::max_samples_in_packet(
		samplingRate, fPacketsPerSecond) * sampleSize;
	// (a high-speed endpoint may take more than one transaction per
	// microframe)
	size_t endpointPacketSize = (endpoint->fMaxPacketSize & 0x7ff)
		* (1 + ((endpoint->fMaxPacketSize >> 11) & 0x3));
	TRACE(INF, "%" B_PRIu32 " Hz: %" B_PRIu32 " packets per second, %" B_PRIu32
		" packets of up to %" B_PRIuSIZE " bytes, %" B_PRIu32
		" samples per buffer\n", samplingRate, fPacketsPerSecond,
		packetsPerBuffer, maxPacketSize, samplesPerBuffer);

	if (maxPacketSize > endpointPacketSize) {
		TRACE(ERR, "%" B_PRIu32 " Hz needs %" B_PRIuSIZE "-byte packets, the "
			"endpoint takes %" B_PRIuSIZE "!\n", samplingRate, maxPacketSize,
			endpointPacketSize);
		return B_BAD_VALUE;
	}

	if (fArea != -1) {
		Stop();
		delete_area(fArea);
		delete_area(fKernelArea);
		delete[] fDescriptors;
		fArea = fKernelArea = -1;
		fDescriptors = NULL;
		fAreaSize = 0;
	}

	// An asynchronous output with a feedback endpoint (USB Audio 2.0) runs
	// at the rate of the device's clock: the packets of a buffer hold what
	// it asks for, and a buffer may need a few more of them.
	fUseFeedback = !fIsInput && IsUAC2() && endpoint->fFeedbackAddress != 0
		&& endpoint->fFeedbackMaxPacketSize >= 3
		&& (endpoint->fEndpointAttributes & 0x0c) == 0x04;
	fNominalFeedback = (uint32)(((uint64)samplingRate << 16) / fFramesPerSecond);
	fFeedbackShift = INT32_MIN;
	atomic_set(&fFeedbackFrames, 0);
	fFeedbackLogged = false;
	fPhase = 0;

	fFrameSize = sampleSize;
	fMaxPacketFrames = endpointPacketSize / sampleSize;
	fPacketsPerBuffer = packetsPerBuffer;
	fSamplesPerBuffer = samplesPerBuffer;
	fMaxPacketsPerBuffer = packetsPerBuffer;
	if (fUseFeedback)
		fMaxPacketsPerBuffer += packetsPerBuffer / 6 + 2;

	fAreaSize = sampleSize * samplesPerBuffer * kSamplesBufferCount;
	TRACE(INF, "estimate fAreaSize:%d\n", fAreaSize);

	// round up to B_PAGE_SIZE and create area
	fAreaSize = (fAreaSize + (B_PAGE_SIZE - 1)) &~ (B_PAGE_SIZE - 1);
	TRACE(INF, "rounded up fAreaSize:%d\n", fAreaSize);

	// Locked: the USB controller's isochronous completion copies into it.
	fArea = create_area(fIsInput ? DRIVER_NAME "_record_area"
		: DRIVER_NAME "_playback_area", (void**)&fBuffers,
		B_ANY_ADDRESS, fAreaSize, B_FULL_LOCK,
		B_READ_AREA | B_WRITE_AREA);
	if (fArea < 0) {
		TRACE(ERR, "Error of creating %#x - "
			"bytes size buffer area:%#010x\n", fAreaSize, fArea);
		fStatus = fArea;
		fAreaSize = 0;
		return fStatus;
	}

	// The kernel is not allowed to touch userspace areas, so we clone our
	// area into kernel space.
	fKernelArea = clone_area("usb_audio cloned area", (void**)&fKernelBuffers,
		B_ANY_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, fArea);
	if (fKernelArea < 0) {
		fStatus = fKernelArea;
		return fStatus;
	}

	// (through the kernel clone: the user-accessible mapping must not be
	// touched from the kernel, SMAP makes that a panic)
	memset(fKernelBuffers, 0, fAreaSize);

	TRACE(INF, "Created area id:%d at addr:%#010x size:%#010lx\n",
		fArea, fBuffers, fAreaSize);

	fDescriptorsCount = fMaxPacketsPerBuffer * kSamplesBufferCount;
	fDescriptors = new(std::nothrow) usb_iso_packet_descriptor[fDescriptorsCount];
	if (fDescriptors == NULL) {
		fDescriptorsCount = 0;
		return B_NO_MEMORY;
	}
	TRACE(INF, "descriptorsCount:%d\n", fDescriptorsCount);

	// samples count
	fSamplesCount = samplesPerBuffer * kSamplesBufferCount;
	TRACE(INF, "samplesCount:%d\n", fSamplesCount);

	// initialize descriptors array: every buffer has the same packets (when
	// the device's feedback sizes them, they are set for each transfer)
	for (size_t i = 0; i < fDescriptorsCount; i++) {
		fDescriptors[i].request_length = sampleSize
			* usb_audio_packets::samples_in_packet(samplingRate,
				fPacketsPerSecond, i % fMaxPacketsPerBuffer);
		fDescriptors[i].actual_length = 0;
		fDescriptors[i].status = B_OK;
	}

	TRACE(ERR, "%s %" B_PRIu32 " Hz, %" B_PRIu32 " bytes per frame, %"
		B_PRIu32 " packets per second, %" B_PRIu32 " frames per buffer%s\n",
		fIsInput ? "recording" : "playback", samplingRate, sampleSize,
		fPacketsPerSecond, samplesPerBuffer,
		fUseFeedback ? ", clocked by the device's feedback" : "");

	return fStatus;
}


/*!	Sets the alternate setting of this streaming interface, and finds the
	pipes of its endpoints.
*/
status_t
Stream::_SetAlternate(const usb_configuration_info* config, size_t alternate)
{
	if (config == NULL) {
		TRACE(ERR, "NULL configuration. Not set.\n");
		return B_ERROR;
	}

	usb_interface_info* interface = &config->interface[fInterface].alt[alternate];
	status_t status = gUSBModule->set_alt_interface(fDevice->fDevice, interface);
	TRACE(INF, "set_alt_interface %d: %x\n", (int)alternate, status);

	fStreamEndpoint = 0;
	fFeedbackEndpoint = 0;
	if (status != B_OK || alternate != fActiveAlternate)
		return status;

	ASEndpointDescriptor* endpoint = fAlternates[fActiveAlternate]->Endpoint();
	for (size_t i = 0; i < interface->endpoint_count; i++) {
		uint8 address = interface->endpoint[i].descr->endpoint_address;
		if (address == endpoint->fEndpointAddress)
			fStreamEndpoint = interface->endpoint[i].handle;
		else if (address == endpoint->fFeedbackAddress && address != 0)
			fFeedbackEndpoint = interface->endpoint[i].handle;
	}

	if (fStreamEndpoint == 0) {
		TRACE(INF, "%s Stream Endpoint [address %#04x] was not found.\n",
			fIsInput ? "Input" : "Output", endpoint->fEndpointAddress);
		return B_ERROR;
	}

	TRACE(INF, "%s Stream Endpoint [address %#04x] handle is: %#010x.\n",
		fIsInput ? "Input" : "Output", endpoint->fEndpointAddress,
		fStreamEndpoint);
	if (fFeedbackEndpoint != 0) {
		TRACE(INF, "Feedback Endpoint [address %#04x] handle is: %#010x.\n",
			endpoint->fFeedbackAddress, fFeedbackEndpoint);
	}
	return B_OK;
}


/*!	USB Audio 2.0: the clock of the terminal this stream is linked to. */
uint8
Stream::_ClockID()
{
	_AudioControl* control = fControlInterface->Find(TerminalLink());
	if (control == NULL
		|| (control->SubType() != USB_AUDIO_AC_INPUT_TERMINAL
			&& control->SubType() != USB_AUDIO_AC_OUTPUT_TERMINAL)) {
		TRACE(ERR, "No terminal %d for the stream\n", TerminalLink());
		return 0;
	}
	return static_cast<_Terminal*>(control)->ClockSourceID();
}


/*!	USB Audio 2.0: the rates the stream's clock runs at, of those the media
	node knows, are those of every alternate.
*/
void
Stream::_InitClockRates()
{
	Vector<uint32> clockRates;
	status_t status = fControlInterface->GetClockRates(_ClockID(), clockRates);

	Vector<uint32> rates;
	for (int32 i = 0; i < clockRates.Count(); i++) {
		if (clockRates[i] <= kMaxUAC2Rate
			&& fAlternates[fActiveAlternate]->GetSamplingRateId(clockRates[i])
				!= 0) {
			rates.PushBack(clockRates[i]);
		}
	}
	if (rates.Count() == 0) {
		TRACE(ERR, "No usable sampling rate from clock %d (%s): try 48 kHz\n",
			_ClockID(), strerror(status));
		rates.PushBack(48000);
	}

	for (int32 i = 0; i < fAlternates.Count(); i++) {
		_ASFormatDescriptor* format = fAlternates[i]->Format();
		if (format == NULL || format->fFormatType != USB_AUDIO_FORMAT_TYPE_I)
			continue;
		static_cast<TypeIFormatDescriptor*>(format)->SetSampleFrequencies(
			rates);
	}
}


status_t
Stream::OnSetConfiguration(usb_device device,
		const usb_configuration_info* config)
{
	if (IsUAC2())
		_InitClockRates();

	return _SetAlternate(config, fActiveAlternate);
}


status_t
Stream::Start()
{
	status_t result = B_BUSY;
	if (!fIsRunning) {
		// each buffer goes again when it is done, the first one first
		fCurrentBuffer = (size_t)-1;
		fPhase = 0;
		fIsRunning = true;
		for (size_t i = 0; i < kSamplesBufferCount; i++) {
			result = _QueueNextTransfer(i, i == 0);
			if (result != B_OK)
				break;
		}
		if (result != B_OK) {
			TRACE(ERR, "Queueing the stream failed: %s\n", strerror(result));
			Stop();
			return result;
		}

		if (fUseFeedback && fFeedbackEndpoint != 0) {
			// as many feedback packets as fit in kFeedbackTransferTime
			ASEndpointDescriptor* endpoint
				= fAlternates[fActiveAlternate]->Endpoint();
			uint32 interval = max_c(1, min_c(endpoint->fFeedbackInterval, 16));
			bigtime_t period = (1000000 / fFramesPerSecond) << (interval - 1);
			fFeedbackPackets = max_c(1, min_c(kFeedbackTransferTime / period,
				(bigtime_t)kMaxFeedbackPackets));
			fFeedbackPacketSize = min_c(endpoint->fFeedbackMaxPacketSize,
				(uint16)kMaxFeedbackPacketSize);
			fFeedbackSlot = 0;
			fFeedbackFailures = 0;
			for (uint32 i = 0; i < kFeedbackTransfers; i++) {
				status_t status = _QueueFeedbackTransfer(i);
				if (status != B_OK) {
					// play at the nominal rate then
					TRACE(ERR, "Feedback transfer failed: %s\n",
						strerror(status));
					break;
				}
			}
		}
	}
	return result;
}


status_t
Stream::Stop()
{
	if (fIsRunning) {
		// wait until possible notification handling finished...
		while (atomic_get(&fInsideNotify) != 0)
			snooze(100);
		fIsRunning = false;
	}
	gUSBModule->cancel_queued_transfers(fStreamEndpoint);
	if (fFeedbackEndpoint != 0)
		gUSBModule->cancel_queued_transfers(fFeedbackEndpoint);

	return B_OK;
}


/*!	The packets for a buffer of an output that the device's feedback clocks:
	they carry the frames it asks for per packet, so the number of packets
	the buffer takes changes. Returns the number of packets; dataLength is
	what they hold.
*/
size_t
Stream::_FeedbackPackets(usb_iso_packet_descriptor* descriptors,
	size_t& dataLength)
{
	int64 perPacket = atomic_get(&fFeedbackFrames);
	if (perPacket == 0)
		perPacket = (int64)fNominalFeedback << fPacketShift;

	uint32 framesLeft = fSamplesPerBuffer;
	size_t count = 0;
	while (framesLeft > 0 && count < fMaxPacketsPerBuffer) {
		fPhase += perPacket;
		int64 frames = fPhase >> 16;
		// (no empty packets: the controller takes none)
		if (frames < 1)
			frames = 1;
		if (frames > fMaxPacketFrames)
			frames = fMaxPacketFrames;
		if (frames > framesLeft)
			frames = framesLeft;
		fPhase -= frames << 16;

		descriptors[count].request_length = frames * fFrameSize;
		descriptors[count].actual_length = 0;
		descriptors[count].status = B_OK;
		framesLeft -= frames;
		count++;
	}

	// what the last packet of a buffer could not take goes into the next
	// one; never let that run away
	if (fPhase > 2 * perPacket)
		fPhase = 2 * perPacket;
	else if (fPhase < -2 * perPacket)
		fPhase = -2 * perPacket;

	if (framesLeft > 0) {
		TRACE(ERR, "%" B_PRIu32 " frames of a buffer did not fit in %"
			B_PRIuSIZE " packets\n", framesLeft, count);
	}
	dataLength = (fSamplesPerBuffer - framesLeft) * fFrameSize;
	return count;
}


status_t
Stream::_QueueNextTransfer(size_t queuedBuffer, bool start)
{
	size_t bufferSize = fFrameSize * fSamplesPerBuffer;
	usb_iso_packet_descriptor* descriptors
		= fDescriptors + queuedBuffer * fMaxPacketsPerBuffer;
	size_t packetsCount = fPacketsPerBuffer;
	size_t dataLength = bufferSize;
	if (fUseFeedback)
		packetsCount = _FeedbackPackets(descriptors, dataLength);

	TRACE(DTA, "buffers:%#010x[%#x]\ndescrs:%#010x[%#x]\n",
		fKernelBuffers + bufferSize * queuedBuffer, dataLength,
		descriptors, packetsCount);

	status_t status = gUSBModule->queue_isochronous(fStreamEndpoint,
		fKernelBuffers + bufferSize * queuedBuffer, dataLength,
		descriptors, packetsCount, &fStartingFrame, USB_ISO_ASAP,
		Stream::_TransferCallback, this);

	TRACE(DTA, "frame:%#010x\n", fStartingFrame);
	return status;
}


void
Stream::_TransferCallback(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	Stream* stream = (Stream*)cookie;

	TRACE(status == B_OK ? DTA : ERR,
		"stream:%010x: status:%#010x, data:%#010x, len:%d\n",
		stream->fStreamEndpoint, status, data, actualLength);

	atomic_add(&stream->fInsideNotify, 1);
	if (status == B_CANCELED || stream->fDevice->fRemoved || !stream->fIsRunning) {
		TRACE(ERR, "Cancelled: c:%p st:%#010x, data:%#010x, len:%d\n",
			cookie, status, data, actualLength);
		atomic_add(&stream->fInsideNotify, -1);
		return;
	}

#if 0
	stream->_DumpDescriptors();
#endif

	if (atomic_add(&stream->fProcessedBuffers, 1) > (int32)kSamplesBufferCount)
		TRACE(ERR, "Processed buffers overflow:%d\n", stream->fProcessedBuffers);
	stream->fRealTime = system_time();

	release_sem_etc(stream->fDevice->fBuffersReadySem, 1, B_DO_NOT_RESCHEDULE);

	// the buffer that is done goes again
	stream->fCurrentBuffer = (stream->fCurrentBuffer + 1) % kSamplesBufferCount;
	status = stream->_QueueNextTransfer(stream->fCurrentBuffer, false);

	atomic_add(&stream->fInsideNotify, -1);
}


status_t
Stream::_QueueFeedbackTransfer(uint32 slot)
{
	for (uint32 i = 0; i < fFeedbackPackets; i++) {
		fFeedbackDescriptors[slot][i].request_length = fFeedbackPacketSize;
		fFeedbackDescriptors[slot][i].actual_length = 0;
		fFeedbackDescriptors[slot][i].status = B_OK;
	}

	return gUSBModule->queue_isochronous(fFeedbackEndpoint,
		fFeedbackData[slot], fFeedbackPackets * fFeedbackPacketSize,
		fFeedbackDescriptors[slot], fFeedbackPackets, &fFeedbackFrame,
		USB_ISO_ASAP, Stream::_FeedbackCallback, this);
}


/*!	A feedback value: frames per frame in 10.14 (full speed, 3 bytes) or per
	microframe in 16.16 (high speed, 4 bytes). Devices get this wrong often
	enough that the format is told by shifting the first value until it
	comes near the nominal rate (as Linux does).
*/
void
Stream::_UpdateFeedback(uint32 value)
{
	if (value == 0)
		return;

	uint64 frames = value;
	uint64 nominal = fNominalFeedback;
	if (fFeedbackShift == INT32_MIN) {
		int32 shift = 0;
		while (frames < nominal - nominal / 4 && shift < 16) {
			frames <<= 1;
			shift++;
		}
		while (frames > nominal + nominal / 2 && shift > -16) {
			frames >>= 1;
			shift--;
		}
		fFeedbackShift = shift;
	} else if (fFeedbackShift >= 0)
		frames <<= fFeedbackShift;
	else
		frames >>= -fFeedbackShift;

	if (frames < nominal - nominal / 8 || frames > nominal + nominal / 8) {
		// maybe the shift was wrong: find it again with the next value
		TRACE(ERR, "Feedback %#" B_PRIx32 " is far from the nominal %#" B_PRIx64
			"\n", value, nominal);
		fFeedbackShift = INT32_MIN;
		return;
	}

	atomic_set(&fFeedbackFrames, (int32)(frames << fPacketShift));
	if (!fFeedbackLogged) {
		fFeedbackLogged = true;
		uint64 rate = frames * fFramesPerSecond;
		TRACE(ERR, "Feedback %#" B_PRIx32 " (shift %" B_PRId32 "): the device "
			"takes %" B_PRIu64 ".%03" B_PRIu64 " Hz\n", value, fFeedbackShift,
			rate >> 16, ((rate & 0xffff) * 1000) >> 16);
	}
}


void
Stream::_FeedbackCallback(void* cookie, status_t status, void* data,
	size_t actualLength)
{
	Stream* stream = (Stream*)cookie;

	atomic_add(&stream->fInsideNotify, 1);
	if (status == B_CANCELED || stream->fDevice->fRemoved
		|| !stream->fIsRunning) {
		atomic_add(&stream->fInsideNotify, -1);
		return;
	}

	uint32 slot = stream->fFeedbackSlot;
	stream->fFeedbackSlot = (slot + 1) % kFeedbackTransfers;

	// the latest value in it
	bool found = false;
	for (int32 i = stream->fFeedbackPackets - 1; i >= 0; i--) {
		usb_iso_packet_descriptor& descriptor
			= stream->fFeedbackDescriptors[slot][i];
		if (descriptor.status != B_OK || descriptor.actual_length < 3)
			continue;

		const uint8* bytes = stream->fFeedbackData[slot]
			+ i * stream->fFeedbackPacketSize;
		uint32 value = bytes[0] | bytes[1] << 8 | bytes[2] << 16;
		if (descriptor.actual_length >= 4 && stream->fFeedbackPacketSize >= 4)
			value = (value | (uint32)bytes[3] << 24) & 0x0fffffff;
		stream->_UpdateFeedback(value);
		found = true;
		break;
	}

	if (found)
		stream->fFeedbackFailures = 0;
	else if (++stream->fFeedbackFailures == 16) {
		// the device does not answer: keep the rate it last asked for
		TRACE(ERR, "No feedback from the device (%s), giving up on it\n",
			strerror(status));
	}

	if (stream->fFeedbackFailures < 16)
		stream->_QueueFeedbackTransfer(slot);

	atomic_add(&stream->fInsideNotify, -1);
}


void
Stream::_DumpDescriptors()
{
	for (size_t i = 0; i < fDescriptorsCount; i++)
		TRACE(ISO, "%d:req_len:%d; act_len:%d; stat:%#010x\n", i,
			fDescriptors[i].request_length,	fDescriptors[i].actual_length,
			fDescriptors[i].status);
}


status_t
Stream::GetEnabledChannels(uint32& offset, multi_channel_enable* Enable)
{
	AudioChannelCluster* cluster = ChannelCluster();
	if (cluster == 0)
		return B_ERROR;

	for (size_t i = 0; i < cluster->ChannelsCount(); i++) {
		B_SET_CHANNEL(Enable->enable_bits, offset++, true);
		TRACE(INF, "Report channel %d as enabled.\n", offset);
	}

	return B_OK;
}


status_t
Stream::SetEnabledChannels(uint32& offset, multi_channel_enable* Enable)
{
	AudioChannelCluster* cluster = ChannelCluster();
	if (cluster == 0)
		return B_ERROR;

	for (size_t i = 0; i < cluster->ChannelsCount(); i++, offset++) {
		TRACE(INF, "%s channel %d.\n",
			(B_TEST_CHANNEL(Enable->enable_bits, offset)
			? "Enable" : "Disable"), offset + 1);
	}

	return B_OK;
}


status_t
Stream::GetGlobalFormat(multi_format_info* Format)
{
	_multi_format* format = fIsInput ? &Format->input : &Format->output;
	format->cvsr = fAlternates[fActiveAlternate]->GetSamplingRate();
	format->rate = fAlternates[fActiveAlternate]->GetSamplingRateId(0);
	format->format = fAlternates[fActiveAlternate]->GetFormatId();
	TRACE(INF, "%s.rate:%d cvsr:%f format:%#08x\n",
		fIsInput ? "input" : "ouput",
		format->rate, format->cvsr, format->format);
	return B_OK;
}


status_t
Stream::SetGlobalFormat(multi_format_info* Format)
{
	_multi_format* format = fIsInput ? &Format->input : &Format->output;
	AudioStreamAlternate* alternate = fAlternates[fActiveAlternate];
	if (format->rate == alternate->GetSamplingRateId(0)
			&& format->format == alternate->GetFormatId()) {
		TRACE(INF, "No changes required\n");
		return B_OK;
	}

	alternate->SetSamplingRateById(format->rate);
	alternate->SetFormatId(format->format);
	TRACE(INF, "%s.rate:%d cvsr:%f format:%#08x\n",
		fIsInput ? "input" : "ouput",
		format->rate, format->cvsr, format->format);

	// cancel data flow - it will be rewaked at next buffer exchange call
	Stop();

	// TODO: wait for cancelling?

	// layout of buffers should be adjusted after changing sampling rate/format
	status_t status = _SetupBuffers();

	if (status != B_OK)
		return status;

	uint32 samplingRate = fAlternates[fActiveAlternate]->GetSamplingRate();

	if (IsUAC2()) {
		// The rate is the clock's. It changes with the interface at zero
		// bandwidth, and the alternate is selected after (like Linux does).
		const usb_configuration_info* config
			= gUSBModule->get_configuration(fDevice->fDevice);
		_SetAlternate(config, 0);
		status = fControlInterface->SetClockRate(_ClockID(), samplingRate);
		status_t alternateStatus = _SetAlternate(config, fActiveAlternate);
		return status != B_OK ? status : alternateStatus;
	}

	// set endpoint speed
	size_t actualLength = 0;
	usb_audio_sampling_freq freq = _ASFormatDescriptor::GetSamFreq(samplingRate);
	uint8 address = fAlternates[fActiveAlternate]->Endpoint()->fEndpointAddress;

	// Only an endpoint with a sampling frequency control takes the request
	// (bmAttributes D0, USB Audio 1.0 §4.6.1.2); others stall it, and the
	// format change itself has worked.
	if ((fAlternates[fActiveAlternate]->Endpoint()->fCSAttributes & 0x01) == 0) {
		TRACE(INF, "ep %#x has no sampling frequency control\n", address);
		return B_OK;
	}

	status = gUSBModule->send_request(fDevice->fDevice,
		USB_REQTYPE_CLASS | USB_REQTYPE_ENDPOINT_OUT,
		USB_AUDIO_SET_CUR, USB_AUDIO_SAMPLING_FREQ_CONTROL << 8,
		address, sizeof(freq), &freq, &actualLength);

	TRACE(ERR, "set_speed %02x%02x%02x for ep %#x %d: %s\n",
		freq.bytes[0], freq.bytes[1], freq.bytes[2],
		address, actualLength, strerror(status));
	return status;
}


status_t
Stream::GetBuffers(multi_buffer_list* List)
{
// TODO: check the available buffers count!
	if (fAreaSize == 0)
		return B_NO_INIT;

	int32 startChannel = List->return_playback_channels;
	buffer_desc** Buffers = List->playback_buffers;

	if (fIsInput) {
		List->flags |= B_MULTI_BUFFER_RECORD;
		List->return_record_buffer_size = fSamplesCount / kSamplesBufferCount;
		List->return_record_buffers = kSamplesBufferCount;
		startChannel = List->return_record_channels;
		Buffers = List->record_buffers;

		TRACE(DTA, "flags:%#10x\nreturn_record_buffer_size:%#010x\n"
			"return_record_buffers:%#010x\n", List->flags,
			List->return_record_buffer_size, List->return_record_buffers);
	} else {
		List->flags |= B_MULTI_BUFFER_PLAYBACK;
		List->return_playback_buffer_size = fSamplesCount / kSamplesBufferCount;
		List->return_playback_buffers = kSamplesBufferCount;

		TRACE(DTA, "flags:%#10x\nreturn_playback_buffer_size:%#010x\n"
			"return_playback_buffers:%#010x\n", List->flags,
			List->return_playback_buffer_size, List->return_playback_buffers);
	}

	TypeIFormatDescriptor* format = static_cast<TypeIFormatDescriptor*>(
		fAlternates[fActiveAlternate]->Format());

	// [buffer][channel] init buffers
	for (size_t buffer = 0; buffer < kSamplesBufferCount; buffer++) {
		TRACE(DTA, "%s buffer #%d:\n", fIsInput ? "input" : "output", buffer + 1);

		struct buffer_desc descs[format->fNumChannels];
		for (size_t channel = startChannel;
				channel < format->fNumChannels; channel++) {
			// init stride to the same for all buffers
			uint32 stride = format->fSubframeSize * format->fNumChannels;
			descs[channel].stride = stride;

			size_t bufferSize = (fSamplesCount / kSamplesBufferCount) * stride;
			descs[channel].base = (char*)fBuffers;
			descs[channel].base += buffer * bufferSize;
			descs[channel].base += channel * format->fSubframeSize;

			TRACE(DTA, "%d:%d: base:%#010x; stride:%#010x\n", buffer, channel,
				descs[channel].base, descs[channel].stride);
		}
		if (!IS_USER_ADDRESS(Buffers[buffer])
				|| user_memcpy(Buffers[buffer], descs, sizeof(descs)) < B_OK) {
			return B_BAD_ADDRESS;
		}
	}

	if (fIsInput) {
		List->return_record_channels += format->fNumChannels;
		TRACE(MIX, "return_record_channels:%#010x\n",
			List->return_record_channels);
	} else {
		List->return_playback_channels += format->fNumChannels;
		TRACE(MIX, "return_playback_channels:%#010x\n",
			List->return_playback_channels);
	}

	return B_OK;
}


bool
Stream::ExchangeBuffer(multi_buffer_info* Info)
{
	if (atomic_get(&fProcessedBuffers) <= 0)
		return false;

	if (fIsInput) {
		Info->recorded_real_time = fRealTime;
		Info->recorded_frames_count += fSamplesCount / kSamplesBufferCount;
		Info->record_buffer_cycle = fCurrentBuffer;
	} else {
		Info->played_real_time = fRealTime;
		Info->played_frames_count += fSamplesCount / kSamplesBufferCount;
		Info->playback_buffer_cycle = fCurrentBuffer;
	}

	atomic_add(&fProcessedBuffers, -1);

	return true;
}
