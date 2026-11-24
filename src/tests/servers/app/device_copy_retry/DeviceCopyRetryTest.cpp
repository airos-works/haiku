/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	device_copy_retry_test: app_server's vertical blank flush when the
	graphics device's copies fail and work again (haiku#201).

	HWInterface is app_server's own (from libasdrawing.a); the graphics
	device is a fake one whose shadows (Bits() is NULL, as in video memory)
	and screen are in memory, whose copies can be made to fail, and whose
	retrace semaphore a thread releases at 60 Hz. Checked:
	- with the device working, the flush copies through it;
	- when it fails, the CPU copies and the screen stays right, and the
	  device is not asked again while it is lost;
	- it is tried again after the retry delay, one call per try while it
	  still fails;
	- once it works again it copies again, and its shadows were brought up
	  to date first (what was drawn while it was lost is in them);
	- a mode set takes it up again at once (_TakeUpDeviceCopies()).
	Before haiku#201 a failure was for good: nothing ever took the device
	up again.

	Prints what it checked; exits 0 when all passed, 1 otherwise. Every wait
	has a deadline.
*/


#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include "HWInterface.h"
#include "MallocBuffer.h"
#include "RenderingBuffer.h"


static const uint32 kWidth = 640;
static const uint32 kHeight = 480;
static int sFailures = 0;


static void
check(bool ok, const char* what)
{
	printf("%s %s\n", ok ? "ok  " : "FAIL", what);
	if (!ok)
		sFailures++;
}


// #pragma mark - what HWInterface.o needs of the rest of app_server


float gRenderScale = 1.0f;

void
font_metrics_changed()
{
}


const color_map*
SystemColorMap()
{
	return NULL;
}


// No cursor and no drawing engine are made here: these are never called.
extern "C" {
#define NOT_HERE(name) void name() { debugger(#name " called"); }
NOT_HERE(_ZN12ServerCursor10SetHotSpotE6BPoint)
NOT_HERE(_ZN12ServerCursorC1E5BRect11color_spacei6BPointi9screen_id)
NOT_HERE(_ZN12ServerCursorC1EPKhjj11color_space)
NOT_HERE(_ZN12ServerCursorC1EPKS_)
NOT_HERE(_ZN12ServerCursorD0Ev)
NOT_HERE(_ZN13DrawingEngineC1EP11HWInterface)
}


// #pragma mark - the fake device


//! A shadow in the device's memory: the CPU has no view of it.
class DeviceBuffer : public RenderingBuffer {
public:
	DeviceBuffer(uint32 width, uint32 height)
		:
		fWidth(width),
		fHeight(height),
		fPixels(new(std::nothrow) uint32[width * height])
	{
		if (fPixels != NULL)
			memset(fPixels, 0, width * height * 4);
	}

	virtual ~DeviceBuffer() { delete[] fPixels; }

	virtual status_t InitCheck() const
		{ return fPixels != NULL ? B_OK : B_NO_MEMORY; }
	virtual color_space ColorSpace() const { return B_RGBA32; }
	virtual void* Bits() const { return NULL; }
	virtual uint32 BytesPerRow() const { return fWidth * 4; }
	virtual uint32 Width() const { return fWidth; }
	virtual uint32 Height() const { return fHeight; }

	uint32* Pixels() const { return fPixels; }

private:
	uint32		fWidth;
	uint32		fHeight;
	uint32*		fPixels;
};


static void
copy_rect(uint8* dst, uint32 dstBPR, const uint8* src, uint32 srcBPR,
	const clipping_rect& r)
{
	for (int32 y = r.top; y <= r.bottom; y++) {
		memcpy(dst + y * dstBPR + r.left * 4, src + y * srcBPR + r.left * 4,
			(r.right - r.left + 1) * 4);
	}
}


class FakeInterface : public HWInterface {
public:
	FakeInterface()
		:
		fFront(kWidth, kHeight),
		fBack(kWidth, kHeight),
		fRetrace(create_sem(0, "fake retrace")),
		fTicker(-1),
		fQuit(false),
		fFailing(false),
		fDeviceCalls(0),
		fDeviceCopies(0)
	{
		memset(fFront.Bits(), 0, fFront.BitsLength());
		memset(fBack.Bits(), 0, fBack.BitsLength());
		for (int32 i = 0; i < 2; i++)
			fShadows[i] = NULL;
		fShadowCount = 0;
	}

