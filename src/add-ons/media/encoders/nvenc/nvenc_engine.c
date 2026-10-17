/* See nvenc_engine.h. */

#include "nvenc_engine.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nvenc_api.h>

#include "nvRmApi.h"
#include "nvos.h"
#include "nvstatus.h"
#include "class/cl0000.h"
#include "class/cl0002.h"
#include "class/cl003e.h"
#include "class/cl0040.h"
#include "class/cl0080.h"
#include "class/cl2080.h"
#include "class/cl2080_notification.h"
#include "class/cl50a0.h"
#include "class/cl90f1.h"
#include "class/clb06f.h"
#include "class/clc06f.h"
#include "class/clc361.h"
#include "class/clc36f.h"
#include "class/clc461.h"
#include "class/clc46f.h"
#include "class/clc56f.h"
#include "class/clc86f.h"
#include "class/clc96f.h"
#include "class/clca6f.h"
#include "ctrl/ctrl0000/ctrl0000gpu.h"
#include "ctrl/ctrl0080/ctrl0080gpu.h"
#include "ctrl/ctrl2080/ctrl2080gpu.h"
#include "ctrl/ctrl2080/ctrl2080mc.h"
#include "ctrl/ctrla06f/ctrla06fgpfifo.h"
#include "ctrl/ctrlc36f.h"

#define USERD_GPGET		(0x88 / 4)
#define USERD_GPPUT		(0x8c / 4)
#define USERD_SIZE		0x1000
#define GPFIFO_ENTRIES		0x100
#define PUSHBUFFER_SIZE		0x10000

typedef struct {
	uint32_t	physical;
	uint32_t	virt;
	NvRmApiMapping	mapping;
} BufferState;

struct NvencEngine {
	NvRmApi		rm;
	int		deviceFd;
	NvencCard	card;
	uint32_t	client, device, subdevice, vaSpace;
	uint32_t	errorNotifier, channel, encoder, usermode;
	NvRmApiMapping	userdMapping, usermodeMapping;
	volatile uint32_t *userd;
	volatile uint32_t *doorbell;
	uint32_t	workSubmitToken;
	NvencBuffer	notifier, gpFifo, pushBuffer, semaphore, clientUserd;
	uint32_t	*pushAt;
	uint32_t	gpPut;
	uint32_t	semaphoreValue;
	char		*reason;
	size_t		reasonSize;
};

/* The encoder classes, newest first, and what each generation can encode.
 * Maxwell's first NVENC (GM107) only knows H.264; the second (GM20x) adds
 * 8-bit HEVC; Pascal 10-bit HEVC; Ada AV1. */
static const struct {
	uint32_t	encoderClass;
	uint32_t	codecs;
} kEncoderClasses[] = {
	{ 0xcfb7, NVENC_CODEC_H264 | NVENC_CODEC_HEVC | NVENC_CODEC_HEVC_10
		| NVENC_CODEC_AV1 | NVENC_CODEC_AV1_10 },	/* Blackwell */
	{ 0xc9b7, NVENC_CODEC_H264 | NVENC_CODEC_HEVC | NVENC_CODEC_HEVC_10
		| NVENC_CODEC_AV1 | NVENC_CODEC_AV1_10 },	/* Ada */
	{ 0xc7b7, NVENC_CODEC_H264 | NVENC_CODEC_HEVC | NVENC_CODEC_HEVC_10 },	/* Ampere */
	{ 0xc5b7, NVENC_CODEC_H264 | NVENC_CODEC_HEVC | NVENC_CODEC_HEVC_10 },
	{ 0xc4b7, NVENC_CODEC_H264 | NVENC_CODEC_HEVC | NVENC_CODEC_HEVC_10 },	/* Turing */
	{ 0xc3b7, NVENC_CODEC_H264 | NVENC_CODEC_HEVC | NVENC_CODEC_HEVC_10 },	/* Volta */
	{ 0xc2b7, NVENC_CODEC_H264 | NVENC_CODEC_HEVC | NVENC_CODEC_HEVC_10 },	/* Pascal */
	{ 0xc1b7, NVENC_CODEC_H264 | NVENC_CODEC_HEVC | NVENC_CODEC_HEVC_10 },	/* GP100 */
	{ 0xd0b7, NVENC_CODEC_H264 | NVENC_CODEC_HEVC },	/* Maxwell 2 */
	{ 0xc0b7, NVENC_CODEC_H264 },	/* Maxwell */
};

