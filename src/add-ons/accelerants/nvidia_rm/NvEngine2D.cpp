#include "NvEngine2D.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include <ErrorUtils.h>

extern "C" {
#include "nvos.h"
#include "nvgputypes.h"
#include "class/cl0002.h" // NV01_CONTEXT_DMA
#include "class/cl003e.h" // NV01_MEMORY_SYSTEM
#include "class/cl0040.h" // NV01_MEMORY_LOCAL_USER
#include "class/cl50a0.h" // NV50_MEMORY_VIRTUAL
#include "class/cl90f1.h" // FERMI_VASPACE_A
#include "class/cl902d.h" // FERMI_TWOD_A
#include "class/clb06f.h" // MAXWELL_CHANNEL_GPFIFO_A
#include "class/clc06f.h" // PASCAL_CHANNEL_GPFIFO_A
#include "class/cl2080_notification.h" // NV2080_ENGINE_TYPE_GRAPHICS
#include "alloc/alloc_channel.h"
#include "ctrl/ctrla06f/ctrla06fgpfifo.h"
}


// Methods of FERMI_TWOD_A (cl902d.h in NVIDIA's open-gpu-doc).
enum {
	kTwoDSetObject = 0x0000,
	kTwoDWaitForIdle = 0x0110,
	kTwoDSetDstFormat = 0x0200,			// format, memory layout
	kTwoDSetDstPitch = 0x0214,			// pitch, width, height, offset upper, lower
	kTwoDSetSrcFormat = 0x0230,			// format, memory layout
	kTwoDInvalidateTextureCache = 0x0240,
	kTwoDSetSrcPitch = 0x0244,			// pitch, width, height, offset upper, lower
	kTwoDSetNumProcessingClusters = 0x0260,
	kTwoDSetRenderEnableC = 0x026c,
	kTwoDSetClipEnable = 0x0290,
	kTwoDSetColorKeyFormat = 0x0294,	// format, key, enable
	kTwoDSetRop = 0x02a0,
	kTwoDSetOperation = 0x02ac,
	kTwoDSetPixelsFromMemoryCorralSize = 0x0884,
	kTwoDSetPixelsFromMemorySafeOverlap = 0x0888,
	kTwoDSetPixelsFromMemorySampleMode = 0x088c,
	kTwoDSetPixelsFromMemoryDstX0 = 0x08b0,	// x0, y0, width, height
	kTwoDSetPixelsFromMemoryDuDxFrac = 0x08c0,	// du/dx frac, int, dv/dy frac, int
	kTwoDSetPixelsFromMemorySrcX0Frac = 0x08d0,	// x0 frac, int, y0 frac, int (go)
	kTwoDRenderSolidPrimMode = 0x0580,	// mode, color format, color
	kTwoDRenderSolidPrimPoint = 0x0600,	// x, y of point 0, then point 1 (go)
};

enum {
	kFormatA8R8G8B8 = 0xcf,
	kFormatX8R8G8B8 = 0xe6,
	kLayoutPitch = 1,
	kOperationSrcCopyAnd = 0,		// leaves out what the colour key matches
	kOperationSrcCopy = 3,
	kOperationRop = 4,
	kRopInvertDestination = 0x55,
	kSolidPrimRects = 4,
	kColorKeyA8R8G8B8 = 2,
	kSampleOriginCenter = 0,
	kSampleFilterBilinear = 1 << 4,
	kInvalidateL1AndL2 = 2,
};

// The channel's own methods, on any subchannel.
enum {
	kHostSemaphoreA = 0x0010,			// offset upper, lower, payload, operation
	kSemaphoreRelease4Bytes = 0x2 | (1 << 24),	// waits for idle first
};

static const uint32 kSubchannel2D = 3;
static const uint32 kGpFifoEntries = 1024;
static const uint32 kPushBufferSize = 1024 * 1024;
static const uint32 kPushWords = kPushBufferSize / 4;
// Room at the end of a submission for the semaphore release.
static const uint32 kTailWords = 8;
static const uint32 kUserdGpGet = 0x88 / 4;
static const uint32 kUserdGpPut = 0x8c / 4;

