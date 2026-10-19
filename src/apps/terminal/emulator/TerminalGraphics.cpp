/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


//!	kitty's terminal graphics protocol.


#include "TerminalGraphics.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include "PlaceholderDiacritics.h"
#include "Terminal.h"


static const size_t kQuota = 320 * 1024 * 1024;
static const size_t kMaxLoadLength = 512 * 1024 * 1024;
static const int32 kMaxImageSize = 10000;
static const int32 kMaxRelativeDepth = 8;
static const int32 kDefaultFrameGap = 40;

static TerminalGraphics::DecodeFunction sDecodePNG = NULL;
static TerminalGraphics::InflateFunction sInflate = NULL;
static uint64 sFrameSerial = 0;


struct TerminalGraphics::Image {
	uint32				id;
	uint32				number;
	bool				clientId;		// the program chose the id
	int32				width;
	int32				height;
	std::vector<std::shared_ptr<ImageFrame> > frames;
	int32				currentFrame;
	int32				animationState;	// 1 stopped, 2 loading, 3 looping
	int32				loops;			// 0 forever, else how many
	int32				loopsDone;
	bigtime_t			frameStart;
	bigtime_t			lastUsed;
	size_t				memory;

	const std::shared_ptr<ImageFrame>& CurrentFrame() const
	{
		if (currentFrame >= 0 && currentFrame < (int32)frames.size())
			return frames[currentFrame];
		return frames[0];
	}
};


struct TerminalGraphics::Placement {
	uint32				imageId;
	uint32				placementId;
	int64				line;			// anchoring line
	int32				column;
	int32				offsetX;
	int32				offsetY;
	int32				sourceX;
	int32				sourceY;
	int32				sourceWidth;
	int32				sourceHeight;
	uint32				requestedColumns;
	uint32				requestedRows;
	int32				columns;		// cells covered
	int32				rows;
	int32				drawX;			// within the covered cells
	int32				drawY;
	int32				drawWidth;
	int32				drawHeight;
	int32				z;
	bool				isVirtual;
	uint32				parentImage;
	uint32				parentPlacement;
	int32				parentColumns;	// H
	int32				parentRows;		// V
};


struct TerminalGraphics::Store {
	std::map<uint32, Image*>		images;
	std::vector<Placement*>			placements;
	size_t							memory;
	uint32							nextId;

	Store()
		:
		memory(0),
		nextId(0x80000000)
	{
	}

	~Store()
	{
		Clear();
	}

	void Clear()
	{
		for (std::map<uint32, Image*>::iterator it = images.begin();
				it != images.end(); it++) {
			delete it->second;
		}
		images.clear();
		for (size_t i = 0; i < placements.size(); i++)
			delete placements[i];
		placements.clear();
		memory = 0;
	}
};


struct TerminalGraphics::Command {
	char		action;
	int32		quiet;
	int32		format;
	char		medium;
	uint32		width;		// s
	uint32		height;		// v
	uint32		size;		// S
	uint32		offset;		// O
	uint32		id;			// i
	uint32		number;		// I
	uint32		placement;	// p
	char		compression;
	int32		more;		// m
	uint32		hints;		// N
	uint32		x;
	uint32		y;
	uint32		w;
	uint32		h;
	uint32		X;
	uint32		Y;
	uint32		c;
	uint32		r;
	uint32		cursorPolicy;	// C
	uint32		unicode;		// U
	int32		z;
	bool		hasZ;
	uint32		parentImage;	// P
	uint32		parentPlacement;	// Q
	int32		H;
	int32		V;
	char		what;			// d
	uint32		assignedId;		// the id an I= image got

	Command()
	{
		memset(this, 0, sizeof(*this));
		action = 't';
		format = 32;
		medium = 'd';
		what = 'a';
	}
};


// #pragma mark -


TerminalGraphics::TerminalGraphics(Terminal* terminal)
	:
	fTerminal(terminal),
	fMain(new Store),
	fAlternate(new Store),
	fAlternateActive(false),
	fGeneration(0),
	fLoading(false)
{
}


TerminalGraphics::~TerminalGraphics()
{
	delete fMain;
	delete fAlternate;
}


/*static*/ void
TerminalGraphics::SetCodecs(DecodeFunction decodePNG, InflateFunction inflate)
{
	sDecodePNG = decodePNG;
	sInflate = inflate;
}


TerminalGraphics::Store&
TerminalGraphics::_Store()
{
	return fAlternateActive ? *fAlternate : *fMain;
}


void
TerminalGraphics::_Changed()
{
	fGeneration++;
	fTerminal->_Changed();
}


TerminalGraphics::Image*
TerminalGraphics::_FindImage(uint32 id)
{
	Store& store = _Store();
	std::map<uint32, Image*>::iterator found = store.images.find(id);
	return found != store.images.end() ? found->second : NULL;
}


TerminalGraphics::Image*
TerminalGraphics::_FindImageByNumber(uint32 number)
{
	Image* newest = NULL;
	Store& store = _Store();
	for (std::map<uint32, Image*>::iterator it = store.images.begin();
			it != store.images.end(); it++) {
		Image* image = it->second;
		if (image->number == number
			&& (newest == NULL || image->lastUsed >= newest->lastUsed)) {
			newest = image;
		}
	}
	return newest;
}


void
TerminalGraphics::_Reply(const Command& command, const char* message)
{
	bool ok = strcmp(message, "OK") == 0;
	if (command.quiet >= 2 || (ok && command.quiet >= 1))
		return;
	if (command.id == 0 && command.number == 0)
		return;

	char buffer[128];
	if (command.number != 0) {
		snprintf(buffer, sizeof(buffer), "\x1b_Gi=%u,I=%u",
			(unsigned)command.assignedId, (unsigned)command.number);
	} else
		snprintf(buffer, sizeof(buffer), "\x1b_Gi=%u", (unsigned)command.id);
	std::string reply = buffer;
	if (command.placement != 0) {
		snprintf(buffer, sizeof(buffer), ",p=%u",
			(unsigned)command.placement);
		reply += buffer;
	}
	reply += ";";
	reply += message;
	reply += "\x1b\\";
	fTerminal->Send(reply);
}


