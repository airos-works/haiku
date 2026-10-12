/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <amdgpu_haiku.h>
#include <FindDirectory.h>
#include <StorageDefs.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void Require(bool okay, const char* message)
{
	if (!okay) {
		fprintf(stderr, "FAIL: %s (%s)\n", message, strerror(errno));
		exit(1);
	}
}

static FILE*
OpenInstalledFirmware(unsigned index)
{
	const char* names[] = {"polaris10_ce_2.bin", "polaris10_pfp_2.bin",
		"polaris10_me_2.bin", "polaris10_rlc.bin", "polaris10_mec_2.bin"};
	const directory_which places[] = {B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY};
	for (directory_which place : places) {
		char path[B_PATH_NAME_LENGTH];
		status_t status = find_directory(place, -1, false, path, sizeof(path));
		if (status != B_OK) {
			errno = status;
			return NULL;
		}
		if (strlcat(path, "/firmware/amdgpu/", sizeof(path)) >= sizeof(path)
			|| strlcat(path, names[index], sizeof(path)) >= sizeof(path)) {
			errno = B_NAME_TOO_LONG;
			return NULL;
		}
		FILE* file = fopen(path, "rb");
		if (file != NULL) {
			printf("Installed GFX firmware: %s\n", path);
			return file;
		}
		if (errno != B_ENTRY_NOT_FOUND)
			return NULL;
	}
	return NULL;
}

