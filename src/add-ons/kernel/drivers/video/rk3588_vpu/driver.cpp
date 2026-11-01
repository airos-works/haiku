/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <bus/FDT.h>
#include <KernelExport.h>
#include <AutoDeleterOS.h>
#include <driver_settings.h>
#include <lock.h>
#include <smp.h>
#include <team.h>
#include <util/AutoLock.h>
#include <vm/vm.h>
#include <arch/arm64/cache_line_size.h>
#include <fcntl.h>
#include <new>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "VpuInterface.h"
#include "VpuPower.h"

using namespace RK3588Vpu;

#define DRIVER_NAME "drivers/video/rk3588_vpu/driver_v1"
#define DEVICE_NAME "drivers/video/rk3588_vpu/device_v1"

static device_manager_info* sDeviceManager;
static mutex sHardwareLock = MUTEX_INITIALIZER("RK3588 VPU hardware");
static struct OpenHandle* sOpenHandles = NULL;
static uint32_t sNextBufferHandle = 0;

class PowerHardware;

struct Controller {
	device_node* node;
	ResourceInfo resources;
	bool cycleEnabled;
	bool rkvdec0CycleEnabled;
	bool av1CycleEnabled;
	bool decodeEnabled;
	bool av1DecodeEnabled;
	bool encodeEnabled;
	MppValidationStats mppStats;

	// Decoding sessions. Cycling the domains around every job cost tens of
	// milliseconds a picture; they now come up with the first job and go
	// down when the last client that decoded closes the device (or after a
	// job that failed, so the next one starts from a clean block).
	PowerHardware* hardware;
	PowerCycle vdpuSession;
	PowerCycle rkvdec0Session;
	PowerCycle av1Session;
	bool vdpuPowered;
	bool rkvdec0Powered;
	bool av1Powered;
	int32_t decodeClients;
	area_id rkvdec0Area;
	void* rkvdec0Registers;
	area_id av1Area;
	void* av1Registers;
	uint32_t jobs;
	bigtime_t jobTime;
	// Encoding sessions on RKVENC core 0, the same way: VENC0 comes up with
	// the first job and goes down with the last client that encoded.
	PowerCycle venc0Session;
	bool venc0Powered;
	int32_t encodeClients;
	area_id rkvenc0Area;
	void* rkvenc0Registers;
	uint32_t encodeJobs;
	bigtime_t encodeTime;
	bigtime_t slowestEncode;
	bigtime_t decodeTime;	// in the decoder, without cache maintenance
	bigtime_t slowestJob;

	// Picture buffers must be physically contiguous below 4 GiB. Asked for
	// one at a time, they stop fitting once a running desktop has broken
	// that memory up; a pool taken when the driver starts keeps them coming.
	area_id poolArea;
	uint8_t* poolAddress;
	uint64_t poolPhysical;
	size_t poolPages;
	uint8_t* poolMap;
};

struct MppPendingJob {
	uint32_t registers[512];
	uint32_t written[16];
	uint64_t readAddress;
	uint32_t readBytes;
	bool valid;
	// The buffers the job reads and writes, and whether the processor
	// shares them (cache maintenance before and after the job).
	struct DmaBuffer* buffers[96];
	bool shared[96];
	uint32_t bufferCount;
};


struct OpenHandle {
	OpenHandle* next;
	Controller* controller;
	team_id owner;
	bool write;
	bool privileged;
	uint32_t mppClientType;
	bool mppCompletionReady;
	uint64_t allocatedBytes;
	struct DmaBuffer* buffers;
	MppPendingJob mppJob;
	bool decodes;
	struct EncoderJob* encoderJob;	// for an RKVENC client
	bool encodes;
};

struct DmaBuffer {
	DmaBuffer* next;
	uint32_t handle;
	area_id kernelArea;
	area_id userArea;
	void* kernelAddress;
	uint64_t physical;
	size_t bytes;
	ssize_t poolPage;		// first page in the pool, or -1
};

static DmaBuffer* FindTeamBuffer(team_id owner, uint32_t handle);


// #pragma mark - RKVENC (VEPU580) jobs


// The register words MPP's VEPU580 H.264 and H.265 HALs write, by block:
// control, frame, rate control table, parameters (H.265 weights), RDO and
// scaling, OSD. The sizes are MPP's structures, the bases its
// VEPU580_*_OFFSET; Rockchip's BSP mpp_rkvenc2.c has the same classes.
struct EncoderRange { uint16_t first, end; };
static const EncoderRange kEncoderWrites[] = {
	{0, 24}, {160, 254}, {1024, 1081}, {1472, 1846}, {2048, 2855}, {3072, 3360}
};
// What it reads back: the interrupt status and the status block.
static const EncoderRange kEncoderReads[] = {{11, 12}, {4096, 4276}};
static const uint32_t kEncoderRegisters = 3360;
static const uint32_t kEncoderReadEnd = 4276;
static const uint32_t kEncoderStartRegister = 4;			// 0x0010, enc_strt
static const uint32_t kEncoderClearRegister = 5;			// 0x0014
static const uint32_t kEncoderInterruptMask = 9;			// 0x0024
static const uint32_t kEncoderInterruptClear = 10;		// 0x0028
static const uint32_t kEncoderInterruptStatus = 11;		// 0x002c
static const uint32_t kEncoderWatchdog = 14;				// 0x0038
static const uint32_t kEncoderDualCore = 193;			// 0x0304, dchs
static const uint32_t kEncoderDone = 1u << 0;
static const uint32_t kEncoderSafeClearDone = 1u << 2;
// bitstream overflow, response timeout, bus errors, watchdog
static const uint32_t kEncoderErrors = 0x1f0;
static const uint64_t kRkvenc0Base = 0xfdbd0000;
static const size_t kRkvenc0Bytes = 0x10000;		// its IOMMU is at +0xf000

struct EncoderJob {
	uint32_t registers[kEncoderRegisters];
	uint32_t written[(kEncoderRegisters + 31) / 32];
	struct {
		uint64_t address;
		uint32_t first;
		uint32_t count;
	} reads[4];
	uint32_t readCount;
	DmaBuffer* buffers[40];
	bool shared[40];
	uint32_t bufferCount;
	bool valid;
};


/*!	Whether a VEPU580 register holds a buffer (BSP mpp_rkvenc2.c's
	translation tables: frame registers 160-183, OSD 3092-3099). */
static bool
IsEncoderAddressRegister(uint32_t index)
{
	return (index >= 160 && index <= 183) || (index >= 3092 && index <= 3099);
}


/*!	Whether the processor shares that buffer: the source picture it wrote,
	the bitstream (top, bottom, read and start) and motion information it
	reads, the ROI maps it wrote. Reconstructed and reference pictures,
	their motion vectors, downscaled copies and loop filter rows pass only
	between jobs. */
static bool
IsSharedEncoderRegister(uint32_t index)
{
	return (index >= 160 && index <= 162) || (index >= 171 && index <= 175)
		|| (index >= 178 && index <= 181) || index >= 3092;
}


static bool
InEncoderRanges(const EncoderRange* ranges, size_t count, uint32_t first,
	uint32_t words)
{
	for (size_t i = 0; i < count; i++) {
		if (first >= ranges[i].first && words <= ranges[i].end - first)
			return true;
	}
	return false;
}


static void
RememberEncoderBuffer(EncoderJob& job, DmaBuffer* buffer, bool shared)
{
	for (uint32_t index = 0; index < job.bufferCount; index++) {
		if (job.buffers[index] == buffer) {
			job.shared[index] |= shared;
			return;
		}
	}
	if (job.bufferCount < sizeof(job.buffers) / sizeof(job.buffers[0])) {
		job.shared[job.bufferCount] = shared;
		job.buffers[job.bufferCount++] = buffer;
	}
}


/*!	Reads the requests of one MPP_IOC_CFG_V1 batch for RKVENC into the
	handle's job: register words only inside the blocks above, every non-zero
	address register a buffer of this team (translated to its physical
	address, plus MPP's offsets), the read-backs inside the readable words.
	With sHardwareLock held. */
static status_t
ValidateEncoderJob(OpenHandle* opened, const MppServiceRequest* userRequests)
{
	EncoderJob& job = *opened->encoderJob;
	job.valid = false;
	job.readCount = 0;
	job.bufferCount = 0;
	memset(job.written, 0, sizeof(job.written));
	struct OffsetEntry { uint32_t index; uint32_t offset; } offsets[64];
	uint32_t offsetCount = 0;
	uint32_t writes = 0;
	bool last = false;
	for (uint32_t index = 0; index < 16 && !last; index++) {
		MppServiceRequest request;
		status_t status = user_memcpy(&request, userRequests + index,
			sizeof(request));
		if (status != B_OK)
			return status;
		if ((request.flags & ~(uint32_t)0x17) != 0
			|| request.dataAddress == 0 || request.bytes == 0
			|| (request.bytes & 3) != 0 || (request.offset & 3) != 0)
			return B_BAD_VALUE;
		uint32_t first = request.offset / 4;
		uint32_t words = request.bytes / 4;
		switch (request.command) {
			case 0x200: // register write
				if (!InEncoderRanges(kEncoderWrites, B_COUNT_OF(kEncoderWrites),
						first, words))
					return B_BAD_VALUE;
				status = user_memcpy(&job.registers[first],
					(const void*)(addr_t)request.dataAddress, request.bytes);
				if (status != B_OK)
					return status;
				for (uint32_t word = first; word < first + words; word++)
					job.written[word / 32] |= 1u << (word % 32);
				writes++;
				break;
			case 0x201: // register read-back
				if (!InEncoderRanges(kEncoderReads, B_COUNT_OF(kEncoderReads),
						first, words) || job.readCount == B_COUNT_OF(job.reads))
					return B_BAD_VALUE;
				job.reads[job.readCount].address = request.dataAddress;
				job.reads[job.readCount].first = first;
				job.reads[job.readCount].count = words;
				job.readCount++;
				break;
			case 0x202: // handle offset table
				if (request.offset != 0 || request.bytes > sizeof(offsets))
					return B_BAD_VALUE;
				offsetCount = request.bytes / sizeof(OffsetEntry);
				status = user_memcpy(offsets,
					(const void*)(addr_t)request.dataAddress, request.bytes);
				if (status != B_OK)
					return status;
				break;
			case 0x203: // row caches in SRAM: not used, they stay in DRAM
				if (request.offset != 0 || request.bytes > 32 * 8)
					return B_BAD_VALUE;
				break;
			default:
				return B_NOT_SUPPORTED;
		}
		last = (request.flags & 2) != 0;
	}
	if (!last || writes == 0 || job.readCount == 0
		|| (job.written[0] & (1u << kEncoderStartRegister)) == 0)
		return B_BAD_VALUE;

	// Each address register's buffer, frame registers 160-183 then OSD.
	DmaBuffer* targets[32] = {};
	for (uint32_t index = 0; index < kEncoderRegisters; index++) {
		if (!IsEncoderAddressRegister(index)
			|| (job.written[index / 32] & (1u << (index % 32))) == 0)
			continue;
		uint32_t handle = job.registers[index];
		if (handle == 0)
			continue;
		DmaBuffer* allocation = FindTeamBuffer(opened->owner, handle);
		if (allocation == NULL || allocation->physical > UINT32_MAX
			|| allocation->bytes > UINT32_MAX - allocation->physical)
			return B_ENTRY_NOT_FOUND;
		job.registers[index] = (uint32_t)allocation->physical;
		targets[index <= 183 ? index - 160 : 24 + index - 3092] = allocation;
		RememberEncoderBuffer(job, allocation, IsSharedEncoderRegister(index));
	}
	for (uint32_t index = 0; index < offsetCount; index++) {
		uint32_t target = offsets[index].index;
		if (!IsEncoderAddressRegister(target))
			return B_BAD_VALUE;
		DmaBuffer* allocation
			= targets[target <= 183 ? target - 160 : 24 + target - 3092];
		if (allocation == NULL)
			return B_BAD_VALUE;
		// The bitstream top (172) may name the end of its buffer: MPP's H.265
		// HAL gives the size there, its H.264 HAL the size less one.
		uint64_t limit = allocation->physical + allocation->bytes
			+ (target == 172 ? 1 : 0);
		if (job.registers[target] + (uint64_t)offsets[index].offset >= limit) {
			dprintf("rk3588_vpu: encode offset %#" B_PRIx32 " for register %"
				B_PRIu32 " is outside its buffer of %zu bytes\n",
				offsets[index].offset, target, allocation->bytes);
			return B_BAD_VALUE;
		}
		job.registers[target] += offsets[index].offset;
	}
	job.valid = true;
	return B_OK;
}