bool
TerminalGraphics::_ParseCommand(const char* data, size_t length,
	Command& command, std::string& payload)
{
	const char* end = data + length;
	const char* separator = (const char*)memchr(data, ';', length);
	const char* controlEnd = separator != NULL ? separator : end;
	if (separator != NULL)
		payload.assign(separator + 1, end - separator - 1);

	const char* position = data;
	while (position < controlEnd) {
		const char* pairEnd = (const char*)memchr(position, ',',
			controlEnd - position);
		if (pairEnd == NULL)
			pairEnd = controlEnd;
		if (pairEnd - position >= 3 && position[1] == '=') {
			char key = position[0];
			std::string value(position + 2, pairEnd - position - 2);
			int64 number = strtoll(value.c_str(), NULL, 10);
			uint32 unsignedValue = (uint32)number;
			int32 signedValue = (int32)std::max((int64)INT32_MIN,
				std::min((int64)INT32_MAX, number));
			switch (key) {
				case 'a': command.action = value[0]; break;
				case 'q': command.quiet = signedValue; break;
				case 'f': command.format = signedValue; break;
				case 't': command.medium = value[0]; break;
				case 's': command.width = unsignedValue; break;
				case 'v': command.height = unsignedValue; break;
				case 'S': command.size = unsignedValue; break;
				case 'O': command.offset = unsignedValue; break;
				case 'i': command.id = unsignedValue; break;
				case 'I': command.number = unsignedValue; break;
				case 'p': command.placement = unsignedValue; break;
				case 'o': command.compression = value[0]; break;
				case 'm': command.more = signedValue; break;
				case 'N': command.hints = unsignedValue; break;
				case 'x': command.x = unsignedValue; break;
				case 'y': command.y = unsignedValue; break;
				case 'w': command.w = unsignedValue; break;
				case 'h': command.h = unsignedValue; break;
				case 'X': command.X = unsignedValue; break;
				case 'Y': command.Y = unsignedValue; break;
				case 'c': command.c = unsignedValue; break;
				case 'r': command.r = unsignedValue; break;
				case 'C': command.cursorPolicy = unsignedValue; break;
				case 'U': command.unicode = unsignedValue; break;
				case 'z': command.z = signedValue; command.hasZ = true; break;
				case 'P': command.parentImage = unsignedValue; break;
				case 'Q': command.parentPlacement = unsignedValue; break;
				case 'H': command.H = signedValue; break;
				case 'V': command.V = signedValue; break;
				case 'd': command.what = value[0]; break;
			}
		}
		position = pairEnd + 1;
	}
	return true;
}


void
TerminalGraphics::HandleCommand(const char* data, size_t length)
{
	Command command;
	std::string payload;
	_ParseCommand(data, length, command, payload);

	if (fLoading) {
		// a chunk of a transmission in progress: only m, q and a=f count
		if (command.action == 'd') {
			// any delete aborts it
			fLoading = false;
			fLoadData.clear();
		} else {
			if (fLoadData.size() + payload.size() > kMaxLoadLength) {
				fLoading = false;
				fLoadData.clear();
				Command header;
				std::string ignored;
				_ParseCommand(fLoadHeader.data(), fLoadHeader.size(), header,
					ignored);
				_Reply(header, "EFBIG:Image too large");
				return;
			}
			fLoadData += payload;
			if (command.more == 1)
				return;

			fLoading = false;
			Command header;
			std::string ignored;
			_ParseCommand(fLoadHeader.data(), fLoadHeader.size(), header,
				ignored);
			if (data[0] != '\0' && strstr(std::string(data, length).c_str(),
					"q=") != NULL) {
				header.quiet = command.quiet;
			}
			std::string all;
			all.swap(fLoadData);
			if (header.action == 'f')
				_Frame(header, all);
			else
				_Transmit(header, all);
			return;
		}
	}

	if (command.id != 0 && command.number != 0) {
		_Reply(command, "EINVAL:Must not specify both i and I");
		return;
	}

	switch (command.action) {
		case 't':
		case 'T':
		case 'q':
		case 'f':
			if (command.more == 1) {
				fLoading = true;
				fLoadHeader.assign(data, length);
				size_t separator = fLoadHeader.find(';');
				if (separator != std::string::npos)
					fLoadHeader.erase(separator);
				fLoadData = payload;
				return;
			}
			if (command.action == 'f')
				_Frame(command, payload);
			else
				_Transmit(command, payload);
			break;
		case 'p':
		{
			Image* image = command.id != 0 ? _FindImage(command.id)
				: command.number != 0 ? _FindImageByNumber(command.number)
				: NULL;
			if (image == NULL) {
				_Reply(command, "ENOENT:No such image");
				break;
			}
			command.assignedId = image->id;
			_Put(command, image);
			break;
		}
		case 'd':
			_Delete(command);
			break;
		case 'a':
			_AnimationControl(command);
			break;
		case 'c':
			_Compose(command);
			break;
	}
}


// #pragma mark - loading


bool
TerminalGraphics::_ReadMedium(const Command& command,
	const std::string& payload, std::vector<uint8>& data)
{
	std::string name;
	if (!base64_decode(payload.data(), payload.size(), name))
		return false;

	if (command.medium == 's') {
		// POSIX shared memory
		if (name.empty() || name[0] != '/'
			|| name.find('/', 1) != std::string::npos) {
			return false;
		}
		int fd = shm_open(name.c_str(), O_RDONLY, 0);
		if (fd < 0)
			return false;
		struct stat st;
		bool result = false;
		if (fstat(fd, &st) == 0 && st.st_size > 0
			&& command.offset < (uint64)st.st_size) {
			size_t size = st.st_size - command.offset;
			if (command.size != 0)
				size = std::min(size, (size_t)command.size);
			void* address = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED,
				fd, 0);
			if (address != MAP_FAILED) {
				const uint8* bytes = (const uint8*)address + command.offset;
				data.assign(bytes, bytes + size);
				munmap(address, st.st_size);
				result = command.size == 0 || data.size() == command.size;
			}
		}
		close(fd);
		shm_unlink(name.c_str());
		return result;
	}

	// a file: regular files only, not in the system's special places
	if (name.empty() || name[0] != '/')
		return false;
	char resolved[PATH_MAX];
	if (realpath(name.c_str(), resolved) == NULL)
		return false;
	static const char* const kForbidden[] = { "/dev/", "/proc/", "/sys/",
		NULL };
	for (int32 i = 0; kForbidden[i] != NULL; i++) {
		if (strncmp(resolved, kForbidden[i], strlen(kForbidden[i])) == 0)
			return false;
	}
	struct stat st;
	if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode))
		return false;

	int fd = open(resolved, O_RDONLY);
	if (fd < 0)
		return false;
	bool result = false;
	if (command.offset <= (uint64)st.st_size) {
		size_t size = st.st_size - command.offset;
		if (command.size != 0)
			size = std::min(size, (size_t)command.size);
		if (size <= kMaxLoadLength) {
			data.resize(size);
			ssize_t bytesRead = pread(fd, data.data(), size, command.offset);
			result = bytesRead == (ssize_t)size
				&& (command.size == 0 || size == command.size);
		}
	}
	close(fd);

	if (command.medium == 't') {
		// temporary files are deleted, but only from temporary places
		const char* tmp = getenv("TMPDIR");
		bool inTemporary = strncmp(resolved, "/tmp/", 5) == 0
			|| strncmp(resolved, "/boot/system/cache/tmp/", 23) == 0
			|| strncmp(resolved, "/dev/shm/", 9) == 0
			|| (tmp != NULL && tmp[0] != '\0'
				&& strncmp(resolved, tmp, strlen(tmp)) == 0);
		if (inTemporary && strstr(resolved, "tty-graphics-protocol") != NULL)
			unlink(resolved);
	}
	return result;
}


