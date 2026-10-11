/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2026, air/OS.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// Polaris10 CP setup and golden values: Linux 6.18.52 gfx_v8_0.c (MIT).
#include "Gfx.h"
#include "GpuPageTable.h"
#include "PolarisTiling.h"
#include "Smu.h"
#include <KernelExport.h>

enum { SECT_NONE, SECT_CONTEXT };
struct cs_extent_def { const unsigned int* extent; uint32 reg_index, reg_count; };
struct cs_section_def { const cs_extent_def* section; uint32 id; };
#include "ClearState.h"
#include "GfxDraw.h"

static const uint32 kGolden[][3] = {
	{0xcd4, 0x000c0fc0, 0x000c0200}, // ATC_MISC_CG
	{0x2684, 0x0001f3cf, 0x00007208}, // CB_HW_CONTROL
	{0x2686, 0x0f000000, 0x0f000000}, // CB_HW_CONTROL_2
	{0x2683, 0x000001ff, 0x00000040}, // CB_HW_CONTROL_3
	{0x260d, 0xf00fffff, 0x00000400}, // DB_DEBUG2
	{0x22fc, 0xffffffff, 0x20000001}, // PA_SC_ENHANCE
	{0xc281, 0x0000ff0f, 0x00000000}, // PA_SC_LINE_STIPPLE_STATE
	{0xa0d4, 0x3f3fffff, 0x16000012}, // PA_SC_RASTER_CONFIG
	{0xa0d5, 0x0000003f, 0x0000002a}, // PA_SC_RASTER_CONFIG_1
	{0xec49, 0x00000003, 0x0001003c}, // RLC_CGCG_CGLS_CTRL
	{0xec9d, 0xffffffff, 0x0001003c}, // RLC_CGCG_CGLS_CTRL_3D
	{0x2300, 0x07f80000, 0x07180000}, // SQ_CONFIG
	{0x2542, 0x000f000f, 0x000b0000}, // TA_CNTL_AUX
	{0x2b80, 0x00100000, 0xf31fff7f}, // TCC_CTRL
	{0x2b05, 0x000003ff, 0x000000f7}, // TCP_ADDR_CONFIG
	{0x2b04, 0xffffffff, 0x00000000}, // TCP_CHAN_STEER_HI
	{0x2232, 0x00000004, 0x00000004}, // VGT_RESET_DEBUG
	{0xc200, 0xffffffff, 0xe0000000}, // GRBM_GFX_INDEX
	{0xa0d4, 0xffffffff, 0x16000012}, // PA_SC_RASTER_CONFIG
	{0xa0d5, 0xffffffff, 0x0000002A}, // PA_SC_RASTER_CONFIG_1
	{0x263e, 0xffffffff, 0x22011003}, // GB_ADDR_CONFIG
	{0x31dc, 0xffffffff, 0x00000800}, // SPI_RESOURCE_RESERVE_CU_0
	{0x31dd, 0xffffffff, 0x00000800}, // SPI_RESOURCE_RESERVE_CU_1
	{0x31e6, 0xffffffff, 0x00FF7FBF}, // SPI_RESOURCE_RESERVE_EN_CU_0
	{0x31e7, 0xffffffff, 0x00FF7FAF}, // SPI_RESOURCE_RESERVE_EN_CU_1
};
// Linux 6.18.52 gmc_v8_0.c, golden_settings_polaris10_a11.
// Disable unused partial-residency apertures and use the Polaris read weight.
static const uint32 kGMCGolden[][3] = {
	{0x9e1, 0x00000003, 0}, // MC_ARB_WTM_GRPWT_RD
	{0x52c, 0x0fffffff, 0x0fffffff}, // VM_PRT_APERTURE0..3_LOW_ADDR
	{0x52d, 0x0fffffff, 0x0fffffff},
	{0x52e, 0x0fffffff, 0x0fffffff},
	{0x52f, 0x0fffffff, 0x0fffffff},
};
static const uint32 kHalt = 0x15000000;
static const uint64 kCommandVA = 0x10000;
static const uint64 kMemoryVA = 0x100000;
// Last command-RAM page is reserved for snooped completion and control data.
// Keep it outside the ring, bulk IB, and minimal IB ranges.
static const uint32 kControlOffset = Gart::kCommandBytes - 4096;
static const uint64 kControlGPU = Gart::kBase + kControlOffset;
static const uint64 kControlVA = kCommandVA + kControlOffset - 65536;
// Match Linux gfx_v8_0: 1024 DWORDs per job, two hardware submissions.
static const uint32 kRingDwords = 2048;
static const uint32 kRingMask = kRingDwords - 1;
static const uint32 kDirectSequences = 16;
static const uint32 kShaderSequences = 16;
static const uint32 kDrawSequences = 4;
static const uint32 kVMShaderSequences = 8;
// A long DE IB separates the entry gate from its tail. Inspect the remaining
// fetch count at that gate instead of assuming the tail is still unfetched.
// Stop before the separate CE IB at
// command VA + 0xc000; the control page is further away at VA + 0xf000.
static const uint32 kShaderIBDwords = 0xc000 / 4;
// gfx803, assembled with LLVM 18. s[0:1] is the output address, s2 the seed,
// s3 the workgroup X ID, and v0 the local thread X ID. No scratch or LDS.
static const uint32 kFillShader[] = {
	0xd1c30000, 0x04018003, // v_mad_u32_u24 v0, s3, 64, v0
	0x24040082,             // v_lshlrev_b32 v2, 2, v0
	0x32040400,             // v_add_u32 v2, vcc, s0, v2
	0x7e060201,             // v_mov_b32 v3, s1
	0xd11c6a03, 0x01a90103, // v_addc_u32 v3, vcc, v3, 0, vcc
	0xbe8400ff, 0x10204081, // s_mov_b32 s4, 0x10204081
	0xd2850004, 0x00000900, // v_mul_lo_u32 v4, v0, s4
	0x2a080802,             // v_xor_b32 v4, s2, v4
	0xdc710000, 0x00000402, // flat_store_dword v[2:3], v4 glc
	0xbf8c0f70,             // s_waitcnt vmcnt(0)
	0xbf810000,             // s_endpgm
};
static uint32 Packet(uint32 op, uint32 count) { return 0xc0000000 | count << 16 | op << 8; }

status_t
GfxEngine::InitializeVM(const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation, const Gart& gart)
{
	// VMID1 is a private diagnostic address space. Only the owned command
	// RAM and GFX scratch are mapped; it cannot reach client BOs or scanout.
	vmArea = -1;
	const uint64 offset = 24ULL << 20;
	if ((regs[0x505] & 1) != 0
		|| !amdgpu_vram_range_is_safe(regs, info, reservation, offset,
			GpuPageTable::kDirectoryBytes + 4096))
		return B_NOT_ALLOWED;
	volatile uint64* directory;
	vmArea = map_physical_memory("amdgpu private GFX VM", info.bar_address[0] + offset,
		GpuPageTable::kDirectoryBytes + 4096, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&directory);
	if (vmArea < 0)
		return vmArea;
	for (uint32 i = 0; i < (GpuPageTable::kDirectoryBytes + 4096) / 8; i++)
		directory[i] = 0;
	volatile uint64* ptes = directory + GpuPageTable::kDirectoryEntries;
	directory[0] = (info.vram_gpu_base + offset + GpuPageTable::kDirectoryBytes) | 1;
	for (uint32 i = 0; i < 16; i++)
		ptes[kCommandVA / 4096 + i] = gart.table[16 + i];
	for (uint32 i = 0; i < 256; i++)
		ptes[kMemoryVA / 4096 + i] = (gpu + i * 4096) | 0x71;
	__sync_synchronize();
	(void)directory[0];
	regs[0x1520] = 1;
	(void)regs[0x1520];
	regs[0x575] = 0;
	regs[0x576] = 0;
	regs[0x577] = 0;
	regs[0x558] = 0;
	regs[0x560] = (GpuPageTable::kSize >> 12) - 1;
	regs[0x550] = (info.vram_gpu_base + offset) >> 12;
	regs[0x547] = regs[0x546];
	regs[0x50d] = 0; // retain the first fault for the IRQ and submitting thread
	regs[0x505] = regs[0x504] | 2; // two levels, 512 PTEs per page
	vmEnabled = true;
	regs[0x51e] = 2;
	bigtime_t deadline = system_time() + 100000;
	while ((regs[0x51f] & 2) == 0) {
		if (system_time() >= deadline)
			return B_TIMED_OUT;
		snooze(10);
	}
	uint32 select = regs[0x391];
	regs[0x391] = 1 << 4;
	regs[0x230d] = 1 << 5 | 3 << 8 | 3 << 3; // NC default, UC APE1, unaligned
	// Linux VI graphics VMs put LDS at 0x2000000000000000. Keep the
	// low GPU virtual addresses used by flat stores outside that aperture.
	regs[0x230a] = 0x2000;
	regs[0x230b] = 1;
	regs[0x230c] = 0;
	regs[0x391] = select;
	return B_OK;
}

