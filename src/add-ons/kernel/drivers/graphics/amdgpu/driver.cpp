/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <Drivers.h>
#include <KernelExport.h>
#include <PCI.h>
#include <boot_item.h>
#include <frame_buffer_console.h>
#include <lock.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <amdgpu_haiku.h>
#include "Rom.h"
#include "Sdma.h"
#include "Smu.h"
#include "Device.h"
#include "FirmwareLoader.h"
#include "UserMemory.h"

// Polaris 10 register indices, from AMD's MIT-licensed register headers:
// Linux drivers/gpu/drm/amd/include/asic_reg/{bif/bif_5_0_d.h,
// gmc/gmc_8_1_d.h,gca/gfx_8_0_d.h,oss/oss_3_0_d.h,uvd/uvd_6_0_d.h,
// dce/dce_11_2_d.h}.
// Only direct, non-destructive reads; no indexed or interrupt-ack registers.
static const uint32 kSnapshotRegisters[] = {
	0x150a,	// CONFIG_MEMSIZE (MiB)
	0x0809,	// MC_VM_FB_LOCATION
	0x081a,	// MC_VM_FB_OFFSET
	0x080d,	// MC_VM_SYSTEM_APERTURE_LOW_ADDR
	0x080e,	// MC_VM_SYSTEM_APERTURE_HIGH_ADDR
	0x0504,	// VM_CONTEXT0_CNTL
	0x2004,	// GRBM_STATUS
	0x263e,	// GB_ADDR_CONFIG
	0x0394,	// SRBM_STATUS
	0x340d,	// SDMA0_STATUS_REG
	0x3412,	// SDMA0_F32_CNTL
	0x3480,	// SDMA0_GFX_RB_CNTL
	0x360d,	// SDMA1_STATUS_REG
	0x3612,	// SDMA1_F32_CNTL
	0x3daf,	// UVD_STATUS
	0x38c4,	// UVD_POWER_STATUS
	// Each head: CRTC_CONTROL, GRPH_PRIMARY_SURFACE_ADDRESS, _HIGH, PITCH.
	0x1b9c, 0x1a04, 0x1a07, 0x1a06,
	0x1d9c, 0x1c04, 0x1c07, 0x1c06,
	0x1f9c, 0x1e04, 0x1e07, 0x1e06,
	0x419c, 0x4004, 0x4007, 0x4006,
	0x439c, 0x4204, 0x4207, 0x4206,
	0x459c, 0x4404, 0x4407, 0x4406,
};

static const uint64 kRegisterMapSize = 0x40000;
static pci_module_info* sPCI;
static pci_info sDevice;
static amdgpu_info sInfo;
static area_id sRegisterArea = -1;
static volatile uint32* sRegisters;
static mutex sLock = MUTEX_INITIALIZER("amdgpu");
static uint32 sOpenCount;
static bool sStartupAttempted;
static status_t sStartupStatus = B_DEV_NOT_READY;
static const char* sDeviceNames[] = { AMDGPU_DEVICE_NAME, NULL };

int32 api_version = B_CUR_DRIVER_API_VERSION;


static bool
is_supported(const pci_info& info)
{
	// Admit only the board being brought up. Sharing a Polaris family name
	// does not establish that another board has been tested.
	return info.vendor_id == 0x1002 && info.device_id == 0x67c7
		&& info.class_base == PCI_display && (info.header_type & 0x7f) == 0;
}


static status_t
find_device(pci_module_info* pci, pci_info& info)
{
	for (uint32 index = 0; pci->get_nth_pci_info(index, &info) == B_OK; index++) {
		if (is_supported(info))
			return B_OK;
	}
	return B_ENTRY_NOT_FOUND;
}


