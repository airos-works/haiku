/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Firmware.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <vector>

static void Reject(const std::vector<uint8_t>& bytes)
{
	amdgpu::AtomRenderInfo info;
	memset(&info, 0xa5, sizeof(info));
	assert(!amdgpu::ParseAtomRenderInfo(bytes.data(), bytes.size(), info));
	amdgpu::AtomRenderInfo zero = {};
	assert(memcmp(&info, &zero, sizeof(info)) == 0);
}

int main(int argc, char** argv)
{
	assert(argc == 2);
	FILE* file = fopen(argv[1], "rb");
	assert(file != NULL && fseek(file, 0, SEEK_END) == 0);
	long size = ftell(file);
	assert(size == 256 * 1024);
	rewind(file);
	std::vector<uint8_t> bytes(size);
	assert(fread(bytes.data(), 1, size, file) == (size_t)size);
	fclose(file);
	amdgpu::AtomRenderInfo info;
	assert(amdgpu::ParseAtomRenderInfo(bytes.data(), bytes.size(), info));
	assert(info.gfxMajor == 8 && info.gfxMinor == 1 && info.shaderEngines == 4
		&& info.shaderArraysPerEngine == 1 && info.cuPerArray == 9
		&& info.backendsPerEngine == 2 && info.tilePipes == 8 && info.tccBlocks == 8);
	assert(info.defaultEngineKHz == 300000 && info.defaultMemoryKHz == 300000
		&& info.referenceKHz == 100000);
	size_t rom = amdgpu::ReadLE16(bytes.data() + 0x48);
	size_t master = amdgpu::ReadLE16(bytes.data() + rom + 32);
	size_t gfx = amdgpu::ReadLE16(bytes.data() + master + 32);
	size_t fw = amdgpu::ReadLE16(bytes.data() + master + 12);
	size_t end = 0;
	for (size_t table : {rom, master, gfx, fw}) {
		size_t extent = table + amdgpu::ReadLE16(bytes.data() + table);
		if (extent > end) end = extent;
	}
	for (size_t n = 0; n < end; n++) {
		std::vector<uint8_t> shortRom(bytes.begin(), bytes.begin() + n);
		Reject(shortRom);
	}
	assert(amdgpu::ParseAtomRenderInfo(bytes.data(), end, info));
	std::vector<uint8_t> unaligned(bytes.size() + 1);
	memcpy(unaligned.data() + 1, bytes.data(), bytes.size());
	assert(amdgpu::ParseAtomRenderInfo(unaligned.data() + 1, bytes.size(), info));
	for (size_t pos : {size_t(0x48), rom + 32, master + 32, master + 12}) {
		for (uint8_t fill : {uint8_t(0), uint8_t(255)}) {
			auto bad = bytes;
			memset(bad.data() + pos, fill, 2);
			Reject(bad);
		}
	}
	for (size_t pos : {rom, master, gfx, fw}) {
		auto bad = bytes;
		bad[pos] = 1; bad[pos + 1] = 0;
		Reject(bad);
	}
	for (size_t pos : {gfx + 2, gfx + 3, gfx + 5, gfx + 6, gfx + 7,
			gfx + 8, gfx + 9, gfx + 10, gfx + 11, fw + 2, fw + 3}) {
		for (uint8_t fill : {uint8_t(0), uint8_t(255)}) {
			auto bad = bytes;
			bad[pos] = fill;
			Reject(bad);
		}
	}
	for (size_t pos : {fw + 8, fw + 12, fw + 82}) {
		auto bad = bytes;
		memset(bad.data() + pos, 0, pos == fw + 82 ? 2 : 4);
		Reject(bad);
	}
	auto bad = bytes;
	bad[gfx + 9] = 2; bad[gfx + 10] = 3;
	Reject(bad); // fractional render-backend bank width
	auto v21 = bytes;
	v21[gfx] = 12; v21[gfx + 1] = 0; v21[gfx + 3] = 1;
	assert(amdgpu::ParseAtomRenderInfo(v21.data(), v21.size(), info));
	printf("PASS: WX5100 ATOM geometry/clocks, %zu truncations, unaligned input, "
		"bad extents, revisions and dimensions\n", end);
}