static void
RememberJobBuffer(MppPendingJob& job, DmaBuffer* buffer, bool shared)
{
	for (uint32_t index = 0; index < job.bufferCount; index++) {
		if (job.buffers[index] == buffer) {
			job.shared[index] |= shared;
			return;
		}
	}
	if (job.bufferCount < sizeof(job.buffers) / sizeof(job.buffers[0])) {
		job.shared[job.bufferCount] = shared;
		job.buffers[job.bufferCount++] = buffer;
	}
}


/*!	Whether the processor shares the buffer an RKVDEC address register
	(128-197) names in an H.264 or HEVC job: the stream and the tables MPP
	writes, and the picture it reads back. Reference pictures (including
	the one errors are concealed from), motion vectors and row caches only
	ever pass between jobs; MPP writes none of them (a missing reference
	gets a new buffer, cleaned when it was handed out). Leaving them alone
	saves most of the maintenance: at 4K, ten bits, a job's references come
	to some 100 MiB, cleaned and invalidated twice. */
static bool
IsSharedRkvdecRegister(uint32_t index)
{
	if (index == 131 || index == 132)
		return false;	// this picture's motion vectors, the error reference
	if (index >= 133 && index <= 142)
		return false;	// row caches
	if (index >= 164 && index <= 179)
		return false;	// reference pictures
	if (index >= 181 && index <= 196)
		return false;	// their motion vectors
	return true;
}


/*!	The same for a VPU981 (AV1) address register. A reference picture
	takes five: luma, chroma, motion vectors and two tile tables, for each
	of seven references; the column buffers and synchronisation areas are
	the decoder's own. What the post-processor writes (registers 326, 328
	and 505) is the picture the processor reads; the rest - the stream, the
	tile and probability tables, film grain, global motion, the picture
	being decoded - stays maintained, as does anything not named here. */
static bool
IsSharedAv1Register(uint32_t index)
{
	if ((index >= 67 && index <= 79) || (index >= 101 && index <= 113)
		|| (index >= 135 && index <= 147)) {
		return (index & 1) == 0;	// references: luma, chroma, vectors
	}
	if ((index >= 192 && index <= 204) || (index >= 226 && index <= 238))
		return (index & 1) != 0;	// references: tile tables
	switch (index) {
		case 85:	// CDEF column buffer
		case 89:	// super-resolution column buffer
		case 91:	// loop restoration column buffer
		case 175:	// motion compensation synchronisation
		case 177:
		case 179:	// vertical filter
			return false;
		default:
			return true;
	}
}

static bool
IsAv1AddressRegister(uint32_t index)
{
	static const uint16_t registers[] = {
		65, 67, 69, 71, 73, 75, 77, 79, 81, 83, 85, 89, 91, 95, 99,
		101, 103, 105, 107, 109, 111, 113, 133, 135, 137, 139, 141,
		143, 145, 147, 167, 169, 171, 173, 175, 177, 179, 183, 190,
		192, 194, 196, 198, 200, 202, 204, 224, 226, 228, 230, 232,
		234, 236, 238, 326, 328, 505
	};
	for (uint32_t candidate : registers) {
		if (candidate == index)
			return true;
	}
	return false;
}


static status_t
ValidateMppJob(OpenHandle* opened, const MppServiceRequest* userRequests)
{
	MppValidationStats trial = {};
	trial.version = 2;
	trial.lastStatus = B_BAD_VALUE;
	uint32_t addresses[70] = {};
	uint32_t av1Handles[512] = {};
	struct OffsetEntry { uint32_t index; uint32_t offset; } offsets[64] = {};
	uint32_t offsetCount = 0;
	bool gotAddresses = false;
	MppPendingJob job = {};
	bool last = false;
	for (uint32_t index = 0; index < 16; index++) {
		MppServiceRequest request;
		status_t status = user_memcpy(&request, userRequests + index,
			sizeof(request));
		if (status != B_OK)
			return status;
		if ((request.flags & ~(uint32_t)0x17) != 0
			|| request.dataAddress == 0 || request.bytes == 0
			|| (request.bytes & 3) != 0 || (request.offset & 3) != 0)
			return B_BAD_VALUE;
		switch (request.command) {
			case 0x200: { // register write
				uint32_t firstRegister = opened->mppClientType == kMppClientAv1
					? 0 : 8;
				uint32_t registerBytes = opened->mppClientType == kMppClientAv1
					? sizeof(job.registers) : 0x480;
				if (request.bytes > registerBytes
					|| request.offset < firstRegister * 4
					|| request.offset >= registerBytes
					|| request.bytes > registerBytes - request.offset)
					return B_BAD_VALUE;
				if (opened->mppClientType == kMppClientRkvdec
					&& request.offset == 128 * 4) {
					if (request.bytes != sizeof(addresses) || gotAddresses)
						return B_BAD_VALUE;
					status = user_memcpy(addresses,
						(const void*)(addr_t)request.dataAddress,
						sizeof(addresses));
					if (status != B_OK)
						return status;
					gotAddresses = true;
				}
				status = user_memcpy(&job.registers[request.offset / 4],
					(const void*)(addr_t)request.dataAddress, request.bytes);
				if (status != B_OK)
					return status;
				for (uint32_t word = request.offset / 4;
					word < (request.offset + request.bytes) / 4; word++) {
					job.written[word / 32] |= 1u << (word % 32);
				}
				trial.writeRequests++;
				break;
			}
			case 0x201: // register read-back
				if ((opened->mppClientType == kMppClientRkvdec
						&& (request.offset != 224 * 4 || request.bytes > 128))
					|| (opened->mppClientType == kMppClientAv1
						&& (request.offset != 0
							|| request.bytes != sizeof(job.registers))))
					return B_BAD_VALUE;
				job.readAddress = request.dataAddress;
				job.readBytes = request.bytes;
				trial.readRequests++;
				break;
			case 0x202: // handle offset table
				if (request.offset != 0 || request.bytes > 64 * 8)
					return B_BAD_VALUE;
				offsetCount = request.bytes / sizeof(OffsetEntry);
				status = user_memcpy(offsets,
					(const void*)(addr_t)request.dataAddress,
					request.bytes);
				if (status != B_OK)
					return status;
				trial.offsetRequests++;
				break;
			case 0x203: // row-cache information
				if (request.offset != 0 || request.bytes > 32 * 8)
					return B_BAD_VALUE;
				trial.rcbRequests++;
				break;
			default:
				return B_NOT_SUPPORTED;
		}
		last = (request.flags & 2) != 0;
		if (last)
			break;
	}
	if (!last || trial.writeRequests == 0 || trial.readRequests != 1
		|| (opened->mppClientType == kMppClientRkvdec && !gotAddresses))
		return B_BAD_VALUE;
	if (opened->mppClientType == kMppClientRkvdec) {
		for (uint32_t index = 0; index < 70; index++) {
			if (addresses[index] == 0)
				continue;
			DmaBuffer* allocation = FindTeamBuffer(opened->owner, addresses[index]);
			if (allocation == NULL)
				return B_ENTRY_NOT_FOUND;
			job.registers[128 + index] = (uint32_t)allocation->physical;
			RememberJobBuffer(job, allocation,
				IsSharedRkvdecRegister(128 + index));
			trial.addressHandles++;
		}
	} else {
		for (uint32_t index = 0; index < 512; index++) {
			if (!IsAv1AddressRegister(index) || job.registers[index] == 0)
				continue;
			uint32_t handle = job.registers[index];
			DmaBuffer* allocation = FindTeamBuffer(opened->owner, handle);
			if (allocation == NULL)
				return B_ENTRY_NOT_FOUND;
			av1Handles[index] = handle;
			job.registers[index] = (uint32_t)allocation->physical;
			RememberJobBuffer(job, allocation, IsSharedAv1Register(index));
			trial.addressHandles++;
		}
	}
	for (uint32_t index = 0; index < offsetCount; index++) {
		if ((opened->mppClientType == kMppClientRkvdec
				&& (offsets[index].index < 128 || offsets[index].index > 197))
			|| (opened->mppClientType == kMppClientAv1
				&& offsets[index].index >= 512))
			return B_BAD_VALUE;
		uint32_t handle = opened->mppClientType == kMppClientRkvdec
			? addresses[offsets[index].index - 128]
			: av1Handles[offsets[index].index];
		DmaBuffer* allocation = FindTeamBuffer(opened->owner, handle);
		if (allocation == NULL || offsets[index].offset >= allocation->bytes)
			return B_BAD_VALUE;
		if (offsets[index].offset > UINT32_MAX
			- job.registers[offsets[index].index])
			return B_BAD_VALUE;
		job.registers[offsets[index].index] += offsets[index].offset;
		trial.offsetEntries++;
	}
	uint32_t decodeMode = UINT32_MAX;
	if (opened->mppClientType == kMppClientRkvdec) {
		// 0 HEVC, 1 H.264, 2 VP9 (MPP's vdpu34x VP9 HAL on the RK3588)
		decodeMode = job.registers[9] & 0x3ff;
		if ((job.registers[10] & 1) == 0 || decodeMode > 2)
			return B_BAD_VALUE;
	} else if ((job.registers[1] & 1) == 0
		|| (job.registers[1] & 0xffffff00) != 0
		|| ((job.registers[3] >> 27) & 0x1f) != 17) {
		return B_BAD_VALUE;
	}
	job.valid = true;
	opened->mppJob = job;
	trial.jobs = 1;
	trial.lastStatus = (opened->mppClientType == kMppClientRkvdec
		&& opened->controller->decodeEnabled)
		|| (opened->mppClientType == kMppClientAv1
			&& opened->controller->av1DecodeEnabled) ? B_OK : B_NOT_SUPPORTED;
	Controller* controller = opened->controller;
	controller->mppStats.version = 2;
	controller->mppStats.jobs++;
	controller->mppStats.writeRequests += trial.writeRequests;
	controller->mppStats.readRequests += trial.readRequests;
	controller->mppStats.offsetRequests += trial.offsetRequests;
	controller->mppStats.rcbRequests += trial.rcbRequests;
	controller->mppStats.addressHandles += trial.addressHandles;
	controller->mppStats.offsetEntries += trial.offsetEntries;
	controller->mppStats.lastStatus = trial.lastStatus;
	static uint32_t sValidations = 0;
	if (sValidations++ < 4 || trial.lastStatus != B_OK)
	dprintf("rk3588_vpu: validated MPP job client=%u mode=%u writes=%u reads=%u offsets=%u"
		" rcb=%u handles=%u entries=%u; submission %s\n",
		opened->mppClientType, decodeMode,
		trial.writeRequests, trial.readRequests, trial.offsetRequests,
		trial.rcbRequests, trial.addressHandles, trial.offsetEntries,
		trial.lastStatus == B_OK ? "enabled" : "disabled");
	return trial.lastStatus;
}

static const size_t kMaximumBufferBytes = 32 * 1024 * 1024;
// MPP keeps up to 35 pictures for an H.264 stream: 146 MiB at 1080p.
static const size_t kMaximumClientBytes = 768 * 1024 * 1024;
// A 4K ten-bit film holds 15 to 20 pictures of 14 MiB (HEVC) or 21 MiB (AV1)
// at once; what does not fit in the pool is write combining memory, which
// the processor reads at a fraction of the speed (AV1 at 4K then played at
// 15 pictures a second instead of 24). dma_pool_mb sets less on boards with
// little memory.
static const size_t kDefaultPoolMegabytes = 640;


// #pragma mark - DMA pool


