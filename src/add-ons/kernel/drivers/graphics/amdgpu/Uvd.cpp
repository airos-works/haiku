/*
 * Copyright 2014 Advanced Micro Devices, Inc.
 * Copyright 2026, air/OS.
 * Distributed under the MIT license in UvdRegisters.h.
 */
// UVD6 initialization and packet formats: Linux 6.18.52 uvd_v6_0.c.
#include "Uvd.h"
#include "Smu.h"
#include "UvdRegisters.h"
#include "UvdFixture.h"
#include <KernelExport.h>
#include <string.h>

namespace {
const uint32 kBytes = 20 << 20;
const uint32 kRing = 3 << 20;
const uint32 kFence = kRing + 0x10000;
const uint32 kIB = kRing + 0x11000;
const uint32 kMessage = 4 << 20;
const uint32 kFeedback = kMessage + 0x1000;
const uint32 kScaling = kMessage + 0x2000;
const uint32 kBitstream = kMessage + 0x3000;
const uint32 kDPB = kMessage + 0x5000;
const uint32 kDPBBytes = 15923584;
// Linux vi.c reports Polaris10 external_rev = chip_rev + 0x50.
// That selects libdrm's separate H.264 context-buffer layout.
const uint32 kPictureDPBBytes = 0x006b9400;
const uint32 kContextBytes = 0x0050af00;
const uint32 kContext = kDPB + ((kPictureDPBBytes + 4095) & ~4095u);
const uint32 kTarget = kDPB + ((kDPBBytes + 4095) & ~4095u) + 4096;
const uint32 kTargetBytes = AMDGPU_UVD_TEST_OUTPUT_BYTES;
const uint32 kGuards[] = {kDPB - 4096, kTarget - 4096, kTarget + kTargetBytes};
static_assert(kContext + kContextBytes <= kTarget - 4096, "UVD context extent");
static_assert(kTarget + kTargetBytes + 4096 <= kBytes, "private UVD extent");

static void Copy(volatile uint32* memory, uint32 offset, const uint8_t* data, uint32 size)
{
	for (uint32 i = 0; i < size; i += 4)
		memory[(offset + i) / 4] = amdgpu::ReadLE32(data + i);
}
}

void
UvdEngine::Snapshot(amdgpu_uvd_test& result)
{
	result.uvd_status = regs[mmUVD_STATUS];
	result.power_status = regs[mmUVD_POWER_STATUS];
	result.ring_control = regs[mmUVD_RBC_RB_CNTL];
	result.rptr = regs[mmUVD_RBC_RB_RPTR];
	result.wptr = regs[mmUVD_RBC_RB_WPTR];
	result.context = regs[mmUVD_CONTEXT_ID];
	result.vm_fault_status = regs[0x536];
	result.vm_fault_address = regs[0x53e];
	if ((result.vm_fault_status & 0xff) == 0 && (regs[0x537] & 0xff) != 0) {
		result.vm_fault_status = regs[0x537];
		result.vm_fault_address = regs[0x53f];
	}
	// Keep setup and failures visible, but do not print on every completed
	// picture. Debug output can synchronously draw through the framebuffer.
	if (result.stage != 5 || result.fence != result.sequence || result.rptr != result.wptr
		|| (result.vm_fault_status & 0xff) != 0) {
		dprintf("amdgpu: UVD stage %u status %#x power %#x ring %#x ptr %u/%u VM %#x page %#x\n",
			(unsigned)result.stage, (unsigned)result.uvd_status, (unsigned)result.power_status,
			(unsigned)result.ring_control, (unsigned)result.rptr, (unsigned)result.wptr,
			(unsigned)result.vm_fault_status, (unsigned)result.vm_fault_address);
	}
}