static const NvU64 kBigPage = 2 * 1024 * 1024;


static inline NvU64 AlignUp(NvU64 value, NvU64 alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


NvEngine2D::Buffer &NvEngine2D::Buffer::operator=(Buffer &&other)
{
	if (this == &other)
		return *this;
	Release();
	fRm = other.fRm;
	fDevice = other.fDevice;
	fMemory = other.fMemory;
	fPhysical = std::move(other.fPhysical);
	fVirtual = std::move(other.fVirtual);
	fMapping = std::move(other.fMapping);
	fGpuAddress = other.fGpuAddress;
	fSize = other.fSize;
	other.fMemory = 0;
	other.fGpuAddress = 0;
	other.fSize = 0;
	return *this;
}


void NvEngine2D::Buffer::Release()
{
	// the CPU's view first, then the GPU's, then the memory
	fMapping = NvRmMemoryMapping();
	if (fGpuAddress != 0 && fVirtual.Get() != 0) {
		try {
			fRm->UnmapMemoryDma(fDevice, fVirtual.Get(), fMemory, 0, fGpuAddress);
		} catch (const std::system_error &ex) {
			debug_printf("[!] nvidia_rm 2D: unmapping a buffer: %s\n", ex.what());
		}
	}
	try {
		fVirtual = NvRmObject();
		fPhysical = NvRmObject();
	} catch (const std::system_error &ex) {
		debug_printf("[!] nvidia_rm 2D: freeing a buffer: %s\n", ex.what());
	}
	fMemory = 0;
	fGpuAddress = 0;
	fSize = 0;
}


NvEngine2D::Buffer NvEngine2D::Allocate(NvU64 size, bool systemMemory, bool mapped)
{
	Buffer buffer;
	buffer.fRm = &fRm;
	buffer.fDevice = fRmDev.Device().Get();
	size = AlignUp(size, B_PAGE_SIZE);
	buffer.fSize = size;

	NV_MEMORY_ALLOCATION_PARAMS physical {};
	physical.owner = fRm.Client().Get();
	physical.type = NVOS32_TYPE_IMAGE;
	physical.flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE;
	physical.size = size;
	physical.alignment = B_PAGE_SIZE;
	// Big pages where the memory allows them: with 4 KiB ones the engine
	// spends its time on address translation once a surface is screen
	// sized (a 4K fill ran at a quarter of the 1080p rate).
	const char *pageSize = getenv("NV2D_SYSMEM_PAGES");
	if (!systemMemory)
		physical.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _BIG);
	else if (pageSize != NULL && strcmp(pageSize, "big") == 0)
		physical.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _BIG);
	else if (pageSize != NULL && strcmp(pageSize, "huge") == 0)
		physical.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _HUGE);
	else
		physical.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB);
	if (systemMemory) {
		physical.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _PCI)
			| DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED);
	} else {
		physical.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM)
			| DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED);
		physical.flags |= NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
	}
	buffer.fPhysical = fRmDev.Device().Alloc(
		systemMemory ? NV01_MEMORY_SYSTEM : NV01_MEMORY_LOCAL_USER, &physical);
	buffer.fMemory = buffer.fPhysical.Get();

	// Resman places the range; picking addresses ourselves runs it out of
	// page tables (the video decoder found that out).
	NV_MEMORY_ALLOCATION_PARAMS virt {};
	virt.owner = fRm.Client().Get();
	virt.type = NVOS32_TYPE_IMAGE;
	virt.flags = NVOS32_ALLOC_FLAGS_VIRTUAL;
	virt.size = size;
	virt.alignment = kBigPage;
	virt.size = AlignUp(size, kBigPage);
	virt.hVASpace = fVaSpace.Get();
	buffer.fVirtual = fRmDev.Device().Alloc(NV50_MEMORY_VIRTUAL, &virt);

	NvU32 flags = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL)
		| (systemMemory ? DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE)
			: DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE));
	buffer.fGpuAddress = fRm.MapMemoryDma(buffer.fDevice, buffer.fVirtual.Get(),
		buffer.fMemory, 0, size, flags);

	if (mapped) {
		buffer.fMapping = fRmDev.MapMemory(buffer.fMemory, systemMemory, 0, size, 0);
		memset(buffer.Address(), 0, size);
	}
	return buffer;
}