// Writes back and drops the lines a pool buffer has in the caches, so the
// decoder sees what the processor wrote and the processor then reads what
// the decoder wrote. Buffers outside the pool are write combining already.
static void
SyncBufferForDevice(DmaBuffer* buffer)
{
	if (buffer->poolPage < 0)
		return;
	static size_t sLine = 0;
	if (sLine == 0) {
		uint64_t ctr = 0;
		asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
		sLine = arm64_data_cache_line_size(ctr);
	}
	addr_t start = (addr_t)buffer->kernelAddress;
	addr_t end = start + buffer->bytes;
	memory_full_barrier();
	for (addr_t p = start; p < end; p += sLine)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	asm volatile("dsb sy" ::: "memory");
}


static void
SyncJobBuffers(const MppPendingJob& job)
{
	for (uint32_t index = 0; index < job.bufferCount; index++) {
		if (job.shared[index])
			SyncBufferForDevice(job.buffers[index]);
	}
}


static bool
PoolRunFree(const Controller* controller, size_t first, size_t count)
{
	for (size_t page = first; page < first + count; page++) {
		if ((controller->poolMap[page / 8] & (1u << (page % 8))) != 0)
			return false;
	}
	return true;
}


static void
PoolMark(Controller* controller, size_t first, size_t count, bool used)
{
	for (size_t page = first; page < first + count; page++) {
		if (used)
			controller->poolMap[page / 8] |= 1u << (page % 8);
		else
			controller->poolMap[page / 8] &= ~(1u << (page % 8));
	}
}


// First fit; returns the first page of the run or -1.
static ssize_t
PoolAllocate(Controller* controller, size_t count)
{
	if (controller->poolMap == NULL || count == 0
		|| count > controller->poolPages) {
		return -1;
	}
	size_t page = 0;
	while (page + count <= controller->poolPages) {
		if (PoolRunFree(controller, page, count)) {
			PoolMark(controller, page, count, true);
			return (ssize_t)page;
		}
		// Skip past the used page that stopped this run.
		size_t next = page;
		while (next < page + count
			&& (controller->poolMap[next / 8] & (1u << (next % 8))) == 0)
			next++;
		page = next + 1;
	}
	return -1;
}


static void
CreateDmaPool(Controller* controller, size_t megabytes)
{
	controller->poolArea = -1;
	if (megabytes == 0)
		return;
	size_t bytes = megabytes * 1024 * 1024;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = UINT64_C(1) << 32;
	physicalRestrictions.alignment = B_PAGE_SIZE;
	void* address = NULL;
	area_id area = create_area_etc(B_SYSTEM_TEAM, "RK3588 VPU DMA pool", bytes,
		B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0,
		&virtualRestrictions, &physicalRestrictions, &address);
	if (area < B_OK) {
		dprintf("rk3588_vpu: no DMA pool of %zu MiB: %s\n", megabytes,
			strerror(area));
		return;
	}
	physical_entry entry = {};
	if (get_memory_map(address, bytes, &entry, 1) != B_OK
		|| entry.size < bytes || entry.address >= (UINT64_C(1) << 32)
		|| bytes > (UINT64_C(1) << 32) - entry.address) {
		delete_area(area);
		dprintf("rk3588_vpu: DMA pool not below 4 GiB\n");
		return;
	}
	// Pool memory stays cacheable, for the processor and its users: reading
	// a decoded picture out of write combining memory cost 23 ms at 720p.
	// The decoder does not snoop the caches, so every job cleans and
	// invalidates the buffers it uses before and after it runs.
	uint64_t ctr = 0;
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	size_t line = arm64_data_cache_line_size(ctr);
	for (addr_t p = (addr_t)address; p < (addr_t)address + bytes; p += line)
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	memory_full_barrier();
	size_t pages = bytes / B_PAGE_SIZE;
	uint8_t* map = (uint8_t*)calloc((pages + 7) / 8, 1);
	if (map == NULL) {
		delete_area(area);
		return;
	}
	controller->poolArea = area;
	controller->poolAddress = (uint8_t*)address;
	controller->poolPhysical = entry.address;
	controller->poolPages = pages;
	controller->poolMap = map;
	dprintf("rk3588_vpu: DMA pool of %zu MiB at %#" B_PRIx64 "\n", megabytes,
		entry.address);
}


static void
DeleteDmaPool(Controller* controller)
{
	if (controller->poolArea >= B_OK)
		delete_area(controller->poolArea);
	free(controller->poolMap);
	controller->poolArea = -1;
	controller->poolMap = NULL;
	controller->poolPages = 0;
}


static void
DeleteBuffer(OpenHandle* opened, DmaBuffer* buffer)
{
	if (buffer->userArea >= B_OK)
		vm_delete_area(opened->owner, buffer->userArea, true);
	if (buffer->kernelArea >= B_OK)
		delete_area(buffer->kernelArea);
	if (buffer->poolPage >= 0) {
		PoolMark(opened->controller, buffer->poolPage,
			buffer->bytes / B_PAGE_SIZE, false);
	}
	free(buffer);
}


// The allocator and decoder use separate device opens in the same team.
// Handles stay unique for the lifetime of this loaded driver module.
static DmaBuffer*
FindTeamBuffer(team_id owner, uint32_t handle)
{
	for (OpenHandle* opened = sOpenHandles; opened != NULL;
		opened = opened->next) {
		if (opened->owner != owner)
			continue;
		for (DmaBuffer* buffer = opened->buffers; buffer != NULL;
			buffer = buffer->next) {
			if (buffer->handle == handle)
				return buffer;
		}
	}
	return NULL;
}


static status_t
AllocateBuffer(OpenHandle* opened, BufferAllocation& request)
{
	if (request.version != kBufferVersion || request.handle != 0
		|| request.area != 0 || request.reserved != 0 || request.address != 0
		|| request.bytes == 0 || request.bytes > kMaximumBufferBytes
		|| sNextBufferHandle == UINT32_MAX) {
		return B_BAD_VALUE;
	}
	size_t bytes = (request.bytes + B_PAGE_SIZE - 1) & ~(B_PAGE_SIZE - 1);
	if (opened->allocatedBytes > kMaximumClientBytes - bytes)
		return B_NO_MEMORY;
	DmaBuffer* buffer = (DmaBuffer*)calloc(1, sizeof(DmaBuffer));
	if (buffer == NULL)
		return B_NO_MEMORY;
	buffer->kernelArea = -1;
	buffer->userArea = -1;
	buffer->bytes = bytes;
	buffer->poolPage = -1;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physical_entry entry = {};
	uint64_t ctr = 0;
	size_t line = 0;
	void* userAddress = NULL;
	status_t status = B_OK;
	physicalRestrictions.high_address = UINT64_C(1) << 32;
	physicalRestrictions.alignment = B_PAGE_SIZE;

	buffer->poolPage = PoolAllocate(opened->controller, bytes / B_PAGE_SIZE);
	if (buffer->poolPage >= 0) {
		Controller* controller = opened->controller;
		size_t offset = (size_t)buffer->poolPage * B_PAGE_SIZE;
		buffer->physical = controller->poolPhysical + offset;
		buffer->kernelAddress = controller->poolAddress + offset;
		// The pool is reused: what another client decoded must not show.
		memset(buffer->kernelAddress, 0, bytes);
		SyncBufferForDevice(buffer);
		buffer->userArea = vm_map_physical_memory(opened->owner,
			"RK3588 VPU DMA mapping", &userAddress,
			B_ANY_ADDRESS | B_WRITE_BACK_MEMORY, bytes,
			B_READ_AREA | B_WRITE_AREA, buffer->physical, false);
		status = buffer->userArea;
		if (status < B_OK)
			goto fail;
		goto mapped;
	}

	if (opened->controller->poolMap != NULL) {
		static bool sPoolFullLogged = false;
		if (!sPoolFullLogged) {
			sPoolFullLogged = true;
			dprintf("rk3588_vpu: DMA pool full, a buffer of %zu KiB is write"
				" combining (slow to read); dma_pool_mb sets the pool's size\n",
				bytes / 1024);
		}
	}
	buffer->kernelArea = create_area_etc(B_SYSTEM_TEAM, "RK3588 VPU DMA buffer",
		bytes, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions,
		&buffer->kernelAddress);
	status = buffer->kernelArea;
	if (status < B_OK)
		goto fail;
	status = get_memory_map(buffer->kernelAddress, bytes, &entry, 1);
	if (status != B_OK)
		goto fail;
	if (entry.size < bytes || (entry.address & (B_PAGE_SIZE - 1)) != 0
		|| entry.address >= (UINT64_C(1) << 32)
		|| bytes > (UINT64_C(1) << 32) - entry.address) {
		status = B_BAD_VALUE;
		goto fail;
	}
	buffer->physical = entry.address;
	// create_area_etc() zeroed through a cached alias. Evict those lines
	// before changing the RAM to Normal Non-cacheable for CPU and VPU access.
	asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
	line = arm64_data_cache_line_size(ctr);
	for (addr_t p = (addr_t)buffer->kernelAddress;
		p < (addr_t)buffer->kernelAddress + bytes; p += line) {
		asm volatile("dc civac, %0" :: "r"(p) : "memory");
	}
	memory_full_barrier();
	status = vm_set_area_memory_type(buffer->kernelArea, buffer->physical,
		B_WRITE_COMBINING_MEMORY);
	if (status != B_OK)
		goto fail;
	memory_full_barrier();
	buffer->userArea = vm_clone_area(opened->owner, "RK3588 VPU DMA mapping",
		&userAddress, B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, 0,
		buffer->kernelArea, true);
	status = buffer->userArea;
	if (status < B_OK)
		goto fail;
mapped:
	buffer->handle = ++sNextBufferHandle;
	request.handle = buffer->handle;
	request.bytes = bytes;
	request.area = buffer->userArea;
	request.address = (addr_t)userAddress;
	buffer->next = opened->buffers;
	opened->buffers = buffer;
	opened->allocatedBytes += bytes;
	return B_OK;

fail:
	DeleteBuffer(opened, buffer);
	return status;
}


static bool
ReadCells(const void* data, int length, uint32_t* cells, size_t count)
{
	if (data == NULL || cells == NULL || length < 0
		|| count > SIZE_MAX / 4 || (size_t)length != count * 4) {
		return false;
	}
	const uint8_t* bytes = (const uint8_t*)data;
	for (size_t i = 0; i < count; i++) {
		cells[i] = (uint32_t)bytes[i * 4] << 24
			| (uint32_t)bytes[i * 4 + 1] << 16
			| (uint32_t)bytes[i * 4 + 2] << 8 | bytes[i * 4 + 3];
	}
	return true;
}


class FdtNode {
public:
	bool SetTo(device_node* node)
	{
		driver_module_info* module;
		void* cookie;
		if (node == NULL || sDeviceManager->get_driver(node, &module, &cookie) != B_OK
			|| strcmp(module->info.name, "bus_managers/fdt/driver_v1") != 0) {
			return false;
		}
		fModule = (fdt_device_module_info*)module;
		fDevice = (fdt_device*)cookie;
		return true;
	}

	bool HasString(const char* property, const char* wanted) const
	{
		int length = 0;
		const char* data = (const char*)fModule->get_prop(fDevice, property, &length);
		if (data == NULL || length <= 0)
			return false;
		while (length > 0) {
			const char* end = (const char*)memchr(data, 0, length);
			if (end == NULL || end == data)
				return false;
			if (strcmp(data, wanted) == 0)
				return true;
			length -= end - data + 1;
			data = end + 1;
		}
		return false;
	}

	bool NameIs(const char* wanted) const
	{
		return strcmp(fModule->get_name(fDevice), wanted) == 0;
	}

	bool Cells(const char* property, uint32_t* cells, size_t count) const
	{
		int length = 0;
		const void* data = fModule->get_prop(fDevice, property, &length);
		return ReadCells(data, length, cells, count);
	}

	bool Reg(uint64_t& base, uint64_t& size) const
	{
		return fModule->get_reg(fDevice, 0, &base, &size);
	}

	bool Enabled() const
	{
		int length = 0;
		const char* status = (const char*)fModule->get_prop(fDevice, "status", &length);
		return status == NULL || (length == 5 && memcmp(status, "okay", 5) == 0)
			|| (length == 3 && memcmp(status, "ok", 3) == 0);
	}

	fdt_device_module_info* fModule = NULL;
	fdt_device* fDevice = NULL;
};