status_t
UvdEngine::Start(const amdgpu::FirmwareView& firmware)
{
	auto write = [&](uint32 reg, uint32 value) { regs[reg] = value; (void)regs[reg]; };
	// Initialize the board-qualified ATOM divider requests before UVD starts.
	// Initialize() has checked the PCI identity and complete ROM digest.
	amdgpu_smc_dump_uvd_clocks(regs);
	status_t clockStatus = amdgpu_smc_set_uvd_clocks(regs);
	if (clockStatus != B_OK)
		return clockStatus;
	write(mmUVD_POWER_STATUS, UVD_POWER_STATUS__UVD_PG_EN_MASK);
	write(mmUVD_POWER_STATUS, regs[mmUVD_POWER_STATUS] & ~UVD_POWER_STATUS__UVD_PG_MODE_MASK);
	// Disable main gates while the VCPU is brought up. SUVD sub-block gate
	// setup is the same as Linux's ungated UVD6 path.
	uint32 suvd = UVD_SUVD_CGC_GATE__SRE_MASK | UVD_SUVD_CGC_GATE__SIT_MASK
		| UVD_SUVD_CGC_GATE__SMP_MASK | UVD_SUVD_CGC_GATE__SCM_MASK
		| UVD_SUVD_CGC_GATE__SDB_MASK | UVD_SUVD_CGC_GATE__SRE_H264_MASK
		| UVD_SUVD_CGC_GATE__SRE_HEVC_MASK | UVD_SUVD_CGC_GATE__SIT_H264_MASK
		| UVD_SUVD_CGC_GATE__SIT_HEVC_MASK | UVD_SUVD_CGC_GATE__SCM_H264_MASK
		| UVD_SUVD_CGC_GATE__SCM_HEVC_MASK | UVD_SUVD_CGC_GATE__SDB_H264_MASK
		| UVD_SUVD_CGC_GATE__SDB_HEVC_MASK;
	write(mmUVD_SUVD_CGC_GATE, regs[mmUVD_SUVD_CGC_GATE] | suvd);
	write(mmUVD_CGC_GATE, 0);
	// Leave dynamic clock gating disabled during qualification.
	write(mmUVD_CGC_CTRL, regs[mmUVD_CGC_CTRL] & ~UVD_CGC_CTRL__DYN_CLOCK_MODE_MASK);
	write(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW, (uint32)gpu);
	write(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH, gpu >> 32);
	uint32 end = (firmware.codeSize + 8 + 4095) & ~4095u;
	write(mmUVD_VCPU_CACHE_OFFSET0, 256 / 8);
	write(mmUVD_VCPU_CACHE_SIZE0, end - 256);
	write(mmUVD_VCPU_CACHE_OFFSET1, end / 8);
	write(mmUVD_VCPU_CACHE_SIZE1, 256 * 1024);
	write(mmUVD_VCPU_CACHE_OFFSET2, (end + 256 * 1024) / 8);
	write(mmUVD_VCPU_CACHE_SIZE2, 200 * 1024 + 50 * 1024 * 40);
	write(mmUVD_UDEC_ADDR_CONFIG, 0x22011003);
	write(mmUVD_UDEC_DB_ADDR_CONFIG, 0x22011003);
	write(mmUVD_UDEC_DBW_ADDR_CONFIG, 0x22011003);
	write(mmUVD_GP_SCRATCH4, 40);
	// Polling only: both interrupt sources stay disabled.
	write(mmUVD_MASTINT_EN, regs[mmUVD_MASTINT_EN]
		& ~(UVD_MASTINT_EN__VCPU_EN_MASK | UVD_MASTINT_EN__SYS_EN_MASK));
	write(mmUVD_LMI_CTRL2, regs[mmUVD_LMI_CTRL2] | UVD_LMI_CTRL2__STALL_ARB_UMC_MASK);
	snooze(1000);
	write(mmUVD_SOFT_RESET, UVD_SOFT_RESET__LMI_SOFT_RESET_MASK
		| UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK | UVD_SOFT_RESET__LBSI_SOFT_RESET_MASK
		| UVD_SOFT_RESET__RBC_SOFT_RESET_MASK | UVD_SOFT_RESET__CSM_SOFT_RESET_MASK
		| UVD_SOFT_RESET__CXW_SOFT_RESET_MASK | UVD_SOFT_RESET__TAP_SOFT_RESET_MASK
		| UVD_SOFT_RESET__LMI_UMC_SOFT_RESET_MASK);
	snooze(5000);
	write(mmSRBM_SOFT_RESET, regs[mmSRBM_SOFT_RESET] & ~SRBM_SOFT_RESET__SOFT_RESET_UVD_MASK);
	snooze(5000);
	write(mmUVD_LMI_CTRL, (0x40 << UVD_LMI_CTRL__WRITE_CLEAN_TIMER__SHIFT)
		| UVD_LMI_CTRL__WRITE_CLEAN_TIMER_EN_MASK | UVD_LMI_CTRL__DATA_COHERENCY_EN_MASK
		| UVD_LMI_CTRL__VCPU_DATA_COHERENCY_EN_MASK | UVD_LMI_CTRL__REQ_MODE_MASK
		| UVD_LMI_CTRL__DISABLE_ON_FWV_FAIL_MASK);
	write(mmUVD_LMI_SWAP_CNTL, 0);
	write(mmUVD_MP_SWAP_CNTL, 0);
	write(mmUVD_MPC_SET_MUXA0, 0x40c2040);
	write(mmUVD_MPC_SET_MUXA1, 0);
	write(mmUVD_MPC_SET_MUXB0, 0x40c2040);
	write(mmUVD_MPC_SET_MUXB1, 0);
	write(mmUVD_MPC_SET_ALU, 0);
	write(mmUVD_MPC_SET_MUX, 0x88);
	write(mmUVD_SOFT_RESET, UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK);
	snooze(5000);
	write(mmUVD_VCPU_CNTL, UVD_VCPU_CNTL__CLK_EN_MASK);
	write(mmUVD_LMI_CTRL2, regs[mmUVD_LMI_CTRL2] & ~UVD_LMI_CTRL2__STALL_ARB_UMC_MASK);
	write(mmUVD_SOFT_RESET, 0);
	bigtime_t deadline = system_time() + 1000000;
	while ((regs[mmUVD_STATUS] & 2) == 0) {
		if (system_time() >= deadline)
			return B_TIMED_OUT;
		snooze(1000);
	}
	write(mmUVD_STATUS, regs[mmUVD_STATUS] & ~(2 << UVD_STATUS__VCPU_REPORT__SHIFT));
	uint32 control = (12 << UVD_RBC_RB_CNTL__RB_BUFSZ__SHIFT)
		| (1 << UVD_RBC_RB_CNTL__RB_BLKSZ__SHIFT)
		| UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK | UVD_RBC_RB_CNTL__RB_NO_UPDATE_MASK
		| UVD_RBC_RB_CNTL__RB_RPTR_WR_EN_MASK;
	write(mmUVD_RBC_RB_CNTL, control);
	write(mmUVD_RBC_RB_WPTR_CNTL, 0);
	write(mmUVD_RBC_RB_RPTR_ADDR, (gpu + kRing) >> 34);
	write(mmUVD_LMI_RBC_RB_64BIT_BAR_LOW, (uint32)(gpu + kRing));
	write(mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH, (gpu + kRing) >> 32);
	write(mmUVD_RBC_RB_RPTR, 0);
	write(mmUVD_RBC_RB_WPTR, 0);
	wptr = 0;
	write(mmUVD_RBC_RB_CNTL, control & ~UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK);
	amdgpu_smc_dump_uvd_clocks(regs);
	return B_OK;
}