static status_t
map_registers()
{
	memset(&sInfo, 0, sizeof(sInfo));
	sInfo.version = AMDGPU_HAIKU_ABI_VERSION;
	sInfo.size = sizeof(sInfo);
	sInfo.vendor = sDevice.vendor_id;
	sInfo.device = sDevice.device_id;
	sInfo.subsystem_vendor = sDevice.u.h0.subsystem_vendor_id;
	sInfo.subsystem_device = sDevice.u.h0.subsystem_id;
	sInfo.revision = sDevice.revision;
	sInfo.bus = sDevice.bus;
	sInfo.slot = sDevice.device;
	sInfo.function = sDevice.function;
	sInfo.interrupt = sDevice.u.h0.interrupt_line;
	sInfo.pci_command = sPCI->read_pci_config(sDevice.bus, sDevice.device,
		sDevice.function, PCI_command, 2);
	if ((sInfo.pci_command & PCI_command_memory) == 0)
		return B_NOT_ALLOWED;

	for (uint32 bar = 0; bar < 6; bar++) {
		sInfo.bar_address[bar] = sDevice.u.h0.base_registers[bar];
		sInfo.bar_size[bar] = sDevice.u.h0.base_register_sizes[bar];
		uint32 flags = sDevice.u.h0.base_register_flags[bar];
		if ((flags & PCI_address_space) == 0
			&& (flags & PCI_address_type) == PCI_address_type_64) {
			if (bar == 5)
				return B_BAD_VALUE;
			sInfo.bar_address[bar]
				|= (uint64)sDevice.u.h0.base_registers[bar + 1] << 32;
			sInfo.bar_size[bar]
				|= (uint64)sDevice.u.h0.base_register_sizes[bar + 1] << 32;
			bar++;
		}
	}

	// GCN 1.2 and newer expose registers at BAR5, not the older BAR2.
	if ((sDevice.u.h0.base_register_flags[5] & PCI_address_space)
			!= 0
		|| sInfo.bar_address[5] == 0 || sInfo.bar_size[5] < kRegisterMapSize)
		return B_BAD_VALUE;

	sRegisterArea = map_physical_memory("amdgpu registers", sInfo.bar_address[5],
		kRegisterMapSize, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA,
		(void**)&sRegisters);
	if (sRegisterArea < 0)
		return sRegisterArea;

	sInfo.vram_size = (uint64)sRegisters[0x150a] << 20;
	sInfo.vram_gpu_base = (uint64)(sRegisters[0x0809] & 0xffff) << 24;
	const frame_buffer_boot_info* boot = (const frame_buffer_boot_info*)
		get_boot_item(FRAME_BUFFER_BOOT_INFO, NULL);
	if (boot != NULL && boot->width > 0 && boot->height > 0
		&& boot->bytes_per_row > 0) {
		sInfo.boot_framebuffer = boot->physical_frame_buffer;
		sInfo.boot_width = boot->width;
		sInfo.boot_height = boot->height;
		sInfo.boot_stride = boot->bytes_per_row;
		sInfo.boot_framebuffer_size
			= (uint64)boot->height * boot->bytes_per_row;
	}
	dprintf("amdgpu: WX 5100 %02x:%02x.%u rev %02x, %" B_PRIu64
		" MiB VRAM, read-only observation\n", sDevice.bus, sDevice.device,
		sDevice.function, sDevice.revision, sInfo.vram_size >> 20);
	return B_OK;
}


static status_t
device_open(const char* name, uint32 flags, void** cookie)
{
	if (strcmp(name, AMDGPU_DEVICE_NAME) != 0)
		return B_ENTRY_NOT_FOUND;
	AmdgpuClient* client = amdgpu_client_open(flags);
	if (client == NULL)
		return B_NO_MEMORY;
	mutex_lock(&sLock);
	status_t status = sRegisterArea < 0 ? map_registers() : B_OK;
	if (status == B_OK) {
		sOpenCount++;
		*cookie = client;
	}
	mutex_unlock(&sLock);
	if (status != B_OK)
		amdgpu_client_free(client);
	return status;
}


static status_t
device_close(void* cookie)
{
	return B_OK;
}


static status_t
device_free(void* cookie)
{
	amdgpu_client_free((AmdgpuClient*)cookie);
	mutex_lock(&sLock);
	if (--sOpenCount == 0 && !amdgpu_device_active()) {
		delete_area(sRegisterArea);
		sRegisterArea = -1;
		sRegisters = NULL;
	}
	mutex_unlock(&sLock);
	return B_OK;
}