static bool
BoardMatches(device_node* codec)
{
	device_node* node = sDeviceManager->get_parent_node(codec);
	while (node != NULL) {
		FdtNode fdt;
		if (!fdt.SetTo(node)) {
			sDeviceManager->put_node(node);
			return false;
		}
		if (fdt.NameIs("")) {
			bool matches = fdt.HasString("compatible", "radxa,rock-5-itx");
			sDeviceManager->put_node(node);
			return matches;
		}
		device_node* parent = sDeviceManager->get_parent_node(node);
		sDeviceManager->put_node(node);
		node = parent;
	}
	return false;
}


static bool
GetInterrupt(const FdtNode& node, unsigned index, uint32_t expectedSpi,
	device_node*& controller, uint32_t& interrupt)
{
	uint32_t specifiers[4];
	int length = 0;
	const void* data = node.fModule->get_prop(node.fDevice, "interrupts", &length);
	if (data == NULL || length < (int)((index + 1) * 16)
		|| !ReadCells((const uint8_t*)data + index * 16, 16, specifiers, 4)
		|| specifiers[0] != 0 || specifiers[1] != expectedSpi
		|| specifiers[2] != 4 || specifiers[3] != 0) {
		return false;
	}
	uint64_t irq = 0;
	if (!node.fModule->get_interrupt(node.fDevice, index, &controller, &irq)
		|| controller == NULL || irq != expectedSpi + 32) {
		return false;
	}
	interrupt = irq;
	return true;
}


static bool
ReadResources(device_node* parent, ResourceInfo& output)
{
	ResourceInfo info = {};
	FdtNode codec;
	if (!codec.SetTo(parent) || !codec.Enabled()
		|| !codec.HasString("compatible", "rockchip,rk3588-vpu121")
		|| !codec.HasString("compatible", "rockchip,rk3568-vpu")
		|| !BoardMatches(parent) || !codec.Reg(info.decoderBase, info.decoderSize)) {
		return false;
	}
	uint32_t clocks[4], power[2], iommuPhandle;
	if (!codec.HasString("clock-names", "aclk")
		|| !codec.HasString("clock-names", "hclk")
		|| !codec.HasString("interrupt-names", "vdpu")
		|| !codec.Cells("clocks", clocks, 4)
		|| !codec.Cells("power-domains", power, 2)
		|| !codec.Cells("iommus", &iommuPhandle, 1)
		|| codec.fModule->get_prop(codec.fDevice, "interrupts-extended", NULL) != NULL) {
		return false;
	}
	device_node* gicNode = NULL;
	if (!GetInterrupt(codec, 0, 119, gicNode, info.decoderInterrupt))
		return false;
	FdtNode gic;
	uint64_t gicSize;
	uint32_t cells;
	if (!gic.SetTo(gicNode) || !gic.HasString("compatible", "arm,gic-v3")
		|| !gic.Cells("#interrupt-cells", &cells, 1) || cells != 4
		|| !gic.Reg(info.interruptBase, gicSize) || gicSize != 0x10000) {
		return false;
	}
	fdt_bus_module_info* busModule;
	fdt_bus* bus;
	if (sDeviceManager->get_driver(codec.fModule->get_bus(codec.fDevice),
			(driver_module_info**)&busModule, (void**)&bus) != B_OK
		|| strcmp(busModule->info.info.name, "bus_managers/fdt/root/driver_v1") != 0) {
		return false;
	}
	FdtNode iommu;
	device_node* iommuNode = busModule->node_by_phandle(bus, iommuPhandle);
	device_node* iommuGic = NULL;
	if (!iommu.SetTo(iommuNode) || !iommu.Enabled()
		|| !iommu.HasString("compatible", "rockchip,rk3588-iommu")
		|| !iommu.HasString("compatible", "rockchip,rk3568-iommu")
		|| !iommu.Reg(info.iommuBase, info.iommuSize)
		|| !GetInterrupt(iommu, 0, 118, iommuGic, info.iommuInterrupt)
		|| iommuGic != gicNode) {
		return false;
	}
	if (clocks[0] == 0 || clocks[0] != clocks[2])
		return false;
	FdtNode clock;
	if (!clock.SetTo(busModule->node_by_phandle(bus, clocks[0]))
		|| !clock.Enabled() || !clock.HasString("compatible", "rockchip,rk3588-cru")
		|| !clock.Cells("#clock-cells", &cells, 1) || cells != 1
		|| !clock.Reg(info.clockBase, info.clockSize)) {
		return false;
	}
	info.clockIds[0] = clocks[1];
	info.clockIds[1] = clocks[3];
	FdtNode powerController;
	device_node* powerNode = busModule->node_by_phandle(bus, power[0]);
	if (!powerController.SetTo(powerNode) || !powerController.Enabled()
		|| !powerController.HasString("compatible", "rockchip,rk3588-power-controller")
		|| !powerController.Cells("#power-domain-cells", &cells, 1) || cells != 1) {
		return false;
	}
	device_node* pmuNode = sDeviceManager->get_parent_node(powerNode);
	FdtNode pmu;
	bool validPower = pmu.SetTo(pmuNode) && pmu.Enabled()
		&& pmu.HasString("compatible", "rockchip,rk3588-pmu")
		&& pmu.Reg(info.powerBase, info.powerSize);
	if (pmuNode != NULL)
		sDeviceManager->put_node(pmuNode);
	if (!validPower)
		return false;
	info.powerDomain = power[1];
	info.version = kResourceVersion;
	info.flags = 1;
	if (!ResourcesMatch(info))
		return false;
	output = info;
	return true;
}


static uint32_t
ReadRegister(const volatile uint32_t* registers, uint32_t offset)
{
	return registers[offset / 4];
}


static Snapshot
CaptureSnapshot(const volatile uint32_t* clock, const volatile uint32_t* power)
{
	Snapshot snapshot = {};
	snapshot.version = kSnapshotVersion;
	snapshot.flags = 1;
	snapshot.startedMicros = system_time();
	memory_read_barrier();
	snapshot.idleRequest = ReadRegister(power, 0x10c);
	snapshot.idleAck = ReadRegister(power, 0x118);
	snapshot.idleStatus = ReadRegister(power, 0x120);
	snapshot.powerGate = ReadRegister(power, 0x14c);
	snapshot.powerStatus = ReadRegister(power, 0x180);
	snapshot.repairStatus = ReadRegister(power, 0x290);
	snapshot.chainStatus = ReadRegister(power, 0x1f0);
	snapshot.memoryStatus = ReadRegister(power, 0x1f8);
	snapshot.clockSelect98 = ReadRegister(clock, 0x488);
	snapshot.clockGate40 = ReadRegister(clock, 0x8a0);
	snapshot.clockGate41 = ReadRegister(clock, 0x8a4);
	snapshot.clockGate44 = ReadRegister(clock, 0x8b0);
	snapshot.clockGate45 = ReadRegister(clock, 0x8b4);
	snapshot.clockGate68 = ReadRegister(clock, 0x910);
	snapshot.clockSelect89 = ReadRegister(clock, 0x464);
	snapshot.clockSelect90 = ReadRegister(clock, 0x468);
	snapshot.clockSelect91 = ReadRegister(clock, 0x46c);
	snapshot.clockSelect163 = ReadRegister(clock, 0x58c);
	snapshot.softReset68 = ReadRegister(clock, 0xb10);
	snapshot.finishedMicros = system_time();
	return snapshot;
}


static status_t
ReadSnapshot(const ResourceInfo& resources, Snapshot& output)
{
	if (!ResourcesMatch(resources))
		return B_NOT_SUPPORTED;
	void* clockAddress = NULL;
	AreaDeleter clockArea(map_physical_memory("VDPU clock observation",
		resources.clockBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA, &clockAddress));
	if (clockArea.Get() < B_OK)
		return clockArea.Get();
	void* powerAddress = NULL;
	AreaDeleter powerArea(map_physical_memory("VDPU power observation",
		resources.powerBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
		B_KERNEL_READ_AREA, &powerAddress));
	if (powerArea.Get() < B_OK)
		return powerArea.Get();
	output = CaptureSnapshot((const volatile uint32_t*)clockAddress,
		(const volatile uint32_t*)powerAddress);
	return B_OK;
}


static status_t
ProbeDmaAddress(DmaProbe& output)
{
	void* address = NULL;
	virtual_address_restrictions virtualRestrictions = {};
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.high_address = UINT64_C(1) << 32;
	AreaDeleter area(create_area_etc(B_SYSTEM_TEAM, "VDPU 32-bit DMA probe",
		B_PAGE_SIZE, B_CONTIGUOUS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0, 0, &virtualRestrictions, &physicalRestrictions, &address));
	if (area.Get() < B_OK)
		return area.Get();
	physical_entry entry = {};
	status_t status = get_memory_map(address, B_PAGE_SIZE, &entry, 1);
	if (status != B_OK)
		return status;
	if (entry.size < B_PAGE_SIZE || entry.address >= (UINT64_C(1) << 32)
		|| B_PAGE_SIZE > (UINT64_C(1) << 32) - entry.address) {
		return B_BAD_VALUE;
	}
	output.physical = entry.address;
	output.bytes = B_PAGE_SIZE;
	return B_OK;
}


class PowerHardware {
public:
	status_t Init(const ResourceInfo& resources)
	{
		if (!ResourcesMatch(resources))
			return B_NOT_SUPPORTED;
		void* clock = NULL;
		fClockArea.SetTo(map_physical_memory("VDPU power cycle CRU",
			resources.clockBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA, &clock));
		if (fClockArea.Get() < B_OK)
			return fClockArea.Get();
		void* power = NULL;
		fPowerArea.SetTo(map_physical_memory("VDPU power cycle PMU",
			resources.powerBase, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &power));
		if (fPowerArea.Get() < B_OK)
			return fPowerArea.Get();
		fClock = (const volatile uint32_t*)clock;
		fPower = (volatile uint32_t*)power;
		fResources = resources;
		return B_OK;
	}

	Snapshot SnapshotNow() { return CaptureSnapshot(fClock, fPower); }

	void WritePower(uint32_t offset, uint32_t value)
	{
		fPower[offset / 4] = value;
		memory_write_barrier();
	}

	bool PowerIsOn(uint32_t mask)
	{
		memory_read_barrier();
		return (ReadRegister(fPower, 0x290) & mask) != 0;
	}

	bool WaitPower(uint32_t mask, bool on)
	{
		bigtime_t deadline = system_time() + 10000;
		do {
			memory_read_barrier();
			bool repaired = (ReadRegister(fPower, 0x290) & mask) != 0;
			bool gateOff = (ReadRegister(fPower, 0x180) & kVdpuGate) != 0;
			if (repaired == on && gateOff != on)
				return true;
		} while (system_time() < deadline);
		return false;
	}

	bool WaitIdle(uint32_t mask, bool idle)
	{
		bigtime_t deadline = system_time() + 10000;
		do {
			memory_read_barrier();
			bool ack = (ReadRegister(fPower, 0x118) & mask) != 0;
			bool status = (ReadRegister(fPower, 0x120) & mask) != 0;
			if (ack == idle && status == idle)
				return true;
		} while (system_time() < deadline);
		return false;
	}

	bool WaitRkvdec0Power(bool on)
	{
		bigtime_t deadline = system_time() + 10000;
		do {
			memory_read_barrier();
			bool repaired = (ReadRegister(fPower, 0x290)
				& kRkvdec0Repair) != 0;
			bool gateOff = (ReadRegister(fPower, 0x180)
				& kRkvdec0Gate) != 0;
			if (repaired == on && gateOff != on)
				return true;
		} while (system_time() < deadline);
		return false;
	}

	bool WaitRkvdec0Memory(bool on)
	{
		bigtime_t deadline = system_time() + 10000;
		do {
			memory_read_barrier();
			bool off = (ReadRegister(fPower, 0x1f8)
				& kRkvdec0Memory) != 0;
			if (off != on)
				return true;
		} while (system_time() < deadline);
		return false;
	}

