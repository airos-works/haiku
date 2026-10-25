/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Audio on the Qualcomm SC8280XP (Radxa Dragon Q8B): the board's sound
	card node ("qcom,sc8280xp-sndcard"), played through the audio DSP's
	AudioReach graphs (the board's topology, linux-firmware's
	SC8280XP-Radxa-Dragon-Q8B-tplg.bin) to the WCD9385's headphone output.
	The DSP is run by qcom_adsp; its packets go through qcom_gpr. More
	devices play to HDMI and the USB-C connectors' DisplayPort: the DSP's
	DISPLAY_PORT_RX into the DisplayPort controller (the board's HDMI
	bridge's, or a USB-C connector's), whose audio engine qcom_display turns
	on.
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <new>

#include <KernelExport.h>
#include <device_manager.h>
#include <kernel.h>
#include <smp.h>
#include <driver_settings.h>
#include <util/AutoLock.h>

#include "driver.h"


#define TRACE(x...) dprintf("qcom_audio: " x)

#define QCOM_AUDIO_DRIVER_NAME "drivers/audio/hmulti/qcom_audio/driver_v1"
#define QCOM_AUDIO_DEVICE_NAME "drivers/audio/hmulti/qcom_audio/device_v1"
// audio/hmulti/qcom/<n>: output n - 1 (the headphones, HDMI, DP-1, DP-2)
#define QCOM_AUDIO_DEVICE_PATH "audio/hmulti/qcom/"


using namespace qcom_audio;


static device_manager_info* sDeviceManager;


static void
data_hook(void* cookie, const qcom_gpr_packet* packet)
{
	AudioDevice* device = (AudioDevice*)cookie;
	if (packet->opcode == AudioPath::kWriteDone) {
		Output* output = device->outputs[(packet->token >> 8) % kOutputCount];
		cpu_status state = disable_interrupts();
		acquire_spinlock(&device->streamLock);
		output->lastPlayed = (packet->token & 0xff) % kBufferCount;
		output->framesPlayed += kBufferFrames;
		output->playedTime = system_time();
		release_spinlock(&device->streamLock);
		restore_interrupts(state);
		atomic_add(&output->doneCount, 1);
		release_sem_etc(output->doneSem, 1, B_DO_NOT_RESCHEDULE);
		return;
	}
	if (packet->opcode == AudioPath::kReadDone) {
		// data_cmd_rsp_rd_sh_mem_ep_data_buffer_done_v2: status, address,
		// handle, the size filled
		const uint32* words = (const uint32*)packet->payload;
		uint32 size = packet->payload_size >= 20 ? words[4] : 0;
		cpu_status state = disable_interrupts();
		acquire_spinlock(&device->streamLock);
		device->lastRecorded = packet->token % kRecordBufferCount;
		device->lastRecordedSize = size;
		if (size != 0) {
			device->framesRecorded += kRecordBufferFrames;
			device->recordedTime = system_time();
		}
		release_spinlock(&device->streamLock);
		restore_interrupts(state);
		atomic_add(&device->recordDoneCount, 1);
		release_sem_etc(device->recordSem, 1, B_DO_NOT_RESCHEDULE);
		return;
	}
	TRACE("data event %#" B_PRIx32 " (token %" B_PRIu32 ")\n", packet->opcode,
		packet->token);
}


/*!	The DSP reaches the buffers it is given as stream 0xc01 (the device
	tree's q6apm "dais" node): a context bank of its own that maps only
	them. The address the DSP is given has bit 32 set, as Linux' (0x1fff80000
	in a trace of Linux playing on this board): below 4 GiB the DSP takes
	an address as physical and reads it on its own untranslated path, and
	its audio process crashed on the first buffer. With bit 32, it reads
	through the SMMU at the low 32 bits (lab: "dsp_iova" sets another
	address; 0 for the headphones' buffers' physical address).
*/
static status_t
attach_dsp_stream(AudioDevice* device)
{
	void* settings = load_driver_settings("qcom_audio");
	device->dspBase = strtoull(get_driver_parameter(settings, "dsp_iova",
		"0x1fff00000", "0x1fff00000"), NULL, 0);
	unload_driver_settings(settings);
	static const uint32 kStreams[][2] = {{0xc01, 0}};
	return device->smmu.Attach("qcom_audio", kStreams, 1);
}


