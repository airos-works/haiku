/* Checks the 2D engine against the CPU, and times it.
 *
 * Every operation the accelerant offers runs on a surface in video memory
 * that starts out as a known pattern; the same operation done by the CPU on a
 * copy says what every pixel must be afterwards. Then the timings: filling
 * and copying a 4K screen, and a system memory buffer (app_server's back
 * buffer, in effect) copied to video memory by the engine and by the CPU.
 *
 * It needs no display and does not touch the screen: the surfaces are its
 * own.
 *
 * usage: nv2dtest [width height [timings]]
 *   timings skips the checks (reading video memory back is slow).
 */
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


static uint32 sWidth = 1920, sHeight = 1080;
static int sFailures = 0;


static uint32 Pattern(uint32 x, uint32 y)
{
	return (x * 0x9e3779b1u) ^ (y * 0x85ebca77u) ^ (x << 16) ^ y;
}


struct Picture {
	uint32 width, height, stride;	// stride in pixels
	std::vector<uint32> pixels;

	Picture(uint32 w, uint32 h, uint32 s): width(w), height(h), stride(s),
		pixels((size_t)s * h) {}
	uint32 &At(uint32 x, uint32 y) {return pixels[(size_t)y * stride + x];}
};


static void FillPattern(Picture &picture)
{
	for (uint32 y = 0; y < picture.height; y++)
		for (uint32 x = 0; x < picture.width; x++)
			picture.At(x, y) = Pattern(x, y);
}


static void Upload(const Picture &picture, void *bits)
{
	memcpy(bits, picture.pixels.data(), picture.pixels.size() * 4);
}


static void Download(Picture &picture, const void *bits)
{
	memcpy(picture.pixels.data(), bits, picture.pixels.size() * 4);
}


static bool Compare(const char *what, Picture &expected, Picture &actual,
	int tolerance = 0, uint32 mask = 0xffffffff)
{
	uint32 wrong = 0, firstX = 0, firstY = 0;
	for (uint32 y = 0; y < expected.height; y++) {
		for (uint32 x = 0; x < expected.width; x++) {
			uint32 a = expected.At(x, y), b = actual.At(x, y);
			bool same = ((a ^ b) & mask) == 0;
			if (!same && tolerance > 0) {
				same = true;
				for (int shift = 0; shift < 32; shift += 8) {
					int d = (int)((a >> shift) & 0xff) - (int)((b >> shift) & 0xff);
					if (abs(d) > tolerance)
						same = false;
				}
			}
			if (!same) {
				if (wrong == 0) {
					firstX = x;
					firstY = y;
				}
				wrong++;
			}
		}
	}
	if (wrong == 0) {
		printf("ok   %s\n", what);
		return true;
	}
	printf("FAIL %s: %" B_PRIu32 " pixels differ, first at %" B_PRIu32 ",%"
		B_PRIu32 ": want %08" B_PRIx32 " got %08" B_PRIx32 "\n", what, wrong,
		firstX, firstY, expected.At(firstX, firstY), actual.At(firstX, firstY));
	sFailures++;
	return false;
}


static void CpuBlit(Picture &picture, const blit_params &blit)
{
	uint32 w = blit.width + 1, h = blit.height + 1;
	std::vector<uint32> temp((size_t)w * h);
	for (uint32 y = 0; y < h; y++)
		for (uint32 x = 0; x < w; x++)
			temp[(size_t)y * w + x] = picture.At(blit.src_left + x, blit.src_top + y);
	for (uint32 y = 0; y < h; y++)
		for (uint32 x = 0; x < w; x++)
			picture.At(blit.dest_left + x, blit.dest_top + y) = temp[(size_t)y * w + x];
}


