/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include <Application.h>
#include <DecoderPlugin.h>
#include <MediaFormats.h>
#include <image.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vector>

static void Require(bool ok, const char* why)
{
	if (!ok) { fprintf(stderr, "FAIL: %s\n", why); exit(1); }
}
static void Check(status_t status, const char* why)
{
	if (status != B_OK) { fprintf(stderr, "FAIL: %s: %s (%ld)\n", why, strerror(status), (long)status); exit(1); }
}
static void Read(FILE* file, void* out, size_t bytes) { Require(fread(out, 1, bytes, file) == bytes, "read fixture"); }
struct Packet { std::vector<uint8> data; int64 time; uint32 key; };
struct Fixture {
	uint32 width, height;
	std::vector<uint8> extra;
	std::vector<Packet> packets;
	Fixture(const char* path) {
		FILE* f = fopen(path, "rb"); Require(f != NULL, "open fixture");
		uint32 h[5]; Read(f, h, sizeof(h));
		Require(h[0] == 0x314b5055 && h[1] <= 4096 && h[2] <= 4096
			&& h[3] <= 4*1024*1024 && h[4] > 0 && h[4] <= 10000, "fixture bounds");
		width = h[1]; height = h[2]; extra.resize(h[3]); Read(f, extra.data(), extra.size());
		packets.resize(h[4]);
		for (auto& p : packets) {
			uint32 size; Read(f, &size, 4); Read(f, &p.time, 8); Read(f, &p.key, 4);
			Require(size > 0 && size <= 4*1024*1024, "packet size");
			p.data.resize(size); Read(f, p.data.data(), size);
		}
		Require(fgetc(f) == EOF, "complete fixture"); fclose(f);
	}
};
class Provider : public ChunkProvider {
public:
	Provider(const Fixture& f) : fixture(f), at(0), calls(0), interruptAt(SIZE_MAX), corrupt(false) {}
	virtual status_t GetNextChunk(const void** data, size_t* bytes, media_header* header) {
		calls++;
		if (at == interruptAt) { interruptAt = SIZE_MAX; return B_INTERRUPTED; }
		if (at == fixture.packets.size()) return B_LAST_BUFFER_ERROR;
		const auto& p = fixture.packets[at++]; *data = p.data.data();
		*bytes = p.data.size() - (corrupt ? 1 : 0);
		memset(header, 0, sizeof(*header)); header->start_time = p.time;
		return B_OK;
	}
	const Fixture& fixture;
	size_t at, calls, interruptAt;
	bool corrupt;
};
struct Context {
	Decoder* decoder;
	Provider* provider;
	const Fixture& fixture;
	std::vector<uint8> pixels;
	color_space space;
	size_t bytes;
	unsigned frames;
	Context(DecoderPlugin* plugin, const Fixture& f) : fixture(f), frames(0) {
		decoder = plugin->NewDecoder(0); Require(decoder != NULL, "new decoder");
		provider = new Provider(f); decoder->SetChunkProvider(provider);
	}
	~Context() { delete decoder; }
	status_t Setup() {
		media_format_description description = {};
		description.family = B_MISC_FORMAT_FAMILY; description.u.misc.file_format = 'ffmp';
		description.u.misc.codec = 27;
		media_format format; format.Clear(); format.type = B_MEDIA_ENCODED_VIDEO;
		Check(BMediaFormats().MakeFormatFor(&description, 1, &format), "register stream format");
		format.u.encoded_video.output.display.line_width = fixture.width;
		format.u.encoded_video.output.display.line_count = fixture.height;
		format.u.encoded_video.output.field_rate = 24;
		return decoder->Setup(&format, fixture.extra.data(), fixture.extra.size());
	}
	void Negotiate(color_space requested) {
		media_format output; output.Clear(); output.type = B_MEDIA_RAW_VIDEO;
		output.u.raw_video.display.format = requested;
		Check(decoder->NegotiateOutputFormat(&output), "negotiate output");
		space = output.u.raw_video.display.format; Require(space == requested, "requested colour space");
		Require(output.u.raw_video.display.line_width == fixture.width
			&& output.u.raw_video.display.line_count == fixture.height, "negotiated dimensions");
		bytes = fixture.width * fixture.height * (space == B_YCbCr422 ? 2 : space == B_RGB32 ? 4 : 3) / (space == B_YCbCr422 || space == B_RGB32 ? 1 : 2);
		pixels.assign(bytes + 64, 0xbd);
	}
	status_t Next(FILE* output, uint32 stage) {
		int64 count = 0; media_header header = {}; media_decode_info info = {};
		status_t status = decoder->Decode(pixels.data() + 32, &count, &header, &info);
		if (status != B_OK) { Require(count == 0, "error returns no frame"); return status; }
		Require(count == 1 && header.size_used == bytes, "one complete frame");
		for (unsigned i = 0; i < 32; i++) Require(pixels[i] == 0xbd && pixels[32 + bytes + i] == 0xbd, "output guards");
		if (output != NULL) {
			uint32 h[] = {stage, fixture.width, fixture.height, (uint32)space, (uint32)bytes};
			Require(fwrite(h, 1, sizeof(h), output) == sizeof(h)
				&& fwrite(&header.start_time, 1, 8, output) == 8
				&& fwrite(pixels.data() + 32, 1, bytes, output) == bytes, "save frame");
		}
		frames++; return B_OK;
	}
	void Seek(size_t packet) {
		provider->at = packet; frames = 0;
		Check(decoder->SeekedTo(0, fixture.packets[packet].time), "seek/reset");
	}
};
int main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	bool absent = (argc == 5 || argc == 6) && strcmp(argv[4], "--expect-no-device") == 0;
	if (!absent && (argc < 6 || argc > 7)) {
		fprintf(stderr, "usage: amduvd_plugin_test add-on fixtureA fixtureB outA outB [--unprivileged]\n"
			"       amduvd_plugin_test add-on fixtureA fixtureB --expect-no-device\n"); return 2;
	}
	Fixture fa(argv[2]), fb(argv[3]);
	FILE* outputs[2] = {absent ? NULL : fopen(argv[4], "wb"), absent ? NULL : fopen(argv[5], "wb")};
	Require(absent || (outputs[0] && outputs[1]), "open outputs");
	BApplication app("application/x-vnd.airOS-amduvd-test");
	// The lab desktop belongs to UID 0; establish its app_server connection
	// before dropping privileges. The decoder/device are opened afterwards.
	if ((absent && argc == 6) || argc == 7)
		Require(strcmp(argv[argc - 1], "--unprivileged") == 0 && setgid(65534) == 0 && setuid(65534) == 0, "drop privileges");
	printf("decoder test UID %ld\n", (long)getuid());
	image_id image = load_add_on(argv[1]); Require(image >= 0, "load add-on");
	MediaPlugin* (*instantiate)();
	Check(get_image_symbol(image, "instantiate_plugin", B_SYMBOL_TYPE_TEXT, (void**)&instantiate), "instantiate symbol");
	MediaPlugin* base = instantiate(); DecoderPlugin* plugin = dynamic_cast<DecoderPlugin*>(base);
	Require(plugin != NULL, "decoder plugin");
	media_format* formats = NULL; size_t count = 99;
	Require(plugin->GetSupportedFormats(&formats, &count) == B_NOT_SUPPORTED && count == 0, "preserve software format lookup");
	Require(plugin->NewDecoder(2) == NULL, "unknown decoder index rejected");
	{
		Context a(plugin, fa), b(plugin, fb);
		status_t sa = a.Setup(), sb = b.Setup();
		if (absent) { Require(sa != B_OK && sb != B_OK, "no device fails setup"); puts("PASS: plugin load, explicit selection and no-device fallback"); return 0; }
		Check(sa, "setup A"); Check(sb, "setup B");
		a.Negotiate((color_space)0x4e563132); b.Negotiate((color_space)0x4e563132);
		for (int i = 0; i < 3; i++) { Check(a.Next(NULL, 0), "warmup A"); Check(b.Next(NULL, 0), "warmup B"); }
		a.Seek(0); a.provider->interruptAt = 2;
		Require(a.Next(NULL, 0) == B_INTERRUPTED, "interruption is not EOF and emits no queued frame");
		a.Seek(0); b.Seek(0);
		bigtime_t start = system_time();
		bool doneA = false, doneB = false;
		while (!doneA || !doneB) {
			if (!doneA) { status_t s = a.Next(outputs[0], 1); if (s == B_LAST_BUFFER_ERROR) doneA = true; else Check(s, "decode A"); }
			if (!doneB) { status_t s = b.Next(outputs[1], 1); if (s == B_LAST_BUFFER_ERROR) doneB = true; else Check(s, "decode B"); }
		}
		Require(a.frames == fa.packets.size() && b.frames == fb.packets.size(), "both streams completely drained");
		printf("interleaved: %u + %u frames in %lld us\n", a.frames, b.frames, (long long)(system_time() - start));
		size_t calls = a.provider->calls;
		Require(a.Next(NULL, 0) == B_LAST_BUFFER_ERROR && a.provider->calls == calls, "EOF remains drained");
		a.Seek(0); a.Negotiate((color_space)0x49343230);
		for (size_t i = 0; i < fa.packets.size(); i++) Check(a.Next(outputs[0], 2), "I420 after EOF seek");
		Require(a.Next(NULL, 0) == B_LAST_BUFFER_ERROR, "I420 fully drained");
		size_t key = 1; while (key < fa.packets.size() && !fa.packets[key].key) key++;
		Require(key < fa.packets.size(), "second keyframe fixture");
		a.Seek(key); a.Negotiate((color_space)0x4e563132);
		for (size_t i = key; i < fa.packets.size(); i++) Check(a.Next(outputs[0], 3), "nonzero keyframe seek");
		Require(a.Next(NULL, 0) == B_LAST_BUFFER_ERROR, "seek tail drained");
		a.Seek(0); a.Negotiate(B_YCbCr422);
		for (size_t i = 0; i < fa.packets.size(); i++) Check(a.Next(outputs[0], 4), "packed YUV");
		a.Seek(0); a.provider->corrupt = true;
		Require(a.Next(NULL, 0) == B_BAD_DATA, "truncated packet rejected");
		Require(a.Next(NULL, 0) == B_BAD_DATA, "decoder remains failed until setup");
		a.provider->corrupt = false; a.provider->at = 0;
		Check(a.Setup(), "setup after failure"); a.Negotiate((color_space)0x4e563132);
		for (size_t i = 0; i < fa.packets.size(); i++) Check(a.Next(outputs[0], 5), "fresh session after failure");
	}
	delete plugin; unload_add_on(image);
	for (auto f : outputs) Require(fclose(f) == 0, "close output");
	puts("PASS: owned predictive decoders, output order/timestamps, EOF, seeks, interruption, formats, packet failure and fresh reuse; compare saved pixels independently");
}