	bool WaitAv1Power(bool on)
	{
		bigtime_t deadline = system_time() + 10000;
		do {
			memory_read_barrier();
			bool repaired = (ReadRegister(fPower, 0x290) & kAv1Repair) != 0;
			bool gateOff = (ReadRegister(fPower, 0x180) & kAv1Gate) != 0;
			if (repaired == on && gateOff != on)
				return true;
		} while (system_time() < deadline);
		return false;
	}

	bool WaitAv1Memory(bool on)
	{
		bigtime_t deadline = system_time() + 10000;
		do {
			memory_read_barrier();
			bool off = (ReadRegister(fPower, 0x1f8) & kAv1Memory) != 0;
			if (off != on)
				return true;
		} while (system_time() < deadline);
		return false;
	}

	status_t ReadDecoderState(DecoderProbe& probe)
	{
		memory_read_barrier();
		if ((ReadRegister(fPower, 0x290) & kVdpuRepair) == 0
			|| (ReadRegister(fPower, 0x180) & kVdpuGate) != 0
			|| (ReadRegister(fPower, 0x120) & kVdpuIdle) != 0) {
			return B_NOT_ALLOWED;
		}
		void* address = NULL;
		AreaDeleter decoderArea(map_physical_memory("VDPU identity observation",
			fResources.decoderBase, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA,
			&address));
		if (decoderArea.Get() < B_OK)
			return decoderArea.Get();
		memory_read_barrier();
		// RK3568/RK3588 VDPU2 places the Hantro decoder bank at +0x400.
		const volatile uint32_t* registers = (const volatile uint32_t*)address;
		probe.decoderId = ReadRegister(registers, 0x400);
		probe.iommuDte = ReadRegister(registers, 0x800);
		probe.iommuStatus = ReadRegister(registers, 0x804);
		probe.iommuInterruptRaw = ReadRegister(registers, 0x814);
		probe.iommuInterruptMask = ReadRegister(registers, 0x81c);
		return B_OK;
	}

	status_t ReadRkvdec0Registers(Rkvdec0RegisterProbe& probe)
	{
		Snapshot state = SnapshotNow();
		if ((state.repairStatus & (kVdpuRepair | kRkvdec0Repair))
				!= (kVdpuRepair | kRkvdec0Repair)
			|| (state.powerStatus & (kVdpuGate | kRkvdec0Gate)) != 0
			|| (state.idleStatus & (kVdpuIdle | kRkvdec0Idle)) != 0
			|| (state.memoryStatus & kRkvdec0Memory) != 0
			|| state.clockGate40 != 0 || state.clockGate44 != 0)
			return B_NOT_ALLOWED;

		// This EDK2 FDT omits the RKVDEC0 node. The exact RK3588 physical
		// addresses are in the matching TRM and mainline rk3588-base.dtsi.
		// Read only, while both required power domains are confirmed on.
		void* address = NULL;
		AreaDeleter area(map_physical_memory("RKVDEC0 register observation",
			0xfdc38000, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA,
			&address));
		if (area.Get() < B_OK)
			return area.Get();
		memory_read_barrier();
		const volatile uint32_t* registers = (const volatile uint32_t*)address;
		probe.link[0] = ReadRegister(registers, 0x000);
		probe.link[1] = ReadRegister(registers, 0x004);
		probe.function[0] = ReadRegister(registers, 0x100);
		probe.function[1] = ReadRegister(registers, 0x104);
		probe.interruptStatus = ReadRegister(registers, 0x480);
		probe.iommuDte = ReadRegister(registers, 0x700);
		probe.iommuStatus = ReadRegister(registers, 0x704);
		probe.iommuFault = ReadRegister(registers, 0x70c);
		return B_OK;
	}

	status_t ReadAv1Registers(Av1RegisterProbe& probe)
	{
		Snapshot state = SnapshotNow();
		if ((state.repairStatus & (kVdpuRepair | kAv1Repair))
				!= (kVdpuRepair | kAv1Repair)
			|| (state.powerStatus & (kVdpuGate | kAv1Gate)) != 0
			|| (state.idleStatus & (kVdpuIdle | kAv1Idle)) != 0
			|| (state.memoryStatus & kAv1Memory) != 0
			|| state.clockSelect163 != 2 || state.clockGate68 != 0
			|| (state.softReset68 & 0x36) != 0) {
			return B_NOT_ALLOWED;
		}
		void* address = NULL;
		AreaDeleter area(map_physical_memory("AV1 VPU981 register observation",
			0xfdc70000, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY, B_KERNEL_READ_AREA,
			&address));
		if (area.Get() < B_OK)
			return area.Get();
		memory_read_barrier();
		const volatile uint32_t* registers = (const volatile uint32_t*)address;
		probe.registers[0] = registers[0];
		probe.registers[1] = registers[1];
		probe.registers[2] = registers[2];
		probe.buildId = registers[309];
		probe.synthesisId = registers[310];
		return B_OK;
	}

	status_t RunRkvdec0Job(const MppPendingJob& job, void* mapped = NULL)
	{
		if (!job.valid || job.readBytes == 0 || job.readBytes > 128)
			return B_BAD_VALUE;
		Snapshot state = SnapshotNow();
		if ((state.repairStatus & (kVdpuRepair | kRkvdec0Repair))
				!= (kVdpuRepair | kRkvdec0Repair)
			|| (state.powerStatus & (kVdpuGate | kRkvdec0Gate)) != 0
			|| (state.idleStatus & (kVdpuIdle | kRkvdec0Idle)) != 0
			|| (state.memoryStatus & kRkvdec0Memory) != 0)
			return B_NOT_ALLOWED;
		void* blockAddress = mapped;
		AreaDeleter blockArea;
		if (blockAddress == NULL) {
			blockArea.SetTo(map_physical_memory("RKVDEC0 decode registers",
				0xfdc38000, B_PAGE_SIZE, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &blockAddress));
			if (blockArea.Get() < B_OK)
				return blockArea.Get();
		}
		volatile uint32_t* registers
			= (volatile uint32_t*)((uint8_t*)blockAddress + 0x100);
		const volatile uint32_t* iommu
			= (const volatile uint32_t*)((uint8_t*)blockAddress + 0x700);
		memory_read_barrier();
		if (registers[0] != kRkvdec0HardwareId || iommu[0] != 0
			|| (iommu[1] & 1) != 0 || (iommu[1] & 0x18) != 0x18)
			return B_NOT_ALLOWED;
		for (uint32_t index = 8; index < 288; index++) {
			if (index == 10 || (job.written[index / 32]
					& (1u << (index % 32))) == 0)
				continue;
			uint32_t value = job.registers[index];
			if (index == 11)
				value |= 1u << 4; // Poll completion without asserting an IRQ.
			registers[index] = value;
		}
		memory_write_barrier();
		registers[10] = 1;
		memory_write_barrier();
		const uint32_t terminal = (1u << 2) | (1u << 4)
			| (1u << 5) | (1u << 9);
		uint32_t status = 0;
		bigtime_t deadline = system_time() + 500000;
		do {
			memory_read_barrier();
			status = registers[224];
			if ((status & terminal) != 0)
				break;
			snooze(50);
		} while (system_time() < deadline);
		uint32_t readback[32] = {};
		for (uint32_t index = 0; index < job.readBytes / 4; index++)
			readback[index] = registers[224 + index];
		registers[224] = 0;
		memory_write_barrier();
		status_t result = user_memcpy((void*)(addr_t)job.readAddress,
			readback, job.readBytes);
		if (result != B_OK)
			return result;
		return (status & terminal) != 0 ? B_OK : B_TIMED_OUT;
	}

	status_t RunAv1Job(const MppPendingJob& job, void* mapped = NULL)
	{
		if (!job.valid || job.readBytes != sizeof(job.registers))
			return B_BAD_VALUE;
		Snapshot state = SnapshotNow();
		if ((state.repairStatus & (kVdpuRepair | kAv1Repair))
				!= (kVdpuRepair | kAv1Repair)
			|| (state.powerStatus & (kVdpuGate | kAv1Gate)) != 0
			|| (state.idleStatus & (kVdpuIdle | kAv1Idle)) != 0
			|| (state.memoryStatus & kAv1Memory) != 0
			|| state.clockSelect163 != 2 || state.clockGate68 != 0
			|| (state.softReset68 & 0x36) != 0) {
			return B_NOT_ALLOWED;
		}
		void* address = mapped;
		AreaDeleter area;
		if (address == NULL) {
			area.SetTo(map_physical_memory("AV1 VPU981 decode registers",
				0xfdc70000, B_PAGE_SIZE,
				B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
				B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address));
			if (area.Get() < B_OK)
				return area.Get();
		}
		volatile uint32_t* registers = (volatile uint32_t*)address;
		memory_read_barrier();
		if (registers[0] != kAv1HardwareId || registers[309] != kAv1BuildId)
			return B_NOT_ALLOWED;
		for (uint32_t index = 2; index < 512; index++) {
			if ((job.written[index / 32] & (1u << (index % 32))) == 0
				|| (index >= 299 && index <= 313)) {
				continue;
			}
			registers[index] = job.registers[index];
		}
		memory_write_barrier();
		registers[1] = job.registers[1] | (1u << 4);
		memory_write_barrier();
		const uint32_t terminal = (1u << 11) | (1u << 12) | (1u << 13)
			| (1u << 14) | (1u << 16) | (1u << 18) | (1u << 21)
			| (1u << 23);
		uint32_t status = 0;
		bigtime_t deadline = system_time() + 500000;
		do {
			memory_read_barrier();
			status = registers[1];
			if ((status & terminal) != 0)
				break;
			snooze(50);
		} while (system_time() < deadline);
		if ((status & (1u << 12)) == 0)
			dprintf("rk3588_vpu: AV1 terminal status=%#" B_PRIx32 "\n", status);
		uint32_t readback[512];
		for (uint32_t index = 0; index < 512; index++)
			readback[index] = registers[index];
		registers[1] = 0;
		memory_write_barrier();
		status_t result = user_memcpy((void*)(addr_t)job.readAddress,
			readback, sizeof(readback));
		if (result != B_OK)
			return result;
		return (status & terminal) != 0 ? B_OK : B_TIMED_OUT;
	}

	bool WaitDomainPower(uint32_t repairMask, uint32_t gateMask, bool on)
	{
		bigtime_t deadline = system_time() + 10000;
		do {
			memory_read_barrier();
			bool repaired = (ReadRegister(fPower, 0x290) & repairMask) != 0;
			bool gateOff = (ReadRegister(fPower, 0x180) & gateMask) != 0;
			if (repaired == on && gateOff != on)
				return true;
		} while (system_time() < deadline);
		return false;
	}

	bool WaitDomainMemory(uint32_t mask, bool on)
	{
		bigtime_t deadline = system_time() + 10000;
		do {
			memory_read_barrier();
			bool off = (ReadRegister(fPower, 0x1f8) & mask) != 0;
			if (off != on)
				return true;
		} while (system_time() < deadline);
		return false;
	}

	uint32_t ReadClock(uint32_t offset)
	{
		memory_read_barrier();
		return ReadRegister(fClock, offset);
	}

	// RKVENC0's clocks as the firmware leaves them: hclk/aclk root, hclk,
	// aclk and core (CRU gate 47 bits 0, 1, 4, 5, 6) ungated, and its BIU,
	// AHB, AXI and core resets (soft reset 47 bits 2-6) released.
	bool Rkvenc0ClocksReady()
	{
		return (ReadClock(0x8bc) & 0x73) == 0 && (ReadClock(0xabc) & 0x7c) == 0;
	}

	bool Venc0Ready()
	{
		Snapshot state = SnapshotNow();
		return (state.repairStatus & kVenc0Repair) != 0
			&& (state.powerStatus & (kVenc0Gate | kVcodecGate)) == 0
			&& (state.idleStatus & kVenc0Idle) == 0
			&& (state.memoryStatus & kVenc0Memory) == 0;
	}

	// BSP mpp_rkvenc2.c rkvenc_soft_reset(): stop the block cleanly.
	void ResetRkvenc(volatile uint32_t* registers)
	{
		registers[kEncoderInterruptMask] = 0x3ff;
		registers[kEncoderClearRegister] = 1;
		memory_write_barrier();
		bigtime_t deadline = system_time() + 1000;
		while ((registers[kEncoderInterruptStatus] & kEncoderSafeClearDone) == 0
			&& system_time() < deadline) {
		}
		registers[kEncoderClearRegister] = 3;
		memory_write_barrier();
		spin(5);
		registers[kEncoderClearRegister] = 0;
		registers[kEncoderInterruptClear] = 0xffffffff;
		registers[kEncoderInterruptStatus] = 0;
		memory_write_barrier();
	}

