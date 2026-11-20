/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Copies by an NVIDIA GPU's 2D engine on nouveau_drm, for the flush of a
	desktop over several GPUs (multi_card.cpp).

	app_server draws into host memory (the flush source); each nouveau card
	gets a buffer object of the same pages (DRM_HAIKU_IOCTL_HOST_BO), and its
	2D engine copies the rectangles that changed from there into its frame
	buffer, so that the CPU no longer writes every changed pixel through each
	card's PCI aperture.

	Everything goes through nouveau's ABI 16 on the card's file: a channel
	on the graphics engine with the 2D object (NV50_2D on Tesla, FERMI_TWOD
	from Fermi on), a ring of push buffers in GART, and GEM_PUSHBUF
	submissions. Addresses are the buffers' in the file's VM (fixed from
	Tesla on), so no relocations.

	The copies are not waited for: each one copies what the source holds
	when the engine reads it, which is what app_server drew last or newer,
	so a copy that runs late shows nothing older than one on time would. A
	push buffer is waited for (GEM_CPU_PREP) only when the ring comes round
	to it again; a source's buffer object is freed only once the GPU is done
	with it (nouveau_drm's host buffer objects wait for that). Waiting on
	each copy halved app_server's flush rate on the Quadro 400 and the
	GT 710: the round trip took 2.5 ms where the CPU copied in 1.2 ms. */


#include "accelerant.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "drm.h"


#define TRACE(x...) debug_printf("amdgpu_kms: " x)


// #pragma mark - nouveau's ABI 16 (include/uapi/drm/nouveau_drm.h)


namespace {

struct nouveau_getparam {
	uint64	param;
	uint64	value;
};

struct nouveau_channel_alloc {
	uint32	fb_ctxdma_handle;
	uint32	tt_ctxdma_handle;
	int32	channel;
	uint32	pushbuf_domains;
	uint32	notifier_handle;
	struct {
		uint32	handle;
		uint32	grclass;
	} subchan[8];
	uint32	nr_subchan;
};

struct nouveau_channel_free {
	int32	channel;
};

struct nouveau_grobj_alloc {
	int32	channel;
	uint32	handle;
	int32	oclass;
};

struct nouveau_gem_info {
	uint32	handle;
	uint32	domain;
	uint64	size;
	uint64	offset;
	uint64	map_handle;
	uint32	tile_mode;
	uint32	tile_flags;
};

struct nouveau_gem_new {
	nouveau_gem_info info;
	uint32	channel_hint;
	uint32	align;
};

struct nouveau_gem_pushbuf_bo {
	uint64	user_priv;
	uint32	handle;
	uint32	read_domains;
	uint32	write_domains;
	uint32	valid_domains;
	struct {
		uint32	valid;
		uint32	domain;
		uint64	offset;
	} presumed;
};

struct nouveau_gem_pushbuf_push {
	uint32	bo_index;
	uint32	pad;
	uint64	offset;
	uint64	length;
};

struct nouveau_gem_pushbuf {
	uint32	channel;
	uint32	nr_buffers;
	uint64	buffers;
	uint32	nr_relocs;
	uint32	nr_push;
	uint64	relocs;
	uint64	push;
	uint32	suffix0;
	uint32	suffix1;
	uint64	vram_available;
	uint64	gart_available;
};

struct nouveau_gem_cpu_prep {
	uint32	handle;
	uint32	flags;
};

}	// namespace


#define NOUVEAU_IOWR(nr, type)	DRM_IOWR(DRM_COMMAND_BASE + (nr), type)
#define NOUVEAU_IOW(nr, type)	DRM_IOW(DRM_COMMAND_BASE + (nr), type)

static const unsigned long kGetParam
	= NOUVEAU_IOWR(0x00, nouveau_getparam);
static const unsigned long kChannelAlloc
	= NOUVEAU_IOWR(0x02, nouveau_channel_alloc);
static const unsigned long kChannelFree
	= NOUVEAU_IOW(0x03, nouveau_channel_free);
static const unsigned long kGrobjAlloc
	= NOUVEAU_IOW(0x04, nouveau_grobj_alloc);
static const unsigned long kGemNew = NOUVEAU_IOWR(0x40, nouveau_gem_new);
static const unsigned long kGemPushbuf
	= NOUVEAU_IOWR(0x41, nouveau_gem_pushbuf);
static const unsigned long kGemCpuPrep
	= NOUVEAU_IOW(0x42, nouveau_gem_cpu_prep);
static const unsigned long kGemInfo = NOUVEAU_IOWR(0x44, nouveau_gem_info);

static const uint64 kParamChipsetId = 11;

static const uint32 kDomainVram = 1 << 1;
static const uint32 kDomainGart = 1 << 2;
static const uint32 kCpuPrepWrite = 0x4;


// #pragma mark - the 2D engine (Mesa's nv50_2d.xml.h)


static const uint32 kSubchannel = 3;
static const uint32 kObjectHandle = 0xbeef002d;
static const uint32 kVramCtxDma = 0xbeef0201;	// Tesla's DMA objects
static const uint32 kGartCtxDma = 0xbeef0202;

static const uint32 kClassNv50 = 0x502d;
static const uint32 kClassFermi = 0x902d;

static const uint32 kMethodObject = 0x0000;
static const uint32 kMethodDmaDst = 0x0184;			// DST, SRC, COND
static const uint32 kMethodDstFormat = 0x0200;		// FORMAT, LINEAR
static const uint32 kMethodDstPitch = 0x0214;		// PITCH, WIDTH, HEIGHT,
													// ADDRESS_HIGH, _LOW
static const uint32 kMethodSrcFormat = 0x0230;		// FORMAT, LINEAR
static const uint32 kMethodSrcPitch = 0x0244;		// as DST's
static const uint32 kMethodSingleGpc = 0x0260;		// Fermi on
static const uint32 kMethodCondMode = 0x026c;
static const uint32 kMethodClipEnable = 0x0290;
static const uint32 kMethodColorKeyEnable = 0x029c;
static const uint32 kMethodOperation = 0x02ac;
static const uint32 kMethodCorralSize = 0x0884;		// Fermi on
static const uint32 kMethodSafeOverlap = 0x0888;
static const uint32 kMethodBlitControl = 0x088c;
static const uint32 kMethodBlitDstX = 0x08b0;		// X, Y, W, H
static const uint32 kMethodBlitDuDxFract = 0x08c0;	// DU_DX F/I, DV_DY F/I
static const uint32 kMethodBlitSrcXFract = 0x08d0;	// X F/I, Y F/I: starts

static const uint32 kFormatBgra8 = 0xcf;
static const uint32 kOperationSrcCopy = 3;
static const uint32 kCondModeAlways = 1;

static const uint32 kPushBufferSize = 16 * 1024;
static const uint32 kPushBuffers = 4;
static const uint32 kPerRectDwords = 10;
static const uint32 kSetupDwords = 64;
static const bigtime_t kSlowCopy = 100000;


struct PushBuffer {
	uint32		handle;
	uint32*		map;
	bool		submitted;	// since last waited for
};


struct NouveauCopier {
	int			fd;
	bool		fermi;
	int32		channel;
	PushBuffer	buffers[kPushBuffers];
	uint32		current;
	uint32*		push;		// the current buffer's
	uint32		pushDwords;
	uint32		pushCapacity;
	uint32		targetHandle;	// the frame buffer the address is of
	uint64		targetVa;
	bool		failed;
};


static uint32
method_header(const NouveauCopier& copier, uint32 method, uint32 count)
{
	if (copier.fermi) {
		return 0x20000000 | (count << 16) | (kSubchannel << 13)
			| (method >> 2);
	}
	return (count << 18) | (kSubchannel << 13) | method;
}


static void
begin(NouveauCopier& copier, uint32 method, uint32 count)
{
	copier.push[copier.pushDwords++] = method_header(copier, method, count);
}


static void
data(NouveauCopier& copier, uint32 value)
{
	copier.push[copier.pushDwords++] = value;
}


static status_t
gem_close(int fd, uint32 handle)
{
	drm_gem_close request = {};
	request.handle = handle;
	return Kms::Ioctl(fd, DRM_IOCTL_GEM_CLOSE, &request);
}


/*!	A channel with the 2D object and a push buffer on \a card, or NULL when
	it is not a nouveau card from Tesla on, or something failed. */
NouveauCopier*
nouveau_copy_init(int fd)
{
	nouveau_getparam param = {};
	param.param = kParamChipsetId;
	if (Kms::Ioctl(fd, kGetParam, &param) != B_OK)
		return NULL;
	uint32 chipset = (uint32)param.value;
	if (chipset < 0x50) {
		TRACE("nouveau chipset %#" B_PRIx32 ": no copies by the GPU\n",
			chipset);
		return NULL;
	}
	bool fermi = chipset >= 0xc0;

	NouveauCopier* copier = (NouveauCopier*)calloc(1, sizeof(NouveauCopier));
	if (copier == NULL)
		return NULL;
	copier->fd = fd;
	copier->fermi = fermi;
	copier->channel = -1;

	nouveau_channel_alloc channel = {};
	if (fermi) {
		channel.fb_ctxdma_handle = 0;
		channel.tt_ctxdma_handle = 0;
	} else {
		channel.fb_ctxdma_handle = kVramCtxDma;
		channel.tt_ctxdma_handle = kGartCtxDma;
	}
	status_t status = Kms::Ioctl(fd, kChannelAlloc, &channel);
	if (status != B_OK) {
		TRACE("nouveau: no channel for copies: %s\n", strerror(status));
		free(copier);
		return NULL;
	}
	copier->channel = channel.channel;

	nouveau_grobj_alloc object = {};
	object.channel = copier->channel;
	object.handle = kObjectHandle;
	object.oclass = fermi ? kClassFermi : kClassNv50;
	status = Kms::Ioctl(fd, kGrobjAlloc, &object);

	for (uint32 i = 0; status == B_OK && i < kPushBuffers; i++) {
		nouveau_gem_new create = {};
		create.info.size = kPushBufferSize;
		create.info.domain = kDomainGart;
		create.align = B_PAGE_SIZE;
		status = Kms::Ioctl(fd, kGemNew, &create);
		if (status != B_OK)
			break;
		copier->buffers[i].handle = create.info.handle;

		drm_haiku_map map = {};
		map.offset = create.info.map_handle;
		map.size = kPushBufferSize;
		map.flags = DRM_HAIKU_MAP_WRITE;
		status = Kms::Ioctl(fd, DRM_HAIKU_IOCTL_MAP, &map);
		if (status == B_OK)
			copier->buffers[i].map = (uint32*)(addr_t)map.address;
	}
	if (status != B_OK) {
		TRACE("nouveau: copies by the 2D engine fail to start: %s\n",
			strerror(status));
		nouveau_copy_uninit(copier);
		return NULL;
	}
	copier->pushCapacity = kPushBufferSize / 4;
	TRACE("nouveau chipset %#" B_PRIx32 ": copies by the 2D engine (%s),"
		" channel %" B_PRId32 "\n", chipset, fermi ? "FERMI_TWOD" : "NV50_2D",
		copier->channel);
	return copier;
}


void
nouveau_copy_uninit(NouveauCopier* copier)
{
	if (copier == NULL)
		return;
	for (uint32 i = 0; i < kPushBuffers; i++) {
		PushBuffer& buffer = copier->buffers[i];
		if (buffer.map != NULL) {
			nouveau_gem_cpu_prep prep = {};
			prep.handle = buffer.handle;
			prep.flags = kCpuPrepWrite;
			Kms::Ioctl(copier->fd, kGemCpuPrep, &prep);
			drm_haiku_unmap unmap = {};
			unmap.address = (addr_t)buffer.map;
			Kms::Ioctl(copier->fd, DRM_HAIKU_IOCTL_UNMAP, &unmap);
		}
		if (buffer.handle != 0)
			gem_close(copier->fd, buffer.handle);
	}
	if (copier->channel >= 0) {
		nouveau_channel_free request = {};
		request.channel = copier->channel;
		Kms::Ioctl(copier->fd, kChannelFree, &request);
	}
	free(copier);
}


/*!	A buffer object on the copier's card of the host memory at \a address
	(page aligned, \a size bytes), which stays wired until it is released. */
status_t
nouveau_copy_import(NouveauCopier* copier, void* address, size_t size,
	uint32& _handle, uint64& _va)
{
	drm_haiku_host_bo request = {};
	request.address = (addr_t)address;
	request.size = size;
	status_t status = Kms::Ioctl(copier->fd, DRM_HAIKU_IOCTL_HOST_BO,
		&request);
	if (status != B_OK)
		return status;
	if (request.offset == 0) {
		gem_close(copier->fd, request.handle);
		return B_ERROR;
	}
	_handle = request.handle;
	_va = request.offset;
	return B_OK;
}


/*!	Lets the source's buffer object go: before the memory itself goes, or
	freeing the memory would wait for it. */
void
nouveau_copy_release(NouveauCopier* copier, uint32 handle)
{
	if (copier != NULL && handle != 0)
		gem_close(copier->fd, handle);
}


static status_t
target_address(NouveauCopier& copier, const Kms::Buffer& target)
{
	if (copier.targetHandle == target.handle && copier.targetVa != 0)
		return B_OK;
	nouveau_gem_info info = {};
	info.handle = target.handle;
	status_t status = Kms::Ioctl(copier.fd, kGemInfo, &info);
	if (status != B_OK)
		return status;
	if (info.offset == 0)
		return B_ERROR;
	copier.targetHandle = target.handle;
	copier.targetVa = info.offset;
	return B_OK;
}


static void
setup(NouveauCopier& copier, uint64 sourceVa, uint32 sourcePitch,
	uint32 sourceWidth, uint32 sourceHeight, const Kms::Buffer& target)
{
	begin(copier, kMethodObject, 1);
	data(copier, copier.fermi ? kClassFermi : kObjectHandle);
	if (copier.fermi) {
		begin(copier, kMethodSingleGpc, 1);
		data(copier, 0);
	} else {
		begin(copier, kMethodDmaDst, 3);
		data(copier, kVramCtxDma);
		data(copier, kVramCtxDma);
		data(copier, kVramCtxDma);
	}
	begin(copier, kMethodOperation, 1);
	data(copier, kOperationSrcCopy);
	begin(copier, kMethodClipEnable, 1);
	data(copier, 0);
	begin(copier, kMethodColorKeyEnable, 1);
	data(copier, 0);
	if (copier.fermi) {
		begin(copier, kMethodCorralSize, 1);
		data(copier, 0x3f);
	}
	begin(copier, kMethodSafeOverlap, 1);
	data(copier, 1);
	begin(copier, kMethodCondMode, 1);
	data(copier, kCondModeAlways);

	begin(copier, kMethodDstFormat, 2);
	data(copier, kFormatBgra8);
	data(copier, 1);
	begin(copier, kMethodDstPitch, 5);
	data(copier, target.pitch);
	data(copier, target.width);
	data(copier, target.height);
	data(copier, (uint32)(copier.targetVa >> 32));
	data(copier, (uint32)copier.targetVa);

	begin(copier, kMethodSrcFormat, 2);
	data(copier, kFormatBgra8);
	data(copier, 1);
	begin(copier, kMethodSrcPitch, 5);
	data(copier, sourcePitch);
	data(copier, sourceWidth);
	data(copier, sourceHeight);
	data(copier, (uint32)(sourceVa >> 32));
	data(copier, (uint32)sourceVa);

	begin(copier, kMethodBlitControl, 1);
	data(copier, 0);
	begin(copier, kMethodBlitDuDxFract, 4);
	data(copier, 0);
	data(copier, 1);
	data(copier, 0);
	data(copier, 1);
}


/*!	Makes the next push buffer of the ring the current one, waiting until
	the GPU is done with what was submitted from it last. */
static status_t
next_buffer(NouveauCopier& copier)
{
	copier.current = (copier.current + 1) % kPushBuffers;
	PushBuffer& buffer = copier.buffers[copier.current];
	copier.push = buffer.map;
	copier.pushDwords = 0;
	if (!buffer.submitted)
		return B_OK;

	bigtime_t start = system_time();
	nouveau_gem_cpu_prep prep = {};
	prep.handle = buffer.handle;
	prep.flags = kCpuPrepWrite;
	status_t status = Kms::Ioctl(copier.fd, kGemCpuPrep, &prep);
	bigtime_t took = system_time() - start;
	if (status != B_OK || took > kSlowCopy) {
		TRACE("nouveau: copies on channel %" B_PRId32 ": %s after %" B_PRId64
			" µs\n", copier.channel, strerror(status), took);
	}
	if (status == B_OK)
		buffer.submitted = false;
	return status;
}


static status_t
submit(NouveauCopier& copier, uint32 sourceHandle, const Kms::Buffer& target)
{
	PushBuffer& pushBuffer = copier.buffers[copier.current];
	nouveau_gem_pushbuf_bo buffers[3] = {};
	buffers[0].handle = pushBuffer.handle;
	buffers[0].valid_domains = kDomainGart | kDomainVram;
	buffers[0].read_domains = kDomainGart;
	buffers[1].handle = sourceHandle;
	buffers[1].valid_domains = kDomainGart;
	buffers[1].read_domains = kDomainGart;
	buffers[2].handle = target.handle;
	buffers[2].valid_domains = kDomainGart | kDomainVram;
	buffers[2].write_domains = kDomainVram;

	nouveau_gem_pushbuf_push push = {};
	push.bo_index = 0;
	push.offset = 0;
	push.length = copier.pushDwords * 4;

	nouveau_gem_pushbuf request = {};
	request.channel = copier.channel;
	request.nr_buffers = 3;
	request.buffers = (addr_t)buffers;
	request.nr_push = 1;
	request.push = (addr_t)&push;
	status_t status = Kms::Ioctl(copier.fd, kGemPushbuf, &request);
	copier.pushDwords = 0;
	if (status == B_OK)
		pushBuffer.submitted = true;
	return status;
}


/*!	Copies the rectangles (exclusive right and bottom, already clipped to
	both buffers) from the source's buffer object into \a target, at the
	same places, and waits until they are done. */
status_t
nouveau_copy_rects(NouveauCopier* copier, uint32 sourceHandle,
	uint64 sourceVa, uint32 sourcePitch, uint32 sourceWidth,
	uint32 sourceHeight, const Kms::Buffer& target, const copy_rect* rects,
	uint32 count)
{
	if (copier == NULL || copier->failed)
		return B_NOT_SUPPORTED;
	status_t status = target_address(*copier, target);
	if (status != B_OK)
		return status;

	uint32 done = 0;
	while (done < count) {
		status = next_buffer(*copier);
		if (status != B_OK)
			break;
		setup(*copier, sourceVa, sourcePitch, sourceWidth, sourceHeight,
			target);
		uint32 room = (copier->pushCapacity - kSetupDwords) / kPerRectDwords;
		uint32 batch = min_c(count - done, room);
		for (uint32 i = 0; i < batch; i++) {
			const copy_rect& rect = rects[done + i];
			begin(*copier, kMethodBlitDstX, 4);
			data(*copier, rect.left);
			data(*copier, rect.top);
			data(*copier, rect.right - rect.left);
			data(*copier, rect.bottom - rect.top);
			begin(*copier, kMethodBlitSrcXFract, 4);
			data(*copier, 0);
			data(*copier, rect.left);
			data(*copier, 0);
			data(*copier, rect.top);
		}
		status = submit(*copier, sourceHandle, target);
		if (status != B_OK)
			break;
		done += batch;
	}
	if (status != B_OK) {
		// a channel that went wrong once is not trusted again: the CPU
		// copies from now on
		copier->failed = true;
		TRACE("nouveau: copies by the 2D engine stopped: %s\n",
			strerror(status));
	}
	return status;
}
