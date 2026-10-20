/* Exercise a decoder plugin directly with the timestamped chunks used by MSE.
 *
 * Usage: ffmpegpreroll ADDON FILE FRAMES [SEEK_SECONDS [TARGET_SECONDS [HINT [VERIFY]]]]
 * Compare the RGB hashes with HINT=0 and HINT=1. TARGET_SECONDS excludes
 * preroll from both hashes. VERIFY=1 also checks that hinted preroll leaves
 * the caller's pixel buffer untouched; disable it for timing measurements.
 * TARGET_SECONDS must leave both preroll and displayed frames in the sample.
 * With TARGET_SECONDS=0 the hash includes every frame, even untimed frames,
 * for comparison against an older decoder. Alpha and row padding are ignored.
 *
 * Needs private media and media/experimental headers; link media, be, crypto.
 * Fixture suggestions: https://fate-suite.ffmpeg.org/av1/ (film_grain.ivf,
 * frames_refs_short_signaling.ivf, non_uniform_tiling.ivf, switch_frame.ivf).
 */
#include <File.h>
#include <MediaIO.h>
#include <OS.h>
#include <image.h>
#include <DecoderPlugin.h>
#include <ReaderPlugin.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

using namespace BPrivate::media;

struct FileIO : BMediaIO {
	BFile file;

	explicit FileIO(const char* path) : file(path, B_READ_ONLY) {}
	void GetFlags(int32* flags) const override { *flags = B_MEDIA_SEEKABLE; }
	ssize_t ReadAt(off_t p, void* b, size_t n) override
		{ return file.ReadAt(p, b, n); }
	ssize_t WriteAt(off_t, const void*, size_t) override { return B_NOT_ALLOWED; }
	off_t Seek(off_t p, uint32 m) override { return file.Seek(p, m); }
	off_t Position() const override { return file.Position(); }
	status_t GetSize(off_t* n) const override { return file.GetSize(n); }
};

struct Chunks : ChunkProvider {
	Reader* reader;
	void* cookie;

	Chunks(Reader* r, void* c) : reader(r), cookie(c) {}
	status_t GetNextChunk(const void** p, size_t* n, media_header* h) override
	{
		status_t status = reader->GetNextChunk(cookie, p, n, h);
		// MSE supplies presentation times in microseconds, without FFmpeg's
		// container-specific packet metadata (whose timestamps use other units).
		h->user_data_type = 0;
		return status;
	}
};

static void
Check(status_t status, const char* step)
{
	if (status == B_OK)
		return;
	fprintf(stderr, "%s: %ld %s\n", step, long(status), strerror(status));
	exit(1);
}

