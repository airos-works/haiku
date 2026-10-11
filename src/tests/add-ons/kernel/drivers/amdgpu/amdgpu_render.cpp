/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <OS.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void Require(bool okay, const char* message)
{
	if (okay) return;
	fprintf(stderr, "FAIL: %s (%d: %s)\n", message, errno, strerror(errno));
	exit(1);
}
static amdgpu_render_info Request()
{
	amdgpu_render_info r = {};
	r.version = AMDGPU_HAIKU_ABI_VERSION; r.size = sizeof(r);
	return r;
}
static void Reject(int fd, amdgpu_render_info& r, size_t size)
{
	Require(ioctl(fd, AMDGPU_RENDER_INFO, &r, size) == -1 && errno == B_BAD_VALUE,
		"malformed query rejected");
}
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	bool absent = argc == 2 && strcmp(argv[1], "--expect-no-device") == 0;
	Require(absent || argc == 1, "usage: amdgpu_render [--expect-no-device]");
	if (!absent && geteuid() == 0)
		Require(setgid(65534) == 0 && setuid(65534) == 0, "drop privilege");
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	if (absent) {
		Require(fd < 0 && errno == ENOENT, "no AMD device in QEMU");
		puts("PASS: no-device render-info handling"); return 0;
	}
	Require(fd >= 0, "open native render query");
	amdgpu_render_info r = Request();
	Require(ioctl(fd, AMDGPU_RENDER_INFO, NULL, sizeof(r)) == -1 && errno == B_BAD_ADDRESS,
		"null query rejected");
	Reject(fd, r, sizeof(r) - 1);
	r.version++; Reject(fd, r, sizeof(r));
	r = Request(); r.size--; Reject(fd, r, sizeof(r));
	r = Request(); r.flags = 1; Reject(fd, r, sizeof(r));
	r = Request(); r.reserved = 1; Reject(fd, r, sizeof(r));
	int readOnly = open("/dev/" AMDGPU_DEVICE_NAME, O_RDONLY);
	Require(readOnly >= 0, "open read-only client");
	r = Request();
	Require(ioctl(readOnly, AMDGPU_RENDER_INFO, &r, sizeof(r)) == -1 && errno == B_NOT_ALLOWED,
		"query startup requires read/write client");
	close(readOnly);
	Require(ioctl(fd, AMDGPU_RENDER_INFO, &r, sizeof(r)) == 0, "query installed renderer");
	printf("GFX %u.%u revision %u: %u SE, %u SA/SE, %u CU/SA, %u RB/SE, %u tile pipes, %u TCC\n",
		r.gfx_major, r.gfx_minor, r.chip_revision, r.shader_engines,
		r.shader_arrays_per_engine, r.cu_per_array, r.backends_per_engine, r.tile_pipes, r.tcc_blocks);
	printf("active CUs %u RB mask %#x GB_ADDR_CONFIG %#x; VRAM type %u width %u "
		"RAMCFG %#x CHMAP %#x MISC0 %#x\n", r.active_cus, r.enabled_backends,
		r.gb_addr_config, r.vram_type, r.vram_bus_width, r.mc_arb_ramcfg, r.mc_shared_chmap, r.mc_seq_misc0);
	Require(r.gfx_major == 8 && r.gfx_minor == 1 && r.shader_engines == 4
		&& r.shader_arrays_per_engine == 1 && r.cu_per_array == 9 && r.backends_per_engine == 2
		&& r.tile_pipes == 8 && r.tcc_blocks == 8, "WX5100 ROM dimensions");
	Require(r.active_cus > 0 && r.active_cus <= 36 && r.enabled_backends != 0
		&& (r.enabled_backends & ~255u) == 0, "bounded active hardware");
	Require(r.vram_type == 5 && r.vram_bus_width == 256, "WX5100 GDDR5 memory configuration");
	for (uint32 se = 0; se < 4; se++)
		printf("SE%u CU %#x fuse %#x user %#x; RB fuse %#x user %#x raster %#x/%#x\n", se,
			r.cu_mask[se][0], r.cu_disable[se][0], r.cu_user_disable[se][0],
			r.rb_disable[se][0], r.rb_user_disable[se][0], r.raster_config[se][0], r.raster_config_1[se][0]);
	for (uint32 i = 0; i < 32; i++) printf("tile[%u]=%#x\n", i, r.tile_mode[i]);
	for (uint32 i = 0; i < 16; i++) printf("macrotile[%u]=%#x\n", i, r.macrotile_mode[i]);
	for (uint32 i = 0; i < 5; i++) printf("firmware[%u]=%u feature %u\n", i,
		r.firmware_version[i], r.firmware_feature[i]);
	printf("clocks kHz default %u/%u reference %u timestamp %u; selector %#x/%#x\n",
		r.default_engine_khz, r.default_memory_khz, r.reference_khz, r.timestamp_khz,
		r.grbm_index_before, r.grbm_index_after);
	Require(r.grbm_index_before == r.grbm_index_after && r.reference_khz == 100000
		&& r.default_engine_khz == 300000 && r.default_memory_khz == 300000,
		"restored selector and ROM clock defaults");
	Require(r.capabilities == (AMDGPU_RENDER_ROOT_SUBMIT | AMDGPU_RENDER_SYNC_SUBMIT
		| AMDGPU_RENDER_COPY_SUBMIT | AMDGPU_RENDER_CHECKED_COPY | AMDGPU_RENDER_QUEUE_COPY)
		&& r.total_vram == (8ULL << 30) && r.visible_vram == (256ULL << 20)
		&& r.total_gart == (1ULL << 30) && r.address_start == 131072 && r.address_end == (1ULL << 36)
		&& r.max_buffer_bytes == (64ULL << 20) && r.max_mapping_bytes == r.max_buffer_bytes
		&& r.max_mapped_bytes == (16ULL << 30) && r.max_mappings == 1024
		&& r.max_vm_clients == 32 && r.max_buffers == 256 && r.page_size == 4096
		&& r.max_ib_bytes == 65536 && r.ib_address_alignment == 256 && r.ib_size_alignment == 1024,
		"native capabilities and allocation/submission limits");
	snooze(100000);
	amdgpu_render_info second = Request();
	Require(ioctl(fd, AMDGPU_RENDER_INFO, &second, sizeof(second)) == 0, "repeat render query");
	uint64 elapsed = second.started_us - r.started_us;
	uint64 ticks = second.gpu_timestamp - r.gpu_timestamp;
	printf("timestamp delta %llu over %llu us at %u kHz\n",
		(unsigned long long)ticks, (unsigned long long)elapsed, r.timestamp_khz);
	uint64 expected = elapsed * r.timestamp_khz / 1000;
	Require(second.gpu_timestamp > r.gpu_timestamp && r.timestamp_khz > 0
		&& ticks > expected * 8 / 10 && ticks < expected * 12 / 10, "GPU timestamp clock rate");
	second.gpu_timestamp = r.gpu_timestamp;
	second.started_us = r.started_us; second.finished_us = r.finished_us;
	Require(memcmp(&r, &second, sizeof(r)) == 0, "stable complete hardware snapshot");
	close(fd);
	puts("PASS: ordinary-client render-info ABI, hardware geometry, tiling, clocks and limits");
	return 0;
}
