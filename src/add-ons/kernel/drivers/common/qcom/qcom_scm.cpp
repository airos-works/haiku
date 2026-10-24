/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "qcom_scm.h"

#include <string.h>

#include <KernelExport.h>
#include <smp.h>


#define ERROR(x...)	dprintf("qcom_scm: " x)
#define INFO(x...)	dprintf("qcom_scm: " x)


#define SCM_PAS_INIT_IMAGE		0x01
#define SCM_PAS_MEM_SETUP		0x02
#define SCM_PAS_AUTH_AND_RESET	0x05
#define SCM_PAS_SHUTDOWN		0x06
#define SCM_INTERRUPTED			1
#define SCM_V2_EBUSY			((uint64)-12)

#define MDT_TYPE_MASK			(7u << 24)
#define MDT_TYPE_HASH			(2u << 24)
#define MDT_RELOCATABLE			(1u << 27)
#define ELF_PT_LOAD				1


struct elf32_header {
	uint8	ident[16];
	uint16	type;
	uint16	machine;
	uint32	version;
	uint32	entry;
	uint32	phoff;
	uint32	shoff;
	uint32	flags;
	uint16	ehsize;
	uint16	phentsize;
	uint16	phnum;
	uint16	shentsize;
	uint16	shnum;
	uint16	shstrndx;
};

struct elf32_program_header {
	uint32	type;
	uint32	offset;
	uint32	vaddr;
	uint32	paddr;
	uint32	filesz;
	uint32	memsz;
	uint32	flags;
	uint32	align;
};


struct scm_arguments {
	uint64	function;
	uint64	info;
	uint64	a0, a1, a2, a3;
	uint64	r0, r1;
	uint32	interrupted;
};


static void
scm_smc(void* data, int cpu)
{
	scm_arguments* arguments = (scm_arguments*)data;
	register uint64 r0 asm("x0") = arguments->function;
	register uint64 r1 asm("x1") = arguments->info;
	register uint64 r2 asm("x2") = arguments->a0;
	register uint64 r3 asm("x3") = arguments->a1;
	register uint64 r4 asm("x4") = arguments->a2;
	register uint64 r5 asm("x5") = arguments->a3;
	register uint64 r6 asm("x6") = 0;
	register uint64 r7 asm("x7") = 0;
	while (true) {
		asm volatile("smc #0"
			: "+r" (r0), "+r" (r1), "+r" (r2), "+r" (r3), "+r" (r4),
				"+r" (r5), "+r" (r6), "+r" (r7)
			:
			: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
				"x16", "x17", "memory");
		if (r0 != SCM_INTERRUPTED)
			break;
		// resume the interrupted call
		arguments->interrupted++;
		r1 = arguments->info;
		r2 = arguments->a0;
		r3 = arguments->a1;
		r4 = arguments->a2;
		r5 = arguments->a3;
		r6 = r7 = 0;
	}
	arguments->r0 = r0;
	arguments->r1 = r1;
}


int64
qcom_scm_call(uint32 service, uint32 command, uint32 argumentInfo, uint64 a0,
	uint64 a1, uint64 a2, uint64 a3, uint64* result)
{
	scm_arguments arguments = {};
	arguments.function = 1ull << 30 | 2ull << 24 | service << 8 | command;
	arguments.info = argumentInfo;
	arguments.a0 = a0;
	arguments.a1 = a1;
	arguments.a2 = a2;
	arguments.a3 = a3;
	for (int attempt = 0; attempt < 100; attempt++) {
		call_single_cpu_sync(0, scm_smc, &arguments);
		if (arguments.r0 != SCM_V2_EBUSY)
			break;
		snooze(30000);
	}
	if (result != NULL)
		*result = arguments.r1;
	return (int64)arguments.r0;
}