static const uint32_t kChannelClasses[] = {
	BLACKWELL_CHANNEL_GPFIFO_B,
	BLACKWELL_CHANNEL_GPFIFO_A,
	HOPPER_CHANNEL_GPFIFO_A,
	AMPERE_CHANNEL_GPFIFO_A,
	TURING_CHANNEL_GPFIFO_A,
	VOLTA_CHANNEL_GPFIFO_A,
	PASCAL_CHANNEL_GPFIFO_A,
	MAXWELL_CHANNEL_GPFIFO_A,
};

/* Hopper's usermode class wants parameters NVK does not give it either;
 * the Turing one is there on everything since. */
static const uint32_t kUsermodeClasses[] = {
	TURING_USERMODE_A,
	VOLTA_USERMODE_A,
};

static bool
fail(NvencEngine *engine, const char *what, uint32_t status)
{
	if (engine->reason != NULL && engine->reasonSize > 0)
		snprintf(engine->reason, engine->reasonSize, "%s failed: %#x", what, status);
	return false;
}

static bool
has_class(const uint32_t *list, uint32_t count, uint32_t wanted)
{
	for (uint32_t i = 0; i < count; i++) {
		if (list[i] == wanted)
			return true;
	}
	return false;
}

bool
nvencAlloc(NvencEngine *engine, NvencBuffer *buffer, uint64_t size, bool inSystemMemory)
{
	memset(buffer, 0, sizeof(*buffer));
	size = (size + 0xfff) & ~0xfffull;
	buffer->size = size;
	buffer->inSystemMemory = inSystemMemory;

	BufferState *state = calloc(1, sizeof(BufferState));
	if (state == NULL)
		return fail(engine, "buffer bookkeeping", 0);
	buffer->driverState = state;

	NV_MEMORY_ALLOCATION_PARAMS physical = {
		.owner = engine->client,
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_ALIGNMENT_FORCE,
		.attr = DRF_DEF(OS32, _ATTR, _PAGE_SIZE, _4KB),
		.size = size,
		.alignment = 0x1000,
	};
	if (inSystemMemory) {
		physical.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _PCI);
		physical.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _CACHED);
	} else {
		physical.attr |= DRF_DEF(OS32, _ATTR, _LOCATION, _VIDMEM);
		physical.attr |= DRF_DEF(OS32, _ATTR, _COHERENCY, _UNCACHED);
		physical.flags |= NVOS32_ALLOC_FLAGS_PERSISTENT_VIDMEM;
	}
	uint32_t status = nvRmApiAlloc(&engine->rm, engine->device, &state->physical,
		inSystemMemory ? NV01_MEMORY_SYSTEM : NV01_MEMORY_LOCAL_USER, &physical);
	if (status != NV_OK)
		return fail(engine, "memory", status);

	/* Let resman place the range: NVDEC ran out of page tables when it chose
	 * addresses itself. */
	NV_MEMORY_ALLOCATION_PARAMS virt = {
		.owner = engine->client,
		.type = NVOS32_TYPE_IMAGE,
		.flags = NVOS32_ALLOC_FLAGS_VIRTUAL,
		.size = size,
		.alignment = 0x10000,
		.hVASpace = engine->vaSpace,
	};
	status = nvRmApiAlloc(&engine->rm, engine->device, &state->virt,
		NV50_MEMORY_VIRTUAL, &virt);
	if (status != NV_OK)
		return fail(engine, "address range", status);

	uint32_t mapFlags = DRF_DEF(OS46, _FLAGS, _PAGE_KIND, _VIRTUAL)
		| (inSystemMemory ? DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _ENABLE)
				  : DRF_DEF(OS46, _FLAGS, _CACHE_SNOOP, _DISABLE));
	NvU64 dmaOffset = 0;
	status = nvRmApiMapMemoryDma(&engine->rm, engine->device, state->virt,
		state->physical, 0, size, mapFlags, &dmaOffset);
	if (status != NV_OK)
		return fail(engine, "mapping for the card", status);
	buffer->gpuAddress = dmaOffset;

	status = nvRmApiMapMemory(&engine->rm, engine->device, state->physical, 0, size,
		inSystemMemory, 0, &state->mapping);
	if (status != NV_OK)
		return fail(engine, "mapping for us", status);
	buffer->data = state->mapping.address;
	memset(buffer->data, 0, size);
	return true;
}