	status_t Start()
	{
		fTicker = spawn_thread(_TickerEntry, "fake retrace", B_DISPLAY_PRIORITY,
			this);
		resume_thread(fTicker);
		LockExclusiveAccess();
		status_t status = _StartFlushing();
		UnlockExclusiveAccess();
		return status;
	}

	virtual status_t Shutdown()
	{
		_StopFlushing();
		_ReleaseFlushShadows();
		fQuit = true;
		status_t result;
		wait_for_thread(fTicker, &result);
		delete_sem(fRetrace);
		return B_OK;
	}

	// a mode set's part in this: the device is tried again at once
	void ModeSet()
	{
		LockExclusiveAccess();
		_TakeUpDeviceCopies();
		UnlockExclusiveAccess();
	}

	void SetRetryDelay(bigtime_t delay) { _SetDeviceCopyRetryDelay(delay); }
	bool Lost() const { return _DeviceCopiesLost(); }
	void SetFailing(bool failing) { fFailing = failing; }
	int32 DeviceCalls() const { return atomic_get((int32*)&fDeviceCalls); }
	int32 DeviceCopies() const { return atomic_get((int32*)&fDeviceCopies); }
	int32 CountShadows() const { return fShadowCount; }
	DeviceBuffer* Shadow(int32 i) const { return fShadows[i]; }

	// what a drawing op does: draws into the back buffer, then copies
	void Draw(const clipping_rect& r, uint32 color)
	{
		LockParallelAccess();
		uint8* bits = (uint8*)fBack.Bits();
		for (int32 y = r.top; y <= r.bottom; y++) {
			uint32* row = (uint32*)(bits + y * fBack.BytesPerRow());
			for (int32 x = r.left; x <= r.right; x++)
				row[x] = color;
		}
		CopyBackToFront(BRect(r.left, r.top, r.right, r.bottom));
		UnlockParallelAccess();
	}

	bool ScreenIsBackBuffer()
	{
		WaitForPendingFlush(1000000);
		return memcmp(fFront.Bits(), fBack.Bits(), fBack.BitsLength()) == 0;
	}

	bool ShadowIsBackBuffer(int32 i)
	{
		LockExclusiveAccess();
		bool same = memcmp(fShadows[i]->Pixels(), fBack.Bits(),
			fBack.BitsLength()) == 0;
		UnlockExclusiveAccess();
		return same;
	}

	// #pragma mark - HWInterface

	virtual status_t SetMode(const display_mode& mode) { return B_OK; }

	virtual void GetMode(display_mode* mode)
	{
		memset(mode, 0, sizeof(*mode));
		mode->timing.pixel_clock = 25175;
		mode->timing.h_display = kWidth;
		mode->timing.h_sync_start = 656;
		mode->timing.h_sync_end = 752;
		mode->timing.h_total = 800;
		mode->timing.v_display = kHeight;
		mode->timing.v_sync_start = 490;
		mode->timing.v_sync_end = 492;
		mode->timing.v_total = 525;
		mode->virtual_width = kWidth;
		mode->virtual_height = kHeight;
		mode->space = B_RGB32;
	}

	virtual status_t GetDeviceInfo(accelerant_device_info* info)
		{ return B_UNSUPPORTED; }
	virtual status_t GetModeList(display_mode** _modeList, uint32* _count)
		{ return B_UNSUPPORTED; }
	virtual status_t GetPixelClockLimits(display_mode* mode, uint32* _low,
		uint32* _high)
		{ return B_UNSUPPORTED; }
	virtual status_t GetTimingConstraints(
		display_timing_constraints* constraints)
		{ return B_UNSUPPORTED; }
	virtual status_t ProposeMode(display_mode* candidate,
		const display_mode* low, const display_mode* high)
		{ return B_UNSUPPORTED; }

	virtual sem_id RetraceSemaphore() { return fRetrace; }
	virtual status_t WaitForRetrace(bigtime_t timeout)
		{ return acquire_sem_etc(fRetrace, 1, B_RELATIVE_TIMEOUT, timeout); }

