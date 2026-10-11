/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Device.h"
#include "Sdma.h"
#include "Gart.h"
#include "Gfx.h"
#include "GfxCommandValidator.h"
#include "GpuPageTable.h"
#include "Uvd.h"
#include "FirmwareLoader.h"
#include "VramAllocator.h"
#include "UserMemory.h"
#include <KernelExport.h>
#include <condition_variable.h>
#include <lock.h>
#include <team.h>
#include <vm/vm.h>
#include <fcntl.h>
#include <stdlib.h>

struct Buffer {
	Buffer* next;
	uint64 handle, offset, bytes, gpu;
	area_id area;
	volatile uint32* cpu;
	uint32 references;
	bool poisoned, system;
};

struct VmBinding {
	VmBinding* next;
	Buffer* buffer;
	uint64 address, bytes, offset;
	uint32 permissions;
};

struct ClientVM {
	ClientVM* nextQuarantined;
	GpuPageTable table;
	GpuPageTable::Allocation commands;
	GpuPageTable::Allocation copiedCommands;
	VmBinding* bindings;
	uint64 bytes;
	uint32 mappings;
};

struct AmdgpuClient {
	AmdgpuClient* next;
	status_t loss;
	uint64 lossGeneration;
	team_id team;
	uint32 flags, references, bufferCount, pending, graphicsPending;
	uint64 bytes, systemBytes, submitted, completed, failedFence;
	status_t failure;
	Buffer* buffers;
	UvdSession* video;
	bool videoBusy, graphicsBusy;
	ClientVM* vm;
};

struct Job {
	Job* next;
	AmdgpuClient* client;
	Buffer* source;
	Buffer* destination;
	amdgpu_dma_submit command;
	uint32* graphicsCommands;
	uint32 graphicsDwords;
};

static mutex sMutex = MUTEX_INITIALIZER("amdgpu buffers");
static ConditionVariable sCompleted;
static VramAllocator sAllocator = {};
static VramAllocator sGartAllocator = {};
static Gart sGart = {};
static GfxEngine sGfx = {};
static UvdEngine sUvd = {};
static amdgpu::AtomVramReservation sReservation;
static amdgpu::AtomRenderInfo sRenderInfo;
static const uint64 kMaxBufferBytes = 64ULL << 20;
static const uint64 kMaxMappedBytes = 16ULL << 30;
static const uint32 kMaxMappings = 1024, kMaxVMClients = 32, kMaxBuffers = 256;
static const uint32 kMaxIbBytes = 65536, kIbAddressAlignment = 256, kIbSizeAlignment = 1024;
static const uint64 kClientAddressStart = AMDGPU_COPY_IB_ADDRESS + kMaxIbBytes;
static bool sUvdClocksQualified;
static SdmaEngine sEngine = {};
static amdgpu_info sInfo;
static bool sActive, sStopping, sVideoExecuting, sGraphicsExecuting;
static status_t sFault;
static sem_id sJobs = -1;
static thread_id sWorker = -1;
static Job* sFirst;
static Job* sLast;
static uint32 sPending, sDmaPending;
static uint64 sNextHandle = 1;
static uint32 sNextVideoHandle = 1;
static uint32 sVideoSessions, sVMCount;
static uint32 sLiveHandles;
static Buffer* sQuarantinedBuffers;
static ClientVM* sQuarantinedVMs;
static UvdSession* sQuarantinedVideos;
static amdgpu_recovery_info sRecovery;
static bool sRecoveryAttempted;
static AmdgpuClient* sClients;
static bool sAutomaticRecovery = true, sAutomaticPending;
static uint64 sAutomaticAttempts, sAutomaticSuccesses;
static status_t sAutomaticStatus;
static void MaybeRecoverAutomatically();
static void ReleaseVM(AmdgpuClient* client);
static void ReleaseVideo(AmdgpuClient* client);
static void WaitGraphicsIdle();
static status_t ExecuteQueuedGraphics(Job* job);

// sMutex serializes the first fault with open, close, and recovery. Retain the
// first loss on every old file even if its queue was empty or fully successful.
// Recovery clears only the device fault; it cannot revive any of these files.
static void
SetFault(status_t status)
{
	if (status == B_OK) return;
	if (sFault == B_OK) {
		sFault = status;
		sAutomaticPending = sAutomaticRecovery;
		// One wake also handles a synchronous fault with no queued jobs. The
		// worker waits for execution owners and drains canceled jobs first.
		if (sAutomaticPending && sJobs >= 0) release_sem(sJobs);
	}
	for (AmdgpuClient* client = sClients; client != NULL; client = client->next) {
		if (client->loss == B_OK) {
			client->loss = sFault;
			client->lossGeneration = sRecovery.generation;
		}
	}
}

// Called with sMutex held, including cleanup and the DMA worker. An async
// fault must quarantine allocations even if the client closes immediately.
static void
ObserveInterruptFault()
{
	status_t status = sGfx.interrupts.Error();
	if (status != B_OK) {
		if (sFault == B_OK) SetFault(status);
		// The execution owner also observes the atomic IRQ error. Do not
		// mutate its engine state or halt it from a concurrent cleanup call.
		if (!sGraphicsExecuting && !sGfx.faulted) {
			sGfx.faulted = true;
			sGfx.Halt();
		}
	}
}

static void
PutBuffer(Buffer* bo)
{
	if (bo == NULL || --bo->references != 0)
		return;
	// GART invalidation shares VM_INVALIDATE_REQUEST with GFX VM binding.
	// No reference remains, so this BO cannot be freed by another waiter.
	if (bo->system) WaitGraphicsIdle();
	ObserveInterruptFault();
	// RAM must remain wired on failure: deleting its area would return pages
	// to the OS while a late GPU write may still be possible. VRAM mappings
	// can be deleted, but the allocator still quarantines their physical pages.
	if (bo->system && !bo->poisoned && sFault == B_OK) {
		status_t status = sGart.Unbind(bo->offset, bo->bytes);
		if (status != B_OK)
			SetFault(status);
	}
	if (bo->poisoned || sFault != B_OK) {
		// Retain both backing storage and its ownership record. No allocation
		// on this fault path, and no lost wired RAM/VRAM ranges after close.
		bo->next = sQuarantinedBuffers;
		sQuarantinedBuffers = bo;
		return;
	}
	if (bo->area >= 0 && (!bo->system || (!bo->poisoned && sFault == B_OK)))
		delete_area(bo->area);
	if (!bo->poisoned && sFault == B_OK) {
		VramAllocator& allocator = bo->system ? sGartAllocator : sAllocator;
		allocator.Free(bo->offset, bo->bytes);
	}
	free(bo);
}

static void
PutClient(AmdgpuClient* client)
{
	if (--client->references == 0) {
		// Closing the file may have deferred the VM until its queued jobs
		// retire. The worker is no longer using it when dropping this reference.
		ReleaseVM(client);
		AmdgpuClient** link = &sClients;
		while (*link != client) link = &(*link)->next;
		*link = client->next;
		free(client);
	}
}

static Buffer*
Lookup(AmdgpuClient* client, uint64 handle)
{
	for (Buffer* bo = client->buffers; bo != NULL; bo = bo->next) {
		if (bo->handle == handle)
			return bo;
	}
	return NULL;
}

static status_t
Executor(void*)
{
	while (acquire_sem(sJobs) == B_OK) {
		mutex_lock(&sMutex);
		// Decode owns SDMA for its private clear/readback. Ordinary DMA must
		// also stay ordered against graphics accessing client-owned buffers.
		while (sGraphicsExecuting || (sVideoExecuting
			&& (sFirst == NULL || sFirst->graphicsCommands == NULL)))
			sCompleted.Wait(&sMutex);
		ObserveInterruptFault();
		Job* job = sFirst;
		if (job == NULL) {
			MaybeRecoverAutomatically();
			bool stop = sStopping;
			mutex_unlock(&sMutex);
			if (stop)
				break;
			continue;
		}
		sFirst = job->next;
		if (sFirst == NULL)
			sLast = NULL;
		status_t status = sFault;
		if (status == B_OK && job->graphicsCommands != NULL) {
			// This helper reserves graphics and drops sMutex during execution.
			// Already running UVD may overlap this job; FIFO DMA still waits.
			status = ExecuteQueuedGraphics(job);
		} else if (status == B_OK) {
			const amdgpu_dma_submit& c = job->command;
			uint64 source = job->source != NULL
				? job->source->gpu + c.source_offset : 0;
			uint64 destination = job->destination->gpu
				+ c.destination_offset;
			mutex_unlock(&sMutex);
			status = sEngine.Execute(c.operation, source, destination, c.bytes, c.value);
			mutex_lock(&sMutex);
		}
		ObserveInterruptFault();
		if (status == B_OK && sFault != B_OK) status = B_DEV_NOT_READY;
		if (status != B_OK) {
			SetFault(status);
			if (job->client->failedFence == 0) {
				job->client->failedFence = job->command.fence;
				job->client->failure = status;
			}
		}
		job->client->completed = job->command.fence;
		job->client->pending--;
		if (job->graphicsCommands != NULL) job->client->graphicsPending--;
		else sDmaPending--;
		sPending--;
		PutBuffer(job->source);
		PutBuffer(job->destination);
		PutClient(job->client);
		free(job->graphicsCommands);
		free(job);
		sCompleted.NotifyAll();
		MaybeRecoverAutomatically();
		mutex_unlock(&sMutex);
	}
	return B_OK;
}

AmdgpuClient*
amdgpu_client_open(uint32 flags)
{
	AmdgpuClient* client = (AmdgpuClient*)calloc(1, sizeof(AmdgpuClient));
	if (client != NULL) {
		client->team = team_get_current_team_id();
		client->flags = flags;
		client->references = 1;
		mutex_lock(&sMutex);
		client->loss = sFault;
		client->lossGeneration = sRecovery.generation;
		client->next = sClients;
		sClients = client;
		mutex_unlock(&sMutex);
	}
	return client;
}

