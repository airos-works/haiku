/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "TermDragAndDrop.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <Bitmap.h>
#include <DataIO.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Path.h>
#include <TranslationUtils.h>
#include <View.h>

#include <SHA256.h>


static const size_t kChunkSize = 3072;		// 4096 base64 characters
static const size_t kMaxPreSent = 64 * 1024 * 1024;
static const off_t kMaxFileSize = 1024LL * 1024 * 1024;
static const size_t kMaxDirectories = 256;


static const char*
error_name(int error)
{
	switch (error) {
		case ENOENT:
		case ENOTDIR:
			return "ENOENT";
		case EPERM:
		case EACCES:
			return "EPERM";
		case EIO:
			return "EIO";
		case EINVAL:
			return "EINVAL";
		case ENOMEM:
			return "ENOMEM";
		case EMFILE:
			return "EMFILE";
		case EFBIG:
			return "EFBIG";
	}
	return "EUNKNOWN";
}


static void
split(const std::string& text, std::vector<std::string>& _parts)
{
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find(' ', start);
		if (end == std::string::npos)
			end = text.size();
		if (end > start)
			_parts.push_back(text.substr(start, end - start));
		start = end + 1;
	}
}


static std::string
join(const std::vector<std::string>& parts)
{
	std::string text;
	for (size_t i = 0; i < parts.size(); i++) {
		if (i > 0)
			text += ' ';
		text += parts[i];
	}
	return text;
}


static std::string
url_for_path(const char* path)
{
	static const char kHex[] = "0123456789ABCDEF";
	std::string url = "file://";
	for (const uint8* c = (const uint8*)path; *c != '\0'; c++) {
		if (isalnum(*c) || strchr("/-._~", *c) != NULL)
			url += (char)*c;
		else {
			url += '%';
			url += kHex[*c >> 4];
			url += kHex[*c & 0xf];
		}
	}
	return url;
}


//!	The path of a file:// URL (any host), or an empty string.
static std::string
path_for_url(const std::string& url)
{
	if (url.compare(0, 7, "file://") != 0)
		return std::string();
	size_t start = url.find('/', 7);
	if (start == std::string::npos)
		return std::string();
	std::string path;
	for (size_t i = start; i < url.size(); i++) {
		if (url[i] == '%' && i + 2 < url.size() && isxdigit(url[i + 1])
			&& isxdigit(url[i + 2])) {
			path += (char)strtol(url.substr(i + 1, 2).c_str(), NULL, 16);
			i += 2;
		} else
			path += url[i];
	}
	return path;
}


//!	The types a drag carries: files as text/uri-list (and their paths as
//!	text/plain), and the data of each MIME type.
static void
drag_types(const BMessage* message, std::vector<std::string>& _types)
{
	entry_ref ref;
	bool files = message->FindRef("refs", &ref) == B_OK;
	if (files)
		_types.push_back("text/uri-list");

	char* name;
	type_code type;
	int32 count;
	for (int32 i = 0; message->GetInfo(B_MIME_TYPE, i, &name, &type, &count)
			== B_OK; i++) {
		if (strchr(name, '/') != NULL
			&& std::find(_types.begin(), _types.end(), name) == _types.end()) {
			_types.push_back(name);
		}
	}
	if (files && std::find(_types.begin(), _types.end(), "text/plain")
			== _types.end()) {
		_types.push_back("text/plain");
	}
}


//!	What a drag allows: 1 copy, 3 copy or move.
static int32
drag_operations(const BMessage* message)
{
	int32 action;
	for (int32 i = 0; message->FindInt32("be:actions", i, &action) == B_OK;
			i++) {
		if (action == B_MOVE_TARGET)
			return 3;
	}
	return 1;
}


