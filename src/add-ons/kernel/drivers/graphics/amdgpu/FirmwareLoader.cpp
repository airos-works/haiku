/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "FirmwareLoader.h"
#include <KernelExport.h>
#include <FindDirectory.h>
#include <StorageDefs.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

InstalledFirmware::InstalledFirmware()
	: view(), mec(), fData(NULL), fSize(0)
{
}

InstalledFirmware::~InstalledFirmware()
{
	free(fData);
}

status_t
InstalledFirmware::Load(const char* name, bool smc)
{
	return Read(name, smc ? 0x20100 : 65536,
		smc ? amdgpu::ParseSmcFirmware : amdgpu::ParseSdmaFirmware);
}

status_t
InstalledFirmware::LoadUvd()
{
	status_t status = Read("polaris10_uvd.bin", 1024 * 1024, amdgpu::ParseUvdFirmware);
	return status == B_OK && view.version != 0x01008210 ? B_BAD_DATA : status;
}

status_t
InstalledFirmware::LoadGfx(uint32 index)
{
	const char* names[] = {"polaris10_ce_2.bin", "polaris10_pfp_2.bin",
		"polaris10_me_2.bin", "polaris10_rlc.bin"};
	const uint32 versions[] = {140, 254, 167, 286};
	if (index >= 4)
		return B_BAD_VALUE;
	status_t status = ReadData(names[index], 65536);
	if (status == B_OK && (!amdgpu::ParseGfxFirmware(fData, fSize, index == 3, view)
		|| view.version != versions[index] || view.featureVersion != (index == 3 ? 1u : 49u)))
		status = B_BAD_DATA;
	return status;
}

status_t
InstalledFirmware::LoadMec()
{
	status_t status = ReadData("polaris10_mec_2.bin", 320 * 1024);
	if (status == B_OK && (!amdgpu::ParseMecFirmware(fData, fSize, mec)
		|| mec.program.version != 730 || mec.program.featureVersion != 49))
		status = B_BAD_DATA;
	return status;
}

status_t
InstalledFirmware::Read(const char* name, size_t limit,
	bool (*parse)(const void*, size_t, amdgpu::FirmwareView&))
{
	status_t status = ReadData(name, limit);
	if (status == B_OK && !parse(fData, fSize, view))
		status = B_BAD_DATA;
	if (status == B_OK)
		dprintf("amdgpu: installed firmware %s version %#x feature %u\n",
			name, (unsigned)view.version, (unsigned)view.featureVersion);
	return status;
}

status_t
InstalledFirmware::ReadData(const char* name, size_t limit)
{
	// Only fixed driver filenames are accepted; never search user directories.
	if (fData != NULL || name == NULL || strchr(name, '/') != NULL)
		return B_BAD_VALUE;
	const directory_which places[] = {B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY};
	int fd = -1;
	char path[B_PATH_NAME_LENGTH];
	for (directory_which place : places) {
		status_t status = find_directory(place, -1, false, path, sizeof(path));
		if (status != B_OK)
			return status;
		if (strlcat(path, "/firmware/amdgpu/", sizeof(path)) >= sizeof(path)
			|| strlcat(path, name, sizeof(path)) >= sizeof(path))
			return B_NAME_TOO_LONG;
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd >= 0)
			break;
		if (errno != B_ENTRY_NOT_FOUND)
			return errno;
	}
	if (fd < 0)
		return B_ENTRY_NOT_FOUND;
	struct stat info;
	status_t status = fstat(fd, &info) == 0 ? B_OK : errno;
	if (status == B_OK && (!S_ISREG(info.st_mode) || info.st_size < 36
		|| (uint64)info.st_size > limit))
		status = B_BAD_DATA;
	if (status == B_OK) {
		fData = malloc(info.st_size);
		if (fData == NULL)
			status = B_NO_MEMORY;
	}
	if (status == B_OK) {
		ssize_t bytes = read(fd, fData, info.st_size);
		if (bytes != info.st_size)
			status = bytes < 0 ? errno : B_IO_ERROR;
	}
	close(fd);
	if (status == B_OK) {
		fSize = info.st_size;
		dprintf("amdgpu: read installed firmware %s (%lu bytes)\n", path,
			(unsigned long)fSize);
	}
	return status;
}