void
nvencFree(NvencEngine *engine, NvencBuffer *buffer)
{
	BufferState *state = buffer->driverState;
	if (state == NULL)
		return;
	if (buffer->data != NULL)
		nvRmApiUnmapMemory(&engine->rm, engine->device, state->physical, 0, &state->mapping);
	if (state->virt != 0) {
		nvRmApiUnmapMemoryDma(&engine->rm, engine->device, state->virt,
			state->physical, 0, buffer->gpuAddress);
		nvRmApiFree(&engine->rm, state->virt);
	}
	if (state->physical != 0)
		nvRmApiFree(&engine->rm, state->physical);
	free(state);
	memset(buffer, 0, sizeof(*buffer));
}

/* Client, device and subdevice, and what the card is. */
static bool
open_device(NvencEngine *engine)
{
	/* Every request goes through the control node; memory on the card is
	 * mapped through the card's own node, which is what nodeName is for. */
	engine->rm.fd = open("/dev/nvidiactl", O_RDWR);
	if (engine->rm.fd < 0)
		return fail(engine, "opening /dev/nvidiactl", 0);
	engine->rm.nodeName = "/dev/graphics/nvidia0";
	engine->deviceFd = open(engine->rm.nodeName, O_RDWR);
	if (engine->deviceFd < 0)
		return fail(engine, "opening /dev/graphics/nvidia0", 0);

	nv_ioctl_card_info_t cards[8];
	memset(cards, 0, sizeof(cards));
	uint32_t status = nvRmApiCardInfo(&engine->rm, cards, sizeof(cards));
	if (status != NV_OK)
		return fail(engine, "asking about the card", status);
	status = nvRmApiAlloc(&engine->rm, 0, &engine->client, NV01_ROOT_CLIENT, NULL);
	if (status != NV_OK)
		return fail(engine, "client", status);
	engine->rm.hClient = engine->client;

	NV0000_CTRL_GPU_GET_ID_INFO_V2_PARAMS id = { .gpuId = cards[0].gpu_id };
	status = nvRmApiControl(&engine->rm, engine->client,
		NV0000_CTRL_CMD_GPU_GET_ID_INFO_V2, &id, sizeof(id));
	if (status != NV_OK)
		return fail(engine, "identifying the card", status);
	NV0080_ALLOC_PARAMETERS deviceParams = {
		.deviceId = id.deviceInstance,
		.hClientShare = engine->client,
	};
	status = nvRmApiAlloc(&engine->rm, engine->client, &engine->device,
		NV01_DEVICE_0, &deviceParams);
	if (status != NV_OK)
		return fail(engine, "device", status);
	NV2080_ALLOC_PARAMETERS subdeviceParams = { .subDeviceId = id.subDeviceInstance };
	status = nvRmApiAlloc(&engine->rm, engine->device, &engine->subdevice,
		NV20_SUBDEVICE_0, &subdeviceParams);
	if (status != NV_OK)
		return fail(engine, "subdevice", status);

	NvencCard *card = &engine->card;
	NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS name = {
		.gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII,
	};
	if (nvRmApiControl(&engine->rm, engine->subdevice,
			NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &name, sizeof(name)) == NV_OK) {
		snprintf(card->name, sizeof(card->name), "%s",
			(const char *)name.gpuNameString.ascii);
	}
	if (card->name[0] == '\0')
		snprintf(card->name, sizeof(card->name), "NVIDIA GPU");

	NV2080_CTRL_MC_GET_ARCH_INFO_PARAMS arch = { 0 };
	if (nvRmApiControl(&engine->rm, engine->subdevice,
			NV2080_CTRL_CMD_MC_GET_ARCH_INFO, &arch, sizeof(arch)) == NV_OK) {
		card->architecture = arch.architecture;
		card->implementation = arch.implementation;
	}

	NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS *classes
		= calloc(1, sizeof(NV0080_CTRL_GPU_GET_CLASSLIST_V2_PARAMS));
	if (classes == NULL)
		return fail(engine, "class list bookkeeping", 0);
	status = nvRmApiControl(&engine->rm, engine->device,
		NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2, classes, sizeof(*classes));
	if (status != NV_OK) {
		free(classes);
		return fail(engine, "listing the card's classes", status);
	}
	for (size_t i = 0; i < sizeof(kEncoderClasses) / sizeof(kEncoderClasses[0]); i++) {
		if (has_class(classes->classList, classes->numClasses,
				kEncoderClasses[i].encoderClass)) {
			card->encoderClass = kEncoderClasses[i].encoderClass;
			card->hardwareCodecs = kEncoderClasses[i].codecs;
			break;
		}
	}
	for (size_t i = 0; i < sizeof(kChannelClasses) / sizeof(kChannelClasses[0]); i++) {
		if (has_class(classes->classList, classes->numClasses, kChannelClasses[i])) {
			card->channelClass = kChannelClasses[i];
			break;
		}
	}
	if (card->channelClass >= VOLTA_CHANNEL_GPFIFO_A) {
		for (size_t i = 0; i < sizeof(kUsermodeClasses) / sizeof(kUsermodeClasses[0]); i++) {
			if (has_class(classes->classList, classes->numClasses,
					kUsermodeClasses[i])) {
				card->usermodeClass = kUsermodeClasses[i];
				break;
			}
		}
	}
	free(classes);

	if (card->encoderClass == 0) {
		if (engine->reason != NULL)
			snprintf(engine->reason, engine->reasonSize, "The %s has no video encoder.",
				card->name);
		return false;
	}
	if (card->channelClass == 0
		|| (card->channelClass >= VOLTA_CHANNEL_GPFIFO_A && card->usermodeClass == 0)) {
		if (engine->reason != NULL)
			snprintf(engine->reason, engine->reasonSize,
				"The %s has no channel kind this add-on knows.", card->name);
		return false;
	}
	return true;
}

