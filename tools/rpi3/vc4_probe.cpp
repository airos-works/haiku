/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	vc4_probe: a one-command check of the VideoCore IV 3D driver
	(/dev/graphics/vc4/0) on a Raspberry Pi 3, without Mesa.

	1. the device and its identity (GET_INFO, GET_PARAM);
	2. buffers: create, map, write and read back through the mapping;
	3. a render job without binning: the render list the driver builds
	   clears a 128x96 linear RGBA8888 surface (2x2 tiles of 64x64); every
	   pixel must have the clear colour and the page behind the surface its
	   old contents;
	4. the same with an (empty) binner list: binning, the semaphore between
	   the lists, the binner's memory, the flush interrupt;
	5. what must be refused: a surface outside its buffer, a shader buffer
	   as a surface, shaders that write memory or never end; a valid shader
	   is accepted;
	6. the driver's counters again (no resets, interrupts came).

	Build (see docs/rpi3/GPU.md):
		aarch64-unknown-haiku-g++ -O2 -Wall -I headers/private \
			-I src/add-ons/kernel/drivers/graphics/vc4 \
			tools/rpi3/vc4_probe.cpp -o vc4_probe
	Run: vc4_probe [-v]; the exit status is the number of failed checks. */


#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <OS.h>

#include <graphics/vc4/vc4_drm.h>
#include <graphics/vc4/vc4_haiku.h>

#include "vc4_packet.h"
#include "vc4_qpu_defines.h"
#include "vc4_regs.h"