status_t
qcom_pas_load(const char* name, uint32 pasId, const uint8* image, size_t size,
	phys_addr_t regionBase, size_t regionSize)
{
	const elf32_header* header = (const elf32_header*)image;
	const elf32_program_header* phdrs
		= (const elf32_program_header*)(image + header->phoff);
	if (size < sizeof(elf32_header) || memcmp(header->ident, "\x7f" "ELF", 4)
			!= 0 || header->phnum < 2
		|| header->phoff + header->phnum * sizeof(*phdrs) > size) {
		return B_BAD_DATA;
	}

	uint32 hash = 0;
	uint32 minAddress = 0xffffffff, maxAddress = 0;
	bool relocatable = false;
	for (uint32 i = 0; i < header->phnum; i++) {
		const elf32_program_header& phdr = phdrs[i];
		if (hash == 0 && i > 0 && (phdr.flags & MDT_TYPE_MASK) == MDT_TYPE_HASH)
			hash = i;
		if (phdr.type != ELF_PT_LOAD || phdr.memsz == 0
			|| (phdr.flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
			continue;
		}
		relocatable |= (phdr.flags & MDT_RELOCATABLE) != 0;
		minAddress = min_c(minAddress, phdr.paddr);
		maxAddress = max_c(maxAddress,
			(phdr.paddr + phdr.memsz + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1));
	}
	if (hash == 0 || phdrs[hash].offset + phdrs[hash].filesz > size)
		return B_BAD_DATA;

	// the metadata: physically contiguous, page aligned, cleaned to memory
	size_t metadataSize = phdrs[0].filesz + phdrs[hash].filesz;
	uint8* metadata;
	area_id metadataArea = create_area("qcom pas metadata", (void**)&metadata,
		B_ANY_KERNEL_ADDRESS, (metadataSize + B_PAGE_SIZE - 1)
			& ~(B_PAGE_SIZE - 1), B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA);
	if (metadataArea < 0)
		return metadataArea;
	physical_entry entry;
	get_memory_map(metadata, B_PAGE_SIZE, &entry, 1);
	memcpy(metadata, image, phdrs[0].filesz);
	memcpy(metadata + phdrs[0].filesz, image + phdrs[hash].offset,
		phdrs[hash].filesz);
	for (addr_t line = (addr_t)metadata; line < (addr_t)metadata + metadataSize;
			line += 64)
		asm volatile("dc civac, %0" : : "r" (line) : "memory");
	memory_full_barrier();

	INFO("%s: %" B_PRIu32 " segments, %s, %#" B_PRIx32 "-%#" B_PRIx32
		", region %#" B_PRIxPHYSADDR " (%#zx)\n", name, header->phnum,
		relocatable ? "relocatable" : "fixed", minAddress, maxAddress,
		regionBase, regionSize);

	int64 result = qcom_scm_call(QCOM_SCM_SVC_PIL, SCM_PAS_INIT_IMAGE,
		QCOM_SCM_ARGS(2) | QCOM_SCM_ARG(0, QCOM_SCM_ARG_VAL)
			| QCOM_SCM_ARG(1, QCOM_SCM_ARG_RW), pasId, entry.address);
	delete_area(metadataArea);
	if (result != 0) {
		ERROR("%s: PAS init image failed: %" B_PRId64 "\n", name, result);
		return B_ERROR;
	}
	if (relocatable) {
		result = qcom_scm_call(QCOM_SCM_SVC_PIL, SCM_PAS_MEM_SETUP,
			QCOM_SCM_ARGS(3), pasId, regionBase, maxAddress - minAddress);
		if (result != 0) {
			ERROR("%s: PAS memory setup failed: %" B_PRId64 "\n", name,
				result);
			return B_ERROR;
		}
	}

	volatile uint8* target;
	area_id targetArea = map_physical_memory("qcom pas region", regionBase,
		regionSize, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&target);
	if (targetArea < 0)
		return targetArea;
	uint32 base = relocatable ? minAddress : (uint32)regionBase;
	status_t status = B_OK;
	for (uint32 i = 0; i < header->phnum && status == B_OK; i++) {
		const elf32_program_header& phdr = phdrs[i];
		if (phdr.type != ELF_PT_LOAD || phdr.memsz == 0
			|| (phdr.flags & MDT_TYPE_MASK) == MDT_TYPE_HASH) {
			continue;
		}
		uint32 offset = phdr.paddr - base;
		if (phdr.paddr < base || offset + phdr.memsz > regionSize
			|| phdr.filesz > phdr.memsz || phdr.offset + phdr.filesz > size) {
			ERROR("%s: segment %" B_PRIu32 " does not fit\n", name, i);
			status = B_BAD_DATA;
			break;
		}
		// word by word: the region is device memory
		for (uint32 b = 0; b < phdr.memsz; b += 4) {
			uint32 word = 0;
			if (b < phdr.filesz)
				memcpy(&word, image + phdr.offset + b, min_c(4, phdr.filesz - b));
			*(volatile uint32*)(target + offset + b) = word;
		}
	}
	memory_full_barrier();
	delete_area(targetArea);
	return status;
}


status_t
qcom_pas_auth_and_reset(const char* name, uint32 pasId)
{
	int64 result = qcom_scm_call(QCOM_SCM_SVC_PIL, SCM_PAS_AUTH_AND_RESET,
		QCOM_SCM_ARGS(1), pasId);
	if (result != 0) {
		ERROR("%s: PAS authentication failed: %" B_PRId64 "\n", name, result);
		return B_ERROR;
	}
	return B_OK;
}


status_t
qcom_pas_shutdown(uint32 pasId)
{
	return qcom_scm_call(QCOM_SCM_SVC_PIL, SCM_PAS_SHUTDOWN, QCOM_SCM_ARGS(1),
		pasId) == 0 ? B_OK : B_ERROR;
}
