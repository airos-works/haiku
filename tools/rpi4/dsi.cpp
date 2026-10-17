/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab tool for the DSI display driver:

	rpi4_dsi state             the driver's state and the registers at init
	rpi4_dsi step <n|off>      one step of the enable sequence
	rpi4_dsi on|off            the whole sequence
	rpi4_dsi regs              the live registers of all blocks
	rpi4_dsi dlist [from [n]]  words of the HVS display list memory
	rpi4_dsi read <block> <offset>
	rpi4_dsi write <block> <offset> <value>
	rpi4_dsi fill <rrggbb>     fill the shown buffer with a colour
	rpi4_dsi bars              colour bars with a frame, through PRESENT
	rpi4_dsi dump <file>       the shown buffer as a raw RGB32 file
	rpi4_dsi info              the auxdisplay info

	Blocks: dsi, pv, hvs, cm. */


#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <rpi_dsi.h>


static const char* kBlocks[] = { "dsi", "pv", "hvs", "cm", "gpio", "i2c" };


static int
block_index(const char* name)
{
	for (int i = 0; i < 6; i++) {
		if (strcmp(name, kBlocks[i]) == 0)
			return i;
	}
	fprintf(stderr, "no block %s (dsi, pv, hvs, cm, gpio, i2c)\n", name);
	exit(1);
}


static void
dump_words(const char* title, const uint32* words, int count)
{
	printf("%s\n", title);
	for (int i = 0; i < count; i++) {
		if (i % 8 == 0)
			printf("  %04x:", i * 4);
		printf(" %08x", (unsigned)words[i]);
		if (i % 8 == 7 || i == count - 1)
			printf("\n");
	}
}


static void
print_timing(const char* title, const rpi_dsi_timing& t)
{
	printf("%s: %ux%u, h %u/%u/%u, v %u/%u/%u, %u kHz, %u lane(s)\n",
		title, (unsigned)t.width, (unsigned)t.height, (unsigned)t.hfront,
		(unsigned)t.hsync, (unsigned)t.hback, (unsigned)t.vfront,
		(unsigned)t.vsync, (unsigned)t.vback, (unsigned)t.clock,
		(unsigned)t.lanes);
}


static int
fail(const char* what)
{
	fprintf(stderr, "%s: %s\n", what, strerror(errno));
	return 1;
}


static uint32*
map_buffer(int fd, uint32 index, uint32* _size)
{
	aux_display_buffer request = {};
	request.index = index;
	if (ioctl(fd, AUX_DISPLAY_CLONE_BUFFER, &request, sizeof(request)) != 0)
		return NULL;
	*_size = request.size;
	return (uint32*)request.address;
}


