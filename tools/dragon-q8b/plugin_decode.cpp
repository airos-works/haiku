/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	Lab test of the qcom_video media add-on itself (what airTime and the
	Media Kit drive), fed from an IVF file without the Media Kit's readers:
	the add-on's decoder 1 decodes the frames, each picture's FNV-1a hash
	(I420) is printed or compared, as iris_decode does with IrisDecoder.

	plugin_decode add-on stream.ivf [reference.hashes] */


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include <MediaFormats.h>
#include <OS.h>
#include <String.h>
#include <image.h>

#include <DecoderPlugin.h>


static uint64
hash(const uint8* data, size_t size)
{
	uint64 value = 0xcbf29ce484222325ULL;
	for (size_t i = 0; i < size; i++) {
		value ^= data[i];
		value *= 0x100000001b3ULL;
	}
	return value;
}


class IvfChunks : public BPrivate::media::ChunkProvider {
public:
	IvfChunks(const std::vector<uint8>& data)
		:
		fData(data),
		fOffset(32),
		fIndex(0)
	{
	}

	status_t GetNextChunk(const void** chunk, size_t* size,
		media_header* header)
	{
		if (fOffset + 12 > fData.size())
			return B_LAST_BUFFER_ERROR;
		size_t length = fData[fOffset] | (fData[fOffset + 1] << 8)
			| (fData[fOffset + 2] << 16) | ((size_t)fData[fOffset + 3] << 24);
		fOffset += 12;
		if (fOffset + length > fData.size())
			return B_LAST_BUFFER_ERROR;
		*chunk = &fData[fOffset];
		*size = length;
		memset(header, 0, sizeof(*header));
		header->type = B_MEDIA_ENCODED_VIDEO;
		header->start_time = fIndex * 40000;
		header->size_used = length;
		fOffset += length;
		fIndex++;
		return B_OK;
	}

private:
	const std::vector<uint8>&	fData;
	size_t						fOffset;
	int64						fIndex;
};