/*!	An output's buffers, mapped for the DSP its dspOffset above the others'
	base, and its graph opened with them as its shared memory (\a openGraph).
*/
static status_t
open_output(Output* output, bool openGraph)
{
	AudioDevice* device = output->device;
	size_t size = ROUNDUP(kBufferCount * kBufferSize, B_PAGE_SIZE);
	output->bufferArea = create_area("qcom audio buffers",
		(void**)&output->buffers, B_ANY_KERNEL_ADDRESS, size, B_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (output->bufferArea < 0)
		return output->bufferArea;
	physical_entry entry;
	get_memory_map(output->buffers, B_PAGE_SIZE, &entry, 1);
	output->buffersPhysical = entry.address;
	memset(output->buffers, 0, kBufferCount * kBufferSize);
	output->buffersDsp = device->dspBase != 0
		? device->dspBase + output->dspOffset : output->buffersPhysical;
	// bit 32 only selects the DSP's path through the SMMU: the address the
	// SMMU sees is the low 32 bits (its fault said so)
	status_t status = device->smmu.Map(output->buffersDsp & 0xffffffff,
		output->buffersPhysical, size, false);
	TRACE("the DSP's %s buffers at %#" B_PRIx64 " (physical %#"
		B_PRIxPHYSADDR ")\n", output->name, output->buffersDsp,
		output->buffersPhysical);
	if (status == B_OK && openGraph)
		status = output->path.Open(48000, 2, 16);
	if (status == B_OK && openGraph)
		status = output->path.MapMemory(output->buffersDsp, size);
	output->open = status == B_OK && openGraph;
	return status;
}


/*!	The capture buffers: their own area, mapped for the DSP kRecordDspOffset
	above the playback buffers (bit 32 set for the DSP, the low 32 bits in
	our context bank), and their own shared memory region.
*/
static status_t
open_capture(AudioDevice* device)
{
	size_t size = ROUNDUP(kRecordBufferCount * kRecordBufferSize, B_PAGE_SIZE);
	device->recordArea = create_area("qcom audio record buffers",
		(void**)&device->recordBuffers, B_ANY_KERNEL_ADDRESS, size,
		B_CONTIGUOUS, B_READ_AREA | B_WRITE_AREA);
	if (device->recordArea < 0)
		return device->recordArea;
	physical_entry entry;
	get_memory_map(device->recordBuffers, B_PAGE_SIZE, &entry, 1);
	device->recordPhysical = entry.address;
	memset(device->recordBuffers, 0, size);
	for (addr_t line = (addr_t)device->recordBuffers;
			line < (addr_t)device->recordBuffers + size; line += 64)
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	memory_full_barrier();
	device->recordDsp = device->dspBase + kRecordDspOffset;
	status_t status = device->smmu.Map(device->recordDsp & 0xffffffff,
		device->recordPhysical, size, false);
	if (status == B_OK)
		status = device->capture.Open(48000, 2, 16);
	if (status == B_OK)
		status = device->capture.MapMemory(device->recordDsp, size);
	device->captureOpen = status == B_OK;
	return status;
}


//!	A filled capture buffer as the DSP wrote it: out of our cache first.
void
qcom_audio::invalidate_record_buffer(AudioDevice* device, uint32 index)
{
	addr_t start = (addr_t)device->recordBuffers + index * kRecordBufferSize;
	for (addr_t line = start; line < start + kRecordBufferSize; line += 64)
		asm volatile("dc ivac, %0" : : "r" (line) : "memory");
	memory_full_barrier();
}


/*!	Lab ("record_test" in the qcom_audio settings): two seconds from the
	capture path; how many buffers came back, at what rate, and their level.
*/
static void
record_test(AudioDevice* device)
{
	status_t status = device->codec.EnableHeadsetMic(48000);
	if (status != B_OK)
		TRACE("record test: headset microphone: %s\n", strerror(status));
	while (acquire_sem_etc(device->recordSem, 1, B_RELATIVE_TIMEOUT, 0)
			== B_OK)
		;
	device->recordDoneCount = 0;
	// as Linux: the stream started, the buffers queued, then the device
	status = device->capture.Start(false);
	for (uint32 i = 0; status == B_OK && i < kRecordBufferCount; i++) {
		device->capture.Read(device->recordDsp + i * kRecordBufferSize,
			kRecordBufferSize, i);
	}
	if (status == B_OK)
		status = device->capture.StartDevice();
	TRACE("record test: capture start: %s\n", strerror(status));
	if (status != B_OK) {
		device->capture.Stop();
		device->codec.DisableHeadsetMic();
		return;
	}
	bigtime_t start = system_time();
	int32 peak[2] = {}, minimum[2] = {};
	uint64 energy[2] = {};
	uint64 samples = 0;
	uint32 firstSize = 0;
	while (system_time() - start < 2000000) {
		if (acquire_sem_etc(device->recordSem, 1, B_RELATIVE_TIMEOUT, 500000)
				!= B_OK) {
			TRACE("record test: no buffer filled for 0.5 s (%" B_PRId32
				" done)\n", device->recordDoneCount);
			device->smmu.Faults("record test");
			break;
		}
		int32 index = device->lastRecorded;
		if (firstSize == 0)
			firstSize = device->lastRecordedSize;
		invalidate_record_buffer(device, index);
		const int16* frames = (const int16*)(device->recordBuffers
			+ index * kRecordBufferSize);
		for (uint32 i = 0; i < kRecordBufferSize / 2; i++) {
			int32 value = frames[i];
			uint32 channel = i & 1;
			peak[channel] = max_c(peak[channel], value);
			minimum[channel] = min_c(minimum[channel], value);
			energy[channel] += (uint64)(value * value);
		}
		samples += kRecordBufferSize / 4;
		device->capture.Read(device->recordDsp + index * kRecordBufferSize,
			kRecordBufferSize, index);
	}
	bigtime_t elapsed = system_time() - start;
	int32 done = device->recordDoneCount;
	device->capture.Stop();
	device->codec.DisableHeadsetMic();
	TRACE("record test: %" B_PRId32 " buffers (the first %" B_PRIu32 " bytes) in %"
		B_PRId64 " us: %" B_PRId64 " frames a second; left %" B_PRId32 "..%"
		B_PRId32 " mean square %" B_PRIu64 ", right %" B_PRId32 "..%" B_PRId32
		" mean square %" B_PRIu64 "\n", done, firstSize,
		elapsed, elapsed > 0 ? (int64)done * (kRecordBufferSize / 4) * 1000000
			/ elapsed : 0, minimum[0], peak[0],
		samples > 0 ? energy[0] / samples : 0, minimum[1], peak[1],
		samples > 0 ? energy[1] / samples : 0);
}


//!	The DSP reads the buffers from memory: our lines of them cleaned first.
void
qcom_audio::clean_buffer(Output* output, uint32 index)
{
	addr_t start = (addr_t)output->buffers + index * kBufferSize;
	for (addr_t line = start; line < start + kBufferSize; line += 64)
		asm volatile("dc cvac, %0" : : "r" (line) : "memory");
	memory_full_barrier();
}


/*!	What an output needs beside its graph: the headphones the RX macro's
	path (it takes the codec DMA's samples before the graph runs, as DAPM
	powers it before the trigger in Linux) and the codec's ports and
	amplifiers; a DisplayPort output its controller's audio engine, which
	the display driver turns on when the output has a link.
*/
status_t
qcom_audio::output_power_up(Output* output)
{
	AudioDevice* device = output->device;
	if (output->dpController != kNoDisplayPort) {
		if (device->dpAudio == NULL)
			return B_DEV_NOT_READY;
		return device->dpAudio->start(output->dpController, 2);
	}
	device->codec.EnableHeadphonePath(48000);
	return device->codec.EnableHeadphones(kHeadphoneGain);
}


void
qcom_audio::output_power_down(Output* output)
{
	AudioDevice* device = output->device;
	if (output->dpController != kNoDisplayPort) {
		if (device->dpAudio != NULL)
			device->dpAudio->stop(output->dpController);
		return;
	}
	device->codec.DisableHeadphones();
	device->codec.DisableHeadphonePath();
}


/*!	Lab ("test_tone" in the qcom_audio settings): a 750 Hz tone for two
	seconds through the graph; how many buffers the DSP played in that time
	says whether it plays at 48 kHz. On the headphones, or HDMI with
	"test_tone_hdmi".
*/
static void
test_tone(Output* output)
{
	AudioDevice* device = output->device;
	// a 750 Hz square wave at a quarter of full scale: 64 frames a period
	for (uint32 index = 0; index < kBufferCount; index++) {
		int16* samples = (int16*)(output->buffers + index * kBufferSize);
		for (uint32 frame = 0; frame < kBufferFrames; frame++) {
			int16 value = (frame & 32) != 0 ? 8192 : -8192;
			samples[frame * 2] = samples[frame * 2 + 1] = value;
		}
		clean_buffer(output, index);
	}
	void* settings = load_driver_settings("qcom_audio");
	bool startOnly = get_driver_boolean_parameter(settings, "start_only",
		false, true);
	bool oneBuffer = get_driver_boolean_parameter(settings, "one_buffer",
		false, true);
	bool startDevice = !get_driver_boolean_parameter(settings,
		"stream_only", false, true);
	unload_driver_settings(settings);
	TRACE("test tone: starting the graph%s at %" B_PRId64 " ms\n",
		startOnly ? " (no buffers)" : "", system_time() / 1000);
	device->smmu.Faults("before the graph");
	status_t status = output_power_up(output);
	if (status != B_OK) {
		TRACE("test tone: %s: %s\n", output->name, strerror(status));
		return;
	}
	snooze(100000);
	status = output->path.Start(startDevice);
	TRACE("test tone: graph start: %s\n", strerror(status));
	if (status != B_OK) {
		output_power_down(output);
		return;
	}
	if (startOnly) {
		snooze(2000000);
		output->path.Stop();
		output_power_down(output);
		TRACE("test tone: stopped\n");
		return;
	}
	if (oneBuffer) {
		// lab ("access_probe"): does the DSP read the buffer through our
		// context bank at all? Its first read then faults there.
		void* probeSettings = load_driver_settings("qcom_audio");
		bool accessProbe = get_driver_boolean_parameter(probeSettings,
			"access_probe", false, true);
		unload_driver_settings(probeSettings);
		if (accessProbe) {
			device->smmu.ClearAccessFlags(output->buffersDsp & 0xffffffff,
				ROUNDUP(kBufferCount * kBufferSize, B_PAGE_SIZE));
			TRACE("one buffer: the buffers' access flags cleared\n");
		}
		// lab: what the SMMU makes of the DSP's reads: our bank, and the
		// firmware's bank 4 (stream 0x1000/0x402, the DSP's own, as it
		// seems), its SCTLR (stall on fault?) and FSR
		volatile uint32* cb4;
		area_id cb4Area = map_physical_memory("qcom audio smmu cb4",
			0x15000000 + 0x80000 + 4 * 0x1000, 0x1000,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&cb4);
		if (cb4Area >= 0) {
			TRACE("bank 4 before: SCTLR %#" B_PRIx32 " FSR %#" B_PRIx32
				" FAR %#" B_PRIx32 "%08" B_PRIx32 "\n", cb4[0], cb4[0x58 / 4],
				cb4[0x64 / 4], cb4[0x60 / 4]);
		}
		output->path.Write(output->buffersDsp, kBufferSize, output->token);
		uint32 loopState = device->dsp.State(NULL, 0);
		TRACE("one buffer: DSP state %#" B_PRIx32 " before\n", loopState);
		for (int i = 0; i < 300; i++) {
			uint32 nowState = device->dsp.State(NULL, 0);
			if (nowState != loopState) {
				char reason[96];
				device->dsp.State(reason, sizeof(reason));
				TRACE("one buffer: DSP state %#" B_PRIx32 " after %d ms: %s\n",
					nowState, i, reason);
				loopState = nowState;
			}
			uint32 bank4 = cb4Area >= 0 ? cb4[0x58 / 4] & ~0x600u : 0;
			if (device->smmu.Faults("one buffer") != 0 || bank4 != 0
				|| output->doneCount > 0 || i == 299) {
				TRACE("one buffer after %d ms: %" B_PRId32 " done; bank 4 FSR %#"
					B_PRIx32 " FAR %#" B_PRIx32 "%08" B_PRIx32 " FSYNR0 %#"
					B_PRIx32 "\n", i, output->doneCount,
					cb4Area >= 0 ? cb4[0x58 / 4] : 0,
					cb4Area >= 0 ? cb4[0x64 / 4] : 0,
					cb4Area >= 0 ? cb4[0x60 / 4] : 0,
					cb4Area >= 0 ? cb4[0x68 / 4] : 0);
				break;
			}
			spin(1000);
		}
		if (cb4Area >= 0)
			delete_area(cb4Area);
		// lab: is the system still alive once the DSP has the buffer? A
		// heartbeat that touches nothing of the LPASS
		uint32 lastState = device->dsp.State(NULL, 0);
		for (int i = 0; i < 1000; i++) {
			snooze(10000);
			char reason[96];
			uint32 state = device->dsp.State(reason, sizeof(reason));
			if (state != lastState || i % 10 == 0) {
				TRACE("one buffer: alive %d on CPU %" B_PRId32 ", %" B_PRId32
					" done, DSP state %#" B_PRIx32 "%s%s\n", i,
					smp_get_current_cpu(), output->doneCount, state,
					(state & 1) != 0 ? ", crashed: " : "",
					(state & 1) != 0 ? reason : "");
				lastState = state;
			}
		}
		output->path.Stop();
		output_power_down(output);
		TRACE("test tone: stopped, %" B_PRId32 " done\n", output->doneCount);
		return;
	}
	bigtime_t start = system_time();
	int32 written = 0;
	for (; written < (int32)kBufferCount; written++) {
		output->path.Write(output->buffersDsp
			+ (written % kBufferCount) * kBufferSize, kBufferSize,
			output->token | (written % kBufferCount));
	}
	while (system_time() - start < 2000000) {
		if (acquire_sem_etc(output->doneSem, 1, B_RELATIVE_TIMEOUT, 500000)
				!= B_OK) {
			TRACE("test tone: no buffer done for 0.5 s (%" B_PRId32 " done)\n",
				output->doneCount);
			device->smmu.Faults("test tone");
			break;
		}
		output->path.Write(output->buffersDsp
			+ (written % kBufferCount) * kBufferSize, kBufferSize,
			output->token | (written % kBufferCount));
		written++;
	}
	bigtime_t elapsed = system_time() - start;
	int32 done = output->doneCount;
	output->path.Stop();
	output_power_down(output);
	TRACE("test tone: %" B_PRId32 " buffers of %" B_PRIu32 " frames played in %"
		B_PRId64 " us: %" B_PRId64 " frames a second\n", done, kBufferFrames,
		elapsed, elapsed > 0 ? (int64)done * kBufferFrames * 1000000 / elapsed
			: 0);
}


static status_t start_up(AudioDevice* device);


/*!	Lab ("watch_dsp"): the DSP's SMP2P state every 5 ms for a minute,
	changes logged with its crash reason, to see which step crashes it.
*/
static status_t
watch_dsp(void* cookie)
{
	AudioDevice* device = (AudioDevice*)cookie;
	uint32 last = 0xffffffff;
	for (int i = 0; i < 12000; i++) {
		char reason[96];
		uint32 state = device->dsp.State(reason, sizeof(reason));
		if (state != last) {
			TRACE("watch: DSP state %#" B_PRIx32 " at %" B_PRId64 " ms%s%s\n",
				state, system_time() / 1000, (state & 1) != 0 ? ": " : "",
				(state & 1) != 0 ? reason : "");
			last = state;
		}
		snooze(5000);
	}
	return B_OK;
}


static status_t
start_thread(void* cookie)
{
	AudioDevice* device = (AudioDevice*)cookie;
	status_t status = start_up(device);
	device->startDone = true;
	return status;
}


//!	Waits for the DSP, then brings the codec and the graph up.
static status_t
start_up(AudioDevice* device)
{
	// on unless the settings say "enabled false"
	void* enableSettings = load_driver_settings("qcom_audio");
	bool enabled = get_driver_boolean_parameter(enableSettings, "enabled",
		true, true);
	unload_driver_settings(enableSettings);
	if (!enabled)
		return B_OK;
	status_t status = device->dsp.WaitReady(60000000);
	enableSettings = load_driver_settings("qcom_audio");
	if (get_driver_boolean_parameter(enableSettings, "watch_dsp", false,
			true)) {
		resume_thread(spawn_kernel_thread(watch_dsp, "qcom audio watch",
			B_URGENT_DISPLAY_PRIORITY, device));
	}
	unload_driver_settings(enableSettings);
	if (status != B_OK) {
		TRACE("the audio DSP did not come up\n");
		return status;
	}
	status = device->codec.PowerUp(device->dsp);
	if (status != B_OK) {
		TRACE("codec: %s\n", strerror(status));
		return status;
	}
	// lab ("record_only"): no playback graph open while capturing
	void* labSettings = load_driver_settings("qcom_audio");
	bool recordOnly = get_driver_boolean_parameter(labSettings, "record_only",
		false, true);
	unload_driver_settings(labSettings);
	status = attach_dsp_stream(device);
	if (status == B_OK)
		status = open_output(&device->headphones, !recordOnly);
	if (status != B_OK) {
		TRACE("playback: %s\n", strerror(status));
		return status;
	}

	// HDMI and USB-C: the display driver turns the DisplayPort's audio on
	if (get_module(QCOM_DP_AUDIO_MODULE_NAME,
			(module_info**)&device->dpAudio) != B_OK) {
		device->dpAudio = NULL;
	}
	for (uint32 i = 1; device->dpAudio != NULL && !recordOnly
			&& i < kOutputCount; i++) {
		status = open_output(device->outputs[i], true);
		if (status != B_OK)
			TRACE("%s: %s\n", device->outputs[i]->name, strerror(status));
	}
	device->ready = true;

	void* settings = load_driver_settings("qcom_audio");
	bool tone = get_driver_boolean_parameter(settings, "test_tone", false,
		true);
	bool hdmiTone = get_driver_boolean_parameter(settings, "test_tone_hdmi",
		false, true);
	bool record = get_driver_boolean_parameter(settings, "record_test", false,
		true);
	unload_driver_settings(settings);
	// the headset input, for multi_audio and the lab test
	status = open_capture(device);
	if (status != B_OK)
		TRACE("capture: %s\n", strerror(status));
	if (tone)
		test_tone(&device->headphones);
	if (hdmiTone && device->hdmi.open)
		test_tone(&device->hdmi);
	if (record && device->captureOpen)
		record_test(device);
	return B_OK;
}


// #pragma mark - driver


static float
qcom_audio_supports_device(device_node* parent)
{
	const char* bus;
	const char* compatible;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false)
			!= B_OK || strcmp(bus, "fdt") != 0
		|| sDeviceManager->get_attr_string(parent, "fdt/compatible",
			&compatible, false) != B_OK
		|| strcmp(compatible, "qcom,sc8280xp-sndcard") != 0) {
		return 0.0f;
	}
	return 0.8f;
}


