// Diagnostic tool: ask NVKMS whether a layout is possible - which monitor
// shows which region of a frame buffer, and how the display engine has to
// scale it - without changing anything on the screen. The mode set is only
// validated (commit = false), which NVKMS allows any client to do while
// app_server owns the display.
//
// usage: nvlayoutcheck <frame buffer width> <height> <dpy>=<w>x<h>@<x>,<y> ...
//   dpy is the NVKMS dpy id as nvdpyinfo prints it. Each monitor is driven
//   at its preferred timing and shows the w x h region of the frame buffer
//   at x,y: a region larger than the monitor asks the head to shrink it, a
//   smaller one to enlarge it. Heads are given out in argument order.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <system_error>
#include <vector>

#include <ErrorUtils.h>
#include <NvKmsApi.h>
#include <NvKmsDevice.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>

#include "NvKmsBitmap.h"


struct Region {
	NvU32 dpy;
	uint32 width, height;
	int32 x, y;
	NvKmsMode mode;
};


static NvKmsMode PreferredMode(NvKmsApi &kms, NvKmsDevice &kmsDev, NvKmsDispHandle disp,
	NVDpyId dpyId)
{
	NvKmsMode firstMode {};
	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = kmsDev.Get();
		params.request.dispHandle = disp;
		params.request.dpyId = dpyId;
		params.request.modeIndex = i;
		CheckErrno(kms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params)));
		if (params.reply.end)
			break;
		if (!params.reply.valid)
			continue;
		if (params.reply.preferredMode)
			return params.reply.mode;
		if (firstMode.timings.hVisible == 0)
			firstMode = params.reply.mode;
	}
	return firstMode;
}


int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <fb width> <fb height> <dpy>=<w>x<h>@<x>,<y> ...\n",
			argv[0]);
		return 1;
	}
	setvbuf(stdout, NULL, _IONBF, 0);

	uint32 fbWidth = atoi(argv[1]);
	uint32 fbHeight = atoi(argv[2]);
	std::vector<Region> regions;
	for (int i = 3; i < argc; i++) {
		Region region {};
		unsigned dpy, width, height;
		int x, y;
		if (sscanf(argv[i], "%u=%ux%u@%d,%d", &dpy, &width, &height, &x, &y) != 5) {
			fprintf(stderr, "cannot read \"%s\"\n", argv[i]);
			return 1;
		}
		region.dpy = dpy;
		region.width = width;
		region.height = height;
		region.x = x;
		region.y = y;
		regions.push_back(region);
	}

	try {
		NvRmApi rm;
		NvRmDevice rmDev(rm, 0);
		NvKmsApi kms;
		NvKmsDevice kmsDev(kms, 0);
		NvKmsDispHandle disp = kmsDev.Info().dispHandles[0];

		NvKmsBitmap framebuffer(rmDev, kmsDev, fbWidth, fbHeight, B_RGB32);

		NvKmsSetModeParams params {};
		params.request.deviceHandle = kmsDev.Get();
		params.request.commit = false;
		params.request.requestedDispsBitMask = 1;
		params.request.disp[0].requestedHeadsBitMask
			= (1U << std::min<NvU32>(kmsDev.Info().numHeads, NVKMS_MAX_HEADS_PER_DISP)) - 1;
		NvU32 head = 0;
		for (auto &region: regions) {
			NVDpyId dpyId = nvNvU32ToDpyId(region.dpy);
			region.mode = PreferredMode(kms, kmsDev, disp, dpyId);
			const NvModeTimings &timings = region.mode.timings;
			printf("head %u: dpy %u %ux%u@%u, region %ux%u at %d,%d (%.3f x %.3f)\n",
				(unsigned)head, (unsigned)region.dpy, (unsigned)timings.hVisible,
				(unsigned)timings.vVisible, (unsigned)(timings.RRx1k / 1000),
				(unsigned)region.width, (unsigned)region.height, (int)region.x, (int)region.y,
				(double)timings.hVisible / region.width, (double)timings.vVisible / region.height);

			NvKmsSetModeOneHeadRequest &request = params.request.disp[0].head[head];
			request.dpyIdList = nvAddDpyIdToEmptyDpyIdList(dpyId);
			request.mode = region.mode;
			request.modeValidationParams.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
			request.viewPortOut = {.x = 0, .y = 0, .width = timings.hVisible,
				.height = timings.vVisible};
			request.viewPortSizeIn = {.width = (NvU16)region.width,
				.height = (NvU16)region.height};
			request.flip.viewPortIn.specified = true;
			request.flip.viewPortIn.point = {.x = (NvU16)region.x, .y = (NvU16)region.y};
			auto &layer = request.flip.layer[NVKMS_MAIN_LAYER];
			layer.surface.handle[0] = framebuffer.Surface().Get();
			layer.surface.specified = true;
			layer.sizeIn.val = {.width = (NvU16)fbWidth, .height = (NvU16)fbHeight};
			layer.sizeIn.specified = true;
			layer.sizeOut.val = {.width = (NvU16)fbWidth, .height = (NvU16)fbHeight};
			layer.sizeOut.specified = true;
			head++;
		}

		int result = kms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params));
		printf("validation: %s (ioctl %d), status %d, disp status %d\n",
			result >= 0 ? "POSSIBLE" : "REFUSED", result, (int)params.reply.status,
			(int)params.reply.disp[0].status);
		for (NvU32 i = 0; i < head; i++) {
			const NvKmsSetModeOneHeadReply &reply = params.reply.disp[0].head[i];
			printf("  head %u: status %d, hw head %u, main layer downscale up to %.2f x %.2f\n",
				(unsigned)i, (int)reply.status, (unsigned)reply.hwHead,
				reply.possibleUsage.layer[NVKMS_MAIN_LAYER].scaling.maxHDownscaleFactor / 1024.0,
				reply.possibleUsage.layer[NVKMS_MAIN_LAYER].scaling.maxVDownscaleFactor / 1024.0);
		}
		return result >= 0 ? 0 : 2;
	} catch (const std::system_error &error) {
		fprintf(stderr, "error: %s\n", error.what());
		return 1;
	}
}
