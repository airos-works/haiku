/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	A stand-in for the nvenc add-on that encodes with x264 instead of a
	card, to try MediaConverter's NVENC paths - the self-test, the encoder's
	streaming, the fall back to the CPU - on a machine without one. Run
	MediaConverter or mediaconvert with MEDIACONVERTER_FAKE_NVIDIA=1b81 (a
	GTX 1070) and this installed as non-packaged/add-ons/media/encoders/nvenc.

	FAKE_NVENC_GOOD=n: only layout n gives correct pictures (default 0)
	FAKE_NVENC_TIMEOUT=1: every picture times out
	It claims H.264 and H.265; H.265 always fails to open. */


#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <deque>
#include <vector>

#include <nvenc_api.h>

#include "../../../../../apps/mediaconverter/thirdparty/x264/x264.h"


struct X264 {
	void			(*param_default)(x264_param_t*);
	int				(*param_default_preset)(x264_param_t*, const char*,
						const char*);
	int				(*param_apply_profile)(x264_param_t*, const char*);
	x264_t*			(*open)(x264_param_t*);
	int				(*headers)(x264_t*, x264_nal_t**, int*);
	int				(*encode)(x264_t*, x264_nal_t**, int*, x264_picture_t*,
						x264_picture_t*);
	int				(*delayed)(x264_t*);
	void			(*close)(x264_t*);
	void			(*picture_init)(x264_picture_t*);
};


struct Packet {
	std::vector<uint8_t>	data;
	int64_t					pts;
	int64_t					dts;
	bool					key;
};


struct nvenc_session {
	X264					api;
	x264_t*					encoder;
	nvenc_config			config;
	bool					garbage;
	bool					drained;
	std::vector<uint8_t>	headers;
	std::deque<Packet*>		packets;
	char					description[160];
};


static bool
load_x264(X264& api)
{
	void* library = dlopen("libx264.so.164", RTLD_NOW);
	if (library == NULL)
		return false;
	api.param_default = (void (*)(x264_param_t*))dlsym(library,
		"x264_param_default");
	api.param_default_preset = (int (*)(x264_param_t*, const char*,
		const char*))dlsym(library, "x264_param_default_preset");
	api.param_apply_profile = (int (*)(x264_param_t*, const char*))dlsym(
		library, "x264_param_apply_profile");
	api.open = (x264_t* (*)(x264_param_t*))dlsym(library,
		"x264_encoder_open_164");
	api.headers = (int (*)(x264_t*, x264_nal_t**, int*))dlsym(library,
		"x264_encoder_headers");
	api.encode = (int (*)(x264_t*, x264_nal_t**, int*, x264_picture_t*,
		x264_picture_t*))dlsym(library, "x264_encoder_encode");
	api.delayed = (int (*)(x264_t*))dlsym(library,
		"x264_encoder_delayed_frames");
	api.close = (void (*)(x264_t*))dlsym(library, "x264_encoder_close");
	api.picture_init = (void (*)(x264_picture_t*))dlsym(library,
		"x264_picture_init");
	return api.param_default_preset != NULL && api.open != NULL
		&& api.encode != NULL && api.picture_init != NULL;
}


static int
fake_probe(uint32_t* codecs, char* name, size_t nameSize, char* reason,
	size_t reasonSize)
{
	X264 api;
	if (!load_x264(api)) {
		snprintf(reason, reasonSize, "The stand-in NVENC needs x264.");
		return -1;
	}
	*codecs = NVENC_CODEC_H264 | NVENC_CODEC_HEVC;
	snprintf(name, nameSize, "Stand-in GeForce GTX 1070");
	return 0;
}


static void
take(nvenc_session* session, x264_nal_t* nals, int count,
	const x264_picture_t& out)
{
	if (count <= 0)
		return;
	Packet* packet = new Packet;
	for (int i = 0; i < count; i++)
		packet->data.insert(packet->data.end(), nals[i].p_payload,
			nals[i].p_payload + nals[i].i_payload);
	packet->pts = out.i_pts;
	packet->dts = out.i_dts;
	packet->key = out.b_keyframe;
	session->packets.push_back(packet);
}


static nvenc_session*
fake_open(const nvenc_config* config, char* reason, size_t reasonSize)
{
	if (config->codec != NVENC_CODEC_H264) {
		snprintf(reason, reasonSize, "The stand-in NVENC only does H.264.");
		return NULL;
	}
	nvenc_session* session = new nvenc_session;
	if (!load_x264(session->api)) {
		delete session;
		return NULL;
	}
	session->config = *config;
	const char* good = getenv("FAKE_NVENC_GOOD");
	session->garbage = config->variant != (uint32_t)(good != NULL ? atoi(good) : 0);
	session->drained = false;

	x264_param_t param;
	session->api.param_default_preset(&param, "veryfast", "zerolatency");
	param.i_width = config->width;
	param.i_height = config->height;
	param.i_csp = X264_CSP_NV12;
	param.i_fps_num = config->frame_rate_num;
	param.i_fps_den = config->frame_rate_den;
	param.i_timebase_num = config->frame_rate_den;
	param.i_timebase_den = config->frame_rate_num;
	param.i_keyint_max = config->key_interval;
	param.i_bframe = 0;
	param.i_threads = 4;
	param.b_repeat_headers = 1;
	param.b_annexb = 1;
	if (config->rate_control == NVENC_RC_CONSTQP) {
		param.rc.i_rc_method = X264_RC_CQP;
		param.rc.i_qp_constant = config->qp;
	} else {
		param.rc.i_rc_method = X264_RC_ABR;
		param.rc.i_bitrate = config->bitrate / 1000;
	}
	if (session->api.param_apply_profile != NULL)
		session->api.param_apply_profile(&param, "main");
	session->encoder = session->api.open(&param);
	if (session->encoder == NULL) {
		snprintf(reason, reasonSize, "x264 did not open");
		delete session;
		return NULL;
	}
	x264_nal_t* nals;
	int count;
	if (session->api.headers(session->encoder, &nals, &count) >= 0) {
		for (int i = 0; i < count; i++)
			session->headers.insert(session->headers.end(), nals[i].p_payload,
				nals[i].p_payload + nals[i].i_payload);
	}
	snprintf(session->description, sizeof(session->description),
		"stand-in (x264), layout %u%s", config->variant,
		session->garbage ? ", gives wrong pictures" : "");
	return session;
}


