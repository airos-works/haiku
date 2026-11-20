/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	acctest <device> [accelerant] [seconds]: drives a graphics device through
	its accelerant as app_server would, in a team of its own: init, the
	display outputs, the preferred mode set, a test pattern drawn into the
	frame buffer the CPU sees, then uninit. For trying an accelerant on a GPU
	app_server does not use. */

#include <Accelerant.h>
#include <graphic_driver.h>
#include <image.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <device> [accelerant path] [seconds]\n",
			argv[0]);
		return 1;
	}
	int fd = open(argv[1], B_READ_WRITE);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	char signature[B_FILE_NAME_LENGTH] = {};
	char path[B_PATH_NAME_LENGTH];
	if (argc > 2)
		strlcpy(path, argv[2], sizeof(path));
	else {
		if (ioctl(fd, B_GET_ACCELERANT_SIGNATURE, signature,
				sizeof(signature)) != 0) {
			perror("B_GET_ACCELERANT_SIGNATURE");
			return 1;
		}
		snprintf(path, sizeof(path), "/boot/system/add-ons/accelerants/%s",
			signature);
	}
	int seconds = argc > 3 ? atoi(argv[3]) : 5;
	image_id image = load_add_on(path);
	if (image < 0) {
		fprintf(stderr, "cannot load %s: %s\n", path, strerror(image));
		return 1;
	}
	GetAccelerantHook hook;
	if (get_image_symbol(image, B_ACCELERANT_ENTRY_POINT, B_SYMBOL_TYPE_ANY,
			(void**)&hook) != B_OK) {
		fprintf(stderr, "no %s\n", B_ACCELERANT_ENTRY_POINT);
		return 1;
	}
	init_accelerant init = (init_accelerant)hook(B_INIT_ACCELERANT, NULL);
	uninit_accelerant uninit = (uninit_accelerant)hook(B_UNINIT_ACCELERANT,
		NULL);
	bigtime_t start = system_time();
	status_t status = init(fd);
	printf("init: %s (%.1f ms)\n", strerror(status),
		(system_time() - start) / 1000.0);
	if (status != B_OK)
		return 1;

	accelerant_device_info deviceInfo;
	get_accelerant_device_info getDeviceInfo = (get_accelerant_device_info)
		hook(B_GET_ACCELERANT_DEVICE_INFO, NULL);
	if (getDeviceInfo != NULL && getDeviceInfo(&deviceInfo) == B_OK)
		printf("device: %s, %s\n", deviceInfo.name, deviceInfo.chipset);

	get_display_output_count outputCount = (get_display_output_count)hook(
		B_GET_DISPLAY_OUTPUT_COUNT, NULL);
	get_display_outputs getOutputs = (get_display_outputs)hook(
		B_GET_DISPLAY_OUTPUTS, NULL);
	if (outputCount != NULL && getOutputs != NULL) {
		uint32 count = outputCount();
		display_output* outputs = new display_output[count];
		if (getOutputs(outputs, &count) == B_OK) {
			for (uint32 i = 0; i < count; i++) {
				printf("output %" B_PRIu32 ": %s %s, %ux%u at %d,%d\n",
					outputs[i].id, outputs[i].name,
					(outputs[i].flags & B_DISPLAY_OUTPUT_CONNECTED) != 0
						? "connected" : "disconnected",
					outputs[i].timing.h_display,
					outputs[i].timing.v_display,
					outputs[i].x, outputs[i].y);
			}
		}
		delete[] outputs;
	}

	display_mode mode;
	get_preferred_display_mode getPreferred = (get_preferred_display_mode)
		hook(B_GET_PREFERRED_DISPLAY_MODE, NULL);
	accelerant_mode_count modeCount = (accelerant_mode_count)hook(
		B_ACCELERANT_MODE_COUNT, NULL);
	get_mode_list getModes = (get_mode_list)hook(B_GET_MODE_LIST, NULL);
	if (getPreferred == NULL || getPreferred(&mode) != B_OK) {
		uint32 count = modeCount();
		display_mode* modes = new display_mode[count];
		getModes(modes);
		mode = modes[0];
		delete[] modes;
	}
	printf("mode: %ux%u (virtual %ux%u), %.1f Hz\n", mode.timing.h_display,
		mode.timing.v_display, mode.virtual_width, mode.virtual_height,
		mode.timing.pixel_clock * 1000.0
			/ (mode.timing.h_total * mode.timing.v_total));
	set_display_mode setMode = (set_display_mode)hook(B_SET_DISPLAY_MODE,
		NULL);
	start = system_time();
	status = setMode(&mode);
	printf("set mode: %s (%.1f ms)\n", strerror(status),
		(system_time() - start) / 1000.0);

	frame_buffer_config config;
	get_frame_buffer_config getConfig = (get_frame_buffer_config)hook(
		B_GET_FRAME_BUFFER_CONFIG, NULL);
	if (status == B_OK && getConfig(&config) == B_OK) {
		printf("frame buffer %p, %" B_PRIu32 " bytes per row\n",
			config.frame_buffer, config.bytes_per_row);
		// a gradient with a white border, so that cut-off edges show
		uint32 width = mode.virtual_width, height = mode.virtual_height;
		for (uint32 y = 0; y < height; y++) {
			uint32* row = (uint32*)((uint8*)config.frame_buffer
				+ y * config.bytes_per_row);
			for (uint32 x = 0; x < width; x++) {
				bool border = x < 4 || y < 4 || x >= width - 4
					|| y >= height - 4;
				row[x] = border ? 0xffffffff : 0xff000000
					| ((x * 255 / width) << 16) | ((y * 255 / height) << 8)
					| 0x40;
			}
		}
		printf("pattern drawn; showing it for %d s\n", seconds);
		sleep(seconds);
	}
	uninit();
	close(fd);
	return 0;
}
