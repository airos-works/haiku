/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMDGPU_FIRMWARE_LOADER_H
#define AMDGPU_FIRMWARE_LOADER_H

#include "Firmware.h"
#include <SupportDefs.h>

class InstalledFirmware {
public:
	InstalledFirmware();
	~InstalledFirmware();
	status_t Load(const char* name, bool smc);
	status_t LoadUvd();
	status_t LoadGfx(uint32 index);
	status_t LoadMec();
	amdgpu::FirmwareView view;
	amdgpu::MecFirmwareView mec;
private:
	status_t ReadData(const char* name, size_t limit);
	status_t Read(const char* name, size_t limit,
		bool (*parse)(const void*, size_t, amdgpu::FirmwareView&));
	void* fData;
	size_t fSize;
	InstalledFirmware(const InstalledFirmware&) = delete;
	InstalledFirmware& operator=(const InstalledFirmware&) = delete;
};
#endif