/*!	Turns the payload of a transmission into RGBA pixels.
*/
bool
TerminalGraphics::_LoadData(Command& command, const std::string& payload,
	std::vector<uint8>& pixels, int32& width, int32& height,
	std::string& error)
{
	std::vector<uint8> data;
	if (command.medium == 'd') {
		std::string decoded;
		if (!base64_decode(payload.data(), payload.size(), decoded)) {
			error = "EINVAL:Invalid base64 data";
			return false;
		}
		data.assign(decoded.begin(), decoded.end());
	} else if (command.medium == 'f' || command.medium == 't'
		|| command.medium == 's') {
		if (!_ReadMedium(command, payload, data)) {
			error = "EBADF:Failed to read image file";
			return false;
		}
	} else {
		error = "EINVAL:Unknown transmission medium";
		return false;
	}

	if (command.format != 24 && command.format != 32
		&& command.format != 100) {
		error = "EINVAL:Unknown image format";
		return false;
	}

	if (command.compression == 'z') {
		size_t expected = command.format == 100 ? command.size
			: (size_t)command.width * command.height * (command.format / 8);
		std::vector<uint8> inflated;
		if (sInflate == NULL || expected == 0 || expected > kMaxLoadLength
			|| !sInflate(data.data(), data.size(), expected, inflated)) {
			error = "EINVAL:Failed to inflate the image data";
			return false;
		}
		data.swap(inflated);
	} else if (command.compression != 0) {
		error = "EINVAL:Unknown compression";
		return false;
	}

	if (command.format == 100) {
		if (sDecodePNG == NULL
			|| !sDecodePNG(data.data(), data.size(), width, height, pixels)) {
			error = "EBADPNG:Failed to decode the PNG image";
			return false;
		}
	} else {
		width = command.width;
		height = command.height;
		if (width <= 0 || height <= 0) {
			error = "EINVAL:Image size missing";
			return false;
		}
		if (width > kMaxImageSize || height > kMaxImageSize) {
			error = "EFBIG:Image too large";
			return false;
		}
		size_t bytesPerPixel = command.format / 8;
		size_t expected = (size_t)width * height * bytesPerPixel;
		if (data.size() < expected) {
			error = "ENODATA:Insufficient image data";
			return false;
		}
		if (command.format == 32) {
			data.resize(expected);
			pixels.swap(data);
		} else {
			pixels.resize((size_t)width * height * 4);
			for (size_t i = 0, j = 0; i < expected; i += 3, j += 4) {
				pixels[j] = data[i];
				pixels[j + 1] = data[i + 1];
				pixels[j + 2] = data[i + 2];
				pixels[j + 3] = 255;
			}
		}
	}

	if (width <= 0 || height <= 0 || width > kMaxImageSize
		|| height > kMaxImageSize) {
		error = "EFBIG:Image too large";
		return false;
	}
	return true;
}


void
TerminalGraphics::_Transmit(Command& command, const std::string& payload)
{
	std::vector<uint8> pixels;
	int32 width = 0;
	int32 height = 0;
	std::string error;
	if (!_LoadData(command, payload, pixels, width, height, error)) {
		_Reply(command, error.c_str());
		return;
	}

	if (command.action == 'q') {
		_Reply(command, "OK");
		return;
	}

	Store& store = _Store();
	size_t memory = pixels.size();
	_EnforceQuota(memory);

	uint32 id = command.id;
	if (id != 0) {
		// a new image with an existing id replaces it, placements and all
		_DeleteImage(id);
	} else {
		do {
			id = store.nextId++;
			if (store.nextId == 0)
				store.nextId = 0x80000000;
		} while (store.images.find(id) != store.images.end());
	}
	command.assignedId = id;

	Image* image = new Image;
	image->id = id;
	image->number = command.number;
	image->clientId = command.id != 0;
	image->width = width;
	image->height = height;
	std::shared_ptr<ImageFrame> frame(new ImageFrame);
	frame->pixels.swap(pixels);
	frame->gap = 0;
	frame->serial = ++sFrameSerial;
	image->frames.push_back(frame);
	image->currentFrame = 0;
	image->animationState = 1;
	image->loops = 0;
	image->loopsDone = 0;
	image->frameStart = system_time();
	image->lastUsed = system_time();
	image->memory = memory;
	store.images[id] = image;
	store.memory += memory;

	if (command.action == 'T')
		_Put(command, image);
	else
		_Reply(command, "OK");
}


void
TerminalGraphics::_EnforceQuota(size_t incoming)
{
	Store& store = _Store();
	while (store.memory + incoming > kQuota && !store.images.empty()) {
		// evict the least recently used image, those without placements
		// first
		Image* victim = NULL;
		bool victimPlaced = true;
		for (std::map<uint32, Image*>::iterator it = store.images.begin();
				it != store.images.end(); it++) {
			Image* image = it->second;
			bool placed = false;
			for (size_t i = 0; i < store.placements.size(); i++) {
				if (store.placements[i]->imageId == image->id) {
					placed = true;
					break;
				}
			}
			if (victim == NULL || (victimPlaced && !placed)
				|| (placed == victimPlaced
					&& image->lastUsed < victim->lastUsed)) {
				victim = image;
				victimPlaced = placed;
			}
		}
		if (victim == NULL)
			break;
		_DeleteImage(victim->id);
	}
}


// #pragma mark - placements


void
TerminalGraphics::_ComputeExtent(Placement& placement, const Image& image)
{
	int32 cellWidth = std::max((int32)1, fTerminal->CellWidth());
	int32 cellHeight = std::max((int32)1, fTerminal->CellHeight());

	int32 sourceX = std::min(placement.sourceX, image.width);
	int32 sourceY = std::min(placement.sourceY, image.height);
	int32 sourceWidth = placement.sourceWidth > 0 ? placement.sourceWidth
		: image.width - sourceX;
	int32 sourceHeight = placement.sourceHeight > 0 ? placement.sourceHeight
		: image.height - sourceY;
	sourceWidth = std::max((int32)0, std::min(sourceWidth,
		image.width - sourceX));
	sourceHeight = std::max((int32)0, std::min(sourceHeight,
		image.height - sourceY));
	placement.sourceX = sourceX;
	placement.sourceY = sourceY;
	placement.sourceWidth = sourceWidth;
	placement.sourceHeight = sourceHeight;

	placement.offsetX = std::min(placement.offsetX, cellWidth - 1);
	placement.offsetY = std::min(placement.offsetY, cellHeight - 1);

	int32 columns = placement.requestedColumns;
	int32 rows = placement.requestedRows;
	int32 drawWidth = sourceWidth;
	int32 drawHeight = sourceHeight;
	int32 drawX = placement.offsetX;
	int32 drawY = placement.offsetY;

	if (sourceWidth == 0 || sourceHeight == 0) {
		drawWidth = drawHeight = 0;
	} else if (columns > 0 && rows > 0) {
		// fit into the box, keeping the aspect ratio
		double boxWidth = (double)columns * cellWidth;
		double boxHeight = (double)rows * cellHeight;
		double scale = std::min(boxWidth / sourceWidth,
			boxHeight / sourceHeight);
		drawWidth = (int32)(sourceWidth * scale + 0.5);
		drawHeight = (int32)(sourceHeight * scale + 0.5);
		drawX = (int32)((boxWidth - drawWidth) / 2);
		drawY = (int32)((boxHeight - drawHeight) / 2);
	} else if (columns > 0) {
		drawWidth = columns * cellWidth;
		drawHeight = (int32)((double)sourceHeight * drawWidth / sourceWidth
			+ 0.5);
		rows = (drawHeight + drawY + cellHeight - 1) / cellHeight;
	} else if (rows > 0) {
		drawHeight = rows * cellHeight;
		drawWidth = (int32)((double)sourceWidth * drawHeight / sourceHeight
			+ 0.5);
		columns = (drawWidth + drawX + cellWidth - 1) / cellWidth;
	} else {
		columns = (drawWidth + drawX + cellWidth - 1) / cellWidth;
		rows = (drawHeight + drawY + cellHeight - 1) / cellHeight;
	}

	placement.columns = std::max((int32)1, columns);
	placement.rows = std::max((int32)1, rows);
	placement.drawX = drawX;
	placement.drawY = drawY;
	placement.drawWidth = drawWidth;
	placement.drawHeight = drawHeight;
}