int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	const bool installed = argc == 2 && strcmp(argv[1], "--installed") == 0;
	if (!installed && argc != 5 && argc != 6) {
		fprintf(stderr, "usage: amdgpu_gfx --installed\n"
			"       amdgpu_gfx CE.bin PFP.bin ME.bin RLC.bin [MEC.bin]\n");
		return 2;
	}
	const bool withMec = installed || argc == 6;
	const uint32 op = withMec ? AMDGPU_GFX_MEC_TEST : AMDGPU_GFX_TEST;
	const size_t requestSize = withMec ? sizeof(amdgpu_gfx_mec_test)
		: sizeof(amdgpu_gfx_test);
	amdgpu_gfx_mec_test request = {};
	request.gfx.version = AMDGPU_HAIKU_ABI_VERSION;
	request.gfx.size = requestSize;
	uint8* images[5] = {};
	for (unsigned i = 0; i < (withMec ? 5u : 4u); i++) {
		FILE* file = installed ? OpenInstalledFirmware(i) : fopen(argv[i + 1], "rb");
		Require(file != NULL, "open firmware");
		Require(fseek(file, 0, SEEK_END) == 0, "firmware seek");
		long size = ftell(file);
		Require(size >= 44 && size <= (i == 4 ? 320 * 1024 : 65536), "firmware size");
		rewind(file);
		images[i] = (uint8*)malloc(size);
		Require(images[i] != NULL, "allocate firmware input");
		Require(fread(images[i], 1, size, file) == (size_t)size, "read firmware");
		fclose(file);
		if (i == 4) {
			request.mec_firmware = (addr_t)images[i];
			request.mec_firmware_size = size;
		} else {
			request.gfx.firmware[i] = (addr_t)images[i];
			request.gfx.firmware_size[i] = size;
		}
	}
	int fd = open("/dev/" AMDGPU_DEVICE_NAME, O_RDWR);
	Require(fd >= 0, "open amdgpu");
	Require(ioctl(fd, op, NULL, requestSize) == -1
		&& errno == B_BAD_ADDRESS, "null request rejected");
	for (unsigned i = 0; i < (withMec ? 5u : 4u); i++) {
		auto bad = request;
		if (i == 4)
			bad.mec_firmware = 0;
		else
			bad.gfx.firmware[i] = 0;
		Require(ioctl(fd, op, &bad, requestSize) == 0
			&& bad.gfx.stage == 0 && bad.gfx.status == B_BAD_ADDRESS,
			"invalid firmware pointer rejected");
		uint8 saved = images[i][12];
		images[i][12] = 0xff;
		bad = request;
		Require(ioctl(fd, op, &bad, requestSize) == 0
			&& bad.gfx.stage == 0 && bad.gfx.status == B_BAD_DATA,
			"wrong firmware IP rejected");
		images[i][12] = saved;
	}
	if (withMec) {
		auto bad = request;
		bad.reserved = 1;
		Require(ioctl(fd, op, &bad, requestSize) == -1 && errno == B_BAD_VALUE,
			"reserved MEC request field rejected");
		uint8 saved = images[4][32];
		images[4][32] ^= 1;
		bad = request;
		Require(ioctl(fd, op, &bad, requestSize) == 0
			&& bad.gfx.stage == 0 && bad.gfx.status == B_BAD_DATA,
			"mixed CP/MEC firmware features rejected");
		images[4][32] = saved;
	}
	puts("PASS: invalid GFX firmware requests rejected before hardware writes");
	if (installed) {
		// Start the existing owned SMC/SDMA service from system firmware if
		// this is the boot's first engine client. GFX remains root-only.
		amdgpu_memory_info memory = {};
		memory.version = AMDGPU_HAIKU_ABI_VERSION;
		memory.size = sizeof(memory);
		Require(ioctl(fd, AMDGPU_MEMORY_INFO, &memory, sizeof(memory)) == 0,
			"start installed DMA service");
	}
	uint32 firstSequence = 0, previousSequence = 0;
	for (unsigned round = 0; round < 61; round++) {
		auto response = request;
		Require(ioctl(fd, op, &response, requestSize) == 0, "GFX ioctl");
		const auto& result = response.gfx;
		if (round == 0)
			firstSequence = result.sequence;
		else {
			uint32 expected = previousSequence + 1;
			if (expected == 0)
				expected++;
			Require(result.sequence == expected, "ordered diagnostic sequence");
		}
		previousSequence = result.sequence;
		printf("GFX stage %u status %#x (%s) seq %u, checked %u, mismatches %u\n",
			(unsigned)result.stage, (unsigned)result.status, strerror(result.status),
			(unsigned)result.sequence, (unsigned)result.checked_bytes, (unsigned)result.mismatches);
		printf("CP %#x ring %#x pointers %u/%u GRBM %#x RLC %#x\n",
			(unsigned)result.cp_control, (unsigned)result.ring_control,
			(unsigned)result.rptr, (unsigned)result.wptr,
			(unsigned)result.grbm_status, (unsigned)result.rlc_status);
		amdgpu_gart_info gart = {};
		gart.version = AMDGPU_HAIKU_ABI_VERSION;
		gart.size = sizeof(gart);
		Require(ioctl(fd, AMDGPU_GART_INFO, &gart, sizeof(gart)) == 0, "VM fault query");
		printf("VM fault status %#x, VMID %u, GPU page %#x, client %#x\n",
			(unsigned)result.vm_fault_status,
			(unsigned)((result.vm_fault_status >> 25) & 15),
			(unsigned)result.vm_fault_address,
			(unsigned)result.vm_fault_client);
		if (result.status != B_OK || result.stage != 5 || result.checked_bytes != 12288
			|| result.mismatches != 0) {
			errno = result.status != B_OK ? result.status : B_BAD_DATA;
			Require(false, "GFX execution, data, guards and VM faults");
		}
	}
	close(fd);
	for (uint8* image : images)
		free(image);
	if (firstSequence == 1) {
		puts("PASS: 61 GFX8 submissions including direct writes, CE indirect fetch, compute address-space switches, rasterization and indirect buffers, complete data/guard checks");
	} else {
		// The diagnostic schedule belongs to the persistent engine, not the
		// descriptor. Reopening continues IB checks after its initial suite.
		printf("PASS: 61 continued GFX8 submissions, sequences %u through %u, "
			"complete data/guard checks\n", (unsigned)firstSequence,
			(unsigned)previousSequence);
	}
	return 0;
}
