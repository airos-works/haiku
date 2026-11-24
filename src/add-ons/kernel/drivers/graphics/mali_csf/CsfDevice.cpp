/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#include <KernelExport.h>
#include <AutoDeleterOS.h>
#include <condition_variable.h>
#include <lock.h>
#include <smp.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__aarch64__)
#include <arch/arm64/cache_line_size.h>
#endif

#include "CsfRun.h"
#include "CsfCommands.h"
#include "CsfDevice.h"

using namespace MaliCSF;

#include "CsfDmaMemory.h"
#include "CsfFirmwareHardware.h"

bool
FirmwareMemoryRetained()
{
	// The area belongs to the system team and survives close, process exit,
	// controller removal and module unload. The name also makes a reloaded
	// driver refuse another cycle after uncertain cleanup. Only a verified
	// normal cycle deletes it; reboot reclaims a deliberately retained area.
	return find_area(kDmaAreaName) >= B_OK;
}


status_t
RunFirmwareRequest(const ResourceInfo& resources, void* buffer, size_t length,
	bool& needsRecovery)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	if (buffer == NULL)
		return B_BAD_ADDRESS;
	if (length < sizeof(FirmwareRunInfo) || length - sizeof(FirmwareRunInfo) > kMaxFirmwareBytes)
		return B_BAD_VALUE;
	uint32 header[2];
	status_t status = user_memcpy(header, buffer, sizeof(header));
	if (status != B_OK)
		return status;
	if (header[0] != kFirmwareRunVersion || header[1] < 20
		|| header[1] != length - sizeof(FirmwareRunInfo))
		return B_BAD_VALUE;
	void* file = malloc(header[1]);
	if (file == NULL)
		return B_NO_MEMORY;
	status = user_memcpy(file, (uint8*)buffer + sizeof(FirmwareRunInfo), header[1]);
	FirmwareImage image;
	FirmwareMemory memory;
	if (status == B_OK && (image.Init(file, header[1]) != FIRMWARE_OK
		|| image.Info().versionHash != 0x01050000 || !memory.Plan(image)))
		status = B_BAD_DATA;
	area_id area = status == B_OK ? AllocateFirmwareMemory(memory) : status;
	if (area < B_OK) {
		free(file);
		return area;
	}
	FirmwareRunInfo info = {};
	info.version = kFirmwareRunVersion;
	info.firmwareBytes = header[1];
	info.flags = kFirmwareAllocated;
	info.tablePages = memory.TablePages();
	info.allocationBytes = memory.RequiredBytes();
	info.rootPhysical = memory.RootPhysical();
	info.translationConfig = FirmwareMemory::TranslationConfig();
	info.memoryAttributes = FirmwareMemory::MemoryAttributes();
	FirmwareHardware hardware;
	status = hardware.Init(resources);
	if (status == B_OK)
		CycleFirmware(hardware, memory, info);
	// Retain one arena after any failed hardware cycle, also preserving the
	// recovery guard across module removal when failure preceded AS exposure.
	// Failures before the cycle starts need no hardware recovery. Never let
	// closing the diagnostic process release uncertain GPU-owned memory.
	bool releasable = (info.flags & kFirmwareNeedsRecovery) == 0
		&& ((info.flags & kFirmwareAddressSpace) == 0
			|| ((info.flags & (kFirmwareCleaned | kFirmwarePowerRestored))
				== (kFirmwareCleaned | kFirmwarePowerRestored)));
	if (releasable && delete_area(area) == B_OK)
		area = -1;
	else {
		info.flags |= kFirmwareMemoryRetained | kFirmwareNeedsRecovery;
	}
	needsRecovery = (info.flags & kFirmwareNeedsRecovery) != 0;
	free(file);
	dprintf("mali_csf: firmware result=%u cleanup=%u flags=%#x tables=%u bytes=%u"
		" root=%#" B_PRIx64 " boot=%u ping=%u version=%#x power=%u restore=%u\n",
		info.result, info.cleanupResult, info.flags, info.tablePages, info.allocationBytes,
		info.rootPhysical, info.boot.count, info.ping.count, info.interface.version,
		info.power.result, info.power.restoreResult);
	if (status != B_OK)
		return status;
	return user_memcpy(buffer, &info, sizeof(info));
}

