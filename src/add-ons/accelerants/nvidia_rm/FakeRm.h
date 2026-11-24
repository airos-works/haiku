#pragma once

#include <atomic>

#include <SupportDefs.h>

extern "C" {
#include "nvtypes.h"
}


// The GPU of FakeRm.cpp, and what the tests make it do.
struct FakeGpu {
	std::atomic<bool> stalled {false};		// fetches nothing
	std::atomic<bool> raiseError {false};	// fills the error notifiers once
	std::atomic<bool> refuseChannels {false};	// resman says no to channels
	std::atomic<bool> quit {false};
	std::atomic<int32> errors {0};			// misuse of resman seen
	std::atomic<int32> entries {0};			// GPFIFO entries carried out

	void Start();
	void Stop();
	// the channels there are now fetch nothing any more (new ones do)
	void FreezeChannels();
	// live objects of that class (all of them for 0)
	int32 CountObjects(NvV32 hClass);
	bool IsLive(NvHandle handle);
	int32 CountDmaMappings();
};

extern FakeGpu gFakeGpu;