void
GfxEngine::DumpExecutionState(const char* point)
{
	// Defined GFX8 address/state registers only; no indexed read ports.
	const uint32 registers[] = {
		0x3038, 0x30b9, 0x30ba, 0x30bb, 0x21b9,
		0x3060, 0x30b2, 0x3061, 0x3062, 0x3063, 0x3064,
		0x3065, 0x30b3, 0x3066, 0x3067, 0x3068, 0x3069,
		0x219c, 0x219d, 0x219e, 0x219f, 0x21a0, 0x21a1, 0x21a2, 0x21a3,
		0x21a4,
		0xc069, 0xc06a, 0xc06d, 0xc06e, 0xc078, 0xc080, 0xc081,
		0xc082, 0xc083, 0xc084, 0xc077, 0xc085, 0xc086, 0xc087,
		0xc088, 0xc089, 0xc08a, 0xc0f0, 0xc0f1, 0xc0f2, 0xc0f3,
		0xc0f4, 0xc0f5, 0xc0f6, 0xc0f7, 0xc0f8, 0xc0f9, 0xc0fb, 0xc0fc,
		0xec1d, 0xec1e, 0xec43, 0xec80,
		0x80a, 0x80b, 0x80c, 0x80f, 0x9e1,
		0x52c, 0x52d, 0x52e, 0x52f, 0x530, 0x531, 0x532, 0x533, 0x534,
		0xd808, 0xdc80, 0xdc81, 0xdc82, 0xdc83, 0xdc84, 0xec71,
		0x3052, 0x3053,
		// CE's unused init and second-IB state were absent from earlier
		// snapshots. Capture every defined base/size at both IB gates.
		0xc098, 0xc099, 0xc09a,
		0xc0c3, 0xc0c4, 0xc0c5, 0xc0c6, 0xc0c7, 0xc0c8,
		0xc0c9, 0xc0ca, 0xc0cb,
		0xc094, 0xc095, 0xc096, 0xc097, // DE preamble bounds
		0x21bc, 0x21bd, 0x21d5, 0x21d6, // ROQ/MEQ thresholds
	};
	for (uint32 index : registers)
		dprintf("amdgpu: GFX %s register %#x = %#x\n", point,
			(unsigned)index, (unsigned)regs[index]);
}

void
GfxEngine::Snapshot(amdgpu_gfx_test& result)
{
	result.cp_control = regs[0x21b6];
	result.ring_control = regs[0x3041];
	result.rptr = regs[0x21c0];
	result.wptr = regs[0x3045];
	result.grbm_status = regs[0x2004];
	result.rlc_status = regs[0xec40];
	result.vm_fault_status = regs[0x536];
	result.vm_fault_address = regs[0x53e];
	result.vm_fault_client = regs[0x538];
	if ((result.vm_fault_status & 0xff) == 0 && (regs[0x537] & 0xff) != 0) {
		result.vm_fault_status = regs[0x537];
		result.vm_fault_address = regs[0x53f];
		result.vm_fault_client = regs[0x539];
	}
	dprintf("amdgpu: GFX snapshot stage %u VM %#x page %#x client %#x\n",
		(unsigned)result.stage, (unsigned)result.vm_fault_status,
		(unsigned)result.vm_fault_address, (unsigned)result.vm_fault_client);
	if (result.stage == 4) {
		const uint32 indexes[] = {0xc08d, 0xc08e, 0x30ad, 0xc092, 0xc093, 0xc0cc, 0xc0cd,
			0xc0ce, 0xc0cf, 0xc0d0, 0xc0d1, 0xc0c3, 0xc0c4, 0xc0c5,
			0xc0c6, 0xc0c7, 0xc0c8, 0xc0c9, 0xc0ca, 0xc0cb,
			0xc0d2, 0xc0d3, 0xeca2, 0xeca3, 0xeca4};
		for (uint32 index : indexes)
			dprintf("amdgpu: GFX IB register %#x = %#x\n", (unsigned)index, (unsigned)regs[index]);
	}
}