status_t
RunCommandRequest(const ResourceInfo& resources, void* buffer, size_t length,
	bool& needsRecovery, bool shader)
{
	if (geteuid() != 0)
		return B_NOT_ALLOWED;
	if (buffer == NULL)
		return B_BAD_ADDRESS;
	if (length < sizeof(CommandRunInfo) || length - sizeof(CommandRunInfo) > kMaxFirmwareBytes)
		return B_BAD_VALUE;
	uint32 header[2];
	status_t status = user_memcpy(header, buffer, sizeof(header));
	if (status != B_OK)
		return status;
	if (header[0] != kCommandRunVersion || header[1] < 20
		|| header[1] != length - sizeof(CommandRunInfo))
		return B_BAD_VALUE;
	void* file = malloc(header[1]);
	CommandRunInfo* info = (CommandRunInfo*)calloc(1, sizeof(CommandRunInfo));
	if (file == NULL || info == NULL) {
		free(file); free(info);
		return B_NO_MEMORY;
	}
	status = user_memcpy(file, (uint8*)buffer + sizeof(CommandRunInfo), header[1]);
	FirmwareImage image;
	CommandMemory memory;
	if (status == B_OK && (image.Init(file, header[1]) != FIRMWARE_OK
		|| image.Info().versionHash != 0x01050000 || !memory.Plan(image)))
		status = B_BAD_DATA;
	area_id area = status == B_OK ? AllocateFirmwareMemory(memory) : status;
	if (area < B_OK) {
		free(file); free(info);
		return area;
	}
	info->version = kCommandRunVersion;
	info->firmwareBytes = header[1];
	info->result = info->cleanupResult = kCommandNotAttempted;
	if (shader) {
		// No application GPU mapping exists yet. Replace the default command
		// regression with the fixed Linux-matched shader and descriptors.
		BuildStoreShader(memory.Code());
		info->flags = kCommandShader;
	}
	info->rootPhysical = memory.RootPhysical();
	info->userTablePages = CommandMemory::kUserTablePages;
	info->userBytes = CommandMemory::kUserBytes;
	info->arenaBytes = memory.RequiredBytes();
	FirmwareRunInfo& firmware = info->firmware;
	firmware.version = kFirmwareRunVersion;
	firmware.firmwareBytes = header[1];
	firmware.flags = kFirmwareAllocated;
	firmware.tablePages = memory.Firmware().TablePages();
	firmware.allocationBytes = memory.Firmware().RequiredBytes();
	firmware.rootPhysical = memory.Firmware().RootPhysical();
	firmware.translationConfig = FirmwareMemory::TranslationConfig();
	firmware.memoryAttributes = FirmwareMemory::MemoryAttributes();
	FirmwareHardware hardware(true);
	status = hardware.Init(resources);
	if (status == B_OK) {
		CommandOperation operation(memory, *info);
		CycleFirmware(hardware, memory.Firmware(), firmware, operation);
	}
	bool releasable = (firmware.flags & kFirmwareNeedsRecovery) == 0
		&& ((firmware.flags & kFirmwareAddressSpace) == 0
			|| ((firmware.flags & (kFirmwareCleaned | kFirmwarePowerRestored))
				== (kFirmwareCleaned | kFirmwarePowerRestored)))
		&& ((info->flags & kCommandMapped) == 0 || (info->flags & kCommandUnmapped) != 0);
	if (!releasable || delete_area(area) != B_OK)
		firmware.flags |= kFirmwareMemoryRetained | kFirmwareNeedsRecovery;
	needsRecovery = (firmware.flags & kFirmwareNeedsRecovery) != 0;
	free(file);
	dprintf("mali_csf: commands result=%u cleanup=%u flags=%#x rounds=%u"
		" fw=%u cleanup=%u flags=%#x root=%#" B_PRIx64 " events=%#x\n",
		info->result, info->cleanupResult, info->flags, info->roundsCompleted,
		firmware.result, firmware.cleanupResult, firmware.flags, info->rootPhysical,
		info->groupEvents);
	if (status == B_OK)
		status = user_memcpy(buffer, info, sizeof(*info));
	free(info);
	return status;
}