static void
hmac_sha256(const std::string& key, const std::string& message,
	uint8 _digest[32])
{
	uint8 block[64];
	memset(block, 0, sizeof(block));
	if (key.size() > sizeof(block)) {
		SHA256 hash;
		hash.Init();
		hash.Update(key.data(), key.size());
		memcpy(block, hash.Digest(), 32);
	} else
		memcpy(block, key.data(), key.size());

	uint8 pad[64];
	for (int i = 0; i < 64; i++)
		pad[i] = block[i] ^ 0x36;
	SHA256 inner;
	inner.Init();
	inner.Update(pad, sizeof(pad));
	inner.Update(message.data(), message.size());
	uint8 innerDigest[32];
	memcpy(innerDigest, inner.Digest(), sizeof(innerDigest));

	for (int i = 0; i < 64; i++)
		pad[i] = block[i] ^ 0x5c;
	SHA256 outer;
	outer.Init();
	outer.Update(pad, sizeof(pad));
	outer.Update(innerDigest, sizeof(innerDigest));
	memcpy(_digest, outer.Digest(), 32);
}


TermDragAndDrop::TermDragAndDrop(BView* view, Writer writer)
	:
	fView(view),
	fWriter(writer),
	fChaining(false),
	fClientID(0),
	fAccepting(false),
	fDragInside(false),
	fDragColumn(-1),
	fDragRow(-1),
	fClientOperation(0),
	fDropped(false),
	fDropFromHere(false),
	fUriListIndex(0),
	fNextHandle(2),
	fOffering(false),
	fMouseDown(false),
	fGestureColumn(0),
	fGestureRow(0),
	fGestureX(0),
	fGestureY(0),
	fGestureSent(false),
	fOfferActive(false),
	fOfferOperations(0),
	fPreSentSize(0),
	fImageIndex(0),
	fDragStarted(false)
{
}


TermDragAndDrop::~TermDragAndDrop()
{
}


/*!	The machine id of kitty's protocol: a random id made once per
	installation (Haiku has no /etc/machine-id), hashed with HMAC-SHA-256
	under the protocol's key, in hex, after its version.
*/
/*static*/ const std::string&
TermDragAndDrop::MachineID()
{
	static std::string sID;
	if (!sID.empty())
		return sID;

	std::string raw;
	BPath path;
	if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK
		&& path.Append("Terminal/machine_id") == B_OK) {
		FILE* file = fopen(path.Path(), "r");
		if (file != NULL) {
			char buffer[128];
			if (fgets(buffer, sizeof(buffer), file) != NULL)
				raw = buffer;
			fclose(file);
		}
		while (!raw.empty() && isspace((uint8)raw[raw.size() - 1]))
			raw.erase(raw.size() - 1);
		if (raw.empty()) {
			uint8 random[16];
			int device = open("/dev/urandom", O_RDONLY);
			if (device >= 0) {
				read(device, random, sizeof(random));
				close(device);
			}
			for (size_t i = 0; i < sizeof(random); i++) {
				char hex[3];
				snprintf(hex, sizeof(hex), "%02x", random[i]);
				raw += hex;
			}
			file = fopen(path.Path(), "w");
			if (file != NULL) {
				fprintf(file, "%s\n", raw.c_str());
				fclose(file);
			}
		}
	}

	uint8 digest[32];
	hmac_sha256("tty-dnd-protocol-machine-id", raw, digest);
	sID = "1:";
	for (size_t i = 0; i < sizeof(digest); i++) {
		char hex[3];
		snprintf(hex, sizeof(hex), "%02x", digest[i]);
		sID += hex;
	}
	return sID;
}