#define QPU_SET_FIELD(value, field) \
	(((uint64)(value) << field ## _SHIFT) & field ## _MASK)

static const uint32 kWidth = 128;
static const uint32 kHeight = 96;
static const uint32 kSurfaceSize = kWidth * kHeight * 4;
static const uint32 kGuard = 0xdeadbeef;

static int sDevice = -1;
static int sFailures = 0;
static bool sVerbose = false;


static void
check(bool ok, const char* what, ...)
{
	va_list args;
	va_start(args, what);
	printf("%s ", ok ? "PASS" : "FAIL");
	vprintf(what, args);
	printf("\n");
	va_end(args);
	if (!ok)
		sFailures++;
}


static status_t
request(uint32 op, void* data, size_t size)
{
	while (true) {
		if (ioctl(sDevice, op, data, size) == 0)
			return B_OK;
		if (errno != B_INTERRUPTED)
			return errno;
	}
}


static uint64
get_param(uint32 param, status_t* _status = NULL)
{
	drm_vc4_get_param p = {};
	p.param = param;
	status_t status = request(VC4_HAIKU_GET_PARAM, &p, sizeof(p));
	if (_status != NULL)
		*_status = status;
	return p.value;
}


static uint32
create_bo(uint32 size)
{
	drm_vc4_create_bo create = {};
	create.size = size;
	status_t status = request(VC4_HAIKU_CREATE_BO, &create, sizeof(create));
	if (status != B_OK) {
		printf("     CREATE_BO(%" B_PRIu32 "): %s\n", size, strerror(status));
		return 0;
	}
	return create.handle;
}


static void*
map_bo(uint32 handle)
{
	drm_vc4_mmap_bo map = {};
	map.handle = handle;
	status_t status = request(VC4_HAIKU_MMAP_BO, &map, sizeof(map));
	if (status != B_OK) {
		printf("     MMAP_BO(%" B_PRIu32 "): %s\n", handle, strerror(status));
		return NULL;
	}
	return (void*)(addr_t)map.offset;
}


static void
close_bo(uint32 handle)
{
	vc4_haiku_handle close = {handle, 0};
	request(VC4_HAIKU_CLOSE_BO, &close, sizeof(close));
}


static void
print_info(const char* when)
{
	vc4_haiku_info info = {};
	status_t status = request(VC4_HAIKU_GET_INFO, &info, sizeof(info));
	if (status != B_OK) {
		printf("     GET_INFO: %s\n", strerror(status));
		return;
	}
	printf("     %s: IDENT %08" B_PRIx32 " %08" B_PRIx32 " %08" B_PRIx32
		", clock %" B_PRIu32 " Hz, pool %" B_PRIu64 " KB (%" B_PRIu64
		" KB free), %" B_PRIu64 " KB outside it, binner %" B_PRIu32
		" KB (slots %#" B_PRIx32 "), jobs %" B_PRIu64 "/%" B_PRIu64
		", resets %" B_PRIu32 ", interrupt %" B_PRIu32 " (%" B_PRIu64
		" handled)\n", when, info.ident[0], info.ident[1], info.ident[2],
		info.clock_rate, info.pool_size / 1024, info.pool_free / 1024,
		info.contiguous_bytes / 1024, info.bin_size / 1024,
		info.bin_slots_used, info.completed, info.submitted, info.resets,
		info.interrupt, info.interrupts);
}


static void
init_submit(drm_vc4_submit_cl& submit, uint32* handles, uint32 handleCount)
{
	memset(&submit, 0, sizeof(submit));
	submit.color_read.hindex = ~0u;
	submit.zs_read.hindex = ~0u;
	submit.color_write.hindex = ~0u;
	submit.msaa_color_write.hindex = ~0u;
	submit.zs_write.hindex = ~0u;
	submit.msaa_zs_write.hindex = ~0u;
	submit.bo_handles = (addr_t)handles;
	submit.bo_handle_count = handleCount;
}


/*!	A clear of the surface in handle index 0 to \a color, with or without
	an (empty) binner list. Returns whether it happened.
*/
static bool
clear_surface(uint32 surface, uint32* pixels, uint32 color, bool withBinner)
{
	const char* kind = withBinner ? "with a binner list" : "render list only";

	for (uint32 i = 0; i < (kSurfaceSize + B_PAGE_SIZE) / 4; i++)
		pixels[i] = kGuard;

	uint32 handles[1] = {surface};
	drm_vc4_submit_cl submit;
	init_submit(submit, handles, 1);

	uint8 bin[VC4_PACKET_TILE_BINNING_MODE_CONFIG_SIZE
		+ VC4_PACKET_START_TILE_BINNING_SIZE
		+ VC4_PACKET_INCREMENT_SEMAPHORE_SIZE + VC4_PACKET_FLUSH_SIZE] = {};
	if (withBinner) {
		// the driver fills in the addresses, sizes and allocation flags
		uint8* packet = bin;
		*packet = VC4_PACKET_TILE_BINNING_MODE_CONFIG;
		packet[13] = (kWidth + 63) / 64;
		packet[14] = (kHeight + 63) / 64;
		packet += VC4_PACKET_TILE_BINNING_MODE_CONFIG_SIZE;
		*packet++ = VC4_PACKET_START_TILE_BINNING;
		*packet++ = VC4_PACKET_INCREMENT_SEMAPHORE;
		*packet++ = VC4_PACKET_FLUSH;
		submit.bin_cl = (addr_t)bin;
		submit.bin_cl_size = sizeof(bin);
	}

	submit.color_write.hindex = 0;
	submit.color_write.offset = 0;
	submit.color_write.bits
		= VC4_SET_FIELD(VC4_RENDER_CONFIG_FORMAT_RGBA8888,
			VC4_RENDER_CONFIG_FORMAT)
		| VC4_SET_FIELD(VC4_TILING_FORMAT_LINEAR,
			VC4_RENDER_CONFIG_MEMORY_FORMAT);
	submit.width = kWidth;
	submit.height = kHeight;
	submit.min_x_tile = 0;
	submit.min_y_tile = 0;
	submit.max_x_tile = (kWidth - 1) / 64;
	submit.max_y_tile = (kHeight - 1) / 64;
	submit.flags = VC4_SUBMIT_CL_USE_CLEAR_COLOR;
	submit.clear_color[0] = submit.clear_color[1] = color;
	submit.clear_z = 0xffffff;
	submit.clear_s = 0;

	bigtime_t start = system_time();
	status_t status = request(VC4_HAIKU_SUBMIT_CL, &submit, sizeof(submit));
	check(status == B_OK, "SUBMIT_CL clear %s: %s, job %" B_PRIu64, kind,
		strerror(status), submit.seqno);
	if (status != B_OK)
		return false;

	drm_vc4_wait_seqno wait = {submit.seqno, 2000000000ull};
	status = request(VC4_HAIKU_WAIT_SEQNO, &wait, sizeof(wait));
	bigtime_t elapsed = system_time() - start;
	check(status == B_OK, "WAIT_SEQNO %" B_PRIu64 ": %s after %" B_PRId64
		" us", submit.seqno, strerror(status), elapsed);

	drm_vc4_wait_bo waitBo = {surface, 0, 0};
	status_t boStatus = request(VC4_HAIKU_WAIT_BO, &waitBo, sizeof(waitBo));
	check(boStatus == B_OK, "WAIT_BO (poll) after the job: %s",
		strerror(boStatus));
	if (status != B_OK)
		return false;

	uint32 wrong = 0, firstWrong = 0;
	for (uint32 i = 0; i < kSurfaceSize / 4; i++) {
		if (pixels[i] != color) {
			if (wrong++ == 0)
				firstWrong = i;
		}
	}
	check(wrong == 0, "%s: all %" B_PRIu32 " pixels %08" B_PRIx32
		" (%" B_PRIu32 " wrong%s)", kind, kSurfaceSize / 4, color, wrong,
		wrong != 0 ? ", see below" : "");
	if (wrong != 0) {
		printf("     first wrong pixel %" B_PRIu32 " (x %" B_PRIu32 ", y %"
			B_PRIu32 "): %08" B_PRIx32 "\n", firstWrong, firstWrong % kWidth,
			firstWrong / kWidth, pixels[firstWrong]);
	}

	uint32 touched = 0;
	for (uint32 i = kSurfaceSize / 4; i < (kSurfaceSize + B_PAGE_SIZE) / 4;
			i++) {
		if (pixels[i] != kGuard)
			touched++;
	}
	check(touched == 0, "%s: the page behind the surface untouched (%"
		B_PRIu32 " words changed)", kind, touched);
	return wrong == 0;
}


static uint64
qpu_nop(uint32 signal)
{
	return QPU_SET_FIELD(signal, QPU_SIG)
		| QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_ADD)
		| QPU_SET_FIELD(QPU_W_NOP, QPU_WADDR_MUL)
		| QPU_SET_FIELD(QPU_A_NOP, QPU_OP_ADD)
		| QPU_SET_FIELD(QPU_M_NOP, QPU_OP_MUL)
		| QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_A)
		| QPU_SET_FIELD(QPU_R_NOP, QPU_RADDR_B);
}