status_t
GfxEngine::Initialize(volatile uint32* r, const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation,
	const amdgpu::FirmwareView firmware[4], SdmaEngine& sdma, Gart& gart,
	amdgpu_gfx_test& result, const amdgpu::MecFirmwareView* mec)
{
	regs = r;
	volatile uint32* control = gart.commandMemory + kControlOffset / 4;
	Snapshot(result);
	result.stage = 1;
	if (faulted || (attempted && !ready))
		return B_DEV_NOT_READY;
	if (ready && mecStarted != (mec != NULL))
		return B_NOT_ALLOWED;
	if (!ready) {
		// BIOS leaves a placeholder base, even with RB_BUFSZ=0. Ownership
		// requires all three processors halted and an empty unconfigured ring.
		dprintf("amdgpu: GFX preflight CP %#x ring %#x base %#x/%#x ptr %u/%u\n",
			(unsigned)r[0x21b6], (unsigned)r[0x3041], (unsigned)r[0x30b1],
			(unsigned)r[0x3040], (unsigned)r[0x21c0], (unsigned)r[0x3045]);
		if (!amdgpu_smc_ready(r) || (r[0x2004] & 0x80000000) != 0
			|| (r[0x21b6] & kHalt) != kHalt || (r[0x3041] & 0x3f) != 0
			|| r[0x21c0] != 0 || r[0x3045] != 0
			|| !amdgpu_vram_range_is_safe(r, info, reservation, kScratchOffset, 1ULL << 20))
			return B_NOT_ALLOWED;
		if (mec != NULL) {
			// Do not take over a firmware/foreign compute queue. No HQD or
			// queue address is programmed by this diagnostic; MEC stays idle.
			if ((r[0x208d] & 0x50000000) != 0x50000000)
				return B_NOT_ALLOWED;
			uint32 select = r[0x391];
			bool inactive = true;
			for (uint32 me = 1; me <= 2; me++) {
				for (uint32 pipe = 0; pipe < 4; pipe++) {
					for (uint32 queue = 0; queue < 8; queue++) {
						r[0x391] = me << 2 | pipe | queue << 8;
						if ((r[0x3247] & 1) != 0)
							inactive = false;
					}
				}
			}
			r[0x391] = select;
			(void)r[0x391];
			if (!inactive)
				return B_NOT_ALLOWED;
			dprintf("amdgpu: GFX MEC preflight: both halted, 64 HQDs inactive\n");
		}
		area = map_physical_memory("amdgpu GFX kernel ring", info.bar_address[0] + (kScratchOffset),
			1 << 20, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			(void**)&memory);
		if (area < 0)
			return area;
		attempted = true;
		gpu = info.vram_gpu_base + (kScratchOffset);
		ring = gart.commandMemory;
		status_t status = InitializeVM(info, reservation, gart);
		if (status != B_OK) {
			faulted = true;
			return status;
		}
		wptr = sequence = 0;
		for (uint32 i = 0; i < 4096 / 4; i++)
			control[i] = 0;
		for (uint32 i = 0; i < (1 << 20) / 4; i++)
			memory[i] = 0;
		for (uint32 i = 0; i < sizeof(kFillShader) / sizeof(uint32); i++)
			memory[0x40000 / 4 + i] = kFillShader[i];
		for (uint32 i = 0; i < sizeof(kTriangleVS) / sizeof(uint32); i++)
			memory[0x41000 / 4 + i] = kTriangleVS[i];
		for (uint32 i = 0; i < sizeof(kColorPS) / sizeof(uint32); i++)
			memory[0x42000 / 4 + i] = kColorPS[i];
		for (uint32 i = 0; i < kRingDwords; i++)
			ring[i] = (i & 1) == 0 ? Packet(0x10, 0) : 0;
		result.stage = 2;
		Snapshot(result);
		r[0x306a] = 0; // no interrupts until the IH path exists
		r[0x21b6] |= kHalt;
		(void)r[0x21b6];
		snooze(50);
		for (const auto& value : kGMCGolden) {
			uint32 before = r[value[0]];
			r[value[0]] = (before & ~value[1]) | value[2];
			dprintf("amdgpu: GMC golden %#x before %#x after %#x\n",
				(unsigned)value[0], (unsigned)before, (unsigned)r[value[0]]);
		}
		for (const auto& value : kGolden)
			r[value[0]] = (r[value[0]] & ~value[1]) | value[2];
		// Firmware leaves a display-oriented table that is not necessarily a
		// valid graphics layout. Match Polaris10's constants initialization
		// before CP starts; Mesa/addrlib must see the actual programmed table.
		// MACROTILE_MODE7 is reserved and intentionally not written by Linux.
		for (uint32 i = 0; i < 32; i++)
			r[0x2644 + i] = kPolaris10TileMode[i];
		for (uint32 i = 0; i < 16; i++) {
			if (i != 7)
				r[0x2664 + i] = kPolaris10MacrotileMode[i];
		}
		for (uint32 i = 0; i < 48; i++) {
			if (i == 39)
				continue;
			uint32 expected = i < 32 ? kPolaris10TileMode[i]
				: kPolaris10MacrotileMode[i - 32];
			uint32 actual = r[0x2644 + i];
			if (actual != expected) {
				dprintf("amdgpu: GFX tiling register %#x got %#x expected %#x\n",
					(unsigned)(0x2644 + i), (unsigned)actual, (unsigned)expected);
				faulted = true;
				Snapshot(result);
				return B_BAD_DATA;
			}
		}
		dprintf("amdgpu: GFX Polaris tiling initialized, 47 registers verified\n");
		r[0x2000] = (r[0x2000] & ~0xffu) | 0xff; // GRBM read timeout
		// The TC path used by indirect fetches needs the CP/shader memory
		// configuration from gfx_v8_0_constants_init, even before shaders.
		r[0x2581] = 1 | 1 << 1 | 3 << 3; // SH_STATIC_MEM_CONFIG
		uint32 select = r[0x391];
		r[0x391] = 0; // private VMID0
		r[0x230d] = 3 << 5 | 3 << 8 | 3 << 3; // UC types, unaligned access
		r[0x230a] = 0;
		r[0x230b] = 1;
		r[0x230c] = 0;
		r[0x391] = select;
		mecStarted = mec != NULL;
		if (mecStarted) {
			// No MEC interrupts until the IH path exists. All pipes/queues
			// are exclusively ours and inactive, as checked above.
			for (uint32 index = 0x3085; index <= 0x308c; index++)
				r[index] = 0;
		}
		status = amdgpu_smc_load_gfx(r, firmware,
			sdma.memory + (1 << 20) / 4, sdma.gpu + (1 << 20), mec);
		if (status != B_OK) {
			faulted = true;
			Halt();
			Snapshot(result);
			return status;
		}
		result.stage = 3;
		Snapshot(result);
		// Polaris10 does not use gfx_v8_0_init_pg's RLC save/restore setup.
		r[0xec00] &= ~1u;
		r[0x2008] |= 4;
		(void)r[0x2008];
		snooze(50);
		r[0x2008] &= ~4u;
		(void)r[0x2008];
		snooze(50);
		r[0xec00] |= 1;
		(void)r[0xec00];
		snooze(50);
		Snapshot(result);
		r[0x21c1] = 0;
		r[0x3051] = 0; // ring VMID0
		const uint32 control = 10 | 8 << 8 | 3 << 15 | 1 << 22;
		r[0x3041] = control | 0x80000000;
		r[0x3045] = 0;
		r[0x3043] = (uint32)(kControlGPU + 0x10);
		r[0x3044] = (kControlGPU + 0x10) >> 32;
		r[0x3046] = (uint32)(kControlGPU + 0x20);
		r[0x3047] = (kControlGPU + 0x20) >> 32;
		snooze(1000);
		r[0x3041] = control;
		r[0x3040] = Gart::kBase >> 8;
		r[0x30b1] = Gart::kBase >> 40;
		r[0x3059] &= ~0x10000000u;
		r[0x30ae] = 7;
		r[0x3050] = 0;
		r[0x304b] = 1;
		r[0x21b6] &= ~kHalt;
		(void)r[0x21b6];
		snooze(50);
		Snapshot(result);
		// Establish whether the loaded CP can execute a basic memory write
		// before the much longer clear-state stream. Use only the reserved
		// snooped control page; no shader/context state is needed here.
		volatile uint32* basicMarker = gart.commandMemory
			+ (kControlOffset + 0x504) / 4;
		ring[wptr++] = Packet(0x37, 3);
		ring[wptr++] = 5 << 8 | 1 << 20;
		ring[wptr++] = (uint32)(kControlGPU + 0x504);
		ring[wptr++] = (kControlGPU + 0x504) >> 32;
		ring[wptr++] = 0x43504231;
		ring[wptr++] = Packet(0x42, 0); // PFP_SYNC_ME
		ring[wptr++] = 0;
		ring[wptr++] = Packet(0x10, 247); // pad this first batch to 256 DWORDs
		while (wptr < 256)
			ring[wptr++] = 0;
		__sync_synchronize();
		r[0x1520] = 1;
		(void)r[0x1520];
		r[0x3045] = wptr;
		bigtime_t basicDeadline = system_time() + 500000;
		while ((*basicMarker != 0x43504231 || r[0x21c0] != wptr)
			&& system_time() < basicDeadline)
			snooze(50);
		dprintf("amdgpu: GFX basic startup marker %#x ring %u/%u VM %#x/%#x\n",
			(unsigned)*basicMarker, (unsigned)r[0x21c0], (unsigned)wptr,
			(unsigned)r[0x536], (unsigned)r[0x537]);
		if (*basicMarker != 0x43504231 || r[0x21c0] != wptr
			|| ((r[0x536] | r[0x537]) & 0xff) != 0) {
			DumpExecutionState("basic startup stalled");
			faulted = true;
			Halt();
			Snapshot(result);
			return B_DEV_NOT_READY;
		}
		// Match the VI CP startup sequence before submitting any IB: disable
		// inherited shadow loads, define the hardware's full clear state,
		// select the Polaris raster layout, and initialize CE partitions.
		ring[wptr++] = Packet(0x4a, 0);
		ring[wptr++] = 2u << 28;
		ring[wptr++] = Packet(0x28, 1);
		ring[wptr++] = 0x80000000;
		ring[wptr++] = 0x80000000;
		for (const cs_extent_def* ext = vi_SECT_CONTEXT_defs; ext->extent != NULL; ext++) {
			ring[wptr++] = Packet(0x69, ext->reg_count);
			ring[wptr++] = ext->reg_index - 0xa000;
			for (uint32 i = 0; i < ext->reg_count; i++)
				ring[wptr++] = ext->extent[i];
		}
		ring[wptr++] = Packet(0x69, 2);
		ring[wptr++] = 0xd4;
		ring[wptr++] = 0x16000012;
		ring[wptr++] = 0x2a;
		ring[wptr++] = Packet(0x4a, 0);
		ring[wptr++] = 3u << 28;
		ring[wptr++] = Packet(0x12, 0);
		ring[wptr++] = 0;
		ring[wptr++] = Packet(0x11, 2);
		ring[wptr++] = 3;
		ring[wptr++] = 0x8000;
		ring[wptr++] = 0x8000;
		// Retire startup separately so its clear-state packets cannot fill
		// the smaller ring together with the first 1024-word data test.
		volatile uint32* startupMarker = gart.commandMemory
			+ (kControlOffset + 0x500) / 4;
		ring[wptr++] = Packet(0x42, 0); // PFP_SYNC_ME
		ring[wptr++] = 0;
		ring[wptr++] = Packet(0x37, 3);
		ring[wptr++] = 5 << 8 | 1 << 20;
		ring[wptr++] = (uint32)(kControlGPU + 0x500);
		ring[wptr++] = (kControlGPU + 0x500) >> 32;
		ring[wptr++] = 0x43535031;
		uint32 startupPadding = (-wptr) & 255;
		if (startupPadding == 1)
			startupPadding += 256;
		if (startupPadding != 0) {
			ring[wptr++] = Packet(0x10, startupPadding - 2);
			for (uint32 i = 1; i < startupPadding; i++)
				ring[wptr++] = 0;
		}
		__sync_synchronize();
		r[0x1520] = 1;
		(void)r[0x1520];
		r[0x3045] = wptr;
		bigtime_t startupDeadline = system_time() + 500000;
		while ((*startupMarker != 0x43535031 || r[0x21c0] != wptr)
			&& system_time() < startupDeadline)
			snooze(50);
		dprintf("amdgpu: GFX startup marker %#x ring %u/%u VM %#x/%#x\n",
			(unsigned)*startupMarker, (unsigned)r[0x21c0], (unsigned)wptr,
			(unsigned)r[0x536], (unsigned)r[0x537]);
		if (*startupMarker != 0x43535031 || r[0x21c0] != wptr
			|| ((r[0x536] | r[0x537]) & 0xff) != 0) {
			DumpExecutionState("clear-state startup stalled");
			faulted = true;
			Halt();
			Snapshot(result);
			return B_DEV_NOT_READY;
		}
		status_t irqStatus = interrupts.Initialize(r, info, pci);
		if (irqStatus != B_OK) {
			dprintf("amdgpu: IH initialization failed %#x\n", (unsigned)irqStatus);
			faulted = true;
			Halt();
			Snapshot(result);
			return irqStatus;
		}
		for (uint32 i = 0; i < 4; i++) {
			firmwareVersion[i] = firmware[i].version;
			firmwareFeature[i] = firmware[i].featureVersion;
		}
		firmwareVersion[4] = mec != NULL ? mec->program.version : 0;
		firmwareFeature[4] = mec != NULL ? mec->program.featureVersion : 0;
		ready = true;
	}
	return B_OK;
}