static status_t
start_installed_device()
{
	mutex_lock(&sLock);
	if (amdgpu_device_active() || sStartupAttempted) {
		status_t status = amdgpu_device_active() ? B_OK : sStartupStatus;
		mutex_unlock(&sLock);
		return status;
	}
	InstalledFirmware smc, sdma;
	status_t status = smc.Load("polaris10_smc.bin", true);
	if (status == B_OK)
		status = sdma.Load("polaris10_sdma.bin", false);
	// Automatic startup uses the versions qualified on this exact board.
	if (status == B_OK && (smc.view.version != 0x171a00
		|| sdma.view.version != 58 || sdma.view.featureVersion != 31))
		status = B_BAD_DATA;
	void* rom = status == B_OK ? malloc(AMDGPU_ROM_SIZE) : NULL;
	if (status == B_OK && rom == NULL)
		status = B_NO_MEMORY;
	bool writable = false;
	if (status == B_OK) {
		status = set_area_protection(sRegisterArea, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		writable = status == B_OK;
	}
	amdgpu::AtomVramReservation reservation;
	amdgpu::AtomRenderInfo renderInfo = {};
	if (status == B_OK)
		status = amdgpu_read_rom(sDevice, sRegisters, rom, AMDGPU_ROM_SIZE);
	if (status == B_OK && !amdgpu::ParseAtomVramReservation(rom, AMDGPU_ROM_SIZE, reservation))
		status = B_BAD_DATA;
	if (status == B_OK)
		amdgpu::ParseAtomRenderInfo(rom, AMDGPU_ROM_SIZE, renderInfo);
	if (status == B_OK) {
		// Missing/malformed files can be repaired and retried. Once hardware
		// startup begins, retain its failure until reload/cold-boot preflight.
		sStartupAttempted = true;
		if (!amdgpu_smc_ready(sRegisters)) {
			amdgpu_smc_bootstrap result = {};
			status = amdgpu_smc_bootstrap_firmware(sRegisters, smc.view, result);
		}
		if (status == B_OK)
			status = amdgpu_device_start(sRegisters, sInfo, sdma.view, reservation,
				amdgpu::IsQualifiedUvdClockRom(rom, AMDGPU_ROM_SIZE), sPCI, renderInfo);
		sStartupStatus = status;
		dprintf("amdgpu: automatic client startup status %#x\n", (unsigned)status);
	}
	if (writable && status != B_OK)
		set_area_protection(sRegisterArea, B_KERNEL_READ_AREA);
	free(rom);
	mutex_unlock(&sLock);
	return status;
}


static status_t
device_control(void* cookie, uint32 op, void* buffer, size_t length)
{
	if ((op >= AMDGPU_CREATE_BUFFER && op <= AMDGPU_GART_INFO)
		|| (op >= AMDGPU_VIDEO_CREATE && op <= AMDGPU_HEVC_DECODE)
		|| op == AMDGPU_CREATE_DEVICE_BUFFER
		|| op == AMDGPU_GFX_SUBMIT_COPY || op == AMDGPU_GFX_QUEUE_COPY
		|| (op >= AMDGPU_VM_INFO && op <= AMDGPU_RENDER_INFO))
	{
		// Raw PM4/shader privilege isolation and fault recovery remain under
		// qualification. Bounded VM_TEST continues to accept ordinary clients.
		if ((op == AMDGPU_GFX_SUBMIT || op == AMDGPU_GFX_SUBMIT_COPY
			|| op == AMDGPU_GFX_QUEUE_COPY) && geteuid() != 0)
			return B_NOT_ALLOWED;
		return amdgpu_client_control((AmdgpuClient*)cookie, op, buffer, length,
			start_installed_device);
	}
	if (op == AMDGPU_DISPLAY_SNAPSHOT) {
		if (length != sizeof(amdgpu_display_snapshot))
			return B_BAD_VALUE;
		uint32 header[4];
		if (user_memcpy(header, buffer, sizeof(header)) != B_OK)
			return B_BAD_ADDRESS;
		if (header[0] != AMDGPU_HAIKU_ABI_VERSION
			|| header[1] != sizeof(amdgpu_display_snapshot) || header[3] != 0)
			return B_BAD_VALUE;
		// DCE 11.2 direct status/configuration registers. Reading these does
		// not acknowledge interrupts or alter the firmware's display state.
		static const uint32 bases[] = {0x1a00, 0x1c00, 0x1e00,
			0x4000, 0x4200, 0x4400};
		static const uint32 offsets[] = {
			0x19c, 0x1a3, 0x1a4, 0x1a6,
			0x180, 0x181, 0x182, 0x187, 0x18d, 0x18e,
			0x0, 0x1, 0x6, 0x4, 0x7, 0x5, 0x8,
			0x15c, 0x15d, 0x66, 0x67, 0x69, 0x68, 0x6a, 0x6b
		};
		static_assert(B_COUNT_OF(bases) == AMDGPU_DCE_HEAD_COUNT,
			"display head count");
		static_assert(B_COUNT_OF(offsets) == AMDGPU_DCE_REGISTER_COUNT,
			"display register count");
		amdgpu_display_snapshot result = {};
		result.version = AMDGPU_HAIKU_ABI_VERSION;
		result.size = sizeof(result);
		result.head_count = AMDGPU_DCE_HEAD_COUNT;
		mutex_lock(&sLock);
		result.started_us = system_time();
		for (uint32 head = 0; head < AMDGPU_DCE_HEAD_COUNT; head++) {
			result.heads[head].register_base = bases[head];
			for (uint32 reg = 0; reg < AMDGPU_DCE_REGISTER_COUNT; reg++)
				result.heads[head].registers[reg] = sRegisters[bases[head] + offsets[reg]];
			result.hpd_status[head] = sRegisters[0x1898 + head * 8];
		}
		result.finished_us = system_time();
		mutex_unlock(&sLock);
		return user_memcpy(buffer, &result, sizeof(result));
	}
	if (op == AMDGPU_UVD_TEST) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		if (length != sizeof(amdgpu_uvd_test))
			return B_BAD_VALUE;
		amdgpu_uvd_test request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != AMDGPU_HAIKU_ABI_VERSION || request.size != sizeof(request)
			|| request.firmware_size < 32 || request.firmware_size > 1024 * 1024
			|| request.output == 0 || request.output_capacity != AMDGPU_UVD_TEST_OUTPUT_BYTES)
			return B_BAD_VALUE;
		amdgpu_uvd_test result = {};
		result.version = AMDGPU_HAIKU_ABI_VERSION;
		result.size = sizeof(result);
		void* image = malloc(request.firmware_size);
		void* output = malloc(AMDGPU_UVD_TEST_OUTPUT_BYTES);
		status_t status = image == NULL || output == NULL ? B_NO_MEMORY
			: AmdgpuCopyFromUser(image, request.firmware, request.firmware_size);
		amdgpu::FirmwareView firmware;
		if (status == B_OK && (!amdgpu::ParseUvdFirmware(image, request.firmware_size, firmware)
			|| firmware.version != 0x01008210))
			status = B_BAD_DATA;
		if (status == B_OK) {
			mutex_lock(&sLock);
			status = amdgpu_device_uvd_test(firmware, result, output);
			mutex_unlock(&sLock);
			if (result.checked_bytes == AMDGPU_UVD_TEST_OUTPUT_BYTES) {
				status_t copied = AmdgpuCopyToUser(request.output, output,
					AMDGPU_UVD_TEST_OUTPUT_BYTES);
				if (status == B_OK)
					status = copied;
			}
		}
		free(image);
		free(output);
		result.status = status;
		return user_memcpy(buffer, &result, sizeof(result));
	}
	if (op == AMDGPU_GFX_TEST || op == AMDGPU_GFX_MEC_TEST) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		bool withMec = op == AMDGPU_GFX_MEC_TEST;
		size_t requiredSize = withMec ? sizeof(amdgpu_gfx_mec_test)
			: sizeof(amdgpu_gfx_test);
		if (length != requiredSize)
			return B_BAD_VALUE;
		amdgpu_gfx_mec_test extendedRequest = {};
		if (user_memcpy(&extendedRequest, buffer, requiredSize) != B_OK)
			return B_BAD_ADDRESS;
		const amdgpu_gfx_test& request = extendedRequest.gfx;
		if (request.version != AMDGPU_HAIKU_ABI_VERSION || request.size != requiredSize
			|| extendedRequest.reserved != 0)
			return B_BAD_VALUE;
		amdgpu_gfx_mec_test extendedResult = {};
		amdgpu_gfx_test& result = extendedResult.gfx;
		result.version = AMDGPU_HAIKU_ABI_VERSION;
		result.size = requiredSize;
		void* images[5] = {};
		amdgpu::FirmwareView firmware[4];
		amdgpu::MecFirmwareView mec = {};
		status_t status = B_OK;
		for (uint32 i = 0; i < 4 && status == B_OK; i++) {
			uint32 size = request.firmware_size[i];
			if (size < 44 || size > 65536) {
				status = B_BAD_VALUE;
				break;
			}
			images[i] = malloc(size);
			if (images[i] == NULL) {
				status = B_NO_MEMORY;
				break;
			}
			status = AmdgpuCopyFromUser(images[i], request.firmware[i], size);
			if (status == B_OK && !amdgpu::ParseGfxFirmware(images[i], size, i == 3, firmware[i]))
				status = B_BAD_DATA;
		}
		if (withMec && status == B_OK) {
			uint32 size = extendedRequest.mec_firmware_size;
			if (size < 44 || size > 320 * 1024)
				status = B_BAD_VALUE;
			else {
				images[4] = malloc(size);
				status = images[4] == NULL ? B_NO_MEMORY
					: AmdgpuCopyFromUser(images[4], extendedRequest.mec_firmware, size);
				if (status == B_OK && !amdgpu::ParseMecFirmware(images[4], size, mec))
					status = B_BAD_DATA;
			}
			if (status == B_OK) {
				for (uint32 i = 0; i < 3; i++) {
					if (firmware[i].featureVersion != mec.program.featureVersion)
						status = B_BAD_DATA;
				}
			}
		}
		if (status == B_OK) {
			mutex_lock(&sLock);
			status = amdgpu_device_gfx_test(firmware, result, withMec ? &mec : NULL);
			mutex_unlock(&sLock);
		}
		for (void* image : images)
			free(image);
		result.status = status;
		return user_memcpy(buffer, &extendedResult, requiredSize);
	}
	if (op == AMDGPU_START_DMA) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		if (length != sizeof(amdgpu_dma_init))
			return B_BAD_VALUE;
		amdgpu_dma_init request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != AMDGPU_HAIKU_ABI_VERSION || request.size != sizeof(request)
			|| request.reserved != 0 || request.firmware_size < 52 || request.firmware_size > 65536)
			return B_BAD_VALUE;
		void* firmware = malloc(request.firmware_size);
		void* rom = malloc(AMDGPU_ROM_SIZE);
		if (firmware == NULL || rom == NULL) {
			free(firmware);
			free(rom);
			return B_NO_MEMORY;
		}
		status_t status = AmdgpuCopyFromUser(firmware, request.firmware, request.firmware_size);
		amdgpu::FirmwareView view;
		if (status == B_OK && !amdgpu::ParseSdmaFirmware(firmware, request.firmware_size, view))
			status = B_BAD_DATA;
		if (status == B_OK) {
			mutex_lock(&sLock);
			if (amdgpu_device_active())
				status = B_BUSY;
			else {
				status = set_area_protection(sRegisterArea, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
				if (status == B_OK) {
					amdgpu::AtomVramReservation reservation;
					amdgpu::AtomRenderInfo renderInfo = {};
					status = amdgpu_read_rom(sDevice, sRegisters, rom, AMDGPU_ROM_SIZE);
					if (status == B_OK && !amdgpu::ParseAtomVramReservation(rom, AMDGPU_ROM_SIZE, reservation))
						status = B_BAD_DATA;
					if (status == B_OK)
						amdgpu::ParseAtomRenderInfo(rom, AMDGPU_ROM_SIZE, renderInfo);
					if (status == B_OK)
						status = amdgpu_device_start(sRegisters, sInfo, view, reservation,
							amdgpu::IsQualifiedUvdClockRom(rom, AMDGPU_ROM_SIZE), sPCI, renderInfo);
					if (status != B_OK)
						set_area_protection(sRegisterArea, B_KERNEL_READ_AREA);
				}
			}
			mutex_unlock(&sLock);
		}
		free(firmware);
		free(rom);
		return status;
	}
	if (op == AMDGPU_SMC_BOOTSTRAP) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		if (length != sizeof(amdgpu_smc_bootstrap))
			return B_BAD_VALUE;
		amdgpu_smc_bootstrap request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != AMDGPU_HAIKU_ABI_VERSION
			|| request.size != sizeof(request) || request.reserved != 0
			|| request.firmware_size < 36 || request.firmware_size > 0x20100)
			return B_BAD_VALUE;
		void* firmware = malloc(request.firmware_size);
		if (firmware == NULL)
			return B_NO_MEMORY;
		status_t status = AmdgpuCopyFromUser(firmware, request.firmware,
			request.firmware_size);
		amdgpu::FirmwareView view;
		if (status == B_OK
			&& !amdgpu::ParseSmcFirmware(firmware, request.firmware_size, view))
			status = B_BAD_DATA;
		amdgpu_smc_bootstrap result = {};
		result.version = AMDGPU_HAIKU_ABI_VERSION;
		result.size = sizeof(result);
		if (status == B_OK) {
			mutex_lock(&sLock);
			status = amdgpu_device_active() ? B_BUSY : set_area_protection(sRegisterArea,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
			if (status == B_OK) {
				status = amdgpu_smc_bootstrap_firmware(sRegisters, view, result);
				set_area_protection(sRegisterArea, B_KERNEL_READ_AREA);
			}
			mutex_unlock(&sLock);
		}
		free(firmware);
		result.status = status;
		return user_memcpy(buffer, &result, sizeof(result));
	}
	if (op == AMDGPU_SDMA_TEST) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		if (length != sizeof(amdgpu_sdma_test_result))
			return B_BAD_VALUE;
		amdgpu_sdma_test_result request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != AMDGPU_HAIKU_ABI_VERSION
			|| request.size != sizeof(request) || request.reserved != 0
			|| request.firmware_size < 52 || request.firmware_size > 65536)
			return B_BAD_VALUE;
		void* firmware = malloc(request.firmware_size);
		void* rom = malloc(AMDGPU_ROM_SIZE);
		if (firmware == NULL || rom == NULL) {
			free(firmware);
			free(rom);
			return B_NO_MEMORY;
		}
		status_t status = AmdgpuCopyFromUser(firmware, request.firmware,
			request.firmware_size);
		amdgpu::FirmwareView view;
		if (status == B_OK
			&& !amdgpu::ParseSdmaFirmware(firmware, request.firmware_size, view))
			status = B_BAD_DATA;
		amdgpu_sdma_test_result result = {};
		result.version = AMDGPU_HAIKU_ABI_VERSION;
		result.size = sizeof(result);
		if (status == B_OK) {
			mutex_lock(&sLock);
			status = amdgpu_device_active() ? B_BUSY : set_area_protection(sRegisterArea,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
			if (status == B_OK) {
				status = amdgpu_read_rom(sDevice, sRegisters, rom, AMDGPU_ROM_SIZE);
				amdgpu::AtomVramReservation reservation;
				if (status == B_OK && !amdgpu::ParseAtomVramReservation(rom,
						AMDGPU_ROM_SIZE, reservation))
					status = B_BAD_DATA;
				if (status == B_OK)
					status = amdgpu_sdma_test(sRegisters, sInfo, view, reservation,
						result);
				set_area_protection(sRegisterArea, B_KERNEL_READ_AREA);
			}
			mutex_unlock(&sLock);
		}
		free(firmware);
		free(rom);
		result.status = status;
		return user_memcpy(buffer, &result, sizeof(result));
	}
	if (op == AMDGPU_READ_ROM) {
		if (geteuid() != 0)
			return B_NOT_ALLOWED;
		if (length != sizeof(amdgpu_rom))
			return B_BAD_VALUE;
		amdgpu_rom request;
		if (user_memcpy(&request, buffer, sizeof(request)) != B_OK)
			return B_BAD_ADDRESS;
		if (request.version != AMDGPU_HAIKU_ABI_VERSION
			|| request.size != sizeof(request) || request.reserved != 0
			|| request.capacity != AMDGPU_ROM_SIZE)
			return B_BAD_VALUE;
		void* rom = malloc(AMDGPU_ROM_SIZE);
		if (rom == NULL)
			return B_NO_MEMORY;
		mutex_lock(&sLock);
		status_t status = amdgpu_device_active() ? B_BUSY : set_area_protection(sRegisterArea,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
		if (status == B_OK) {
			status = amdgpu_read_rom(sDevice, sRegisters, rom, AMDGPU_ROM_SIZE);
			set_area_protection(sRegisterArea, B_KERNEL_READ_AREA);
		}
		mutex_unlock(&sLock);
		if (status == B_OK)
			status = AmdgpuCopyToUser(request.data, rom, AMDGPU_ROM_SIZE);
		free(rom);
		return status;
	}
	if (op != AMDGPU_GET_INFO)
		return B_DEV_INVALID_IOCTL;
	if (length != sizeof(amdgpu_info))
		return B_BAD_VALUE;

	uint32 header[2];
	if (user_memcpy(header, buffer, sizeof(header)) != B_OK)
		return B_BAD_ADDRESS;
	if (header[0] != AMDGPU_HAIKU_ABI_VERSION || header[1] != sizeof(amdgpu_info))
		return B_BAD_VALUE;

	mutex_lock(&sLock);
	amdgpu_info info = sInfo;
	info.register_count = B_COUNT_OF(kSnapshotRegisters);
	static_assert(B_COUNT_OF(kSnapshotRegisters) == AMDGPU_INFO_REGISTER_COUNT,
		"register snapshot size");
	for (uint32 i = 0; i < info.register_count; i++) {
		info.registers[i].index = kSnapshotRegisters[i];
		info.registers[i].value = sRegisters[kSnapshotRegisters[i]];
	}
	mutex_unlock(&sLock);
	return user_memcpy(buffer, &info, sizeof(info));
}