void
TerminalGraphics::_Put(Command& command, Image* image)
{
	Store& store = _Store();
	image->lastUsed = system_time();

	bool relative = command.parentImage != 0;
	bool isVirtual = command.unicode != 0;
	if (relative && isVirtual) {
		_Reply(command, "EINVAL:A virtual placement cannot be relative");
		return;
	}

	Placement* parent = NULL;
	if (relative) {
		for (size_t i = 0; i < store.placements.size(); i++) {
			Placement* candidate = store.placements[i];
			if (candidate->imageId == command.parentImage
				&& (command.parentPlacement == 0
					|| candidate->placementId == command.parentPlacement)) {
				parent = candidate;
				break;
			}
		}
		if (parent == NULL) {
			_Reply(command, "ENOPARENT:Parent placement not found");
			return;
		}
		// chains and cycles
		int32 depth = 1;
		const Placement* link = parent;
		while (link->parentImage != 0) {
			if (link->parentImage == image->id
				&& (command.placement == 0
					|| link->parentPlacement == command.placement)) {
				_Reply(command, "ECYCLE:Cycle in relative placements");
				return;
			}
			const Placement* next = NULL;
			for (size_t i = 0; i < store.placements.size(); i++) {
				Placement* candidate = store.placements[i];
				if (candidate->imageId == link->parentImage
					&& (link->parentPlacement == 0
						|| candidate->placementId == link->parentPlacement)) {
					next = candidate;
					break;
				}
			}
			if (next == NULL)
				break;
			link = next;
			if (++depth > kMaxRelativeDepth) {
				_Reply(command, "ETOODEEP:Too many relative placements");
				return;
			}
		}
		if (parent->imageId == image->id
			&& (command.placement == 0
				|| parent->placementId == command.placement)) {
			_Reply(command, "ECYCLE:Cycle in relative placements");
			return;
		}
	}

	// the same placement id replaces the placement
	Placement* placement = NULL;
	if (command.placement != 0 && image->clientId) {
		for (size_t i = 0; i < store.placements.size(); i++) {
			Placement* candidate = store.placements[i];
			if (candidate->imageId == image->id
				&& candidate->placementId == command.placement) {
				placement = candidate;
				break;
			}
		}
	}
	if (placement == NULL) {
		placement = new Placement;
		memset(placement, 0, sizeof(*placement));
		store.placements.push_back(placement);
	}

	Terminal::CursorInfo cursor = fTerminal->Cursor();

	placement->imageId = image->id;
	placement->placementId = image->clientId ? command.placement : 0;
	placement->line = fAlternateActive ? cursor.y
		: fTerminal->AbsoluteLine(cursor.y);
	placement->column = cursor.x;
	placement->offsetX = command.X;
	placement->offsetY = command.Y;
	placement->sourceX = command.x;
	placement->sourceY = command.y;
	placement->sourceWidth = command.w;
	placement->sourceHeight = command.h;
	placement->requestedColumns = command.c;
	placement->requestedRows = command.r;
	placement->z = command.z;
	placement->isVirtual = isVirtual;
	placement->parentImage = relative ? command.parentImage : 0;
	placement->parentPlacement = relative ? parent->placementId : 0;
	placement->parentColumns = command.H;
	placement->parentRows = command.V;
	_ComputeExtent(*placement, *image);

	_Reply(command, "OK");
	_Changed();

	if (relative || isVirtual || command.cursorPolicy == 1)
		return;

	// move the cursor past the image: right by its columns, down to its
	// last row
	Terminal* terminal = fTerminal;
	int32 x = terminal->fCursorX + placement->columns;
	int32 y = terminal->fCursorY + placement->rows - 1;
	if (x >= terminal->fColumns) {
		x = 0;
		y++;
	}
	if (y > terminal->fBottom) {
		int32 count = y - terminal->fBottom;
		terminal->_ScrollUp(count);
		y = terminal->fBottom;
	}
	terminal->_SetCursor(x, y);
}


/*!	Where a placement is: its line and column, following relative
	placements to their parents.
*/
bool
TerminalGraphics::_PlacementPosition(const Placement& placement,
	int64& line, int32& column, int32 depth) const
{
	if (placement.parentImage == 0) {
		line = placement.line;
		column = placement.column;
		return !placement.isVirtual;
	}
	if (depth > kMaxRelativeDepth)
		return false;

	const Store& store = fAlternateActive ? *fAlternate : *fMain;
	for (size_t i = 0; i < store.placements.size(); i++) {
		const Placement* parent = store.placements[i];
		if (parent->imageId != placement.parentImage
			|| (placement.parentPlacement != 0
				&& parent->placementId != placement.parentPlacement)) {
			continue;
		}
		if (parent->isVirtual)
			return false;
		if (!_PlacementPosition(*parent, line, column, depth + 1))
			return false;
		line += placement.parentRows;
		column += placement.parentColumns;
		return true;
	}
	return false;
}


int32
TerminalGraphics::_PlacementRow(const Placement& placement) const
{
	int64 line;
	int32 column;
	if (!_PlacementPosition(placement, line, column))
		return INT32_MIN;
	int64 base = fAlternateActive ? 0 : (int64)fTerminal->LinesScrolled();
	return (int32)(line - base);
}


// #pragma mark - deleting


void
TerminalGraphics::_DeleteImage(uint32 id)
{
	Store& store = _Store();
	std::map<uint32, Image*>::iterator found = store.images.find(id);
	if (found == store.images.end())
		return;

	for (size_t i = 0; i < store.placements.size();) {
		Placement* placement = store.placements[i];
		if (placement->imageId == id) {
			store.placements.erase(store.placements.begin() + i);
			_DeleteChildren(placement->imageId, placement->placementId,
				true);
			delete placement;
			i = 0;
			continue;
		}
		i++;
	}

	found = store.images.find(id);
	if (found != store.images.end()) {
		store.memory -= std::min(store.memory, found->second->memory);
		delete found->second;
		store.images.erase(found);
	}
	_Changed();
}


void
TerminalGraphics::_DeleteChildren(uint32 imageId, uint32 placementId,
	bool freeData)
{
	Store& store = _Store();
	for (size_t i = 0; i < store.placements.size(); i++) {
		Placement* child = store.placements[i];
		if (child->parentImage == imageId
			&& (child->parentPlacement == 0
				|| child->parentPlacement == placementId)) {
			_DeletePlacement(child, true);
			i = (size_t)-1;
		}
	}
}