int
main(int argc, char** argv)
{
	const char* command = argc > 1 ? argv[1] : "state";
	int fd = open("/dev/" RPI_DSI_DEVICE, O_RDWR);
	if (fd < 0)
		return fail("/dev/" RPI_DSI_DEVICE);

	if (strcmp(command, "state") == 0) {
		rpi_dsi_state state;
		if (ioctl(fd, RPI_DSI_GET_STATE, &state, sizeof(state)) != 0)
			return fail("get state");
		printf("steps done: %u\n", (unsigned)state.steps_done);
		print_timing("panel", state.panel);
		print_timing("mode", state.mode);
		printf("PLLD %u Hz, PLLD_PER %u Hz, DSI clock %u Hz (/%u), escape "
			"%u Hz\n", (unsigned)state.plld_rate,
			(unsigned)state.plld_per_rate, (unsigned)state.hs_clock,
			(unsigned)state.pll_divider, (unsigned)state.escape_clock);
		printf("display list word %u, channel %u, buffers %u x %u bytes at "
			"0x%llx, shown %u\n", (unsigned)state.display_list,
			(unsigned)state.channel, (unsigned)state.buffer_count,
			(unsigned)state.buffer_size,
			(unsigned long long)state.buffer_address, (unsigned)state.shown);
		dump_words("HVS at init", state.initial_hvs, 32);
		dump_words("PV1 at init", state.initial_pv, 16);
		dump_words("DSI1 at init", state.initial_dsi, 36);
		return 0;
	}

	if (strcmp(command, "info") == 0) {
		aux_display_info info;
		if (ioctl(fd, AUX_DISPLAY_GET_INFO, &info, sizeof(info)) != 0)
			return fail("get info");
		printf("%s: %ux%u, %u bytes per row, color space 0x%x, %u buffers of "
			"%u bytes, flags 0x%x, %u.%03u Hz\n", info.name,
			(unsigned)info.width, (unsigned)info.height,
			(unsigned)info.bytes_per_row, (unsigned)info.color_space,
			(unsigned)info.buffer_count, (unsigned)info.buffer_size,
			(unsigned)info.flags, (unsigned)info.refresh_rate / 1000,
			(unsigned)info.refresh_rate % 1000);
		return 0;
	}

	if (strcmp(command, "step") == 0 && argc > 2) {
		uint32 step = strcmp(argv[2], "off") == 0
			? RPI_DSI_STEP_OFF : atoi(argv[2]);
		if (ioctl(fd, RPI_DSI_RUN_STEP, &step, sizeof(step)) != 0)
			return fail("step");
		printf("step %u done\n", (unsigned)step);
		return 0;
	}

	if (strcmp(command, "on") == 0 || strcmp(command, "off") == 0) {
		uint32 on = strcmp(command, "on") == 0 ? 1 : 0;
		if (ioctl(fd, AUX_DISPLAY_SET_POWER, &on, sizeof(on)) != 0)
			return fail(command);
		printf("%s\n", command);
		return 0;
	}

	if (strcmp(command, "regs") == 0) {
		static const int kCounts[] = { 35, 16, 32, 0 };
		for (int block = 0; block < 3; block++) {
			uint32 words[36];
			for (int i = 0; i < kCounts[block]; i++) {
				rpi_dsi_register request = { (uint32)block, (uint32)i * 4, 0 };
				if (ioctl(fd, RPI_DSI_READ_REGISTER, &request,
						sizeof(request)) != 0) {
					return fail("read register");
				}
				words[i] = request.value;
			}
			dump_words(kBlocks[block], words, kCounts[block]);
		}
		static const struct { const char* name; uint32 offset; } kClocks[] = {
			{ "CM_PLLD", 0x10c }, { "CM_LOCK", 0x114 },
			{ "CM_DSI1ECTL", 0x158 }, { "CM_DSI1EDIV", 0x15c },
			{ "CM_DSI1PCTL", 0x160 }, { "CM_DSI1PDIV", 0x164 },
			{ "A2W_PLLD_ANA1", 0x1054 }, { "A2W_PLLD_CTRL", 0x1140 },
			{ "A2W_PLLD_FRAC", 0x1240 }, { "A2W_PLLD_DSI0", 0x1340 },
			{ "A2W_PLLD_CORE", 0x1440 }, { "A2W_PLLD_PER", 0x1540 },
			{ "A2W_PLLD_DSI1", 0x1640 },
		};
		printf("clocks\n");
		for (size_t i = 0; i < sizeof(kClocks) / sizeof(kClocks[0]); i++) {
			rpi_dsi_register request = { RPI_DSI_BLOCK_CLOCKS,
				kClocks[i].offset, 0 };
			if (ioctl(fd, RPI_DSI_READ_REGISTER, &request, sizeof(request))
					!= 0) {
				return fail("read register");
			}
			printf("  %-14s %08x\n", kClocks[i].name, (unsigned)request.value);
		}
		return 0;
	}

	if (strcmp(command, "dlist") == 0) {
		static uint32 words[4096];
		if (ioctl(fd, RPI_DSI_READ_DISPLAY_LIST, words, sizeof(words)) != 0)
			return fail("read display list");
		int from = argc > 2 ? atoi(argv[2]) : 0;
		int count = argc > 3 ? atoi(argv[3]) : 64;
		if (from < 0 || from >= 4096)
			from = 0;
		if (count < 1 || from + count > 4096)
			count = 4096 - from;
		printf("display list words %d..%d\n", from, from + count - 1);
		for (int i = 0; i < count; i++) {
			if (i % 8 == 0)
				printf("  %4d:", from + i);
			printf(" %08x", (unsigned)words[from + i]);
			if (i % 8 == 7 || i == count - 1)
				printf("\n");
		}
		return 0;
	}

	if ((strcmp(command, "read") == 0 && argc > 3)
		|| (strcmp(command, "write") == 0 && argc > 4)) {
		rpi_dsi_register request = { (uint32)block_index(argv[2]),
			(uint32)strtoul(argv[3], NULL, 0), 0 };
		uint32 op = RPI_DSI_READ_REGISTER;
		if (command[0] == 'w') {
			request.value = strtoul(argv[4], NULL, 0);
			op = RPI_DSI_WRITE_REGISTER;
		}
		if (ioctl(fd, op, &request, sizeof(request)) != 0)
			return fail(command);
		printf("%s 0x%x = 0x%08x\n", argv[2], (unsigned)request.offset,
			(unsigned)request.value);
		return 0;
	}

	if (strcmp(command, "fill") == 0 && argc > 2) {
		aux_display_info info;
		if (ioctl(fd, AUX_DISPLAY_GET_INFO, &info, sizeof(info)) != 0)
			return fail("get info");
		uint32 size;
		uint32* pixels = map_buffer(fd, 0, &size);
		if (pixels == NULL)
			return fail("clone buffer");
		uint32 color = strtoul(argv[2], NULL, 16);
		for (uint32 i = 0; i < size / 4; i++)
			pixels[i] = color;
		uint32 index = 0;
		if (ioctl(fd, AUX_DISPLAY_PRESENT, &index, sizeof(index)) != 0)
			return fail("present");
		printf("buffer 0 filled with %06x\n", (unsigned)color);
		return 0;
	}

	if (strcmp(command, "bars") == 0) {
		aux_display_info info;
		if (ioctl(fd, AUX_DISPLAY_GET_INFO, &info, sizeof(info)) != 0)
			return fail("get info");
		uint32 size;
		uint32* pixels = map_buffer(fd, 1, &size);
		if (pixels == NULL)
			return fail("clone buffer");
		static const uint32 kColors[] = { 0xffffff, 0xffff00, 0x00ffff,
			0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
		uint32 stride = info.bytes_per_row / 4;
		for (uint32 y = 0; y < info.height; y++) {
			for (uint32 x = 0; x < info.width; x++) {
				uint32 color = kColors[x * 8 / info.width];
				if (x < 2 || y < 2 || x >= info.width - 2
					|| y >= info.height - 2) {
					color = 0x808080;
				}
				if (y >= info.height * 3 / 4)
					color = (x * 255 / info.width) * 0x010101;
				pixels[y * stride + x] = color;
			}
		}
		uint32 index = 1;
		if (ioctl(fd, AUX_DISPLAY_PRESENT, &index, sizeof(index)) != 0)
			return fail("present");
		printf("colour bars in buffer 1, presented\n");
		return 0;
	}

	if (strcmp(command, "touch") == 0) {
		static const uint32 kAddresses[] = { 0x14, 0x5d };
		for (int i = 0; i < 2; i++) {
			rpi_dsi_touch_probe probe = {};
			probe.address = kAddresses[i];
			if (ioctl(fd, RPI_DSI_TOUCH_PROBE, &probe, sizeof(probe)) != 0)
				return fail("touch probe");
			printf("0x%02x: %s (BSC status 0x%x)", (unsigned)probe.address,
				probe.status == B_OK ? "answers" : strerror(probe.status),
				(unsigned)probe.controller_status);
			if (probe.status == B_OK) {
				printf(", id \"%c%c%c%c\" firmware %02x%02x",
					probe.id[0], probe.id[1], probe.id[2], probe.id[3],
					probe.id[5], probe.id[4]);
			}
			printf("\n");
		}
		return 0;
	}

	if (strcmp(command, "dump") == 0 && argc > 2) {
		aux_display_info info;
		if (ioctl(fd, AUX_DISPLAY_GET_INFO, &info, sizeof(info)) != 0)
			return fail("get info");
		rpi_dsi_state state;
		if (ioctl(fd, RPI_DSI_GET_STATE, &state, sizeof(state)) != 0)
			return fail("get state");
		uint32 size;
		uint32* pixels = map_buffer(fd, state.shown, &size);
		if (pixels == NULL)
			return fail("clone buffer");
		FILE* file = fopen(argv[2], "wb");
		if (file == NULL)
			return fail(argv[2]);
		// a header line, then the rows as B_RGB32 (BGRA in memory)
		fprintf(file, "RGB32 %u %u\n", (unsigned)info.width, (unsigned)info.height);
		for (uint32 y = 0; y < info.height; y++) {
			fwrite((uint8*)pixels + y * info.bytes_per_row, 1,
				info.width * 4, file);
		}
		fclose(file);
		printf("buffer %u written to %s\n", (unsigned)state.shown, argv[2]);
		return 0;
	}

	fprintf(stderr, "usage: rpi4_dsi state|info|step <n|off>|on|off|regs|"
		"dlist [from [n]]|read <block> <offset>|write <block> <offset> "
		"<value>|fill <rrggbb>|bars|dump <file>|touch\n");
	return 1;
}