/*!	An OSC 72 command: key=value metadata, then the payload. A chain of
	chunks (m=1) keeps the first chunk's metadata. Data streams (pre-sent
	data) are taken chunk by chunk; lists are put together first.
*/
void
TermDragAndDrop::Command(const std::string& data)
{
	size_t separator = data.find(';');
	std::string metadata = data.substr(0, separator);
	std::string payload = separator == std::string::npos
		? std::string() : data.substr(separator + 1);

	Keys keys;
	size_t start = 0;
	while (start < metadata.size()) {
		size_t end = metadata.find(':', start);
		if (end == std::string::npos)
			end = metadata.size();
		std::string pair = metadata.substr(start, end - start);
		if (pair.size() < 2 || pair[1] != '=')
			return;		// malformed: dropped, as kitty does
		keys[pair[0]] = pair.substr(2);
		start = end + 1;
	}

	bool more = keys.count('m') != 0 && keys['m'] == "1";
	if (fChaining) {
		Keys chained = fChainKeys;
		chained['m'] = more ? "1" : "0";
		keys = chained;
	}
	char type = keys.count('t') != 0 && !keys['t'].empty()
		? keys['t'][0] : 'a';

	if (type == 'p' || type == 'e' || type == 'k') {
		if (more && !fChaining) {
			fChaining = true;
			fChainKeys = keys;
		} else if (!more)
			fChaining = false;
		_Packet(keys, payload);
		return;
	}

	fChainText += payload;
	if (more) {
		if (!fChaining) {
			fChaining = true;
			fChainKeys = keys;
		}
		return;
	}
	fChaining = false;
	std::string text = fChainText;
	fChainText.clear();
	_Packet(keys, text);
}


void
TermDragAndDrop::_Packet(const Keys& keys, const std::string& payload)
{
	char type = keys.count('t') != 0 && !keys.at('t').empty()
		? keys.at('t')[0] : 'a';
	int32 x = keys.count('x') != 0 ? atoi(keys.at('x').c_str()) : 0;
	int32 y = keys.count('y') != 0 ? atoi(keys.at('y').c_str()) : 0;
	int32 operation = keys.count('o') != 0 ? atoi(keys.at('o').c_str()) : 0;
	if ((type == 'a' || type == 'o' || type == 'q') && keys.count('i') != 0)
		fClientID = atoi(keys.at('i').c_str());

	switch (type) {
		case 'q':
			_Send("t=q");
			break;

		case 'a':
			if (x == 1) {
				// the program's machine: other than ours, it is remote and
				// reads dropped files through the terminal
				fClientMachine = payload;
				while (!fClientMachine.empty()
					&& isspace((uint8)fClientMachine.back())) {
					fClientMachine.erase(fClientMachine.size() - 1);
				}
			} else
				fAccepting = true;
			break;

		case 'A':
			fAccepting = false;
			_EndDrop();
			break;

		case 'm':
			// the program accepts (copy 1, move 2) or rejects (0) the drag
			fClientOperation = operation;
			break;

		case 'r':
			_DropRequest(keys);
			break;

		case 'o':
			if (x == 1)
				fOffering = true;
			else if (x == 2) {
				fOffering = false;
				_EndOffer();
			} else if (!fMouseDown || !fGestureSent || fDragStarted) {
				_Send("t=E", "EPERM");
			} else {
				_EndOffer();
				fOfferActive = true;
				fOfferOperations = operation;
				split(payload, fOfferTypes);
			}
			break;

		case 'p':
			_PreSent(keys, payload, payload.empty());
			break;

		case 'P':
			if (x >= 0)
				fImageIndex = x;
			else if (x == -1)
				_StartDrag();
			break;

		case 'E':
			// the program cancels its drag (y=-1) or cannot give data
			if (y == -1)
				_EndOffer();
			break;

		default:
			// t=e and t=k: data the terminal only asks for in remote drags,
			// which it does not do
			break;
	}
}


// #pragma mark - drops


void
TermDragAndDrop::DragMoved(int32 column, int32 row, int32 pixelX,
	int32 pixelY, const BMessage* message)
{
	if (!fAccepting)
		return;

	std::vector<std::string> types;
	drag_types(message, types);
	bool entered = !fDragInside;
	bool typesChanged = types != fDragTypes;
	if (!entered && !typesChanged && column == fDragColumn
		&& row == fDragRow) {
		return;
	}
	if (entered) {
		// a new drag: nothing accepted yet
		_EndDrop();
		fClientOperation = 0;
	}
	fDragInside = true;
	fDragColumn = column;
	fDragRow = row;

	char keys[128];
	snprintf(keys, sizeof(keys), "t=m:x=%d:y=%d:X=%d:Y=%d:o=%d",
		(int)std::max(column, (int32)0), (int)std::max(row, (int32)0),
		(int)pixelX, (int)pixelY, (int)drag_operations(message));
	if (entered || typesChanged) {
		// the types only when they are new
		fDragTypes = types;
		_Send(keys, join(types));
	} else
		_Send(keys);
}