NvEngine2D::Buffer NvEngine2D::Import(NvHandle memory, NvU64 size)
{
	Buffer buffer;
	buffer.fRm = &fRm;
	buffer.fDevice = fRmDev.Device().Get();
	buffer.fMemory = memory;
	buffer.fSize = AlignUp(size, B_PAGE_SIZE);

	// The frame buffer may sit on big pages, which want a range aligned to
	// them.
	NV_MEMORY_ALLOCATION_PARAMS virt {};
	virt.owner = fRm.Client().Get();
	virt.type = NVOS32_TYPE_IMAGE;
	virt.flags = NVOS32_ALLOC_FLAGS_VIRTUAL;
	virt.size = AlignUp(size, kBigPage);
	virt.alignment = kBigPage;
	virt.hVASpace = fVaSpace.Get();
	buffer.fVirtual = fRmDev.Device().Alloc(NV50_MEMORY_VIRTUAL, &virt);

	buffer.fGpuAddress = fRm.MapMemoryDma(buffer.fDevice, buffer.fVirtual.Get(),
		memory, 0, buffer.fSize, DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL)
			| DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE));
	return buffer;
}


NvEngine2D::NvEngine2D(NvRmApi &rm, NvRmDevice &rmDev):
	fRm(rm),
	fRmDev(rmDev)
{
	try {
		Init();
	} catch (const std::system_error &ex) {
		debug_printf("[!] nvidia_rm 2D: %s failed: %s\n", fStep, ex.what());
		throw;
	}
}


const char *NvEngine2D::FailedStep() const
{
	return fStep;
}