void
TerminalGraphics::_DeletePlacement(Placement* placement, bool freeData)
{
	Store& store = _Store();
	std::vector<Placement*>::iterator found = std::find(
		store.placements.begin(), store.placements.end(), placement);
	if (found == store.placements.end())
		return;
	store.placements.erase(found);

	uint32 imageId = placement->imageId;
	_DeleteChildren(imageId, placement->placementId, true);
	delete placement;

	if (freeData) {
		bool used = false;
		for (size_t i = 0; i < store.placements.size(); i++) {
			if (store.placements[i]->imageId == imageId) {
				used = true;
				break;
			}
		}
		if (!used)
			_DeleteImage(imageId);
	}
	_Changed();
}


void
TerminalGraphics::_Delete(Command& command)
{
	Store& store = _Store();
	char what = command.what;
	bool freeData = what >= 'A' && what <= 'Z';
	char lower = freeData ? what - 'A' + 'a' : what;
	int32 rows = fTerminal->Rows();

	// which placements, by a predicate on each
	std::vector<Placement*> victims;
	for (size_t i = 0; i < store.placements.size(); i++) {
		Placement* placement = store.placements[i];
		int64 line;
		int32 column;
		bool positioned = _PlacementPosition(*placement, line, column);
		int32 row = positioned ? _PlacementRow(*placement) : INT32_MIN;
		bool onScreen = positioned && row + placement->rows > 0
			&& row < rows;
		bool selected = false;

		switch (lower) {
			case 'a':
				selected = onScreen && !placement->isVirtual;
				break;
			case 'i':
				selected = placement->imageId == command.id
					&& (command.placement == 0
						|| placement->placementId == command.placement);
				break;
			case 'n':
			{
				Image* image = _FindImageByNumber(command.number);
				selected = image != NULL && placement->imageId == image->id
					&& (command.placement == 0
						|| placement->placementId == command.placement);
				break;
			}
			case 'c':
			case 'p':
			case 'q':
			{
				if (!positioned || placement->isVirtual)
					break;
				int32 x;
				int32 y;
				if (lower == 'c') {
					Terminal::CursorInfo cursor = fTerminal->Cursor();
					x = cursor.x;
					y = cursor.y;
				} else {
					x = (int32)command.x - 1;
					y = (int32)command.y - 1;
				}
				selected = x >= column && x < column + placement->columns
					&& y >= row && y < row + placement->rows
					&& (lower != 'q' || placement->z == command.z);
				break;
			}
			case 'r':
				selected = placement->imageId >= command.x
					&& placement->imageId <= command.y;
				break;
			case 'x':
				selected = positioned && !placement->isVirtual
					&& (int32)command.x - 1 >= column
					&& (int32)command.x - 1 < column + placement->columns;
				break;
			case 'y':
				selected = positioned && !placement->isVirtual
					&& (int32)command.y - 1 >= row
					&& (int32)command.y - 1 < row + placement->rows;
				break;
			case 'z':
				selected = positioned && !placement->isVirtual
					&& placement->z == command.z;
				break;
		}
		if (selected)
			victims.push_back(placement);
	}

	for (size_t i = 0; i < victims.size(); i++) {
		// a victim may have gone with its parent already
		if (std::find(store.placements.begin(), store.placements.end(),
				victims[i]) != store.placements.end()) {
			_DeletePlacement(victims[i], freeData);
		}
	}

	if (freeData) {
		if (lower == 'i') {
			if (command.placement == 0)
				_DeleteImage(command.id);
		} else if (lower == 'n') {
			Image* image = _FindImageByNumber(command.number);
			if (image != NULL && command.placement == 0)
				_DeleteImage(image->id);
		} else if (lower == 'r') {
			std::vector<uint32> ids;
			for (std::map<uint32, Image*>::iterator it = store.images.begin();
					it != store.images.end(); it++) {
				if (it->first >= command.x && it->first <= command.y)
					ids.push_back(it->first);
			}
			for (size_t i = 0; i < ids.size(); i++)
				_DeleteImage(ids[i]);
		}
	}

	if (lower == 'f') {
		// the animation frames (all but the root frame)
		Image* image = command.id != 0 ? _FindImage(command.id)
			: _FindImageByNumber(command.number);
		if (image != NULL && image->frames.size() > 1) {
			image->frames.resize(1);
			image->currentFrame = 0;
			image->animationState = 1;
			_Changed();
		}
	}

	_Changed();
}


void
TerminalGraphics::_RemoveUnusedImages()
{
	// images without placements whose id the program never saw
	Store& store = _Store();
	std::vector<uint32> unused;
	for (std::map<uint32, Image*>::iterator it = store.images.begin();
			it != store.images.end(); it++) {
		if (it->second->clientId || it->second->number != 0)
			continue;
		bool used = false;
		for (size_t i = 0; i < store.placements.size(); i++) {
			if (store.placements[i]->imageId == it->first) {
				used = true;
				break;
			}
		}
		if (!used)
			unused.push_back(it->first);
	}
	for (size_t i = 0; i < unused.size(); i++)
		_DeleteImage(unused[i]);
}


// #pragma mark - animation


void
TerminalGraphics::_Frame(Command& command, const std::string& payload)
{
	Image* image = command.id != 0 ? _FindImage(command.id)
		: command.number != 0 ? _FindImageByNumber(command.number) : NULL;
	if (image == NULL) {
		_Reply(command, "ENOENT:No such image");
		return;
	}
	command.assignedId = image->id;

	// the data rectangle defaults to the whole image
	Command data = command;
	if (data.width == 0)
		data.width = image->width;
	if (data.height == 0)
		data.height = image->height;

	std::vector<uint8> pixels;
	int32 width = 0;
	int32 height = 0;
	std::string error;
	if (!_LoadData(data, payload, pixels, width, height, error)) {
		_Reply(command, error.c_str());
		return;
	}

	int32 x = command.x;
	int32 y = command.y;
	if (x < 0 || y < 0 || x + width > image->width
		|| y + height > image->height) {
		_Reply(command, "EINVAL:Frame data outside of the image");
		return;
	}

	std::shared_ptr<ImageFrame> frame;
	if (command.r > 0) {
		// edit an existing frame
		if (command.r > image->frames.size()) {
			_Reply(command, "ENOENT:No such frame");
			return;
		}
		frame.reset(new ImageFrame(*image->frames[command.r - 1]));
	} else {
		frame.reset(new ImageFrame);
		if (command.c > 0) {
			if (command.c > image->frames.size()) {
				_Reply(command, "ENOENT:No such frame");
				return;
			}
			frame->pixels = image->frames[command.c - 1]->pixels;
		} else {
			// a canvas in the background color, 0xRRGGBBAA
			frame->pixels.resize((size_t)image->width * image->height * 4);
			uint32 color = command.Y;
			for (size_t i = 0; i < frame->pixels.size(); i += 4) {
				frame->pixels[i] = color >> 24;
				frame->pixels[i + 1] = (color >> 16) & 0xff;
				frame->pixels[i + 2] = (color >> 8) & 0xff;
				frame->pixels[i + 3] = color & 0xff;
			}
		}
		frame->gap = kDefaultFrameGap;
	}

	// composite the data onto the canvas
	bool replace = command.X == 1;
	for (int32 row = 0; row < height; row++) {
		uint8* destination = frame->pixels.data()
			+ ((size_t)(y + row) * image->width + x) * 4;
		const uint8* source = pixels.data() + (size_t)row * width * 4;
		for (int32 column = 0; column < width; column++) {
			uint8* d = destination + column * 4;
			const uint8* s = source + column * 4;
			if (replace || s[3] == 255) {
				memcpy(d, s, 4);
			} else if (s[3] != 0) {
				uint32 alpha = s[3];
				uint32 outAlpha = alpha + d[3] * (255 - alpha) / 255;
				for (int i = 0; i < 3; i++) {
					d[i] = outAlpha == 0 ? 0
						: (s[i] * alpha + d[i] * d[3] * (255 - alpha) / 255)
							/ outAlpha;
				}
				d[3] = outAlpha;
			}
		}
	}

	if (command.hasZ && command.z != 0)
		frame->gap = command.z;
	frame->serial = ++sFrameSerial;

	Store& store = _Store();
	if (command.r > 0)
		image->frames[command.r - 1] = frame;
	else {
		_EnforceQuota(frame->pixels.size());
		image->frames.push_back(frame);
		image->memory += frame->pixels.size();
		store.memory += frame->pixels.size();
	}
	_Reply(command, "OK");
	_Changed();
}