static status_t
qcom_audio_register_device(device_node* parent)
{
	device_attr attrs[] = {
		{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE,
			{ .string = "Qualcomm SC8280XP audio" } },
		{}
	};
	return sDeviceManager->register_node(parent, QCOM_AUDIO_DRIVER_NAME,
		attrs, NULL, NULL);
}


static status_t
qcom_audio_init_driver(device_node* node, void** _cookie)
{
	AudioDevice* device = new(std::nothrow) AudioDevice;
	if (device == NULL)
		return B_NO_MEMORY;
	device->node = node;
	device->ready = false;
	device->startDone = false;
	device->dspBase = 0;
	device->dpAudio = NULL;
	B_INITIALIZE_SPINLOCK(&device->streamLock);
	mutex_init(&device->lock, "qcom audio");
	for (Output* output : device->outputs)
		output->doneSem = create_sem(0, "qcom audio buffer done");
	device->recordArea = -1;
	device->recordBuffers = NULL;
	device->recordSem = create_sem(0, "qcom audio buffer filled");
	device->recordDoneCount = 0;
	device->lastRecorded = -1;
	device->lastRecordedSize = 0;
	device->captureOpen = false;
	device->capturing = false;
	device->recordToRefill = -1;
	device->framesRecorded = 0;
	device->recordedTime = 0;
	status_t status = device->dsp.Init(data_hook, device);
	if (status != B_OK) {
		TRACE("no GPR (qcom_adsp): %s\n", strerror(status));
		delete device;
		return status;
	}
	device->startThread = spawn_kernel_thread(start_thread, "qcom audio start",
		B_NORMAL_PRIORITY, device);
	if (device->startThread >= 0)
		resume_thread(device->startThread);
	*_cookie = device;
	return B_OK;
}