void NvEngine2D::Init()
{
	fStep = "the address space";
	NV_VASPACE_ALLOCATION_PARAMETERS vaParams {};
	vaParams.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS;
	fVaSpace = fRmDev.Device().Alloc(FERMI_VASPACE_A, &vaParams);

	fStep = "the channel's buffers";
	fNotifier = Allocate(B_PAGE_SIZE, true);
	fGpFifo = Allocate(kGpFifoEntries * 8, true);
	fPushBuffer = Allocate(kPushBufferSize, true);
	fSemaphore = Allocate(B_PAGE_SIZE, true);
	fPush = (uint32*)fPushBuffer.Address();

	fStep = "the error notifier";
	NV_CONTEXT_DMA_ALLOCATION_PARAMS notifierParams {};
	notifierParams.flags = DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL)
		| DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE);
	notifierParams.hMemory = fNotifier.fMemory;
	notifierParams.offset = 0;
	notifierParams.limit = fNotifier.Size() - 1;
	fErrorContext = fRmDev.Device().Alloc(NV01_CONTEXT_DMA, &notifierParams);

	fStep = "the channel";
	NV_CHANNEL_ALLOC_PARAMS channelParams {};
	channelParams.hObjectError = fErrorContext.Get();
	channelParams.gpFifoOffset = fGpFifo.GpuAddress();
	channelParams.gpFifoEntries = kGpFifoEntries;
	channelParams.hVASpace = fVaSpace.Get();
	channelParams.engineType = NV2080_ENGINE_TYPE_GRAPHICS;
	// Pascal, else Maxwell; later cards want a doorbell, which this does not
	// ring, and say no here.
	try {
		fChannel = fRmDev.Device().Alloc(PASCAL_CHANNEL_GPFIFO_A, &channelParams);
	} catch (const std::system_error &) {
		fChannel = fRmDev.Device().Alloc(MAXWELL_CHANNEL_GPFIFO_A, &channelParams);
	}

	fStep = "USERD";
	// Before Volta resman owns USERD, reached by mapping the channel.
	fUserdMapping = fRmDev.MapMemory(fChannel.Get(), false, 0, B_PAGE_SIZE,
		DRF_DEF(OS33, _FLAGS, _FIFO_MAPPING, _ENABLE), fRmDev.Subdevice().Get());
	fUserd = (volatile uint32*)fUserdMapping.Address();

	fStep = "the 2D object";
	fTwoD = fChannel.Alloc(FERMI_TWOD_A, nullptr);

	fStep = "binding the channel";
	NVA06F_CTRL_BIND_PARAMS bind {};
	bind.engineType = NV2080_ENGINE_TYPE_GRAPHICS;
	fChannel.Control(NVA06F_CTRL_CMD_BIND, &bind, sizeof(bind));
	fStep = "scheduling the channel";
	NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS schedule {};
	schedule.bEnable = NV_TRUE;
	fChannel.Control(NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &schedule, sizeof(schedule));

	fStep = "the first submission";
	// The state everything below assumes, as nouveau sets it up.
	std::lock_guard<std::recursive_mutex> lock(fLock);
	Method(kTwoDSetObject, FERMI_TWOD_A);
	Method(kTwoDSetNumProcessingClusters, 0);
	SetOperation(kOperationSrcCopy);
	Method(kTwoDSetClipEnable, 0);
	uint32 colorKey[] = {kColorKeyA8R8G8B8, 0, 0};
	Methods(kTwoDSetColorKeyFormat, colorKey, 3);
	Method(kTwoDSetPixelsFromMemoryCorralSize, 0x3f);
	Method(kTwoDSetPixelsFromMemorySafeOverlap, 1);
	Method(kTwoDSetRenderEnableC, 1);
	uint32 unscaled[] = {0, 1, 0, 1};
	Methods(kTwoDSetPixelsFromMemoryDuDxFrac, unscaled, 4);
	Method(kTwoDSetPixelsFromMemorySampleMode, kSampleOriginCenter);
	if (!Wait(Submit(), 1000000)) {
		debug_printf("[!] nvidia_rm 2D: the engine does not answer\n");
		RaiseErrno(ETIMEDOUT);
	}
	fStep = nullptr;
}


NvEngine2D::~NvEngine2D()
{
	if (!fLost)
		WaitIdle();
	fTarget = Surface();
	fTwoD = NvRmObject();
	fUserdMapping = NvRmMemoryMapping();
	fChannel = NvRmObject();
	fErrorContext = NvRmObject();
	fSemaphore.Release();
	fPushBuffer.Release();
	fGpFifo.Release();
	fNotifier.Release();
	fVaSpace = NvRmObject();
}


// #pragma mark - the push buffer


void NvEngine2D::Reserve(uint32 words)
{
	if (fPushAt + words + kTailWords <= kPushWords)
		return;
	// Back to the start: whatever is there was handed over a lap ago, so
	// once everything is done, nothing reads it any more.
	Wait(Submit());
	fPushStart = fPushAt = 0;
}


void NvEngine2D::Methods(uint32 method, const uint32 *values, uint32 count)
{
	if (fLost)
		return;
	Reserve(count + 1);
	// A Fermi method header: increasing methods, count arguments.
	fPush[fPushAt++] = (1u << 29) | (count << 16) | (kSubchannel2D << 13)
		| (method >> 2);
	for (uint32 i = 0; i < count; i++)
		fPush[fPushAt++] = values[i];
}


void NvEngine2D::Method(uint32 method, uint32 value)
{
	Methods(method, &value, 1);
}