	/*!	One VEPU580 frame on core 0, as rkvenc_run()/rkvenc_irq() in the BSP
		driver do it, polled: the IOMMU must be in bypass (physical
		addresses), the start word goes last, the interrupt status is what
		MPP reads back for word 11. The dual-core handshake is turned off:
		with one core and one job at a time, a reference is complete before
		the next picture starts. */
	status_t RunRkvencJob(const EncoderJob& job, void* mapped,
		uint32_t& interruptStatus)
	{
		interruptStatus = 0;
		if (!job.valid || mapped == NULL)
			return B_BAD_VALUE;
		if (!Venc0Ready() || !Rkvenc0ClocksReady())
			return B_NOT_ALLOWED;
		volatile uint32_t* registers = (volatile uint32_t*)mapped;
		const volatile uint32_t* iommu
			= (const volatile uint32_t*)((uint8_t*)mapped + 0xf000);
		memory_read_barrier();
		if (iommu[0] != 0 || (iommu[1] & 1) != 0)
			return B_NOT_ALLOWED;

		registers[kEncoderClearRegister] = 2;	// force clear
		memory_write_barrier();
		spin(5);
		registers[kEncoderClearRegister] = 0;
		registers[0x5300 / 4] = 2;		// clear the hardware counters
		for (uint32_t index = 0; index < kEncoderRegisters; index++) {
			if (index == kEncoderStartRegister
				|| (job.written[index / 32] & (1u << (index % 32))) == 0)
				continue;
			uint32_t value = job.registers[index];
			if (index == kEncoderDualCore)
				value &= ~(uint32_t)0x30;	// dchs_txe, dchs_rxe
			registers[index] = value;
		}
		// Watchdog: about half a second of core clock (bits 0-23, in units
		// of 1024 cycles at up to 800 MHz); the poll below gives up at one.
		uint32_t watchdog = registers[kEncoderWatchdog] & 0xff000000;
		registers[kEncoderWatchdog] = watchdog | 0x60000;
		memory_write_barrier();
		registers[kEncoderStartRegister] = job.registers[kEncoderStartRegister];
		memory_write_barrier();

		uint32_t status = 0;
		bigtime_t deadline = system_time() + 1000000;
		do {
			memory_read_barrier();
			status = registers[kEncoderInterruptStatus];
			if ((status & (kEncoderDone | kEncoderErrors)) != 0)
				break;
			snooze(50);
		} while (system_time() < deadline);
		interruptStatus = status;
		registers[kEncoderInterruptClear] = status;
		memory_write_barrier();
		if ((status & kEncoderErrors) != 0 || (status & kEncoderDone) == 0) {
			ResetRkvenc(registers);
			return (status & kEncoderDone) == 0 && (status & kEncoderErrors) == 0
				? B_TIMED_OUT : B_IO_ERROR;
		}
		return B_OK;
	}

	// Copies the status words MPP asked for, after a job.
	status_t ReadRkvencResults(const EncoderJob& job, void* mapped,
		uint32_t interruptStatus)
	{
		const volatile uint32_t* registers = (const volatile uint32_t*)mapped;
		for (uint32_t i = 0; i < job.readCount; i++) {
			uint32_t words[180];
			uint32_t count = job.reads[i].count;
			if (count > B_COUNT_OF(words))
				return B_BAD_VALUE;
			for (uint32_t w = 0; w < count; w++) {
				uint32_t index = job.reads[i].first + w;
				words[w] = index == kEncoderInterruptStatus
					? interruptStatus : registers[index];
			}
			status_t status = user_memcpy((void*)(addr_t)job.reads[i].address,
				words, count * 4);
			if (status != B_OK)
				return status;
		}
		return B_OK;
	}

private:
	ResourceInfo fResources = {};
	AreaDeleter fClockArea;
	AreaDeleter fPowerArea;
	const volatile uint32_t* fClock = NULL;
	volatile uint32_t* fPower = NULL;
};


// #pragma mark - decoding sessions


// With sHardwareLock held.
static void
PowerDownDecoder(Controller* controller)
{
	if (controller->hardware == NULL)
		return;
	PowerHardware& hardware = *controller->hardware;
	if (controller->rkvdec0Powered) {
		PowerOffRkvdec0(hardware, controller->rkvdec0Session);
		controller->rkvdec0Powered = false;
	}
	if (controller->av1Powered) {
		PowerOffAv1(hardware, controller->av1Session);
		controller->av1Powered = false;
	}
	if (controller->vdpuPowered) {
		PowerOffVdpu(hardware, controller->vdpuSession);
		controller->vdpuPowered = false;
	}
	dprintf("rk3588_vpu: decoder powered down after %" B_PRIu32 " jobs,"
		" %" B_PRId64 " us a job (%" B_PRId64 " us decoding), slowest %"
		B_PRId64 " us; restore %u/%u/%u\n", controller->jobs,
		controller->jobs > 0 ? controller->jobTime / controller->jobs : 0,
		controller->jobs > 0 ? controller->decodeTime / controller->jobs : 0,
		controller->slowestJob, controller->vdpuSession.restoreResult,
		controller->rkvdec0Session.restoreResult,
		controller->av1Session.restoreResult);
	controller->jobs = 0;
	controller->jobTime = 0;
	controller->decodeTime = 0;
	controller->slowestJob = 0;
}


// With sHardwareLock held: brings up the parent and the decoder's domain if
// they are not up, and maps the decoder's registers once.
static status_t
EnsureDecoderPowered(Controller* controller, bool rkvdec)
{
	if (controller->hardware == NULL) {
		PowerHardware* hardware = new(std::nothrow) PowerHardware;
		if (hardware == NULL)
			return B_NO_MEMORY;
		status_t status = hardware->Init(controller->resources);
		if (status != B_OK) {
			delete hardware;
			return status;
		}
		controller->hardware = hardware;
	}
	PowerHardware& hardware = *controller->hardware;

	if (!controller->vdpuPowered) {
		if (!PowerOnVdpu(hardware, controller->vdpuSession)) {
			PowerOffVdpu(hardware, controller->vdpuSession);
			dprintf("rk3588_vpu: VDPU did not power up: %u/%u\n",
				controller->vdpuSession.result,
				controller->vdpuSession.restoreResult);
			return B_ERROR;
		}
		controller->vdpuPowered = true;
	}

	bool& powered = rkvdec ? controller->rkvdec0Powered
		: controller->av1Powered;
	PowerCycle& session = rkvdec ? controller->rkvdec0Session
		: controller->av1Session;
	if (!powered) {
		bool on = rkvdec ? PowerOnRkvdec0(hardware, session)
			: PowerOnAv1(hardware, session);
		if (!on) {
			if (rkvdec)
				PowerOffRkvdec0(hardware, session);
			else
				PowerOffAv1(hardware, session);
			dprintf("rk3588_vpu: %s did not power up: %u/%u\n",
				rkvdec ? "RKVDEC0" : "AV1", session.result,
				session.restoreResult);
			if (!controller->rkvdec0Powered && !controller->av1Powered)
				PowerDownDecoder(controller);
			return B_ERROR;
		}
		powered = true;
	}

	area_id& area = rkvdec ? controller->rkvdec0Area : controller->av1Area;
	void*& registers = rkvdec ? controller->rkvdec0Registers
		: controller->av1Registers;
	if (registers == NULL) {
		area = map_physical_memory(rkvdec ? "RKVDEC0 decode registers"
				: "AV1 VPU981 decode registers",
			rkvdec ? 0xfdc38000 : 0xfdc70000, B_PAGE_SIZE,
			B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &registers);
		if (area < B_OK) {
			registers = NULL;
			return area;
		}
	}
	return B_OK;
}


// #pragma mark - encoding sessions


// With sHardwareLock held.
static void
PowerDownEncoder(Controller* controller)
{
	if (controller->hardware == NULL || !controller->venc0Powered)
		return;
	PowerOffVenc0(*controller->hardware, controller->venc0Session);
	controller->venc0Powered = false;
	dprintf("rk3588_vpu: encoder powered down after %" B_PRIu32 " jobs,"
		" %" B_PRId64 " us a job, slowest %" B_PRId64 " us; restore %u\n",
		controller->encodeJobs, controller->encodeJobs > 0
			? controller->encodeTime / controller->encodeJobs : 0,
		controller->slowestEncode, controller->venc0Session.restoreResult);
	controller->encodeJobs = 0;
	controller->encodeTime = 0;
	controller->slowestEncode = 0;
}


// With sHardwareLock held: brings VENC0 up if it is not, after checking the
// clocks the firmware left, and maps RKVENC0's registers once.
static status_t
EnsureEncoderPowered(Controller* controller)
{
	if (controller->hardware == NULL) {
		PowerHardware* hardware = new(std::nothrow) PowerHardware;
		if (hardware == NULL)
			return B_NO_MEMORY;
		status_t status = hardware->Init(controller->resources);
		if (status != B_OK) {
			delete hardware;
			return status;
		}
		controller->hardware = hardware;
	}
	PowerHardware& hardware = *controller->hardware;
	if (!controller->venc0Powered) {
		if (!hardware.Rkvenc0ClocksReady()) {
			dprintf("rk3588_vpu: RKVENC0 clocks gated or in reset"
				" (gate %#" B_PRIx32 ", reset %#" B_PRIx32 "); not encoding\n",
				hardware.ReadClock(0x8bc), hardware.ReadClock(0xabc));
			return B_NOT_SUPPORTED;
		}
		if (!PowerOnVenc0(hardware, controller->venc0Session)) {
			PowerOffVenc0(hardware, controller->venc0Session);
			dprintf("rk3588_vpu: VENC0 did not power up: %u/%u\n",
				controller->venc0Session.result,
				controller->venc0Session.restoreResult);
			return B_ERROR;
		}
		controller->venc0Powered = true;
	}
	if (controller->rkvenc0Registers == NULL) {
		controller->rkvenc0Area = map_physical_memory("RKVENC0 registers",
			kRkvenc0Base, kRkvenc0Bytes, B_ANY_KERNEL_ADDRESS | B_UNCACHED_MEMORY,
			B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			&controller->rkvenc0Registers);
		if (controller->rkvenc0Area < B_OK) {
			controller->rkvenc0Registers = NULL;
			return controller->rkvenc0Area;
		}
		memory_read_barrier();
		dprintf("rk3588_vpu: RKVENC0 version %#" B_PRIx32 ", IOMMU dte %#"
			B_PRIx32 " status %#" B_PRIx32 "\n",
			((volatile uint32_t*)controller->rkvenc0Registers)[0],
			((volatile uint32_t*)controller->rkvenc0Registers)[0xf000 / 4],
			((volatile uint32_t*)controller->rkvenc0Registers)[0xf004 / 4]);
	}
	return B_OK;
}


// With sHardwareLock held: one validated RKVENC job, start to finish.
static status_t
RunEncoderJob(OpenHandle* opened)
{
	Controller* controller = opened->controller;
	EncoderJob& job = *opened->encoderJob;
	status_t status = EnsureEncoderPowered(controller);
	if (status != B_OK) {
		job.valid = false;
		return status;
	}
	if (!opened->encodes) {
		opened->encodes = true;
		controller->encodeClients++;
	}
	bigtime_t started = system_time();
	for (uint32_t i = 0; i < job.bufferCount; i++) {
		if (job.shared[i])
			SyncBufferForDevice(job.buffers[i]);
	}
	uint32_t interruptStatus = 0;
	status = controller->hardware->RunRkvencJob(job,
		controller->rkvenc0Registers, interruptStatus);
	for (uint32_t i = 0; i < job.bufferCount; i++) {
		if (job.shared[i])
			SyncBufferForDevice(job.buffers[i]);
	}
	if (status == B_OK) {
		status = controller->hardware->ReadRkvencResults(job,
			controller->rkvenc0Registers, interruptStatus);
	}
	bigtime_t spent = system_time() - started;
	job.valid = false;
	controller->encodeJobs++;
	controller->encodeTime += spent;
	if (spent > controller->slowestEncode)
		controller->slowestEncode = spent;
	opened->mppCompletionReady = status == B_OK;
	if (status != B_OK) {
		dprintf("rk3588_vpu: encode job failed (%s, interrupt status %#"
			B_PRIx32 ") after %" B_PRId64 " us; powering the encoder down\n",
			strerror(status), interruptStatus, spent);
		PowerDownEncoder(controller);
	}
	return status;
}