status_t
GfxEngine::ShaderMemoryInfo(amdgpu_shader_memory_info& result)
{
	if (!ready || faulted || !mecStarted)
		return B_DEV_NOT_READY;
	result = {};
	result.version = AMDGPU_HAIKU_ABI_VERSION;
	result.size = sizeof(result);
	result.vmid = 2;
	// Caller holds the device mutex with all engines idle. Polaris register
	// indices: AMD gfx_8_0_d.h and gmc_8_2_d.h; no indirect data or ACK reads.
	result.selector_before = regs[0x391];
	regs[0x391] = result.vmid << 4;
	(void)regs[0x391];
	result.sh_mem_config = regs[0x230d];
	result.sh_mem_bases = regs[0x230a];
	result.sh_mem_ape1_base = regs[0x230b];
	result.sh_mem_ape1_limit = regs[0x230c];
	regs[0x391] = result.selector_before;
	result.selector_after = regs[0x391];
	result.gds_base = regs[0x3304];
	result.gds_size = regs[0x3305];
	result.gws = regs[0x3322];
	result.oa = regs[0x3332];
	result.atc_ats_control = regs[0xcc9];
	result.atc_ats_debug = regs[0xcca];
	result.atc_vmid_pasid = regs[0xce9];
	result.atc_l1_address_offset = regs[0xcdd];
	for (uint32 aperture = 0; aperture < 2; aperture++) {
		for (uint32 field = 0; field < 4; field++)
			result.atc_aperture[aperture][field] = regs[0xcc0 + aperture + field * 2];
	}
	return B_OK;
}

status_t
GfxEngine::RenderInfo(const amdgpu::AtomRenderInfo& rom, amdgpu_render_info& result)
{
	if (!ready || faulted || !mecStarted)
		return B_DEV_NOT_READY;
	if (rom.gfxMajor != 8 || rom.shaderEngines == 0)
		return B_NOT_SUPPORTED;
	result = {};
	result.version = AMDGPU_HAIKU_ABI_VERSION;
	result.size = sizeof(result);
	result.started_us = system_time();
	result.gfx_major = rom.gfxMajor;
	result.gfx_minor = rom.gfxMinor;
	result.shader_engines = rom.shaderEngines;
	result.shader_arrays_per_engine = rom.shaderArraysPerEngine;
	result.cu_per_array = rom.cuPerArray;
	result.backends_per_engine = rom.backendsPerEngine;
	result.tile_pipes = rom.tilePipes;
	result.tcc_blocks = rom.tccBlocks;
	result.default_engine_khz = rom.defaultEngineKHz;
	result.default_memory_khz = rom.defaultMemoryKHz;
	result.reference_khz = rom.referenceKHz;
	result.timestamp_khz = amdgpu_smc_timestamp_khz(regs, rom.referenceKHz);
	result.chip_revision = regs[0xfc3] >> 28;
	result.gb_addr_config = regs[0x263e];
	for (uint32 i = 0; i < 32; i++) result.tile_mode[i] = regs[0x2644 + i];
	for (uint32 i = 0; i < 16; i++) result.macrotile_mode[i] = regs[0x2664 + i];
	result.mc_arb_ramcfg = regs[0x9d8];
	result.mc_shared_chmap = regs[0x801];
	result.mc_seq_misc0 = regs[0xa80];
	result.vram_type = result.mc_seq_misc0 >> 28;
	const uint32 channels[] = {1, 2, 4, 8, 3, 6, 10, 12, 16};
	uint32 channel = (result.mc_shared_chmap >> 12) & 15;
	if (channel < sizeof(channels) / sizeof(channels[0]))
		result.vram_bus_width = channels[channel]
			* ((result.mc_arb_ramcfg & 0x100) != 0 ? 64 : 32);
	for (uint32 i = 0; i < 5; i++) {
		result.firmware_version[i] = firmwareVersion[i];
		result.firmware_feature[i] = firmwareFeature[i];
	}
	// Only known SE/SH banks from the validated ATOM table. The device mutex
	// and idle queues exclude command processors and other selector users.
	uint32 index = regs[0xc200];
	result.grbm_index_before = index;
	uint32 rbWidth = rom.backendsPerEngine / rom.shaderArraysPerEngine;
	for (uint32 se = 0; se < rom.shaderEngines; se++) {
		for (uint32 sa = 0; sa < rom.shaderArraysPerEngine; sa++) {
			regs[0xc200] = (se << 16) | (sa << 8) | (1u << 30);
			(void)regs[0xc200];
			result.cu_disable[se][sa] = regs[0x226f];
			result.cu_user_disable[se][sa] = regs[0x2270];
			uint32 mask = ~((result.cu_disable[se][sa]
				| result.cu_user_disable[se][sa]) >> 16) & ((1u << rom.cuPerArray) - 1);
			result.cu_mask[se][sa] = mask;
			while (mask != 0) { result.active_cus++; mask &= mask - 1; }
			result.rb_disable[se][sa] = regs[0x263d];
			result.rb_user_disable[se][sa] = regs[0x26df];
			mask = ~((result.rb_disable[se][sa] | result.rb_user_disable[se][sa]) >> 16)
				& ((1u << rbWidth) - 1);
			result.enabled_backends |= mask << ((se * rom.shaderArraysPerEngine + sa) * rbWidth);
			result.raster_config[se][sa] = regs[0xa0d4];
			result.raster_config_1[se][sa] = regs[0xa0d5];
		}
	}
	regs[0xc200] = index;
	result.grbm_index_after = regs[0xc200];
	// RLC atomically latches both halves; do not combine live counter reads.
	regs[0xec26] = 1;
	result.gpu_timestamp = regs[0xec24];
	result.gpu_timestamp |= (uint64)regs[0xec25] << 32;
	result.finished_us = system_time();
	if (result.grbm_index_after != index || result.active_cus == 0
		|| result.enabled_backends == 0 || result.vram_bus_width == 0)
		return B_BAD_DATA;
	return B_OK;
}