uint32 NvEngine2D::Submit()
{
	if (fLost || fPushAt == fPushStart)
		return fSequence;

	uint32 sequence = fSequence + 1;
	NvU64 semaphore = fSemaphore.GpuAddress();
	uint32 release[] = {(uint32)(semaphore >> 32) & 0xff, (uint32)semaphore,
		sequence, kSemaphoreRelease4Bytes};
	fPush[fPushAt++] = (1u << 29) | (4 << 16) | (kSubchannel2D << 13)
		| (kHostSemaphoreA >> 2);
	for (uint32 value: release)
		fPush[fPushAt++] = value;

	// Wait for a free entry: the GPU has fetched up to GPGet.
	uint32 next = (fGpPut + 1) % kGpFifoEntries;
	bigtime_t start = system_time();
	while (next == fUserd[kUserdGpGet]) {
		if (CheckLost() || system_time() - start > 2000000) {
			debug_printf("[!] nvidia_rm 2D: the channel stopped fetching\n");
			fLost = true;
			return fSequence;
		}
		snooze(20);
	}

	NvU64 address = fPushBuffer.GpuAddress() + (NvU64)fPushStart * 4;
	uint32 words = fPushAt - fPushStart;
	uint32 *entry = (uint32*)fGpFifo.Address() + 2 * fGpPut;
	entry[0] = (uint32)address & ~3u;
	entry[1] = ((uint32)(address >> 32) & 0xff) | (words << 10);
	fGpPut = next;
	// the methods and the entry are in memory before the GPU is told
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	fUserd[kUserdGpPut] = fGpPut;

	fPushStart = fPushAt;
	fSequence = sequence;
	return sequence;
}


bool NvEngine2D::IsDone(uint32 sequence) const
{
	uint32 done = *(volatile uint32*)fSemaphore.Address();
	return (int32)(done - sequence) >= 0;
}


bool NvEngine2D::CheckLost()
{
	const NvNotification *notifier = (const NvNotification*)fNotifier.Address();
	const volatile NvNotification &error
		= notifier[NV_CHANNELGPFIFO_NOTIFICATION_TYPE_ERROR];
	if (error.status != 0) {
		if (!fLost) {
			debug_printf("[!] nvidia_rm 2D: channel error %#" B_PRIx32
				" (status %#x)\n", (uint32)error.info32, (unsigned)error.status);
		}
		fLost = true;
	}
	return fLost;
}


bool NvEngine2D::Wait(uint32 sequence, bigtime_t timeout)
{
	if (fLost)
		return false;
	// The engine is quick: most waits are over before a snooze would be.
	bigtime_t start = system_time();
	while (!IsDone(sequence)) {
		bigtime_t waited = system_time() - start;
		if (waited > timeout || CheckLost()) {
			if (!fLost) {
				debug_printf("[!] nvidia_rm 2D: submission %" B_PRIu32
					" not done after %" B_PRId64 " us (done: %" B_PRIu32
					"), giving up on the engine\n", sequence, waited,
					*(volatile uint32*)fSemaphore.Address());
			}
			fLost = true;
			return false;
		}
		if (waited > 50)
			snooze(waited > 1000 ? 200 : 20);
	}
	return true;
}


// #pragma mark - operations


void NvEngine2D::SetTarget(const Surface &surface)
{
	fTarget = surface;
}


void NvEngine2D::SetDestination(const Surface &surface)
{
	uint32 format[] = {kFormatA8R8G8B8, kLayoutPitch};
	Methods(kTwoDSetDstFormat, format, 2);
	uint32 values[] = {surface.bytesPerRow, surface.width, surface.height,
		(uint32)(surface.gpuAddress >> 32), (uint32)surface.gpuAddress};
	Methods(kTwoDSetDstPitch, values, 5);
}


void NvEngine2D::SetSource(const Surface &surface, bool opaque)
{
	uint32 format[] = {opaque ? kFormatX8R8G8B8 : kFormatA8R8G8B8, kLayoutPitch};
	Methods(kTwoDSetSrcFormat, format, 2);
	uint32 values[] = {surface.bytesPerRow, surface.width, surface.height,
		(uint32)(surface.gpuAddress >> 32), (uint32)surface.gpuAddress};
	Methods(kTwoDSetSrcPitch, values, 5);
	// what the CPU wrote since, or the engine itself, is read afresh
	Method(kTwoDInvalidateTextureCache, kInvalidateL1AndL2);
}