int
main(int argc, char** argv)
{
	if (argc < 4)
		return 2;
	const int wanted = atoi(argv[3]);
	const bigtime_t target = argc > 5 ? atof(argv[5]) * 1000000 : 0;
	const bool hint = argc > 6 && atoi(argv[6]);
	const bool verify = argc > 7 && atoi(argv[7]);
	if (wanted <= 0 || target < 0)
		return 2;

	image_id image = load_add_on(argv[1]);
	Check(image < 0 ? image : B_OK, "load addon");
	MediaPlugin* (*create)();
	Check(get_image_symbol(image, "instantiate_plugin", B_SYMBOL_TYPE_TEXT,
		(void**)&create), "symbol");
	std::unique_ptr<MediaPlugin> plugin(create());
	auto* readers = dynamic_cast<ReaderPlugin*>(plugin.get());
	auto* decoders = dynamic_cast<DecoderPlugin*>(plugin.get());
	if (readers == NULL || decoders == NULL)
		return 1;
	media_format* supported = NULL;
	size_t supportedCount = 0;
	Check(decoders->GetSupportedFormats(&supported, &supportedCount), "formats");

	FileIO file(argv[2]);
	Check(file.file.InitCheck(), "file");
	std::unique_ptr<Reader> reader(readers->NewReader());
	reader->Setup(&file);
	int32 count = 0;
	Check(reader->Sniff(&count), "sniff");
	void* cookie = NULL;
	media_format encoded = {};
	int64 frames = 0;
	bigtime_t duration = 0;
	const void* info = NULL;
	size_t infoSize = 0;
	for (int32 i = 0; i < count; i++) {
		Check(reader->AllocateCookie(i, &cookie), "cookie");
		Check(reader->GetStreamInfo(cookie, &frames, &duration, &encoded,
			&info, &infoSize), "stream");
		if (encoded.type == B_MEDIA_ENCODED_VIDEO)
			break;
		reader->FreeCookie(cookie);
		cookie = NULL;
	}
	if (cookie == NULL)
		return 1;
	if (argc > 4) {
		bigtime_t time = atof(argv[4]) * 1000000;
		int64 frame = 0;
		Check(reader->Seek(cookie, B_MEDIA_SEEK_TO_TIME
			| B_MEDIA_SEEK_CLOSEST_BACKWARD, &frame, &time), "seek reader");
	}

	std::unique_ptr<Decoder> decoder(decoders->NewDecoder(0));
	decoder->SetChunkProvider(new Chunks(reader.get(), cookie));
	bigtime_t started = system_time();
	Check(decoder->Setup(&encoded, info, infoSize), "setup");
	media_format output = {};
	output.type = B_MEDIA_RAW_VIDEO;
	output.u.raw_video = media_raw_video_format::wildcard;
	output.u.raw_video.display.format = B_RGB32;
	Check(decoder->NegotiateOutputFormat(&output), "output");
	const double setupMs = (system_time() - started) / 1000.;
	const auto& display = output.u.raw_video.display;
	std::vector<unsigned char> pixels(size_t(display.bytes_per_row)
		* display.line_count, 0xa5);
	media_codec_info name = {};
	decoder->GetCodecInfo(&name);
	printf("decoder=%s size=%ux%u setupMs=%.3f\n", name.pretty_name,
		display.line_width, display.line_count, setupMs);

	SHA256_CTX hash;
	SHA256_Init(&hash);
	int got = 0, skipped = 0;
	double firstDisplayMs = -1, decodeOnlyMs = 0;
	bigtime_t firstPTS = 0;
	started = system_time();
	while (got < wanted) {
		int64 decoded = 0;
		media_header header = {};
		media_decode_info parameters = {};
		if (hint)
			parameters.time_to_decode = -target;
		std::vector<unsigned char> before;
		if (verify)
			before = pixels;
		bigtime_t decodeStarted = system_time();
		status_t status = decoder->Decode(pixels.data(), &decoded, &header,
			&parameters);
		decodeOnlyMs += (system_time() - decodeStarted) / 1000.;
		Check(status, "decode");
		if (decoded != 1)
			return 1;
		got++;
		if (target > 0 && header.start_time < target) {
			skipped++;
			if (hint && verify && pixels != before) {
				fprintf(stderr, "preroll buffer changed at %lld\n", header.start_time);
				return 1;
			}
			continue;
		}
		if (firstDisplayMs < 0) {
			firstDisplayMs = (system_time() - started) / 1000.;
			firstPTS = header.start_time;
		}
		for (uint32 y = 0; y < display.line_count; y++) {
			for (uint32 x = 0; x < display.line_width; x++) {
				SHA256_Update(&hash, pixels.data()
					+ size_t(y) * display.bytes_per_row + 4 * x, 3);
			}
		}
	}
	printf("skipped=%d firstPTS=%lld firstDisplayMs=%.3f decodeOnlyMs=%.3f\n",
		skipped, firstPTS, firstDisplayMs, decodeOnlyMs);
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256_Final(digest, &hash);
	printf("frames=%d decodeMs=%.3f sha256=", got,
		(system_time() - started) / 1000.);
	for (auto byte : digest)
		printf("%02x", byte);
	puts("");

	decoder.reset();
	reader->FreeCookie(cookie);
	reader.reset();
	plugin.reset();
	unload_add_on(image);
	return got == wanted && (!target || (skipped > 0 && skipped < got)) ? 0 : 1;
}