void
TermDragAndDrop::DragExited()
{
	if (!fAccepting || !fDragInside)
		return;
	fDragInside = false;
	fDragTypes.clear();
	_Send("t=m:x=-1:y=-1");
	_EndDrop();
}


bool
TermDragAndDrop::Dropped(const BMessage* message, int32 column, int32 row,
	int32 pixelX, int32 pixelY)
{
	if (!fAccepting)
		return false;

	fDragInside = false;
	fDragTypes.clear();
	if (fClientOperation == 0) {
		// not accepted: the drop does not happen
		_Send("t=m:x=-1:y=-1");
		_EndDrop();
		return true;
	}

	_EndDrop();
	fDropMessage = *message;
	drag_types(message, fDropTypes);
	entry_ref ref;
	for (int32 i = 0; message->FindRef("refs", i, &ref) == B_OK; i++) {
		BPath path(&ref);
		if (path.InitCheck() == B_OK)
			fDropPaths.push_back(path.Path());
	}
	void* source;
	fDropFromHere = message->FindPointer("_terminal:dnd-source", &source)
			== B_OK && source == this;
	fDropped = true;

	char keys[128];
	snprintf(keys, sizeof(keys), "t=M:x=%d:y=%d:X=%d:Y=%d:o=%d",
		(int)std::max(column, (int32)0), (int)std::max(row, (int32)0),
		(int)pixelX, (int)pixelY, (int)drag_operations(message));
	_Send(keys, join(fDropTypes));
	return true;
}


/*!	t=r: the data of a dropped type (x, from 1), an entry of the dropped
	text/uri-list (y), an entry of a directory sent before (Y); or, without
	x, the end of the drop (or of a directory).
*/
void
TermDragAndDrop::_DropRequest(const Keys& keys)
{
	bool hasX = keys.count('x') != 0;
	bool hasY = keys.count('y') != 0;
	bool hasHandle = keys.count('Y') != 0;
	int32 x = hasX ? atoi(keys.at('x').c_str()) : 0;
	int32 y = hasY ? atoi(keys.at('y').c_str()) : 0;
	int32 handle = hasHandle ? atoi(keys.at('Y').c_str()) : 0;

	if (!hasX && hasHandle) {
		fDirectories.erase(handle);
		return;
	}
	if (!hasX) {
		// done: copied (o=1), moved (o=2) or cancelled
		_EndDrop();
		return;
	}

	char id[96];
	if (hasHandle)
		snprintf(id, sizeof(id), "t=r:Y=%d:x=%d", (int)handle, (int)x);
	else if (hasY)
		snprintf(id, sizeof(id), "t=r:x=%d:y=%d", (int)x, (int)y);
	else
		snprintf(id, sizeof(id), "t=r:x=%d", (int)x);

	if (!fDropped) {
		_SendError(id, "EPERM");
		return;
	}

	if (hasHandle) {
		std::map<int32, Directory>::iterator found = fDirectories.find(handle);
		if (found == fDirectories.end())
			_SendError(id, "EINVAL");
		else if (x < 1 || x > (int32)found->second.entries.size())
			_SendError(id, "ENOENT");
		else if (fDropFromHere)
			_SendError(id, "EPERM");
		else {
			_SendEntry(id, found->second.path + "/"
				+ found->second.entries[x - 1]);
		}
		return;
	}

	if (x < 1 || x > (int32)fDropTypes.size()) {
		_SendError(id, "ENOENT");
		return;
	}

	if (hasY) {
		// files of the drop, for a program on another machine
		if (fUriListIndex != x)
			_SendError(id, "EINVAL");
		else if (y < 1 || y > (int32)fDropPaths.size())
			_SendError(id, "ENOENT");
		else if (fDropFromHere)
			_SendError(id, "EPERM");
		else
			_SendEntry(id, fDropPaths[y - 1]);
		return;
	}

	const std::string& type = fDropTypes[x - 1];
	std::string data;
	if (!_DropData(type, data)) {
		_SendError(id, "ENOENT");
		return;
	}
	if (type == "text/uri-list") {
		fUriListIndex = x;
		if (_IsRemote())
			snprintf(id, sizeof(id), "t=r:X=1:x=%d", (int)x);
	}
	_SendData(id, data);
}


