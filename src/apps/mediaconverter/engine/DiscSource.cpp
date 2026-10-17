/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "DiscSource.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mem.h>
}


static const int kSectorSize = 2048;
// Titles shorter than this are menus, logos and warnings.
static const bigtime_t kMinimumTitle = 10 * 1000000LL;


static uint16
be16(const uint8* data)
{
	return (uint16)(data[0] << 8 | data[1]);
}


static uint32
be32(const uint8* data)
{
	return (uint32)data[0] << 24 | (uint32)data[1] << 16
		| (uint32)data[2] << 8 | data[3];
}


static uint32
le32(const uint8* data)
{
	return (uint32)data[3] << 24 | (uint32)data[2] << 16
		| (uint32)data[1] << 8 | data[0];
}


static bool
read_at(const char* path, off_t offset, void* buffer, size_t size)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return false;
	ssize_t done = pread(fd, buffer, size, offset);
	close(fd);
	return done == (ssize_t)size;
}


static bool
is_directory(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}


/*!	The entry of \a directory named \a name in any case, or "". */
static BString
find_entry(const char* directory, const char* name)
{
	BString found;
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return found;
	while (dirent* entry = readdir(dir)) {
		if (strcasecmp(entry->d_name, name) == 0) {
			found.SetToFormat("%s/%s", directory, entry->d_name);
			break;
		}
	}
	closedir(dir);
	return found;
}


static BString
leaf_of(const char* path)
{
	BString leaf(path);
	while (leaf.EndsWith("/") && leaf.Length() > 1)
		leaf.Truncate(leaf.Length() - 1);
	int32 slash = leaf.FindLast('/');
	if (slash >= 0)
		leaf.Remove(0, slash + 1);
	return leaf;
}


static BString
parent_of(const char* path)
{
	BString parent(path);
	while (parent.EndsWith("/") && parent.Length() > 1)
		parent.Truncate(parent.Length() - 1);
	int32 slash = parent.FindLast('/');
	if (slash > 0)
		parent.Truncate(slash);
	else if (slash == 0)
		parent = "/";
	else
		parent = ".";
	return parent;
}


struct LanguagePair {
	const char*	two;
	const char*	three;
};

// the disc's ISO 639-1 codes as the muxers want them (639-2/B)
static const LanguagePair kLanguagePairs[] = {
	{ "en", "eng" }, { "fr", "fre" }, { "de", "ger" }, { "es", "spa" },
	{ "it", "ita" }, { "pt", "por" }, { "nl", "dut" }, { "sv", "swe" },
	{ "no", "nor" }, { "nb", "nob" }, { "da", "dan" }, { "fi", "fin" },
	{ "is", "ice" }, { "pl", "pol" }, { "cs", "cze" }, { "sk", "slo" },
	{ "hu", "hun" }, { "ro", "rum" }, { "bg", "bul" }, { "el", "gre" },
	{ "tr", "tur" }, { "ru", "rus" }, { "uk", "ukr" }, { "sr", "srp" },
	{ "hr", "hrv" }, { "sl", "slv" }, { "he", "heb" }, { "iw", "heb" },
	{ "ar", "ara" }, { "fa", "per" }, { "hi", "hin" }, { "bn", "ben" },
	{ "ta", "tam" }, { "te", "tel" }, { "th", "tha" }, { "vi", "vie" },
	{ "id", "ind" }, { "in", "ind" }, { "ms", "may" }, { "tl", "fil" },
	{ "zh", "chi" }, { "ja", "jpn" }, { "ko", "kor" }, { "ca", "cat" },
	{ "eu", "baq" }, { "gl", "glg" }, { "cy", "wel" }, { "ga", "gle" },
	{ "la", "lat" }, { "et", "est" }, { "lv", "lav" }, { "lt", "lit" },
	{ "mi", "mao" },
};


static BString
language_from_disc(const uint8* code)
{
	char two[3] = { (char)tolower(code[0]), (char)tolower(code[1]), 0 };
	for (size_t i = 0; i < sizeof(kLanguagePairs) / sizeof(kLanguagePairs[0]);
			i++) {
		if (strcmp(two, kLanguagePairs[i].two) == 0)
			return BString(kLanguagePairs[i].three);
	}
	return BString();
}


// #pragma mark - the files of a DVD


/*!	A run of bytes in a file: a whole VOB in a folder, or a VOB's extent in
	an image. */
struct FileRun {
	BString		path;
	off_t		offset;
	off_t		length;
};


/*!	The VIDEO_TS files of a DVD folder, or of a DVD image through its
	ISO 9660 directory (DVD-Video discs carry one beside their UDF one). */
class DvdFiles {
public:
	status_t	Open(const char* path);
	bool		Find(const char* name, FileRun* run) const;
	status_t	Read(const char* name, std::vector<uint8>& data) const;

private:
	status_t	_OpenImage(const char* path);
	status_t	_OpenFolder(const char* path);

	std::map<BString, FileRun> fFiles;	// by upper case name
};


status_t
DvdFiles::Open(const char* path)
{
	fFiles.clear();
	return is_directory(path) ? _OpenFolder(path) : _OpenImage(path);
}


