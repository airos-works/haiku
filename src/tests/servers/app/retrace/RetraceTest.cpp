/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Is drawing that asks for B_WAIT_FOR_RETRACE timed to the display?

	retrace_test blanks [seconds]
		BScreen::WaitForRetrace() in a loop: how far apart the blanks are.
		They should come at the display's refresh rate, evenly.
	retrace_test draw [seconds] [--no-wait] [--update] [--size WxH]
		a window drawing a full bitmap a frame with B_WAIT_FOR_RETRACE
		(--no-wait: without), Sync() after each; frames per second. With
		--update the bitmap is drawn in Draw(), in an update: the copy to
		the screen waits for the update's end.
	retrace_test lines --line ...
		the scan line source's raw register, every 100 µs for 40 ms (to
		check how it counts).

	Where the display's scan line can be read, both also tell where the
	scan was: after a blank, how far past its start; after a drawing, how
	far past the blank its copy to the screen ended. Copies that wait for
	the blank end early in the frame, ahead of the scan, which they
	outpace; without, they end anywhere. Scan line sources:
		--line sunxi	the Cubie A7S (TCON-TV1; the lab's driver settings
						allow the register reads)
		--line amdgpu	the Radeon Pro WX 5100 (the DCE head showing the
						boot frame buffer)
*/


#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <vector>

#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>

#include <amdgpu_haiku.h>
#include <sunxi_display.h>


// #pragma mark - scan line


struct ScanLine {
	virtual			~ScanLine() {}
	// the line, and the frame's lines and where its blank starts
	virtual	bool	Read(int32& line, int32& total, int32& blankStart) = 0;
	// the register the line comes from, as read last
			uint32	raw;
};


/*!	TCON-TV1's debug register: the line it sends in bits 27..16, counted
	from the start of the vertical sync; the active lines start after the
	sync and the back porch (_InitTcon(): BASIC4/5).
*/
struct SunxiScanLine : ScanLine {
	SunxiScanLine()
	{
		fDevice = open("/dev/" SUNXI_DISPLAY_DEVICE, O_RDWR);
	}

	~SunxiScanLine()
	{
		if (fDevice >= 0)
			close(fDevice);
	}

	uint32 Register(uint64 address)
	{
		sunxi_display_register request = {};
		request.address = address;
		if (ioctl(fDevice, SUNXI_DISPLAY_DEBUG_REGISTER, &request,
				sizeof(request)) != 0) {
			return 0xffffffff;
		}
		return request.value;
	}

	bool Read(int32& line, int32& total, int32& blankStart)
	{
		if (fDevice < 0)
			return false;
		uint32 debug = Register(0x057310fc);
		uint32 basic4 = Register(0x057310a4);
		uint32 basic1 = Register(0x0573109c);
		if (debug == 0xffffffff || basic4 == 0xffffffff)
			return false;
		raw = debug;
		total = (basic4 >> 16) / 2;
		int32 vsyncAndBackPorch = (basic4 & 0xfff) + 1;
		int32 active = (basic1 & 0xfff) + 1;
		// as lines from the start of the active area
		line = ((int32)((debug >> 16) & 0xfff) - vsyncAndBackPorch + total)
			% total;
		blankStart = active;
		return true;
	}

	int fDevice;
};


/*!	CRTC_STATUS_POSITION of the head that shows the boot frame buffer (the
	first enabled one). The DCE counts lines from the start of the vertical
	sync; the blank runs from CRTC_V_BLANK_START to CRTC_V_BLANK_END.
*/
struct AmdgpuScanLine : ScanLine {
	AmdgpuScanLine()
	{
		fDevice = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	}

	~AmdgpuScanLine()
	{
		if (fDevice >= 0)
			close(fDevice);
	}

	bool Read(int32& line, int32& total, int32& blankStart)
	{
		if (fDevice < 0)
			return false;
		amdgpu_display_snapshot snapshot = {};
		snapshot.version = AMDGPU_HAIKU_ABI_VERSION;
		snapshot.size = sizeof(snapshot);
		if (ioctl(fDevice, AMDGPU_DISPLAY_SNAPSHOT, &snapshot,
				sizeof(snapshot)) != 0) {
			return false;
		}
		for (uint32 i = 0; i < snapshot.head_count; i++) {
			const uint32* r = snapshot.heads[i].registers;
			if ((r[AMDGPU_DCE_CRTC_CONTROL] & 1) == 0)
				continue;
			raw = r[AMDGPU_DCE_POSITION];
			line = raw & 0x3fff;
			total = (r[AMDGPU_DCE_V_TOTAL] & 0x3fff) + 1;
			blankStart = r[AMDGPU_DCE_V_BLANK] & 0x3fff;
			return true;
		}
		return false;
	}

	int fDevice;
};