void NvEngine2D::SetOperation(uint32 operation)
{
	if (operation == fOperation)
		return;
	Method(kTwoDSetOperation, operation);
	fOperation = operation;
}


void NvEngine2D::BeginSolid(uint32 color)
{
	uint32 values[] = {kSolidPrimRects, kFormatA8R8G8B8, color};
	Methods(kTwoDRenderSolidPrimMode, values, 3);
}


// Exclusive right and bottom, as the engine has them.
void NvEngine2D::Rect(int32 left, int32 top, int32 right, int32 bottom)
{
	left = std::max<int32>(left, 0);
	top = std::max<int32>(top, 0);
	right = std::min<int32>(right, fTarget.width);
	bottom = std::min<int32>(bottom, fTarget.height);
	if (left >= right || top >= bottom)
		return;
	uint32 values[] = {(uint32)left, (uint32)top, (uint32)right, (uint32)bottom};
	Methods(kTwoDRenderSolidPrimPoint, values, 4);
}


void NvEngine2D::FillRectangles(uint32 color, const fill_rect_params *list,
	uint32 count)
{
	if (fLost || !fTarget.IsSet() || count == 0)
		return;
	SetDestination(fTarget);
	SetOperation(kOperationSrcCopy);
	BeginSolid(color);
	for (uint32 i = 0; i < count; i++)
		Rect(list[i].left, list[i].top, list[i].right + 1, list[i].bottom + 1);
}


void NvEngine2D::InvertRectangles(const fill_rect_params *list, uint32 count)
{
	if (fLost || !fTarget.IsSet() || count == 0)
		return;
	SetDestination(fTarget);
	Method(kTwoDSetRop, kRopInvertDestination);
	SetOperation(kOperationRop);
	BeginSolid(0);
	for (uint32 i = 0; i < count; i++)
		Rect(list[i].left, list[i].top, list[i].right + 1, list[i].bottom + 1);
	SetOperation(kOperationSrcCopy);
}


// A span is three values: the line, its first and its last pixel.
void NvEngine2D::FillSpans(uint32 color, const uint16 *list, uint32 count)
{
	if (fLost || !fTarget.IsSet() || count == 0)
		return;
	SetDestination(fTarget);
	SetOperation(kOperationSrcCopy);
	BeginSolid(color);
	for (uint32 i = 0; i < count; i++, list += 3) {
		uint16 left = std::min(list[1], list[2]);
		uint16 right = std::max(list[1], list[2]);
		Rect(left, list[0], right + 1, list[0] + 1);
	}
}


void NvEngine2D::BlitRect(int32 dstX, int32 dstY, uint32 width, uint32 height,
	int32 srcX, int32 srcY)
{
	uint32 dst[] = {(uint32)dstX, (uint32)dstY, width, height};
	Methods(kTwoDSetPixelsFromMemoryDstX0, dst, 4);
	uint32 src[] = {0, (uint32)srcX, 0, (uint32)srcY};
	Methods(kTwoDSetPixelsFromMemorySrcX0Frac, src, 4);
}


void NvEngine2D::Blit(const blit_params *list, uint32 count)
{
	BlitWith(kOperationSrcCopy, list, count);
}


void NvEngine2D::BlitWith(uint32 operation, const blit_params *list,
	uint32 count)
{
	if (fLost || !fTarget.IsSet() || count == 0)
		return;
	SetDestination(fTarget);
	// The "and" copy also leaves out pixels whose alpha is zero; read as
	// opaque, only the colour key decides (on the colour: in a B_RGB32 frame
	// buffer the fourth byte means nothing).
	SetSource(fTarget, operation == kOperationSrcCopyAnd);
	SetOperation(operation);
	for (uint32 i = 0; i < count; i++) {
		const blit_params &blit = list[i];
		BlitRect(blit.dest_left, blit.dest_top, blit.width + 1, blit.height + 1,
			blit.src_left, blit.src_top);
		// Blits in a list may overlap each other, each must see the last:
		// the engine is pipelined and reads through its texture cache.
		if (i + 1 < count) {
			Method(kTwoDWaitForIdle, 0);
			Method(kTwoDInvalidateTextureCache, kInvalidateL1AndL2);
		}
	}
	SetOperation(kOperationSrcCopy);
}