status_t
UvdEngine::TimeoutTest(amdgpu_uvd_timeout_test& result)
{
	if (!ready || faulted || area < 0) return B_DEV_NOT_READY;
	volatile uint32* ib = memory + kIB / 4;
	ib[0] = mmUVD_CONTEXT_ID; ib[1] = 0x52535454;
	for (uint32 i = 2; i < 16; i++) ib[i] = 0x80000000;
	regs[mmUVD_RBC_RB_CNTL] |= UVD_RBC_RB_CNTL__RB_NO_FETCH_MASK;
	(void)regs[mmUVD_RBC_RB_CNTL];
	amdgpu_uvd_test test = {};
	bigtime_t start = system_time();
	status_t status = Submit(16, test);
	result.elapsed_us = system_time() - start;
	result.sequence = test.sequence; result.fence = test.fence;
	result.rptr = test.rptr; result.wptr = test.wptr; result.uvd_status = test.uvd_status;
	// Even an unexpectedly retiring diagnostic leaves fetch disabled. Keep
	// ownership faulted until a complete, separately requested recovery.
	faulted = true;
	Stop();
	result.status = status == B_OK ? B_BAD_DATA : status;
	return result.status;
}

status_t
UvdEngine::Recover(const amdgpu::FirmwareView& firmware, amdgpu_recovery_info& result)
{
	if (!attempted || !faulted || area < 0 || regs == NULL || memory == NULL
		|| firmware.version != 0x01008210 || firmware.codeSize > kRing - 8)
		return B_DEV_NOT_READY;
	const uint32 mask = SRBM_SOFT_RESET__SOFT_RESET_UVD_MASK;
	uint32 reset = regs[mmSRBM_SOFT_RESET];
	if ((reset & mask) != 0) return B_NOT_ALLOWED;
	const uint32 heads[] = {0x1a00, 0x1c00, 0x1e00, 0x4000, 0x4200, 0x4400};
	const uint32 offsets[] = {1, 4, 6, 7, 0x19c};
	uint32 display[6][5];
	for (uint32 h = 0; h < 6; h++)
		for (uint32 i = 0; i < 5; i++) display[h][i] = regs[heads[h] + offsets[i]];
	result.stage = 8;
	Stop();
	// Linux 6.18.52 uvd_v6_0_pre/soft/post_soft_reset and OSS3's UVD
	// mask. All old session addresses remain owned throughout this reset.
	dprintf("amdgpu: UVD recovery before status %#x SRBM %#x ring %u/%u\n",
		(unsigned)regs[mmUVD_STATUS], (unsigned)regs[0x394],
		(unsigned)regs[mmUVD_RBC_RB_RPTR], (unsigned)regs[mmUVD_RBC_RB_WPTR]);
	result.srbm_reset |= mask;
	regs[mmSRBM_SOFT_RESET] = reset | mask; (void)regs[mmSRBM_SOFT_RESET]; snooze(50);
	regs[mmSRBM_SOFT_RESET] = reset; (void)regs[mmSRBM_SOFT_RESET]; snooze(50);
	bigtime_t deadline = system_time() + 100000;
	while ((regs[0x394] & 0x80002) != 0 && system_time() < deadline) snooze(10);
	if ((regs[0x394] & 0x80002) != 0 || regs[mmSRBM_SOFT_RESET] != reset)
		return B_DEV_NOT_READY;
	// The reset has quiesced UVD. Erase the reserved VCPU session tables and
	// reload firmware; client-owned DPB/output/readback storage stays intact.
	result.stage = 9;
	for (uint32 i = 0; i < kBytes / 4; i++) memory[i] = 0;
	Copy(memory, 0, firmware.code, firmware.codeSize);
	__sync_synchronize();
	regs[0x1520] = 1; (void)regs[0x1520]; snooze(5000);
	status_t status = Start(firmware);
	if (status == B_OK) {
		result.stage = 10;
		sequence = 0;
		regs[mmUVD_CONTEXT_ID] = 0xcafedead;
		memory[kIB / 4] = mmUVD_CONTEXT_ID;
		memory[kIB / 4 + 1] = 0x52535455;
		for (uint32 i = 2; i < 16; i++) memory[kIB / 4 + i] = 0x80000000;
		amdgpu_uvd_test proof = {};
		status = Submit(16, proof);
		if (status == B_OK && (proof.sequence != 1 || proof.fence != 1
			|| proof.context != 1 || proof.rptr != proof.wptr)) status = B_BAD_DATA;
	}
	for (uint32 h = 0; status == B_OK && h < 6; h++)
		for (uint32 i = 0; i < 5; i++)
			if (regs[heads[h] + offsets[i]] != display[h][i]) status = B_BAD_DATA;
	if (status == B_OK) {
		faulted = false; ready = true;
		dprintf("amdgpu: UVD reset firmware/ring/display proof passed; old sessions invalid\n");
	} else {
		faulted = true; Stop();
	}
	return status;
}