void
amdgpu_client_free(AmdgpuClient* client)
{
	mutex_lock(&sMutex);
	ObserveInterruptFault();
	ReleaseVideo(client);
	if (client->graphicsPending == 0) ReleaseVM(client);
	while (client->buffers != NULL) {
		Buffer* bo = client->buffers;
		client->buffers = bo->next;
		sLiveHandles--;
		if (bo->area >= 0 && vm_change_clones_to_null_areas(bo->area) != B_OK)
			bo->poisoned = true;
		PutBuffer(bo);
	}
	PutClient(client); // queued jobs retain their own reference
	mutex_unlock(&sMutex);
}

bool
amdgpu_device_active()
{
	return sActive;
}

static status_t
Start(volatile uint32* regs, const amdgpu_info& info,
	const amdgpu::FirmwareView& firmware, const amdgpu::AtomVramReservation& reservation,
	bool uvdClocksQualified)
{
	if (sActive)
		return B_BUSY;
	// The first 64 MiB contains scanout and all kernel ring/SMU allocations.
	// Account separately for ATOM's firmware/driver ranges and active cursors.
	if (info.bar_size[0] <= (64ULL << 20) || !sAllocator.Init(info.vram_size))
		return B_NO_MEMORY;
	uint64 scratchEnd = reservation.start != 0 ? reservation.start : info.bar_size[0];
	bool valid = sAllocator.Reserve(0, 64ULL << 20)
		&& sAllocator.Reserve(reservation.start, reservation.size)
		&& reservation.driverScratchSize <= scratchEnd
		&& sAllocator.Reserve(scratchEnd - reservation.driverScratchSize,
			reservation.driverScratchSize);
	const uint32 heads[] = {0x1a00, 0x1c00, 0x1e00, 0x4000, 0x4200, 0x4400};
	for (uint32 base : heads) {
		if ((regs[base + 0x19c] & 1) == 0 || (regs[base + 0x66] & 1) == 0)
			continue;
		uint64 cursor = (uint64)regs[base + 0x69] << 32 | (regs[base + 0x67] & ~0xffu);
		valid &= cursor >= info.vram_gpu_base
			&& sAllocator.Reserve(cursor - info.vram_gpu_base, 128 * 128 * 4);
	}
	if (!valid) {
		sAllocator.Uninit();
		return B_BAD_DATA;
	}
	sJobs = create_sem(0, "amdgpu jobs");
	if (sJobs < 0) {
		sAllocator.Uninit();
		return sJobs;
	}
	status_t status = sEngine.Initialize(regs, info, firmware, reservation);
	if (status != B_OK) {
		delete_sem(sJobs);
		sJobs = -1;
		sAllocator.Uninit();
		return status;
	}
	if (!sGartAllocator.Init(Gart::kSize) || !sGartAllocator.Reserve(0, Gart::kCommandBytes))
		status = B_NO_MEMORY;
	else
		status = sGart.Initialize(regs, info, reservation);
	if (status != B_OK) {
		sEngine.Uninitialize();
		sGartAllocator.Uninit();
		delete_sem(sJobs);
		sJobs = -1;
		sAllocator.Uninit();
		return status;
	}
	sInfo = info;
	sReservation = reservation;
	sUvdClocksQualified = uvdClocksQualified;
	sFault = B_OK;
	sStopping = false;
	sVideoExecuting = false;
	sGraphicsExecuting = false;
	sCompleted.Init(&sCompleted, "amdgpu fence");
	sWorker = spawn_kernel_thread(Executor, "amdgpu jobs", B_NORMAL_PRIORITY, NULL);
	if (sWorker < 0) {
		sGart.Uninitialize(false);
		sGartAllocator.Uninit();
		sEngine.Uninitialize();
		delete_sem(sJobs);
		sJobs = -1;
		sAllocator.Uninit();
		return sWorker;
	}
	sActive = true;
	resume_thread(sWorker);
	dprintf("amdgpu: client DMA ready, %" B_PRIu64 " MiB visible VRAM\n", info.bar_size[0] >> 20);
	return B_OK;
}

status_t
amdgpu_device_start(volatile uint32* regs, const amdgpu_info& info,
	const amdgpu::FirmwareView& firmware, const amdgpu::AtomVramReservation& reservation,
	bool uvdClocksQualified, pci_module_info* pci, const amdgpu::AtomRenderInfo& renderInfo)
{
	mutex_lock(&sMutex);
	status_t status = Start(regs, info, firmware, reservation, uvdClocksQualified);
	if (status == B_OK) {
		sGfx.pci = pci;
		sRenderInfo = renderInfo;
	}
	mutex_unlock(&sMutex);
	return status;
}

template<typename T> static status_t
ReadRequest(T& request, void* data, size_t length)
{
	if (length != sizeof(T))
		return B_BAD_VALUE;
	if (user_memcpy(&request, data, sizeof(T)) != B_OK)
		return B_BAD_ADDRESS;
	return request.version == AMDGPU_HAIKU_ABI_VERSION && request.size == sizeof(T)
		? B_OK : B_BAD_VALUE;
}

static status_t
WaitDmaIdle(bool includeVideo = true, bool includeGraphics = true)
{
	bigtime_t deadline = system_time() + 5000000;
	auto busy = [&]() {
		// Video owns SDMA but can overlap graphics. Once graphics shares this
		// queue, counting every queued job here would serialize decode again.
		return (includeGraphics ? sPending : sDmaPending) != 0
			|| (includeVideo && sVideoExecuting)
			|| (includeGraphics && sGraphicsExecuting);
	};
	while (busy() && sFault == B_OK && !sStopping) {
		status_t status = sCompleted.Wait(&sMutex, B_ABSOLUTE_TIMEOUT, deadline);
		if (status != B_OK && busy())
			return status;
	}
	return sFault == B_OK && !sStopping ? B_OK : B_DEV_NOT_READY;
}

static void
WaitGraphicsIdle()
{
	// Teardown must wait for the owner even on fault. It detaches VMID2
	// before signaling success, or quarantines backing storage on failure.
	while (sGraphicsExecuting)
		sCompleted.Wait(&sMutex);
}

// The caller owns idle UVD/SDMA/GFX and sMutex. A GFX reset does not destroy
// firmware sessions: retire them explicitly before returning any address to
// either allocator. On failure the remaining ownership record stays intact.
static status_t
RetireVideo(UvdSession* s, bool reclaimed)
{
	if (s->created) {
		amdgpu_uvd_test result = {};
		status_t status = sUvd.Session(*s, 2, NULL, NULL, 0, sEngine, result);
		if (status != B_OK) return status;
	}
	if (s->readbackBytes != 0) {
		if (s->readbackBound) {
			status_t status = sGart.Unbind(s->readbackOffset, s->readbackBytes);
			if (status != B_OK) return status;
			s->readbackBound = false;
		}
		if (!sGartAllocator.Free(s->readbackOffset, s->readbackBytes)) return B_BAD_DATA;
		if (s->readbackArea >= 0) delete_area(s->readbackArea);
		s->readbackArea = -1;
		if (reclaimed) sRecovery.reclaimed_ram += s->readbackBytes;
		s->readbackBytes = 0;
	}
	if (!sAllocator.Free(s->offset, s->layout.bytes)) return B_BAD_DATA;
	delete_area(s->area);
	s->area = -1;
	if (reclaimed) sRecovery.reclaimed_vram += s->layout.bytes;
	return B_OK;
}

static void
ReleaseVideo(AmdgpuClient* client)
{
	UvdSession* s = client->video;
	if (s == NULL) return;
	// Wait even after a fault. Recovery may finish while this wait yields;
	// decide whether to retire or quarantine only after reacquiring all owners.
	while (sPending != 0 || sGraphicsExecuting || sVideoExecuting)
		sCompleted.Wait(&sMutex);
	ObserveInterruptFault();
	client->systemBytes -= s->readbackBytes;
	client->bytes -= s->layout.bytes;
	if (sFault == B_OK) {
		status_t status = RetireVideo(s, false);
		if (status != B_OK) SetFault(status);
	}
	client->video = NULL;
	sVideoSessions--;
	if (sFault != B_OK) {
		s->nextQuarantined = sQuarantinedVideos;
		sQuarantinedVideos = s;
	} else
		free(s);
}

static bool VideoLayout(const amdgpu_h264_config& c, amdgpu::UvdVideoLayout& l)
{
	return amdgpu::UvdH264Size(c, l);
}
static bool VideoLayout(const amdgpu_hevc_config& c, amdgpu::UvdVideoLayout& l)
{
	return amdgpu::UvdHevcSize(c, l);
}
static void SetVideoConfig(UvdSession& s, const amdgpu_h264_config& c)
{
	s.hevc = false; s.config = c;
}
static void SetVideoConfig(UvdSession& s, const amdgpu_hevc_config& c)
{
	s.hevc = true; s.hevcConfig = c;
}
static void FinishVideoCreate(amdgpu_video_create&, const UvdSession&)
{
}
static void FinishVideoCreate(amdgpu_hevc_create& r, const UvdSession& s)
{
	r.output_height = s.layout.outputHeight;
	r.pixel_format = s.hevcConfig.profile == 2 ? AMDGPU_VIDEO_P010 : AMDGPU_VIDEO_NV12;
}
static bool VideoPictureValid(const UvdSession& s, const amdgpu_h264_picture& p, uint32 bytes)
{
	return !s.hevc && amdgpu::UvdH264Validate(s.config, p, bytes);
}
static bool VideoPictureValid(const UvdSession& s, const amdgpu_hevc_picture& p, uint32 bytes)
{
	return s.hevc && amdgpu::UvdHevcValidate(s.hevcConfig, p, bytes);
}