bool
TermDragAndDrop::_DropData(const std::string& type, std::string& data) const
{
	if (type == "text/uri-list") {
		for (size_t i = 0; i < fDropPaths.size(); i++)
			data += url_for_path(fDropPaths[i].c_str()) + "\r\n";
		return !fDropPaths.empty();
	}

	const void* bytes;
	ssize_t size;
	if (fDropMessage.FindData(type.c_str(), B_MIME_TYPE, &bytes, &size)
			== B_OK) {
		data.assign((const char*)bytes, size);
		return true;
	}
	if (type == "text/plain" && !fDropPaths.empty()) {
		for (size_t i = 0; i < fDropPaths.size(); i++) {
			if (i > 0)
				data += '\n';
			data += fDropPaths[i];
		}
		return true;
	}
	return false;
}


//!	A file (its bytes), a symbolic link (X=1, its target) or a directory
//!	(X=handle, its entries).
void
TermDragAndDrop::_SendEntry(const std::string& keys, const std::string& path)
{
	struct stat st;
	if (lstat(path.c_str(), &st) != 0) {
		_SendError(keys, error_name(errno));
		return;
	}

	if (S_ISLNK(st.st_mode)) {
		char target[B_PATH_NAME_LENGTH];
		ssize_t length = readlink(path.c_str(), target, sizeof(target));
		if (length < 0) {
			_SendError(keys, error_name(errno));
			return;
		}
		_SendData(keys + ":X=1", std::string(target, length));
		return;
	}

	if (S_ISDIR(st.st_mode)) {
		if (fDirectories.size() >= kMaxDirectories) {
			_SendError(keys, "ENOMEM");
			return;
		}
		DIR* dir = opendir(path.c_str());
		if (dir == NULL) {
			_SendError(keys, error_name(errno));
			return;
		}
		Directory directory;
		directory.path = path;
		std::string list;
		while (struct dirent* entry = readdir(dir)) {
			if (strcmp(entry->d_name, ".") == 0
				|| strcmp(entry->d_name, "..") == 0) {
				continue;
			}
			struct stat entryStat;
			std::string entryPath = path + "/" + entry->d_name;
			if (lstat(entryPath.c_str(), &entryStat) != 0
				|| !(S_ISREG(entryStat.st_mode) || S_ISDIR(entryStat.st_mode)
					|| S_ISLNK(entryStat.st_mode))) {
				continue;
			}
			if (!directory.entries.empty())
				list += '\0';
			list += entry->d_name;
			directory.entries.push_back(entry->d_name);
		}
		closedir(dir);
		int32 handle = fNextHandle++;
		fDirectories[handle] = directory;
		char suffix[32];
		snprintf(suffix, sizeof(suffix), ":X=%d", (int)handle);
		_SendData(keys + suffix, list);
		return;
	}

	if (!S_ISREG(st.st_mode)) {
		_SendError(keys, "EINVAL");
		return;
	}
	if (st.st_size > kMaxFileSize) {
		_SendError(keys, "EFBIG");
		return;
	}
	int file = open(path.c_str(), O_RDONLY);
	if (file < 0) {
		_SendError(keys, error_name(errno));
		return;
	}
	char buffer[kChunkSize];
	while (true) {
		ssize_t bytesRead = read(file, buffer, sizeof(buffer));
		if (bytesRead < 0) {
			close(file);
			_SendError(keys, "EIO");
			return;
		}
		if (bytesRead == 0)
			break;
		std::string encoded;
		base64_encode(buffer, bytesRead, encoded);
		_Send(keys + ":m=1", encoded);
	}
	close(file);
	_Send(keys);
}