static void
qcom_audio_uninit_driver(void* cookie)
{
	AudioDevice* device = (AudioDevice*)cookie;
	if (device->startThread >= 0) {
		status_t result;
		wait_for_thread(device->startThread, &result);
	}
	device->capture.Close();
	for (Output* output : device->outputs)
		output->path.Close();
	if (device->dpAudio != NULL)
		put_module(QCOM_DP_AUDIO_MODULE_NAME);
	device->codec.PowerDown();
	device->dsp.Uninit();
	device->smmu.Detach();
	for (Output* output : device->outputs) {
		if (output->bufferArea >= 0)
			delete_area(output->bufferArea);
		delete_sem(output->doneSem);
	}
	if (device->recordArea >= 0)
		delete_area(device->recordArea);
	delete_sem(device->recordSem);
	mutex_destroy(&device->lock);
	delete device;
}


static status_t
qcom_audio_register_child_devices(void* cookie)
{
	AudioDevice* device = (AudioDevice*)cookie;
	status_t status = B_OK;
	for (uint32 i = 0; status == B_OK && i < kOutputCount; i++) {
		char path[64];
		snprintf(path, sizeof(path), QCOM_AUDIO_DEVICE_PATH "%" B_PRIu32,
			i + 1);
		status = sDeviceManager->publish_device(device->node, path,
			QCOM_AUDIO_DEVICE_NAME);
	}
	return status;
}