template<typename Request>
static status_t
CreateVideo(AmdgpuClient* client, void* data, size_t length)
{
	Request request;
	status_t status = ReadRequest(request, data, length);
	amdgpu::UvdVideoLayout layout;
	if (status != B_OK) return status;
	if (!VideoLayout(request.config, layout)) return B_BAD_VALUE;
	if (client->video != NULL) return B_BUSY;
	if (sFault != B_OK || client->loss != B_OK) return B_DEV_NOT_READY;
	if (sNextVideoHandle == 0 || sVideoSessions >= 32) return B_NO_MEMORY;
	UvdSession* s = (UvdSession*)calloc(1, sizeof(UvdSession));
	if (s == NULL) return B_NO_MEMORY;
	s->readbackArea = -1;
	SetVideoConfig(*s, request.config); s->layout = layout;
	if (!sAllocator.Allocate(layout.bytes, 4096, sInfo.bar_size[0], s->offset)) {
		free(s); return B_NO_MEMORY;
	}
	s->area = map_physical_memory("amdgpu private video session",
		sInfo.bar_address[0] + s->offset, layout.bytes, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, (void**)&s->cpu);
	if (s->area < 0) {
		status = s->area; sAllocator.Free(s->offset, layout.bytes); free(s); return status;
	}
	s->gpu = sInfo.vram_gpu_base + s->offset;
	s->handle = sNextVideoHandle++;
	client->video = s; client->bytes += layout.bytes; sVideoSessions++;
	status = WaitDmaIdle();
	if (status == B_OK && client->loss != B_OK) status = B_DEV_NOT_READY;
	amdgpu_uvd_test result = {};
	if (status == B_OK) {
		uint64 bytes = ((layout.outputBytes + 4095ULL) & ~4095ULL)
			+ UvdSession::kGuardReadbackBytes + 8192;
		if (!sGartAllocator.Allocate(bytes, 4096, Gart::kSize, s->readbackOffset))
			status = B_NO_MEMORY;
		else {
			s->readbackBytes = bytes; client->systemBytes += bytes;
			virtual_address_restrictions va = {};
			physical_address_restrictions pa = {};
			s->readbackArea = create_area_etc(B_SYSTEM_TEAM, "amdgpu private video readback",
				bytes, B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
				0, 0, &va, &pa, &s->readback);
			status = s->readbackArea < 0 ? s->readbackArea : B_OK;
			if (status == B_OK) {
				memset(s->readback, 0x7d, bytes);
				status = sGart.Bind(s->readbackOffset, bytes, s->readback);
				if (status == B_TIMED_OUT) SetFault(status);
				s->readbackBound = status == B_OK;
				s->readbackGpu = Gart::kBase + s->readbackOffset;
			}
		}
	}
	// The engine and its worker are idle, and sMutex prevents new jobs
	// from being queued until the private fill/decode/readback completes.
	for (uint64 offset = 0; status == B_OK && offset < layout.bytes;) {
		uint64 bytes = min_c(layout.bytes - offset, 64ULL << 20);
		status = sEngine.Execute(AMDGPU_DMA_FILL, 0, s->gpu + offset, bytes, 0);
		if (status != B_OK) SetFault(status);
		offset += bytes;
	}
	if (status == B_OK && !sUvd.ready) {
		InstalledFirmware firmware;
		status = firmware.LoadUvd();
		if (status == B_OK)
			status = sUvd.Initialize(sEngine.regs, sInfo, sReservation,
				sUvdClocksQualified, firmware.view, result);
	}
	if (status == B_OK) status = sUvd.Session(*s, 0, NULL, NULL, 0, sEngine, result);
	if (!sVideoExecuting && sUvd.faulted && sFault == B_OK) SetFault(status);
	if (status == B_OK) {
		request.handle = s->handle; request.pitch = layout.pitch;
		request.output_bytes = layout.outputBytes; request.allocated_bytes = layout.bytes;
		FinishVideoCreate(request, *s);
		status = user_memcpy(data, &request, sizeof(request));
	}
	if (status != B_OK) ReleaseVideo(client);
	return status;
}

template<typename Request>
static status_t
DecodeVideo(AmdgpuClient* client, void* data, size_t length)
{
	Request request;
	status_t status = ReadRequest(request, data, length);
	if (status != B_OK) return status;
	UvdSession* s = client->video;
	if (s == NULL || request.handle != s->handle || request.bitstream == 0 || request.output == 0
		|| request.output_capacity != s->layout.outputBytes
		|| !VideoPictureValid(*s, request.picture, request.bitstream_bytes))
		return B_BAD_VALUE;
	if (sFault != B_OK || client->loss != B_OK) return B_DEV_NOT_READY;
	if (!AmdgpuUserRange(request.bitstream, request.bitstream_bytes)
		|| !AmdgpuUserRange(request.output, s->layout.outputBytes))
		return B_BAD_ADDRESS;
	void* input = malloc(request.bitstream_bytes);
	status = input == NULL ? B_NO_MEMORY
		: AmdgpuCopyFromUser(input, request.bitstream, request.bitstream_bytes);
	amdgpu_uvd_test result = {};
	if (status == B_OK) status = WaitDmaIdle(true, false);
	if (status == B_OK && client->loss != B_OK) status = B_DEV_NOT_READY;
	if (status == B_OK) {
		// No queued DMA remains. Keep UVD and SDMA exclusively reserved while
		// initialized graphics runs with its own reservation. The ioctl's file
		// reference pins this client; videoBusy freezes its session, and all
		// input/output storage here belongs to this kernel-private session.
		sVideoExecuting = true;
		mutex_unlock(&sMutex);
		status = sUvd.Session(*s, 1, &request.picture, input, request.bitstream_bytes, sEngine, result);
		mutex_lock(&sMutex);
		sVideoExecuting = false;
		ObserveInterruptFault();
		if (sUvd.faulted && sFault == B_OK) SetFault(status);
		// A concurrent graphics fault quarantines the whole device, even if
		// this decode retired successfully. Do not publish its output.
		if (status == B_OK && sFault != B_OK) status = B_DEV_NOT_READY;
		sCompleted.NotifyAll();
	}
	if (status == B_OK)
		status = AmdgpuCopyToUser(request.output, (uint8*)s->readback + 4096,
			s->layout.outputBytes);
	request.sequence = result.sequence; request.fence = result.fence;
	request.rptr = result.rptr; request.wptr = result.wptr;
	request.guard_mismatches = result.guard_mismatches;
	request.vm_fault_status = result.vm_fault_status; request.vm_fault_address = result.vm_fault_address;
	memcpy(request.feedback, result.feedback, sizeof(request.feedback));
	status_t copied = user_memcpy(data, &request, sizeof(request));
	free(input);
	return status != B_OK ? status : copied;
}

static status_t
VideoControl(AmdgpuClient* client, uint32 op, void* data, size_t length)
{
	if (op == AMDGPU_VIDEO_CREATE) return CreateVideo<amdgpu_video_create>(client, data, length);
	if (op == AMDGPU_HEVC_CREATE) return CreateVideo<amdgpu_hevc_create>(client, data, length);
	if (op == AMDGPU_VIDEO_DECODE) return DecodeVideo<amdgpu_video_decode>(client, data, length);
	if (op == AMDGPU_HEVC_DECODE) return DecodeVideo<amdgpu_hevc_decode>(client, data, length);
	if (op == AMDGPU_VIDEO_DESTROY) {
		amdgpu_video_destroy request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if (client->video == NULL || request.handle != client->video->handle) return B_BAD_VALUE;
		ReleaseVideo(client);
		return sFault == B_OK ? B_OK : B_DEV_NOT_READY;
	}
	return B_BAD_VALUE;
}

static bool
AllocatePageTable(void*, size_t bytes, GpuPageTable::Allocation& allocation)
{
	uint64 offset;
	if (!sAllocator.Allocate(bytes, 4096, sInfo.bar_size[0], offset))
		return false;
	void* address;
	area_id area = map_physical_memory("amdgpu client page table",
		sInfo.bar_address[0] + offset, bytes, B_ANY_KERNEL_ADDRESS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, &address);
	if (area < 0) {
		sAllocator.Free(offset, bytes);
		return false;
	}
	allocation.cpu = (volatile uint64*)address;
	allocation.gpu = sInfo.vram_gpu_base + offset;
	allocation.cookie = area;
	return true;
}

static void
ReleasePageTable(void*, size_t bytes, GpuPageTable::Allocation& allocation, bool reclaim)
{
	// Complete CPU writes to this BAR mapping before another allocation can
	// reuse its pages, including a close without any intervening GPU job.
	__sync_synchronize();
	(void)allocation.cpu[bytes / 8 - 1];
	delete_area(allocation.cookie);
	if (reclaim)
		sAllocator.Free(allocation.gpu - sInfo.vram_gpu_base, bytes);
}

static void
ReleaseVM(AmdgpuClient* client)
{
	ClientVM* vm = client->vm;
	if (vm == NULL)
		return;
	// Decide reclamation only after the last possible wait. A live-client
	// reset may complete while this cleanup waits for another graphics owner.
	WaitGraphicsIdle();
	ObserveInterruptFault();
	const bool retain = sFault != B_OK;
	if (!retain) {
		vm->table.Uninitialize(true);
		ReleasePageTable(NULL, 4096, vm->commands, true);
		if (vm->copiedCommands.cpu != NULL)
			ReleasePageTable(NULL, kMaxIbBytes, vm->copiedCommands, true);
	}
	while (vm->bindings != NULL) {
		VmBinding* binding = vm->bindings;
		vm->bindings = binding->next;
		PutBuffer(binding->buffer);
		free(binding);
	}
	if (retain) {
		vm->nextQuarantined = sQuarantinedVMs;
		sQuarantinedVMs = vm;
	} else
		free(vm);
	client->vm = NULL;
	sVMCount--;
}

static status_t
CreateVM(AmdgpuClient* client)
{
	if (client->vm != NULL)
		return B_OK;
	if (sVMCount >= kMaxVMClients)
		return B_NO_MEMORY;
	ClientVM* vm = (ClientVM*)calloc(1, sizeof(ClientVM));
	if (vm == NULL)
		return B_NO_MEMORY;
	bool okay = vm->table.Initialize(NULL, AllocatePageTable, ReleasePageTable);
	// A general shader can read even the reserved GPU pages. Keep diagnostic
	// commands (including destination and seed) private to this client, and
	// clear fresh storage before publishing it. The shader page is public code.
	if (okay)
		okay = AllocatePageTable(NULL, 4096, vm->commands);
	if (okay) {
		for (uint32 i = 0; i < 4096 / 8; i++) vm->commands.cpu[i] = 0;
	}
	uint64 entry = vm->commands.gpu | 0x31;
	if (okay)
		okay = vm->table.Map(GfxEngine::kClientIbVA, &entry, 1) == GpuPageTable::OK;
	entry = (sInfo.vram_gpu_base + GfxEngine::kScratchOffset + 0x40000) | 0x31;
	if (okay)
		okay = vm->table.Map(GfxEngine::kClientShaderVA, &entry, 1) == GpuPageTable::OK;
	if (!okay) {
		vm->table.Uninitialize(true);
		if (vm->commands.cpu != NULL)
			ReleasePageTable(NULL, 4096, vm->commands, true);
		free(vm);
		return B_NO_MEMORY;
	}
	client->vm = vm;
	sVMCount++;
	return B_OK;
}