status_t
UvdEngine::Submit(uint32 words, amdgpu_uvd_test& result)
{
	volatile uint32* ring = memory + kRing / 4;
	auto emit = [&](uint32 value) { ring[wptr++ & 1023] = value; };
	auto set = [&](uint32 reg, uint32 value) { emit(reg); emit(value); };
	memory[kFence / 4] = 0;
	uint32 fence = ++sequence;
	set(mmUVD_LMI_RBC_IB_VMID, 0);
	set(mmUVD_LMI_RBC_IB_64BIT_BAR_LOW, (uint32)(gpu + kIB));
	set(mmUVD_LMI_RBC_IB_64BIT_BAR_HIGH, (gpu + kIB) >> 32);
	set(mmUVD_RBC_IB_SIZE, words);
	set(mmUVD_CONTEXT_ID, fence);
	set(mmUVD_GPCOM_VCPU_DATA0, (uint32)(gpu + kFence));
	set(mmUVD_GPCOM_VCPU_DATA1, (gpu + kFence) >> 32);
	set(mmUVD_GPCOM_VCPU_CMD, 0); // firmware writes the fence; no interrupt trap
	while ((wptr & 15) != 0)
		emit(0x80000000); // PACKET2
	__sync_synchronize();
	regs[0x1520] = 1;
	(void)regs[0x1520];
	regs[mmUVD_RBC_RB_WPTR] = wptr & 1023;
	bigtime_t deadline = system_time() + 1000000;
	do {
		regs[0xbcc] = 1;
		(void)regs[0xbcc];
		__sync_synchronize();
		if (memory[kFence / 4] == fence && regs[mmUVD_RBC_RB_RPTR] == (wptr & 1023))
			break;
		snooze(100);
	} while (system_time() < deadline);
	result.fence = memory[kFence / 4];
	result.sequence = fence;
	Snapshot(result);
	if (result.fence != fence || result.rptr != result.wptr)
		return B_TIMED_OUT;
	return ((regs[0x536] | regs[0x537]) & 0xff) == 0 ? B_OK : B_BAD_DATA;
}

