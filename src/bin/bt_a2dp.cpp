/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//! Streams audio to a Bluetooth speaker or headset (A2DP source).


#include <getopt.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Looper.h>
#include <MediaRoster.h>
#include <Message.h>
#include <Messenger.h>
#include <OS.h>

#include <bluetooth/bdaddrUtils.h>
#include <bluetooth/DiscoveryAgent.h>
#include <bluetooth/LocalDevice.h>
#include <bluetooth/bluetooth_error.h>
#include <bluetooth/HCI/btHCI.h>
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>

#include <A2dpSource.h>
#include <CommandManager.h>
#include <SbcEncoder.h>
#include <bluetoothserver_p.h>


using namespace Bluetooth;


static void
usage(const char* name)
{
	fprintf(stderr, "Usage: %s [options] <address>\n"
		"Connects to an A2DP sink, sets up an SBC stream and starts it.\n"
		"  -q, --query        only look up the sink's SDP record\n"
		"  -s, --seconds <n>  how long to keep the stream started (3)\n"
		"  -t, --tone <l,r>   play sines of l and r Hz (1000,1500)\n"
		"  -w, --sweep        play a sweep from 50 Hz to 16 kHz\n"
		"  -f, --file <raw>   play 16-bit interleaved PCM at the stream's\n"
		"                     rate and channel count\n"
		"  -l, --lead <s>     silence before the signal (1)\n"
		"  -v, --volume <dB>  level of tone and sweep (-6)\n"
		"  -r, --rate <hz>    preferred sample rate, 44100 or 48000\n"
		"  -b, --bitpool <n>  highest bitpool to use (53)\n"
		"  -d, --disconnect   drop the ACL link at the end\n"
		"  -u, --use          make the device the Bluetooth audio output\n"
		"                     and exit; \"none\" as address forgets it\n"
		"  -n, --name <name>  the device's name, with --use\n"
		"  -o, --output       show the system's audio output and exit\n"
		"  -i, --inquiry      look for the device first (and list what\n"
		"                     answers), to page it with what it reports\n"
		"  -D, --discoverable make this computer discoverable and\n"
		"                     connectable, and exit\n",
		name);
}


enum signal_kind {
	SIGNAL_NONE,
	SIGNAL_TONE,
	SIGNAL_SWEEP,
	SIGNAL_FILE
};


struct Generator {
	signal_kind	kind;
	double		frequency[2];
	double		phase[2];
	double		amplitude;
	double		seconds;
	double		lead;
	uint32		rate;
	uint32		channels;
	uint64		position;
	FILE*		file;

	void Fill(int16* samples, uint32 count)
	{
		for (uint32 i = 0; i < count; i++, position++) {
			const double time = (double)position / rate - lead;
			if (kind == SIGNAL_FILE && time >= 0) {
				if (fread(samples + i * channels, sizeof(int16), channels,
						file) != channels)
					memset(samples + i * channels, 0, channels * 2);
				continue;
			}
			for (uint32 channel = 0; channel < channels; channel++) {
				double value = 0;
				if (time >= 0 && kind == SIGNAL_TONE) {
					phase[channel] += 2 * M_PI * frequency[channel] / rate;
					value = sin(phase[channel]);
				} else if (time >= 0 && kind == SIGNAL_SWEEP) {
					// Exponential sweep, the same on both channels.
					const double k = log(16000.0 / 50.0) / seconds;
					value = sin(2 * M_PI * 50.0 * (exp(k * time) - 1) / k);
				}
				samples[i * channels + channel]
					= (int16)lrint(value * amplitude * 32767);
			}
		}
	}
};