void
TerminalGraphics::_AnimationControl(Command& command)
{
	Image* image = command.id != 0 ? _FindImage(command.id)
		: command.number != 0 ? _FindImageByNumber(command.number) : NULL;
	if (image == NULL) {
		_Reply(command, "ENOENT:No such image");
		return;
	}
	command.assignedId = image->id;

	if (command.r > 0 && command.hasZ && command.z != 0
		&& command.r <= image->frames.size()) {
		std::shared_ptr<ImageFrame> frame(
			new ImageFrame(*image->frames[command.r - 1]));
		frame->gap = command.z;
		image->frames[command.r - 1] = frame;
	}
	if (command.c > 0 && command.c <= image->frames.size()) {
		image->currentFrame = command.c - 1;
		image->frameStart = system_time();
	}
	if (command.width != 0) {
		// s: 1 stop, 2 loading, 3 loop
		image->animationState = command.width;
		if (command.width == 1)
			image->loopsDone = 0;
		image->frameStart = system_time();
	}
	if (command.height != 0) {
		// v: 1 = forever, N = N - 1 loops
		image->loops = command.height == 1 ? 0 : command.height - 1;
		image->loopsDone = 0;
	}
	_Changed();
}


void
TerminalGraphics::_Compose(Command& command)
{
	Image* image = command.id != 0 ? _FindImage(command.id)
		: command.number != 0 ? _FindImageByNumber(command.number) : NULL;
	if (image == NULL || command.r == 0 || command.c == 0
		|| command.r > image->frames.size()
		|| command.c > image->frames.size()) {
		_Reply(command, "ENOENT:No such image or frame");
		return;
	}
	command.assignedId = image->id;

	int32 width = command.w != 0 ? command.w : image->width;
	int32 height = command.h != 0 ? command.h : image->height;
	int32 sourceX = command.X;
	int32 sourceY = command.Y;
	int32 x = command.x;
	int32 y = command.y;
	if (sourceX + width > image->width || sourceY + height > image->height
		|| x + width > image->width || y + height > image->height) {
		_Reply(command, "EINVAL:Rectangle outside of the image");
		return;
	}
	if (command.r == command.c && sourceX < x + width && x < sourceX + width
		&& sourceY < y + height && y < sourceY + height) {
		_Reply(command, "EINVAL:Overlapping rectangles in the same frame");
		return;
	}

	const ImageFrame& source = *image->frames[command.r - 1];
	std::shared_ptr<ImageFrame> destination(
		new ImageFrame(*image->frames[command.c - 1]));
	bool replace = command.cursorPolicy == 1;
	for (int32 row = 0; row < height; row++) {
		for (int32 column = 0; column < width; column++) {
			const uint8* s = source.pixels.data()
				+ ((size_t)(sourceY + row) * image->width + sourceX + column)
					* 4;
			uint8* d = destination->pixels.data()
				+ ((size_t)(y + row) * image->width + x + column) * 4;
			if (replace || s[3] == 255)
				memcpy(d, s, 4);
			else if (s[3] != 0) {
				uint32 alpha = s[3];
				uint32 outAlpha = alpha + d[3] * (255 - alpha) / 255;
				for (int i = 0; i < 3; i++) {
					d[i] = outAlpha == 0 ? 0
						: (s[i] * alpha + d[i] * d[3] * (255 - alpha) / 255)
							/ outAlpha;
				}
				d[3] = outAlpha;
			}
		}
	}
	destination->serial = ++sFrameSerial;
	image->frames[command.c - 1] = destination;
	_Reply(command, "OK");
	_Changed();
}


bool
TerminalGraphics::HasAnimations() const
{
	const Store& store = fAlternateActive ? *fAlternate : *fMain;
	for (std::map<uint32, Image*>::const_iterator it = store.images.begin();
			it != store.images.end(); it++) {
		if (it->second->frames.size() > 1 && it->second->animationState > 1)
			return true;
	}
	return false;
}


bigtime_t
TerminalGraphics::Animate(bigtime_t now, bool& _changed)
{
	_changed = false;
	bigtime_t next = B_INFINITE_TIMEOUT;
	Store& store = _Store();

	for (std::map<uint32, Image*>::iterator it = store.images.begin();
			it != store.images.end(); it++) {
		Image* image = it->second;
		int32 count = image->frames.size();
		if (count < 2 || image->animationState < 2)
			continue;

		// only animate what is placed somewhere
		bool placed = false;
		for (size_t i = 0; i < store.placements.size(); i++) {
			if (store.placements[i]->imageId == image->id) {
				placed = true;
				break;
			}
		}
		if (!placed)
			continue;

		for (int32 guard = 0; guard < count * 2; guard++) {
			const ImageFrame& frame = *image->frames[image->currentFrame];
			int32 gap = image->currentFrame == 0 && frame.gap == 0
				? 0 : frame.gap;
			if (gap > 0 && now - image->frameStart < (bigtime_t)gap * 1000) {
				next = std::min(next,
					image->frameStart + (bigtime_t)gap * 1000);
				break;
			}

			int32 nextFrame = image->currentFrame + 1;
			if (nextFrame >= count) {
				if (image->animationState == 2) {
					// loading: wait for more frames
					break;
				}
				image->loopsDone++;
				if (image->loops > 0 && image->loopsDone >= image->loops) {
					image->animationState = 1;
					break;
				}
				nextFrame = 0;
			}
			image->frameStart = gap > 0
				? image->frameStart + (bigtime_t)gap * 1000 : now;
			if (now - image->frameStart > 1000000)
				image->frameStart = now;
			image->currentFrame = nextFrame;
			_changed = true;
		}
	}

	if (_changed)
		_Changed();
	return next;
}


// #pragma mark - terminal hooks


void
TerminalGraphics::Scrolled(int32 count, bool toHistory, int32 bottom)
{
	Store& store = _Store();
	if (store.placements.empty())
		return;

	if (toHistory && !fAlternateActive) {
		// the lines moved into the history; placements below a partial
		// scroll region stay where they are
		if (bottom >= 0) {
			int64 base = (int64)fTerminal->LinesScrolled() - count;
			for (size_t i = 0; i < store.placements.size(); i++) {
				Placement* placement = store.placements[i];
				if (placement->parentImage == 0 && !placement->isVirtual
					&& placement->line - base > bottom) {
					placement->line += count;
				}
			}
		}
		HistoryTrimmed();
		return;
	}

	RegionScrolled(0, bottom >= 0 ? bottom : fTerminal->Rows() - 1, 0,
		fTerminal->Columns() - 1, -count);
}