status_t
UvdEngine::Initialize(volatile uint32* r, const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation, bool clocksQualified,
	const amdgpu::FirmwareView& firmware, amdgpu_uvd_test& result)
{
	regs = r;
	result.stage = 1;
	result.firmware_version = firmware.version;
	Snapshot(result);
	if (faulted || (attempted && !ready))
		return B_DEV_NOT_READY;
	status_t status = B_OK;
	if (!ready) {
		if (!clocksQualified || info.vendor != 0x1002 || info.device != 0x67c7
			|| info.subsystem_vendor != 0x1028 || info.subsystem_device != 0x0b0d
			|| info.revision != 0 || firmware.version != 0x01008210)
			return B_NOT_SUPPORTED;
		const uint64 offset = 44ULL << 20;
		uint32 end = ((firmware.codeSize + 8 + 4095) & ~4095u)
			+ 256 * 1024 + 200 * 1024 + 50 * 1024 * 40;
		if (!amdgpu_smc_ready(r) || r[mmUVD_STATUS] != 0
			|| r[mmUVD_RBC_RB_RPTR] != 0 || r[mmUVD_RBC_RB_WPTR] != 0
			|| end > kRing || !amdgpu_vram_range_is_safe(r, info, reservation, offset, kBytes))
			return B_NOT_ALLOWED;
		area = map_physical_memory("amdgpu UVD private memory", info.bar_address[0] + offset,
			kBytes, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			(void**)&memory);
		if (area < 0)
			return area;
		gpu = info.vram_gpu_base + offset;
		for (uint32 i = 0; i < kBytes / 4; i++)
			memory[i] = 0;
		Copy(memory, 0, firmware.code, firmware.codeSize);
		__sync_synchronize();
		r[0x1520] = 1;
		(void)r[0x1520];
		result.stage = 2;
		attempted = true;
		status = Start(firmware);
		Snapshot(result);
		if (status == B_OK) {
			result.stage = 3;
			r[mmUVD_CONTEXT_ID] = 0xcafedead;
			memory[kIB / 4] = mmUVD_CONTEXT_ID;
			memory[kIB / 4 + 1] = 0x13579bdf;
			for (uint32 i = 2; i < 16; i++)
				memory[kIB / 4 + i] = 0x80000000;
			status = Submit(16, result);
			ready = status == B_OK;
		}
	}
	if (status != B_OK) { faulted = true; Stop(); }
	return status;
}

