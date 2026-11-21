/*
 * Copyright 2026, the air/OS project.
 * Distributed under the terms of the MIT License.
 *
 * nvmm_hva_map_existing(): a guest maps memory its host process already has
 * (part of a fully locked area, here shared with a clone of it), reads what
 * the host wrote there and the host sees what the guest writes; unmapping
 * takes it from the guest and leaves the area alone.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <string.h>
#include <err.h>
#include <errno.h>

#include <OS.h>

#include <nvmm.h>

#include "h_os.h"

#define GPA_CODE	0x2000
#define GPA_SHARED	0x8000
#define AREA_PAGES	4
#define MAP_PAGES	2
#define MANY		200
	/* the area's pages 1 and 2 are the guest's 0x8000 and 0x9000 */

static const uint8_t kCode[] = {
	0x48, 0x8b, 0x04, 0x25, 0x00, 0x80, 0x00, 0x00,	/* mov 0x8000,%rax */
	0x48, 0x83, 0xc0, 0x01,				/* add $1,%rax */
	0x48, 0x89, 0x04, 0x25, 0x08, 0x80, 0x00, 0x00,	/* mov %rax,0x8008 */
	0x48, 0x8b, 0x04, 0x25, 0x00, 0x90, 0x00, 0x00,	/* mov 0x9000,%rax */
	0x48, 0x83, 0xc0, 0x01,				/* add $1,%rax */
	0x48, 0x89, 0x04, 0x25, 0x08, 0x90, 0x00, 0x00,	/* mov %rax,0x9008 */
	0x48, 0xc7, 0xc1, 0xff, 0xff, 0xff, 0xff,	/* mov $-1,%rcx */
	0x0f, 0x32					/* rdmsr: stop */
};

static int nfail;

static void
check(bool ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok" : "*** FAILED", what);
	if (!ok)
		nfail++;
}

static void *
map_page(struct nvmm_machine *mach, uint64_t gpa, int prot)
{
	void *page = mmap(NULL, PAGE_SIZE, PROT_READ|PROT_WRITE,
	    MAP_ANON|MAP_PRIVATE, -1, 0);
	if (page == MAP_FAILED)
		err(errno, "mmap");
	if (nvmm_hva_map(mach, (uintptr_t)page, PAGE_SIZE) == -1)
		err(errno, "nvmm_hva_map");
	if (nvmm_gpa_map(mach, (uintptr_t)page, gpa, PAGE_SIZE, prot) == -1)
		err(errno, "nvmm_gpa_map");
	memset(page, 0, PAGE_SIZE);
	return page;
}

static void
init_seg(struct nvmm_x64_state_seg *seg, int type, int sel)
{
	seg->selector = sel;
	seg->attrib.type = type;
	seg->attrib.s = (type & 0b10000) != 0;
	seg->attrib.dpl = 0;
	seg->attrib.p = 1;
	seg->attrib.avl = 1;
	seg->attrib.l = 1;
	seg->attrib.def = 0;
	seg->attrib.g = 1;
	seg->limit = 0x0000FFFF;
	seg->base = 0x00000000;
}

static void
reset_machine64(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
	struct nvmm_x64_state *state = vcpu->state;

	if (nvmm_vcpu_getstate(mach, vcpu, NVMM_X64_STATE_ALL) == -1)
		err(errno, "nvmm_vcpu_getstate");

	memset(state, 0, sizeof(*state));

	state->gprs[NVMM_X64_GPR_RFLAGS] = PSL_MBO;
	init_seg(&state->segs[NVMM_X64_SEG_CS], SDT_MEMERA, GSEL(GCODE_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_SS], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_DS], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_ES], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_FS], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_GS], SDT_MEMRWA, GSEL(GDATA_SEL, SEL_KPL));
	init_seg(&state->segs[NVMM_X64_SEG_GDT], 0, 0);
	init_seg(&state->segs[NVMM_X64_SEG_IDT], 0, 0);
	init_seg(&state->segs[NVMM_X64_SEG_LDT], SDT_SYSLDT, 0);
	init_seg(&state->segs[NVMM_X64_SEG_TR], SDT_SYS386BSY, 0);

	state->crs[NVMM_X64_CR_CR0] = CR0_PG|CR0_PE|CR0_NE|CR0_TS|CR0_MP|CR0_WP|CR0_AM;
	state->crs[NVMM_X64_CR_CR4] = CR4_PAE;
	state->msrs[NVMM_X64_MSR_EFER] = EFER_LME | EFER_SCE | EFER_LMA;
	state->msrs[NVMM_X64_MSR_PAT] = MSR_PAT_VALUE;
	state->crs[NVMM_X64_CR_CR3] = 0x3000;
	state->gprs[NVMM_X64_GPR_RIP] = GPA_CODE;

	if (nvmm_vcpu_setstate(mach, vcpu, NVMM_X64_STATE_ALL) == -1)
		err(errno, "nvmm_vcpu_setstate");
}