static ScanLine*
scan_line_source(const char* name)
{
	if (name == NULL)
		return NULL;
	ScanLine* source = NULL;
	if (strcmp(name, "sunxi") == 0)
		source = new SunxiScanLine;
	else if (strcmp(name, "amdgpu") == 0)
		source = new AmdgpuScanLine;
	else {
		fprintf(stderr, "unknown scan line source %s\n", name);
		exit(1);
	}
	int32 line, total, blankStart;
	if (!source->Read(line, total, blankStart)) {
		fprintf(stderr, "cannot read the scan line (%s)\n", name);
		exit(1);
	}
	printf("scan line source %s: %" B_PRId32 " lines, blank from line %"
		B_PRId32 "\n", name, total, blankStart);
	return source;
}


/*!	How far into the frame, counted from the start of the blank, as a
	fraction of the frame.
*/
static bool
phase(ScanLine* source, double& fraction)
{
	int32 line, total, blankStart;
	if (source == NULL || !source->Read(line, total, blankStart) || total <= 0)
		return false;
	fraction = (double)((line - blankStart + total) % total) / total;
	return true;
}


static void
print_phases(const char* what, std::vector<double>& phases)
{
	if (phases.empty())
		return;
	std::sort(phases.begin(), phases.end());
	size_t count = phases.size();
	int buckets[10] = {};
	for (size_t i = 0; i < count; i++)
		buckets[std::min(9, (int)(phases[i] * 10))]++;
	printf("%s, as a fraction of the frame from the blank's start "
		"(%zu samples):\n  median %.3f, 90%% %.3f, max %.3f\n  ", what, count,
		phases[count / 2], phases[count * 9 / 10], phases[count - 1]);
	for (int i = 0; i < 10; i++)
		printf("%.1f-:%d ", i / 10.0, buckets[i]);
	printf("\n");
}


// #pragma mark - blanks


static int
test_blanks(double seconds, ScanLine* source)
{
	BScreen screen;
	display_mode mode;
	double refresh = 0;
	if (screen.GetMode(&mode) == B_OK && mode.timing.h_total != 0
		&& mode.timing.v_total != 0) {
		refresh = mode.timing.pixel_clock * 1000.0
			/ ((double)mode.timing.h_total * mode.timing.v_total);
	}
	printf("display mode %ux%u, %.2f Hz\n", mode.virtual_width,
		mode.virtual_height, refresh);

	status_t status = screen.WaitForRetrace(1000000);
	if (status != B_OK) {
		printf("FAIL: no blank: %s\n", strerror(status));
		return 1;
	}

	std::vector<bigtime_t> intervals;
	std::vector<double> phases;
	int32 timeouts = 0;
	bigtime_t start = system_time();
	bigtime_t last = start;
	while (system_time() - start < (bigtime_t)(seconds * 1000000)) {
		status = screen.WaitForRetrace(100000);
		bigtime_t now = system_time();
		double fraction;
		if (phase(source, fraction))
			phases.push_back(fraction);
		if (status != B_OK) {
			timeouts++;
			last = now;
			continue;
		}
		intervals.push_back(now - last);
		last = now;
	}

	if (intervals.empty()) {
		printf("FAIL: no blanks in %.1f s (%" B_PRId32 " timeouts)\n",
			seconds, timeouts);
		return 1;
	}
	double sum = 0, squares = 0;
	bigtime_t shortest = intervals[0], longest = intervals[0];
	for (bigtime_t interval : intervals) {
		sum += interval;
		squares += (double)interval * interval;
		shortest = std::min(shortest, interval);
		longest = std::max(longest, interval);
	}
	double mean = sum / intervals.size();
	double deviation = sqrt(std::max(0.0, squares / intervals.size()
		- mean * mean));
	printf("%zu blanks in %.1f s, %" B_PRId32 " timeouts: every %.3f ms "
		"(%.2f Hz), deviation %.3f ms, %.3f to %.3f ms\n", intervals.size(),
		seconds, timeouts, mean / 1000, 1000000 / mean, deviation / 1000,
		shortest / 1000.0, longest / 1000.0);
	print_phases("where the scan was when the wait ended", phases);

	// A frame buffer driver may report a mode that is not the display's
	// (the boot frame buffer's), so the rhythm counts, not the mode.
	if (refresh > 0 && fabs(mean - 1000000 / refresh) > 1000000 / refresh * 0.02)
		printf("(the mode says %.2f Hz: not the display's timing)\n", refresh);
	bool good = timeouts == 0 && deviation < mean * 0.05;
	printf("%s\n", good ? "PASS" : "FAIL");
	return good ? 0 : 1;
}


// #pragma mark - draw


class DrawView : public BView {
public:
	DrawView(BRect frame)
		:
		BView(frame, "draw", B_FOLLOW_ALL, B_WILL_DRAW),
		fBitmap(NULL),
		fOptions(0)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
	}

	void SetBitmap(BBitmap* bitmap, uint32 options)
	{
		fBitmap = bitmap;
		fOptions = options;
	}

	void Draw(BRect)
	{
		if (fBitmap == NULL) {
			SetHighColor(0, 0, 0);
			FillRect(Bounds());
			return;
		}
		DrawBitmap(fBitmap, fBitmap->Bounds(), Bounds(), fOptions);
	}