static status_t
stream(A2dpSource& source, Generator& generator, double seconds)
{
	SbcEncoder encoder;
	const SbcConfiguration& config = source.Configuration();
	status_t status = encoder.SetTo(config);
	if (status != B_OK)
		return status;

	const uint32 perPacket = source.MaxFramesPerPacket();
	const uint32 frameSamples = encoder.FrameSamples();
	if (perPacket == 0)
		return B_BAD_VALUE;

	int16 samples[16 * 8 * 2];
	uint8 frames[15 * 520];
	const uint64 total = (uint64)((seconds + generator.lead) * config.sampleRate);
	uint64 sent = 0;
	uint32 packets = 0;
	uint32 errors = 0;
	bigtime_t late = 0;
	const bigtime_t start = system_time();
	while (sent < total) {
		for (uint32 i = 0; i < perPacket; i++) {
			generator.Fill(samples, frameSamples);
			encoder.Encode(samples, frames + i * encoder.FrameSize());
		}

		// Real time, a little ahead so that the sink's buffer fills.
		const bigtime_t due = start + (bigtime_t)(sent * 1000000
			/ config.sampleRate) - 40000;
		const bigtime_t now = system_time();
		if (due > now)
			snooze_until(due, B_SYSTEM_TIMEBASE);
		else if (now - due > late)
			late = now - due;

		status = source.SendFrames(frames, perPacket);
		if (status == B_NOT_ALLOWED) {
			fprintf(stderr, "the stream stopped after %" B_PRIu32
				" packets\n", packets);
			break;
		}
		if (status != B_OK)
			errors++;
		packets++;
		sent += perPacket * frameSamples;
	}

	printf("sent %" B_PRIu32 " packets of %" B_PRIu32 " frames (%" B_PRIu64
		" samples) in %" B_PRId64 " ms, %" B_PRIu32 " send errors, most late "
		"%" B_PRId64 " ms\n", packets, perPacket, sent,
		(system_time() - start) / 1000, errors, late / 1000);
	return errors == 0 ? B_OK : B_ERROR;
}


/*!	Receives what a Classic inquiry finds: address, signal strength, page
	scan repetition mode and clock offset of each device.
*/
class InquiryLooper : public BLooper {
public:
	InquiryLooper(const bdaddr_t& wanted)
		:
		BLooper("inquiry"),
		fWanted(wanted),
		fFound(false),
		fDone(create_sem(0, "inquiry done"))
	{
	}

	~InquiryLooper()
	{
		delete_sem(fDone);
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case BT_MSG_INQUIRY_DEVICE:
			{
				uint8 count = message->GetUInt8("count", 0);
				for (uint8 i = 0; i < count; i++) {
					const void* data;
					ssize_t size;
					if (message->FindData("bdaddr", B_ANY_TYPE, i, &data,
							&size) != B_OK || size != sizeof(bdaddr_t))
						continue;
					bdaddr_t address;
					memcpy(&address, data, sizeof(address));
					int8 rssi = 127;
					uint8 mode = 0;
					uint16 offset = 0;
					message->FindInt8("rssi", i, &rssi);
					message->FindUInt8("page_repetition_mode", i, &mode);
					message->FindUInt16("clock_offset", i, &offset);
					const void* deviceClass;
					uint32 record = 0;
					if (message->FindData("dev_class", B_ANY_TYPE, i,
							&deviceClass, &size) == B_OK && size == 3) {
						const uint8* bytes = (const uint8*)deviceClass;
						record = bytes[0] | (bytes[1] << 8) | (bytes[2] << 16);
					}
					const char* name = message->GetString("friendly_name", i,
						"");
					printf("  %s  rssi %4d  page scan R%u  clock offset "
						"%#06x  class %#08" B_PRIx32 "  %s\n",
						bdaddrUtils::ToString(address).String(), rssi, mode,
						offset, record, name);
					if (bdaddrUtils::Compare(address, fWanted)) {
						fFound = true;
						fMode = mode;
						fOffset = offset;
						fClass = record;
					}
				}
				break;
			}
			case BT_MSG_INQUIRY_COMPLETED:
			case BT_MSG_INQUIRY_TERMINATED:
			case BT_MSG_INQUIRY_ERROR:
				release_sem(fDone);
				break;
			default:
				BLooper::MessageReceived(message);
		}
	}

	bool Wait(bigtime_t timeout)
	{
		acquire_sem_etc(fDone, 1, B_RELATIVE_TIMEOUT, timeout);
		return fFound;
	}

	bdaddr_t	fWanted;
	bool		fFound;
	uint8		fMode;
	uint16		fOffset;
	uint32		fClass;
	sem_id		fDone;
};