static status_t
PrepareCopiedCommands(ClientVM* vm)
{
	// graphicsBusy excludes another operation on this VM. No user handle or
	// CPU clone exposes this allocation, and its GPU PTEs never allow writes.
	if (vm->copiedCommands.cpu == NULL) {
		GpuPageTable::Allocation allocation = {};
		if (!AllocatePageTable(NULL, kMaxIbBytes, allocation)) return B_NO_MEMORY;
		memset((void*)allocation.cpu, 0, kMaxIbBytes);
		uint64 entries[kMaxIbBytes / 4096];
		for (uint32 i = 0; i < kMaxIbBytes / 4096; i++)
			entries[i] = (allocation.gpu + i * 4096) | 0x31;
		if (vm->table.Map(AMDGPU_COPY_IB_ADDRESS, entries, kMaxIbBytes / 4096)
				!= GpuPageTable::OK) {
			ReleasePageTable(NULL, kMaxIbBytes, allocation, true);
			return B_NO_MEMORY;
		}
		vm->copiedCommands = allocation;
	}
	return B_OK;
}

static status_t
CopyCommands(ClientVM* vm, uint64 address, uint32 bytes)
{
	status_t status = PrepareCopiedCommands(vm);
	if (status != B_OK) return status;
	uint8* copied = (uint8*)vm->copiedCommands.cpu;
	status = AmdgpuCopyFromUser(copied, address, bytes);
	uint32 cleared = status == B_OK ? bytes : 0;
	memset(copied + cleared, 0, kMaxIbBytes - cleared);
	__sync_synchronize();
	(void)vm->copiedCommands.cpu[kMaxIbBytes / 8 - 1];
	return status;
}

static status_t
StartGraphics()
{
	// Callers normally drained graphics already. Keep engine-state reads
	// serialized even if a future caller reaches this helper directly.
	if (sGraphicsExecuting) {
		status_t status = WaitDmaIdle(false);
		if (status != B_OK) return status;
	}
	if (!sGfx.ready) {
		// Initial firmware/ring setup uses SDMA and shared registers. Only
		// already initialized graphics may overlap private video execution.
		status_t status = WaitDmaIdle();
		if (status != B_OK) return status;
	}
	if (sGfx.ready)
		return sGfx.faulted ? B_DEV_NOT_READY : sGfx.mecStarted ? B_OK : B_NOT_ALLOWED;
	InstalledFirmware images[5];
	amdgpu::FirmwareView firmware[4];
	for (uint32 i = 0; i < 4; i++) {
		status_t status = images[i].LoadGfx(i);
		if (status != B_OK)
			return status;
		firmware[i] = images[i].view;
	}
	status_t status = images[4].LoadMec();
	if (status != B_OK)
		return status;
	amdgpu_gfx_test result = {};
	status = sGfx.Initialize(sEngine.regs, sInfo, sReservation, firmware,
		sEngine, sGart, result, &images[4].mec);
	if (sGfx.faulted)
		SetFault(status);
	return status;
}

template<typename Execute>
static status_t
ExecuteGraphics(Execute execute)
{
	// The caller drained ordinary DMA and other graphics jobs. Its ioctl
	// file reference pins the client; graphicsBusy freezes the VM bindings
	// and their BO references until retirement and VMID2 detachment.
	sGraphicsExecuting = true;
	mutex_unlock(&sMutex);
	status_t status = execute();
	mutex_lock(&sMutex);
	sGraphicsExecuting = false;
	ObserveInterruptFault();
	if (sGfx.faulted && sFault == B_OK) SetFault(status);
	if (status == B_OK && sFault != B_OK) status = B_DEV_NOT_READY;
	sCompleted.NotifyAll();
	return status;
}

static status_t
ExecuteQueuedGraphics(Job* job)
{
	// The job's client reference retains this VM after close. VM mutations
	// are excluded by graphicsPending; only this worker replaces its private
	// executable copy, after the previous graphics job detached VMID2.
	ClientVM* vm = job->client->vm;
	uint32 bytes = job->graphicsDwords * 4;
	memcpy((void*)vm->copiedCommands.cpu, job->graphicsCommands, bytes);
	memset((uint8*)vm->copiedCommands.cpu + bytes, 0, kMaxIbBytes - bytes);
	__sync_synchronize();
	(void)vm->copiedCommands.cpu[kMaxIbBytes / 8 - 1];
	amdgpu_vm_test result = {};
	return ExecuteGraphics([&]() {
		return sGfx.ExecuteIB(vm->table.directory.gpu, AMDGPU_COPY_IB_ADDRESS,
			job->graphicsDwords, sGart, result);
	});
}

static status_t
QueueGraphics(AmdgpuClient* client, void* data, size_t length)
{
	amdgpu_gfx_queue request;
	status_t status = ReadRequest(request, data, length);
	if (status != B_OK) return status;
	if (request.reserved[0] || request.reserved[1]
		|| (request.flags & ~AMDGPU_GFX_VALIDATE_COMMANDS)
		|| (request.dwords & (kIbSizeAlignment / 4 - 1))
		|| request.dwords == 0 || request.dwords > kMaxIbBytes / 4)
		return B_BAD_VALUE;
	if (sFault != B_OK || client->loss != B_OK || client->vm == NULL)
		return B_DEV_NOT_READY;
	if (sPending >= 256 || client->pending >= 64 || client->submitted == UINT64_MAX)
		return B_WOULD_BLOCK;
	Job* job = (Job*)calloc(1, sizeof(Job));
	if (job == NULL) return B_NO_MEMORY;
	uint32 bytes = request.dwords * 4;
	job->graphicsCommands = (uint32*)malloc(bytes);
	if (job->graphicsCommands == NULL) status = B_NO_MEMORY;
	else status = AmdgpuCopyFromUser(job->graphicsCommands, request.address, bytes);
	if (status == B_OK && (request.flags & AMDGPU_GFX_VALIDATE_COMMANDS)
		&& amdgpu::ValidateGfx8Commands(job->graphicsCommands, request.dwords).error
			!= amdgpu::GfxCommandValidation::OK)
		status = B_NOT_ALLOWED;
	// Startup may drain prior DMA, but normal enqueue never waits for GPU
	// completion. No worker may initialize graphics with itself in sPending.
	if (status == B_OK && !sGfx.ready) status = StartGraphics();
	// Startup can drop the lock while draining existing work. DMA on this
	// file and other clients may have changed both quotas while we waited.
	if (status == B_OK && (sPending >= 256 || client->pending >= 64
		|| client->submitted == UINT64_MAX)) status = B_WOULD_BLOCK;
	if (status == B_OK && client->loss != B_OK) status = B_DEV_NOT_READY;
	if (status == B_OK) status = PrepareCopiedCommands(client->vm);
	if (status == B_OK) {
		request.fence = client->submitted + 1;
		status = user_memcpy(data, &request, sizeof(request));
	}
	if (status != B_OK) {
		free(job->graphicsCommands);
		free(job);
		return status;
	}
	job->client = client;
	job->command.fence = request.fence;
	job->graphicsDwords = request.dwords;
	client->submitted = request.fence;
	client->pending++;
	client->graphicsPending++;
	client->references++;
	if (sLast != NULL) sLast->next = job;
	else sFirst = job;
	sLast = job;
	sPending++;
	release_sem(sJobs);
	return B_OK;
}