static status_t
create_shader(const uint64* code, uint32 count, uint32* _handle)
{
	drm_vc4_create_shader_bo create = {};
	create.size = count * sizeof(uint64);
	create.data = (addr_t)code;
	status_t status = request(VC4_HAIKU_CREATE_SHADER_BO, &create,
		sizeof(create));
	*_handle = create.handle;
	return status;
}


static void
test_validation(uint32 surface)
{
	// a valid shader: the end of the program and its two delay slots
	uint64 good[3] = {qpu_nop(QPU_SIG_PROG_END), qpu_nop(QPU_SIG_NONE),
		qpu_nop(QPU_SIG_NONE)};
	uint32 shader = 0;
	status_t status = create_shader(good, 3, &shader);
	check(status == B_OK, "CREATE_SHADER_BO of a valid shader: %s",
		strerror(status));

	// general VPM DMA (stores to memory)
	uint64 vpm[4] = {qpu_nop(QPU_SIG_NONE), qpu_nop(QPU_SIG_PROG_END),
		qpu_nop(QPU_SIG_NONE), qpu_nop(QPU_SIG_NONE)};
	vpm[0] = (vpm[0] & ~QPU_WADDR_ADD_MASK)
		| QPU_SET_FIELD(QPU_W_VPM_ADDR, QPU_WADDR_ADD);
	uint32 bad = 0;
	status = create_shader(vpm, 4, &bad);
	check(status == B_BAD_VALUE, "a shader with VPM DMA refused: %s",
		strerror(status));

	// no end
	uint64 endless[2] = {qpu_nop(QPU_SIG_NONE), qpu_nop(QPU_SIG_NONE)};
	status = create_shader(endless, 2, &bad);
	check(status == B_BAD_VALUE, "a shader without an end refused: %s",
		strerror(status));

	if (shader != 0) {
		// read-only, but mappable
		const uint64* code = (const uint64*)map_bo(shader);
		check(code != NULL && code[0] == good[0],
			"the shader buffer maps (read-only) with its code");

		// a shader as a surface
		uint32 handles[1] = {shader};
		drm_vc4_submit_cl submit;
		init_submit(submit, handles, 1);
		submit.color_write.hindex = 0;
		submit.color_write.bits = VC4_SET_FIELD(VC4_RENDER_CONFIG_FORMAT_RGBA8888,
			VC4_RENDER_CONFIG_FORMAT);
		submit.width = 16;
		submit.height = 16;
		status = request(VC4_HAIKU_SUBMIT_CL, &submit, sizeof(submit));
		check(status == B_BAD_VALUE, "a shader buffer as a surface refused: %s",
			strerror(status));
		close_bo(shader);
	}

	// a surface larger than its buffer
	uint32 handles[1] = {surface};
	drm_vc4_submit_cl submit;
	init_submit(submit, handles, 1);
	submit.color_write.hindex = 0;
	submit.color_write.bits = VC4_SET_FIELD(VC4_RENDER_CONFIG_FORMAT_RGBA8888,
		VC4_RENDER_CONFIG_FORMAT);
	submit.width = 1024;
	submit.height = 1024;
	submit.max_x_tile = 15;
	submit.max_y_tile = 15;
	status = request(VC4_HAIKU_SUBMIT_CL, &submit, sizeof(submit));
	check(status == B_BAD_VALUE, "a 1024x1024 surface in a %" B_PRIu32
		" byte buffer refused: %s", kSurfaceSize + B_PAGE_SIZE,
		strerror(status));

	// tiles outside the frame
	submit.width = kWidth;
	submit.height = kHeight;
	submit.max_x_tile = 2;
	submit.max_y_tile = 1;
	status = request(VC4_HAIKU_SUBMIT_CL, &submit, sizeof(submit));
	check(status == B_BAD_VALUE, "a tile outside the frame refused: %s",
		strerror(status));

	// a binner list that does not end with the semaphore and the flush
	uint8 bin[VC4_PACKET_TILE_BINNING_MODE_CONFIG_SIZE
		+ VC4_PACKET_START_TILE_BINNING_SIZE] = {};
	bin[0] = VC4_PACKET_TILE_BINNING_MODE_CONFIG;
	bin[13] = 2;
	bin[14] = 2;
	bin[VC4_PACKET_TILE_BINNING_MODE_CONFIG_SIZE]
		= VC4_PACKET_START_TILE_BINNING;
	submit.max_x_tile = 1;
	submit.bin_cl = (addr_t)bin;
	submit.bin_cl_size = sizeof(bin);
	status = request(VC4_HAIKU_SUBMIT_CL, &submit, sizeof(submit));
	check(status == B_BAD_VALUE, "an unterminated binner list refused: %s",
		strerror(status));
}


