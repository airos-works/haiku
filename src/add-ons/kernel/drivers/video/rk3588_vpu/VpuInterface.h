/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef RK3588_VPU_INTERFACE_H
#define RK3588_VPU_INTERFACE_H

#include <stdint.h>
#include <string.h>

namespace RK3588Vpu {

static const uint32_t kGetResources = 0x52565000;
static const uint32_t kGetSnapshot = 0x52565001;
static const uint32_t kCycleVdpuPower = 0x52565002;
static const uint32_t kProbeDecoder = 0x52565003;
static const uint32_t kProbeDma = 0x52565004;
static const uint32_t kCycleRkvdec0Power = 0x52565005;
static const uint32_t kCycleRkvdec0WithVdpu = 0x52565006;
static const uint32_t kProbeRkvdec0Registers = 0x52565007;
static const uint32_t kAllocBuffer = 0x52565008;
static const uint32_t kFreeBuffer = 0x52565009;
static const uint32_t kValidateBuffer = 0x5256500a;
// Rockchip MPP's Linux V1 request number (_IOW('v', 1, unsigned int)).
// The driver implements the query, client-init, register, buffer and polling
// requests used by the pinned Haiku MPP port.
static const uint32_t kMppServiceV1 = 0x40047601;
static const uint32_t kGetMppValidation = 0x5256500b;
static const uint32_t kCycleAv1WithVdpu = 0x5256500c;
static const uint32_t kProbeAv1Registers = 0x5256500d;
static const uint32_t kResourceVersion = 1;
static const uint32_t kSnapshotVersion = 4;
static const uint32_t kPowerCycleVersion = 1;
static const uint32_t kDecoderProbeVersion = 2;
static const uint32_t kDmaProbeVersion = 1;
static const uint32_t kBufferVersion = 1;

enum PowerResult : uint32_t {
	kPowerOK = 0,
	kPowerInvalidState = 1,
	kPowerOnTimeout = 2,
	kPowerIdleTimeout = 3,
	kPowerOffTimeout = 4,
	kPowerRestoreMismatch = 5,
	kPowerMemoryTimeout = 6,
};

// Pointer-free diagnostic ABI. No VPU registers are mapped by these requests.
struct ResourceInfo {
	uint32_t version;
	uint32_t flags;
	uint64_t decoderBase;
	uint64_t decoderSize;
	uint64_t iommuBase;
	uint64_t iommuSize;
	uint64_t clockBase;
	uint64_t clockSize;
	uint64_t powerBase;
	uint64_t powerSize;
	uint64_t interruptBase;
	uint32_t decoderInterrupt;
	uint32_t iommuInterrupt;
	uint32_t clockIds[2];
	uint32_t powerDomain;
};

struct Snapshot {
	uint32_t version;
	uint32_t flags;
	int64_t startedMicros;
	int64_t finishedMicros;
	uint32_t idleRequest;
	uint32_t idleAck;
	uint32_t idleStatus;
	uint32_t powerGate;
	uint32_t powerStatus;
	uint32_t repairStatus;
	uint32_t chainStatus;
	uint32_t memoryStatus;
	uint32_t clockSelect98;
	uint32_t clockGate40;
	uint32_t clockGate41;
	uint32_t clockGate44;
	uint32_t clockGate45;
	uint32_t clockGate68;
	uint32_t clockSelect89;
	uint32_t clockSelect90;
	uint32_t clockSelect91;
	uint32_t clockSelect163;
	uint32_t softReset68;
};

struct PowerCycle {
	uint32_t version;
	uint32_t result;
	uint32_t restoreResult;
	uint32_t flags;
	Snapshot before;
	Snapshot powered;
	Snapshot after;
};

struct ParentChildCycle {
	uint32_t version;
	PowerCycle parent;
	PowerCycle child;
};

struct Rkvdec0RegisterProbe {
	uint32_t version;
	int32_t readStatus;
	uint32_t link[2];
	uint32_t function[2];
	uint32_t interruptStatus;
	uint32_t iommuDte;
	uint32_t iommuStatus;
	uint32_t iommuFault;
	ParentChildCycle cycle;
};

struct Av1RegisterProbe {
	uint32_t version;
	int32_t readStatus;
	uint32_t registers[3];
	uint32_t buildId;
	uint32_t synthesisId;
	ParentChildCycle cycle;
};

struct DecoderProbe {
	uint32_t version;
	int32_t readStatus;
	uint32_t decoderId;
	uint32_t iommuDte;
	uint32_t iommuStatus;
	uint32_t iommuInterruptRaw;
	uint32_t iommuInterruptMask;
	PowerCycle cycle;
};

struct DmaProbe {
	uint32_t version;
	int32_t status;
	uint64_t physical;
	uint64_t bytes;
};

// A device handle, not a physical address, is passed to future decode jobs.
// The cloned area belongs to the process that opened the device.
struct BufferAllocation {
	uint32_t version;
	uint32_t handle;
	uint64_t bytes;
	int32_t area;
	uint32_t reserved;
	uint64_t address;
};

struct BufferRelease {
	uint32_t version;
	uint32_t handle;
};

struct BufferValidation {
	uint32_t version;
	uint32_t handle;
	uint64_t offset;
	uint64_t bytes;
};

struct MppServiceRequest {
	uint32_t command;
	uint32_t flags;
	uint32_t bytes;
	uint32_t offset;
	uint64_t dataAddress;
};

static const uint32_t kMppQueryHardware = 0x000;
static const uint32_t kMppQueryId = 0x001;
static const uint32_t kMppInitClient = 0x100;
static const uint32_t kMppAttachBuffer = 0x401;
static const uint32_t kMppDetachBuffer = 0x402;
static const uint32_t kMppClientRkvdec = 9;
static const uint32_t kMppClientAv1 = 4;
static const uint32_t kRkvdec0HardwareId = 0x53813f05;
static const uint32_t kAv1HardwareId = 0x80019000;
// RKVENC (VEPU580), H.264 and H.265 encoding; MPP's HWID_VEPU58X.
static const uint32_t kMppClientRkvenc = 16;
static const uint32_t kRkvencHardwareId = 0x50603312;
static const uint32_t kAv1BuildId = 0x00001f8e;

struct MppValidationStats {
	uint32_t version;
	uint32_t jobs;
	uint32_t writeRequests;
	uint32_t readRequests;
	uint32_t offsetRequests;
	uint32_t rcbRequests;
	int32_t lastStatus;
	uint32_t addressHandles;
	uint32_t offsetEntries;
	uint32_t reserved;
};

inline bool
ResourcesMatch(const ResourceInfo& info)
{
	return info.version == kResourceVersion && info.flags == 1
		&& info.decoderBase == 0xfdb50000 && info.decoderSize == 0x800
		&& info.iommuBase == 0xfdb50800 && info.iommuSize == 0x40
		&& info.clockBase == 0xfd7c0000 && info.clockSize == 0x5c000
		&& info.powerBase == 0xfd8d8000 && info.powerSize == 0x400
		&& info.interruptBase == 0xfe600000
		&& info.decoderInterrupt == 151 && info.iommuInterrupt == 150
		&& info.clockIds[0] == 433 && info.clockIds[1] == 434
		&& info.powerDomain == 21;
}

} // namespace RK3588Vpu

#endif