static status_t
VMControl(AmdgpuClient* client, uint32 op, void* data, size_t length)
{
	if (sFault != B_OK || client->loss != B_OK)
		return B_DEV_NOT_READY;
	if (op == AMDGPU_VM_INFO) {
		amdgpu_vm_info request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if (request.reserved[0] != 0 || request.reserved[1] != 0) return B_BAD_VALUE;
		status = CreateVM(client);
		if (status != B_OK) return status;
		request.address_start = kClientAddressStart;
		request.address_end = GpuPageTable::kSize;
		request.page_size = 4096;
		request.mapped_bytes = client->vm->bytes;
		request.mapping_count = client->vm->mappings;
		return user_memcpy(data, &request, sizeof(request));
	}
	ClientVM* vm = client->vm;
	if (vm == NULL)
		return B_DEV_NOT_READY;
	if (op == AMDGPU_GFX_SUBMIT || op == AMDGPU_GFX_SUBMIT_COPY
		|| op == AMDGPU_GFX_GDS_PROBE_COPY) {
		const bool probe = op == AMDGPU_GFX_GDS_PROBE_COPY;
		const bool copy = probe || op == AMDGPU_GFX_SUBMIT_COPY;
		amdgpu_gfx_submit request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if ((probe ? request.flags > AMDGPU_GDS_PROBE_LAST
			: (request.flags & ~(copy ? AMDGPU_GFX_VALIDATE_COMMANDS : 0u)) != 0)
			|| request.reserved != 0
			|| (!copy && (request.address & (kIbAddressAlignment - 1)) != 0)
			|| (request.dwords & (kIbSizeAlignment / 4 - 1)) != 0
			|| request.dwords == 0 || request.dwords > kMaxIbBytes / 4)
			return B_BAD_VALUE;
		if (copy) {
			status = CopyCommands(vm, request.address, request.dwords * 4);
			if (status != B_OK) return status;
			if (probe || (request.flags & AMDGPU_GFX_VALIDATE_COMMANDS)) {
				amdgpu::GfxCommandValidation checked = amdgpu::ValidateGfx8Commands(
					(const volatile uint32*)vm->copiedCommands.cpu, request.dwords);
				if (checked.error != amdgpu::GfxCommandValidation::OK) {
					// Never leave a rejected stream in the private executable copy.
					memset((void*)vm->copiedCommands.cpu, 0, kMaxIbBytes);
					__sync_synchronize();
					(void)vm->copiedCommands.cpu[kMaxIbBytes / 8 - 1];
					return B_NOT_ALLOWED;
				}
			}
		} else {
			VmBinding* binding = vm->bindings;
			while (binding != NULL) {
				if (request.address >= binding->address
					&& request.address - binding->address <= binding->bytes
					&& (uint64)request.dwords * 4 <= binding->bytes - (request.address - binding->address))
					break;
				binding = binding->next;
			}
			const uint32 permissions = AMDGPU_VM_READ | AMDGPU_VM_EXECUTE;
			if (binding == NULL || (binding->permissions & permissions) != permissions)
				return B_NOT_ALLOWED;
		}
		// All VM mappings remain immutable while WaitDmaIdle releases sMutex.
		// Binding references also retain BOs whose public handles are freed.
		status = WaitDmaIdle(probe);
		if (status == B_OK) status = StartGraphics();
		if (status == B_OK && client->loss != B_OK) status = B_DEV_NOT_READY;
		amdgpu_vm_test result = {};
		bigtime_t started = system_time();
		if (status == B_OK) {
			status = ExecuteGraphics([&]() {
				return sGfx.ExecuteIB(vm->table.directory.gpu,
					copy ? AMDGPU_COPY_IB_ADDRESS : request.address,
					request.dwords, sGart, result, probe ? request.flags + 1 : 0);
			});
		}
		request.elapsed_us = system_time() - started;
		request.status = status; request.completion = result.completion;
		memcpy(request.vm_fault_status, result.vm_fault_status, sizeof(request.vm_fault_status));
		request.rptr = result.rptr; request.wptr = result.wptr;
		return user_memcpy(data, &request, sizeof(request));
	}
	if (op == AMDGPU_VM_TEST) {
		amdgpu_vm_test request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if (request.reserved != 0 || request.reserved_out != 0 || (request.address & 3) != 0)
			return B_BAD_VALUE;
		VmBinding* binding = vm->bindings;
		while (binding != NULL) {
			if (request.address >= binding->address
				&& request.address - binding->address <= binding->bytes
				&& 4096 <= binding->bytes - (request.address - binding->address))
				break;
			binding = binding->next;
		}
		if (binding == NULL || (binding->permissions & AMDGPU_VM_WRITE) == 0)
			return B_NOT_ALLOWED;
		// graphicsBusy prevents this file's VM changes across the wait.
		// Mapping references keep the BO alive even if its handle is freed.
		status = WaitDmaIdle(false);
		if (status == B_OK) status = StartGraphics();
		if (status == B_OK && client->loss != B_OK) status = B_DEV_NOT_READY;
		amdgpu_vm_test result = {};
		result.version = AMDGPU_HAIKU_ABI_VERSION;
		result.size = sizeof(result);
		result.address = request.address; result.value = request.value;
		if (status == B_OK) {
			status = ExecuteGraphics([&]() {
				return sGfx.ExecuteVM(vm->table.directory.gpu, request.address,
					request.value, (volatile uint32*)vm->commands.cpu, sGart, result);
			});
		}
		result.status = status;
		return user_memcpy(data, &result, sizeof(result));
	}
	amdgpu_vm_mapping request;
	status_t status = ReadRequest(request, data, length);
	if (status != B_OK) return status;
	if (request.reserved != 0 || request.address < kClientAddressStart
		|| request.bytes == 0 || request.bytes > kMaxBufferBytes
		|| ((request.address | request.bytes | request.buffer_offset) & 4095) != 0
		|| request.address >= GpuPageTable::kSize
		|| request.bytes > GpuPageTable::kSize - request.address)
		return B_BAD_VALUE;
	if (op == AMDGPU_VM_UNMAP) {
		if (request.handle != 0 || request.buffer_offset != 0 || request.permissions != 0)
			return B_BAD_VALUE;
		VmBinding** link = &vm->bindings;
		while (*link != NULL && ((*link)->address != request.address || (*link)->bytes != request.bytes))
			link = &(*link)->next;
		if (*link == NULL) return B_BAD_VALUE;
		VmBinding* binding = *link;
		if (!vm->table.Unmap(request.address, request.bytes / 4096))
			return B_BAD_DATA;
		*link = binding->next;
		vm->mappings--; vm->bytes -= binding->bytes;
		PutBuffer(binding->buffer);
		free(binding);
		return B_OK;
	}
	Buffer* buffer = Lookup(client, request.handle);
	if (buffer == NULL || request.buffer_offset > buffer->bytes
		|| request.bytes > buffer->bytes - request.buffer_offset
		|| request.permissions == 0 || (request.permissions & ~7u) != 0
		|| ((request.permissions & AMDGPU_VM_EXECUTE) != 0
			&& (request.permissions & AMDGPU_VM_READ) == 0))
		return B_BAD_VALUE;
	if (vm->mappings >= kMaxMappings || vm->bytes > kMaxMappedBytes - request.bytes)
		return B_NO_MEMORY;
	VmBinding* binding = (VmBinding*)malloc(sizeof(VmBinding));
	uint32 pages = request.bytes / 4096;
	uint64* entries = (uint64*)malloc(pages * sizeof(uint64));
	if (binding == NULL || entries == NULL) {
		free(binding); free(entries);
		return B_NO_MEMORY;
	}
	uint64 flags = 1 | ((request.permissions & AMDGPU_VM_READ) != 0 ? 0x20 : 0)
		| ((request.permissions & AMDGPU_VM_WRITE) != 0 ? 0x40 : 0)
		| ((request.permissions & AMDGPU_VM_EXECUTE) != 0 ? 0x10 : 0);
	for (uint32 i = 0; i < pages; i++) {
		uint64 offset = request.buffer_offset + (uint64)i * 4096;
		entries[i] = buffer->system
			? (sGart.table[(buffer->offset + offset) / 4096] & (GpuPageTable::kPhysicalMask | 6)) | flags
			: (buffer->gpu + offset) | flags;
	}
	GpuPageTable::Result mapped = vm->table.Map(request.address, entries, pages);
	free(entries);
	if (mapped != GpuPageTable::OK) {
		free(binding);
		return mapped == GpuPageTable::NO_MEMORY ? B_NO_MEMORY : B_BAD_VALUE;
	}
	binding->buffer = buffer; binding->address = request.address;
	binding->bytes = request.bytes; binding->offset = request.buffer_offset;
	binding->permissions = request.permissions; binding->next = vm->bindings;
	vm->bindings = binding;
	buffer->references++;
	vm->mappings++; vm->bytes += request.bytes;
	return B_OK;
}

static uint64
VMStorageBytes(const ClientVM* vm)
{
	return GpuPageTable::kDirectoryBytes + (uint64)vm->table.leafCount * 4096
		+ 4096 + (vm->copiedCommands.cpu != NULL ? kMaxIbBytes : 0);
}

static amdgpu_recovery_info
RecoveryInfo()
{
	amdgpu_recovery_info info = sRecovery;
	info.version = AMDGPU_HAIKU_ABI_VERSION; info.size = sizeof(info);
	info.flags = info.reserved = info.reserved_out = 0;
	info.status = sFault;
	info.quarantined_ram = info.quarantined_vram = 0;
	info.buffers = info.vms = info.videos = 0;
	for (Buffer* bo = sQuarantinedBuffers; bo != NULL; bo = bo->next) {
		info.buffers++;
		if (bo->system) info.quarantined_ram += bo->bytes;
		else info.quarantined_vram += bo->bytes;
	}
	for (ClientVM* vm = sQuarantinedVMs; vm != NULL; vm = vm->nextQuarantined) {
		info.vms++; info.quarantined_vram += VMStorageBytes(vm);
	}
	for (UvdSession* v = sQuarantinedVideos; v != NULL; v = v->nextQuarantined) {
		info.videos++; info.quarantined_vram += v->layout.bytes;
		info.quarantined_ram += v->readbackBytes;
	}
	info.live_handles = sLiveHandles; info.live_vms = sVMCount;
	info.live_videos = sVideoSessions; info.pending = sPending;
	return info;
}

static status_t
ReclaimGraphicsQuarantine()
{
	// Called only after a successful reset/private shader proof and VMID2
	// detachment. The global fault still blocks every client allocation.
	while (sQuarantinedVideos != NULL) {
		UvdSession* s = sQuarantinedVideos;
		status_t status = RetireVideo(s, true);
		if (status != B_OK) return status;
		sQuarantinedVideos = s->nextQuarantined;
		free(s);
	}
	while (sQuarantinedBuffers != NULL) {
		Buffer* bo = sQuarantinedBuffers;
		if (bo->area >= 0) {
			status_t status = vm_change_clones_to_null_areas(bo->area);
			if (status != B_OK) return status;
		}
		if (bo->system) {
			status_t status = sGart.Unbind(bo->offset, bo->bytes);
			if (status != B_OK) return status;
		}
		VramAllocator& allocator = bo->system ? sGartAllocator : sAllocator;
		if (!allocator.Free(bo->offset, bo->bytes)) return B_BAD_DATA;
		if (bo->area >= 0) delete_area(bo->area);
		if (bo->system) sRecovery.reclaimed_ram += bo->bytes;
		else sRecovery.reclaimed_vram += bo->bytes;
		sQuarantinedBuffers = bo->next;
		free(bo);
	}
	while (sQuarantinedVMs != NULL) {
		ClientVM* vm = sQuarantinedVMs;
		sRecovery.reclaimed_vram += VMStorageBytes(vm);
		vm->table.Uninitialize(true);
		ReleasePageTable(NULL, 4096, vm->commands, true);
		if (vm->copiedCommands.cpu != NULL)
			ReleasePageTable(NULL, kMaxIbBytes, vm->copiedCommands, true);
		sQuarantinedVMs = vm->nextQuarantined;
		free(vm);
	}
	return B_OK;
}

// A restarted firmware has no old sessions. Check their private VRAM guards
// before marking them retired; leave all live storage owned by the lost files.
static status_t
InvalidateVideoSessions()
{
	auto invalidate = [](UvdSession* s) -> status_t {
		if (s == NULL || !s->created) return B_OK;
		for (uint32 offset : s->layout.guards)
			for (uint32 i = 0; i < 1024; i++)
				if (s->cpu[offset / 4 + i] != 0xabcddcba) return B_BAD_DATA;
		s->created = false;
		return B_OK;
	};
	for (AmdgpuClient* client = sClients; client != NULL; client = client->next) {
		status_t status = invalidate(client->video);
		if (status != B_OK) return status;
	}
	for (UvdSession* s = sQuarantinedVideos; s != NULL; s = s->nextQuarantined) {
		status_t status = invalidate(s);
		if (status != B_OK) return status;
	}
	return B_OK;
}