int
main(int argc, char** argv)
{
	sVerbose = argc > 1 && strcmp(argv[1], "-v") == 0;

	sDevice = open(VC4_HAIKU_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	check(sDevice >= 0, "open %s: %s", VC4_HAIKU_DEVICE_PATH,
		sDevice >= 0 ? "ok" : strerror(errno));
	if (sDevice < 0) {
		printf("     no device: is the vc4 driver in the image? The kernel "
			"log says why it did not start (grep vc4 /var/log/syslog).\n");
		return 1;
	}

	print_info("before");

	status_t status;
	uint32 ident0 = get_param(DRM_VC4_PARAM_V3D_IDENT0, &status);
	uint32 ident1 = get_param(DRM_VC4_PARAM_V3D_IDENT1);
	check(status == B_OK && ident0 == V3D_EXPECTED_IDENT0,
		"V3D_IDENT0 %08" B_PRIx32 ": \"%c%c%c\" version %" B_PRIu32,
		ident0, ident0 & 0xff, (ident0 >> 8) & 0xff, (ident0 >> 16) & 0xff,
		ident0 >> 24);
	printf("     V3D %" B_PRIu32 ".%" B_PRIu32 ", %" B_PRIu32 " slice(s), %"
		B_PRIu32 " QPUs and %" B_PRIu32 " TMUs per slice, %" B_PRIu32
		" KB VPM\n", ident0 >> 24, ident1 & 0xf, (ident1 >> 4) & 0xf,
		(ident1 >> 8) & 0xf, (ident1 >> 12) & 0xf, (ident1 >> 28) * 4);
	check(get_param(DRM_VC4_PARAM_SUPPORTS_BRANCHES) == 1
			&& get_param(DRM_VC4_PARAM_SUPPORTS_THREADED_FS) == 1
			&& get_param(DRM_VC4_PARAM_SUPPORTS_ETC1) == 1
			&& get_param(DRM_VC4_PARAM_SUPPORTS_FIXED_RCL_ORDER) == 1,
		"GET_PARAM: branches, threaded FS, ETC1, fixed RCL order");
	get_param(99, &status);
	check(status == B_BAD_VALUE, "GET_PARAM of an unknown parameter refused");

	// buffers
	uint32 surface = create_bo(kSurfaceSize + B_PAGE_SIZE);
	uint32* pixels = surface != 0 ? (uint32*)map_bo(surface) : NULL;
	check(pixels != NULL, "CREATE_BO + MMAP_BO of %" B_PRIu32 " bytes",
		kSurfaceSize + B_PAGE_SIZE);
	if (pixels == NULL)
		return sFailures;

	bool zero = true;
	for (uint32 i = 0; i < (kSurfaceSize + B_PAGE_SIZE) / 4; i++)
		zero &= pixels[i] == 0;
	check(zero, "a new buffer is cleared");
	for (uint32 i = 0; i < kSurfaceSize / 4; i++)
		pixels[i] = i * 2654435761u;
	bool same = true;
	for (uint32 i = 0; i < kSurfaceSize / 4; i++)
		same &= pixels[i] == i * 2654435761u;
	check(same, "the mapping reads back what was written");

	area_id area = area_for(pixels);
	status = delete_area(area);
	check(status != B_OK, "the mapping cannot be deleted by the team: %s",
		strerror(status));

	uint32 big = create_bo(8 * 1024 * 1024);
	check(big != 0 && map_bo(big) != NULL, "an 8 MB buffer");
	if (big != 0)
		close_bo(big);

	// jobs
	clear_surface(surface, pixels, 0xff336699, false);
	clear_surface(surface, pixels, 0x80ff4020, true);
	clear_surface(surface, pixels, 0x12345678, false);

	test_validation(surface);

	vc4_haiku_info info = {};
	request(VC4_HAIKU_GET_INFO, &info, sizeof(info));
	print_info("after");
	check(info.resets == 0, "no job hung (%" B_PRIu32 " resets)", info.resets);
	check(info.interrupts > 0, "the core's interrupts arrive");

	drm_vc4_get_hang_state hang = {};
	status = request(VC4_HAIKU_GET_HANG_STATE, &hang, sizeof(hang));
	if (status == B_OK) {
		printf("     last hang: bin %#" B_PRIx32 " at %#" B_PRIx32 ", render %#"
			B_PRIx32 " at %#" B_PRIx32 ", CT0CS %#" B_PRIx32 " CT1CS %#"
			B_PRIx32 ", ERRSTAT %#" B_PRIx32 "\n", hang.start_bin, hang.ct0ca,
			hang.start_render, hang.ct1ca, hang.ct0cs, hang.ct1cs,
			hang.errstat);
	}

	close_bo(surface);
	close(sDevice);

	printf("%s: %d check(s) failed\n", sFailures == 0 ? "PASS" : "FAIL",
		sFailures);
	return sFailures;
}