static void
close_device(NvencEngine *engine)
{
	if (engine->subdevice != 0)
		nvRmApiFree(&engine->rm, engine->subdevice);
	if (engine->device != 0)
		nvRmApiFree(&engine->rm, engine->device);
	if (engine->client != 0)
		nvRmApiFree(&engine->rm, engine->client);
	if (engine->deviceFd >= 0)
		close(engine->deviceFd);
	if (engine->rm.fd >= 0)
		close(engine->rm.fd);
}

bool
nvencIdentify(NvencCard *card, char *reason, size_t reasonSize)
{
	NvencEngine engine;
	memset(&engine, 0, sizeof(engine));
	engine.rm.fd = -1;
	engine.deviceFd = -1;
	engine.reason = reason;
	engine.reasonSize = reasonSize;
	bool ok = open_device(&engine);
	*card = engine.card;
	close_device(&engine);
	return ok;
}

NvencEngine *
nvencOpen(char *reason, size_t reasonSize)
{
	NvencEngine *engine = calloc(1, sizeof(NvencEngine));
	if (engine == NULL)
		return NULL;
	engine->reason = reason;
	engine->reasonSize = reasonSize;
	engine->rm.fd = -1;
	engine->deviceFd = -1;
	if (!open_device(engine))
		goto error;

	const NvencCard *card = &engine->card;
	bool volta = card->channelClass >= VOLTA_CHANNEL_GPFIFO_A;

	NV_VASPACE_ALLOCATION_PARAMETERS vaParams = {
		.flags = NV_VASPACE_ALLOCATION_FLAGS_RETRY_PTE_ALLOC_IN_SYS,
	};
	uint32_t status = nvRmApiAlloc(&engine->rm, engine->device, &engine->vaSpace,
		FERMI_VASPACE_A, &vaParams);
	if (status != NV_OK) {
		fail(engine, "address space", status);
		goto error;
	}

	if (!nvencAlloc(engine, &engine->notifier, 0x1000, true)
		|| !nvencAlloc(engine, &engine->gpFifo, GPFIFO_ENTRIES * 8, true)
		|| !nvencAlloc(engine, &engine->pushBuffer, PUSHBUFFER_SIZE, true)
		|| !nvencAlloc(engine, &engine->semaphore, 0x1000, true)
		|| (volta && !nvencAlloc(engine, &engine->clientUserd, USERD_SIZE, false))) {
		goto error;
	}

	NV_CONTEXT_DMA_ALLOCATION_PARAMS notifierParams = {
		.flags = DRF_DEF(OS03, _FLAGS, _MAPPING, _KERNEL)
			| DRF_DEF(OS03, _FLAGS, _HASH_TABLE, _DISABLE),
		.hMemory = ((BufferState *)engine->notifier.driverState)->physical,
		.offset = 0,
		.limit = engine->notifier.size - 1,
	};
	status = nvRmApiAlloc(&engine->rm, engine->device, &engine->errorNotifier,
		NV01_CONTEXT_DMA, &notifierParams);
	if (status != NV_OK) {
		fail(engine, "error notifier", status);
		goto error;
	}

	NV_CHANNEL_ALLOC_PARAMS channelParams = {
		.hObjectError = engine->errorNotifier,
		.gpFifoOffset = engine->gpFifo.gpuAddress,
		.gpFifoEntries = GPFIFO_ENTRIES,
		.hVASpace = engine->vaSpace,
		.engineType = NV2080_ENGINE_TYPE_NVENC0,
	};
	if (volta) {
		channelParams.hUserdMemory[0]
			= ((BufferState *)engine->clientUserd.driverState)->physical;
		channelParams.userdOffset[0] = 0;
	}
	status = nvRmApiAlloc(&engine->rm, engine->device, &engine->channel,
		card->channelClass, &channelParams);
	if (status != NV_OK) {
		fail(engine, "channel on the encoder", status);
		goto error;
	}
	if (volta) {
		engine->userd = engine->clientUserd.data;
		status = nvRmApiAlloc(&engine->rm, engine->subdevice, &engine->usermode,
			card->usermodeClass, NULL);
		if (status != NV_OK) {
			fail(engine, "usermode doorbell", status);
			goto error;
		}
		status = nvRmApiMapMemory(&engine->rm, engine->subdevice, engine->usermode, 0,
			0x1000, false, 0, &engine->usermodeMapping);
		if (status != NV_OK) {
			fail(engine, "mapping the doorbell", status);
			goto error;
		}
		engine->doorbell = (volatile uint32_t *)((uint8_t *)engine->usermodeMapping.address
			+ NVC361_NOTIFY_CHANNEL_PENDING);
	} else {
		/* Before Volta resman owns USERD, reached by mapping the channel. */
		status = nvRmApiMapMemory(&engine->rm, engine->subdevice, engine->channel, 0,
			0x1000, false, DRF_DEF(OS33, _FLAGS, _FIFO_MAPPING, _ENABLE),
			&engine->userdMapping);
		if (status != NV_OK) {
			fail(engine, "the channel's control page", status);
			goto error;
		}
		engine->userd = engine->userdMapping.address;
	}

	status = nvRmApiAlloc(&engine->rm, engine->channel, &engine->encoder,
		card->encoderClass, NULL);
	if (status != NV_OK) {
		fail(engine, "encoder object", status);
		goto error;
	}
	NVA06F_CTRL_BIND_PARAMS bind = { .engineType = NV2080_ENGINE_TYPE_NVENC0 };
	status = nvRmApiControl(&engine->rm, engine->channel, NVA06F_CTRL_CMD_BIND,
		&bind, sizeof(bind));
	if (status != NV_OK) {
		fail(engine, "binding the channel", status);
		goto error;
	}
	NVA06F_CTRL_GPFIFO_SCHEDULE_PARAMS schedule = { .bEnable = NV_TRUE };
	status = nvRmApiControl(&engine->rm, engine->channel,
		NVA06F_CTRL_CMD_GPFIFO_SCHEDULE, &schedule, sizeof(schedule));
	if (status != NV_OK) {
		fail(engine, "scheduling the channel", status);
		goto error;
	}
	if (volta) {
		NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_PARAMS token = { 0 };
		status = nvRmApiControl(&engine->rm, engine->channel,
			NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN, &token, sizeof(token));
		if (status != NV_OK) {
			fail(engine, "the channel's doorbell token", status);
			goto error;
		}
		engine->workSubmitToken = token.workSubmitToken;
	}
	return engine;

error:
	nvencClose(engine);
	return NULL;
}

