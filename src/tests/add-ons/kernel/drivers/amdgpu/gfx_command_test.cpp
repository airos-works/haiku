/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "GfxCommandValidator.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <vector>

using amdgpu::GfxCommandValidation;
using amdgpu::ValidateGfx8Commands;
using Words = std::vector<uint32_t>;
static unsigned checks;

static void Check(const Words& words, bool allowed)
{
	auto result = ValidateGfx8Commands(words.data(), words.size());
	if ((result.error == GfxCommandValidation::OK) != allowed) {
		fprintf(stderr, "unexpected policy result %d at %u, expected %s\n",
			result.error, result.dword, allowed ? "accept" : "reject");
		abort();
	}
	checks++;
}
static Words Packet(uint32_t op, std::initializer_list<uint32_t> p, uint32_t flags = 0)
{
	Words words; words.reserve(p.size() + 1);
	words.push_back(0xc0000000 | (uint32_t(p.size() - 1) << 16) | op << 8 | flags);
	for (uint32_t word : p) words.push_back(word);
	return words;
}
static void SelfTest()
{
	Check({}, false); Check({0x80000000, 0xffff1000}, true);
	Check({0}, false); Check({0x40000000}, false); Check({0x80000001}, false);
	Check({0xc0011200, 0}, false); Check(Packet(0x12, {0}), true);
	Check(Packet(0x12, {1}), false); Check(Packet(0x12, {0}, 1), false);
	Check(Packet(0x12, {0}, 2), false); Check(Packet(0x12, {0}, 4), false);
	for (uint32_t op : {0x22,0x23,0x33,0x3f,0x51,0x58,0x5e,0x5f,0x61,0x63,0x68,0x77,0x80,0x81,0x9b,0x9f})
		Check(Packet(op, {0,0,0}), false);
	Check(Packet(0x69, {0xda, 0}), false); // CP_VMID is inside context space.
	Check(Packet(0x69, {0xd7,0,0,0,0}), false); // Register span crosses privilege boundary.
	Check(Packet(0x69, {0x100002aa,0}), true);
	Check(Packet(0x69, {0x100002aa,0,0}), false);
	Check(Packet(0x69, {0x200002aa,0}), false);
	Check(Packet(0x69, {0x000102aa,0}), false);
	Check(Packet(0x69, {0xffffffff,0}), false);
	Check(Packet(0x76, {0x214,0}), false); // COMPUTE_VMID.
	for (uint32_t stage = 0; stage < 6; stage++) {
		uint32_t base = stage * 0x40;
		for (uint32_t i = 0; i < 4; i++) Check(Packet(0x76, {base + i,0}), false);
		Check(Packet(0x76, {base + 10,0x002c0040}), true);
		Check(Packet(0x76, {base + 10,0x003c0040}), false); // PRIV.
		Check(Packet(0x76, {base + 10,0x006c0040}), false); // DEBUG_MODE.
		Check(Packet(0x76, {base + 11,0x40}), false); // TRAP_PRESENT.
		Check(Packet(0x76, {base + 9,0x100}), false); // INST_ATC.
	}
	Check(Packet(0x76, {0x212,0x003c0040}), false);
	Check(Packet(0x76, {0x213,0x40}), false);
	Check(Packet(0x76, {0x213,0x2000}), false); // Compute exception bits.
	Check(Packet(0x76, {0x213,0x1000000}), false);
	Check(Packet(0x76, {0x20c,0x10000000}), false); // Shader outside 64 GiB VA.
	Check(Packet(0x76, {0x7,0x80000000}), false); // Reserved RSRC3 bits.
	Check(Packet(0x76, {0x215,0x80000000}), false); // Reserved resource-limit bits.
	Check(Packet(0x79, {0x10000242,4}), true);
	Check(Packet(0x79, {0x242,0x80000000}), false);
	Check(Packet(0x79, {0x200,0}), false); // Unreviewed UCONFIG register.
	Check(Packet(0x15, {1,1,1,0x45},2), true);
	Check(Packet(0x15, {1,1,1,0x1045},2), false); // DATA_ATC.
	Check(Packet(0x15, {1,1,1,0x4045},2), false); // RESTORE.
	Check(Packet(0x28, {0x80000001,0x80000000}), false); // Global state load.
	Check(Packet(0x28, {0x80000000,0x80000001}), false); // Global state shadow.
	Check(Packet(0x38, {0,0x51,0x53,0x52,1,0,0,20,0}), true);
	Check(Packet(0x38, {0,0x213,0x53,0x52,1,0,0,20,0}), false); // Indirect register write.
	Check(Packet(0x38, {0,0x51,0x53,0x80000214,1,0,0,20,0}), false);
	Check(Packet(0x38, {0,0x51,0x53,0x40000052,1,0x20000,16,20,0}), false);
	Check(Packet(0x37, {0x100500,0x20000,0,0x12345678}), true);
	Check(Packet(0x37, {0x100000,0x20000,0,0x12345678}), false); // MMIO destination.
	Check(Packet(0x37, {0x100500,0x10000,0,0}), false); // Private command VA.
	Check(Packet(0x37, {0x100500,0x20000,16,0}), false); // VA high bits.
	Check(Packet(0x37, {0x100500,0xfffffffc,15,0,0}), false); // Span overflow.
	Check(Packet(0x50, {0xe0300000,0x20000,0,0x30000,0,0x40000010}), true);
	Check(Packet(0x50, {0xe0300000,0x20000,0,0x30000,0,0x44000010}), false); // SAS MMIO.
	Check(Packet(0x50, {0xe0300000,0x20000,0,0x30000,0,0x48000010}), false); // DAS MMIO.
	Check(Packet(0x50, {0xa0300000,0x20000,0,0x30000,0,16}), false); // GDS source.
	Check(Packet(0x50, {0xe0100000,0x20000,0,0x30000,0,16}), false); // GDS destination.
	Check(Packet(0x3c, {3,0xc03f,0,1,1,4}), true);
	Check(Packet(0x3c, {3,0x8c00,0,1,1,4}), false); // Arbitrary MMIO poll.
	Check(Packet(0x47, {0x528,0x20000,0x20000000,1,0}), true);
	Check(Packet(0x47, {0x528,0x20000,0x22000000,1,0}), false); // Client IRQ injection.
	Check(Packet(0x47, {0x528,0x20000,0xa0000000,1,0}), false); // GDS read.
	Check(Packet(0x46, {0x33}), false); // Thread trace start.
	Check(Packet(0x40, {0x100509,0,0,0x20000,0}), true); // Timestamp to own VA.
	Check(Packet(0x40, {0x100500,0,0,0x20000,0}), false); // MMIO source.
	Words oversized(16385, 0x80000000); Check(oversized, false);
	puts("PASS: command policy privilege/control/address rejection cases");
}
static void Capture(const char* path)
{
	FILE* f = fopen(path, "rb"); assert(f);
	assert(fseek(f,0,SEEK_END) == 0); long size = ftell(f);
	assert(size > 0 && size <= 65536 && size % 4 == 0);
	assert(fseek(f,0,SEEK_SET) == 0); Words words(size / 4);
	assert(fread(words.data(),1,size,f) == (size_t)size); fclose(f);
	auto result = ValidateGfx8Commands(words.data(),words.size());
	if (result.error != GfxCommandValidation::OK) {
		fprintf(stderr,"%s: rejected error %d at DWORD %u = %#x\n",path,
			result.error,result.dword,words[result.dword]); exit(1);
	}
	// Every cut inside an actual packet must fail; cuts on packet boundaries
	// are valid prefixes. Allocate exactly each prefix so ASan detects overread.
	std::vector<bool> boundaries(words.size()+1,false);
	for (unsigned at=0;at<words.size();) {
		uint32_t h=words[at];
		at += h == 0xffff1000 || h == 0x80000000 ? 1 : ((h >> 16) & 0x3fff) + 2;
		boundaries[at]=true;
	}
	for (unsigned n=0;n<=words.size();n++) Check(Words(words.begin(),words.begin()+n),boundaries[n]);
	// Exercise altered words/counts without inventing an acceptance oracle.
	uint32_t random=0x51008;
	for (unsigned n=0;n<256;n++) {
		random=random*1664525+1013904223; unsigned at=random%words.size();
		Words changed=words; random=random*1664525+1013904223; changed[at]=random;
		auto mutated=ValidateGfx8Commands(changed.data(),changed.size());
		assert(mutated.error == GfxCommandValidation::OK || mutated.dword < changed.size()); checks++;
	}
}
int main(int argc,char** argv)
{
	SelfTest();
	for (int i=1;i<argc;i++) Capture(argv[i]);
	printf("PASS: %u policy checks; %d complete real IBs accepted\n",checks,argc-1);
	return 0;
}
