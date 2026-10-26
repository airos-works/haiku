/* Checks the accelerant's 2D hooks as a BWindowScreen program sees them.
 *
 * Such a program clones the accelerant into its own team and draws on the
 * screen app_server shows. This needs no screen: while app_server is not on
 * this card (no frame buffer published), it publishes a surface of its own
 * as the screen, clones the accelerant it is given, and checks every hook
 * against the CPU - fills, inverts, spans, blits, transparent and scaled
 * blits, sync tokens, and copies from a frame buffer source. The surface is
 * unpublished at the end.
 *
 * With --init it stands in for app_server instead: it initializes the
 * accelerant itself, which sets a mode (so only while app_server is on
 * another driver, and with a monitor or a force_edid setting), and checks
 * the same on the real frame buffer.
 *
 * With --attach it clones the accelerant app_server runs, as a BWindowScreen
 * program would, tests on the top left 1280x720 of the real screen and puts
 * those pixels back afterwards.
 *
 * usage: nvclone2dtest [--init | --attach] <accelerant>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <vector>

#include <Accelerant.h>
#include <OS.h>
#include <image.h>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvUtils.h"

extern "C" {
#include "nvos.h"
#include "class/cl0040.h"
#include "ctrl/ctrl0000/ctrl0000client.h"
#include "rs_access.h"
#include "nv-haiku.h"
}


static uint32 kWidth = 1280, kHeight = 720;
static int sFailures = 0;


static uint32 Pattern(uint32 x, uint32 y)
{
	return ((x * 0x9e3779b1u) ^ (y * 0x85ebca77u) ^ (x << 16) ^ y) | 0xff000000;
}


static void Check(const char *what, const std::vector<uint32> &expected,
	const uint32 *bits, uint32 stride, uint32 mask = 0xffffffff)
{
	uint32 wrong = 0, firstX = 0, firstY = 0;
	for (uint32 y = 0; y < kHeight; y++) {
		for (uint32 x = 0; x < kWidth; x++) {
			if (((expected[y * kWidth + x] ^ bits[y * stride + x]) & mask) != 0) {
				if (wrong++ == 0) {
					firstX = x;
					firstY = y;
				}
			}
		}
	}
	if (wrong == 0) {
		printf("ok   %s\n", what);
		return;
	}
	printf("FAIL %s: %" B_PRIu32 " pixels, first %" B_PRIu32 ",%" B_PRIu32
		": want %08" B_PRIx32 " got %08" B_PRIx32 "\n", what, wrong, firstX,
		firstY, expected[firstY * kWidth + firstX], bits[firstY * stride + firstX]);
	sFailures++;
}


int main(int argc, char **argv)
{
	bool init = argc == 3 && strcmp(argv[1], "--init") == 0;
	bool attach = argc == 3 && strcmp(argv[1], "--attach") == 0;
	if (argc != 2 && !init && !attach) {
		fprintf(stderr, "usage: nvclone2dtest [--init | --attach] <accelerant>\n");
		return 2;
	}
	const char *accelerantPath = argv[argc - 1];

	int deviceFd = open("/dev/graphics/nvidia0", O_RDWR | O_CLOEXEC);
	if (deviceFd < 0) {
		perror("opening /dev/graphics/nvidia0");
		return 2;
	}
	nv_haiku_scanout_info published {};
	if (ioctl(deviceFd, NV_HAIKU_BASE + NV_HAIKU_GET_SCANOUT, &published,
			sizeof(published)) == 0 && published.memory != 0 && !attach) {
		fprintf(stderr, "a frame buffer is published (app_server is on this "
			"card); not replacing it\n");
		return 2;
	}
	if (attach && published.memory == 0) {
		fprintf(stderr, "no frame buffer published: is app_server on this card?\n");
		return 2;
	}

	struct Unpublish {
		int fd;
		bool armed;
		~Unpublish()
		{
			if (!armed)
				return;
			nv_haiku_scanout_info none {};
			ioctl(fd, NV_HAIKU_BASE + NV_HAIKU_PUBLISH_SCANOUT, &none,
				sizeof(none));
		}
	} unpublish {deviceFd, false};

	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);

		uint32 *bits = NULL;
		uint32 bytesPerRow = 0;
		NvRmObject memory;
		NvRmMemoryMapping mapping;
		if (!init && !attach) {
			// the "screen": video memory, allocated and shared the way the
			// accelerant does its frame buffers
			bytesPerRow = (kWidth * 4 + 255) & ~255u;
			uint64 size = ((uint64)bytesPerRow * kHeight + B_PAGE_SIZE - 1)
				& ~(uint64)(B_PAGE_SIZE - 1);
			NvU8 compressible = 0;
			nvKmsKapiAllocateVideoMemory(rmDev, memory, NvKmsSurfaceMemoryLayoutPitch,
				size, NVKMS_KAPI_ALLOCATION_TYPE_SCANOUT, &compressible);
			NV0000_CTRL_CLIENT_SHARE_OBJECT_PARAMS share {};
			share.hObject = memory.Get();
			share.sharePolicy.type = RS_SHARE_TYPE_ALL;
			RS_ACCESS_MASK_ADD(&share.sharePolicy.accessMask, RS_ACCESS_DUP_OBJECT);
			rm.Client().Control(NV0000_CTRL_CMD_CLIENT_SHARE_OBJECT, &share,
				sizeof(share));
			mapping = rmDev.MapMemory(memory.Get(), false, 0, size, 0);
			bits = (uint32*)mapping.Address();

			nv_haiku_scanout_info info {};
			info.client = rm.Client().Get();
			info.memory = memory.Get();
			info.width = kWidth;
			info.height = kHeight;
			info.bytes_per_row = bytesPerRow;
			info.color_space = B_RGB32;
			info.size = size;
			CheckErrno(ioctl(deviceFd, NV_HAIKU_BASE + NV_HAIKU_PUBLISH_SCANOUT,
				&info, sizeof(info)));
			unpublish.armed = true;
		}

		image_id image = load_add_on(accelerantPath);
		if (image < 0) {
			fprintf(stderr, "loading %s: %s\n", accelerantPath, strerror(image));
			return 2;
		}
		GetAccelerantHook hook;
		CheckErrno(get_image_symbol(image, B_ACCELERANT_ENTRY_POINT,
			B_SYMBOL_TYPE_ANY, (void**)&hook) == B_OK ? 0 : -1);

		status_t status;
		if (init) {
			init_accelerant initHook = (init_accelerant)hook(B_INIT_ACCELERANT, NULL);
			status = initHook(deviceFd);
			printf("%s init: %s\n", status == B_OK ? "ok  " : "FAIL", strerror(status));
			if (status != B_OK)
				return 1;
			// as app_server does: the first mode listed is the layout's
			uint32 count = ((accelerant_mode_count)hook(B_ACCELERANT_MODE_COUNT,
				NULL))();
			std::vector<display_mode> modes(count);
			if (count == 0
				|| ((get_mode_list)hook(B_GET_MODE_LIST, NULL))(modes.data()) != B_OK
				|| ((set_display_mode)hook(B_SET_DISPLAY_MODE, NULL))(&modes[0])
					!= B_OK) {
				printf("FAIL setting the first listed mode\n");
				return 1;
			}
			display_mode mode {};
			((get_display_mode)hook(B_GET_DISPLAY_MODE, NULL))(&mode);
			kWidth = std::min<uint32>(mode.virtual_width, 1920);
			kHeight = std::min<uint32>(mode.virtual_height, 1080);
			printf("     mode %ux%u, checking %" B_PRIu32 "x%" B_PRIu32 "\n",
				mode.virtual_width, mode.virtual_height, kWidth, kHeight);
		} else {
			clone_accelerant cloneHook = (clone_accelerant)hook(B_CLONE_ACCELERANT, NULL);
			char cloneInfo[] = "/dev/graphics/nvidia0";
			status = cloneHook(cloneInfo);
			printf("%s clone: %s\n", status == B_OK ? "ok  " : "FAIL", strerror(status));
			if (status != B_OK)
				return 1;

			// a clone sets no modes
			bool modeHooks = hook(B_SET_DISPLAY_MODE, NULL) != NULL
				|| hook(B_SET_DISPLAY_LAYOUT, NULL) != NULL
				|| hook(B_MOVE_CURSOR, NULL) != NULL;
			printf("%s the clone offers no mode or cursor hooks\n",
				modeHooks ? "FAIL" : "ok  ");
			sFailures += modeHooks;
		}

		auto engineCount = (accelerant_engine_count)hook(B_ACCELERANT_ENGINE_COUNT, NULL);
		auto acquire = (acquire_engine)hook(B_ACQUIRE_ENGINE, NULL);
		auto release = (release_engine)hook(B_RELEASE_ENGINE, NULL);
		auto waitIdle = (wait_engine_idle)hook(B_WAIT_ENGINE_IDLE, NULL);
		auto getSync = (get_sync_token)hook(B_GET_SYNC_TOKEN, NULL);
		auto syncTo = (sync_to_token)hook(B_SYNC_TO_TOKEN, NULL);
		auto fill = (fill_rectangle)hook(B_FILL_RECTANGLE, NULL);
		auto invert = (invert_rectangle)hook(B_INVERT_RECTANGLE, NULL);
		auto span = (fill_span)hook(B_FILL_SPAN, NULL);
		auto blit = (screen_to_screen_blit)hook(B_SCREEN_TO_SCREEN_BLIT, NULL);
		auto keyed = (screen_to_screen_transparent_blit)hook(
			B_SCREEN_TO_SCREEN_TRANSPARENT_BLIT, NULL);
		auto scaled = (screen_to_screen_scaled_filtered_blit)hook(
			B_SCREEN_TO_SCREEN_SCALED_FILTERED_BLIT, NULL);
		auto fbConfig = (get_frame_buffer_config)hook(B_GET_FRAME_BUFFER_CONFIG, NULL);
		auto createSource = (create_frame_buffer_source)hook(
			B_CREATE_FRAME_BUFFER_SOURCE, NULL);
		auto deleteSource = (delete_frame_buffer_source)hook(
			B_DELETE_FRAME_BUFFER_SOURCE, NULL);
		auto copyTo = (copy_to_frame_buffer)hook(B_COPY_TO_FRAME_BUFFER, NULL);
		bool all = engineCount && acquire && release && waitIdle && getSync
			&& syncTo && fill && invert && span && blit && keyed && scaled
			&& fbConfig && createSource && deleteSource && copyTo;
		printf("%s every 2D hook is there (%" B_PRIu32 " engine)\n",
			all ? "ok  " : "FAIL", engineCount ? engineCount() : 0);
		if (!all)
			return 1;

		// the clone's view of the screen is the same memory
		frame_buffer_config config {};
		fbConfig(&config);
		std::vector<uint32> saved;
		if (init || attach) {
			bits = (uint32*)config.frame_buffer;
			bytesPerRow = config.bytes_per_row;
			if (attach) {
				kWidth = std::min<uint32>(published.width, kWidth);
				kHeight = std::min<uint32>(published.height, kHeight);
				saved.resize((size_t)kWidth * kHeight);
				for (uint32 y = 0; y < kHeight; y++) {
					memcpy(&saved[(size_t)y * kWidth],
						(uint8*)bits + (size_t)y * bytesPerRow, kWidth * 4);
				}
			}
		} else {
			bits[5] = 0x12345678;
			bool same = config.frame_buffer != NULL
				&& config.bytes_per_row == bytesPerRow
				&& ((volatile uint32*)config.frame_buffer)[5] == 0x12345678;
			printf("%s the clone's frame buffer is the published one\n",
				same ? "ok  " : "FAIL");
			sFailures += !same;
		}
		uint32 stride = bytesPerRow / 4;

		std::vector<uint32> expected((size_t)kWidth * kHeight);
		auto reset = [&]() {
			for (uint32 y = 0; y < kHeight; y++)
				for (uint32 x = 0; x < kWidth; x++)
					bits[y * stride + x] = expected[y * kWidth + x] = Pattern(x, y);
		};
		auto at = [&](uint32 x, uint32 y) -> uint32 & {
			return expected[y * kWidth + x];
		};
		auto cpuBlit = [&](const blit_params &b) {
			std::vector<uint32> temp((size_t)(b.width + 1) * (b.height + 1));
			for (uint32 y = 0; y <= b.height; y++)
				for (uint32 x = 0; x <= b.width; x++)
					temp[y * (b.width + 1) + x] = at(b.src_left + x, b.src_top + y);
			for (uint32 y = 0; y <= b.height; y++)
				for (uint32 x = 0; x <= b.width; x++)
					at(b.dest_left + x, b.dest_top + y) = temp[y * (b.width + 1) + x];
		};

		engine_token *token = NULL;
		sync_token sync {};

		reset();
		acquire(B_2D_ACCELERATION, 0xff, NULL, &token);
		fill_rect_params rects[] = {{0, 0, 9, 9}, {100, 50, 300, 60},
			{(uint16)(kWidth - 1), (uint16)(kHeight - 1), (uint16)(kWidth - 1),
				(uint16)(kHeight - 1)}};
		fill(token, 0xff00ff00, rects, 3);
		release(token, &sync);
		syncTo(&sync);
		for (auto &r: rects)
			for (uint32 y = r.top; y <= r.bottom; y++)
				for (uint32 x = r.left; x <= r.right; x++)
					at(x, y) = 0xff00ff00;
		Check("fill, waited for with the release's sync token", expected, bits, stride);

		reset();
		acquire(B_2D_ACCELERATION, 0xff, NULL, &token);
		invert(token, rects, 3);
		uint16 spans[] = {200, 5, 600, 201, 600, 5};
		span(token, 0xff102030, spans, 2);
		getSync(token, &sync);
		release(token, NULL);
		syncTo(&sync);
		for (auto &r: rects)
			for (uint32 y = r.top; y <= r.bottom; y++)
				for (uint32 x = r.left; x <= r.right; x++)
					at(x, y) = ~at(x, y);
		for (uint32 x = 5; x <= 600; x++) {
			at(x, 200) = 0xff102030;
			at(x, 201) = 0xff102030;
		}
		Check("invert and spans, waited for with get_sync_token", expected, bits, stride);

		reset();
		acquire(B_2D_ACCELERATION, 0xff, NULL, &token);
		blit_params blits[] = {{0, 0, 10, 10, 399, 299}, {0, 1, 0, 0,
			(uint16)(kWidth - 1), (uint16)(kHeight - 2)}};
		blit(token, blits, 2);
		release(token, NULL);
		waitIdle();
		cpuBlit(blits[0]);
		cpuBlit(blits[1]);
		Check("blits, overlapping, then a scroll", expected, bits, stride);

		reset();
		uint32 key = Pattern(20, 20);
		blit_params keyedBlit = {0, 0, 500, 400, 63, 63};
		acquire(B_2D_ACCELERATION, 0xff, NULL, &token);
		keyed(token, key, &keyedBlit, 1);
		release(token, NULL);
		waitIdle();
		for (uint32 y = 0; y < 64; y++)
			for (uint32 x = 0; x < 64; x++)
				if (((Pattern(x, y) ^ key) & 0xffffff) != 0)
					at(500 + x, 400 + y) = Pattern(x, y);
		Check("transparent blit", expected, bits, stride, 0x00ffffff);

		reset();
		acquire(B_2D_ACCELERATION, 0xff, NULL, &token);
		fill_rect_params flat = {0, 0, 49, 49};
		fill(token, 0xff808080, &flat, 1);
		scaled_blit_params up = {5, 5, 39, 39, 700, 100, 119, 119};
		scaled(token, &up, 1);
		release(token, NULL);
		waitIdle();
		for (uint32 y = 0; y < 50; y++)
			for (uint32 x = 0; x < 50; x++)
				at(x, y) = 0xff808080;
		for (uint32 y = 0; y < 120; y++)
			for (uint32 x = 0; x < 120; x++)
				at(700 + x, 100 + y) = 0xff808080;
		Check("scaled filtered blit", expected, bits, stride);

		// a source in system memory, copied to the screen
		frame_buffer_source source {};
		status = createSource(kWidth, kHeight, B_RGBA32, &source);
		printf("%s create a frame buffer source: %s\n", status == B_OK ? "ok  " : "FAIL",
			strerror(status));
		if (status == B_OK) {
			reset();
			uint32 sourceStride = source.bytes_per_row / 4;
			for (uint32 y = 0; y < kHeight; y++)
				for (uint32 x = 0; x < kWidth; x++)
					((uint32*)source.bits)[y * sourceStride + x] = ~Pattern(x, y) | 0xff000000;
			fill_rect_params copies[] = {{0, 0, 639, 359}, {1000, 600,
				(uint16)(kWidth - 1), (uint16)(kHeight - 1)}};
			status = copyTo(source.id, copies, 2);
			for (auto &r: copies)
				for (uint32 y = r.top; y <= r.bottom; y++)
					for (uint32 x = r.left; x <= r.right; x++)
						at(x, y) = ~Pattern(x, y) | 0xff000000;
			printf("%s copy to the frame buffer: %s\n", status == B_OK ? "ok  " : "FAIL",
				strerror(status));
			Check("what the copy left on the screen", expected, bits, stride);
			deleteSource(source.id);
		}

		if (attach) {
			for (uint32 y = 0; y < kHeight; y++) {
				memcpy((uint8*)bits + (size_t)y * bytesPerRow,
					&saved[(size_t)y * kWidth], kWidth * 4);
			}
		}

		auto uninit = (uninit_accelerant)hook(B_UNINIT_ACCELERANT, NULL);
		uninit();
		printf("ok   uninit\n");
	} catch (const std::system_error &ex) {
		fprintf(stderr, "[!] %s\n", ex.what());
		return 2;
	}

	printf("\n%d failures\n", sFailures);
	return sFailures == 0 ? 0 : 1;
}
