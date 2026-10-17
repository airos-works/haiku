/* The add-on's side of nvenc_api.h. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nvenc_api.h>

#include "nvenc_common.h"
#include "nvenc_engine.h"
#include "nvenc_h264.h"
#include "nvenc_hevc.h"

/* What this add-on can drive; the card says what it has. */
#define IMPLEMENTED_CODECS	(NVENC_CODEC_H264 | NVENC_CODEC_HEVC | NVENC_CODEC_HEVC_10)

struct nvenc_session {
	NvencEngine	*engine;
	uint32_t	codec;
	NvencH264	*h264;
	NvencHevc	*hevc;
	NvencQueue	queue;
	bool		drained;
	char		description[320];
};

static void
set_reason(char *reason, size_t size, const char *text)
{
	if (reason != NULL && size > 0)
		snprintf(reason, size, "%s", text);
}

static int
nvenc_probe(uint32_t *codecs, char *name, size_t nameSize, char *reason,
	size_t reasonSize)
{
	*codecs = 0;
	if (access("/dev/nvidiactl", F_OK) != 0) {
		set_reason(reason, reasonSize, "The NVIDIA driver (nvidia_rm) is not running.");
		return -1;
	}
	NvencCard card;
	char why[200] = "";
	if (!nvencIdentify(&card, why, sizeof(why))) {
		if (card.name[0] != '\0' && card.encoderClass == 0) {
			set_reason(reason, reasonSize, why);
		} else if (reason != NULL) {
			snprintf(reason, reasonSize, "The NVIDIA driver did not answer (%s).", why);
		}
		if (name != NULL && nameSize > 0)
			snprintf(name, nameSize, "%s", card.name);
		return -1;
	}
	if (name != NULL && nameSize > 0)
		snprintf(name, nameSize, "%s", card.name);
	*codecs = card.hardwareCodecs & IMPLEMENTED_CODECS;
	if (*codecs == 0) {
		if (reason != NULL) {
			snprintf(reason, reasonSize,
				"This add-on cannot drive the %s's encoder (class %#x).", card.name,
				card.encoderClass);
		}
		return -1;
	}
	return 0;
}

static void
nvenc_close(nvenc_session *session)
{
	if (session == NULL)
		return;
	nvencH264Close(session->h264);
	nvencHevcClose(session->hevc);
	nvencQueueClear(&session->queue);
	nvencClose(session->engine);
	free(session);
}

static nvenc_session *
nvenc_open(const nvenc_config *config, char *reason, size_t reasonSize)
{
	if ((config->codec & IMPLEMENTED_CODECS) == 0) {
		set_reason(reason, reasonSize, "This add-on does not encode that format.");
		return NULL;
	}
	nvenc_session *session = calloc(1, sizeof(nvenc_session));
	if (session == NULL)
		return NULL;
	session->codec = config->codec;
	session->engine = nvencOpen(reason, reasonSize);
	if (session->engine == NULL) {
		free(session);
		return NULL;
	}
	if ((nvencCard(session->engine)->hardwareCodecs & config->codec) == 0) {
		if (reason != NULL) {
			snprintf(reason, reasonSize, "The %s cannot encode that format.",
				nvencCard(session->engine)->name);
		}
		nvenc_close(session);
		return NULL;
	}
	if (config->codec == NVENC_CODEC_H264)
		session->h264 = nvencH264Open(session->engine, config, reason, reasonSize);
	else
		session->hevc = nvencHevcOpen(session->engine, config, reason, reasonSize);
	if (session->h264 == NULL && session->hevc == NULL) {
		nvenc_close(session);
		return NULL;
	}
	const char *what = session->h264 != NULL ? nvencH264Describe(session->h264)
		: nvencHevcDescribe(session->hevc);
	snprintf(session->description, sizeof(session->description), "%s; input %s", what,
		kNvencLayouts[config->variant].name);
	return session;
}

static int
nvenc_headers(nvenc_session *session, uint8_t *buffer, size_t *size)
{
	size_t written = session->h264 != NULL
		? nvencH264Headers(session->h264, buffer, *size)
		: nvencHevcHeaders(session->hevc, buffer, *size);
	*size = written;
	return written > 0 ? 0 : -1;
}

static int
nvenc_encode(nvenc_session *session, const nvenc_picture *picture)
{
	if (picture == NULL) {
		/* every picture is finished as it is handed over */
		session->drained = true;
		return 0;
	}
	char reason[256];
	int status = session->h264 != NULL
		? nvencH264Encode(session->h264, picture, &session->queue, reason, sizeof(reason))
		: nvencHevcEncode(session->hevc, picture, &session->queue, reason, sizeof(reason));
	if (status != 0) {
		snprintf(session->description, sizeof(session->description), "%s", reason);
		fprintf(stderr, "nvenc: %s\n", reason);
	}
	return status;
}

static int
nvenc_receive(nvenc_session *session, nvenc_packet *packet)
{
	NvencPacket *next = nvencQueuePop(&session->queue);
	if (next == NULL)
		return session->drained ? 2 : 1;
	memset(packet, 0, sizeof(*packet));
	packet->data = next->data;
	packet->size = next->size;
	packet->pts = next->pts;
	packet->dts = next->dts;
	packet->key = next->key;
	packet->cookie = next;
	return 0;
}

static void
nvenc_release(nvenc_session *session, nvenc_packet *packet)
{
	(void)session;
	NvencPacket *next = packet->cookie;
	if (next == NULL)
		return;
	free(next->data);
	free(next);
	packet->cookie = NULL;
}

static const char *
nvenc_describe(nvenc_session *session)
{
	return session->description;
}

static uint32_t
nvenc_variants(uint32_t variant, const char **name)
{
	if (name != NULL)
		*name = variant < kNvencLayoutCount ? kNvencLayouts[variant].name : NULL;
	return kNvencLayoutCount;
}

static const nvenc_api kApi = {
	.version = NVENC_API_VERSION,
	.probe = nvenc_probe,
	.open = nvenc_open,
	.headers = nvenc_headers,
	.encode = nvenc_encode,
	.receive = nvenc_receive,
	.release = nvenc_release,
	.close = nvenc_close,
	.describe = nvenc_describe,
	.variants = nvenc_variants,
};

__attribute__((visibility("default"))) const nvenc_api *
nvenc_get_api(void)
{
	return &kApi;
}