static status_t
RecoverGraphics(bool allowLive)
{
	// Old queues must be terminal. Live lost VMs/BOs/video keep their identity
	// and allocation; only closed quarantine can be reclaimed by the reset.
	if (sPending != 0 || sGraphicsExecuting || sVideoExecuting
		|| (!allowLive && (sLiveHandles != 0 || sVMCount != 0 || sVideoSessions != 0)))
		return B_BUSY;
	if (sFault == B_OK || sRecoveryAttempted) return B_NOT_ALLOWED;
	// SDMA reset is not qualified yet. A partial GFX initialization is also
	// excluded; standalone video can recover before GFX has ever started.
	if (sEngine.faulted || (sGfx.attempted && !sGfx.ready)
		|| (!sGfx.faulted && !sUvd.faulted)) return B_NOT_SUPPORTED;
	const bool resetGfx = sGfx.ready, resetUvd = sUvd.faulted;
	for (AmdgpuClient* client = sClients; client != NULL; client = client->next) {
		if (client->loss == B_OK) return B_BAD_DATA;
	}
	InstalledFirmware images[6];
	amdgpu::FirmwareView firmware[4];
	status_t status = B_OK;
	if (resetGfx) {
		for (uint32 i = 0; i < 4; i++) {
			status = images[i].LoadGfx(i);
			if (status != B_OK) return status;
			firmware[i] = images[i].view;
		}
		status = images[4].LoadMec();
		if (status != B_OK) return status;
	}
	if (resetUvd) {
		status = images[5].LoadUvd();
		if (status != B_OK) return status;
	}
	sRecoveryAttempted = true;
	sRecovery.stage = 0;
	sRecovery.grbm_reset = sRecovery.srbm_reset = 0;
	sRecovery.reclaimed_vram = sRecovery.reclaimed_ram = 0;
	auto snapshot = [](uint32* out) {
		out[0] = sEngine.regs[0x2004]; out[1] = sEngine.regs[0x2002];
		out[2] = sEngine.regs[0x394]; out[3] = sEngine.regs[0x536] | sEngine.regs[0x537];
	};
	snapshot(sRecovery.before);
	if (resetGfx) {
		// A UVD fault loses every client too. Reset initialized GFX under the
		// same transaction before making any of those addresses reusable.
		sGfx.faulted = true; sGfx.Halt();
		status = sGfx.Recover(sInfo, sReservation, sRenderInfo, firmware, images[4].mec,
			sEngine, sGart, sRecovery, sVideoSessions != 0 || sQuarantinedVideos != NULL);
	} else {
		// The UVD failure path stopped the sole execution owner. Record and
		// acknowledge retained VM fault latches before testing fresh firmware.
		sEngine.regs[0x50c] |= 1; sEngine.regs[0x50d] |= 1; (void)sEngine.regs[0x50d];
		if (((sEngine.regs[0x536] | sEngine.regs[0x537]) & 0xff) != 0) status = B_BAD_DATA;
	}
	if (status == B_OK && resetUvd) {
		status = sUvd.Recover(images[5].view, sRecovery);
		if (status == B_OK) status = InvalidateVideoSessions();
	}
	if (status == B_OK) status = ReclaimGraphicsQuarantine();
	if (status == B_OK) {
		sFault = B_OK;
		sAutomaticPending = false;
		sRecovery.generation++;
		sRecovery.stage = 7;
		sRecoveryAttempted = false;
		dprintf("amdgpu: recovered engines GFX %u UVD %u generation %" B_PRIu64
			" reclaimed VRAM %" B_PRIu64 " RAM %" B_PRIu64 "\n",
			(unsigned)resetGfx, (unsigned)resetUvd, sRecovery.generation,
			sRecovery.reclaimed_vram, sRecovery.reclaimed_ram);
	} else {
		if (sGfx.ready) { sGfx.faulted = true; sGfx.Halt(); }
		if (resetUvd && sUvd.attempted) { sUvd.faulted = true; sUvd.Stop(); }
	}
	snapshot(sRecovery.after);
	sRecovery.status = status;
	sCompleted.NotifyAll();
	return status;
}

// Only the existing executor calls this, with sMutex held. The fault wake is
// not a timer or an observation retry. Never consume a pending request while
// accepted jobs or execution owners still hold the device.
static void
MaybeRecoverAutomatically()
{
	if (!sAutomaticPending || !sAutomaticRecovery || sStopping || sFault == B_OK
		|| sPending != 0 || sGraphicsExecuting || sVideoExecuting)
		return;
	sAutomaticPending = false;
	if (sEngine.faulted) {
		sAutomaticStatus = B_NOT_SUPPORTED;
		dprintf("amdgpu: automatic recovery excludes SDMA faults\n");
		return;
	}
	sAutomaticAttempts++;
	sAutomaticStatus = RecoverGraphics(true);
	if (sAutomaticStatus == B_OK) sAutomaticSuccesses++;
	dprintf("amdgpu: automatic recovery attempt %" B_PRIu64 " status %#" B_PRIx32
		" successful %" B_PRIu64 "\n", sAutomaticAttempts, sAutomaticStatus,
		sAutomaticSuccesses);
}

