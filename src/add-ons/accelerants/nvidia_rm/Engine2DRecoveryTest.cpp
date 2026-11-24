/* Checks NvEngine2D::Recover() against a resman and GPU in memory
 * (FakeRm.cpp): no card needed, it runs on any x86_64 air/OS (haiku#201).
 *
 * The engine is lost in the three ways it can be - a submission not done in
 * time (the GPU stalls), the channel's error notifier, the test setting
 * (LoseAt()) - and set up again each time. Checked: the address space and
 * every buffer in it stay where they were (the frame buffer's import, the
 * sources); everything of the old channel is freed, in resman's order, and
 * made anew; work runs on the new channel; work of before the loss counts as
 * done when it was, and as not done (without losing the engine again) when
 * it went with the channel; a caller waiting without the lock is let out
 * before anything it reads is freed; a recovery that fails (the GPU still
 * stalls, resman refuses the channel) leaves the engine lost and nothing
 * behind, and the next one works; nothing leaks over many rounds.
 *
 * usage: nv2drecoverytest [rounds]
 * Prints what it checked; exits 0 when all passed, 1 otherwise.
 */
#include <memory>

#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include <OS.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "FakeRm.h"
#include "NvEngine2D.h"

extern "C" {
#include "nvos.h"
#include "class/cl0040.h"
#include "class/cl902d.h"
#include "class/clc06f.h"
}


static int sFailures = 0;


static void Check(bool ok, const char *what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		sFailures++;
}


static void Crashed(int signal)
{
	printf("FAIL crashed with signal %d (freed memory read?)\n", signal);
	_exit(1);
}


struct Waiter {
	NvEngine2D *engine;
	uint32 sequence;
	volatile bool done = false;
	bool result = true;
};


static status_t WaiterThread(void *data)
{
	Waiter *waiter = (Waiter*)data;
	// as CopyFrameBufferSource() waits: without the lock, sleeping
	waiter->result = waiter->engine->Wait(waiter->sequence, 5000000, false);
	waiter->done = true;
	return B_OK;
}