void
TermDragAndDrop::_EndDrop()
{
	fDropped = false;
	fDropMessage.MakeEmpty();
	fDropTypes.clear();
	fDropPaths.clear();
	fDropFromHere = false;
	fUriListIndex = 0;
	fDirectories.clear();
}


bool
TermDragAndDrop::_IsRemote() const
{
	// no id says the same machine, as in kitty
	return !fClientMachine.empty() && fClientMachine != MachineID();
}


// #pragma mark - drags


void
TermDragAndDrop::MouseDown(BPoint where, int32 column, int32 row,
	int32 pixelX, int32 pixelY)
{
	fMouseDown = true;
	fGesturePoint = where;
	fGestureColumn = column;
	fGestureRow = row;
	fGestureX = pixelX;
	fGestureY = pixelY;
	fGestureSent = false;
}


//!	Returns whether the mouse belongs to the program's drag gesture.
bool
TermDragAndDrop::MouseMoved(BPoint where)
{
	if (!fOffering || !fMouseDown)
		return false;
	if (!fGestureSent && (fabs(where.x - fGesturePoint.x) > 4
			|| fabs(where.y - fGesturePoint.y) > 4)) {
		fGestureSent = true;
		char keys[96];
		snprintf(keys, sizeof(keys), "t=o:x=%d:y=%d:X=%d:Y=%d",
			(int)fGestureColumn, (int)fGestureRow, (int)fGestureX,
			(int)fGestureY);
		_Send(keys);
	}
	return true;
}


void
TermDragAndDrop::MouseUp()
{
	if (fDragStarted) {
		// Haiku does not tell the source where its drag went: dropped and
		// finished when the button goes up
		_Send("t=e:x=3");
		_Send("t=e:x=4:y=0");
	}
	fMouseDown = false;
	fGestureSent = false;
	_EndOffer();
}


void
TermDragAndDrop::_PreSent(const Keys& keys, const std::string& payload,
	bool end)
{
	if (!fOfferActive || fDragStarted)
		return;

	int32 x = keys.count('x') != 0 ? atoi(keys.at('x').c_str()) : 0;
	if (x >= (int32)fOfferTypes.size()) {
		_Send("t=E", "EINVAL");
		_EndOffer();
		return;
	}

	std::string* target;
	if (x >= 0)
		target = &fPreSent[x];
	else {
		// a drag image: -1 the first
		int32 index = -x - 1;
		bool known = fImages.count(index) != 0;
		Image& image = fImages[index];
		if (!known) {
			image.format = keys.count('y') != 0 ? atoi(keys.at('y').c_str())
				: 32;
			image.width = keys.count('X') != 0 ? atoi(keys.at('X').c_str())
				: 0;
			image.height = keys.count('Y') != 0 ? atoi(keys.at('Y').c_str())
				: 0;
			image.opacity = keys.count('o') != 0
				? atoi(keys.at('o').c_str()) : 0;
			image.complete = false;
		}
		if (end) {
			image.complete = true;
			return;
		}
		target = &image.data;
	}
	if (end)
		return;

	size_t before = target->size();
	if (!fDecoders[x].Decode(payload.data(), payload.size(), *target)) {
		_Send("t=E", "EINVAL");
		_EndOffer();
		return;
	}
	fPreSentSize += target->size() - before;
	if (fPreSentSize > kMaxPreSent) {
		_Send("t=E", "EFBIG");
		_EndOffer();
	}
}