/*!	A scroll region moved by \a count rows (negative: up). Placements
	entirely inside it move along; those that leave it are cut off.
*/
void
TerminalGraphics::RegionScrolled(int32 top, int32 bottom, int32 left,
	int32 right, int32 count)
{
	Store& store = _Store();
	bool changed = false;
	std::vector<Placement*> gone;
	for (size_t i = 0; i < store.placements.size(); i++) {
		Placement* placement = store.placements[i];
		if (placement->parentImage != 0 || placement->isVirtual)
			continue;
		int32 row = _PlacementRow(*placement);
		if (row < top || row + placement->rows - 1 > bottom
			|| placement->column < left
			|| placement->column + placement->columns - 1 > right) {
			continue;
		}

		int32 newRow = row + count;
		placement->line += count;
		changed = true;

		if (newRow + placement->rows - 1 < top || newRow > bottom) {
			gone.push_back(placement);
			continue;
		}

		// cut what left the region
		int32 cellHeight = std::max((int32)1, fTerminal->CellHeight());
		if (newRow < top && placement->drawHeight > 0) {
			int32 cutRows = top - newRow;
			int32 cutPixels = cutRows * cellHeight - placement->drawY;
			if (cutPixels > 0) {
				int32 sourceCut = (int32)((int64)cutPixels
					* placement->sourceHeight / placement->drawHeight);
				placement->sourceY += sourceCut;
				placement->sourceHeight -= sourceCut;
				placement->drawHeight -= cutPixels;
				placement->drawY = 0;
			} else
				placement->drawY -= cutRows * cellHeight;
			placement->line += cutRows;
			placement->rows -= cutRows;
		} else if (newRow + placement->rows - 1 > bottom
			&& placement->drawHeight > 0) {
			int32 keepRows = bottom - newRow + 1;
			int32 keepPixels = keepRows * cellHeight - placement->drawY;
			if (keepPixels < placement->drawHeight) {
				placement->sourceHeight = (int32)((int64)keepPixels
					* placement->sourceHeight / placement->drawHeight);
				placement->drawHeight = keepPixels;
			}
			placement->rows = keepRows;
		}
		if (placement->drawHeight <= 0 || placement->sourceHeight <= 0)
			gone.push_back(placement);
	}

	for (size_t i = 0; i < gone.size(); i++)
		_DeletePlacement(gone[i], false);
	_RemoveUnusedImages();
	if (changed)
		_Changed();
}


void
TerminalGraphics::HistoryTrimmed()
{
	// placements that left the top of the history
	Store& store = _Store();
	int32 first = -fTerminal->HistorySize();
	std::vector<Placement*> gone;
	for (size_t i = 0; i < store.placements.size(); i++) {
		Placement* placement = store.placements[i];
		if (placement->parentImage != 0 || placement->isVirtual)
			continue;
		if (_PlacementRow(*placement) + placement->rows <= first)
			gone.push_back(placement);
	}
	for (size_t i = 0; i < gone.size(); i++)
		_DeletePlacement(gone[i], false);
	if (!gone.empty())
		_RemoveUnusedImages();
}


void
TerminalGraphics::ScreenCleared()
{
	// clearing the screen clears the images on it (so "clear" works)
	Store& store = _Store();
	int32 rows = fTerminal->Rows();
	std::vector<Placement*> gone;
	for (size_t i = 0; i < store.placements.size(); i++) {
		Placement* placement = store.placements[i];
		if (placement->isVirtual)
			continue;
		int32 row = _PlacementRow(*placement);
		if (row + placement->rows > 0 && row < rows)
			gone.push_back(placement);
	}
	for (size_t i = 0; i < gone.size(); i++) {
		if (std::find(store.placements.begin(), store.placements.end(),
				gone[i]) != store.placements.end()) {
			_DeletePlacement(gone[i], false);
		}
	}
	_RemoveUnusedImages();
	_Changed();
}


void
TerminalGraphics::HistoryCleared()
{
	Store& store = _Store();
	std::vector<Placement*> gone;
	for (size_t i = 0; i < store.placements.size(); i++) {
		Placement* placement = store.placements[i];
		if (placement->isVirtual)
			continue;
		if (_PlacementRow(*placement) + placement->rows <= 0)
			gone.push_back(placement);
	}
	for (size_t i = 0; i < gone.size(); i++) {
		if (std::find(store.placements.begin(), store.placements.end(),
				gone[i]) != store.placements.end()) {
			_DeletePlacement(gone[i], false);
		}
	}
	_RemoveUnusedImages();
	_Changed();
}


void
TerminalGraphics::ScreenSwitched(bool alternate)
{
	fAlternateActive = alternate;
	fLoading = false;
	fLoadData.clear();
	if (alternate) {
		// the alternate screen starts out empty, images too
		fAlternate->Clear();
	}
	_Changed();
}


void
TerminalGraphics::Reset()
{
	fMain->Clear();
	fAlternate->Clear();
	fAlternateActive = false;
	fLoading = false;
	fLoadData.clear();
	_Changed();
}


void
TerminalGraphics::Resized(int32 oldColumns, int32 oldRows)
{
	// recompute the cells the placements cover (the cell size may have
	// changed too)
	for (int32 i = 0; i < 2; i++) {
		Store& store = i == 0 ? *fMain : *fAlternate;
		for (size_t j = 0; j < store.placements.size(); j++) {
			Placement* placement = store.placements[j];
			std::map<uint32, Image*>::iterator found
				= store.images.find(placement->imageId);
			if (found != store.images.end()) {
				placement->sourceWidth = placement->sourceWidth;
				_ComputeExtent(*placement, *found->second);
			}
		}
	}
	_Changed();
}


void
TerminalGraphics::Reflowed(const std::vector<int64>& lineMap, int64 oldBase,
	int64 newBase)
{
	for (size_t i = 0; i < fMain->placements.size(); i++) {
		Placement* placement = fMain->placements[i];
		if (placement->parentImage != 0 || placement->isVirtual)
			continue;
		int64 index = placement->line - oldBase;
		if (index >= 0 && index < (int64)lineMap.size())
			placement->line = newBase + lineMap[index];
		else if (!lineMap.empty() && index >= (int64)lineMap.size()) {
			placement->line = newBase + lineMap.back()
				+ (index - (int64)lineMap.size() + 1);
		}
	}
	_Changed();
}


// #pragma mark - drawing


bool
TerminalGraphics::HasPlacements() const
{
	const Store& store = fAlternateActive ? *fAlternate : *fMain;
	return !store.placements.empty();
}