status_t
DvdFiles::_OpenFolder(const char* path)
{
	BString folder = leaf_of(path).ICompare("VIDEO_TS") == 0 ? BString(path)
		: find_entry(path, "VIDEO_TS");
	if (folder.IsEmpty() || !is_directory(folder.String()))
		return B_ENTRY_NOT_FOUND;
	DIR* dir = opendir(folder.String());
	if (dir == NULL)
		return B_ENTRY_NOT_FOUND;
	while (dirent* entry = readdir(dir)) {
		FileRun run;
		run.path.SetToFormat("%s/%s", folder.String(), entry->d_name);
		struct stat st;
		if (stat(run.path.String(), &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		run.offset = 0;
		run.length = st.st_size;
		BString name(entry->d_name);
		name.ToUpper();
		fFiles[name] = run;
	}
	closedir(dir);
	return fFiles.count("VIDEO_TS.IFO") != 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


/*!	Calls \a function with each record of an ISO 9660 directory. */
template<typename Function>
static bool
read_iso_directory(const char* image, uint32 extent, uint32 size,
	Function function)
{
	if (size == 0 || size > 4 * 1024 * 1024)
		return false;
	std::vector<uint8> data(size);
	if (!read_at(image, (off_t)extent * kSectorSize, data.data(), size))
		return false;
	uint32 i = 0;
	while (i < size) {
		uint8 length = data[i];
		if (length == 0) {
			// records do not cross sectors
			i = (i / kSectorSize + 1) * kSectorSize;
			continue;
		}
		if (i + length > size || length < 34)
			break;
		const uint8* record = &data[i];
		uint8 nameLength = record[32];
		if (33 + nameLength <= length) {
			BString name((const char*)record + 33, nameLength);
			int32 version = name.FindFirst(';');
			if (version >= 0)
				name.Truncate(version);
			function(name, le32(record + 2), le32(record + 10),
				(record[25] & 2) != 0);
		}
		i += length;
	}
	return true;
}


status_t
DvdFiles::_OpenImage(const char* path)
{
	uint8 descriptor[kSectorSize];
	if (!read_at(path, 16 * kSectorSize, descriptor, sizeof(descriptor))
		|| descriptor[0] != 1 || memcmp(descriptor + 1, "CD001", 5) != 0)
		return B_ENTRY_NOT_FOUND;
	const uint8* root = descriptor + 156;
	uint32 folderExtent = 0;
	uint32 folderSize = 0;
	read_iso_directory(path, le32(root + 2), le32(root + 10),
		[&](const BString& name, uint32 extent, uint32 size, bool directory) {
			if (directory && name.ICompare("VIDEO_TS") == 0) {
				folderExtent = extent;
				folderSize = size;
			}
		});
	if (folderExtent == 0)
		return B_ENTRY_NOT_FOUND;
	read_iso_directory(path, folderExtent, folderSize,
		[&](const BString& name, uint32 extent, uint32 size, bool directory) {
			if (directory || name.IsEmpty() || name[0] < ' ')
				return;
			FileRun run;
			run.path = path;
			run.offset = (off_t)extent * kSectorSize;
			run.length = size;
			BString upper(name);
			upper.ToUpper();
			fFiles[upper] = run;
		});
	return fFiles.count("VIDEO_TS.IFO") != 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


bool
DvdFiles::Find(const char* name, FileRun* run) const
{
	std::map<BString, FileRun>::const_iterator found = fFiles.find(name);
	if (found == fFiles.end())
		return false;
	*run = found->second;
	return true;
}


status_t
DvdFiles::Read(const char* name, std::vector<uint8>& data) const
{
	FileRun run;
	if (!Find(name, &run) || run.length > 16 * 1024 * 1024)
		return B_ENTRY_NOT_FOUND;
	data.resize(run.length);
	return read_at(run.path.String(), run.offset, data.data(), run.length)
		? B_OK : B_IO_ERROR;
}


// #pragma mark - DVD titles


struct DiscStream {
	int			id;			// libavformat's stream id
	BString		language;
	bool		commentary;
	bool		forced;
};


struct DvdTitleMap {
	DiscTitle	title;
	int			vts;
	// byte ranges in the title set's VOBs, in playing order
	std::vector<std::pair<int64, int64> > ranges;
	std::vector<bigtime_t> chapters;
	std::vector<DiscStream> audio;
	std::vector<DiscStream> subtitles;
	uint32		palette[16];	// 0x00YYCrCb
	int			width;
	int			height;
};


static int
bcd(uint8 value)
{
	return (value >> 4) * 10 + (value & 0xf);
}


static bigtime_t
dvd_time(const uint8* time)
{
	double rate = (time[3] >> 6) == 1 ? 25.0 : 30000.0 / 1001.0;
	return (bigtime_t)(bcd(time[0]) * 3600 + bcd(time[1]) * 60
		+ bcd(time[2])) * 1000000LL
		+ (bigtime_t)(bcd(time[3] & 0x3f) * 1000000.0 / rate);
}


/*!	One title of a title set: its program chains' cells (the first angle of
	an angle block), chapter starts, streams and subpicture palette. */
static bool
dvd_map_title(const std::vector<uint8>& ifo, int ttn, DvdTitleMap& map)
{
	size_t size = ifo.size();
	if (size < 0x300 || memcmp(ifo.data(), "DVDVIDEO-VTS", 12) != 0)
		return false;
	const uint8* data = ifo.data();
	size_t pttTable = (size_t)be32(data + 0xc8) * kSectorSize;
	size_t pgcTable = (size_t)be32(data + 0xcc) * kSectorSize;
	if (pttTable + 8 > size || pgcTable + 8 > size)
		return false;

	// the title's chapters: (program chain, program) pairs
	uint16 titleCount = be16(data + pttTable);
	if (ttn < 1 || ttn > titleCount
		|| pttTable + 8 + 4 * titleCount > size)
		return false;
	size_t pttEnd = pttTable + be32(data + pttTable + 4) + 1;
	size_t pttStart = pttTable + be32(data + pttTable + 8 + 4 * (ttn - 1));
	size_t pttStop = ttn < titleCount
		? pttTable + be32(data + pttTable + 8 + 4 * ttn) : pttEnd;
	if (pttStop > size || pttStart >= pttStop)
		return false;
	std::vector<std::pair<int, int> > parts;
	for (size_t at = pttStart; at + 4 <= pttStop; at += 4)
		parts.push_back(std::make_pair(be16(data + at), be16(data + at + 2)));
	if (parts.empty())
		return false;

	uint16 chainCount = be16(data + pgcTable);
	if (pgcTable + 8 + 8 * chainCount > size)
		return false;
	std::vector<int> chains;
	for (size_t i = 0; i < parts.size(); i++) {
		if (std::find(chains.begin(), chains.end(), parts[i].first)
				== chains.end())
			chains.push_back(parts[i].first);
	}

	// each chain's cells, and where its programs start
	std::map<std::pair<int, int>, bigtime_t> programStarts;
	bigtime_t elapsed = 0;
	int64 lastEnd = -1;
	for (size_t c = 0; c < chains.size(); c++) {
		int number = chains[c];
		if (number < 1 || number > chainCount)
			return false;
		size_t chain = pgcTable + be32(data + pgcTable + 8 + 8 * (number - 1)
			+ 4);
		if (chain + 0xec > size)
			return false;
		const uint8* pgc = data + chain;
		int programs = pgc[2];
		int cells = pgc[3];
		size_t programMap = chain + be16(pgc + 0xe6);
		size_t cellTable = chain + be16(pgc + 0xe8);
		if (programMap + programs > size || cellTable + 24 * cells > size)
			return false;
		if (c == 0) {
			for (int i = 0; i < 16; i++)
				map.palette[i] = be32(pgc + 0xa4 + 4 * i);
			// the streams this chain plays: their numbers in the VOBs
			int audioCount = std::min((int)be16(data + 0x202), 8);
			for (int i = 0; i < audioCount; i++) {
				uint16 control = be16(pgc + 0x0c + 2 * i);
				if ((control & 0x8000) == 0)
					continue;
				const uint8* attributes = data + 0x204 + 8 * i;
				int number = (control >> 8) & 7;
				DiscStream stream;
				switch (attributes[0] >> 5) {
					case 0: stream.id = 0x80 + number; break;	// AC-3
					case 2:
					case 3: stream.id = 0x1c0 + number; break;	// MPEG
					case 4: stream.id = 0xa0 + number; break;	// LPCM
					case 6: stream.id = 0x88 + number; break;	// DTS
					default: continue;
				}
				if (((attributes[0] >> 2) & 3) == 1)
					stream.language = language_from_disc(attributes + 2);
				stream.commentary = attributes[5] == 3 || attributes[5] == 4;
				stream.forced = false;
				map.audio.push_back(stream);
			}
			bool wide = ((be16(data + 0x200) >> 10) & 3) == 3;
			int subtitleCount = std::min((int)be16(data + 0x254), 32);
			for (int i = 0; i < subtitleCount; i++) {
				uint32 control = be32(pgc + 0x1c + 4 * i);
				if ((control & 0x80000000) == 0)
					continue;
				const uint8* attributes = data + 0x256 + 6 * i;
				DiscStream stream;
				stream.id = 0x20 + ((wide ? control >> 16 : control >> 24)
					& 0x1f);
				if (((attributes[0] >> 2) & 3) == 1)
					stream.language = language_from_disc(attributes + 2);
				stream.commentary = attributes[5] >= 13;
				stream.forced = attributes[5] == 9;
				bool known = false;
				for (size_t j = 0; j < map.subtitles.size(); j++)
					known |= map.subtitles[j].id == stream.id;
				if (!known)
					map.subtitles.push_back(stream);
			}
		}

		std::vector<bigtime_t> cellStarts(cells + 1, elapsed);
		for (int i = 0; i < cells; i++) {
			const uint8* cell = data + cellTable + 24 * i;
			int blockMode = (cell[0] >> 6) & 3;
			int blockType = (cell[0] >> 4) & 3;
			cellStarts[i] = elapsed;
			// another angle of the cell before
			if (blockType == 1 && blockMode != 1)
				continue;
			bigtime_t length = dvd_time(cell + 4);
			uint32 first = be32(cell + 8);
			uint32 last = be32(cell + 20);
			if (last < first)
				continue;
			int64 start = (int64)first * kSectorSize;
			int64 end = ((int64)last + 1) * kSectorSize;
			if (start == lastEnd && !map.ranges.empty())
				map.ranges.back().second = end;
			else
				map.ranges.push_back(std::make_pair(start, end));
			lastEnd = end;
			elapsed += length;
		}
		for (int p = 1; p <= programs; p++) {
			int entryCell = pgc[be16(pgc + 0xe6) + p - 1];
			if (entryCell >= 1 && entryCell <= cells)
				programStarts[std::make_pair(number, p)]
					= cellStarts[entryCell - 1];
		}
	}

	for (size_t i = 0; i < parts.size(); i++) {
		std::map<std::pair<int, int>, bigtime_t>::iterator found
			= programStarts.find(parts[i]);
		if (found != programStarts.end()
			&& (map.chapters.empty() || found->second > map.chapters.back()))
			map.chapters.push_back(found->second);
	}
	map.title.duration = elapsed;
	map.title.chapters = map.chapters.size();
	uint16 video = be16(data + 0x200);
	map.width = 720;
	map.height = ((video >> 12) & 3) == 1 ? 576 : 480;
	return !map.ranges.empty();
}


static status_t
dvd_read_titles(const DvdFiles& files, std::vector<DvdTitleMap>& titles,
	BString* error)
{
	std::vector<uint8> manager;
	if (files.Read("VIDEO_TS.IFO", manager) != B_OK || manager.size() < 0x100
		|| memcmp(manager.data(), "DVDVIDEO-VMG", 12) != 0) {
		if (error != NULL)
			*error = "The DVD's VIDEO_TS.IFO cannot be read.";
		return B_BAD_DATA;
	}
	size_t table = (size_t)be32(manager.data() + 0xc4) * kSectorSize;
	if (table + 8 > manager.size()) {
		if (error != NULL)
			*error = "The DVD has no title table.";
		return B_BAD_DATA;
	}
	int count = be16(manager.data() + table);
	std::map<int, std::vector<uint8> > sets;
	for (int i = 0; i < count; i++) {
		size_t at = table + 8 + 12 * i;
		if (at + 12 > manager.size())
			break;
		const uint8* entry = manager.data() + at;
		DvdTitleMap map;
		map.title.number = i + 1;
		map.title.angles = std::max(1, (int)entry[1]);
		map.vts = entry[6];
		if (sets.count(map.vts) == 0) {
			BString name;
			name.SetToFormat("VTS_%02d_0.IFO", map.vts);
			if (files.Read(name.String(), sets[map.vts]) != B_OK)
				continue;
		}
		if (dvd_map_title(sets[map.vts], entry[7], map))
			titles.push_back(map);
	}
	if (titles.empty()) {
		if (error != NULL)
			*error = "The DVD's titles cannot be read.";
		return B_BAD_DATA;
	}
	return B_OK;
}


// #pragma mark - reading a DVD title


/*!	libavformat's view of a title: its cells one after the other, read from
	the title set's VOBs. */
struct DiscIO {
	virtual			~DiscIO() {}
	virtual	int64	Size() const = 0;
};


struct DvdReader : DiscIO {
	std::vector<FileRun>	vobs;		// VTS_nn_1.VOB and on
	std::vector<int64>		vobStarts;
	std::vector<std::pair<int64, int64> > ranges;
	std::vector<int64>		rangeStarts;
	int64					size;
	int64					position;
	std::map<BString, int>	files;

	virtual int64 Size() const
	{
		return size;
	}

	virtual ~DvdReader()
	{
		for (std::map<BString, int>::iterator i = files.begin();
				i != files.end(); i++)
			close(i->second);
	}

	int File(const BString& path)
	{
		std::map<BString, int>::iterator found = files.find(path);
		if (found != files.end())
			return found->second;
		int fd = open(path.String(), O_RDONLY);
		if (fd >= 0)
			files[path] = fd;
		return fd;
	}

	// bytes at an offset in the title set's VOBs
	ssize_t ReadVobs(int64 offset, uint8* buffer, size_t size)
	{
		size_t index = std::upper_bound(vobStarts.begin(), vobStarts.end(),
			offset) - vobStarts.begin();
		if (index == 0 || index > vobs.size())
			return -1;
		const FileRun& run = vobs[index - 1];
		int64 inRun = offset - vobStarts[index - 1];
		if (inRun >= run.length)
			return 0;
		size = std::min((int64)size, run.length - inRun);
		int fd = File(run.path);
		if (fd < 0)
			return -1;
		return pread(fd, buffer, size, run.offset + inRun);
	}
};


static int
dvd_read(void* opaque, uint8_t* buffer, int size)
{
	DvdReader* reader = (DvdReader*)opaque;
	int done = 0;
	while (done < size && reader->position < reader->size) {
		size_t index = std::upper_bound(reader->rangeStarts.begin(),
			reader->rangeStarts.end(), reader->position)
			- reader->rangeStarts.begin() - 1;
		const std::pair<int64, int64>& range = reader->ranges[index];
		int64 inRange = reader->position - reader->rangeStarts[index];
		int64 count = std::min((int64)(size - done),
			range.second - range.first - inRange);
		ssize_t read = reader->ReadVobs(range.first + inRange, buffer + done,
			count);
		if (read <= 0)
			return done > 0 ? done : AVERROR(EIO);
		done += read;
		reader->position += read;
	}
	return done > 0 ? done : AVERROR_EOF;
}


static int64_t
dvd_seek(void* opaque, int64_t offset, int whence)
{
	DvdReader* reader = (DvdReader*)opaque;
	if ((whence & AVSEEK_SIZE) != 0)
		return reader->size;
	switch (whence & ~AVSEEK_FORCE) {
		case SEEK_SET: break;
		case SEEK_CUR: offset += reader->position; break;
		case SEEK_END: offset += reader->size; break;
		default: return AVERROR(EINVAL);
	}
	if (offset < 0)
		return AVERROR(EINVAL);
	reader->position = offset;
	return offset;
}


/*!	Whether the title's first packs are CSS scrambled (the PES scrambling
	control bits of its video and private streams). */
static bool
dvd_is_scrambled(DvdReader* reader)
{
	uint8 sector[kSectorSize];
	reader->position = 0;
	for (int i = 0; i < 64; i++) {
		if (dvd_read(reader, sector, kSectorSize) != kSectorSize)
			break;
		if (be32(sector) != 0x1ba)
			continue;
		size_t at = 14 + (sector[13] & 7);
		while (at + 7 < kSectorSize && (be32(sector + at) >> 8) == 1) {
			uint8 id = sector[at + 3];
			size_t length = be16(sector + at + 4);
			if ((id >= 0xe0 && id <= 0xef) || id == 0xbd
				|| (id >= 0xc0 && id <= 0xdf)) {
				if ((sector[at + 6] & 0xc0) == 0x80
					&& (sector[at + 6] & 0x30) != 0) {
					reader->position = 0;
					return true;
				}
			}
			at += 6 + length;
		}
	}
	reader->position = 0;
	return false;
}


/*!	Chapters on the packets' clock: the title's own times plus where its
	timestamps start (a DVD's first picture is at 0.28 s or so). */
static void
add_chapters(AVFormatContext* context, const std::vector<bigtime_t>& starts,
	bigtime_t duration)
{
	bigtime_t base = context->start_time != AV_NOPTS_VALUE
		? std::max(context->start_time, (int64_t)0) : 0;
	for (size_t i = 0; i < starts.size(); i++) {
		AVChapter* chapter = (AVChapter*)av_mallocz(sizeof(AVChapter));
		if (chapter == NULL)
			return;
		chapter->id = i + 1;
		chapter->time_base = AV_TIME_BASE_Q;
		chapter->start = base + starts[i];
		chapter->end = base + (i + 1 < starts.size() ? starts[i + 1]
			: duration);
		char title[32];
		snprintf(title, sizeof(title), "Chapter %d", (int)i + 1);
		av_dict_set(&chapter->metadata, "title", title, 0);
		int count = context->nb_chapters;
		av_dynarray_add(&context->chapters, &count, chapter);
		if (context->chapters == NULL)
			return;
		context->nb_chapters = count;
	}
}


static void
label_stream(AVStream* stream, const DiscStream& disc)
{
	if (!disc.language.IsEmpty())
		av_dict_set(&stream->metadata, "language", disc.language.String(), 0);
	if (disc.commentary) {
		stream->disposition |= AV_DISPOSITION_COMMENT;
		av_dict_set(&stream->metadata, "title", "Commentary", 0);
	}
	if (disc.forced)
		stream->disposition |= AV_DISPOSITION_FORCED;
}


/*!	FFmpeg's DVD subtitle decoder takes the palette (RGB) and picture size
	from its extradata, as VobSub's .idx has them. */
static void
set_subtitle_palette(AVStream* stream, const DvdTitleMap& map)
{
	BString text;
	text.SetToFormat("size: %dx%d\npalette: ", map.width, map.height);
	for (int i = 0; i < 16; i++) {
		int y = (map.palette[i] >> 16) & 0xff;
		int cr = (map.palette[i] >> 8) & 0xff;
		int cb = map.palette[i] & 0xff;
		int r = (int)(y + 1.402 * (cr - 128));
		int g = (int)(y - 0.344136 * (cb - 128) - 0.714136 * (cr - 128));
		int b = (int)(y + 1.772 * (cb - 128));
		r = std::max(0, std::min(255, r));
		g = std::max(0, std::min(255, g));
		b = std::max(0, std::min(255, b));
		char color[16];
		snprintf(color, sizeof(color), "%s%02x%02x%02x", i > 0 ? ", " : "",
			r, g, b);
		text << color;
	}
	text << "\n";
	AVCodecParameters* parameters = stream->codecpar;
	av_freep(&parameters->extradata);
	parameters->extradata = (uint8*)av_mallocz(text.Length()
		+ AV_INPUT_BUFFER_PADDING_SIZE);
	if (parameters->extradata == NULL) {
		parameters->extradata_size = 0;
		return;
	}
	memcpy(parameters->extradata, text.String(), text.Length());
	parameters->extradata_size = text.Length();
}


static int
open_dvd(AVFormatContext** context, const char* path, int32 number,
	BString* error)
{
	DvdFiles files;
	std::vector<DvdTitleMap> titles;
	if (files.Open(path) != B_OK) {
		if (error != NULL)
			*error = "This is not a DVD.";
		return AVERROR(ENOENT);
	}
	if (dvd_read_titles(files, titles, error) != B_OK)
		return AVERROR_INVALIDDATA;
	const DvdTitleMap* map = NULL;
	for (size_t i = 0; i < titles.size(); i++) {
		if (titles[i].title.number == number
			|| (number < 0 && (map == NULL
				|| titles[i].title.duration > map->title.duration)))
			map = &titles[i];
	}
	if (map == NULL) {
		if (error != NULL)
			error->SetToFormat("The DVD has no title %d.", (int)number);
		return AVERROR(ENOENT);
	}

	DvdReader* reader = new DvdReader;
	int64 start = 0;
	for (int i = 1; i <= 9; i++) {
		BString name;
		name.SetToFormat("VTS_%02d_%d.VOB", map->vts, i);
		FileRun run;
		if (!files.Find(name.String(), &run))
			break;
		reader->vobs.push_back(run);
		reader->vobStarts.push_back(start);
		start += run.length;
	}
	int64 size = 0;
	for (size_t i = 0; i < map->ranges.size(); i++) {
		// cells past the VOBs' end (a damaged copy) are left out
		std::pair<int64, int64> range = map->ranges[i];
		range.second = std::min(range.second, start);
		if (range.second <= range.first)
			continue;
		reader->ranges.push_back(range);
		reader->rangeStarts.push_back(size);
		size += range.second - range.first;
	}
	reader->size = size;
	reader->position = 0;
	if (reader->vobs.empty() || size == 0) {
		delete reader;
		if (error != NULL)
			*error = "The DVD title's video files are missing.";
		return AVERROR(ENOENT);
	}
	if (dvd_is_scrambled(reader)) {
		delete reader;
		if (error != NULL) {
			*error = "This DVD is encrypted (CSS), which MediaConverter "
				"cannot read.";
		}
		return AVERROR(EACCES);
	}

	const int kBufferSize = 64 * 1024;
	uint8* buffer = (uint8*)av_malloc(kBufferSize);
	AVIOContext* io = buffer != NULL ? avio_alloc_context(buffer, kBufferSize,
		0, reader, dvd_read, NULL, dvd_seek) : NULL;
	AVFormatContext* format = avformat_alloc_context();
	if (io == NULL || format == NULL) {
		if (io == NULL)
			av_free(buffer);
		else {
			av_freep(&io->buffer);
			avio_context_free(&io);
		}
		avformat_free_context(format);
		delete reader;
		return AVERROR(ENOMEM);
	}
	format->pb = io;
	format->opaque = reader;
	// a DVD's streams can start late: look further than for a file
	format->probesize = 64 * 1024 * 1024;
	format->max_analyze_duration = 15 * AV_TIME_BASE;
	int result = avformat_open_input(&format, NULL,
		av_find_input_format("mpeg"), NULL);
	if (result < 0) {
		// avformat_open_input() freed the context, not the I/O
		av_freep(&io->buffer);
		avio_context_free(&io);
		delete reader;
		if (error != NULL)
			*error = "The DVD title cannot be read.";
		return result;
	}

	// subtitles appear when someone first speaks; the disc says which
	// there are, and the demuxer puts packets in streams by their id
	for (size_t i = 0; i < map->subtitles.size(); i++) {
		AVStream* stream = avformat_new_stream(format, NULL);
		if (stream == NULL)
			break;
		stream->id = map->subtitles[i].id;
		stream->time_base = (AVRational){ 1, 90000 };
		stream->codecpar->codec_type = AVMEDIA_TYPE_SUBTITLE;
		stream->codecpar->codec_id = AV_CODEC_ID_DVD_SUBTITLE;
	}
	result = avformat_find_stream_info(format, NULL);
	if (result < 0) {
		close_source(&format);
		if (error != NULL)
			*error = "The DVD title's streams cannot be read.";
		return result;
	}

	for (unsigned i = 0; i < format->nb_streams; i++) {
		AVStream* stream = format->streams[i];
		for (size_t j = 0; j < map->audio.size(); j++) {
			if (map->audio[j].id == stream->id)
				label_stream(stream, map->audio[j]);
		}
		for (size_t j = 0; j < map->subtitles.size(); j++) {
			if (map->subtitles[j].id == stream->id) {
				label_stream(stream, map->subtitles[j]);
				set_subtitle_palette(stream, *map);
			}
		}
	}
	format->duration = map->title.duration;
	add_chapters(format, map->chapters, map->title.duration);
	av_dict_set(&format->metadata, "title", disc_name(path).String(), 0);
	*context = format;
	return 0;
}


// #pragma mark - Blu-ray


struct BdPlayItem {
	BString		clip;		// the stream file's path
	uint32		in;			// 45 kHz
	uint32		out;
};


struct BdPlaylist {
	DiscTitle	title;
	std::vector<BdPlayItem> items;
	std::vector<bigtime_t> chapters;
	std::map<int, DiscStream> streams;	// by PID
};


/*!	The BDMV folder of a Blu-ray folder (or an AVCHD one). */
static BString
bdmv_folder(const char* path)
{
	if (leaf_of(path).ICompare("BDMV") == 0 && is_directory(path))
		return BString(path);
	BString folder = find_entry(path, "BDMV");
	return is_directory(folder.String()) ? folder : BString();
}


/*!	The stream file of clip \a name ("00001"): STREAM/00001.m2ts, or .MTS
	on an AVCHD disc. */
static BString
bd_clip_path(const BString& bdmv, const char* name)
{
	BString stream = find_entry(bdmv.String(), "STREAM");
	if (stream.IsEmpty())
		return stream;
	static const char* kExtensions[] = { "m2ts", "mts" };
	for (size_t i = 0; i < 2; i++) {
		BString file;
		file.SetToFormat("%s.%s", name, kExtensions[i]);
		BString found = find_entry(stream.String(), file.String());
		if (!found.IsEmpty())
			return found;
	}
	return BString();
}


/*!	The streams of a play item's STN table: their PIDs and languages. */
static void
bd_read_streams(const uint8* table, size_t size,
	std::map<int, DiscStream>& streams)
{
	// two reserved bytes, the counts (video, audio, presentation and
	// interactive graphics, then secondary streams), five reserved bytes
	if (size < 14)
		return;
	int counts[4];
	for (int i = 0; i < 4; i++)
		counts[i] = table[2 + i];
	size_t at = 14;
	// secondary streams carry more after their attributes: stop before them
	for (int kind = 0; kind < 4; kind++) {
		for (int i = 0; i < counts[kind]; i++) {
			if (at + 1 > size)
				return;
			size_t entryLength = table[at];
			const uint8* entry = table + at + 1;
			at += 1 + entryLength;
			if (at + 1 > size)
				return;
			size_t attributesLength = table[at];
			const uint8* attributes = table + at + 1;
			at += 1 + attributesLength;
			if (at > size || entryLength < 3 || attributesLength < 1)
				continue;
			// stream type 1: in the clip itself, by PID
			if (entry[0] != 1)
				continue;
			DiscStream stream;
			stream.id = be16(entry + 1);
			stream.commentary = false;
			stream.forced = false;
			uint8 coding = attributes[0];
			const uint8* language = NULL;
			if ((coding >= 0x80 && coding <= 0x86) || coding == 0xa1
				|| coding == 0xa2) {
				if (attributesLength >= 5)
					language = attributes + 2;
			} else if (coding == 0x90 || coding == 0x91) {
				if (attributesLength >= 4)
					language = attributes + 1;
			} else if (coding == 0x92) {
				if (attributesLength >= 5)
					language = attributes + 2;
			}
			if (language != NULL)
				stream.language = BString((const char*)language, 3);
			streams[stream.id] = stream;
		}
	}
}


static bool
bd_read_playlist(const BString& bdmv, const char* file, int32 number,
	BdPlaylist& playlist)
{
	FILE* handle = fopen(file, "rb");
	if (handle == NULL)
		return false;
	std::vector<uint8> data(1024 * 1024);
	size_t size = fread(data.data(), 1, data.size(), handle);
	fclose(handle);
	const uint8* mpls = data.data();
	if (size < 40 || memcmp(mpls, "MPLS", 4) != 0)
		return false;
	size_t list = be32(mpls + 8);
	size_t marks = be32(mpls + 12);
	if (list + 10 > size)
		return false;
	int itemCount = be16(mpls + list + 6);
	size_t at = list + 10;
	playlist.title.number = number;
	playlist.title.angles = 1;
	bigtime_t elapsed = 0;
	std::vector<bigtime_t> itemStarts;
	for (int i = 0; i < itemCount; i++) {
		if (at + 2 > size)
			return false;
		size_t length = be16(mpls + at);
		const uint8* item = mpls + at + 2;
		if (at + 2 + length > size || length < 32)
			return false;
		BdPlayItem playItem;
		BString name((const char*)item, 5);
		playItem.clip = bd_clip_path(bdmv, name.String());
		bool multiAngle = (item[10] & 0x10) != 0;
		playItem.in = be32(item + 12);
		playItem.out = be32(item + 16);
		if (playItem.clip.IsEmpty() || playItem.out <= playItem.in)
			return false;
		itemStarts.push_back(elapsed);
		elapsed += (bigtime_t)(playItem.out - playItem.in) * 1000000 / 45000;
		size_t stn = 32;
		if (multiAngle && length >= 34) {
			playlist.title.angles = std::max(playlist.title.angles,
				(int32)item[32]);
			stn = 34 + 10 * std::max(0, item[32] - 1);
		}
		if (i == 0 && stn + 2 <= length) {
			size_t stnLength = be16(item + stn);
			bd_read_streams(item + stn + 2, std::min(stnLength,
				length - stn - 2), playlist.streams);
		}
		playlist.items.push_back(playItem);
		at += 2 + length;
	}
	if (playlist.items.empty())
		return false;
	if (marks + 6 <= size) {
		int markCount = be16(mpls + marks + 4);
		for (int i = 0; i < markCount; i++) {
			const uint8* mark = mpls + marks + 6 + 14 * i;
			if (mark + 14 > mpls + size)
				break;
			int item = be16(mark + 2);
			if (mark[1] != 1 || item >= (int)playlist.items.size())
				continue;
			uint32 time = be32(mark + 4);
			bigtime_t start = itemStarts[item]
				+ ((bigtime_t)time - playlist.items[item].in) * 1000000 / 45000;
			if (start < 0 || start >= elapsed)
				continue;
			if (playlist.chapters.empty() || start > playlist.chapters.back()
					+ 1000000)
				playlist.chapters.push_back(start);
		}
	}
	if (playlist.chapters.empty() || playlist.chapters[0] > 1000000)
		playlist.chapters.insert(playlist.chapters.begin(), 0);
	else
		playlist.chapters[0] = 0;
	playlist.title.duration = elapsed;
	playlist.title.chapters = playlist.chapters.size();
	return true;
}


/*!	The disc's playlists, the same one under several numbers (as discs
	that hide the main one among copies have it) once. */
static status_t
bd_read_playlists(const char* path, std::vector<BdPlaylist>& playlists,
	BString* error)
{
	BString bdmv = bdmv_folder(path);
	BString folder = bdmv.IsEmpty() ? BString()
		: find_entry(bdmv.String(), "PLAYLIST");
	DIR* dir = folder.IsEmpty() ? NULL : opendir(folder.String());
	if (dir == NULL) {
		if (error != NULL)
			*error = "The Blu-ray has no playlists.";
		return B_ENTRY_NOT_FOUND;
	}
	std::vector<BString> names;
	while (dirent* entry = readdir(dir)) {
		BString name(entry->d_name);
		BString lower(name);
		lower.ToLower();
		if (lower.EndsWith(".mpls") || lower.EndsWith(".mpl"))
			names.push_back(name);
	}
	closedir(dir);
	std::sort(names.begin(), names.end());
	for (size_t i = 0; i < names.size(); i++) {
		BString file;
		file.SetToFormat("%s/%s", folder.String(), names[i].String());
		BdPlaylist playlist;
		if (!bd_read_playlist(bdmv, file.String(), atoi(names[i].String()),
				playlist))
			continue;
		bool copy = false;
		for (size_t j = 0; j < playlists.size() && !copy; j++) {
			const BdPlaylist& other = playlists[j];
			if (other.items.size() != playlist.items.size())
				continue;
			bool same = true;
			for (size_t k = 0; k < other.items.size() && same; k++) {
				same = other.items[k].clip == playlist.items[k].clip
					&& other.items[k].in == playlist.items[k].in
					&& other.items[k].out == playlist.items[k].out;
			}
			copy = same;
		}
		if (!copy)
			playlists.push_back(playlist);
	}
	if (playlists.empty()) {
		if (error != NULL)
			*error = "The Blu-ray's playlists cannot be read.";
		return B_BAD_DATA;
	}
	return B_OK;
}


/*!	AACS encrypted units have the copy permission bits of their first
	packet set (and no sync bytes after their first 16 bytes). */
static bool
bd_is_encrypted(const char* clip)
{
	uint8 unit[6144];
	if (!read_at(clip, 0, unit, sizeof(unit)))
		return false;
	if ((unit[0] & 0xc0) != 0)
		return true;
	for (size_t at = 4; at < sizeof(unit); at += 192) {
		if (unit[at] != 0x47)
			return true;
	}
	return false;
}


struct ScriptReader : DiscIO {
	BString		script;
	int64		position;
	int64		size;		// the clips'

	virtual int64 Size() const
	{
		return size;
	}
};


static int
script_read(void* opaque, uint8_t* buffer, int size)
{
	ScriptReader* reader = (ScriptReader*)opaque;
	int64 left = reader->script.Length() - reader->position;
	if (left <= 0)
		return AVERROR_EOF;
	int count = (int)std::min((int64)size, left);
	memcpy(buffer, reader->script.String() + reader->position, count);
	reader->position += count;
	return count;
}


static BString
concat_quote(const BString& path)
{
	BString quoted(path);
	quoted.ReplaceAll("'", "'\\''");
	BString result;
	result << "'" << quoted << "'";
	return result;
}


/*!	A playlist plays through FFmpeg's concat demuxer: each play item's
	stream file from its in to its out time, the timestamps made one line. */
static int
open_bluray(AVFormatContext** context, const char* path, int32 number,
	BString* error)
{
	std::vector<BdPlaylist> playlists;
	if (bd_read_playlists(path, playlists, error) != B_OK)
		return AVERROR_INVALIDDATA;
	const BdPlaylist* playlist = NULL;
	for (size_t i = 0; i < playlists.size(); i++) {
		if (playlists[i].title.number == number
			|| (number < 0 && (playlist == NULL || playlists[i].title.duration
				> playlist->title.duration)))
			playlist = &playlists[i];
	}
	if (playlist == NULL) {
		if (error != NULL)
			error->SetToFormat("The Blu-ray has no playlist %05d.",
				(int)number);
		return AVERROR(ENOENT);
	}
	if (bd_is_encrypted(playlist->items[0].clip.String())) {
		if (error != NULL) {
			*error = "This Blu-ray is encrypted (AACS), which MediaConverter "
				"cannot read.";
		}
		return AVERROR(EACCES);
	}

	// the first clip's stream order is the concat demuxer's: it gives the
	// PIDs the playlist's languages belong to
	std::vector<int> pids;
	AVFormatContext* first = NULL;
	if (avformat_open_input(&first, playlist->items[0].clip.String(), NULL,
			NULL) >= 0) {
		if (avformat_find_stream_info(first, NULL) >= 0) {
			for (unsigned i = 0; i < first->nb_streams; i++)
				pids.push_back(first->streams[i]->id);
		}
		avformat_close_input(&first);
	}

	ScriptReader* reader = new ScriptReader;
	reader->position = 0;
	reader->size = 0;
	reader->script = "ffconcat version 1.0\n";
	for (size_t i = 0; i < playlist->items.size(); i++) {
		const BdPlayItem& item = playlist->items[i];
		struct stat st;
		if ((i == 0 || item.clip != playlist->items[i - 1].clip)
			&& stat(item.clip.String(), &st) == 0)
			reader->size += st.st_size;
		BString line;
		line.SetToFormat("file %s\ninpoint %.6f\noutpoint %.6f\n",
			concat_quote(item.clip).String(), item.in / 45000.0,
			item.out / 45000.0);
		reader->script << line;
	}

	const int kBufferSize = 4096;
	uint8* buffer = (uint8*)av_malloc(kBufferSize);
	AVIOContext* io = buffer != NULL ? avio_alloc_context(buffer, kBufferSize,
		0, reader, script_read, NULL, NULL) : NULL;
	AVFormatContext* format = avformat_alloc_context();
	if (io == NULL || format == NULL) {
		if (io == NULL)
			av_free(buffer);
		else {
			av_freep(&io->buffer);
			avio_context_free(&io);
		}
		avformat_free_context(format);
		delete reader;
		return AVERROR(ENOMEM);
	}
	format->pb = io;
	format->opaque = reader;
	AVDictionary* options = NULL;
	av_dict_set(&options, "safe", "0", 0);
	int result = avformat_open_input(&format, NULL,
		av_find_input_format("concat"), &options);
	av_dict_free(&options);
	if (result < 0) {
		av_freep(&io->buffer);
		avio_context_free(&io);
		delete reader;
		if (error != NULL)
			*error = "The Blu-ray playlist cannot be read.";
		return result;
	}
	result = avformat_find_stream_info(format, NULL);
	if (result < 0) {
		close_source(&format);
		if (error != NULL)
			*error = "The Blu-ray playlist's streams cannot be read.";
		return result;
	}

	for (unsigned i = 0; i < format->nb_streams && i < pids.size(); i++) {
		AVStream* stream = format->streams[i];
		stream->id = pids[i];
		std::map<int, DiscStream>::const_iterator found
			= playlist->streams.find(pids[i]);
		if (found != playlist->streams.end()
			&& av_dict_get(stream->metadata, "language", NULL, 0) == NULL)
			label_stream(stream, found->second);
	}
	format->duration = playlist->title.duration;
	add_chapters(format, playlist->chapters, playlist->title.duration);
	av_dict_set(&format->metadata, "title", disc_name(path).String(), 0);
	*context = format;
	return 0;
}


// #pragma mark - the API


disc_kind
identify_disc(const char* path)
{
	if (path == NULL || path[0] == '\0')
		return kDiscNone;
	if (is_directory(path)) {
		if (!bdmv_folder(path).IsEmpty()) {
			BString playlists = find_entry(bdmv_folder(path).String(),
				"PLAYLIST");
			if (!playlists.IsEmpty())
				return kDiscBluray;
		}
	}
	DvdFiles files;
	if (files.Open(path) == B_OK)
		return kDiscDVD;
	return kDiscNone;
}


/*!	The folder or image that is the disc: Movie for Movie/VIDEO_TS. */
static BString
disc_root(const char* path)
{
	BString leaf = leaf_of(path);
	if (leaf.ICompare("VIDEO_TS") == 0 || leaf.ICompare("BDMV") == 0)
		return parent_of(path);
	BString root(path);
	while (root.EndsWith("/") && root.Length() > 1)
		root.Truncate(root.Length() - 1);
	return root;
}


BString
disc_name(const char* path)
{
	BString name = leaf_of(disc_root(path).String());
	if (!is_directory(disc_root(path).String())) {
		int32 dot = name.FindLast('.');
		if (dot > 0)
			name.Truncate(dot);
	}
	return name;
}


BString
disc_folder(const char* path)
{
	return parent_of(disc_root(path).String());
}


status_t
list_disc_titles(const char* path, std::vector<DiscTitle>& titles,
	int32* mainTitle, BString* error)
{
	titles.clear();
	*mainTitle = -1;
	bigtime_t longest = -1;
	switch (identify_disc(path)) {
		case kDiscDVD:
		{
			DvdFiles files;
			std::vector<DvdTitleMap> maps;
			if (files.Open(path) != B_OK
				|| dvd_read_titles(files, maps, error) != B_OK)
				return B_BAD_DATA;
			for (size_t i = 0; i < maps.size(); i++) {
				if (maps[i].title.duration < kMinimumTitle)
					continue;
				titles.push_back(maps[i].title);
				if (maps[i].title.duration > longest) {
					longest = maps[i].title.duration;
					*mainTitle = maps[i].title.number;
				}
			}
			break;
		}
		case kDiscBluray:
		{
			std::vector<BdPlaylist> playlists;
			if (bd_read_playlists(path, playlists, error) != B_OK)
				return B_BAD_DATA;
			for (size_t i = 0; i < playlists.size(); i++) {
				if (playlists[i].title.duration < kMinimumTitle)
					continue;
				titles.push_back(playlists[i].title);
				if (playlists[i].title.duration > longest) {
					longest = playlists[i].title.duration;
					*mainTitle = playlists[i].title.number;
				}
			}
			break;
		}
		default:
			if (error != NULL)
				*error = "This is not a DVD or Blu-ray.";
			return B_BAD_VALUE;
	}
	if (titles.empty()) {
		if (error != NULL)
			*error = "The disc has no titles longer than ten seconds.";
		return B_ENTRY_NOT_FOUND;
	}
	return B_OK;
}


BString
disc_source(const char* path, int32 title)
{
	BString source;
	source.SetToFormat("disc:%d:%s", (int)title, path);
	return source;
}


bool
parse_disc_source(const char* source, BString* path, int32* title)
{
	if (source == NULL || strncmp(source, "disc:", 5) != 0)
		return false;
	char* end = NULL;
	long number = strtol(source + 5, &end, 10);
	if (end == source + 5 || *end != ':')
		return false;
	*title = (int32)number;
	*path = end + 1;
	return true;
}


int
open_source(AVFormatContext** context, const char* source, BString* error)
{
	*context = NULL;
	BString path;
	int32 title;
	if (parse_disc_source(source, &path, &title)) {
		switch (identify_disc(path.String())) {
			case kDiscDVD:
				return open_dvd(context, path.String(), title, error);
			case kDiscBluray:
				return open_bluray(context, path.String(), title, error);
			default:
				if (error != NULL)
					*error = "The disc cannot be found.";
				return AVERROR(ENOENT);
		}
	}

	int result = avformat_open_input(context, source, NULL, NULL);
	if (result < 0) {
		if (error != NULL) {
			char message[AV_ERROR_MAX_STRING_SIZE];
			av_strerror(result, message, sizeof(message));
			error->SetToFormat("Cannot open the file: %s", message);
		}
		return result;
	}
	result = avformat_find_stream_info(*context, NULL);
	if (result < 0) {
		avformat_close_input(context);
		if (error != NULL)
			*error = "The file's streams cannot be read.";
	}
	return result;
}


int64
source_size(AVFormatContext* context)
{
	if (context == NULL)
		return 0;
	if ((context->flags & AVFMT_FLAG_CUSTOM_IO) != 0 && context->opaque != NULL)
		return ((DiscIO*)context->opaque)->Size();
	int64 size = context->pb != NULL ? avio_size(context->pb) : 0;
	return size > 0 ? size : 0;
}


void
close_source(AVFormatContext** context)
{
	if (context == NULL || *context == NULL)
		return;
	DiscIO* reader = NULL;
	AVIOContext* io = NULL;
	if (((*context)->flags & AVFMT_FLAG_CUSTOM_IO) != 0) {
		reader = (DiscIO*)(*context)->opaque;
		io = (*context)->pb;
	}
	avformat_close_input(context);
	if (io != NULL) {
		av_freep(&io->buffer);
		avio_context_free(&io);
	}
	delete reader;
}