int main(int argc, char **argv)
{
	signal(SIGSEGV, Crashed);
	signal(SIGBUS, Crashed);
	signal(SIGALRM, Crashed);
	alarm(120);
	int rounds = argc > 1 ? atoi(argv[1]) : 50;

	gFakeGpu.Start();
	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);
		int32 objectsBefore = gFakeGpu.CountObjects(0);

		std::unique_ptr<NvEngine2D> enginePointer
			= std::make_unique<NvEngine2D>(rm, rmDev);
		NvEngine2D &engine = *enginePointer;
		std::unique_lock<std::recursive_mutex> lock(engine.Lock());
		Check(!engine.IsLost() && gFakeGpu.entries > 0,
			"engine up, its first submission done");

		// what the accelerant keeps in the address space: the frame buffer
		// (somebody else's memory, imported) and app_server's sources
		NV_MEMORY_ALLOCATION_PARAMS fbParams {};
		fbParams.size = 1024 * 768 * 4;
		NvRmObject frameBuffer = rmDev.Device().Alloc(NV01_MEMORY_LOCAL_USER,
			&fbParams);
		NvEngine2D::Buffer target = engine.Import(frameBuffer.Get(),
			fbParams.size);
		NvEngine2D::Buffer source = engine.Allocate(1024 * 768 * 4, true);
		NvEngine2D::Buffer shadow = engine.Allocate(1024 * 768 * 4, false,
			false);
		NvU64 targetAddress = target.GpuAddress();
		NvU64 sourceAddress = source.GpuAddress();
		NvU64 shadowAddress = shadow.GpuAddress();
		NvEngine2D::Surface screen;
		screen.gpuAddress = targetAddress;
		screen.width = 1024;
		screen.height = 768;
		screen.bytesPerRow = 4096;
		engine.SetTarget(screen);
		NvEngine2D::Surface back = screen;
		back.gpuAddress = sourceAddress;
		int32 objectsUp = gFakeGpu.CountObjects(0);
		int32 mappingsUp = gFakeGpu.CountDmaMappings();
		clipping_rect all = {0, 0, 1023, 767};

		auto works = [&](const char *what) {
			engine.Copy(back, screen, &all, 1);
			fill_rect_params fill = {0, 0, 99, 99};
			engine.FillRectangles(0x123456, &fill, 1);
			uint32 sequence = engine.Submit();
			bool ok = engine.Wait(sequence, 1000000) && !engine.IsLost();
			char line[256];
			snprintf(line, sizeof(line), "%s: work runs", what);
			Check(ok, line);
		};
		auto kept = [&](const char *what) {
			char line[256];
			snprintf(line, sizeof(line), "%s: buffers where they were, "
				"nothing leaked", what);
			Check(target.GpuAddress() == targetAddress
				&& source.GpuAddress() == sourceAddress
				&& shadow.GpuAddress() == shadowAddress
				&& gFakeGpu.CountObjects(0) == objectsUp
				&& gFakeGpu.CountDmaMappings() == mappingsUp
				&& gFakeGpu.errors == 0, line);
		};
		works("before any loss");

		// 1. the GPU stalls: a submission is not done in time
		uint32 done = engine.Submit();
		gFakeGpu.stalled = true;
		engine.Copy(back, screen, &all, 1);
		uint32 stuck = engine.Submit();
		Check(!engine.Wait(stuck, 100000) && engine.IsLost(),
			"stall: lost after the timeout");
		engine.Copy(back, screen, &all, 1);
		Check(engine.Submit() == stuck, "stall: a lost engine takes no work");
		NvHandle channelsBefore = gFakeGpu.CountObjects(PASCAL_CHANNEL_GPFIFO_A);
		gFakeGpu.stalled = false;
		Check(engine.Recover() && !engine.IsLost() && engine.Recoveries() == 1,
			"stall: set up again");
		Check(gFakeGpu.CountObjects(PASCAL_CHANNEL_GPFIFO_A) == channelsBefore
			&& gFakeGpu.CountObjects(FERMI_TWOD_A) == 1,
			"stall: one channel, one 2D object (the old ones freed)");
		kept("stall");
		Check(engine.Wait(done, 100000), "stall: work done before counts as done");
		Check(!engine.Wait(stuck, 100000) && !engine.IsLost(),
			"stall: work that went with the channel counts as not done, "
			"and waiting for it does not lose the engine");
		works("stall");

		// 2. the channel's error notifier
		gFakeGpu.raiseError = true;
		snooze(5000);
		engine.Copy(back, screen, &all, 1);
		Check(!engine.Wait(engine.Submit(), 1000000) && engine.IsLost(),
			"channel error: lost");
		Check(engine.Recover() && !engine.IsLost(), "channel error: set up again");
		kept("channel error");
		works("channel error");

		// 3. the test setting, with a caller waiting without the lock: it
		// is let out before what it reads is freed (the channel's work is
		// never done; a new channel works)
		gFakeGpu.FreezeChannels();
		engine.Copy(back, screen, &all, 1);
		Waiter waiter;
		waiter.engine = &engine;
		waiter.sequence = engine.Submit();
		thread_id thread = spawn_thread(WaiterThread, "waiter",
			B_NORMAL_PRIORITY, &waiter);
		resume_thread(thread);
		lock.unlock();
		snooze(20000);
		lock.lock();
		engine.LoseAt(1);
		Check(!engine.Wait(engine.Submit(), 1000000) && engine.IsLost(),
			"test setting: lost");
		Check(engine.Recover() && !engine.IsLost(), "test setting: set up "
			"again while a caller waited without the lock");
		status_t result;
		wait_for_thread(thread, &result);
		Check(waiter.done && !waiter.result, "test setting: the waiter was "
			"told its work is not done");
		kept("test setting");
		works("test setting");

		// 4. a recovery that fails: the GPU still stalls, then resman
		// refuses the channel; the next one works
		gFakeGpu.stalled = true;
		engine.LoseAt(1);
		engine.Wait(engine.Submit());
		bigtime_t start = system_time();
		Check(!engine.Recover() && engine.IsLost(),
			"GPU still stalled: the recovery fails, the engine stays lost");
		printf("     (gave up after %lld ms)\n",
			(long long)(system_time() - start) / 1000);
		Check(gFakeGpu.CountObjects(PASCAL_CHANNEL_GPFIFO_A) == 0
			&& gFakeGpu.CountObjects(FERMI_TWOD_A) == 0
			&& gFakeGpu.errors == 0,
			"GPU still stalled: no channel left behind");
		gFakeGpu.stalled = false;
		gFakeGpu.refuseChannels = true;
		Check(!engine.Recover() && engine.IsLost()
			&& gFakeGpu.CountObjects(PASCAL_CHANNEL_GPFIFO_A) == 0
			&& gFakeGpu.errors == 0,
			"channel refused: the recovery fails, nothing left behind");
		gFakeGpu.refuseChannels = false;
		Check(engine.Recover() && !engine.IsLost(), "then: set up again");
		kept("after failed recoveries");
		works("after failed recoveries");

		// 5. many rounds
		bool all_ok = true;
		for (int i = 0; i < rounds && all_ok; i++) {
			engine.LoseAt(1);
			engine.Copy(back, screen, &all, 1);
			engine.Wait(engine.Submit());
			all_ok = engine.IsLost() && engine.Recover() && !engine.IsLost();
			engine.Copy(back, screen, &all, 1);
			all_ok = all_ok && engine.Wait(engine.Submit(), 1000000);
		}
		char line[128];
		snprintf(line, sizeof(line), "%d rounds of loss and recovery", rounds);
		Check(all_ok, line);
		kept("after the rounds");

		// the engine goes, and with it everything it made
		lock.unlock();
		shadow.Release();
		source.Release();
		target.Release();
		frameBuffer = NvRmObject();
		enginePointer.reset();
		Check(gFakeGpu.CountObjects(0) == objectsBefore && gFakeGpu.errors == 0,
			"engine gone: everything freed");
	} catch (const std::system_error &ex) {
		printf("FAIL threw: %s\n", ex.what());
		sFailures++;
	}
	gFakeGpu.Stop();
	printf("%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