void
nvencClose(NvencEngine *engine)
{
	if (engine == NULL)
		return;
	if (engine->encoder != 0)
		nvRmApiFree(&engine->rm, engine->encoder);
	if (engine->usermodeMapping.address != NULL) {
		nvRmApiUnmapMemory(&engine->rm, engine->subdevice, engine->usermode, 0,
			&engine->usermodeMapping);
	}
	if (engine->usermode != 0)
		nvRmApiFree(&engine->rm, engine->usermode);
	if (engine->userdMapping.address != NULL) {
		nvRmApiUnmapMemory(&engine->rm, engine->subdevice, engine->channel, 0,
			&engine->userdMapping);
	}
	if (engine->channel != 0)
		nvRmApiFree(&engine->rm, engine->channel);
	if (engine->errorNotifier != 0)
		nvRmApiFree(&engine->rm, engine->errorNotifier);
	nvencFree(engine, &engine->clientUserd);
	nvencFree(engine, &engine->semaphore);
	nvencFree(engine, &engine->pushBuffer);
	nvencFree(engine, &engine->gpFifo);
	nvencFree(engine, &engine->notifier);
	if (engine->vaSpace != 0)
		nvRmApiFree(&engine->rm, engine->vaSpace);
	close_device(engine);
	free(engine);
}