status_t
UvdEngine::Test(volatile uint32* r, const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation, bool clocksQualified,
	const amdgpu::FirmwareView& firmware, amdgpu_uvd_test& result, void* output)
{
	status_t status = Initialize(r, info, reservation, clocksQualified, firmware, result);
	if (status == B_OK) {
		for (uint32 i = kMessage / 4; i < kBytes / 4; i++)
			memory[i] = 0;
		Copy(memory, kMessage, uvd_create_msg, sizeof(uvd_create_msg));
		memory[(kMessage + 0x10) / 4] = 7; // H.264 performance stream
		memory[(kMessage + 0x28) / 4] = kPictureDPBBytes;
		volatile uint32* ib = memory + kIB / 4;
		uint32 count = 0;
		auto command = [&](uint32 offset, uint32 cmd) {
			uint64 address = gpu + offset;
			ib[count++] = mmUVD_GPCOM_VCPU_DATA0; ib[count++] = (uint32)address;
			ib[count++] = mmUVD_GPCOM_VCPU_DATA1; ib[count++] = address >> 32;
			ib[count++] = mmUVD_GPCOM_VCPU_CMD; ib[count++] = cmd << 1;
		};
		auto pad = [&]() { while ((count & 15) != 0) ib[count++] = 0x80000000; };
		result.stage = 4;
		command(kMessage, 0);
		pad();
		status = Submit(count, result);
		if (status == B_OK) {
			Copy(memory, kMessage, uvd_decode_msg, sizeof(uvd_decode_msg));
			Copy(memory, kMessage + sizeof(uvd_decode_msg), avc_decode_msg, sizeof(avc_decode_msg));
			memory[(kMessage + 0x10) / 4] = 7;
			memory[(kMessage + 0x24) / 4] = kPictureDPBBytes;
			memory[(kMessage + 0x2c) / 4] = kContextBytes;
			memory[(kMessage + 0x98) / 4] = (memory[(kMessage + 0x98) / 4] & 0xffff0000) | 0x200;
			Copy(memory, kScaling, uvd_it_scaling_table, sizeof(uvd_it_scaling_table));
			Copy(memory, kBitstream, uvd_bitstream, sizeof(uvd_bitstream));
			for (uint32 offset : kGuards)
				for (uint32 i = 0; i < 1024; i++)
					memory[offset / 4 + i] = 0xabcddcba;
			count = 0;
			command(kMessage, 0); command(kDPB, 1); command(kTarget, 2);
			command(kFeedback, 3); command(kBitstream, 0x100); command(kScaling, 0x204);
			command(kContext, 0x206);
			ib[count++] = mmUVD_ENGINE_CNTL; ib[count++] = 1;
			pad();
			result.stage = 5;
			status = Submit(count, result);
			if (status == B_OK) {
				regs[0xbcc] = 1;
				(void)regs[0xbcc];
				__sync_synchronize();
				for (uint32 i = 0; i < kTargetBytes / 4; i++)
					((uint32*)output)[i] = memory[kTarget / 4 + i];
				result.checked_bytes = kTargetBytes;
				for (uint32 i = 0; i < kTargetBytes; i++)
					result.checksum += ((uint8*)output)[i];
				for (uint32 offset : kGuards)
					for (uint32 i = 0; i < 1024; i++)
						result.guard_mismatches += memory[offset / 4 + i] != 0xabcddcba;
				for (uint32 i = 0; i < 8; i++)
					result.feedback[i] = memory[kFeedback / 4 + i];
				// Destroy even if pixel verification fails, after a completed decode.
				Copy(memory, kMessage, uvd_destroy_msg, sizeof(uvd_destroy_msg));
				count = 0; command(kMessage, 0); pad();
				result.stage = 6;
				status = Submit(count, result);
				if (status == B_OK && (result.checksum != SUM_DECODE || result.guard_mismatches != 0))
					status = B_BAD_DATA;
			}
		}
	}
	if (status == B_OK)
		result.stage = 7;
	else {
		faulted = true;
		Stop();
	}
	Snapshot(result);
	return status;
}

void
UvdEngine::Stop()
{
	regs[mmUVD_RBC_RB_CNTL] = 0x11010101;
	regs[mmUVD_LMI_CTRL2] |= UVD_LMI_CTRL2__STALL_ARB_UMC_MASK;
	(void)regs[mmUVD_LMI_CTRL2];
	snooze(1000);
	regs[mmUVD_SOFT_RESET] = UVD_SOFT_RESET__VCPU_SOFT_RESET_MASK;
	(void)regs[mmUVD_SOFT_RESET];
	snooze(5000);
	regs[mmUVD_VCPU_CNTL] = 0;
	regs[mmUVD_LMI_CTRL2] &= ~UVD_LMI_CTRL2__STALL_ARB_UMC_MASK;
	regs[mmUVD_STATUS] = 0;
	(void)regs[mmUVD_STATUS];
	ready = false;
}

void
UvdEngine::Uninitialize()
{
	if (!attempted)
		return;
	Stop();
	if (area >= 0)
		delete_area(area);
	// Fixed kernel-reserved VRAM is never returned to client allocations.
	area = -1;
}