int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s add-on stream.ivf [reference.hashes]\n",
			argv[0]);
		return 1;
	}
	FILE* file = fopen(argv[2], "rb");
	if (file == NULL)
		return 1;
	std::vector<uint8> data;
	uint8 block[65536];
	size_t got;
	while ((got = fread(block, 1, sizeof(block), file)) > 0)
		data.insert(data.end(), block, block + got);
	fclose(file);
	if (data.size() < 32 || memcmp(data.data(), "DKIF", 4) != 0)
		return 1;
	uint32 width = data[12] | (data[13] << 8);
	uint32 height = data[14] | (data[15] << 8);
	uint32 codec = memcmp(&data[8], "MPG2", 4) == 0 ? 2 : 167;

	std::vector<uint64> references;
	if (argc > 3) {
		FILE* hashes = fopen(argv[3], "r");
		char line[128];
		while (hashes != NULL && fgets(line, sizeof(line), hashes) != NULL) {
			unsigned index;
			unsigned long long value;
			if (sscanf(line, "hash %u %llx", &index, &value) == 2)
				references.push_back(value);
		}
		if (hashes != NULL)
			fclose(hashes);
	}

	// LOAD_ALL: first every other add-on of the system's, as the Media Kit
	// and airTime have them loaded
	if (getenv("LOAD_ALL") != NULL) {
		const char* kOthers[] = { "00_rockchip_mpp", "00_sunxi_cedar",
			"ffmpeg", "rpi_hevc", "rpi_mmal", "raw_decoder" };
		for (const char* name : kOthers) {
			BString path("/boot/system/add-ons/media/plugins/");
			path << name;
			image_id other = load_add_on(path.String());
			MediaPlugin* (*make)() = NULL;
			if (other >= 0 && get_image_symbol(other, "instantiate_plugin",
					B_SYMBOL_TYPE_TEXT, (void**)&make) == B_OK) {
				MediaPlugin* instance = make();
				printf("loaded %s\n", name);
				(void)instance;
			}
		}
	}

	// HOG_MB: that much memory in use first, so the codec's buffers come
	// from elsewhere in physical memory
	if (getenv("HOG_MB") != NULL) {
		size_t size = (size_t)atoi(getenv("HOG_MB")) << 20;
		uint8* hog = (uint8*)malloc(size);
		for (size_t i = 0; hog != NULL && i < size; i += 4096)
			hog[i] = 1;
		printf("holding %zu MB\n", size >> 20);
	}

	image_id image = load_add_on(argv[1]);
	MediaPlugin* (*instantiate)() = NULL;
	if (image < 0 || get_image_symbol(image, "instantiate_plugin",
			B_SYMBOL_TYPE_TEXT, (void**)&instantiate) != B_OK) {
		fprintf(stderr, "cannot load %s\n", argv[1]);
		return 1;
	}
	DecoderPlugin* plugin = dynamic_cast<DecoderPlugin*>(instantiate());
	Decoder* decoder = plugin != NULL ? plugin->NewDecoder(
		getenv("PLUGIN_INDEX") != NULL ? atoi(getenv("PLUGIN_INDEX")) : 1) : NULL;
	if (decoder == NULL)
		return 1;
	decoder->SetChunkProvider(new IvfChunks(data));

	media_format_description description;
	description.family = B_MISC_FORMAT_FAMILY;
	description.u.misc.file_format = 'ffmp';
	description.u.misc.codec = codec;
	media_format format;
	BMediaFormats formats;
	if (formats.GetFormatFor(description, &format) != B_OK) {
		format.Clear();
		format.type = B_MEDIA_ENCODED_VIDEO;
		formats.MakeFormatFor(&description, 1, &format);
	}
	format.type = B_MEDIA_ENCODED_VIDEO;
	format.u.encoded_video.output.display.line_width = width;
	format.u.encoded_video.output.display.line_count = height;
	status_t status = decoder->Setup(&format, NULL, 0);
	printf("%ux%u, codec %u: setup %s\n", width, height, codec,
		strerror(status));
	if (status != B_OK)
		return 1;

	media_format output;
	output.Clear();
	output.type = B_MEDIA_RAW_VIDEO;
	output.u.raw_video = media_raw_video_format::wildcard;
	output.u.raw_video.display.format = (color_space)0x49343230;	// 'I420'
	status = decoder->NegotiateOutputFormat(&output);
	if (status != B_OK) {
		printf("negotiation: %s\n", strerror(status));
		return 1;
	}
	uint32 stride = output.u.raw_video.display.bytes_per_row;
	uint32 rows = output.u.raw_video.display.line_count;
	uint32 columns = output.u.raw_video.display.line_width;
	std::vector<uint8> buffer((size_t)stride * rows * 2);
	std::vector<uint8> frame((size_t)columns * rows * 3 / 2);

	uint32 pictures = 0, identical = 0;
	bigtime_t start = system_time();
	while (true) {
		int64 count = 0;
		media_header header;
		media_decode_info info;
		status = decoder->Decode(buffer.data(), &count, &header, &info);
		if (status != B_OK)
			break;
		// unpadded I420, as the reference hashes are
		uint8* target = frame.data();
		for (uint32 y = 0; y < rows; y++, target += columns)
			memcpy(target, &buffer[(size_t)y * stride], columns);
		const uint8* chroma = &buffer[(size_t)stride * rows];
		for (uint32 plane = 0; plane < 2; plane++) {
			for (uint32 y = 0; y < rows / 2; y++, target += columns / 2) {
				memcpy(target, chroma + (size_t)y * (stride / 2),
					columns / 2);
			}
			chroma += (size_t)(stride / 2) * (rows / 2);
		}
		uint64 value = hash(frame.data(), frame.size());
		if (pictures < references.size() && references[pictures] == value)
			identical++;
		pictures++;
	}
	printf("%u pictures in %" B_PRId64 " ms, ended with %s; %u of %zu "
		"identical\n", pictures, (system_time() - start) / 1000,
		strerror(status), identical, references.size());
	delete decoder;
	return 0;
}