const NvencCard *
nvencCard(const NvencEngine *engine)
{
	return &engine->card;
}

void
nvencBegin(NvencEngine *engine)
{
	engine->pushAt = engine->pushBuffer.data;
	nvencMethod(engine, NVENC_SET_OBJECT, engine->card.encoderClass);
}

void
nvencMethod(NvencEngine *engine, uint32_t method, uint32_t value)
{
	uint32_t *end = (uint32_t *)engine->pushBuffer.data + PUSHBUFFER_SIZE / 4;
	if (engine->pushAt + 2 > end)
		return;
	/* A Fermi method header: increasing methods, one argument, subchannel 0. */
	*engine->pushAt++ = (0x2u << 28) | (1u << 16) | (method >> 2);
	*engine->pushAt++ = value;
}

void
nvencAddress(NvencEngine *engine, uint32_t method, uint64_t gpuAddress)
{
	nvencMethod(engine, method, (uint32_t)(gpuAddress >> 8));
}

int
nvencExecute(NvencEngine *engine, int timeoutMs, char *reason, size_t reasonSize)
{
	volatile uint32_t *semaphore = engine->semaphore.data;
	uint32_t want = ++engine->semaphoreValue;
	*semaphore = 0;

	nvencMethod(engine, NVENC_SEMAPHORE_A,
		(uint32_t)(engine->semaphore.gpuAddress >> 32) & 0xff);
	nvencMethod(engine, NVENC_SEMAPHORE_B, (uint32_t)engine->semaphore.gpuAddress);
	nvencMethod(engine, NVENC_SEMAPHORE_C, want);
	nvencMethod(engine, NVENC_SEMAPHORE_D, 0);

	uint32_t words = (uint32_t)(engine->pushAt - (uint32_t *)engine->pushBuffer.data);
	uint32_t *entry = (uint32_t *)engine->gpFifo.data + 2 * engine->gpPut;
	entry[0] = (uint32_t)engine->pushBuffer.gpuAddress & ~3u;
	entry[1] = ((uint32_t)(engine->pushBuffer.gpuAddress >> 32) & 0xff) | (words << 10);
	engine->gpPut = (engine->gpPut + 1) % GPFIFO_ENTRIES;
	__sync_synchronize();
	engine->userd[USERD_GPPUT] = engine->gpPut;
	if (engine->doorbell != NULL) {
		__sync_synchronize();
		*engine->doorbell = engine->workSubmitToken;
	}

	/* The error notifier's first entry: resman fills it in when the channel
	 * faults (an engine exception, a bad address). */
	volatile const uint32_t *notifier = engine->notifier.data;
	for (int waited = 0; waited < timeoutMs * 10; waited++) {
		if (*semaphore == want)
			return 0;
		if (notifier[3] != 0) {
			/* timestamp, info32, then info16 and the status */
			if (reason != NULL) {
				snprintf(reason, reasonSize,
					"the encoder's channel failed (info %#x, status %#x)",
					notifier[2], notifier[3] >> 16);
			}
			return NVENC_FAILED;
		}
		usleep(100);
	}
	if (reason != NULL) {
		snprintf(reason, reasonSize, "the encoder did not answer (GPGet %u, GPPut %u)",
			engine->userd[USERD_GPGET], engine->gpPut);
	}
	return NVENC_TIMED_OUT;
}

size_t
nvencBlockLinearOffset(int x, int y, int pitch, bool linesInFours)
{
	const int blockHeight = 2;		/* groups of eight lines per block */
	int blocksPerRow = pitch / 64;
	int blockY = (y / 8) / blockHeight;
	int groupInBlock = (y / 8) % blockHeight;
	size_t base = ((size_t)(blockY * blocksPerRow + x / 64) * blockHeight
		+ groupInBlock) * 512;
	int xx = x % 64, yy = y % 8;
	if (linesInFours) {
		return base + (xx / 32) * 256 + (yy / 4) * 128
			+ ((xx % 32) / 16) * 64 + (yy % 4) * 16 + (xx % 16);
	}
	return base + (xx / 32) * 256 + (yy / 2) * 64
		+ ((xx % 32) / 16) * 32 + (yy % 2) * 16 + (xx % 16);
}