static float
SupportsDevice(device_node* parent)
{
	const char* bus;
	if (sDeviceManager->get_attr_string(parent, B_DEVICE_BUS, &bus, false) != B_OK
		|| strcmp(bus, "fdt") != 0) {
		return 0;
	}
	ResourceInfo resources;
	return ReadResources(parent, resources) ? 0.8f : 0;
}


static status_t
RegisterDevice(device_node* parent)
{
	device_attr attributes[] = {
		{B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "RK3588 VPU resource interface"}},
		{NULL}
	};
	return sDeviceManager->register_node(parent, DRIVER_NAME, attributes, NULL, NULL);
}


static status_t
InitDriver(device_node* node, void** cookie)
{
	Controller* controller = (Controller*)calloc(1, sizeof(Controller));
	if (controller == NULL)
		return B_NO_MEMORY;
	controller->poolArea = -1;
	controller->rkvdec0Area = -1;
	controller->av1Area = -1;
	controller->rkvenc0Area = -1;
	size_t poolMegabytes = 0;
	device_node* parent = sDeviceManager->get_parent_node(node);
	bool valid = ReadResources(parent, controller->resources);
	void* settings = load_driver_settings("rk3588_vpu");
	if (valid && settings != NULL) {
		const char* profile = get_driver_parameter(settings, "power_profile", "", "");
		controller->cycleEnabled
			= strcmp(profile, "rock5-itx-edk2-v1.1-vdpu-power") == 0;
		profile = get_driver_parameter(settings, "rkvdec0_profile", "", "");
		controller->rkvdec0CycleEnabled
			= strcmp(profile, "rock5-itx-edk2-v1.1-rkvdec0-power") == 0;
		profile = get_driver_parameter(settings, "av1_profile", "", "");
		controller->av1CycleEnabled
			= strcmp(profile, "rock5-itx-edk2-v1.1-av1-power") == 0;
		profile = get_driver_parameter(settings, "decode_profile", "", "");
		controller->decodeEnabled = strcmp(profile,
			"rock5-itx-edk2-v1.1-rkvdec0-h264") == 0
			|| strcmp(profile,
				"rock5-itx-edk2-v1.1-rkvdec0-h264-h265") == 0;
		profile = get_driver_parameter(settings, "av1_decode_profile", "", "");
		controller->av1DecodeEnabled = strcmp(profile,
			"rock5-itx-edk2-v1.1-vpu981-av1") == 0;
		// Installations made before the encoder have the decoder's profile
		// but no encode_profile line (the settings file is copied once):
		// they encode too, unless the line names something else ("off").
		profile = get_driver_parameter(settings, "encode_profile", "", "");
		controller->encodeEnabled = strcmp(profile,
			"rock5-itx-edk2-v1.1-rkvenc0-h264-h265") == 0
			|| (profile[0] == '\0' && controller->decodeEnabled);
		if (controller->decodeEnabled || controller->av1DecodeEnabled
			|| controller->encodeEnabled) {
			const char* pool = get_driver_parameter(settings, "dma_pool_mb",
				NULL, NULL);
			poolMegabytes = pool != NULL ? strtoul(pool, NULL, 10)
				: kDefaultPoolMegabytes;
		}
	}
	if (settings != NULL)
		unload_driver_settings(settings);
	if (parent != NULL)
		sDeviceManager->put_node(parent);
	if (!valid) {
		free(controller);
		return B_NOT_SUPPORTED;
	}
	controller->node = node;
	CreateDmaPool(controller, poolMegabytes);
	dprintf("rk3588_vpu: validated VDPU and IOMMU resources;"
		" VDPU power cycle %s, RKVDEC0 power cycle %s, AV1 power cycle %s,"
		" RKVDEC decode %s, AV1 decode %s, RKVENC encode %s\n",
		controller->cycleEnabled ? "enabled" : "disabled",
		controller->rkvdec0CycleEnabled ? "enabled" : "disabled",
		controller->av1CycleEnabled ? "enabled" : "disabled",
		controller->decodeEnabled ? "enabled" : "disabled",
		controller->av1DecodeEnabled ? "enabled" : "disabled",
		controller->encodeEnabled ? "enabled" : "disabled");
	*cookie = controller;
	return B_OK;
}

static void
UninitDriver(void* cookie)
{
	Controller* controller = (Controller*)cookie;
	{
		MutexLocker locker(sHardwareLock);
		PowerDownDecoder(controller);
		PowerDownEncoder(controller);
		delete controller->hardware;
		controller->hardware = NULL;
		if (controller->rkvdec0Area >= B_OK)
			delete_area(controller->rkvdec0Area);
		if (controller->av1Area >= B_OK)
			delete_area(controller->av1Area);
		if (controller->rkvenc0Area >= B_OK)
			delete_area(controller->rkvenc0Area);
	}
	DeleteDmaPool(controller);
	free(controller);
}
static status_t InitDevice(void* driver, void** device) { *device = driver; return B_OK; }
static void UninitDevice(void*) {}

static status_t
PublishDevices(void* cookie)
{
	return sDeviceManager->publish_device(((Controller*)cookie)->node,
		"video/rk3588_vpu/0", DEVICE_NAME);
}

static status_t
Open(void* cookie, const char*, int mode, void** handle)
{
	Controller* controller = (Controller*)cookie;
	int access = mode & O_ACCMODE;
	if (access != O_RDONLY && access != O_RDWR)
		return B_NOT_ALLOWED;
	if (access == O_RDWR && ((!controller->cycleEnabled
			&& !controller->rkvdec0CycleEnabled && !controller->av1CycleEnabled
			&& !controller->decodeEnabled && !controller->av1DecodeEnabled
			&& !controller->encodeEnabled)
			|| (geteuid() != 0 && !controller->decodeEnabled
				&& !controller->av1DecodeEnabled && !controller->encodeEnabled)))
		return B_NOT_ALLOWED;
	OpenHandle* opened = (OpenHandle*)calloc(1, sizeof(OpenHandle));
	if (opened == NULL)
		return B_NO_MEMORY;
	opened->controller = controller;
	opened->owner = team_get_current_team_id();
	opened->write = access == O_RDWR;
	opened->privileged = geteuid() == 0;
	opened->mppClientType = UINT32_MAX;
	{
		MutexLocker locker(sHardwareLock);
		opened->next = sOpenHandles;
		sOpenHandles = opened;
	}
	*handle = opened;
	return B_OK;
}

static status_t Close(void*) { return B_OK; }
static status_t
Free(void* cookie)
{
	OpenHandle* opened = (OpenHandle*)cookie;
	MutexLocker locker(sHardwareLock);
	for (OpenHandle** slot = &sOpenHandles; *slot != NULL;
		slot = &(*slot)->next) {
		if (*slot == opened) {
			*slot = opened->next;
			break;
		}
	}
	if (opened->decodes && --opened->controller->decodeClients == 0)
		PowerDownDecoder(opened->controller);
	if (opened->encodes && --opened->controller->encodeClients == 0)
		PowerDownEncoder(opened->controller);
	free(opened->encoderJob);
	while (opened->buffers != NULL) {
		DmaBuffer* buffer = opened->buffers;
		opened->buffers = buffer->next;
		DeleteBuffer(opened, buffer);
	}
	free(opened);
	return B_OK;
}
static status_t Read(void*, off_t, void*, size_t* size) { *size = 0; return B_NOT_ALLOWED; }
static status_t Write(void*, off_t, const void*, size_t* size) { *size = 0; return B_NOT_ALLOWED; }

