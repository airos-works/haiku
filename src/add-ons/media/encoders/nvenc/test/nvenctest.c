/* nvenctest: encode a synthetic moving picture with the nvenc add-on and
 * write the stream, for trying the add-on on a card without MediaConverter.
 *
 *   nvenctest <add-on> <h264|hevc|hevc10> <width> <height> <pictures> <out>
 *     [qp] [variant] [key interval] [source.yuv]
 *
 * The pictures are a gradient with a square and a ramp moving over it, so
 * that P pictures have motion to find. With a fourth path the NV12 (or
 * P010) source is written too, to compare the decoded stream against. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <image.h>
#include <OS.h>

#include <nvenc_api.h>

static void
make_picture(uint8_t *luma, uint8_t *chroma, int width, int height, int index,
	int bytes)
{
	for (int y = 0; y < height; y++) {
		for (int x = 0; x < width; x++) {
			/* a texture panning 3 to the left and 1 up per picture, and a
			 * square moving the other way over it */
			int tx = x + 3 * index, ty = y + index;
			int value = 64 + ((tx * 7 + ty * 3) & 0x3f) + (((tx >> 4) ^ (ty >> 4)) & 1) * 96;
			int sx = width - 1 - (index * 5) % width, sy = (index * 2) % height;
			if (x >= sx && x < sx + width / 5 && y >= sy && y < sy + height / 5)
				value = 235 - (((x - sx) ^ (y - sy)) & 0x3f);
			if (bytes == 1)
				luma[(size_t)y * width + x] = value;
			else
				((uint16_t *)luma)[(size_t)y * width + x] = (uint16_t)(value << 8);
		}
	}
	for (int y = 0; y < height / 2; y++) {
		for (int x = 0; x < width / 2; x++) {
			int tx = x + (3 * index) / 2, ty = y + index / 2;
			int u = 128 + ((tx * 2) & 0x3f) - 32;
			int v = 128 + ((ty * 3 + tx) & 0x3f) - 32;
			if (bytes == 1) {
				chroma[(size_t)y * width + 2 * x] = u;
				chroma[(size_t)y * width + 2 * x + 1] = v;
			} else {
				uint16_t *line = (uint16_t *)chroma + (size_t)y * width;
				line[2 * x] = (uint16_t)(u << 8);
				line[2 * x + 1] = (uint16_t)(v << 8);
			}
		}
	}
}

int
main(int argc, char **argv)
{
	if (argc < 7) {
		fprintf(stderr, "usage: %s <add-on> <h264|hevc|hevc10> <width> <height> "
			"<pictures> <out> [qp] [variant] [key interval] [source.yuv]\n", argv[0]);
		return 2;
	}
	image_id image = load_add_on(argv[1]);
	if (image < 0) {
		fprintf(stderr, "cannot load %s\n", argv[1]);
		return 1;
	}
	nvenc_get_api_func getApi;
	if (get_image_symbol(image, NVENC_GET_API_SYMBOL, B_SYMBOL_TYPE_TEXT,
			(void **)&getApi) != B_OK) {
		fprintf(stderr, "no %s in %s\n", NVENC_GET_API_SYMBOL, argv[1]);
		return 1;
	}
	const nvenc_api *api = getApi();

	uint32_t codecs = 0;
	char name[128] = "", reason[256] = "";
	if (api->probe(&codecs, name, sizeof(name), reason, sizeof(reason)) != 0) {
		fprintf(stderr, "probe: %s\n", reason);
		return 1;
	}
	printf("%s, codecs %#x, %u variants\n", name, codecs, api->variants(0, NULL));

	nvenc_config config;
	memset(&config, 0, sizeof(config));
	int bytes = 1;
	if (strcmp(argv[2], "h264") == 0)
		config.codec = NVENC_CODEC_H264;
	else if (strcmp(argv[2], "hevc") == 0)
		config.codec = NVENC_CODEC_HEVC;
	else if (strcmp(argv[2], "hevc10") == 0) {
		config.codec = NVENC_CODEC_HEVC_10;
		bytes = 2;
	} else {
		fprintf(stderr, "unknown codec %s\n", argv[2]);
		return 2;
	}
	int width = atoi(argv[3]), height = atoi(argv[4]), pictures = atoi(argv[5]);
	config.width = width;
	config.height = height;
	config.frame_rate_num = 30;
	config.frame_rate_den = 1;
	config.rate_control = NVENC_RC_CONSTQP;
	config.qp = argc > 7 ? atoi(argv[7]) : 26;
	config.variant = argc > 8 ? atoi(argv[8]) : 0;
	config.key_interval = argc > 9 ? atoi(argv[9]) : 30;
	config.preset = 4;
	config.colour_primaries = config.transfer = config.matrix = 1;
	config.timeout_ms = 2000;

	const char *variantName = "";
	api->variants(config.variant, &variantName);
	printf("variant %u: %s\n", config.variant, variantName);
	nvenc_session *session = api->open(&config, reason, sizeof(reason));
	if (session == NULL) {
		fprintf(stderr, "open: %s\n", reason);
		return 1;
	}
	printf("%s\n", api->describe(session));

	FILE *out = fopen(argv[6], "wb");
	FILE *source = argc > 10 ? fopen(argv[10], "wb") : NULL;
	if (out == NULL) {
		fprintf(stderr, "cannot write %s\n", argv[6]);
		return 1;
	}
	uint8_t headers[512];
	size_t headersSize = sizeof(headers);
	if (api->headers(session, headers, &headersSize) != 0)
		fprintf(stderr, "no headers\n");

	size_t lumaSize = (size_t)width * height * bytes;
	uint8_t *luma = malloc(lumaSize);
	uint8_t *chroma = malloc(lumaSize / 2);
	int failed = 0;
	size_t total = 0;
	bigtime_t start = system_time();
	for (int i = 0; i <= pictures; i++) {
		int status;
		if (i < pictures) {
			make_picture(luma, chroma, width, height, i, bytes);
			if (source != NULL) {
				fwrite(luma, 1, lumaSize, source);
				fwrite(chroma, 1, lumaSize / 2, source);
			}
			nvenc_picture picture;
			memset(&picture, 0, sizeof(picture));
			picture.planes[0] = luma;
			picture.planes[1] = chroma;
			picture.strides[0] = picture.strides[1] = width * bytes;
			picture.pts = i;
			status = api->encode(session, &picture);
		} else
			status = api->encode(session, NULL);
		if (status != 0) {
			fprintf(stderr, "picture %d: %s\n", i, api->describe(session));
			failed = 1;
			break;
		}
		nvenc_packet packet;
		int got;
		while ((got = api->receive(session, &packet)) == 0) {
			fwrite(packet.data, 1, packet.size, out);
			total += packet.size;
			if (getenv("NVENCTEST_VERBOSE") != NULL) {
				printf("packet pts %lld, %zu bytes%s\n", (long long)packet.pts,
					packet.size, packet.key ? ", key" : "");
			}
			api->release(session, &packet);
		}
		if (got < 0) {
			fprintf(stderr, "receive failed\n");
			failed = 1;
			break;
		}
	}
	bigtime_t elapsed = system_time() - start;
	printf("%s: %zu bytes, %.1f pictures/s\n", failed ? "FAILED" : "done", total,
		pictures * 1e6 / (elapsed > 0 ? elapsed : 1));
	fclose(out);
	if (source != NULL)
		fclose(source);
	api->close(session);
	free(luma);
	free(chroma);
	return failed;
}