static bool
inquire(const bdaddr_t& address, uint8& mode, uint16& offset,
	uint32& deviceClass)
{
	BMessenger server(BLUETOOTH_SIGNATURE);
	BMessage acquire(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage reply;
	hci_id hid;
	if (server.SendMessage(&acquire, &reply) != B_OK
		|| reply.FindInt32("hci_id", &hid) != B_OK)
		return false;

	InquiryLooper* looper = new InquiryLooper(address);
	looper->Run();

	// General inquiry for 8 x 1.28 s, as the Bluetooth kit does it.
	size_t size;
	void* command = buildInquiry(BT_GIAC, 8, BT_MAX_RESPONSES, &size);
	BMessage request(BT_MSG_HANDLE_SIMPLE_REQUEST);
	request.AddInt32("hci_id", hid);
	request.AddData("raw command", B_ANY_TYPE, command, size);
	request.AddInt16("eventExpected", HCI_EVENT_CMD_STATUS);
	request.AddInt16("opcodeExpected",
		PACK_OPCODE(OGF_LINK_CONTROL, OCF_INQUIRY));
	request.AddInt16("eventExpected", HCI_EVENT_INQUIRY_RESULT);
	request.AddInt16("eventExpected", HCI_EVENT_INQUIRY_RESULT_WITH_RSSI);
	request.AddInt16("eventExpected", HCI_EVENT_EXTENDED_INQUIRY_RESULT);
	request.AddInt16("eventExpected", HCI_EVENT_INQUIRY_COMPLETE);
	free(command);

	printf("inquiry (10 s):\n");
	bool found = false;
	if (server.SendMessage(&request, looper) == B_OK)
		found = looper->Wait(15000000);
	if (found) {
		mode = looper->fMode;
		offset = looper->fOffset;
		deviceClass = looper->fClass;
	}
	looper->Lock();
	looper->Quit();
	return found;
}


static int
show_output()
{
	BMediaRoster* roster = BMediaRoster::Roster();
	media_node node;
	if (roster == NULL || roster->GetAudioOutput(&node) != B_OK) {
		printf("no audio output\n");
		return 1;
	}
	live_node_info info;
	if (roster->GetLiveNodeInfo(node, &info) != B_OK)
		strlcpy(info.name, "?", sizeof(info.name));
	media_input inputs[4];
	int32 count = 0;
	roster->GetConnectedInputsFor(node, inputs, 4, &count);
	printf("audio output: %s (node %" B_PRId32 "), %" B_PRId32 " connected "
		"input(s)", info.name, node.node, count);
	if (count > 0) {
		printf(", %.0f Hz %" B_PRIu32 " channels",
			inputs[0].format.u.raw_audio.frame_rate,
			inputs[0].format.u.raw_audio.channel_count);
	}
	printf("\n");
	return 0;
}


static void
drop_link(const bdaddr_t& address)
{
	BMessenger server(BLUETOOTH_SIGNATURE);
	BMessage request(BT_MSG_ACQUIRE_LOCAL_DEVICE);
	BMessage reply;
	hci_id hid;
	if (server.SendMessage(&request, &reply) != B_OK
		|| reply.FindInt32("hci_id", &hid) != B_OK)
		return;

	BMessage disconnect(BT_REQ_DISCONNECT);
	disconnect.AddInt32("hci_id", hid);
	disconnect.AddData("bdaddr", B_ANY_TYPE, &address, sizeof(bdaddr_t));
	disconnect.AddUInt8("reason", BT_REMOTE_USER_ENDED_CONNECTION);
	server.SendMessage(&disconnect, &reply);
}


int
main(int argc, char** argv)
{
	static const struct option kOptions[] = {
		{ "query", no_argument, NULL, 'q' },
		{ "seconds", required_argument, NULL, 's' },
		{ "rate", required_argument, NULL, 'r' },
		{ "bitpool", required_argument, NULL, 'b' },
		{ "disconnect", no_argument, NULL, 'd' },
		{ "tone", required_argument, NULL, 't' },
		{ "sweep", no_argument, NULL, 'w' },
		{ "file", required_argument, NULL, 'f' },
		{ "lead", required_argument, NULL, 'l' },
		{ "volume", required_argument, NULL, 'v' },
		{ "use", no_argument, NULL, 'u' },
		{ "name", required_argument, NULL, 'n' },
		{ "output", no_argument, NULL, 'o' },
		{ "inquiry", no_argument, NULL, 'i' },
		{ "discoverable", no_argument, NULL, 'D' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};

	bool queryOnly = false;
	bool dropLink = false;
	double seconds = 3;
	uint32 rate = 44100;
	int bitpool = 53;
	Generator generator = {};
	generator.kind = SIGNAL_NONE;
	generator.frequency[0] = 1000;
	generator.frequency[1] = 1500;
	generator.amplitude = pow(10, -6 / 20.0);
	generator.lead = 1;
	const char* fileName = NULL;
	bool use = false;
	const char* useName = NULL;
	int option;
	bool inquiry = false;
	while ((option = getopt_long(argc, argv, "qs:r:b:dt:wf:l:v:un:oiDh",
			kOptions, NULL)) != -1) {
		switch (option) {
			case 'D':
			{
				LocalDevice* local = LocalDevice::GetLocalDevice();
				status_t status = local != NULL
					? local->SetDiscoverable(HCI_SCAN_INQUIRY_PAGE) : B_DEV_NOT_READY;
				printf("discoverable: %s\n", strerror(status));
				return status == B_OK ? 0 : 1;
			}
			case 'i':
				inquiry = true;
				break;
			case 'o':
				return show_output();
			case 'u':
				use = true;
				break;
			case 'n':
				useName = optarg;
				break;
			case 't':
				generator.kind = SIGNAL_TONE;
				sscanf(optarg, "%lf,%lf", &generator.frequency[0],
					&generator.frequency[1]);
				break;
			case 'w':
				generator.kind = SIGNAL_SWEEP;
				break;
			case 'f':
				generator.kind = SIGNAL_FILE;
				fileName = optarg;
				break;
			case 'l':
				generator.lead = atof(optarg);
				break;
			case 'v':
				generator.amplitude = pow(10, atof(optarg) / 20.0);
				break;
			case 'q':
				queryOnly = true;
				break;
			case 's':
				seconds = atof(optarg);
				break;
			case 'r':
				rate = strtoul(optarg, NULL, 10);
				break;
			case 'b':
				bitpool = atoi(optarg);
				break;
			case 'd':
				dropLink = true;
				break;
			default:
				usage(argv[0]);
				return option == 'h' ? 0 : 1;
		}
	}
	if (optind + 1 != argc) {
		usage(argv[0]);
		return 1;
	}

	if (use && strcmp(argv[optind], "none") == 0) {
		status_t status = SetAudioSink(NULL, NULL);
		if (status != B_OK)
			fprintf(stderr, "%s: %s\n", argv[0], strerror(status));
		return status == B_OK ? 0 : 1;
	}

	const bdaddr_t address = bdaddrUtils::FromString(argv[optind]);
	if (bdaddrUtils::Compare(address, bdaddrUtils::NullAddress())) {
		fprintf(stderr, "%s: bad address \"%s\"\n", argv[0], argv[optind]);
		return 1;
	}

	if (use) {
		status_t status = SetAudioSink(&address, useName);
		if (status != B_OK) {
			fprintf(stderr, "%s: %s\n", argv[0], strerror(status));
			return 1;
		}
		printf("Bluetooth audio plays to %s\n", argv[optind]);
		return 0;
	}

	A2dpSource source(address);
	source.SetPreferredSampleRate(rate);
	if (inquiry) {
		uint8 mode;
		uint16 offset;
		uint32 deviceClass;
		if (inquire(address, mode, offset, deviceClass)) {
			printf("found it; paging with R%u, clock offset %#x\n", mode,
				offset);
			source.SetPageParameters(mode, offset);
			source.SetDeviceClass(deviceClass);
		} else
			printf("the device did not answer the inquiry\n");
	}
	if (bitpool >= 2 && bitpool <= 250)
		source.SetMaxBitpool(bitpool);

	if (queryOnly) {
		// SDP needs a link; Connect() makes one, so make it that way and
		// leave the stream alone.
		A2dpSinkInfo info;
		BString error;
		status_t status = A2dpSource::QuerySink(address, info, &error);
		if (status != B_OK) {
			fprintf(stderr, "%s: %s\n", argv[0], error.String());
			return 1;
		}
		printf("Audio Sink: A2DP %x.%02x, AVDTP %x.%02x on PSM %#x, "
			"features %#x\n", info.a2dpVersion >> 8, info.a2dpVersion & 0xff,
			info.avdtpVersion >> 8, info.avdtpVersion & 0xff, info.psm,
			info.features);
		return 0;
	}

	bigtime_t start = system_time();
	status_t status = source.Connect();
	if (status != B_OK) {
		fprintf(stderr, "%s: connect failed: %s (%s)\n", argv[0],
			source.LastError(), strerror(status));
		if (dropLink)
			drop_link(address);
		return 1;
	}

	const SbcConfiguration& config = source.Configuration();
	const A2dpSinkInfo& info = source.SinkInfo();
	printf("connected in %" B_PRId64 " ms: A2DP %x.%02x, AVDTP %x.%02x\n",
		(system_time() - start) / 1000, info.a2dpVersion >> 8,
		info.a2dpVersion & 0xff, info.avdtpVersion >> 8,
		info.avdtpVersion & 0xff);
	printf("SBC %" B_PRIu32 " Hz, %s, %u blocks, %u subbands, %s, bitpool %u "
		"(sink %u-%u): %" B_PRIuSIZE "-byte frames, %" B_PRIu32 " kbit/s; "
		"media MTU %" B_PRIuSIZE "\n", config.sampleRate,
		config.channelMode == SBC_MODE_JOINT_STEREO ? "joint stereo"
			: config.channelMode == SBC_MODE_STEREO ? "stereo"
			: config.channelMode == SBC_MODE_DUAL_CHANNEL ? "dual channel"
			: "mono",
		config.blocks, config.subbands,
		config.allocation == SBC_ALLOCATION_LOUDNESS ? "loudness" : "SNR",
		config.bitpool, config.minBitpool, config.maxBitpool,
		config.FrameSize(), config.BitRate() / 1000, source.MediaMTU());

	start = system_time();
	status = source.Start();
	if (status != B_OK) {
		fprintf(stderr, "%s: start failed: %s\n", argv[0], source.LastError());
		source.Disconnect();
		if (dropLink)
			drop_link(address);
		return 1;
	}
	printf("streaming (start took %" B_PRId64 " ms)\n",
		(system_time() - start) / 1000);

	if (generator.kind == SIGNAL_NONE)
		snooze((bigtime_t)(seconds * 1000000));
	else {
		generator.rate = config.sampleRate;
		generator.channels = config.Channels();
		generator.seconds = seconds;
		if (generator.kind == SIGNAL_FILE) {
			generator.file = fopen(fileName, "rb");
			if (generator.file == NULL) {
				fprintf(stderr, "%s: cannot open %s\n", argv[0], fileName);
				generator.kind = SIGNAL_NONE;
			}
		}
		stream(source, generator, seconds);
		if (generator.file != NULL)
			fclose(generator.file);
	}

	status = source.Suspend();
	printf("suspend: %s\n", status == B_OK ? "ok" : source.LastError());
	source.Disconnect();
	printf("closed\n");
	if (dropLink)
		drop_link(address);
	return 0;
}
