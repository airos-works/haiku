/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Uvd.h"
#include "UvdRegisters.h"
#include <KernelExport.h>
#include <string.h>

namespace {
static void Copy(volatile uint32* memory, uint32 offset, const uint8* data, uint32 bytes)
{
	for (uint32 i = 0; i < bytes; i += 4) {
		uint32 value = 0;
		for (uint32 j = 0; j < 4 && i + j < bytes; j++) value |= (uint32)data[i + j] << (8 * j);
		memory[(offset + i) / 4] = value;
	}
}
}

status_t
UvdEngine::Session(UvdSession& s, uint32 type, const void* picture,
	const void* bitstream, uint32 bytes, SdmaEngine& dma, amdgpu_uvd_test& result)
{
	if (!ready || faulted) return B_DEV_NOT_READY;
	if (type > 2 || (type == 0) == s.created) return B_BAD_VALUE;
	const auto* avc = static_cast<const amdgpu_h264_picture*>(picture);
	const auto* hevc = static_cast<const amdgpu_hevc_picture*>(picture);
	uint32 hevcNextSlots = 0;
	if (type == 1) {
		if (picture == NULL || bitstream == NULL || !s.readbackBound || s.frames == UINT32_MAX)
			return B_BAD_VALUE;
		if (s.hevc) {
			if (!amdgpu::UvdHevcValidate(s.hevcConfig, *hevc, bytes)
				|| !amdgpu::UvdHevcBitstream((const uint8*)bitstream, bytes, hevc->nal_type))
				return B_BAD_VALUE;
			// A private DPB slot must have completed in this session before
			// it can be referenced. First-picture random access has no DPB.
			if (!amdgpu::UvdHevcNextReferences(*hevc, s.hevcReferenceSlots,
				s.frames == 0, hevcNextSlots))
				return B_BAD_VALUE;
		} else if (!amdgpu::UvdH264Validate(s.config, *avc, bytes)
			|| !amdgpu::UvdH264Bitstream((const uint8*)bitstream, bytes, (avc->flags & AMDGPU_H264_IDR) != 0)
			|| (s.frames == 0 && (avc->flags & AMDGPU_H264_IDR) == 0))
			return B_BAD_VALUE;
	}
	const auto& l = s.layout;
	const uint64 guardReadback = 4096 + ((l.outputBytes + 4095ULL) & ~4095ULL);
	auto timeout = [&](uint32 stage) {
		s.sdmaTimeoutStage = 0;
		amdgpu_sdma_timeout_test test = {};
		status_t status = dma.TimeoutTest(test);
		dprintf("amdgpu: UVD session %u SDMA timeout stage %u status 0x%x "
			"DMA fence %u/%u ring %u/%u elapsed %llu us UVD fence %u/%u\n",
			(unsigned)s.handle, (unsigned)stage, (unsigned)status,
			(unsigned)test.fence, (unsigned)test.sequence, (unsigned)test.rptr,
			(unsigned)test.wptr, (unsigned long long)test.elapsed_us,
			(unsigned)result.fence, (unsigned)result.sequence);
		return status;
	};
	bigtime_t timing[6]; timing[0] = system_time();
	if (type == 1) {
		status_t status = s.sdmaTimeoutStage == AMDGPU_VIDEO_SDMA_CLEAR
			? timeout(AMDGPU_VIDEO_SDMA_CLEAR)
			: dma.Execute(AMDGPU_DMA_FILL, 0, s.gpu + l.target, l.outputBytes, 0);
		if (status != B_OK) { faulted = true; Stop(); return status; }
		// A different CPU fill each frame also makes a short/stale DMA copy
		// visible to the independent pixel comparison. Guard pages stay fixed.
		memset((uint8*)s.readback + 4096, (s.frames & 1) ? 0x5a : 0xa5, l.outputBytes);
		// Poison every mirror before copying. A stale or incomplete guard
		// readback must fail rather than reuse a previous frame's good data.
		memset((uint8*)s.readback + guardReadback, 0x6b, UvdSession::kGuardReadbackBytes);
	}
	timing[1] = system_time();
	uint8 message[amdgpu::kUvdMessageBytes];
	if (s.hevc) {
		if (!amdgpu::UvdHevcMessage(message, type, s.handle, s.hevcConfig, hevc, bytes, s.frames + 1))
			return B_BAD_VALUE;
	} else
		amdgpu::UvdH264Message(message, type, s.handle, s.config, l, avc, bytes, s.frames + 1);
	Copy(s.cpu, l.message, message, sizeof(message));
	if (type == 0) {
		for (uint32 offset : l.guards)
			for (uint32 i = 0; i < 1024; i++) s.cpu[offset / 4 + i] = 0xabcddcba;
	}
	if (type == 1) {
		for (uint32 i = 0; i < 4096 / 4; i++) s.cpu[l.feedback / 4 + i] = 0;
		s.cpu[l.feedback / 4] = 4096;
		Copy(s.cpu, l.bitstream, (const uint8*)bitstream, bytes);
		for (uint32 i = (bytes + 3) / 4; i < ((bytes + 127) & ~127u) / 4; i++)
			s.cpu[l.bitstream / 4 + i] = 0;
		if (s.hevc) {
			uint8 scaling[amdgpu::kUvdHevcScalingBytes];
			amdgpu::UvdHevcScaling(scaling, *hevc);
			Copy(s.cpu, l.scaling, scaling, sizeof(scaling));
		} else {
			Copy(s.cpu, l.scaling, &avc->scaling4x4[0][0], 96);
			Copy(s.cpu, l.scaling + 96, &avc->scaling8x8[0][0], 128);
		}
	}
	volatile uint32* ib = memory + ((3 << 20) + 0x11000) / 4;
	uint32 count = 0;
	auto command = [&](uint32 offset, uint32 cmd) {
		uint64 address = s.gpu + offset;
		ib[count++] = mmUVD_GPCOM_VCPU_DATA0; ib[count++] = (uint32)address;
		ib[count++] = mmUVD_GPCOM_VCPU_DATA1; ib[count++] = address >> 32;
		ib[count++] = mmUVD_GPCOM_VCPU_CMD; ib[count++] = cmd << 1;
	};
	command(l.session, 5); command(l.message, 0);
	if (type == 1) {
		command(l.dpb, 1); command(l.context, 0x206); command(l.bitstream, 0x100);
		command(l.target, 2); command(l.feedback, 3); command(l.scaling, 0x204);
		ib[count++] = mmUVD_ENGINE_CNTL; ib[count++] = 1;
	}
	while ((count & 15) != 0) ib[count++] = 0x80000000;
	result.stage = type == 0 ? 4 : type == 1 ? 5 : 6;
	timing[2] = system_time();
	status_t status = Submit(count, result);
	timing[3] = system_time();
	if (status == B_OK && type == 1) {
		// One retired DMA batch publishes the output and all four private
		// guard pages into cached RAM. Avoid thousands of PCI BAR reads per
		// frame while preserving the complete guard comparison below.
		SdmaCopy copies[5] = {{s.gpu + l.target, s.readbackGpu + 4096, l.outputBytes}};
		for (uint32 guard = 0; guard < 4; guard++)
			copies[guard + 1] = {s.gpu + l.guards[guard],
				s.readbackGpu + guardReadback + guard * 4096, 4096};
		status = s.sdmaTimeoutStage == AMDGPU_VIDEO_SDMA_READBACK
			? timeout(AMDGPU_VIDEO_SDMA_READBACK) : dma.CopyRegions(copies, 5);
		if (status == B_OK) {
			const uint8* ram = (const uint8*)s.readback;
			for (uint32 i = 0; i < 4096; i++)
				result.guard_mismatches += ram[i] != 0x7d;
			for (uint64 i = 4096 + l.outputBytes; i < guardReadback; i++)
				result.guard_mismatches += ram[i] != 0x7d;
			for (uint64 i = guardReadback + UvdSession::kGuardReadbackBytes; i < s.readbackBytes; i++)
				result.guard_mismatches += ram[i] != 0x7d;
		}
	}
	timing[4] = system_time();
	if (status == B_OK) {
		const uint32* mirror = type == 1
			? (const uint32*)((const uint8*)s.readback + guardReadback) : NULL;
		for (uint32 guard = 0; guard < 4; guard++) {
			uint32 mismatches = 0, first = 0, firstValue = 0;
			uint32 offset = l.guards[guard];
			for (uint32 i = 0; i < 1024; i++) {
				uint32 value = type == 1 ? mirror[guard * 1024 + i] : s.cpu[offset / 4 + i];
				if (value == 0xabcddcba) continue;
				if (mismatches++ == 0) { first = i; firstValue = value; }
			}
			result.guard_mismatches += mismatches;
			if (mismatches != 0)
				dprintf("amdgpu: UVD session %u guard %u offset 0x%x "
					"words %u first +0x%x value 0x%x\n", (unsigned)s.handle,
					(unsigned)guard, (unsigned)offset, (unsigned)mismatches,
					(unsigned)(first * 4), (unsigned)firstValue);
		}
		if (result.guard_mismatches != 0) status = B_BAD_DATA;
	}
	timing[5] = system_time();
	if (status == B_OK) {
		if (type == 1) {
			for (uint32 i = 0; i < 8; i++) result.feedback[i] = s.cpu[l.feedback / 4 + i];
			result.checked_bytes = l.outputBytes;
			s.frames++;
			if (s.hevc) s.hevcReferenceSlots = hevcNextSlots;
			for (unsigned i = 0; i < 5; i++) s.timingTotal[i] += timing[i + 1] - timing[i];
		} else {
			s.created = type == 0;
			if (type == 2 && s.frames != 0) {
				dprintf("amdgpu: UVD session %u %ux%u %u frames mean us clear %lld "
					"upload %lld submit %lld readback %lld guards %lld\n",
					(unsigned)s.handle, (unsigned)(s.hevc ? s.hevcConfig.width : s.config.width),
					(unsigned)(s.hevc ? s.hevcConfig.height : s.config.height),
					(unsigned)s.frames, (long long)(s.timingTotal[0] / s.frames),
					(long long)(s.timingTotal[1] / s.frames), (long long)(s.timingTotal[2] / s.frames),
					(long long)(s.timingTotal[3] / s.frames), (long long)(s.timingTotal[4] / s.frames));
			}
		}
	} else {
		if (result.guard_mismatches != 0)
			dprintf("amdgpu: UVD session %u guard failures %u\n", (unsigned)s.handle,
				(unsigned)result.guard_mismatches);
		faulted = true;
		Stop();
	}
	return status;
}