static status_t
Control(AmdgpuClient* client, uint32 op, void* data, size_t length)
{
	if (!sActive || sStopping)
		return B_DEV_NOT_READY;
	ObserveInterruptFault();
	if (client->team != team_get_current_team_id()
		|| (client->flags & O_ACCMODE) != O_RDWR)
		return B_NOT_ALLOWED;
	if (op == AMDGPU_UVD_TIMEOUT_TEST) {
		amdgpu_uvd_timeout_test request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if (request.flags || request.reserved) return B_BAD_VALUE;
		if (sFault != B_OK || client->loss != B_OK || !sUvd.ready) return B_DEV_NOT_READY;
		if (sPending || sGraphicsExecuting || sVideoExecuting) return B_BUSY;
		request.status = B_OK; request.sequence = request.fence = request.rptr = request.wptr = 0;
		request.uvd_status = 0; request.elapsed_us = 0;
		// Reject an unwritable result before deliberately faulting hardware.
		status = user_memcpy(data, &request, sizeof(request));
		if (status != B_OK) return status;
		status = sUvd.TimeoutTest(request);
		if (status != B_OK) SetFault(status);
		return user_memcpy(data, &request, sizeof(request));
	}
	if (op == AMDGPU_RECOVERY_POLICY) {
		amdgpu_recovery_policy request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if ((request.flags & ~AMDGPU_RECOVERY_SET_POLICY) || request.reserved
			|| request.reserved_out || request.enabled > 1
			|| (!request.flags && request.enabled)) return B_BAD_VALUE;
		const bool update = request.flags & AMDGPU_RECOVERY_SET_POLICY;
		if (update) {
			if (sFault != B_OK || sRecoveryAttempted) return B_NOT_ALLOWED;
			if (sPending || sGraphicsExecuting || sVideoExecuting) return B_BUSY;
		}
		request.flags = 0;
		if (!update) request.enabled = sAutomaticRecovery;
		request.pending = sAutomaticPending;
		request.status = sAutomaticStatus;
		request.attempts = sAutomaticAttempts;
		request.successes = sAutomaticSuccesses;
		status = user_memcpy(data, &request, sizeof(request));
		if (status == B_OK && update) sAutomaticRecovery = request.enabled != 0;
		return status;
	}
	if (op == AMDGPU_CLIENT_STATUS) {
		amdgpu_client_status request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if (request.flags != 0 || request.reserved != 0) return B_BAD_VALUE;
		request.flags = client->loss != B_OK ? AMDGPU_CLIENT_LOST : 0;
		if (client->loss != B_OK && sRecovery.generation > client->lossGeneration)
			request.flags |= AMDGPU_CLIENT_RESET_COMPLETED;
		request.status = client->loss;
		request.device_status = sFault;
		request.generation = sRecovery.generation;
		request.loss_generation = client->lossGeneration;
		request.submitted = client->submitted;
		request.completed = client->completed;
		request.failed_fence = client->failedFence;
		return user_memcpy(data, &request, sizeof(request));
	}
	if (op == AMDGPU_RECOVERY_INFO || op == AMDGPU_GFX_RECOVER
		|| op == AMDGPU_GFX_RECOVER_LIVE) {
		amdgpu_recovery_info request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if (request.flags != 0 || request.reserved != 0) return B_BAD_VALUE;
		status = op == AMDGPU_RECOVERY_INFO ? sFault
			: RecoverGraphics(op == AMDGPU_GFX_RECOVER_LIVE);
		request = RecoveryInfo();
		request.status = status;
		return user_memcpy(data, &request, sizeof(request));
	}
	if (op == AMDGPU_IRQ_INFO) {
		amdgpu_irq_info info;
		status_t status = ReadRequest(info, data, length);
		if (status != B_OK) return status;
		if (info.reserved != 0) return B_BAD_VALUE;
		sGfx.interrupts.Snapshot(info);
		return user_memcpy(data, &info, sizeof(info));
	}
	if (op == AMDGPU_DIAGNOSTIC_RAM_PAGE) {
		amdgpu_diagnostic_ram_page request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK) return status;
		if (request.flags != 0 || request.reserved != 0 || (request.offset & 4095) != 0)
			return B_BAD_VALUE;
		if (sFault != B_OK) return B_DEV_NOT_READY;
		Buffer* bo = Lookup(client, request.handle);
		if (bo == NULL || !bo->system || bo->area < 0 || bo->cpu == NULL
			|| bo->bytes < 4096 || request.offset > bo->bytes - 4096)
			return B_BAD_VALUE;
		// sMutex holds the live handle. RAM pages are wired for the BO lifetime;
		// independently cross-check the CPU mapping against the installed GART.
		physical_entry entry = {};
		uint32 count = 1;
		status = get_memory_map_etc(B_SYSTEM_TEAM, (const uint8*)bo->cpu + request.offset,
			4096, &entry, &count);
		if (status != B_OK) return status;
		uint64 pte = sGart.table[(bo->offset + request.offset) / 4096];
		if (count != 1 || entry.size != 4096 || (entry.address & 4095) != 0
			|| (pte & 7) != 7 || entry.address != (pte & GpuPageTable::kPhysicalMask))
			return B_BAD_DATA;
		request.physical_address = entry.address;
		return user_memcpy(data, &request, sizeof(request));
	}
	if (op == AMDGPU_OA_INFO) {
		amdgpu_oa_info info;
		status_t status = ReadRequest(info, data, length);
		if (status != B_OK) return status;
		if (info.flags != 0 || info.reserved != 0) return B_BAD_VALUE;
		status = WaitDmaIdle();
		if (status == B_OK) status = sGfx.OaInfo(info);
		if (status != B_OK) return status;
		return user_memcpy(data, &info, sizeof(info));
	}
	if (op == AMDGPU_GWS_INFO) {
		amdgpu_gws_info info;
		status_t status = ReadRequest(info, data, length);
		if (status != B_OK) return status;
		if (info.flags != 0 || info.reserved != 0) return B_BAD_VALUE;
		status = WaitDmaIdle();
		if (status == B_OK) status = sGfx.GwsInfo(info);
		if (status != B_OK) return status;
		return user_memcpy(data, &info, sizeof(info));
	}
	if (op == AMDGPU_SHADER_MEMORY_INFO) {
		amdgpu_shader_memory_info info;
		status_t status = ReadRequest(info, data, length);
		if (status != B_OK) return status;
		if (info.flags != 0 || info.reserved != 0) return B_BAD_VALUE;
		status = WaitDmaIdle();
		if (status == B_OK) status = sGfx.ShaderMemoryInfo(info);
		if (status != B_OK) return status;
		return user_memcpy(data, &info, sizeof(info));
	}
	if (op == AMDGPU_RENDER_INFO) {
		amdgpu_render_info info;
		status_t status = ReadRequest(info, data, length);
		if (status != B_OK) return status;
		if (info.flags != 0 || info.reserved != 0) return B_BAD_VALUE;
		if (sRenderInfo.gfxMajor == 0) return B_NOT_SUPPORTED;
		status = WaitDmaIdle(false);
		if (status == B_OK) status = StartGraphics();
		if (status == B_OK) status = sGfx.RenderInfo(sRenderInfo, info);
		if (status != B_OK) return status;
		info.capabilities = AMDGPU_RENDER_ROOT_SUBMIT | AMDGPU_RENDER_SYNC_SUBMIT
			| AMDGPU_RENDER_COPY_SUBMIT | AMDGPU_RENDER_CHECKED_COPY | AMDGPU_RENDER_QUEUE_COPY;
		info.total_vram = sInfo.vram_size;
		info.visible_vram = sInfo.bar_size[0];
		info.total_gart = Gart::kSize;
		info.address_start = kClientAddressStart;
		info.address_end = GpuPageTable::kSize;
		info.max_buffer_bytes = info.max_mapping_bytes = kMaxBufferBytes;
		info.max_mapped_bytes = kMaxMappedBytes;
		info.max_mappings = kMaxMappings;
		info.max_vm_clients = kMaxVMClients;
		info.max_buffers = kMaxBuffers;
		info.page_size = 4096;
		info.max_ib_bytes = kMaxIbBytes;
		info.ib_address_alignment = kIbAddressAlignment;
		info.ib_size_alignment = kIbSizeAlignment;
		return user_memcpy(data, &info, sizeof(info));
	}
	if (op == AMDGPU_GFX_QUEUE_COPY) {
		if (client->graphicsBusy) return B_BUSY;
		client->graphicsBusy = true;
		status_t status = QueueGraphics(client, data, length);
		client->graphicsBusy = false;
		return status;
	}
	if ((op >= AMDGPU_VM_INFO && op <= AMDGPU_GFX_SUBMIT)
		|| op == AMDGPU_GFX_SUBMIT_COPY || op == AMDGPU_GFX_GDS_PROBE_COPY) {
		if (client->graphicsBusy || client->graphicsPending) return B_BUSY;
		client->graphicsBusy = true;
		status_t status = VMControl(client, op, data, length);
		client->graphicsBusy = false;
		return status;
	}
	if (op >= AMDGPU_VIDEO_CREATE && op <= AMDGPU_HEVC_DECODE) {
		// WaitDmaIdle releases sMutex. Keep another thread on this file from
		// destroying/replacing the session while the first call holds pointers.
		if (client->videoBusy) return B_BUSY;
		client->videoBusy = true;
		status_t status = VideoControl(client, op, data, length);
		client->videoBusy = false;
		return status;
	}
	if (op == AMDGPU_MEMORY_INFO) {
		amdgpu_memory_info info;
		status_t status = ReadRequest(info, data, length);
		if (status != B_OK)
			return status;
		info.total_vram = sInfo.vram_size;
		info.visible_vram = sInfo.bar_size[0];
		info.allocated_bytes = sAllocator.allocated;
		info.client_bytes = client->bytes;
		info.submitted = client->submitted;
		info.completed = client->completed;
		info.pending_jobs = sPending;
		info.faulted = sFault != B_OK;
		return user_memcpy(data, &info, sizeof(info));
	}
	if (op == AMDGPU_GART_INFO) {
		amdgpu_gart_info info;
		status_t status = ReadRequest(info, data, length);
		if (status != B_OK)
			return status;
		info.total_bytes = Gart::kSize;
		info.allocated_bytes = sGartAllocator.allocated;
		info.client_bytes = client->systemBytes;
		info.bound_pages = sGart.boundPages;
		info.scatter_boundaries = sGart.scatterBoundaries;
		info.vm_fault_status = sGart.regs[0x536];
		info.vm_fault_address = sGart.regs[0x53e];
		return user_memcpy(data, &info, sizeof(info));
	}
	if (op == AMDGPU_WAIT_FENCE) {
		amdgpu_fence_wait request;
		status_t status = ReadRequest(request, data, length);
		if (status != B_OK)
			return status;
		if (request.reserved != 0 || request.fence == 0 || request.fence > client->submitted
			|| request.timeout_us < 0 || request.timeout_us > 5000000)
			return B_BAD_VALUE;
		bigtime_t deadline = system_time() + request.timeout_us;
		while (client->completed < request.fence) {
			if (request.timeout_us == 0)
				return B_WOULD_BLOCK;
			status = sCompleted.Wait(&sMutex, B_ABSOLUTE_TIMEOUT | B_CAN_INTERRUPT, deadline);
			if (status != B_OK && client->completed < request.fence)
				return status;
		}
		request.status = client->failedFence != 0 && request.fence >= client->failedFence
			? client->failure : B_OK;
		return user_memcpy(data, &request, sizeof(request));
	}
	if (op == AMDGPU_SUBMIT_DMA) {
		amdgpu_dma_submit c;
		status_t status = ReadRequest(c, data, length);
		if (status != B_OK)
			return status;
		// Recovery starts a new device epoch, but cannot turn this file's
		// failed queue back into a successful fence timeline. Keep old waits
		// and cleanup available; new work requires a fresh client.
		if (sFault != B_OK || client->loss != B_OK)
			return B_DEV_NOT_READY;
		if ((c.operation != AMDGPU_DMA_COPY && c.operation != AMDGPU_DMA_FILL)
			|| c.bytes == 0 || c.bytes > kMaxBufferBytes
			|| ((c.source_offset | c.destination_offset | c.bytes) & 3) != 0)
			return B_BAD_VALUE;
		Buffer* dst = Lookup(client, c.destination);
		Buffer* src = c.operation == AMDGPU_DMA_COPY ? Lookup(client, c.source) : NULL;
		if (dst == NULL || c.destination_offset > dst->bytes
			|| c.bytes > dst->bytes - c.destination_offset)
			return B_BAD_VALUE;
		if (c.operation == AMDGPU_DMA_COPY) {
			if (src == NULL || c.source_offset > src->bytes || c.bytes > src->bytes - c.source_offset)
				return B_BAD_VALUE;
			if (src == dst && (c.source_offset < c.destination_offset
				? c.destination_offset - c.source_offset < c.bytes
				: c.source_offset - c.destination_offset < c.bytes))
				return B_BAD_VALUE;
		} else if (c.source != 0 || c.source_offset != 0)
			return B_BAD_VALUE;
		if (sPending >= 256 || client->pending >= 64 || client->submitted == UINT64_MAX)
			return B_WOULD_BLOCK;
		Job* job = (Job*)calloc(1, sizeof(Job));
		if (job == NULL)
			return B_NO_MEMORY;
		c.fence = client->submitted + 1;
		status = user_memcpy(data, &c, sizeof(c));
		if (status != B_OK) {
			free(job);
			return status;
		}
		job->client = client;
		job->source = src;
		job->destination = dst;
		job->command = c;
		client->submitted = c.fence;
		client->pending++;
		client->references++;
		dst->references++;
		if (src != NULL)
			src->references++;
		if (sLast != NULL)
			sLast->next = job;
		else
			sFirst = job;
		sLast = job;
		sPending++;
		sDmaPending++;
		release_sem(sJobs);
		return B_OK;
	}
	if (op != AMDGPU_CREATE_BUFFER && op != AMDGPU_CREATE_SYSTEM_BUFFER
		&& op != AMDGPU_CREATE_DEVICE_BUFFER
		&& op != AMDGPU_CREATE_DIAGNOSTIC_RAM_BUFFER
		&& op != AMDGPU_MAP_BUFFER && op != AMDGPU_FREE_BUFFER)
		return B_DEV_INVALID_IOCTL;
	amdgpu_buffer request;
	status_t status = ReadRequest(request, data, length);
	if (status != B_OK)
		return status;
	if (request.reserved != 0)
		return B_BAD_VALUE;
	if (op == AMDGPU_CREATE_BUFFER || op == AMDGPU_CREATE_SYSTEM_BUFFER
		|| op == AMDGPU_CREATE_DEVICE_BUFFER || op == AMDGPU_CREATE_DIAGNOSTIC_RAM_BUFFER) {
		if (sFault != B_OK || client->loss != B_OK)
			return B_DEV_NOT_READY;
		if (request.bytes == 0 || request.bytes > kMaxBufferBytes)
			return B_BAD_VALUE;
		const bool diagnosticRAM = op == AMDGPU_CREATE_DIAGNOSTIC_RAM_BUFFER;
		if (diagnosticRAM && request.bytes != 8192) return B_BAD_VALUE;
		const bool deviceOnly = op == AMDGPU_CREATE_DEVICE_BUFFER;
		if (deviceOnly) {
			// Initialization uses the same SDMA engine as queued client jobs.
			// Drain first; the reacquired sMutex excludes further submissions.
			status = WaitDmaIdle();
			if (status != B_OK)
				return status;
		} else if (op == AMDGPU_CREATE_SYSTEM_BUFFER || diagnosticRAM) {
			// Bind/Flush must not race GFX's VMID2 invalidate request.
			status = WaitDmaIdle(false);
			if (status != B_OK) return status;
		}
		// The drain releases sMutex. This file can fail while waiting, and
		// another resource-free client can recover the device before we
		// reacquire it. A cleared global fault cannot revive this lost file.
		if (sFault != B_OK || client->loss != B_OK)
			return B_DEV_NOT_READY;
		if (client->bufferCount >= kMaxBuffers || sNextHandle == 0)
			return B_NO_MEMORY;
		Buffer* bo = (Buffer*)calloc(1, sizeof(Buffer));
		if (bo == NULL)
			return B_NO_MEMORY;
		bo->bytes = (request.bytes + 4095) & ~4095ULL;
		bo->system = op == AMDGPU_CREATE_SYSTEM_BUFFER || diagnosticRAM;
		bo->area = -1;
		VramAllocator& allocator = bo->system ? sGartAllocator : sAllocator;
		uint64 limit = bo->system ? Gart::kSize
			: (deviceOnly ? sInfo.vram_size : sInfo.bar_size[0]);
		if (!allocator.Allocate(bo->bytes, 4096, limit, bo->offset,
				deviceOnly ? sInfo.bar_size[0] : 0)) {
			free(bo);
			return B_NO_MEMORY;
		}
		bo->references = 1;
		bo->gpu = (bo->system ? Gart::kBase : sInfo.vram_gpu_base) + bo->offset;
		if (deviceOnly) {
			status = sEngine.Execute(AMDGPU_DMA_FILL, 0, bo->gpu, bo->bytes, 0);
			if (status != B_OK) {
				// Never publish or recycle an allocation with uncertain writes.
				SetFault(status);
				bo->poisoned = true;
				PutBuffer(bo);
				return status;
			}
		} else if (bo->system) {
			virtual_address_restrictions va = {};
			physical_address_restrictions pa = {};
			if (diagnosticRAM) {
				pa.low_address = 1ULL << 32;
				pa.high_address = 1ULL << 36;
				pa.alignment = 4096;
			}
			bo->area = create_area_etc(B_SYSTEM_TEAM, "amdgpu client RAM", bo->bytes,
				B_FULL_LOCK, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA, 0, 0, &va, &pa,
				(void**)&bo->cpu);
		} else {
			bo->area = map_physical_memory("amdgpu client VRAM", sInfo.bar_address[0] + bo->offset,
				bo->bytes, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
				(void**)&bo->cpu);
		}
		if (!deviceOnly && bo->area < 0) {
			status = bo->area;
			allocator.Free(bo->offset, bo->bytes);
			free(bo);
			return status;
		}
		if (!deviceOnly) {
			for (uint64 i = 0; i < bo->bytes / 4; i++)
				bo->cpu[i] = 0;
			__sync_synchronize();
			(void)bo->cpu[bo->bytes / 4 - 1];
		}
		if (bo->system) {
			status = sGart.Bind(bo->offset, bo->bytes, (const void*)bo->cpu);
			if (status != B_OK) {
				// An invalidation timeout requires retaining backing RAM.
				if (status == B_TIMED_OUT)
					SetFault(status);
				PutBuffer(bo);
				return status;
			}
		}
		bo->handle = sNextHandle++;
		request.handle = bo->handle;
		request.bytes = bo->bytes;
		request.address = 0;
		request.area = -1;
		status = user_memcpy(data, &request, sizeof(request));
		if (status != B_OK) {
			PutBuffer(bo);
			return status;
		}
		bo->next = client->buffers;
		client->buffers = bo;
		client->bufferCount++;
		sLiveHandles++;
		if (bo->system)
			client->systemBytes += bo->bytes;
		else
			client->bytes += bo->bytes;
		return B_OK;
	}
	Buffer* bo = Lookup(client, request.handle);
	if (bo == NULL)
		return B_BAD_VALUE;
	if (op == AMDGPU_MAP_BUFFER) {
		if (bo->area < 0)
			return B_NOT_ALLOWED;
		void* address = NULL;
		area_id area = vm_clone_area(client->team, "amdgpu buffer", &address,
			B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, 0, bo->area, true);
		if (area < 0)
			return area;
		request.area = area;
		request.address = (addr_t)address;
		request.bytes = bo->bytes;
		status = user_memcpy(data, &request, sizeof(request));
		if (status != B_OK)
			vm_delete_area(client->team, area, true);
		return status;
	}
	status = bo->area >= 0 ? vm_change_clones_to_null_areas(bo->area) : B_OK;
	if (status != B_OK)
		return status;
	Buffer** link = &client->buffers;
	while (*link != bo)
		link = &(*link)->next;
	*link = bo->next;
	client->bufferCount--;
	sLiveHandles--;
	if (bo->system)
		client->systemBytes -= bo->bytes;
	else
		client->bytes -= bo->bytes;
	PutBuffer(bo);
	return B_OK;
}

