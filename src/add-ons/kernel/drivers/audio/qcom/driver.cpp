/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Audio on the Qualcomm SC8280XP (Radxa Dragon Q8B): the board's sound
	card node ("qcom,sc8280xp-sndcard"), played through the audio DSP's
	AudioReach graphs (the board's topology, linux-firmware's
	SC8280XP-Radxa-Dragon-Q8B-tplg.bin) to the WCD9385's headphone output.
	The DSP is run by qcom_adsp; its packets go through qcom_gpr.
*/


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
#define QCOM_AUDIO_DEVICE_PATH "audio/hmulti/qcom/1"


using namespace qcom_audio;


static device_manager_info* sDeviceManager;


static void
data_hook(void* cookie, const qcom_gpr_packet* packet)
{
	AudioDevice* device = (AudioDevice*)cookie;
	if (packet->opcode == Playback::kWriteDone) {
		cpu_status state = disable_interrupts();
		acquire_spinlock(&device->streamLock);
		device->lastPlayed = packet->token % kBufferCount;
		device->framesPlayed += kBufferFrames;
		device->playedTime = system_time();
		release_spinlock(&device->streamLock);
		restore_interrupts(state);
		atomic_add(&device->doneCount, 1);
		release_sem_etc(device->doneSem, 1, B_DO_NOT_RESCHEDULE);
		return;
	}
	TRACE("data event %#" B_PRIx32 " (token %" B_PRIu32 ")\n", packet->opcode,
		packet->token);
}


/*!	The DSP reaches the buffers it is given as stream 0xc01 (the device
	tree's q6apm "dais" node): a context bank of its own that maps only
	them. They are given to it at an I/O address at the top of the low
	4 GiB, where Linux' IOMMU allocator puts them (lab: "dsp_iova" sets
	another; 0 for their physical address).
*/
static status_t
map_dsp_stream(AudioDevice* device)
{
	void* settings = load_driver_settings("qcom_audio");
	device->buffersDsp = strtoull(get_driver_parameter(settings, "dsp_iova",
		"0xfff00000", "0xfff00000"), NULL, 0);
	unload_driver_settings(settings);
	if (device->buffersDsp == 0)
		device->buffersDsp = device->buffersPhysical;
	static const uint32 kStreams[][2] = {{0xc01, 0}};
	status_t status = device->smmu.Attach("qcom_audio", kStreams, 1);
	if (status == B_OK) {
		status = device->smmu.Map(device->buffersDsp,
			device->buffersPhysical,
			ROUNDUP(kBufferCount * kBufferSize, B_PAGE_SIZE), false);
	}
	TRACE("the DSP's buffers at %#" B_PRIx64 " (physical %#" B_PRIxPHYSADDR
		")\n", device->buffersDsp, device->buffersPhysical);
	return status;
}


static status_t
allocate_buffers(AudioDevice* device)
{
	device->bufferArea = create_area("qcom audio buffers",
		(void**)&device->buffers, B_ANY_KERNEL_ADDRESS,
		ROUNDUP(kBufferCount * kBufferSize, B_PAGE_SIZE), B_CONTIGUOUS,
		B_READ_AREA | B_WRITE_AREA);
	if (device->bufferArea < 0)
		return device->bufferArea;
	physical_entry entry;
	get_memory_map(device->buffers, B_PAGE_SIZE, &entry, 1);
	device->buffersPhysical = entry.address;
	memset(device->buffers, 0, kBufferCount * kBufferSize);
	return B_OK;
}


//!	The DSP reads the buffers from memory: our lines of them cleaned first.
void
qcom_audio::clean_buffer(AudioDevice* device, uint32 index)
{
	addr_t start = (addr_t)device->buffers + index * kBufferSize;
	for (addr_t line = start; line < start + kBufferSize; line += 64)
		asm volatile("dc cvac, %0" : : "r" (line) : "memory");
	memory_full_barrier();
}