static status_t
device_read(void* cookie, off_t position, void* buffer, size_t* length)
{
	*length = 0;
	return B_NOT_ALLOWED;
}


static status_t
device_write(void* cookie, off_t position, const void* buffer, size_t* length)
{
	*length = 0;
	return B_NOT_ALLOWED;
}


static device_hooks sHooks = {
	device_open, device_close, device_free, device_control,
	device_read, device_write, NULL, NULL, NULL, NULL
};


status_t
init_hardware()
{
	pci_module_info* pci;
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&pci);
	if (status != B_OK)
		return status;
	pci_info info;
	status = find_device(pci, info);
	put_module(B_PCI_MODULE_NAME);
	dprintf("amdgpu: probe: %s\n", status == B_OK
		? "WX 5100 found" : "no supported device");
	return status;
}


status_t
init_driver()
{
	status_t status = get_module(B_PCI_MODULE_NAME, (module_info**)&sPCI);
	if (status != B_OK)
		return status;
	status = find_device(sPCI, sDevice);
	if (status != B_OK) {
		put_module(B_PCI_MODULE_NAME);
		sPCI = NULL;
	}
	return status;
}


void
uninit_driver()
{
	amdgpu_device_stop();
	if (sRegisterArea >= 0) {
		delete_area(sRegisterArea);
		sRegisterArea = -1;
		sRegisters = NULL;
	}
	put_module(B_PCI_MODULE_NAME);
}


const char**
publish_devices()
{
	return sDeviceNames;
}


device_hooks*
find_device(const char* name)
{
	return strcmp(name, AMDGPU_DEVICE_NAME) == 0 ? &sHooks : NULL;
}