status_t
GfxEngine::Test(volatile uint32* r, const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation,
	const amdgpu::FirmwareView firmware[4], SdmaEngine& sdma, Gart& gart,
	amdgpu_gfx_test& result, const amdgpu::MecFirmwareView* mec)
{
	status_t initialized = Initialize(r, info, reservation, firmware, sdma, gart, result, mec);
	if (initialized != B_OK)
		return initialized;
	volatile uint32* control = gart.commandMemory + kControlOffset / 4;
	result.stage = 4;
	uint32 n = 0;
	volatile uint32* ib = gart.commandMemory + 65536 / 4;
	volatile uint32* data = memory + 0x20000 / 4;
	if (++sequence == 0)
		sequence++;
	result.sequence = sequence;
	const uint32 shaderEnd = kDirectSequences + kShaderSequences;
	const uint32 drawEnd = shaderEnd + kDrawSequences;
	const uint32 ceEnd = drawEnd + 1;
	const uint32 vmShaderEnd = ceEnd + kVMShaderSequences;
	const bool direct = sequence <= kDirectSequences;
	const bool ceIB = sequence == ceEnd;
	const bool vmShader = sequence > ceEnd && sequence <= vmShaderEnd;
	const bool privateShader = vmShader && ((sequence - ceEnd) & 1) != 0;
	if (privateShader) {
		// The previous submission has retired. Remove its stale packets so
		// the entry checkpoint cannot prefetch an old outer-ring postlude.
		for (uint32 i = 0; i < kRingDwords; i++)
			ring[i] = (i & 1) == 0 ? Packet(0x10, 0) : 0;
	}
	const bool shader = (!direct && sequence <= shaderEnd) || vmShader;
	const bool draw = sequence > shaderEnd && sequence <= drawEnd;
	const bool minimalIB = sequence == vmShaderEnd + 1;
	// Separate CP memory reads from the first IB transition. The last eight
	// direct submissions cover ME/PFP, SRC_MEM/TC_L2, and VRAM/snooped RAM.
	// All addresses remain inside the diagnostic's private allocations.
	const bool cpRead = direct && sequence > kDirectSequences - 8;
	const uint32 readCase = cpRead ? sequence - (kDirectSequences - 7) : 0;
	const uint32 readExpected[2] = {0xa1324bf7u ^ sequence,
		0x591e8307u ^ sequence};
	if (cpRead) {
		for (uint32 i = 0; i < 2; i++) {
			memory[0x43000 / 4 + i] = readExpected[i];
			control[0x200 / 4 + i] = readExpected[i];
			control[0x300 / 4 + i] = 0;
		}
	}
	if (minimalIB || privateShader || ceIB)
		DumpExecutionState("before IB");
	uint64 destination = direct || shader || draw || minimalIB || ceIB ? gpu : kMemoryVA;
	uint64 markerAddress = privateShader ? kControlVA
		: (direct || shader || draw || minimalIB || ceIB ? kControlGPU : kControlVA);
	for (uint32 i = 0; i < 3072; i++)
		data[(int32)i - 1024] = 0xabcddcba;
	ib[n++] = Packet(0x37, 1026); // WRITE_DATA, 1024 payload DWORDs
	ib[n++] = 5 << 8 | 1 << 20; // memory, write confirmation
	ib[n++] = (uint32)(destination + 0x20000);
	ib[n++] = (destination + 0x20000) >> 32;
	for (uint32 i = 0; i < 1024; i++)
		ib[n++] = 0x71324589 ^ (i * 0x10204081u) ^ sequence;
	// A confirmed memory write in the same IB follows all payload writes.
	control[0x0 / 4] = 0;
	ib[n++] = Packet(0x37, 3);
	ib[n++] = 5 << 8 | 1 << 20;
	ib[n++] = (uint32)markerAddress;
	ib[n++] = markerAddress >> 32;
	ib[n++] = sequence;
	// Like Mesa, use one counted NOP for the padding block. Keep this
	// diagnostic independent of the header-only (-1 count) NOP encoding.
	uint32 padding = (-n) & 255;
	if (padding == 1)
		padding += 256;
	if (padding != 0) {
		ib[n++] = Packet(0x10, padding - 2);
		for (uint32 i = 1; i < padding; i++)
			ib[n++] = 0;
	}
	auto emit = [&](uint32 word) { ring[wptr++ & kRingMask] = word; };
	auto snapshotVM = [&](uint32 slot) {
		for (uint32 context = 0; context < 2; context++) {
			uint32 offset = 0x100 + (slot * 2 + context) * 4;
			control[offset / 4] = 0xffffffff;
			emit(Packet(0x40, 4)); // COPY_DATA: register to confirmed memory
			emit(5 << 8 | 1 << 20);
			emit(0x536 + context);
			emit(0);
			emit((uint32)(kControlGPU + offset));
			emit((kControlGPU + offset) >> 32);
		}
		uint32 offset = 0x110 + slot * 4;
		control[offset / 4] = 0xffffffff;
		emit(Packet(0x40, 4));
		emit(5 << 8 | 1 << 20);
		emit(0xc08e); // CP_PFP_LOAD_CONTROL
		emit(0);
		emit((uint32)(kControlGPU + offset));
		emit((kControlGPU + offset) >> 32);
	};
	// Invalidate caches after CPU updates and synchronize PFP before IB reads.
	// gfx_v8_0_emit_mem_sync uses this full-range VI cache operation.
	emit(Packet(0x43, 3));
	emit(1 << 22 | 1 << 23 | 1 << 27 | 1 << 29 | 1 << 18);
	emit(0xffffffff);
	emit(0);
	emit(10);
	emit(Packet(0x42, 0)); // PFP_SYNC_ME
	emit(0);
	// Match amdgpu_ib_schedule's kernel-job wrapper on GFX8. The private
	// condition is always true; no client can modify it or these packets.
	control[0x40 / 4] = 1;
	emit(Packet(0x22, 3)); // COND_EXEC
	emit((uint32)(kControlGPU + 0x40));
	emit((kControlGPU + 0x40) >> 32);
	emit(0);
	uint32 conditionOffset = wptr;
	emit(0); // patch the number of following DWORDs after the EOP fence
	emit(Packet(0x3c, 5)); // PFP HDP write/wait/write handshake
	emit(1 << 6 | 3 | 1 << 8);
	emit(0x1537);
	emit(0x1538);
	emit(1);
	emit(1);
	emit(0x20);
	snapshotVM(0);
	if (shader) {
		// VMID belongs to the IB execution context. A SET_SH_REG of
		// COMPUTE_VMID in the kernel ring cannot select a client VM.
		// Preserve the marker packet before reusing the bulk-data IB for the
		// longer shader stream. Its NOP runway overwrites the old packet.
		const uint32 marker[] = {ib[1028], ib[1029], ib[1030], ib[1031], ib[1032]};
		volatile uint32* shaderCommands = ib;
		uint32 shaderLength = 0;
		auto emitShader = [&](uint32 word) {
			if (privateShader)
				shaderCommands[shaderLength++] = word;
			else
				emit(word);
		};
		if (privateShader) {
			control[0x80 / 4] = control[0x84 / 4] = 0;
			emitShader(Packet(0x37, 3));
			emitShader(5 << 8 | 1 << 20);
			emitShader(kControlVA + 0x84);
			emitShader(0);
			emitShader(sequence);
			emitShader(Packet(0x42, 0));
			emitShader(0);
			emitShader(Packet(0x3c, 5)); // PFP waits for CPU's first release
			emitShader(5 | 1 << 4 | 1 << 8); // memory >= 1
			emitShader(kControlVA + 0x80);
			emitShader(0);
			emitShader(1);
			emitShader(0xffffffff);
			emitShader(0x20);
			// Use separate two-DWORD NOPs so this is a stream of packets,
			// not a single large NOP whose payload could simply be skipped.
			// The entry gate plus its packets occupy an even DWORD count.
			while (shaderLength < kShaderIBDwords - 256) {
				emitShader(Packet(0x10, 0));
				emitShader(0);
			}
		}
		const uint64 shaderBase = privateShader ? kMemoryVA : gpu;
		dprintf("amdgpu: compute seq %u VMID %u code %#" B_PRIx64
			" output %#" B_PRIx64 "\n", (unsigned)sequence,
			privateShader ? 1u : 0u, shaderBase + 0x40000, shaderBase + 0x20000);
		auto setShader = [&](uint32 reg, uint32 value) {
			emitShader(Packet(0x76, 1) | 2); // SET_SH_REG, compute shader type
			emitShader(reg - 0x2c00);
			emitShader(value);
		};
		setShader(0x2e04, 0); // COMPUTE_START_X/Y/Z
		setShader(0x2e05, 0);
		setShader(0x2e06, 0);
		setShader(0x2e07, 64); // NUM_THREAD_X/Y/Z
		setShader(0x2e08, 1);
		setShader(0x2e09, 1);
		setShader(0x2e0c, (shaderBase + 0x40000) >> 8);
		setShader(0x2e0d, (shaderBase + 0x40000) >> 40);
		setShader(0x2e12, 1 | 1 << 6 | 0xc0 << 12); // 8 VGPR, 16 SGPR
		setShader(0x2e13, 3 << 1 | 1 << 7); // 3 user SGPRs + group X
		setShader(0x2e14, privateShader ? 1 : 0); // explicit shader address space
		setShader(0x2e15, 0); // RESOURCE_LIMITS: one group per CU, no wave limit
		setShader(0x2e16, 0xffffffff); // STATIC_THREAD_MGMT_SE0/1
		setShader(0x2e17, 0xffffffff);
		setShader(0x2e18, 0); // no scratch ring
		setShader(0x2e19, 0xffffffff); // STATIC_THREAD_MGMT_SE2/3
		setShader(0x2e1a, 0xffffffff);
		setShader(0x2e40, (uint32)(shaderBase + 0x20000));
		setShader(0x2e41, (shaderBase + 0x20000) >> 32);
		setShader(0x2e42, 0x71324589 ^ sequence);
		emitShader(Packet(0x15, 3) | 2); // DISPATCH_DIRECT: 16 groups x 64 threads
		emitShader(16);
		emitShader(1);
		emitShader(1);
		emitShader(1 | 1 << 2); // COMPUTE_SHADER_EN, FORCE_START_AT_000
		emitShader(Packet(0x46, 0));
		emitShader(7 | 4 << 8); // CS_PARTIAL_FLUSH before the completion marker
		// Only the shader writes the payload. The CP writes its marker after
		// all waves finish; EOP below makes their stores visible to the CPU.
		for (uint32 word : marker)
			emitShader(word);
		if (privateShader) {
			// Hold PFP inside the IB after its shader and confirmed marker.
			// The CPU samples both fault contexts before allowing IB return.
			emitShader(Packet(0x42, 0)); // PFP_SYNC_ME
			emitShader(0);
			emitShader(Packet(0x3c, 5)); // WAIT_REG_MEM, memory, PFP, >= 2
			emitShader(5 | 1 << 4 | 1 << 8);
			emitShader(kControlVA + 0x80);
			emitShader(0);
			emitShader(2);
			emitShader(0xffffffff);
			emitShader(0x20);
			uint32 pad = (-shaderLength) & 255;
			if (pad == 1)
				pad += 256;
			if (pad != 0) {
				emitShader(Packet(0x10, pad - 2));
				for (uint32 i = 1; i < pad; i++)
					emitShader(0);
			}
			emit(Packet(0x28, 1));
			emit(0x80000000);
			emit(0x80000000);
			emit(Packet(0x3f, 2));
			emit(kCommandVA);
			emit(0);
			emit(shaderLength | 1 << 24);
		}
	}
	if (draw) {
		auto setContext = [&](uint32 reg, uint32 value) {
			emit(Packet(0x69, 1));
			emit(reg - 0xa000);
			emit(value);
		};
		auto setShader = [&](uint32 reg, uint32 value) {
			emit(Packet(0x76, 1));
			emit(reg - 0x2c00);
			emit(value);
		};
		for (const auto& entry : kDrawContext)
			setContext(entry[0], entry[1]);
		setContext(0xa318, (gpu + 0x20000) >> 8);
		setContext(0xa319, 3); // 32 pixels / 8 - 1
		setContext(0xa31a, 15); // 32 * 32 / 64 - 1
		setContext(0xa31b, 0); // single slice
		setContext(0xa31c, 10 << 2 | 1 << 7 | 1 << 15); // RGBA8 UNORM, linear
		setContext(0xa31d, 0); // 1 sample, no FMASK
		for (uint32 reg = 0xa31e; reg <= 0xa325; reg++)
			setContext(reg, 0); // no DCC/CMASK/FMASK or fast-clear metadata
		for (uint32 target = 1; target < 8; target++)
			setContext(0xa31c + target * 15, 0);
		for (uint32 reg = 0xa2fe; reg <= 0xa30d; reg++)
			setContext(reg, 0); // center sample locations
		setContext(0x1000a2aa, 0x2010007f); // IA index 1, 2 prim groups/wave
		setShader(0x2c46, 0xffff);
		setShader(0x2c48, (gpu + 0x41000) >> 8);
		setShader(0x2c49, (gpu + 0x41000) >> 40);
		setShader(0x2c4a, 1 | 1 << 6 | 0xc0 << 12); // 8 VGPR, 16 SGPR
		setShader(0x2c4b, 0); // vertex ID only; no scratch, user SGPR or streamout
		setShader(0x2c07, 0xffff);
		setShader(0x2c08, (gpu + 0x42000) >> 8);
		setShader(0x2c09, (gpu + 0x42000) >> 40);
		setShader(0x2c0a, 1 << 6 | 0xc0 << 12); // 4 VGPR, 16 SGPR
		setShader(0x2c0b, 4 << 1); // RGBA in s0..s3
		setShader(0x2c0c, (sequence & 1) ? 0x3f800000 : 0);
		setShader(0x2c0d, (sequence & 1) ? 0 : 0x3f800000);
		setShader(0x2c0e, 0);
		setShader(0x2c0f, 0x3f800000);
		emit(Packet(0x79, 1)); // VGT_PRIMITIVE_TYPE, index 1
		emit(0x10000242);
		emit(4); // triangle list
		emit(Packet(0x2f, 0)); // NUM_INSTANCES
		emit(1);
		emit(Packet(0x2d, 1)); // DRAW_INDEX_AUTO, exactly three vertices
		emit(3);
		emit(2); // auto-generated indices
		emit(Packet(0x46, 0));
		emit(0x10 | 4 << 8); // PS_PARTIAL_FLUSH
		for (uint32 i = 1028; i < 1033; i++)
			emit(ib[i]);
	}

	if (direct || minimalIB || ceIB) {
		// The minimal IB obtains its completion marker exclusively from the
		// private VM's five-DWORD IB. Only its bulk payload runs directly.
		uint32 directLength = minimalIB ? 1028 : n;
		for (uint32 i = 0; i < directLength; i++)
			emit(ib[i]);
	}
	// First exercise direct ring packets, then the indirect-buffer fetch path.
	// Both streams and all addresses are private to the kernel.
	if (ceIB) {
		// libdrm basic_tests.c's CE counter handshake, with the DE wait in
		// the direct ring. This isolates CE indirect fetch before the first
		// DE indirect fetch. Only our existing private VM1 command RAM is used.
		volatile uint32* commands = ib + 0xc000 / 4;
		control[0x400 / 4] = 0;
		commands[0] = Packet(0x89, 0); // SET_CE_DE_COUNTERS
		commands[1] = 0;
		commands[2] = Packet(0x37, 3); // CE confirmed write, private VM1
		commands[3] = 2u << 30 | 5 << 8 | 1 << 20;
		commands[4] = kControlVA + 0x400;
		commands[5] = 0;
		commands[6] = 0xcea00000 ^ sequence;
		commands[7] = Packet(0x84, 0); // INCREMENT_CE_COUNTER
		commands[8] = 1;
		emit(Packet(0x33, 2)); // INDIRECT_BUFFER_CONST
		emit(kCommandVA + 0xc000);
		emit(0);
		emit(9 | 1 << 24);
		emit(Packet(0x86, 0)); // WAIT_ON_CE_COUNTER
		emit(1);
	}
	if (!direct && !shader && !draw && !ceIB) {
		// Explicitly disable inherited register loads/shadowing outside the
		// clear-state preamble before entering a private indirect buffer.
		emit(Packet(0x28, 1));
		emit(0x80000000);
		emit(0x80000000);
		uint64 address = kCommandVA;
		uint32 length = n;
		if (minimalIB) {
			// Match the unpadded WRITE_DATA used by Linux's GFX8 ring test.
			// This also proves that the IB actually writes through VMID1.
			address += 0x8000;
			length = 5;
			ib[0x8000 / 4] = Packet(0x37, 3);
			ib[0x8000 / 4 + 1] = 5 << 8 | 1 << 20;
			ib[0x8000 / 4 + 2] = kControlVA;
			ib[0x8000 / 4 + 3] = 0;
			ib[0x8000 / 4 + 4] = sequence;
		}
		ring[wptr++ & kRingMask] = Packet(0x3f, 2);
		ring[wptr++ & kRingMask] = (uint32)address;
		ring[wptr++ & kRingMask] = address >> 32;
		ring[wptr++ & kRingMask] = length | 1 << 24; // private VMID1
	}
	if (cpRead) {
		const uint64 source = (readCase & 4) != 0
			? kControlGPU + 0x200 : gpu + 0x43000;
		emit(Packet(0x40, 4)); // COPY_DATA, two confirmed DWORDs to snooped RAM
		emit(((readCase & 1) != 0 ? 2 : 1) | 5 << 8 | 1 << 16
			| 1 << 20 | ((readCase & 2) != 0 ? 1u << 30 : 0));
		emit((uint32)source);
		emit(source >> 32);
		emit((uint32)(kControlGPU + 0x300));
		emit((kControlGPU + 0x300) >> 32);
	}
	control[0x4 / 4] = 0;
	auto emitPostlude = [&]() {
		snapshotVM(1);
		emit(Packet(0x37, 3)); // PFP invalidates HDP after the command stream
		emit(1 << 30 | 1 << 20);
		emit(0xbcc);
		emit(0);
		emit(1);
		// VI requires a dummy EOP followed by the real event. This fence is
		// outside the IB and covers its return plus cache visibility.
		for (uint32 value = 0; value < 2; value++) {
			emit(Packet(0x47, 4));
			emit(0x14 | 5 << 8 | 1 << 15 | 1 << 16 | 1 << 17);
			emit((uint32)(kControlGPU + 0x04));
			emit((kControlGPU + 0x04) >> 32 | 1 << 29);
			emit(value == 0 ? sequence - 1 : sequence);
			emit(0);
		}
	};
	auto submit = [&]() {
		uint32 pad = (-wptr) & 255;
		if (pad == 1)
			pad += 256;
		if (pad != 0) {
			emit(Packet(0x10, pad - 2));
			for (uint32 i = 1; i < pad; i++)
				emit(0);
		}
		__sync_synchronize();
		(void)ring[(wptr - 1) & kRingMask];
		r[0x1520] = 1;
		(void)r[0x1520];
		r[0x3045] = wptr & kRingMask;
		(void)r[0x3045];
	};
	// Withhold even the bytes of the postlude until both IB checkpoints.
	// The first submission ends with NOPs, and its condition covers only
	// that first batch. The separately submitted postlude is unconditional.
	if (!privateShader)
		emitPostlude();
	ring[conditionOffset & kRingMask] = wptr - conditionOffset - 1;
	submit();
	if (privateShader) {
		bigtime_t gateDeadline = system_time() + 500000;
		while (control[0x84 / 4] != sequence && system_time() < gateDeadline)
			snooze(50);
		snooze(1000);
		dprintf("amdgpu: GFX IB entry seq %u marker %u shader marker %u EOP %u VM %#x/%#x"
			" pages %#x/%#x IB remaining %u ring %u/%u\n",
			(unsigned)sequence, (unsigned)control[0x84 / 4],
			(unsigned)control[0], (unsigned)control[1], (unsigned)r[0x536],
			(unsigned)r[0x537], (unsigned)r[0x53e], (unsigned)r[0x53f],
			(unsigned)r[0xc0ce], (unsigned)r[0x21c0], (unsigned)r[0x3045]);
		DumpExecutionState("IB entry gate");
		control[0x80 / 4] = 1;
		__sync_synchronize();
		r[0x1520] = 1;
		(void)r[0x1520];
		gateDeadline = system_time() + 500000;
		while (control[0] != sequence && system_time() < gateDeadline)
			snooze(50);
		snooze(1000);
		dprintf("amdgpu: GFX inside IB seq %u marker %u VM %#x/%#x"
			" pages %#x/%#x IB remaining %u ring %u/%u\n",
			(unsigned)sequence, (unsigned)control[0], (unsigned)r[0x536],
			(unsigned)r[0x537], (unsigned)r[0x53e], (unsigned)r[0x53f],
			(unsigned)r[0xc0ce], (unsigned)r[0x21c0], (unsigned)r[0x3045]);
		DumpExecutionState("IB tail gate");
		dprintf("amdgpu: GFX deferred postlude seq %u begins at ring %u\n",
			(unsigned)sequence, (unsigned)(wptr & kRingMask));
		emitPostlude();
		submit();
		// Always release the private wait, including a missing-marker case.
		control[0x80 / 4] = 2;
		__sync_synchronize();
		r[0x1520] = 1;
		(void)r[0x1520];
	}
	bigtime_t deadline = system_time() + 500000;
	while ((control[0x0 / 4] != sequence || control[0x4 / 4] != sequence
		|| r[0x21c0] != (wptr & kRingMask)) && system_time() < deadline)
		snooze(50);
	__sync_synchronize();
	if (ceIB) {
		dprintf("amdgpu: GFX CE indirect seq %u marker %#x counter %u VM %#x/%#x\n",
			(unsigned)sequence, (unsigned)control[0x400 / 4],
			(unsigned)r[0xc09a], (unsigned)r[0x536],
			(unsigned)r[0x537]);
		// The CE-only marker proves execution. The counter register's value
		// after the handshake is diagnostic, not a completion contract.
		if (control[0x400 / 4] != (0xcea00000 ^ sequence))
			result.mismatches++;
	}
	if (cpRead) {
		dprintf("amdgpu: GFX CP read case %u seq %u %s %s %s got %#x/%#x"
			" expected %#x/%#x VM %#x/%#x\n", (unsigned)readCase,
			(unsigned)sequence, (readCase & 4) != 0 ? "RAM" : "VRAM",
			(readCase & 2) != 0 ? "PFP" : "ME",
			(readCase & 1) != 0 ? "TC_L2" : "SRC_MEM",
			(unsigned)control[0x300 / 4], (unsigned)control[0x300 / 4 + 1],
			(unsigned)readExpected[0], (unsigned)readExpected[1],
			(unsigned)r[0x536], (unsigned)r[0x537]);
		for (uint32 i = 0; i < 2; i++) {
			if (control[0x300 / 4 + i] != readExpected[i])
				result.mismatches++;
		}
	}
	status_t status = control[0x0 / 4] == sequence
		&& control[0x4 / 4] == sequence && r[0x21c0] == (wptr & kRingMask)
		? B_OK : B_TIMED_OUT;
	if (interrupts.Error() != B_OK) status = interrupts.Error();
	if (status == B_OK) {
		// The completion page is snooped RAM. Invalidate HDP only after GPU
		// completion so CPU reads of the VRAM payload cannot reuse old data.
		r[0xbcc] = 1;
		(void)r[0xbcc];
		__sync_synchronize();
		for (uint32 i = 0; i < 3072; i++) {
			uint32 expected = i >= 1024 && i < 2048
				? 0x71324589 ^ ((i - 1024) * 0x10204081u) ^ sequence : 0xabcddcba;
			if (draw && i >= 1024 && i < 2048) {
				uint32 pixel = i - 1024;
				expected = pixel % 32 + pixel / 32 <= 31
					? ((sequence & 1) ? 0xff0000ff : 0xff00ff00) : 0xabcddcba;
			}
			if (data[(int32)i - 1024] != expected) {
				if (draw && result.mismatches < 8)
					dprintf("amdgpu: draw word %u got %#x expected %#x\n",
						(unsigned)i, (unsigned)data[(int32)i - 1024], (unsigned)expected);
				result.mismatches++;
			}
		}
		result.checked_bytes = 12288;
		if (result.mismatches != 0 || ((r[0x536] | r[0x537]) & 0xff) != 0)
			status = B_BAD_DATA;
	}
	Snapshot(result);
	dprintf("amdgpu: GFX VM checkpoints seq %u before %#x/%#x after %#x/%#x\n",
		(unsigned)sequence, (unsigned)control[0x100 / 4],
		(unsigned)control[0x104 / 4], (unsigned)control[0x108 / 4],
		(unsigned)control[0x10c / 4]);
	dprintf("amdgpu: GFX load control before %#x after %#x\n",
		(unsigned)control[0x110 / 4], (unsigned)control[0x114 / 4]);
	if (status != B_OK) {
		dprintf("amdgpu: GFX completion marker %u EOP %u expected %u\n",
			(unsigned)control[0], (unsigned)control[1], (unsigned)sequence);
		DumpExecutionState("after fault");
		const uint32 registers[] = {0x208d, 0x21c2, 0x3043, 0x3044, 0x3046,
			0x3047, 0x3061, 0x3066, 0x230a, 0x230b, 0x230c, 0x230d,
			0x500, 0x501, 0x502, 0x578, 0x504, 0x50c, 0x54f, 0x546,
			0x3051, 0xa0da, 0x2e0c, 0x2e0d, 0x2e12, 0x2e13, 0x2e14, 0x2e15};
		for (uint32 index : registers)
			dprintf("amdgpu: GFX fault register %#x = %#x\n", (unsigned)index, (unsigned)r[index]);
		faulted = true;
		Halt();
	} else
		result.stage = 5;
	dprintf("amdgpu: GFX stage %u status %#x seq %u ring %u/%u GRBM %#x RLC %#x\n",
		(unsigned)result.stage, (unsigned)status, (unsigned)sequence,
		(unsigned)result.rptr, (unsigned)result.wptr,
		(unsigned)result.grbm_status, (unsigned)result.rlc_status);
	return status;
}

