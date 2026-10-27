// scan_timing_test: checks BScreen::GetScanTiming() (summit#119, haiku#39).
//   scan_timing_test            outputs for parts of the screen
//   scan_timing_test phase N    N calls 500 ms apart: phase drift and call time
//   scan_timing_test stale      calls after 2 s of a quiet desktop
//   scan_timing_test retrace N  N WaitForRetrace() wakes against the prediction
#include <Application.h>
#include <Screen.h>
#include <OS.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void
print(const char* what, const display_scan_timing& t, bigtime_t took)
{
	printf("%-12s output %u flags %s frame %.0f,%.0f-%.0f,%.0f device "
		"%d,%d-%d,%d start %lld period %lld gen %u (%.2f ms)\n", what,
		(unsigned)t.output_id,
		t.flags & B_SCAN_TIMING_FROM_HARDWARE ? "hardware"
			: t.flags & B_SCAN_TIMING_FROM_BLANK ? "blank" : "none",
		t.frame.left, t.frame.top, t.frame.right, t.frame.bottom,
		(int)t.device_frame.left, (int)t.device_frame.top,
		(int)t.device_frame.right, (int)t.device_frame.bottom,
		(long long)t.scan_start, (long long)t.period,
		(unsigned)t.generation, took / 1000.0);
}


static status_t
get(BScreen& screen, BRect frame, display_scan_timing& t, bigtime_t* took)
{
	bigtime_t t0 = system_time();
	status_t status = screen.GetScanTiming(frame, &t);
	*took = system_time() - t0;
	return status;
}


// the offset of a from the frame grid through b, in -period/2..period/2
static double
phase(bigtime_t a, bigtime_t b, bigtime_t period)
{
	double d = fmod(double(a - b), double(period));
	if (d > period / 2.0)
		d -= period;
	if (d < -period / 2.0)
		d += period;
	return d;
}


int
main(int argc, char** argv)
{
	BApplication app("application/x-vnd.airos-scantiming");
	BScreen screen;
	BRect f = screen.Frame();
	display_scan_timing t;
	bigtime_t took;
	const char* mode = argc > 1 ? argv[1] : "parts";
	int count = argc > 2 ? atoi(argv[2]) : 20;

	if (strcmp(mode, "parts") == 0) {
		struct { const char* name; BRect r; } parts[] = {
			{"screen", f},
			{"invalid", BRect()},
			{"left", BRect(f.left, f.top, f.left + f.Width() / 2 - 1, f.bottom)},
			{"right", BRect(f.left + f.Width() / 2, f.top, f.right, f.bottom)},
			{"topleft", BRect(f.left, f.top, f.left + 99, f.top + 99)},
			{"botright", BRect(f.right - 99, f.bottom - 99, f.right, f.bottom)},
			{"outside", BRect(f.right + 100, 0, f.right + 200, 100)},
		};
		for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
			status_t status = get(screen, parts[i].r, t, &took);
			if (status != B_OK)
				printf("%-12s %s\n", parts[i].name, strerror(status));
			else
				print(parts[i].name, t, took);
		}
		return 0;
	}

	if (strcmp(mode, "phase") == 0) {
		display_scan_timing first;
		if (get(screen, f, first, &took) != B_OK)
			return 1;
		print("first", first, took);
		double worst = 0, sum = 0;
		bigtime_t worstTook = 0;
		for (int i = 0; i < count; i++) {
			snooze(500000);
			if (get(screen, f, t, &took) != B_OK)
				return 1;
			double d = phase(t.scan_start, first.scan_start, first.period);
			if (fabs(d) > worst)
				worst = fabs(d);
			sum += took;
			if (took > worstTook)
				worstTook = took;
			printf("%2d phase %+8.1f us period %lld flags %u call %.3f ms\n", i, d,
				(long long)t.period, (unsigned)t.flags, took / 1000.0);
		}
		printf("phase: worst %.1f us; call mean %.3f ms worst %.3f ms\n", worst,
			sum / count / 1000.0, worstTook / 1000.0);
		return 0;
	}

	if (strcmp(mode, "stale") == 0) {
		for (int i = 0; i < 3; i++) {
			snooze(2000000);
			if (get(screen, f, t, &took) != B_OK)
				return 1;
			print("after 2 s", t, took);
		}
		return 0;
	}

	if (strcmp(mode, "retrace") == 0) {
		if (get(screen, f, t, &took) != B_OK)
			return 1;
		print("timing", t, took);
		double worst = 0, sum = 0;
		int n = 0;
		for (int i = 0; i < count; i++) {
			if (screen.WaitForRetrace(100000) != B_OK) {
				printf("WaitForRetrace failed\n");
				return 1;
			}
			bigtime_t woke = system_time();
			double d = phase(woke, t.scan_start, t.period);
			sum += d;
			n++;
			if (fabs(d) > fabs(worst))
				worst = d;
		}
		printf("WaitForRetrace wake vs predicted start: mean %+.1f us, "
			"worst %+.1f us over %d\n", sum / n, worst, n);
		return 0;
	}
	return 1;
}