// #pragma mark - device


static status_t
qcom_audio_init_device(void* driverCookie, void** _deviceCookie)
{
	*_deviceCookie = driverCookie;
	return B_OK;
}


static void
qcom_audio_uninit_device(void*)
{
}


//!	The output a published device is: the headphones (1), HDMI (2), DP-1 (3)
//!	or DP-2 (4).
static status_t
qcom_audio_open(void* deviceCookie, const char* path, int, void** _cookie)
{
	AudioDevice* device = (AudioDevice*)deviceCookie;
	// the DSP and the codec come up a while after boot
	for (int i = 0; !device->ready; i++) {
		if (i == 300 || device->startThread < 0 || device->startDone)
			return B_DEV_NOT_READY;
		snooze(100000);
	}
	uint32 index = 0;
	size_t prefix = strlen(QCOM_AUDIO_DEVICE_PATH);
	if (path != NULL && strncmp(path, QCOM_AUDIO_DEVICE_PATH, prefix) == 0)
		index = strtoul(path + prefix, NULL, 10) - 1;
	if (index >= kOutputCount)
		return B_ENTRY_NOT_FOUND;
	Output* output = device->outputs[index];
	if (!output->open)
		return B_DEV_NOT_READY;
	if (atomic_add(&output->openCount, 1) != 0) {
		atomic_add(&output->openCount, -1);
		return B_BUSY;
	}
	*_cookie = output;
	return B_OK;
}