static int
fake_headers(nvenc_session* session, uint8_t* buffer, size_t* size)
{
	if (*size < session->headers.size())
		return -1;
	memcpy(buffer, session->headers.data(), session->headers.size());
	*size = session->headers.size();
	return 0;
}


static int
fake_encode(nvenc_session* session, const nvenc_picture* picture)
{
	if (getenv("FAKE_NVENC_TIMEOUT") != NULL) {
		snprintf(session->description, sizeof(session->description),
			"NVENC: the encoder did not answer (stand-in)");
		return NVENC_TIMED_OUT;
	}
	x264_nal_t* nals;
	int count;
	x264_picture_t out;
	if (picture == NULL) {
		session->drained = true;
		while (session->api.delayed(session->encoder) > 0) {
			if (session->api.encode(session->encoder, &nals, &count, NULL,
					&out) < 0)
				break;
			take(session, nals, count, out);
		}
		return 0;
	}

	// a wrong layout: the picture comes out shifted by half its height
	std::vector<uint8_t> shifted;
	const uint8_t* planes[2] = { picture->planes[0], picture->planes[1] };
	if (session->garbage) {
		uint32_t width = session->config.width, height = session->config.height;
		shifted.resize((size_t)width * height * 3 / 2);
		for (uint32_t y = 0; y < height; y++) {
			memcpy(&shifted[(size_t)y * width], picture->planes[0]
				+ (size_t)((y + height / 2) % height) * picture->strides[0], width);
		}
		for (uint32_t y = 0; y < height / 2; y++) {
			memcpy(&shifted[(size_t)width * height + (size_t)y * width],
				picture->planes[1] + (size_t)y * picture->strides[1], width);
		}
		planes[0] = shifted.data();
		planes[1] = shifted.data() + (size_t)width * height;
	}

	x264_picture_t in;
	session->api.picture_init(&in);
	in.img.i_csp = X264_CSP_NV12;
	in.img.i_plane = 2;
	in.img.plane[0] = (uint8_t*)planes[0];
	in.img.plane[1] = (uint8_t*)planes[1];
	in.img.i_stride[0] = session->garbage ? session->config.width
		: picture->strides[0];
	in.img.i_stride[1] = session->garbage ? session->config.width
		: picture->strides[1];
	in.i_pts = picture->pts;
	in.i_type = picture->force_key ? X264_TYPE_IDR : X264_TYPE_AUTO;
	if (session->api.encode(session->encoder, &nals, &count, &in, &out) < 0)
		return NVENC_FAILED;
	take(session, nals, count, out);
	return 0;
}


static int
fake_receive(nvenc_session* session, nvenc_packet* packet)
{
	if (session->packets.empty())
		return session->drained ? 2 : 1;
	Packet* next = session->packets.front();
	session->packets.pop_front();
	memset(packet, 0, sizeof(*packet));
	packet->data = next->data.data();
	packet->size = next->data.size();
	packet->pts = next->pts;
	packet->dts = next->dts;
	packet->key = next->key;
	packet->cookie = next;
	return 0;
}


static void
fake_release(nvenc_session* session, nvenc_packet* packet)
{
	delete (Packet*)packet->cookie;
	packet->cookie = NULL;
}


static void
fake_close(nvenc_session* session)
{
	if (session == NULL)
		return;
	for (size_t i = 0; i < session->packets.size(); i++)
		delete session->packets[i];
	session->api.close(session->encoder);
	delete session;
}


static const char*
fake_describe(nvenc_session* session)
{
	return session->description;
}


static uint32_t
fake_variants(uint32_t variant, const char** name)
{
	static const char* kNames[] = { "stand-in layout 0", "stand-in layout 1",
		"stand-in layout 2" };
	if (name != NULL)
		*name = variant < 3 ? kNames[variant] : NULL;
	return 3;
}


static const nvenc_api kApi = {
	NVENC_API_VERSION,
	fake_probe,
	fake_open,
	fake_headers,
	fake_encode,
	fake_receive,
	fake_release,
	fake_close,
	fake_describe,
	fake_variants,
};


extern "C" __attribute__((visibility("default"))) const nvenc_api*
nvenc_get_api()
{
	return &kApi;
}