static status_t
Control(void* cookie, uint32 op, void* buffer, size_t length)
{
	if (buffer == NULL)
		return B_BAD_ADDRESS;
	OpenHandle* opened = (OpenHandle*)cookie;
	Controller* controller = opened->controller;
	if (op == kGetMppValidation) {
		if (length != sizeof(MppValidationStats))
			return B_BAD_VALUE;
		MutexLocker locker(sHardwareLock);
		return user_memcpy(buffer, &controller->mppStats,
			sizeof(controller->mppStats));
	}
	if (op == kMppServiceV1) {
		static_assert(sizeof(MppServiceRequest) == 24,
			"Rockchip MPP V1 request layout changed");
		if (!opened->write || opened->owner != team_get_current_team_id())
			return B_NOT_ALLOWED;
		if (length != sizeof(MppServiceRequest))
			return B_BAD_VALUE;
		MppServiceRequest request;
		status_t status = user_memcpy(&request, buffer, sizeof(request));
		if (status != B_OK)
			return status;
		if (request.command == 0x300) {
			if ((request.flags & ~((uint32_t)0x12)) != 0
				|| request.bytes != 0 || request.offset != 0
				|| request.dataAddress != 0)
				return B_BAD_VALUE;
			MutexLocker locker(sHardwareLock);
			if (!opened->mppCompletionReady)
				return B_WOULD_BLOCK;
			opened->mppCompletionReady = false;
			return B_OK;
		}
		if (request.command >= 0x200 && request.command <= 0x203
			&& opened->mppClientType == kMppClientRkvenc) {
			if (!controller->encodeEnabled || opened->encoderJob == NULL)
				return B_NOT_ALLOWED;
			MutexLocker locker(sHardwareLock);
			status = ValidateEncoderJob(opened, (const MppServiceRequest*)buffer);
			if (status != B_OK) {
				static int32 sRefusals = 0;
				if (atomic_add(&sRefusals, 1) < 8) {
					dprintf("rk3588_vpu: encode job refused: %s\n",
						strerror(status));
				}
				return status;
			}
			return RunEncoderJob(opened);
		}
		if (request.command >= 0x200 && request.command <= 0x203) {
			if (opened->mppClientType != kMppClientRkvdec
				&& opened->mppClientType != kMppClientAv1)
				return B_NOT_ALLOWED;
			MutexLocker locker(sHardwareLock);
			status = ValidateMppJob(opened,
				(const MppServiceRequest*)buffer);
			if (status != B_OK)
				return status;
			bool rkvdec = opened->mppClientType == kMppClientRkvdec;
			if (!controller->cycleEnabled
				|| (rkvdec && (!controller->decodeEnabled
					|| !controller->rkvdec0CycleEnabled))
				|| (!rkvdec && (!controller->av1DecodeEnabled
					|| !controller->av1CycleEnabled))) {
				return B_NOT_ALLOWED;
			}
			status = EnsureDecoderPowered(controller, rkvdec);
			if (status != B_OK) {
				opened->mppJob.valid = false;
				controller->mppStats.lastStatus = status;
				return status;
			}
			if (!opened->decodes) {
				opened->decodes = true;
				controller->decodeClients++;
			}
			bigtime_t started = system_time();
			SyncJobBuffers(opened->mppJob);
			bigtime_t decodeStarted = system_time();
			status = rkvdec
				? controller->hardware->RunRkvdec0Job(opened->mppJob,
					controller->rkvdec0Registers)
				: controller->hardware->RunAv1Job(opened->mppJob,
					controller->av1Registers);
			controller->decodeTime += system_time() - decodeStarted;
			SyncJobBuffers(opened->mppJob);
			bigtime_t spent = system_time() - started;
			opened->mppJob.valid = false;
			controller->jobs++;
			controller->jobTime += spent;
			if (spent > controller->slowestJob)
				controller->slowestJob = spent;
			opened->mppCompletionReady = status == B_OK;
			controller->mppStats.lastStatus = status;
			if (status != B_OK) {
				// Start the next job from a block that was powered off.
				dprintf("rk3588_vpu: MPP job failed (%s) after %" B_PRId64
					" us; powering the decoder down\n", strerror(status),
					spent);
				PowerDownDecoder(controller);
			}
			return status;
		}
		bool mappingRequest = request.command == kMppAttachBuffer
			|| request.command == kMppDetachBuffer;
		if ((request.flags != 0
				&& !(mappingRequest && request.flags == 2))
			|| request.offset != 0 || request.dataAddress == 0)
			return B_BAD_VALUE;
		void* data = (void*)(addr_t)request.dataAddress;
		if (request.command == kMppQueryHardware) {
			if (request.bytes != 0)
				return B_BAD_VALUE;
			const uint32_t supported = (1u << kMppClientRkvdec)
				| (1u << kMppClientAv1)
				| (controller->encodeEnabled ? 1u << kMppClientRkvenc : 0);
			return user_memcpy(data, &supported, sizeof(supported));
		}
		if (request.bytes != sizeof(uint32_t))
			return B_BAD_VALUE;
		if (request.command == kMppAttachBuffer
			|| request.command == kMppDetachBuffer) {
			if (opened->mppClientType != kMppClientRkvdec
				&& opened->mppClientType != kMppClientAv1
				&& opened->mppClientType != kMppClientRkvenc)
				return B_NOT_ALLOWED;
			uint32_t handle;
			status = user_memcpy(&handle, data, sizeof(handle));
			if (status != B_OK)
				return status;
			MutexLocker locker(sHardwareLock);
			return handle != 0 && FindTeamBuffer(opened->owner, handle) != NULL
				? B_OK : B_ENTRY_NOT_FOUND;
		}
		if (request.command == kMppInitClient) {
			uint32_t client;
			status = user_memcpy(&client, data, sizeof(client));
			if (status != B_OK)
				return status;
			if (client != kMppClientRkvdec && client != kMppClientAv1
				&& (client != kMppClientRkvenc || !controller->encodeEnabled))
				return B_NOT_SUPPORTED;
			EncoderJob* job = NULL;
			if (client == kMppClientRkvenc) {
				job = (EncoderJob*)calloc(1, sizeof(EncoderJob));
				if (job == NULL)
					return B_NO_MEMORY;
			}
			MutexLocker locker(sHardwareLock);
			if (opened->mppClientType != UINT32_MAX) {
				free(job);
				return B_BUSY;
			}
			opened->mppClientType = client;
			opened->encoderJob = job;
			return B_OK;
		}
		if (request.command == kMppQueryId) {
			if (opened->mppClientType != kMppClientRkvdec
				&& opened->mppClientType != kMppClientAv1
				&& opened->mppClientType != kMppClientRkvenc)
				return B_NOT_ALLOWED;
			const uint32_t id = opened->mppClientType == kMppClientRkvdec
				? kRkvdec0HardwareId : opened->mppClientType == kMppClientAv1
				? kAv1HardwareId : kRkvencHardwareId;
			return user_memcpy(data, &id, sizeof(id));
		}
		return B_NOT_SUPPORTED;
	}
	if (op == kAllocBuffer || op == kFreeBuffer || op == kValidateBuffer) {
		if (!opened->write || opened->owner != team_get_current_team_id())
			return B_NOT_ALLOWED;
		MutexLocker locker(sHardwareLock);
		if (op == kValidateBuffer) {
			if (length != sizeof(BufferValidation))
				return B_BAD_VALUE;
			BufferValidation request;
			status_t status = user_memcpy(&request, buffer, sizeof(request));
			if (status != B_OK)
				return status;
			if (request.version != kBufferVersion || request.handle == 0
				|| request.bytes == 0)
				return B_BAD_VALUE;
			DmaBuffer* allocation = FindTeamBuffer(opened->owner, request.handle);
			if (allocation == NULL)
				return B_ENTRY_NOT_FOUND;
			if (request.offset >= allocation->bytes
				|| request.bytes > allocation->bytes - request.offset)
				return B_BAD_VALUE;
			return B_OK;
		}
		if (op == kAllocBuffer) {
			if (length != sizeof(BufferAllocation))
				return B_BAD_VALUE;
			BufferAllocation request;
			status_t status = user_memcpy(&request, buffer, sizeof(request));
			if (status != B_OK)
				return status;
			status = AllocateBuffer(opened, request);
			if (status != B_OK)
				return status;
			status = user_memcpy(buffer, &request, sizeof(request));
			if (status != B_OK) {
				DmaBuffer* allocated = opened->buffers;
				opened->buffers = allocated->next;
				opened->allocatedBytes -= allocated->bytes;
				DeleteBuffer(opened, allocated);
			}
			return status;
		}
		if (length != sizeof(BufferRelease))
			return B_BAD_VALUE;
		BufferRelease request;
		status_t status = user_memcpy(&request, buffer, sizeof(request));
		if (status != B_OK)
			return status;
		if (request.version != kBufferVersion || request.handle == 0)
			return B_BAD_VALUE;
		for (DmaBuffer** slot = &opened->buffers; *slot != NULL;
			slot = &(*slot)->next) {
			if ((*slot)->handle == request.handle) {
				DmaBuffer* allocated = *slot;
				*slot = allocated->next;
				opened->allocatedBytes -= allocated->bytes;
				DeleteBuffer(opened, allocated);
				return B_OK;
			}
		}
		return B_ENTRY_NOT_FOUND;
	}
	if (op == kGetResources) {
		if (length != sizeof(ResourceInfo))
			return B_BAD_VALUE;
		return user_memcpy(buffer, &controller->resources, sizeof(ResourceInfo));
	}
	if (op == kGetSnapshot) {
		if (length != sizeof(Snapshot))
			return B_BAD_VALUE;
		Snapshot snapshot;
		status_t status = ReadSnapshot(controller->resources, snapshot);
		if (status != B_OK)
			return status;
		return user_memcpy(buffer, &snapshot, sizeof(Snapshot));
	}
	if (op == kProbeDma) {
		if (length != sizeof(DmaProbe))
			return B_BAD_VALUE;
		if (!opened->write || !opened->privileged)
			return B_NOT_ALLOWED;
		DmaProbe probe = {};
		probe.version = kDmaProbeVersion;
		probe.status = ProbeDmaAddress(probe);
		return user_memcpy(buffer, &probe, sizeof(probe));
	}
	if (op == kCycleVdpuPower || op == kProbeDecoder
		|| op == kCycleRkvdec0Power || op == kCycleRkvdec0WithVdpu
		|| op == kProbeRkvdec0Registers || op == kCycleAv1WithVdpu
		|| op == kProbeAv1Registers) {
		if (length != (op == kProbeDecoder ? sizeof(DecoderProbe)
				: op == kProbeRkvdec0Registers ? sizeof(Rkvdec0RegisterProbe)
				: op == kProbeAv1Registers ? sizeof(Av1RegisterProbe)
				: op == kCycleAv1WithVdpu ? sizeof(ParentChildCycle)
				: op == kCycleRkvdec0WithVdpu ? sizeof(ParentChildCycle)
				: sizeof(PowerCycle)))
			return B_BAD_VALUE;
		if (!opened->write || !opened->privileged)
			return B_NOT_ALLOWED;
		if ((op == kCycleVdpuPower || op == kProbeDecoder)
			&& !controller->cycleEnabled) {
			return B_NOT_ALLOWED;
		}
		if ((op == kCycleRkvdec0Power || op == kCycleRkvdec0WithVdpu
				|| op == kProbeRkvdec0Registers)
			&& !controller->rkvdec0CycleEnabled)
			return B_NOT_ALLOWED;
		if ((op == kCycleRkvdec0WithVdpu || op == kProbeRkvdec0Registers)
			&& !controller->cycleEnabled)
			return B_NOT_ALLOWED;
		if ((op == kCycleAv1WithVdpu || op == kProbeAv1Registers)
			&& (!controller->cycleEnabled || !controller->av1CycleEnabled))
			return B_NOT_ALLOWED;
		MutexLocker locker(sHardwareLock);
		PowerHardware hardware;
		status_t status = hardware.Init(controller->resources);
		if (status != B_OK)
			return status;
		DecoderProbe probe = {};
		probe.version = kDecoderProbeVersion;
		probe.readStatus = B_NOT_SUPPORTED;
		if (op == kProbeDecoder) {
			CycleVdpuPower(hardware, probe.cycle, [&] {
				probe.readStatus = hardware.ReadDecoderState(probe);
			});
		} else if (op == kProbeAv1Registers) {
			Av1RegisterProbe result = {};
			result.version = 1;
			result.readStatus = B_NOT_SUPPORTED;
			result.cycle.version = 1;
			CycleVdpuPower(hardware, result.cycle.parent, [&] {
				CycleAv1Power(hardware, result.cycle.child, [&] {
					result.readStatus = hardware.ReadAv1Registers(result);
				});
			});
			dprintf("rk3588_vpu: AV1 register observation status=%#x"
				" parent=%u/%u child=%u/%u build=%#x synthesis=%#x\n",
				result.readStatus, result.cycle.parent.result,
				result.cycle.parent.restoreResult, result.cycle.child.result,
				result.cycle.child.restoreResult, result.buildId,
				result.synthesisId);
			return user_memcpy(buffer, &result, sizeof(result));
		} else if (op == kProbeRkvdec0Registers) {
			Rkvdec0RegisterProbe result = {};
			result.version = 1;
			result.readStatus = B_NOT_SUPPORTED;
			result.cycle.version = 1;
			CycleVdpuPower(hardware, result.cycle.parent, [&] {
				CycleRkvdec0Power(hardware, result.cycle.child, [&] {
					result.readStatus = hardware.ReadRkvdec0Registers(result);
				});
			});
			dprintf("rk3588_vpu: RKVDEC0 register observation status=%#x"
				" parent=%u/%u child=%u/%u\n", result.readStatus,
				result.cycle.parent.result,
				result.cycle.parent.restoreResult,
				result.cycle.child.result,
				result.cycle.child.restoreResult);
			return user_memcpy(buffer, &result, sizeof(result));
		} else if (op == kCycleAv1WithVdpu) {
			ParentChildCycle pair = {};
			pair.version = 1;
			CycleVdpuPower(hardware, pair.parent, [&] {
				CycleAv1Power(hardware, pair.child);
			});
			dprintf("rk3588_vpu: AV1 parent cycle result=%u restore=%u;"
				" child result=%u restore=%u\n", pair.parent.result,
				pair.parent.restoreResult, pair.child.result,
				pair.child.restoreResult);
			return user_memcpy(buffer, &pair, sizeof(pair));
		} else if (op == kCycleRkvdec0WithVdpu) {
			ParentChildCycle pair = {};
			pair.version = 1;
			CycleVdpuPower(hardware, pair.parent, [&] {
				CycleRkvdec0Power(hardware, pair.child);
			});
			dprintf("rk3588_vpu: parent cycle result=%u restore=%u;"
				" child result=%u restore=%u\n",
				pair.parent.result, pair.parent.restoreResult,
				pair.child.result, pair.child.restoreResult);
			return user_memcpy(buffer, &pair, sizeof(pair));
		} else if (op == kCycleRkvdec0Power)
			CycleRkvdec0Power(hardware, probe.cycle);
		else
			CycleVdpuPower(hardware, probe.cycle);
		dprintf("rk3588_vpu: power cycle result=%u restore=%u flags=%#x\n",
			probe.cycle.result, probe.cycle.restoreResult, probe.cycle.flags);
		if (op == kProbeDecoder)
			return user_memcpy(buffer, &probe, sizeof(probe));
		return user_memcpy(buffer, &probe.cycle, sizeof(probe.cycle));
	}
	return B_DEV_INVALID_IOCTL;
}

module_dependency module_dependencies[] = {
	{B_DEVICE_MANAGER_MODULE_NAME, (module_info**)&sDeviceManager},
	{}
};

static device_module_info sDevice = {
	{DEVICE_NAME, 0, NULL},
	InitDevice, UninitDevice, NULL,
	Open, Close, Free, Read, Write, NULL, Control, NULL, NULL
};

static driver_module_info sDriver = {
	{DRIVER_NAME, 0, NULL},
	SupportsDevice, RegisterDevice, InitDriver, UninitDriver, PublishDevices,
	NULL, NULL, NULL, NULL
};

module_info* modules[] = {
	(module_info*)&sDriver,
	(module_info*)&sDevice,
	NULL
};