status_t
amdgpu_client_control(AmdgpuClient* client, uint32 op, void* data, size_t length,
	status_t (*initialize)())
{
	if (client->team != team_get_current_team_id()
		|| (client->flags & O_ACCMODE) != O_RDWR)
		return B_NOT_ALLOWED;
	mutex_lock(&sMutex);
	bool needsStart = !sActive;
	mutex_unlock(&sMutex);
	if (needsStart) {
		// Validate the triggering request before loading firmware or touching
		// engines. Close/free/map/submit cannot initialize an absent device.
		status_t status;
		if (op == AMDGPU_RENDER_INFO) {
			amdgpu_render_info request;
			status = ReadRequest(request, data, length);
			if (status == B_OK && (request.flags != 0 || request.reserved != 0))
				status = B_BAD_VALUE;
		} else if (op == AMDGPU_VM_INFO) {
			amdgpu_vm_info request;
			status = ReadRequest(request, data, length);
			if (status == B_OK && (request.reserved[0] != 0 || request.reserved[1] != 0))
				status = B_BAD_VALUE;
		} else if (op == AMDGPU_MEMORY_INFO) {
			amdgpu_memory_info request;
			status = ReadRequest(request, data, length);
		} else if (op == AMDGPU_GART_INFO) {
			amdgpu_gart_info request;
			status = ReadRequest(request, data, length);
		} else if (op == AMDGPU_CREATE_BUFFER || op == AMDGPU_CREATE_SYSTEM_BUFFER
			|| op == AMDGPU_CREATE_DEVICE_BUFFER) {
			amdgpu_buffer request;
			status = ReadRequest(request, data, length);
			if (status == B_OK && (request.reserved != 0 || request.bytes == 0
				|| request.bytes > kMaxBufferBytes))
				status = B_BAD_VALUE;
		} else if (op == AMDGPU_VIDEO_CREATE) {
			amdgpu_video_create request;
			amdgpu::UvdH264Layout layout;
			status = ReadRequest(request, data, length);
			if (status == B_OK && !amdgpu::UvdH264Size(request.config, layout)) status = B_BAD_VALUE;
		} else if (op == AMDGPU_HEVC_CREATE) {
			amdgpu_hevc_create request;
			amdgpu::UvdHevcLayout layout;
			status = ReadRequest(request, data, length);
			if (status == B_OK && !amdgpu::UvdHevcSize(request.config, layout)) status = B_BAD_VALUE;
		} else
			return B_DEV_NOT_READY;
		if (status != B_OK)
			return status;
		// Driver startup acquires sLock before sMutex. Do not hold sMutex
		// here: concurrent first clients serialize in that startup callback.
		status = initialize();
		if (status != B_OK)
			return status;
	}
	mutex_lock(&sMutex);
	status_t status = Control(client, op, data, length);
	mutex_unlock(&sMutex);
	return status;
}

status_t
amdgpu_device_gfx_test(const amdgpu::FirmwareView firmware[4],
	amdgpu_gfx_test& result, const amdgpu::MecFirmwareView* mec)
{
	mutex_lock(&sMutex);
	ObserveInterruptFault();
	status_t status = !sActive || sStopping || sFault != B_OK ? B_DEV_NOT_READY
		: sPending != 0 || sVideoExecuting || sGraphicsExecuting ? B_BUSY
		: sGfx.Test(sEngine.regs, sInfo, sReservation,
			firmware, sEngine, sGart, result, mec);
	if (!sGraphicsExecuting && sGfx.faulted && sFault == B_OK)
		SetFault(status);
	mutex_unlock(&sMutex);
	return status;
}

status_t
amdgpu_device_uvd_test(const amdgpu::FirmwareView& firmware,
	amdgpu_uvd_test& result, void* output)
{
	mutex_lock(&sMutex);
	ObserveInterruptFault();
	status_t status = !sActive || sStopping || sFault != B_OK ? B_DEV_NOT_READY
		: sPending != 0 || sVideoExecuting || sGraphicsExecuting ? B_BUSY
		: sUvd.Test(sEngine.regs, sInfo, sReservation, sUvdClocksQualified,
			firmware, result, output);
	if (!sVideoExecuting && sUvd.faulted && sFault == B_OK)
		SetFault(status);
	mutex_unlock(&sMutex);
	return status;
}

void
amdgpu_device_stop()
{
	if (!sActive)
		return;
	mutex_lock(&sMutex);
	ObserveInterruptFault();
	sStopping = true;
	sCompleted.NotifyAll();
	while (sVideoExecuting || sGraphicsExecuting)
		sCompleted.Wait(&sMutex);
	mutex_unlock(&sMutex);
	release_sem(sJobs);
	status_t result;
	wait_for_thread(sWorker, &result);
	delete_sem(sJobs);
	sJobs = sWorker = -1;
	sUvd.Uninitialize();
	sGfx.Uninitialize();
	sEngine.Uninitialize();
	sGart.Uninitialize(sFault != B_OK);
	sGartAllocator.Uninit();
	sAllocator.Uninit();
	sActive = false;
}