static status_t
qcom_audio_close(void* cookie)
{
	Output* output = (Output*)cookie;
	MutexLocker locker(output->device->lock);
	multi_audio_stop(output);
	return B_OK;
}


static status_t
qcom_audio_free(void* cookie)
{
	atomic_add(&((Output*)cookie)->openCount, -1);
	return B_OK;
}


static status_t
qcom_audio_read(void*, off_t, void*, size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
qcom_audio_write(void*, off_t, const void*, size_t* _length)
{
	*_length = 0;
	return B_NOT_SUPPORTED;
}


static status_t
qcom_audio_control(void* cookie, uint32 operation, void* buffer, size_t length)
{
	return multi_audio_control((Output*)cookie, operation, buffer, length);
}


module_dependency module_dependencies[] = {
	{ B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager },
	{}
};


static driver_module_info sQcomAudioDriver = {
	{
		QCOM_AUDIO_DRIVER_NAME,
		0,
		NULL
	},
	qcom_audio_supports_device,
	qcom_audio_register_device,
	qcom_audio_init_driver,
	qcom_audio_uninit_driver,
	qcom_audio_register_child_devices,
	NULL,
	NULL
};


static device_module_info sQcomAudioDevice = {
	{
		QCOM_AUDIO_DEVICE_NAME,
		0,
		NULL
	},
	qcom_audio_init_device,
	qcom_audio_uninit_device,
	NULL,
	qcom_audio_open,
	qcom_audio_close,
	qcom_audio_free,
	qcom_audio_read,
	qcom_audio_write,
	NULL,
	qcom_audio_control,
	NULL,
	NULL
};


module_info* modules[] = {
	(module_info*)&sQcomAudioDriver,
	(module_info*)&sQcomAudioDevice,
	NULL
};