// The colour key alone makes matching pixels transparent black; it is the
// "and" form of the copy that leaves them out.
void NvEngine2D::TransparentBlit(uint32 transparentColor,
	const blit_params *list, uint32 count)
{
	if (fLost || !fTarget.IsSet() || count == 0)
		return;
	uint32 key[] = {kColorKeyA8R8G8B8, transparentColor | 0xff000000, 1};
	Methods(kTwoDSetColorKeyFormat, key, 3);
	BlitWith(kOperationSrcCopyAnd, list, count);
	Method(kTwoDSetColorKeyFormat + 8, 0);
}


void NvEngine2D::ScaledFilteredBlit(const scaled_blit_params *list, uint32 count)
{
	if (fLost || !fTarget.IsSet() || count == 0)
		return;
	SetDestination(fTarget);
	SetSource(fTarget);
	SetOperation(kOperationSrcCopy);
	Method(kTwoDSetPixelsFromMemorySampleMode,
		kSampleOriginCenter | kSampleFilterBilinear);
	for (uint32 i = 0; i < count; i++) {
		const scaled_blit_params &blit = list[i];
		uint32 srcWidth = blit.src_width + 1, srcHeight = blit.src_height + 1;
		uint32 dstWidth = blit.dest_width + 1, dstHeight = blit.dest_height + 1;
		int64 duDx = ((int64)srcWidth << 32) / dstWidth;
		int64 dvDy = ((int64)srcHeight << 32) / dstHeight;
		uint32 step[] = {(uint32)duDx, (uint32)(duDx >> 32), (uint32)dvDy,
			(uint32)(dvDy >> 32)};
		Methods(kTwoDSetPixelsFromMemoryDuDxFrac, step, 4);
		uint32 dst[] = {blit.dest_left, blit.dest_top, dstWidth, dstHeight};
		Methods(kTwoDSetPixelsFromMemoryDstX0, dst, 4);
		uint32 src[] = {0, blit.src_left, 0, blit.src_top};
		Methods(kTwoDSetPixelsFromMemorySrcX0Frac, src, 4);
		if (i + 1 < count) {
			Method(kTwoDWaitForIdle, 0);
			Method(kTwoDInvalidateTextureCache, kInvalidateL1AndL2);
		}
	}
	uint32 unscaled[] = {0, 1, 0, 1};
	Methods(kTwoDSetPixelsFromMemoryDuDxFrac, unscaled, 4);
	Method(kTwoDSetPixelsFromMemorySampleMode, kSampleOriginCenter);
}


void NvEngine2D::Copy(const Surface &from, const Surface &to,
	const clipping_rect *rects, uint32 count)
{
	if (fLost || !from.IsSet() || !to.IsSet() || count == 0)
		return;
	SetDestination(to);
	SetSource(from);
	SetOperation(kOperationSrcCopy);
	int32 width = std::min(from.width, to.width);
	int32 height = std::min(from.height, to.height);
	for (uint32 i = 0; i < count; i++) {
		clipping_rect r = rects[i];
		r.left = std::max<int32>(r.left, 0);
		r.top = std::max<int32>(r.top, 0);
		r.right = std::min<int32>(r.right, width - 1);
		r.bottom = std::min<int32>(r.bottom, height - 1);
		if (r.left > r.right || r.top > r.bottom)
			continue;
		BlitRect(r.left, r.top, r.right - r.left + 1, r.bottom - r.top + 1,
			r.left, r.top);
	}
}