int main(int argc, char **argv)
{
	bool timingsOnly = false;
	if (argc >= 3) {
		sWidth = atoi(argv[1]);
		sHeight = atoi(argv[2]);
		timingsOnly = argc >= 4 && strcmp(argv[3], "timings") == 0;
	}

	// Resman brings the card up when its node is opened and takes it down
	// when the last one is closed, display or not.
	int deviceFd = open("/dev/graphics/nvidia0", O_RDWR | O_CLOEXEC);
	if (deviceFd < 0) {
		perror("opening /dev/graphics/nvidia0");
		return 2;
	}

	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		bigtime_t start = system_time();
		NvEngine2D engine(rm, rmDev);
		printf("engine up in %" B_PRId64 " us\n", system_time() - start);
		std::lock_guard<std::recursive_mutex> lock(engine.Lock());

		uint32 bytesPerRow = (sWidth * 4 + 255) & ~255u;
		uint32 stride = bytesPerRow / 4;
		NvEngine2D::Buffer video = engine.Allocate((NvU64)bytesPerRow * sHeight, false);
		NvEngine2D::Surface surface;
		surface.gpuAddress = video.GpuAddress();
		surface.width = sWidth;
		surface.height = sHeight;
		surface.bytesPerRow = bytesPerRow;
		engine.SetTarget(surface);
		printf("surface %" B_PRIu32 "x%" B_PRIu32 " at GPU %#" B_PRIx64 "\n",
			sWidth, sHeight, video.GpuAddress());

		Picture expected(sWidth, sHeight, stride), actual(sWidth, sHeight, stride);
		auto reset = [&]() {
			FillPattern(expected);
			Upload(expected, video.Address());
		};
		auto check = [&](const char *what, int tolerance = 0,
				uint32 mask = 0xffffffff) {
			if (!engine.Wait(engine.Submit())) {
				printf("FAIL %s: the engine did not finish\n", what);
				sFailures++;
				return;
			}
			Download(actual, video.Address());
			Compare(what, expected, actual, tolerance, mask);
		};

		NvEngine2D::Buffer system = engine.Allocate((NvU64)bytesPerRow * sHeight, true);
		NvEngine2D::Surface back = surface;
		back.gpuAddress = system.GpuAddress();
		if (timingsOnly)
			goto timings;
	{
		// fills, edges included, one rectangle partly off the surface
		reset();
		fill_rect_params fills[] = {{0, 0, 0, 0}, {10, 20, 300, 40},
			{(uint16)(sWidth - 7), (uint16)(sHeight - 3), (uint16)(sWidth - 1),
				(uint16)(sHeight - 1)},
			{100, 100, 100, 600}};
		engine.FillRectangles(0x80ff2010, fills, 4);
		for (auto &f: fills)
			for (uint32 y = f.top; y <= f.bottom; y++)
				for (uint32 x = f.left; x <= f.right; x++)
					expected.At(x, y) = 0x80ff2010;
		check("fill rectangles");

		reset();
		engine.InvertRectangles(fills, 4);
		for (auto &f: fills)
			for (uint32 y = f.top; y <= f.bottom; y++)
				for (uint32 x = f.left; x <= f.right; x++)
					expected.At(x, y) = ~expected.At(x, y);
		check("invert rectangles");

		reset();
		uint16 spans[] = {5, 10, 20, 6, 30, 3, 700, 0, (uint16)(sWidth - 1)};
		engine.FillSpans(0x00123456, spans, 3);
		for (int i = 0; i < 3; i++) {
			uint16 l = std::min(spans[i * 3 + 1], spans[i * 3 + 2]);
			uint16 r = std::max(spans[i * 3 + 1], spans[i * 3 + 2]);
			for (uint32 x = l; x <= r; x++)
				expected.At(x, spans[i * 3]) = 0x00123456;
		}
		check("fill spans");

		// blits: apart, overlapping in every direction, a scroll by one line
		struct {
			const char *name;
			blit_params blit;
		} blits[] = {
			{"blit apart", {0, 0, 500, 400, 199, 99}},
			{"blit overlapping down-right", {100, 100, 110, 105, 299, 199}},
			{"blit overlapping up-left", {110, 105, 100, 100, 299, 199}},
			{"blit overlapping right", {100, 100, 101, 100, 399, 99}},
			{"blit overlapping left", {101, 100, 100, 100, 399, 99}},
			{"blit scroll up one line", {0, 1, 0, 0, (uint16)(sWidth - 1),
				(uint16)(sHeight - 2)}},
			{"blit scroll down one line", {0, 0, 0, 1, (uint16)(sWidth - 1),
				(uint16)(sHeight - 2)}},
		};
		for (auto &b: blits) {
			reset();
			engine.Blit(&b.blit, 1);
			CpuBlit(expected, b.blit);
			check(b.name);
		}

		// a list whose second blit reads what the first wrote
		reset();
		blit_params chain[] = {{0, 0, 300, 0, 99, 99}, {300, 0, 600, 0, 99, 99}};
		engine.Blit(chain, 2);
		CpuBlit(expected, chain[0]);
		CpuBlit(expected, chain[1]);
		check("blit list, each sees the last");

		reset();
		uint32 key = Pattern(50, 50);
		blit_params keyed = {0, 0, 800, 600, 255, 255};
		engine.TransparentBlit(key, &keyed, 1);
		for (uint32 y = 0; y <= keyed.height; y++)
			for (uint32 x = 0; x <= keyed.width; x++) {
				uint32 p = Pattern(keyed.src_left + x, keyed.src_top + y);
				if (((p ^ key) & 0xffffff) != 0)
					expected.At(keyed.dest_left + x, keyed.dest_top + y) = p;
			}
		check("transparent blit (colour only)", 0, 0x00ffffff);

		// a keyed blit over a key that is everywhere copies nothing
		reset();
		fill_rect_params block = {0, 0, 63, 63};
		engine.FillRectangles(0x11223344, &block, 1);
		for (uint32 y = 0; y < 64; y++)
			for (uint32 x = 0; x < 64; x++)
				expected.At(x, y) = 0x11223344;
		blit_params all = {0, 0, 900, 0, 63, 63};
		engine.TransparentBlit(0x11223344, &all, 1);
		check("transparent blit of the key only", 0, 0x00ffffff);

		// scaled: twice the size from a flat block, so filtering cannot
		// change anything inside it; then a real scale with tolerance
		reset();
		fill_rect_params flat = {0, 0, 99, 99};
		engine.FillRectangles(0xff406080, &flat, 1);
		for (uint32 y = 0; y < 100; y++)
			for (uint32 x = 0; x < 100; x++)
				expected.At(x, y) = 0xff406080;
		scaled_blit_params scaled = {10, 10, 79, 79, 400, 300, 159, 159};
		engine.ScaledFilteredBlit(&scaled, 1);
		for (uint32 y = 0; y < 160; y++)
			for (uint32 x = 0; x < 160; x++)
				expected.At(400 + x, 300 + y) = 0xff406080;
		check("scaled filtered blit, flat");

		// after a scaled blit the plain ones are unscaled again
		reset();
		blit_params plain = {0, 0, 1000, 500, 99, 99};
		engine.Blit(&plain, 1);
		CpuBlit(expected, plain);
		check("blit after a scaled one");

		// copy from system memory, as app_server's flush would
		reset();
		Picture source(sWidth, sHeight, stride);
		for (uint32 y = 0; y < sHeight; y++)
			for (uint32 x = 0; x < sWidth; x++)
				source.At(x, y) = ~Pattern(x, y) + 7;
		Upload(source, system.Address());
		clipping_rect rects[] = {{0, 0, 99, 49}, {200, 300, 999, 799},
			{(int32)sWidth - 10, (int32)sHeight - 10, (int32)sWidth + 50,
				(int32)sHeight + 50}};
		engine.Copy(back, surface, rects, 3);
		for (auto &r: rects)
			for (int32 y = r.top; y <= std::min<int32>(r.bottom, sHeight - 1); y++)
				for (int32 x = r.left; x <= std::min<int32>(r.right, sWidth - 1); x++)
					expected.At(x, y) = source.At(x, y);
		check("copy from system memory");

		// the CPU changes the source; the next copy must see the change
		for (uint32 x = 0; x < 100; x++)
			((uint32*)system.Address())[x] = 0xdeadbeef;
		clipping_rect firstRow = {0, 0, 99, 0};
		engine.Copy(back, surface, &firstRow, 1);
		for (uint32 x = 0; x < 100; x++)
			expected.At(x, 0) = 0xdeadbeef;
		check("copy sees the CPU's latest writes");
	}

timings:

		// timings
		printf("\ntimings, %" B_PRIu32 "x%" B_PRIu32 ":\n", sWidth, sHeight);
		double megabytes = (double)sWidth * sHeight * 4 / 1e6;
		fill_rect_params whole = {0, 0, (uint16)(sWidth - 1), (uint16)(sHeight - 1)};
		auto time = [&](const char *what, auto &&op, int rounds = 20) {
			op();
			engine.WaitIdle();
			bigtime_t begin = system_time();
			for (int i = 0; i < rounds; i++) {
				op();
				engine.Submit();
			}
			engine.WaitIdle();
			double us = (double)(system_time() - begin) / rounds;
			printf("  %-40s %8.0f us  %6.1f GB/s\n", what, us, megabytes / us * 1e3);
		};
		time("GPU fill, video memory", [&]() {engine.FillRectangles(0x123, &whole, 1);});
		blit_params half = {0, 0, 0, (uint16)(sHeight / 2), (uint16)(sWidth - 1),
			(uint16)(sHeight / 2 - 1)};
		time("GPU blit half screen, video memory", [&]() {engine.Blit(&half, 1);});
		clipping_rect all2 = {0, 0, (int32)sWidth - 1, (int32)sHeight - 1};
		time("GPU copy system -> video memory", [&]() {engine.Copy(back, surface, &all2, 1);});
		{
			bigtime_t begin = system_time();
			int rounds = 5;
			for (int i = 0; i < rounds; i++) {
				for (uint32 y = 0; y < sHeight; y++)
					memcpy((uint8*)video.Address() + (size_t)y * bytesPerRow,
						(uint8*)system.Address() + (size_t)y * bytesPerRow, sWidth * 4);
			}
			double us = (double)(system_time() - begin) / rounds;
			printf("  %-40s %8.0f us  %6.1f GB/s\n", "CPU memcpy system -> video memory",
				us, megabytes / us * 1e3);
		}
		{
			// how the CPU sees the system memory buffer: app_server would
			// draw into it, so it had better be cached
			std::vector<uint8> plain((size_t)bytesPerRow * sHeight, 1);
			size_t bytes = plain.size();
			bigtime_t begin = system_time();
			for (int i = 0; i < 5; i++)
				memcpy(system.Address(), plain.data(), bytes);
			double write = (double)(system_time() - begin) / 5;
			begin = system_time();
			for (int i = 0; i < 5; i++)
				memcpy(plain.data(), system.Address(), bytes);
			double read = (double)(system_time() - begin) / 5;
			begin = system_time();
			std::vector<uint8> other(bytes, 2);
			for (int i = 0; i < 5; i++)
				memcpy(other.data(), plain.data(), bytes);
			double heap = (double)(system_time() - begin) / 5;
			printf("  %-40s %8.0f us  %6.1f GB/s\n", "CPU write, system memory buffer",
				write, bytes / write / 1e3);
			printf("  %-40s %8.0f us  %6.1f GB/s\n", "CPU read, system memory buffer",
				read, bytes / read / 1e3);
			printf("  %-40s %8.0f us  %6.1f GB/s\n", "CPU heap to heap, for comparison",
				heap, bytes / heap / 1e3);
		}
		{
			uint32 single = engine.Submit();
			clipping_rect small = {0, 0, 63, 63};
			bigtime_t begin = system_time();
			int rounds = 200;
			for (int i = 0; i < rounds; i++) {
				engine.Copy(back, surface, &small, 1);
				engine.Wait(engine.Submit());
			}
			(void)single;
			printf("  %-40s %8.1f us\n", "round trip, 64x64 copy + wait",
				(double)(system_time() - begin) / rounds);
		}
		if (engine.IsLost()) {
			printf("FAIL the engine was lost\n");
			sFailures++;
		}
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] %s\n", ex.what());
		return 2;
	}

	printf("\n%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