/* Runs the guest code; the exit it stopped with. */
static uint64_t
run(struct nvmm_machine *mach, struct nvmm_vcpu *vcpu)
{
	reset_machine64(mach, vcpu);
	while (1) {
		if (nvmm_vcpu_run(mach, vcpu) == -1)
			err(errno, "nvmm_vcpu_run");
		if (vcpu->exit->reason != NVMM_VCPU_EXIT_NONE)
			return vcpu->exit->reason;
	}
}

int
main(void)
{
	struct nvmm_machine mach;
	struct nvmm_vcpu vcpu;
	uint64_t *L4, *L3, *L2, *L1;
	uint8_t *code;
	uint8_t *base = NULL, *other = NULL, *unlocked = NULL, *readonly = NULL;
	uint64_t *page1, *page2;
	area_id area, clone, a;
	uint64_t reason;
	int ret;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (nvmm_init() == -1)
		err(errno, "nvmm_init");
	if (nvmm_machine_create(&mach) == -1)
		err(errno, "nvmm_machine_create");
	if (nvmm_vcpu_create(&mach, 0, &vcpu) == -1)
		err(errno, "nvmm_vcpu_create");

	code = map_page(&mach, GPA_CODE, PROT_READ|PROT_EXEC);
	memcpy(code, kCode, sizeof(kCode));
	L4 = map_page(&mach, 0x3000, PROT_READ|PROT_WRITE);
	L3 = map_page(&mach, 0x4000, PROT_READ|PROT_WRITE);
	L2 = map_page(&mach, 0x5000, PROT_READ|PROT_WRITE);
	L1 = map_page(&mach, 0x6000, PROT_READ|PROT_WRITE);
	L4[0] = PTE_P | PTE_W | 0x4000;
	L3[0] = PTE_P | PTE_W | 0x5000;
	L2[0] = PTE_P | PTE_W | 0x6000;
	L1[GPA_CODE / PAGE_SIZE] = PTE_P | PTE_W | GPA_CODE;
	L1[GPA_SHARED / PAGE_SIZE] = PTE_P | PTE_W | GPA_SHARED;
	L1[GPA_SHARED / PAGE_SIZE + 1] = PTE_P | PTE_W | (GPA_SHARED + PAGE_SIZE);

	/* what does not qualify */
	a = create_area("unlocked", (void **)&unlocked, B_ANY_ADDRESS, PAGE_SIZE,
	    B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	ret = nvmm_hva_map_existing(&mach, (uintptr_t)unlocked, PAGE_SIZE);
	check(ret == -1 && errno == EINVAL, "an area that is not locked is refused");
	delete_area(a);
	a = create_area("read only", (void **)&readonly, B_ANY_ADDRESS, PAGE_SIZE,
	    B_FULL_LOCK, B_READ_AREA);
	ret = nvmm_hva_map_existing(&mach, (uintptr_t)readonly, PAGE_SIZE);
	check(ret == -1 && errno == EINVAL, "a read-only area is refused");
	delete_area(a);

	area = create_area("nvmm shared", (void **)&base, B_ANY_ADDRESS,
	    AREA_PAGES * PAGE_SIZE, B_FULL_LOCK,
	    B_READ_AREA | B_WRITE_AREA | B_CLONEABLE_AREA);
	if (area < 0)
		errx(1, "create_area: %s", strerror(area));
	memset(base, 0xa5, AREA_PAGES * PAGE_SIZE);
	page1 = (uint64_t *)(base + PAGE_SIZE);
	page2 = (uint64_t *)(base + 2 * PAGE_SIZE);

	ret = nvmm_hva_map_existing(&mach, (uintptr_t)base + PAGE_SIZE,
	    AREA_PAGES * PAGE_SIZE);
	check(ret == -1 && errno == EINVAL, "a range past the area's end is refused");
	/* (libnvmm aborts when the kernel refuses a nvmm_gpa_map(), as it does
	   for memory that was not registered: not tried here) */

	/* the middle of the area, into the guest */
	ret = nvmm_hva_map_existing(&mach, (uintptr_t)page1,
	    MAP_PAGES * PAGE_SIZE);
	check(ret == 0, "nvmm_hva_map_existing");
	check(page1[0] == 0xa5a5a5a5a5a5a5a5ULL, "the area keeps its contents");
	ret = nvmm_gpa_map(&mach, (uintptr_t)page1, GPA_SHARED,
	    MAP_PAGES * PAGE_SIZE, PROT_READ|PROT_WRITE);
	check(ret == 0, "nvmm_gpa_map of it");

	/* the other process' view: a clone made after the guest mapped it */
	clone = clone_area("nvmm shared clone", (void **)&other, B_ANY_ADDRESS,
	    B_READ_AREA | B_WRITE_AREA, area);
	if (clone < 0)
		errx(1, "clone_area: %s", strerror(clone));

	page1[0] = 0x1111;
	((uint64_t *)(other + 2 * PAGE_SIZE))[0] = 0x2222;
	reason = run(&mach, &vcpu);
	check(reason == NVMM_VCPU_EXIT_RDMSR, "the guest ran to its end");
	check(page1[1] == 0x1112, "the guest read and wrote the first page");
	check(page2[1] == 0x2223, "the guest read the clone's write on the second");
	check(((uint64_t *)(other + PAGE_SIZE))[1] == 0x1112,
	    "the clone sees the guest's write");
	check(base[0] == 0xa5 && base[PAGE_SIZE - 1] == 0xa5
	    && base[3 * PAGE_SIZE] == 0xa5, "the pages around are untouched");

	/* again, with new values (the pages are mapped now) */
	page1[0] = 0x7000;
	page2[0] = 0x8000;
	reason = run(&mach, &vcpu);
	check(reason == NVMM_VCPU_EXIT_RDMSR && page1[1] == 0x7001
	    && page2[1] == 0x8001, "a second run sees new values");

	/* unmapping takes it from the guest, not from us */
	ret = nvmm_hva_unmap(&mach, (uintptr_t)page1, MAP_PAGES * PAGE_SIZE);
	check(ret == 0, "nvmm_hva_unmap");
	page1[0] = 0x3333;
	page1[1] = 0;
	reason = run(&mach, &vcpu);
	check(reason == NVMM_VCPU_EXIT_MEMORY && vcpu.exit->u.mem.gpa == GPA_SHARED,
	    "the guest no longer has the memory");
	check(page1[0] == 0x3333 && page1[1] == 0 && page2[0] == 0x8000,
	    "the area is still ours, unchanged");
	memset(base, 0x5a, AREA_PAGES * PAGE_SIZE);
	check(other[2 * PAGE_SIZE + 5] == 0x5a, "and still shared with the clone");

	/* and back (libnvmm still lists the old mapping), then everything goes
	   while it is mapped */
	ret = nvmm_gpa_unmap(&mach, (uintptr_t)page1, GPA_SHARED,
	    MAP_PAGES * PAGE_SIZE);
	check(ret == 0, "nvmm_gpa_unmap of what is no longer there");
	ret = nvmm_hva_map_existing(&mach, (uintptr_t)page1,
	    MAP_PAGES * PAGE_SIZE);
	if (ret == 0) {
		ret = nvmm_gpa_map(&mach, (uintptr_t)page1, GPA_SHARED,
		    MAP_PAGES * PAGE_SIZE, PROT_READ|PROT_WRITE);
	}
	page1[0] = 0x4444;
	page2[0] = 0x5555;
	reason = run(&mach, &vcpu);
	check(ret == 0 && reason == NVMM_VCPU_EXIT_RDMSR && page1[1] == 0x4445
	    && page2[1] == 0x5556, "mapped again, the guest has it again");

	/* many small pieces (a renderer's allocations): more than the 32 host
	   mappings NVMM has elsewhere */
	{
		uint8_t *many = NULL;
		int i, mapped = 0, unmapped = 0;
		area_id manyArea = create_area("nvmm many", (void **)&many,
		    B_ANY_ADDRESS, MANY * PAGE_SIZE, B_FULL_LOCK,
		    B_READ_AREA | B_WRITE_AREA);
		if (manyArea < 0)
			errx(1, "create_area: %s", strerror(manyArea));
		for (i = 0; i < MANY; i++) {
			if (nvmm_hva_map_existing(&mach,
			    (uintptr_t)many + i * PAGE_SIZE, PAGE_SIZE) == 0)
				mapped++;
		}
		check(mapped == MANY, "200 separate pieces can be registered");
		ret = nvmm_hva_map_existing(&mach, (uintptr_t)many + PAGE_SIZE,
		    PAGE_SIZE);
		check(ret == -1, "but none of them twice");
		for (i = 0; i < MANY; i++) {
			if (nvmm_hva_unmap(&mach, (uintptr_t)many + i * PAGE_SIZE,
			    PAGE_SIZE) == 0)
				unmapped++;
		}
		check(unmapped == MANY, "and released");
		delete_area(manyArea);
	}

	delete_area(area);
	check(((uint64_t *)(other + PAGE_SIZE))[1] == 0x4445,
	    "the clone outlives the area");
	if (nvmm_vcpu_destroy(&mach, &vcpu) == -1)
		err(errno, "nvmm_vcpu_destroy");
	if (nvmm_machine_destroy(&mach) == -1)
		err(errno, "nvmm_machine_destroy");
	check(((uint64_t *)(other + 2 * PAGE_SIZE))[1] == 0x5556,
	    "and the machine");
	delete_area(clone);

	printf("%s\n", nfail == 0 ? "All tests passed" : "TESTS FAILED");
	return nfail != 0;
}