void
TerminalGraphics::CollectDraws(int32 firstRow, int32 rowCount,
	std::vector<ImageDraw>& draws)
{
	Store& store = _Store();
	int32 cellWidth = std::max((int32)1, fTerminal->CellWidth());
	int32 cellHeight = std::max((int32)1, fTerminal->CellHeight());

	for (size_t i = 0; i < store.placements.size(); i++) {
		const Placement& placement = *store.placements[i];
		if (placement.isVirtual || placement.drawWidth <= 0
			|| placement.drawHeight <= 0) {
			continue;
		}
		int64 line;
		int32 column;
		if (!_PlacementPosition(placement, line, column))
			continue;
		int64 base = fAlternateActive ? 0
			: (int64)fTerminal->LinesScrolled();
		int32 row = (int32)(line - base);
		if (row + placement.rows <= firstRow || row >= firstRow + rowCount)
			continue;

		std::map<uint32, Image*>::iterator found
			= store.images.find(placement.imageId);
		if (found == store.images.end())
			continue;
		const Image& image = *found->second;

		ImageDraw draw;
		draw.imageId = image.id;
		draw.frame = image.CurrentFrame();
		draw.imageWidth = image.width;
		draw.imageHeight = image.height;
		draw.sourceX = placement.sourceX;
		draw.sourceY = placement.sourceY;
		draw.sourceWidth = placement.sourceWidth;
		draw.sourceHeight = placement.sourceHeight;
		draw.x = column * cellWidth + placement.drawX;
		draw.y = (row - firstRow) * cellHeight + placement.drawY;
		draw.width = placement.drawWidth;
		draw.height = placement.drawHeight;
		draw.z = placement.z;
		draws.push_back(draw);
	}
}


/*!	The images Unicode placeholders show: each U+10EEEE cell names an image
	(foreground color, third diacritic), a placement (underline color) and
	the row and column of the image's cell grid it shows (first and second
	diacritic), where missing numbers continue the cell to the left.
*/
void
TerminalGraphics::CollectPlaceholderDraws(const TerminalCell* cells,
	int32 columns, int32 viewRow, std::vector<ImageDraw>& draws)
{
	Store& store = _Store();
	int32 cellWidth = std::max((int32)1, fTerminal->CellWidth());
	int32 cellHeight = std::max((int32)1, fTerminal->CellHeight());

	bool previousValid = false;
	uint32 previousImage = 0;
	uint32 previousPlacement = 0;
	int32 previousRow = 0;
	int32 previousColumn = 0;
	int32 previousHigh = 0;
	TermColor previousForeground = 0;
	TermColor previousUnderline = 0;

	for (int32 x = 0; x < columns; x++) {
		const TerminalCell& cell = cells[x];
		uint32 codePoints[8];
		int32 count = fTerminal->GetCharacter(cell.character, codePoints, 8);
		if (count < 1 || codePoints[0] != kPlaceholderCharacter) {
			previousValid = false;
			continue;
		}

		// the image id: foreground color (24 or 8 bits)
		TermColor foreground = cell.style.foreground;
		uint32 imageId = 0;
		if (is_direct_color(foreground))
			imageId = foreground & 0xffffff;
		else if (is_indexed_color(foreground))
			imageId = foreground & 0xff;
		TermColor underline = cell.style.underlineColor;
		uint32 placementId = 0;
		if (is_direct_color(underline))
			placementId = underline & 0xffffff;
		else if (is_indexed_color(underline))
			placementId = underline & 0xff;

		int32 values[3] = { -1, -1, -1 };
		int32 valueCount = 0;
		for (int32 i = 1; i < count && valueCount < 3; i++) {
			int32 value = placeholder_diacritic_value(codePoints[i]);
			if (value < 0)
				break;
			values[valueCount++] = value;
		}

		bool sameColors = previousValid
			&& previousForeground == foreground
			&& previousUnderline == underline;
		int32 row = values[0];
		int32 column = values[1];
		int32 high = values[2];
		if (valueCount == 0 && sameColors) {
			row = previousRow;
			column = previousColumn + 1;
			high = previousHigh;
		} else if (valueCount == 1 && sameColors && previousRow == row) {
			column = previousColumn + 1;
			high = previousHigh;
		} else if (valueCount == 2 && sameColors && previousRow == row
			&& previousColumn + 1 == column) {
			high = previousHigh;
		}
		if (row < 0)
			row = 0;
		if (column < 0)
			column = 0;
		if (high < 0)
			high = 0;
		imageId |= (uint32)high << 24;

		previousValid = true;
		previousImage = imageId;
		previousPlacement = placementId;
		previousRow = row;
		previousColumn = column;
		previousHigh = high;
		previousForeground = foreground;
		previousUnderline = underline;

		std::map<uint32, Image*>::iterator found
			= store.images.find(imageId);
		if (found == store.images.end())
			continue;
		const Image& image = *found->second;

		const Placement* placement = NULL;
		for (size_t i = 0; i < store.placements.size(); i++) {
			const Placement* candidate = store.placements[i];
			if (candidate->imageId == imageId && candidate->isVirtual
				&& (placementId == 0
					|| candidate->placementId == placementId)) {
				placement = candidate;
				break;
			}
		}
		if (placement == NULL || placement->sourceWidth <= 0
			|| placement->sourceHeight <= 0) {
			continue;
		}

		// the image fitted into the placement's box of cells
		int32 boxColumns = placement->requestedColumns > 0
			? placement->requestedColumns : placement->columns;
		int32 boxRows = placement->requestedRows > 0
			? placement->requestedRows : placement->rows;
		double boxWidth = (double)boxColumns * cellWidth;
		double boxHeight = (double)boxRows * cellHeight;
		double scale = std::min(boxWidth / placement->sourceWidth,
			boxHeight / placement->sourceHeight);
		double fittedWidth = placement->sourceWidth * scale;
		double fittedHeight = placement->sourceHeight * scale;
		double fittedX = (boxWidth - fittedWidth) / 2;
		double fittedY = (boxHeight - fittedHeight) / 2;

		// this cell's part of it
		double cellLeft = (double)column * cellWidth;
		double cellTop = (double)row * cellHeight;
		double left = std::max(cellLeft, fittedX);
		double top = std::max(cellTop, fittedY);
		double right = std::min(cellLeft + cellWidth, fittedX + fittedWidth);
		double bottom = std::min(cellTop + cellHeight,
			fittedY + fittedHeight);
		if (right <= left || bottom <= top)
			continue;

		ImageDraw draw;
		draw.imageId = image.id;
		draw.frame = image.CurrentFrame();
		draw.imageWidth = image.width;
		draw.imageHeight = image.height;
		draw.sourceX = placement->sourceX + (left - fittedX) / scale;
		draw.sourceY = placement->sourceY + (top - fittedY) / scale;
		draw.sourceWidth = (right - left) / scale;
		draw.sourceHeight = (bottom - top) / scale;
		draw.x = x * cellWidth + (int32)(left - cellLeft);
		draw.y = viewRow * cellHeight + (int32)(top - cellTop);
		draw.width = (int32)(right - left + 0.5);
		draw.height = (int32)(bottom - top + 0.5);
		draw.z = placement->z;

		// continue the previous draw where this cell extends it
		if (!draws.empty()) {
			ImageDraw& last = draws.back();
			if (last.imageId == draw.imageId && last.frame == draw.frame
				&& last.y == draw.y && last.height == draw.height
				&& last.x + last.width == draw.x
				&& last.sourceY == draw.sourceY
				&& fabsf(last.sourceX + last.sourceWidth - draw.sourceX)
					< 0.01f) {
				last.width += draw.width;
				last.sourceWidth += draw.sourceWidth;
				continue;
			}
		}
		draws.push_back(draw);
	}
	(void)previousImage;
	(void)previousPlacement;
}