	virtual status_t SetDPMSMode(uint32 state) { return B_UNSUPPORTED; }
	virtual uint32 DPMSMode() { return B_DPMS_ON; }
	virtual uint32 DPMSCapabilities() { return 0; }
	virtual status_t SetBrightness(float) { return B_UNSUPPORTED; }
	virtual status_t GetBrightness(float*) { return B_UNSUPPORTED; }

	virtual RenderingBuffer* FrontBuffer() const
		{ return (RenderingBuffer*)&fFront; }
	virtual RenderingBuffer* BackBuffer() const
		{ return (RenderingBuffer*)&fBack; }
	virtual bool IsDoubleBuffered() const { return true; }

protected:
	// as AccelerantHWInterface has them, with the device's copies made by
	// the CPU here
	virtual RenderingBuffer* _CreateFlushShadow(uint32 width, uint32 height)
	{
		DeviceBuffer* shadow = new(std::nothrow) DeviceBuffer(width, height);
		if (shadow != NULL && fShadowCount < 2)
			fShadows[fShadowCount++] = shadow;
		return shadow;
	}

	virtual bool _DeviceCopiesShadows() const { return !_DeviceCopiesLost(); }

	virtual bool _CopyShadowByDevice(RenderingBuffer* shadow,
		const BRegion& region)
	{
		DeviceBuffer* source = dynamic_cast<DeviceBuffer*>(shadow);
		if (source == NULL || _DeviceCopiesLost())
			return false;
		atomic_add(&fDeviceCalls, 1);
		if (fFailing) {
			_DeviceCopyFailed("copy to the screen");
			return false;
		}
		for (int32 i = 0; i < region.CountRects(); i++) {
			copy_rect((uint8*)fFront.Bits(), fFront.BytesPerRow(),
				(const uint8*)source->Pixels(), source->BytesPerRow(),
				region.RectAtInt(i));
		}
		atomic_add(&fDeviceCopies, 1);
		_DeviceCopyWorked();
		return true;
	}

	virtual bool _CopyBackToShadowByDevice(RenderingBuffer* shadow,
		const IntRect& area)
	{
		DeviceBuffer* target = dynamic_cast<DeviceBuffer*>(shadow);
		if (target == NULL || _DeviceCopiesLost())
			return false;
		atomic_add(&fDeviceCalls, 1);
		if (fFailing) {
			_DeviceCopyFailed("copy the back buffer");
			return false;
		}
		clipping_rect r = { area.left, area.top, area.right, area.bottom };
		copy_rect((uint8*)target->Pixels(), target->BytesPerRow(),
			(const uint8*)fBack.Bits(), fBack.BytesPerRow(), r);
		atomic_add(&fDeviceCopies, 1);
		_DeviceCopyWorked();
		return true;
	}

private:
	static status_t _TickerEntry(void* data)
	{
		FakeInterface* self = (FakeInterface*)data;
		bigtime_t next = system_time();
		while (!self->fQuit) {
			next += 16667;
			snooze_until(next, B_SYSTEM_TIMEBASE);
			// every waiter goes, as the accelerants release it
			int32 count;
			if (get_sem_count(self->fRetrace, &count) == B_OK && count < 0)
				release_sem_etc(self->fRetrace, -count, 0);
		}
		return B_OK;
	}

	MallocBuffer		fFront;
	MallocBuffer		fBack;
	sem_id				fRetrace;
	thread_id			fTicker;
	volatile bool		fQuit;
	volatile bool		fFailing;
	int32				fDeviceCalls;
	int32				fDeviceCopies;
	DeviceBuffer*		fShadows[2];
	int32				fShadowCount;
};


static void
crashed(int signal)
{
	printf("FAIL crashed with signal %d\n", signal);
	_exit(1);
}


static clipping_rect
rect(int32 left, int32 top, int32 right, int32 bottom)
{
	clipping_rect r = { left, top, right, bottom };
	return r;
}


//! Waits up to \a timeout for the device to be taken up again.
static bool
wait_until_found(FakeInterface& interface, bigtime_t timeout)
{
	bigtime_t deadline = system_time() + timeout;
	while (interface.Lost() && system_time() < deadline)
		snooze(10000);
	return !interface.Lost();
}