private:
	BBitmap*	fBitmap;
	uint32		fOptions;
};


/*!	One grey: the frames alternate between two, so that a copy that tears
	shows both at once (a capture of the display tells), and only greys not
	far apart, so that the flicker is not hard on whoever watches. */
static BBitmap*
make_bitmap(BRect bounds, uint8 value)
{
	BBitmap* bitmap = new BBitmap(bounds, B_RGB32);
	uint32* bits = (uint32*)bitmap->Bits();
	int32 count = bitmap->BitsLength() / 4;
	for (int32 i = 0; i < count; i++)
		bits[i] = 0xff000000 | value * 0x010101;
	return bitmap;
}


static int
test_draw(double seconds, bool wait, bool update, int32 width, int32 height,
	ScanLine* source)
{
	BRect frame(50, 50, 50 + width - 1, 50 + height - 1);
	BWindow* window = new BWindow(frame, "retrace_test", B_TITLED_WINDOW,
		B_NOT_RESIZABLE | B_NOT_ZOOMABLE);
	DrawView* view = new DrawView(window->Bounds());
	window->AddChild(view);
	window->Show();
	snooze(500000);

	window->Lock();
	BRect bounds = view->Bounds();
	window->Unlock();
	BBitmap* bitmaps[2] = {
		make_bitmap(bounds, 96), make_bitmap(bounds, 160)
	};
	uint32 options = wait ? B_WAIT_FOR_RETRACE : 0;

	std::vector<double> phases;
	int32 frames = 0;
	bigtime_t start = system_time();
	while (system_time() - start < (bigtime_t)(seconds * 1000000)) {
		if (!window->Lock())
			break;
		if (update) {
			view->SetBitmap(bitmaps[frames & 1], options);
			view->Invalidate();
			window->UpdateIfNeeded();
		} else
			view->DrawBitmap(bitmaps[frames & 1], bounds, bounds, options);
		view->Sync();
		window->Unlock();
		double fraction;
		if (phase(source, fraction))
			phases.push_back(fraction);
		frames++;
	}
	bigtime_t elapsed = system_time() - start;

	printf("%" B_PRId32 " frames of %" B_PRId32 "x%" B_PRId32 " in %.1f s "
		"(%.1f a second), %s B_WAIT_FOR_RETRACE%s\n", frames, width, height,
		elapsed / 1000000.0, frames * 1000000.0 / elapsed,
		wait ? "with" : "without", update ? ", in updates" : "");
	print_phases("where the scan was when the copy had ended", phases);

	window->Lock();
	window->Quit();
	delete bitmaps[0];
	delete bitmaps[1];
	return 0;
}


static int
test_lines(ScanLine* source)
{
	if (source == NULL) {
		fprintf(stderr, "lines needs --line\n");
		return 1;
	}
	bigtime_t start = system_time();
	for (int i = 0; i < 400; i++) {
		int32 line, total, blankStart;
		bigtime_t when = system_time();
		if (!source->Read(line, total, blankStart))
			return 1;
		printf("%7.3f ms raw %#010" B_PRIx32 " line %" B_PRId32 "\n",
			(when - start) / 1000.0, source->raw, line);
		snooze_until(start + (i + 1) * 100, B_SYSTEM_TIMEBASE);
	}
	return 0;
}


// #pragma mark -


static void
usage()
{
	fprintf(stderr, "usage: retrace_test blanks [seconds] [--line "
		"sunxi|amdgpu]\n"
		"       retrace_test draw [seconds] [--no-wait] [--update] [--size WxH] "
		"[--line sunxi|amdgpu]\n"
		"       retrace_test lines --line sunxi|amdgpu\n");
	exit(1);
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc < 2)
		usage();

	const char* test = argv[1];
	double seconds = 5;
	bool wait = true;
	bool update = false;
	int32 width = 640, height = 480;
	const char* line = NULL;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--no-wait") == 0)
			wait = false;
		else if (strcmp(argv[i], "--update") == 0)
			update = true;
		else if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
			if (sscanf(argv[++i], "%" B_SCNd32 "x%" B_SCNd32, &width, &height)
					!= 2)
				usage();
		} else if (strcmp(argv[i], "--line") == 0 && i + 1 < argc)
			line = argv[++i];
		else if (argv[i][0] != '-')
			seconds = atof(argv[i]);
		else
			usage();
	}

	BApplication app("application/x-vnd.airos-retrace-test");
	ScanLine* source = scan_line_source(line);

	int result;
	if (strcmp(test, "blanks") == 0)
		result = test_blanks(seconds, source);
	else if (strcmp(test, "draw") == 0)
		result = test_draw(seconds, wait, update, width, height, source);
	else if (strcmp(test, "lines") == 0)
		result = test_lines(source);
	else
		usage();

	delete source;
	return result;
}