//!	t=P:x=-1: the drag starts, with the data sent before (files of a
//!	text/uri-list as references) and the chosen image.
void
TermDragAndDrop::_StartDrag()
{
	if (!fOfferActive || !fMouseDown || fDragStarted) {
		_Send("t=E", "EPERM");
		_EndOffer();
		return;
	}

	BMessage drag(B_MIME_DATA);
	for (size_t i = 0; i < fOfferTypes.size(); i++) {
		std::map<int32, std::string>::iterator found = fPreSent.find(i);
		if (found == fPreSent.end())
			continue;
		const std::string& type = fOfferTypes[i];
		const std::string& data = found->second;
		drag.AddData(type.c_str(), B_MIME_TYPE, data.data(), data.size());
		if (type != "text/uri-list")
			continue;
		size_t start = 0;
		while (start < data.size()) {
			size_t end = data.find('\n', start);
			if (end == std::string::npos)
				end = data.size();
			std::string line = data.substr(start, end - start);
			if (!line.empty() && line.back() == '\r')
				line.erase(line.size() - 1);
			std::string path = path_for_url(line);
			entry_ref ref;
			if (!path.empty() && line[0] != '#'
				&& get_ref_for_path(path.c_str(), &ref) == B_OK) {
				drag.AddRef("refs", &ref);
				drag.what = B_SIMPLE_DATA;
			}
			start = end + 1;
		}
	}
	if ((fOfferOperations & 2) != 0) {
		drag.AddInt32("be:actions", B_COPY_TARGET);
		drag.AddInt32("be:actions", B_MOVE_TARGET);
	}
	drag.AddPointer("_terminal:dnd-source", this);

	BBitmap* image = _MakeImage(fImageIndex);
	if (image != NULL) {
		BRect bounds = image->Bounds();
		fView->DragMessage(&drag, image, B_OP_ALPHA,
			BPoint(bounds.Width() / 2, bounds.Height() / 2));
	} else {
		BRect frame(fGesturePoint, fGesturePoint);
		frame.InsetBy(-16, -10);
		fView->DragMessage(&drag, frame);
	}
	fDragStarted = true;
	_Send("t=E", "OK");
}