status_t
GfxEngine::ExecuteVM(uint64 directory, uint64 destination, uint32 value,
	volatile uint32* commands, Gart& gart, amdgpu_vm_test& result)
{
	volatile uint32* ib = commands;
	uint32 count = 0;
	auto command = [&](uint32 word) { ib[count++] = word; };
	auto shader = [&](uint32 reg, uint32 word) {
		command(Packet(0x76, 1) | 2);
		command(reg - 0x2c00);
		command(word);
	};
	shader(0x2e04, 0); shader(0x2e05, 0); shader(0x2e06, 0);
	shader(0x2e07, 64); shader(0x2e08, 1); shader(0x2e09, 1);
	shader(0x2e0c, kClientShaderVA >> 8); shader(0x2e0d, 0);
	shader(0x2e12, 1 | 1 << 6 | 0xc0 << 12);
	shader(0x2e13, 3 << 1 | 1 << 7);
	shader(0x2e14, 2);
	shader(0x2e15, 0);
	shader(0x2e16, 0xffffffff); shader(0x2e17, 0xffffffff);
	shader(0x2e18, 0);
	shader(0x2e19, 0xffffffff); shader(0x2e1a, 0xffffffff);
	shader(0x2e40, (uint32)destination);
	shader(0x2e41, destination >> 32);
	shader(0x2e42, value);
	command(Packet(0x15, 3) | 2);
	command(16); command(1); command(1); command(1 | 1 << 2);
	command(Packet(0x46, 0)); command(7 | 4 << 8);
	uint32 padding = (-count) & 255;
	if (padding == 1) padding += 256;
	if (padding != 0) {
		command(Packet(0x10, padding - 2));
		for (uint32 i = 1; i < padding; i++) command(0);
	}
	return ExecuteIB(directory, kClientIbVA, count, gart, result);
}