int
main()
{
	signal(SIGSEGV, crashed);
	signal(SIGBUS, crashed);
	signal(SIGILL, crashed);
	// nothing here may hang for good
	alarm(60);
	signal(SIGALRM, crashed);

	FakeInterface interface;
	interface.SetRetryDelay(300000);
	if (interface.Start() != B_OK) {
		printf("FAIL the flush did not start\n");
		return 1;
	}
	check(interface.CountShadows() == 2, "two shadows in the device's memory");

	// 1. the device works
	interface.Draw(rect(10, 10, 200, 100), 0xff112233);
	check(interface.ScreenIsBackBuffer(), "device working: screen right");
	check(interface.DeviceCopies() > 0, "device working: the device copied");
	check(!interface.Lost(), "device working: not lost");

	// 2. it fails: the CPU copies, the screen stays right
	interface.SetFailing(true);
	interface.Draw(rect(50, 200, 400, 300), 0xff445566);
	check(interface.Lost(), "device failed: lost");
	check(interface.ScreenIsBackBuffer(), "device failed: screen right "
		"(the CPU copied from the back buffer)");

	// 3. while it is lost it is not asked
	int32 calls = interface.DeviceCalls();
	interface.Draw(rect(300, 20, 600, 150), 0xff778899);
	check(interface.ScreenIsBackBuffer(), "device lost: screen right");
	check(interface.DeviceCalls() == calls, "device lost: not asked");

	// 4. tried again after the delay (300 ms, then 600 ms): one call per
	// try while it still fails
	snooze(1100000);
	int32 tries = interface.DeviceCalls() - calls;
	printf("     %d tries in 1.1 s while it fails\n", (int)tries);
	check(tries >= 1 && tries <= 3, "still failing: tried again, one call "
		"per try, less often each time");
	check(interface.Lost(), "still failing: lost");

	// 5. drawn while lost, then the device works again
	interface.Draw(rect(0, 400, 639, 479), 0xffaabbcc);
	interface.SetFailing(false);
	bool found = wait_until_found(interface, 3000000);
	check(found, "device working again: taken up within 3 s");
	check(interface.ShadowIsBackBuffer(0) && interface.ShadowIsBackBuffer(1),
		"device working again: its shadows have what was drawn meanwhile");
	int32 copies = interface.DeviceCopies();
	interface.Draw(rect(100, 100, 500, 450), 0xffddeeff);
	check(interface.ScreenIsBackBuffer(), "device working again: screen "
		"right");
	check(interface.DeviceCopies() > copies, "device working again: the "
		"device copies");

	// 6. a mode set takes it up at once, whatever the delay
	interface.SetRetryDelay(100000000);
	interface.SetFailing(true);
	interface.Draw(rect(5, 5, 60, 60), 0xff010203);
	check(interface.Lost(), "mode set: lost before");
	interface.Draw(rect(20, 300, 620, 320), 0xff040506);
	interface.SetFailing(false);
	interface.ModeSet();
	check(!interface.Lost(), "mode set: taken up at once");
	check(interface.ShadowIsBackBuffer(0) && interface.ShadowIsBackBuffer(1),
		"mode set: shadows up to date");
	copies = interface.DeviceCopies();
	interface.Draw(rect(200, 200, 300, 300), 0xff070809);
	check(interface.ScreenIsBackBuffer(), "mode set: screen right");
	check(interface.DeviceCopies() > copies, "mode set: the device copies");

	// 7. a mode set while it still fails: one call, lost again, screen right
	interface.SetFailing(true);
	interface.Draw(rect(0, 0, 10, 10), 0xff0a0b0c);
	calls = interface.DeviceCalls();
	interface.ModeSet();
	check(interface.Lost() && interface.DeviceCalls() - calls == 1,
		"mode set while failing: one call, still lost");
	interface.Draw(rect(30, 30, 40, 40), 0xff0d0e0f);
	check(interface.ScreenIsBackBuffer(), "mode set while failing: screen "
		"right");

	interface.Shutdown();
	printf("%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