//!	The drag image: RGB (24), RGBA (32), PNG (100) or text drawn (0).
BBitmap*
TermDragAndDrop::_MakeImage(int32 index)
{
	std::map<int32, Image>::iterator found = fImages.find(index);
	if (found == fImages.end() || !found->second.complete)
		return NULL;
	const Image& image = found->second;

	if (image.format == 100) {
		BMemoryIO stream(image.data.data(), image.data.size());
		BBitmap* picture = BTranslationUtils::GetBitmap(&stream);
		if (picture == NULL)
			return NULL;
		BBitmap* rgba = new(std::nothrow) BBitmap(picture->Bounds(), 0,
			B_RGBA32);
		if (rgba == NULL || rgba->InitCheck() != B_OK
			|| rgba->ImportBits(picture) != B_OK) {
			delete rgba;
			rgba = NULL;
		}
		delete picture;
		return rgba;
	}

	if (image.format == 24 || image.format == 32) {
		int32 bytes = image.format / 8;
		if (image.width <= 0 || image.height <= 0
			|| (size_t)image.width * image.height * bytes
				!= image.data.size()) {
			_Send("t=E", "EINVAL");
			return NULL;
		}
		BBitmap* bitmap = new(std::nothrow) BBitmap(
			BRect(0, 0, image.width - 1, image.height - 1), 0, B_RGBA32);
		if (bitmap == NULL || bitmap->InitCheck() != B_OK) {
			delete bitmap;
			return NULL;
		}
		const uint8* source = (const uint8*)image.data.data();
		for (int32 y = 0; y < image.height; y++) {
			uint8* row = (uint8*)bitmap->Bits() + y * bitmap->BytesPerRow();
			for (int32 x = 0; x < image.width; x++, source += bytes) {
				row[x * 4 + 0] = source[2];
				row[x * 4 + 1] = source[1];
				row[x * 4 + 2] = source[0];
				row[x * 4 + 3] = bytes == 4 ? source[3] : 255;
			}
		}
		return bitmap;
	}

	if (image.format == 0 && !image.data.empty()) {
		// text, X/Y times the terminal's font size, on a background
		BFont font;
		fView->GetFont(&font);
		if (image.width > 0 && image.height > 0)
			font.SetSize(font.Size() * image.width / image.height);
		std::string text = image.data;
		std::replace(text.begin(), text.end(), '\n', ' ');
		font_height height;
		font.GetHeight(&height);
		int32 width = (int32)ceilf(font.StringWidth(text.c_str())) + 8;
		int32 lineHeight = (int32)ceilf(height.ascent + height.descent) + 4;

		BBitmap staging(BRect(0, 0, width - 1, lineHeight - 1),
			B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
		BBitmap* bitmap = new(std::nothrow) BBitmap(staging.Bounds(), 0,
			B_RGBA32);
		if (staging.InitCheck() != B_OK || bitmap == NULL
			|| bitmap->InitCheck() != B_OK) {
			delete bitmap;
			return NULL;
		}
		BView* view = new BView(staging.Bounds(), "text", B_FOLLOW_NONE, 0);
		staging.AddChild(view);
		staging.Lock();
		view->SetHighColor(0, 0, 0);
		view->FillRect(view->Bounds());
		view->SetHighColor(255, 255, 255);
		view->SetLowColor(0, 0, 0);
		view->SetFont(&font);
		view->DrawString(text.c_str(), BPoint(4, 2 + height.ascent));
		view->Sync();
		staging.Unlock();

		// the text in the view's color over the background's opacity
		rgb_color color = fView->HighColor();
		rgb_color background = fView->LowColor();
		uint32 opacity = std::min(1024, std::max(0, (int)image.opacity))
			* 255 / 1024;
		for (int32 y = 0; y < lineHeight; y++) {
			const uint8* source = (const uint8*)staging.Bits()
				+ y * staging.BytesPerRow();
			uint8* row = (uint8*)bitmap->Bits() + y * bitmap->BytesPerRow();
			for (int32 x = 0; x < width; x++) {
				uint32 coverage = source[x * 4 + 1];
				uint32 alpha = coverage + opacity * (255 - coverage) / 255;
				row[x * 4 + 0] = (color.blue * coverage
					+ background.blue * (255 - coverage)) / 255;
				row[x * 4 + 1] = (color.green * coverage
					+ background.green * (255 - coverage)) / 255;
				row[x * 4 + 2] = (color.red * coverage
					+ background.red * (255 - coverage)) / 255;
				row[x * 4 + 3] = alpha;
			}
		}
		return bitmap;
	}
	return NULL;
}


void
TermDragAndDrop::_EndOffer()
{
	fOfferActive = false;
	fOfferOperations = 0;
	fOfferTypes.clear();
	fPreSent.clear();
	fDecoders.clear();
	fPreSentSize = 0;
	fImages.clear();
	fImageIndex = 0;
	fDragStarted = false;
}


// #pragma mark - sending


void
TermDragAndDrop::_Send(const std::string& keys, const std::string& payload)
{
	std::string packet = "\x1b]72;" + keys;
	if (fClientID > 0) {
		char id[24];
		snprintf(id, sizeof(id), ":i=%d", (int)fClientID);
		packet += id;
	}
	if (!payload.empty())
		packet += ";" + payload;
	packet += "\x1b\\";
	fWriter(packet);
}


//!	Data in base64 chunks, then an empty packet: the end.
void
TermDragAndDrop::_SendData(const std::string& keys, const std::string& data)
{
	for (size_t start = 0; start < data.size(); start += kChunkSize) {
		std::string encoded;
		base64_encode(data.data() + start,
			std::min(kChunkSize, data.size() - start), encoded);
		_Send(keys + ":m=1", encoded);
	}
	_Send(keys);
}


void
TermDragAndDrop::_SendError(const std::string& keys, const char* error)
{
	// the keys of the request, with t=R
	std::string errorKeys = keys;
	if (errorKeys.compare(0, 3, "t=r") == 0)
		errorKeys[2] = 'R';
	_Send(errorKeys, error);
}
