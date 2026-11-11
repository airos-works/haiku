/* How the GPU's clocks follow the load, seen through the 2D engine.
 *
 * Resman picks the performance state (P0 fastest, P8 slowest) from how busy
 * the GPU is. A desktop gives it one short job a frame, so the same operation
 * can take five times as long as it does under a sustained load. This times a
 * 4K fill and copies from idle, after pauses, and at fixed rates, and prints
 * the current performance state (NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE,
 * a mask: 0x1 = P0, 0x100 = P8).
 *
 * usage: nvramptest [width height]           ramp from idle, pauses, 60 Hz loads
 *        nvramptest width height burst [s]   60/120/240 Hz after s idle, then
 *                                            the same with PERF_BOOST held
 *        nvramptest width height decay       how long full speed lasts
 *
 * Holds /dev/graphics/nvidia0 open like nv2dtest; needs no display.
 */
#include <algorithm>
#include <vector>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include <OS.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvEngine2D.h"

#include <ctrl/ctrl2080/ctrl2080perf.h>


static uint32 sWidth = 3840, sHeight = 2160;


int main(int argc, char **argv)
{
	if (argc >= 3) {
		sWidth = atoi(argv[1]);
		sHeight = atoi(argv[2]);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);

	int deviceFd = open("/dev/graphics/nvidia0", O_RDWR | O_CLOEXEC);
	if (deviceFd < 0) {
		perror("opening /dev/graphics/nvidia0");
		return 2;
	}

	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);
		NvEngine2D engine(rm, rmDev);
		std::lock_guard<std::recursive_mutex> lock(engine.Lock());

		uint32 bytesPerRow = (sWidth * 4 + 255) & ~255u;
		NvEngine2D::Buffer video = engine.Allocate((NvU64)bytesPerRow * sHeight, false);
		NvEngine2D::Surface surface;
		surface.gpuAddress = video.GpuAddress();
		surface.width = sWidth;
		surface.height = sHeight;
		surface.bytesPerRow = bytesPerRow;
		engine.SetTarget(surface);

		NvEngine2D::Buffer system = engine.Allocate((NvU64)bytesPerRow * sHeight, true);
		NvEngine2D::Surface back = surface;
		back.gpuAddress = system.GpuAddress();

		fill_rect_params whole = {0, 0, (uint16)(sWidth - 1), (uint16)(sHeight - 1)};
		clipping_rect all = {0, 0, (int32)sWidth - 1, (int32)sHeight - 1};
		clipping_rect small = {100, 100, 163, 163};

		auto fill = [&]() -> bigtime_t {
			bigtime_t begin = system_time();
			engine.FillRectangles(0x123, &whole, 1);
			engine.Wait(engine.Submit());
			return system_time() - begin;
		};
		auto upload = [&]() -> bigtime_t {
			bigtime_t begin = system_time();
			engine.Copy(back, surface, &all, 1);
			engine.Wait(engine.Submit());
			return system_time() - begin;
		};
		auto tiny = [&]() -> bigtime_t {
			bigtime_t begin = system_time();
			engine.Copy(back, surface, &small, 1);
			engine.Wait(engine.Submit());
			return system_time() - begin;
		};
		auto warm = [&](bigtime_t howLong) {
			bigtime_t end = system_time() + howLong;
			while (system_time() < end)
				fill();
		};
		auto median = [](std::vector<bigtime_t> &v) -> bigtime_t {
			if (v.empty())
				return 0;
			std::sort(v.begin(), v.end());
			return v[v.size() / 2];
		};

		printf("surface %" B_PRIu32 "x%" B_PRIu32 "\n", sWidth, sHeight);

		auto pstate = [&]() -> uint32 {
			NV2080_CTRL_PERF_GET_CURRENT_PSTATE_PARAMS params {};
			try {
				rmDev.Subdevice().Control(NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE,
					&params, sizeof(params));
			} catch (const std::system_error &ex) {
				return 0xffffffff;
			}
			return params.currPstate;
		};
		auto boost = [&](uint32 flags, uint32 seconds) {
			NV2080_CTRL_PERF_BOOST_PARAMS params {};
			params.flags = flags;
			params.duration = seconds;
			try {
				rmDev.Subdevice().Control(NV2080_CTRL_CMD_PERF_BOOST, &params,
					sizeof(params));
				printf("  PERF_BOOST flags %#" B_PRIx32 " for %" B_PRIu32 " s: ok\n",
					flags, seconds);
			} catch (const std::system_error &ex) {
				printf("  PERF_BOOST flags %#" B_PRIx32 ": %s\n", flags, ex.what());
			}
		};
		auto burst = [&](const char *what, int seconds, bigtime_t period) {
			printf("%s: one 4K fill every %.1f ms for %d s "
				"(per second: fastest, median, slowest us; pstate mask)\n", what,
				period / 1000.0, seconds);
			bigtime_t start = system_time();
			bigtime_t next = start;
			int second = 0;
			std::vector<bigtime_t> times;
			while (true) {
				bigtime_t now = system_time() - start;
				if (now / 1000000 != second || now >= seconds * 1000000LL) {
					bigtime_t low = *std::min_element(times.begin(), times.end());
					bigtime_t high = *std::max_element(times.begin(), times.end());
					printf("  second %2d: %5" B_PRId64 " %5" B_PRId64 " %5" B_PRId64
						"   pstate %#" B_PRIx32 "\n", second + 1, low, median(times),
						high, pstate());
					times.clear();
					second = now / 1000000;
					if (now >= seconds * 1000000LL)
						break;
				}
				times.push_back(fill());
				next += period;
				snooze_until(next, B_SYSTEM_TIMEBASE);
			}
		};

		if (argc >= 4 && strcmp(argv[3], "decay") == 0) {
			printf("pstate mask at start %#" B_PRIx32 "\n", pstate());
			static const int kLong[] = {7, 10, 15, 20, 30, 45};
			for (int gap: kLong) {
				warm(1000000);
				uint32 warmState = pstate();
				snooze((bigtime_t)gap * 1000000);
				uint32 idleState = pstate();
				bigtime_t a = fill(), b = fill(), c = fill(), d = fill();
				printf("  %3d s pause: %5" B_PRId64 "  %5" B_PRId64 " %5" B_PRId64
					" %5" B_PRId64 "   pstate warm %#" B_PRIx32 " idle %#" B_PRIx32
					"\n", gap, a, b, c, d, warmState, idleState);
			}
			return 0;
		}
		if (argc >= 4 && strcmp(argv[3], "burst") == 0) {
			int idle = argc >= 5 ? atoi(argv[4]) : 30;
			printf("%d s idle first; pstate mask %#" B_PRIx32 "\n", idle, pstate());
			snooze((bigtime_t)idle * 1000000);
			printf("pstate mask after the idle time %#" B_PRIx32 "\n", pstate());
			burst("cold, 60 a second", 8, 16667);
			burst("then 120 a second", 5, 8333);
			burst("then 240 a second", 5, 4167);
			burst("back to 60 a second", 8, 16667);
			boost(NV2080_CTRL_PERF_BOOST_FLAGS_CMD_BOOST_TO_MAX, 30);
			burst("boosted to the maximum, 60 a second", 8, 16667);
			boost(NV2080_CTRL_PERF_BOOST_FLAGS_CMD_CLEAR, 0);
			burst("boost cleared, 60 a second", 12, 16667);
			return 0;
		}

		// A: from idle, fills back to back
		printf("\nA. 5 s idle, then fills back to back for 3 s "
			"(per 50 ms: count, fastest, median, slowest us)\n");
		snooze(5000000);
		{
			bigtime_t start = system_time();
			bigtime_t slot = 50000;
			int index = 0;
			std::vector<bigtime_t> times;
			bigtime_t first = fill();
			printf("  first fill %" B_PRId64 " us\n", first);
			while (system_time() - start < 3000000) {
				bigtime_t t = fill();
				bigtime_t now = system_time() - start;
				if (now / slot != index) {
					if (!times.empty() && (index < 20 || index % 10 == 0)) {
						bigtime_t low = *std::min_element(times.begin(), times.end());
						bigtime_t high = *std::max_element(times.begin(), times.end());
						printf("  %4d ms: %3zu fills, %5" B_PRId64 " %5" B_PRId64
							" %5" B_PRId64 "\n", (int)(index * slot / 1000),
							times.size(), low, median(times), high);
					}
					times.clear();
					index = now / slot;
				}
				times.push_back(t);
			}
		}

		// B: how long does full speed last once the load stops?
		printf("\nB. warm, then one fill after a pause "
			"(pause ms: first fill, next three us; tiny copy first, warm us)\n");
		static const int kGaps[] = {5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
		for (int gap: kGaps) {
			warm(1000000);
			snooze((bigtime_t)gap * 1000);
			bigtime_t a = fill(), b = fill(), c = fill(), d = fill();
			warm(1000000);
			snooze((bigtime_t)gap * 1000);
			bigtime_t t = tiny();
			warm(200000);
			bigtime_t tw = tiny();
			printf("  %5d ms: %5" B_PRId64 "  %5" B_PRId64 " %5" B_PRId64 " %5"
				B_PRId64 "   tiny %5" B_PRId64 " (warm %" B_PRId64 ")\n", gap,
				a, b, c, d, t, tw);
		}

		// C: the desktop's pattern: one operation per frame
		static const struct {const char *name; int kind;} kLoads[] = {
			{"one 4K fill", 0}, {"one 4K system->video copy", 1},
			{"one 64x64 copy", 2}};
		for (auto &load: kLoads) {
			printf("\nC. 5 s idle, then %s every 16.7 ms for 5 s "
				"(per second: fastest, median, slowest us)\n", load.name);
			snooze(5000000);
			bigtime_t start = system_time();
			bigtime_t next = start;
			int second = 0;
			std::vector<bigtime_t> times;
			while (true) {
				bigtime_t now = system_time() - start;
				if (now / 1000000 != second || now >= 5000000) {
					bigtime_t low = *std::min_element(times.begin(), times.end());
					bigtime_t high = *std::max_element(times.begin(), times.end());
					printf("  second %d: %5" B_PRId64 " %5" B_PRId64 " %5" B_PRId64
						"\n", second + 1, low, median(times), high);
					times.clear();
					second = now / 1000000;
					if (now >= 5000000)
						break;
				}
				times.push_back(load.kind == 0 ? fill()
					: load.kind == 1 ? upload() : tiny());
				next += 16667;
				snooze_until(next, B_SYSTEM_TIMEBASE);
			}
			warm(1000000);
			std::vector<bigtime_t> warmTimes;
			for (int i = 0; i < 20; i++) {
				warmTimes.push_back(load.kind == 0 ? fill()
					: load.kind == 1 ? upload() : tiny());
			}
			printf("  same operation back to back after 1 s of load: median %"
				B_PRId64 " us\n", median(warmTimes));
		}

		if (engine.IsLost()) {
			printf("FAIL the engine was lost\n");
			return 1;
		}
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] %s\n", ex.what());
		return 2;
	}
	return 0;
}