status_t
GfxEngine::ExecuteIB(uint64 directory, uint64 address, uint32 dwords,
	Gart& gart, amdgpu_vm_test& result, uint32 diagnosticGds)
{
	if (diagnosticGds > 3)
		return B_BAD_VALUE;
	if (!ready || faulted || !mecStarted)
		return B_DEV_NOT_READY;
	if (interrupts.Error() != B_OK)
		return interrupts.Error();
	volatile uint32* r = regs;
	if (r[0x21c0] != (wptr & kRingMask))
		return B_BUSY;
	// GFX8 GDS is byte-addressed, 64 KiB on the qualified Polaris device.
	// VMID0's capacity is retained for firmware; this driver has no other
	// GDS users. Probe windows are wholly within the first owned 4 KiB.
	if (diagnosticGds != 0 && (r[0x3300] != 0 || r[0x3301] != 0x10000))
		return B_NOT_SUPPORTED;
	// VMID2 is leased only for this synchronous job. Between jobs it points
	// at the kernel diagnostic's empty/private directory, never a client.
	auto bind = [&](uint64 pageDirectory) {
		r[0x1520] = 1;
		(void)r[0x1520];
		r[0x551] = pageDirectory >> 12;
		(void)r[0x551];
		r[0x51e] = 4;
		bigtime_t deadline = system_time() + 100000;
		while ((r[0x51f] & 4) == 0) {
			if (system_time() >= deadline)
				return B_TIMED_OUT;
			snooze(10);
		}
		return B_OK;
	};
	status_t status = bind(directory);
	if (status == B_OK) {
		uint32 select = r[0x391];
		r[0x391] = 2 << 4;
		r[0x230d] = 1 << 5 | 3 << 8 | 3 << 3;
		r[0x230a] = 0x2000;
		r[0x230b] = 1;
		r[0x230c] = 0;
		r[0x391] = select;
		// No client owns GDS, global wave sync or ordered-append resources.
		// Linux gfx_v8_0_init_gds_vmid applies the same zero allocation.
		r[0x3304] = 0; r[0x3305] = 0;
		r[0x3322] = 0; r[0x3332] = 0;
		if (diagnosticGds != 0) {
			const uint32 base = diagnosticGds == 2 ? 1024 : 0;
			const uint32 size = diagnosticGds == 1 ? 4096 : diagnosticGds == 2 ? 256 : 0;
			// Matches Linux gfx_v8_0_ring_emit_gds_switch's byte units and
			// fixed VMID register layout. No job is in flight at this point.
			r[0x3304] = base; r[0x3305] = size;
			if (r[0x3304] != base || r[0x3305] != size
				|| r[0x3322] != 0 || r[0x3332] != 0) {
				r[0x3304] = 0; r[0x3305] = 0;
				faulted = true;
				Halt();
				return B_BAD_DATA;
			}
			dprintf("amdgpu: GDS probe mode %u VMID2 base %u size %u GWS %u OA %u\n",
				(unsigned)(diagnosticGds - 1), (unsigned)r[0x3304],
				(unsigned)r[0x3305], (unsigned)r[0x3322], (unsigned)r[0x3332]);
		}
		volatile uint32* completion = gart.commandMemory + (kControlOffset + 0x600) / 4;
		*completion = 0;
		if (++vmSequence == 0) vmSequence++;
		auto emit = [&](uint32 word) { ring[wptr++ & kRingMask] = word; };
		emit(Packet(0x43, 3));
		emit(1 << 22 | 1 << 23 | 1 << 27 | 1 << 29 | 1 << 18);
		emit(0xffffffff); emit(0); emit(10);
		emit(Packet(0x42, 0)); emit(0);
		emit(Packet(0x28, 1)); emit(0x80000000); emit(0x80000000);
		emit(Packet(0x76, 1) | 2); emit(0x2e14 - 0x2c00); emit(2);
		emit(Packet(0x3f, 2)); emit((uint32)address); emit(address >> 32);
		emit(dwords | 2 << 24);
		// Both EOPs are in the trusted VMID0 ring. No client mapping includes
		// this completion page. Raw PM4 remains root-only while hardware
		// privilege enforcement is being qualified.
		const uint64 fenceAddress = kControlGPU + 0x600;
		for (uint32 i = 0; i < 2; i++) {
			emit(Packet(0x47, 4));
			emit(0x14 | 5 << 8 | 1 << 15 | 1 << 16 | 1 << 17);
			emit((uint32)fenceAddress);
			emit(fenceAddress >> 32 | 1 << 29 | (i == 1 ? 2 << 24 : 0));
			emit(i == 0 ? vmSequence - 1 : vmSequence); emit(0);
		}
		uint32 padding = (-wptr) & 255;
		if (padding == 1) padding += 256;
		if (padding != 0) {
			emit(Packet(0x10, padding - 2));
			for (uint32 i = 1; i < padding; i++) emit(0);
		}
		uint64 ticket = interrupts.Ticket(completion, vmSequence);
		__sync_synchronize();
		(void)ring[(wptr - 1) & kRingMask];
		r[0x1520] = 1; (void)r[0x1520];
		r[0x3045] = wptr & kRingMask; (void)r[0x3045];
		bigtime_t deadline = system_time() + 500000;
		status = interrupts.Wait(ticket, deadline);
		// The IRQ proves notification, the private fence proves retirement.
		// Ring padding can retire shortly after EOP; it shares the same limit.
		while (status == B_OK && interrupts.Error() == B_OK
			&& (*completion != vmSequence || r[0x21c0] != (wptr & kRingMask))
			&& system_time() < deadline)
			snooze(50);
		__sync_synchronize();
		if (status == B_OK)
			status = *completion == vmSequence && r[0x21c0] == (wptr & kRingMask)
				? B_OK : B_TIMED_OUT;
		if (interrupts.Error() != B_OK) status = interrupts.Error();
		result.completion = *completion;
		if (status == B_OK && ((r[0x536] | r[0x537]) & 0xff) != 0)
			status = B_BAD_DATA;
		if (status == B_OK) {
			r[0xbcc] = 1; (void)r[0xbcc];
			__sync_synchronize();
			// All work has retired. Detach and invalidate before a caller can
			// unmap or close; only then may page tables/backing BOs be reused.
			status = bind(gpu - kScratchOffset + (24ULL << 20));
			if (diagnosticGds != 0) {
				r[0x3305] = 0; r[0x3304] = 0;
				if (r[0x3305] != 0 || r[0x3304] != 0) status = B_BAD_DATA;
			}
		}
	}
	result.vm_fault_status[0] = r[0x536];
	result.vm_fault_status[1] = r[0x537];
	result.rptr = r[0x21c0]; result.wptr = r[0x3045];
	if (status != B_OK) {
		faulted = true;
		Halt();
		amdgpu_irq_info irq;
		interrupts.Snapshot(irq);
		dprintf("amdgpu: client IRQ status %#x events %" B_PRIu64 " vectors %" B_PRIu64
			" ring %#x/%#x last %#x/%#x/%#x/%#x\n", (unsigned)irq.status,
			irq.eop_events, irq.vectors, (unsigned)irq.rptr, (unsigned)irq.wptr,
			(unsigned)irq.last[0], (unsigned)irq.last[1], (unsigned)irq.last[2], (unsigned)irq.last[3]);
		DumpExecutionState("client VM fault");
		// Alternate-translation configuration/fault evidence. Direct reads;
		// no W1C acknowledgement or indexed data port is touched.
		const uint32 atcRegisters[] = {0xcc9, 0xcca, 0xccc, 0xccd, 0xcce,
			0xccf, 0xcd0, 0xcd1, 0xcd2, 0xce9};
		for (uint32 index : atcRegisters)
			dprintf("amdgpu: ATC fault register %#x = %#x\n", (unsigned)index,
				(unsigned)r[index]);
	}
	dprintf("amdgpu: client VM seq %u status %#x faults %#x/%#x ring %u/%u\n",
		(unsigned)vmSequence, (unsigned)status, (unsigned)result.vm_fault_status[0],
		(unsigned)result.vm_fault_status[1], (unsigned)result.rptr, (unsigned)result.wptr);
	return status;
}

void
GfxEngine::Halt()
{
	regs[0x21b6] |= kHalt;
	if (mecStarted) {
		regs[0x208d] |= 0x50000000;
		(void)regs[0x208d];
	}
	(void)regs[0x21b6];
}

void
GfxEngine::Uninitialize()
{
	if (!attempted)
		return;
	Halt();
	interrupts.Uninitialize();
	regs[0xec00] &= ~1u;
	if (vmEnabled) {
		regs[0x505] &= ~1u;
		(void)regs[0x505];
		regs[0x51e] = 6;
		(void)regs[0x51f];
		vmEnabled = false;
	}
	if (vmArea >= 0)
		delete_area(vmArea);
	vmArea = -1;
	delete_area(area);
	ready = false;
	area = -1;
}