/*!	Lab ("test_tone" in the qcom_audio settings): a 750 Hz tone for two
	seconds through the graph; how many buffers the DSP played in that time
	says whether it plays at 48 kHz.
*/
static void
test_tone(AudioDevice* device)
{
	// a 750 Hz square wave at a quarter of full scale: 64 frames a period
	for (uint32 index = 0; index < kBufferCount; index++) {
		int16* samples = (int16*)(device->buffers + index * kBufferSize);
		for (uint32 frame = 0; frame < kBufferFrames; frame++) {
			int16 value = (frame & 32) != 0 ? 8192 : -8192;
			samples[frame * 2] = samples[frame * 2 + 1] = value;
		}
		clean_buffer(device, index);
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
	// the RX macro takes the codec DMA's samples before the graph runs, as
	// DAPM powers its path before the trigger in Linux
	device->codec.EnableHeadphonePath(48000);
	// the codec's ports and amplifiers, at -9 dB
	device->codec.EnableHeadphones(6);
	snooze(100000);
	status_t status = device->playback.Start(startDevice);
	TRACE("test tone: graph start: %s\n", strerror(status));
	if (status != B_OK)
		return;
	if (startOnly) {
		snooze(2000000);
		device->playback.Stop();
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
			device->smmu.ClearAccessFlags(device->buffersDsp,
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
		device->playback.Write(device->buffersDsp, kBufferSize, 0);
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
				|| device->doneCount > 0 || i == 299) {
				TRACE("one buffer after %d ms: %" B_PRId32 " done; bank 4 FSR %#"
					B_PRIx32 " FAR %#" B_PRIx32 "%08" B_PRIx32 " FSYNR0 %#"
					B_PRIx32 "\n", i, device->doneCount,
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
					smp_get_current_cpu(), device->doneCount, state,
					(state & 1) != 0 ? ", crashed: " : "",
					(state & 1) != 0 ? reason : "");
				lastState = state;
			}
		}
		device->playback.Stop();
		TRACE("test tone: stopped, %" B_PRId32 " done\n", device->doneCount);
		return;
	}
	bigtime_t start = system_time();
	int32 written = 0;
	for (; written < (int32)kBufferCount; written++) {
		device->playback.Write(device->buffersDsp
			+ (written % kBufferCount) * kBufferSize, kBufferSize, written);
	}
	while (system_time() - start < 2000000) {
		if (acquire_sem_etc(device->doneSem, 1, B_RELATIVE_TIMEOUT, 500000)
				!= B_OK) {
			TRACE("test tone: no buffer done for 0.5 s (%" B_PRId32 " done)\n",
				device->doneCount);
			device->smmu.Faults("test tone");
			break;
		}
		device->playback.Write(device->buffersDsp
			+ (written % kBufferCount) * kBufferSize, kBufferSize, written);
		written++;
	}
	bigtime_t elapsed = system_time() - start;
	int32 done = device->doneCount;
	device->playback.Stop();
	device->codec.DisableHeadphones();
	device->codec.DisableHeadphonePath();
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
	// lab only for now: the codec's bring-up hung the board now and then
	// (docs/dragon-q8b/STATUS.md), so nothing runs without "enabled true"
	void* enableSettings = load_driver_settings("qcom_audio");
	bool enabled = get_driver_boolean_parameter(enableSettings, "enabled",
		false, true);
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
	status = allocate_buffers(device);
	if (status == B_OK)
		status = map_dsp_stream(device);
	if (status == B_OK)
		status = device->playback.Open(48000, 2, 16);
	if (status == B_OK) {
		status = device->playback.MapMemory(device->buffersDsp,
			kBufferCount * kBufferSize);
	}
	if (status != B_OK) {
		TRACE("playback: %s\n", strerror(status));
		return status;
	}
	device->ready = true;

	void* settings = load_driver_settings("qcom_audio");
	bool tone = get_driver_boolean_parameter(settings, "test_tone", false,
		true);
	unload_driver_settings(settings);
	if (tone)
		test_tone(device);
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
	device->bufferArea = -1;
	device->buffers = NULL;
	device->doneCount = 0;
	device->openCount = 0;
	device->running = false;
	device->lastPlayed = device->toRefill = -1;
	device->framesPlayed = 0;
	device->playedTime = 0;
	B_INITIALIZE_SPINLOCK(&device->streamLock);
	mutex_init(&device->lock, "qcom audio");
	device->doneSem = create_sem(0, "qcom audio buffer done");
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
	device->playback.Close();
	device->codec.PowerDown();
	device->dsp.Uninit();
	device->smmu.Detach();
	if (device->bufferArea >= 0)
		delete_area(device->bufferArea);
	delete_sem(device->doneSem);
	mutex_destroy(&device->lock);
	delete device;
}


static status_t
qcom_audio_register_child_devices(void* cookie)
{
	AudioDevice* device = (AudioDevice*)cookie;
	return sDeviceManager->publish_device(device->node, QCOM_AUDIO_DEVICE_PATH,
		QCOM_AUDIO_DEVICE_NAME);
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


static status_t
qcom_audio_open(void* deviceCookie, const char*, int, void** _cookie)
{
	AudioDevice* device = (AudioDevice*)deviceCookie;
	// the DSP and the codec come up a while after boot
	for (int i = 0; !device->ready; i++) {
		if (i == 300 || device->startThread < 0 || device->startDone)
			return B_DEV_NOT_READY;
		snooze(100000);
	}
	if (atomic_add(&device->openCount, 1) != 0) {
		atomic_add(&device->openCount, -1);
		return B_BUSY;
	}
	*_cookie = device;
	return B_OK;
}


static status_t
qcom_audio_close(void* cookie)
{
	AudioDevice* device = (AudioDevice*)cookie;
	MutexLocker locker(device->lock);
	multi_audio_stop(device);
	return B_OK;
}


static status_t
qcom_audio_free(void* cookie)
{
	atomic_add(&((AudioDevice*)cookie)->openCount, -1);
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
	return multi_audio_control((AudioDevice*)cookie, operation, buffer,
		length);
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
