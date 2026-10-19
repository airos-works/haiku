/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "TermFileTransfer.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <Alert.h>
#include <Catalog.h>
#include <Handler.h>
#include <Invoker.h>
#include <Looper.h>
#include <MessageRunner.h>
#include <String.h>
#include <Window.h>

#include <SHA256.h>
#include <zlib.h>

#include "Base64.h"
#include "PrefHandler.h"
#include "TermConst.h"
#include "TermCrypto.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "Terminal TermFileTransfer"


static const size_t kChunkSize = 4096;
static const size_t kMaxSessions = 10;
static const size_t kMaxFiles = 100000;
static const size_t kMaxBacklog = 256 * 1024;
static const bigtime_t kPumpDelay = 10000;
static const bigtime_t kSessionLifetime = 10 * 60 * 1000000LL;


//!	Only the characters of kitty's "safe strings".
static std::string
safe_string(const std::string& text)
{
	std::string safe;
	for (size_t i = 0; i < text.size(); i++) {
		char c = text[i];
		if (isalnum((unsigned char)c) || strchr("_:./@-", c) != NULL)
			safe += c;
	}
	return safe;
}


static std::string
field(const std::map<std::string, std::string>& fields, const char* key)
{
	std::map<std::string, std::string>::const_iterator found
		= fields.find(key);
	return found != fields.end() ? found->second : std::string();
}


static std::string
decode_base64(const std::string& text)
{
	std::string decoded;
	base64_decode(text.data(), text.size(), decoded);
	return decoded;
}


//!	Without padding: kitty's own programs take nothing else.
static std::string
encode_base64(const std::string& data)
{
	std::string encoded;
	base64_encode(data.data(), data.size(), encoded, false);
	return encoded;
}


static std::string
error_text(int error, const char* message = NULL)
{
	const char* name = "EIO";
	switch (error) {
		case ENOENT:
		case ENOTDIR:
			name = "ENOENT";
			break;
		case EPERM:
		case EACCES:
		case EROFS:
			name = "EPERM";
			break;
		case EISDIR:
			name = "EISDIR";
			break;
		case EINVAL:
			name = "EINVAL";
			break;
		case ENOSPC:
			name = "ENOSPC";
			break;
		case EEXIST:
			name = "EEXIST";
			break;
	}
	std::string text = name;
	text += ":";
	text += message != NULL ? message : strerror(error);
	return text;
}


//!	A path of the protocol: absolute, or ~/ for the home directory.
static bool
local_path(const std::string& path, std::string& _local)
{
	if (path.compare(0, 2, "~/") == 0 || path == "~") {
		const char* home = getenv("HOME");
		_local = std::string(home != NULL ? home : "/boot/home")
			+ path.substr(1);
	} else if (!path.empty() && path[0] == '/')
		_local = path;
	else
		return false;
	return _local.size() < B_PATH_NAME_LENGTH;
}


static void
split_path(const std::string& path, std::vector<std::string>& components)
{
	size_t start = 0;
	while (start < path.size()) {
		size_t end = path.find('/', start);
		if (end == std::string::npos)
			end = path.size();
		std::string component = path.substr(start, end - start);
		if (component == "..") {
			if (!components.empty())
				components.pop_back();
		} else if (!component.empty() && component != ".")
			components.push_back(component);
		start = end + 1;
	}
}


//!	An absolute path without "." and "..", as written (links not followed).
static std::string
normalize_path(const std::string& path)
{
	std::vector<std::string> components;
	split_path(path, components);
	std::string normalized;
	for (size_t i = 0; i < components.size(); i++)
		normalized += "/" + components[i];
	return normalized.empty() ? "/" : normalized;
}


//!	The path of \a target as seen from the directory \a from.
static std::string
relative_path(const std::string& from, const std::string& target)
{
	std::vector<std::string> fromComponents;
	std::vector<std::string> targetComponents;
	split_path(from, fromComponents);
	split_path(target, targetComponents);
	size_t common = 0;
	while (common < fromComponents.size()
		&& common < targetComponents.size()
		&& fromComponents[common] == targetComponents[common]) {
		common++;
	}
	std::string relative;
	for (size_t i = common; i < fromComponents.size(); i++)
		relative += "../";
	for (size_t i = common; i < targetComponents.size(); i++) {
		relative += targetComponents[i];
		if (i + 1 < targetComponents.size())
			relative += "/";
	}
	return relative.empty() ? "." : relative;
}


static std::string
directory_of(const std::string& path)
{
	size_t slash = path.rfind('/');
	if (slash == std::string::npos || slash == 0)
		return "/";
	return path.substr(0, slash);
}


//!	Creates the directories of a path that are not there yet.
static int
make_directories(const std::string& path)
{
	for (size_t slash = path.find('/', 1); slash != std::string::npos;
			slash = path.find('/', slash + 1)) {
		std::string parent = path.substr(0, slash);
		if (mkdir(parent.c_str(), 0755) != 0 && errno != EEXIST)
			return errno;
	}
	if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
		return errno;
	struct stat st;
	if (stat(path.c_str(), &st) != 0)
		return errno;
	return S_ISDIR(st.st_mode) ? 0 : ENOTDIR;
}


//!	Copies a file with its permissions and times; returns an errno.
static int
copy_file(const std::string& from, const std::string& to)
{
	int source = open(from.c_str(), O_RDONLY);
	if (source < 0)
		return errno;
	struct stat st;
	fstat(source, &st);
	int target = open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL,
		st.st_mode & 07777);
	if (target < 0) {
		int error = errno;
		close(source);
		return error;
	}
	int error = 0;
	char buffer[65536];
	ssize_t bytesRead;
	while ((bytesRead = read(source, buffer, sizeof(buffer))) > 0) {
		if (write(target, buffer, bytesRead) != bytesRead) {
			error = errno != 0 ? errno : ENOSPC;
			break;
		}
	}
	if (bytesRead < 0)
		error = errno;
	struct timespec times[2] = { st.st_atim, st.st_mtim };
	futimens(target, times);
	close(source);
	close(target);
	if (error != 0)
		unlink(to.c_str());
	return error;
}


static void
set_times(const std::string& path, int64 modified, bool link = false)
{
	if (modified < 0)
		return;
	struct timespec times[2];
	times[0].tv_sec = times[1].tv_sec = modified / 1000000000;
	times[0].tv_nsec = times[1].tv_nsec = modified % 1000000000;
	utimensat(AT_FDCWD, path.c_str(), times,
		link ? AT_SYMLINK_NOFOLLOW : 0);
}


static bool
deflate_data(z_stream* stream, const char* data, size_t length, bool finish,
	std::string& output)
{
	stream->next_in = (Bytef*)data;
	stream->avail_in = length;
	char buffer[16384];
	int result;
	do {
		stream->next_out = (Bytef*)buffer;
		stream->avail_out = sizeof(buffer);
		result = deflate(stream, finish ? Z_FINISH : Z_NO_FLUSH);
		if (result == Z_STREAM_ERROR)
			return false;
		output.append(buffer, sizeof(buffer) - stream->avail_out);
	} while (stream->avail_out == 0 || (finish && result != Z_STREAM_END));
	return true;
}


TermFileTransfer::File::File()
	:
	modified(-1),
	permissions(-1),
	fd(-1),
	inflater(NULL),
	written(0),
	failed(false),
	done(false)
{
}


TermFileTransfer::Session::Session()
	:
	sending(false),
	accepted(false),
	asked(false),
	listed(false),
	quiet(0),
	expected(0),
	lastActive(0),
	readFD(-1),
	deflater(NULL),
	readDone(false)
{
}


TermFileTransfer::TermFileTransfer(BHandler* target, Writer writer,
	Backlog backlog)
	:
	fTarget(target),
	fWriter(writer),
	fBacklog(backlog),
	fPumpScheduled(false)
{
}


TermFileTransfer::~TermFileTransfer()
{
	while (!fSessions.empty())
		_End(fSessions.begin()->first);
}


/*!	A command: fields of key=value, by ";". Sessions start with send or
	receive and wait for the user's answer before anything else.
*/
void
TermFileTransfer::Command(const std::string& data)
{
	_Expire();

	Fields fields;
	size_t start = 0;
	while (start < data.size()) {
		size_t end = data.find(';', start);
		if (end == std::string::npos)
			end = data.size();
		std::string pair = data.substr(start, end - start);
		size_t equal = pair.find('=');
		if (equal != std::string::npos)
			fields[pair.substr(0, equal)] = pair.substr(equal + 1);
		start = end + 1;
	}

	std::string action = field(fields, "ac");
	std::string id = safe_string(field(fields, "id"));
	if (id.empty())
		return;

	std::map<std::string, Session>::iterator found = fSessions.find(id);
	if (action == "send" || action == "receive") {
		// a new session of the same id ends the one before
		if (found != fSessions.end())
			_End(id);
		size_t count = 0;
		for (found = fSessions.begin(); found != fSessions.end(); found++) {
			if (found->second.sending == (action == "send"))
				count++;
		}
		if (count >= kMaxSessions)
			return;

		Session& session = fSessions[id];
		session.id = id;
		session.sending = action == "send";
		session.quiet = atoi(field(fields, "q").c_str());
		session.expected = std::max(0, atoi(field(fields, "sz").c_str()));
		session.lastActive = system_time();
		session.accepted = _CheckPassword(session, field(fields, "pw"));

		if (session.sending) {
			if (session.accepted)
				_Status(session, "", "OK");
			else
				_Ask(session);
		} else if (session.expected == 0) {
			_Status(session, "", "ENOENT:No files found", -1, true);
			_End(id);
		}
		return;
	}

	if (found == fSessions.end())
		return;
	Session& session = found->second;
	session.lastActive = system_time();

	if (action == "cancel") {
		_Status(session, "", "CANCELED");
		_End(id);
		return;
	}

	// a receive session's paths come before it is asked about
	if (!session.sending && !session.listed && !session.asked
		&& action == "file"
		&& (int32)session.requests.size() < session.expected) {
		session.requests.push_back(std::make_pair(
			safe_string(field(fields, "fid")),
			decode_base64(field(fields, "n"))));
		if ((int32)session.requests.size() == session.expected) {
			if (session.accepted) {
				_Status(session, "", "OK");
				_List(session);
			} else
				_Ask(session);
		}
		return;
	}

	if (!session.accepted || (!session.sending && !session.listed)) {
		// nothing may come before the answer
		_End(id);
		return;
	}

	if (action == "finish" || (action == "status" && !session.sending)) {
		if (session.sending)
			_Commit(session);
		_End(id);
	} else if (session.sending && action == "file")
		_SendFile(session, fields);
	else if (session.sending && (action == "data" || action == "end_data"))
		_SendData(session, fields, action == "end_data");
	else if (!session.sending && action == "file")
		_ReceiveFile(session, fields);
}


void
TermFileTransfer::Answer(const char* id, bool allowed)
{
	std::map<std::string, Session>::iterator found = fSessions.find(id);
	if (found == fSessions.end() || found->second.accepted)
		return;
	Session& session = found->second;
	if (!allowed) {
		_Status(session, "", "EPERM:User refused the transfer", -1, true);
		_End(id);
		return;
	}
	session.accepted = true;
	session.lastActive = system_time();
	_Status(session, "", "OK");
	if (!session.sending)
		_List(session);
}


void
TermFileTransfer::Pump()
{
	fPumpScheduled = false;
	size_t backlog = fBacklog();
	if (backlog >= kMaxBacklog) {
		_SchedulePump(true);
		return;
	}

	size_t budget = kMaxBacklog - backlog;
	bool more = false;
	std::vector<std::string> ids;
	for (std::map<std::string, Session>::iterator it = fSessions.begin();
			it != fSessions.end(); it++) {
		ids.push_back(it->first);
	}
	for (size_t i = 0; i < ids.size(); i++) {
		std::map<std::string, Session>::iterator found = fSessions.find(ids[i]);
		if (found != fSessions.end() && !found->second.sending
			&& _PumpSession(found->second, budget)) {
			more = true;
		}
	}
	if (more)
		_SchedulePump(budget == 0);
}


void
TermFileTransfer::_Ask(Session& session)
{
	session.asked = true;
	BString text;
	if (session.sending) {
		text = B_TRANSLATE("A program in Terminal wants to send files to "
			"this computer. They can be written anywhere you may write.");
	} else {
		text = B_TRANSLATE("A program in Terminal wants to read these files "
			"from this computer:");
		text << "\n";
		for (size_t i = 0; i < session.requests.size() && i < 10; i++)
			text << "\n" << session.requests[i].second.c_str();
		if (session.requests.size() > 10)
			text << "\n…";
	}
	BAlert* alert = new BAlert(B_TRANSLATE("File transfer"), text,
		B_TRANSLATE("Deny"), B_TRANSLATE("Allow"), NULL, B_WIDTH_AS_USUAL,
		B_WARNING_ALERT);
	alert->SetShortcut(0, B_ESCAPE);
	BWindow* window = dynamic_cast<BWindow*>(fTarget->Looper());
	if (window != NULL)
		alert->CenterIn(window->Frame());
	BMessage* answer = new BMessage(kFileTransferAnswer);
	answer->AddString("id", session.id.c_str());
	alert->Go(new BInvoker(answer, fTarget));
}


/*!	The settings' FileTransferPassword lets a session go without asking:
	kitty's programs encrypt "<time>:<id>;<password>" for the public key
	(kitty-1:), older ones send a SHA-256 hash of "<id>;<password>".
*/
bool
TermFileTransfer::_CheckPassword(const Session& session,
	const std::string& field) const
{
	const char* password = PrefHandler::Default()->getString(
		PREF_FILE_TRANSFER_PASSWORD);
	if (password == NULL || password[0] == '\0' || field.empty())
		return false;
	std::string bypass = decode_base64(field);
	std::string expected = session.id + ";" + password;

	if (bypass.compare(0, 8, "kitty-1:") == 0) {
		std::string plaintext;
		return TermCrypto::Decrypt(bypass.c_str() + 8, plaintext)
			&& TermCrypto::CheckTimestamped(plaintext, expected);
	}

	if (bypass.compare(0, 7, "sha256:") != 0 || bypass.size() != 7 + 64)
		return false;
	SHA256 hash;
	hash.Init();
	hash.Update(expected.data(), expected.size());
	const uint8* digest = hash.Digest();
	char hex[65];
	for (int i = 0; i < 32; i++)
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	// compared in constant time
	uint8 difference = 0;
	for (int i = 0; i < 64; i++)
		difference |= (uint8)(tolower(bypass[7 + i]) ^ hex[i]);
	return difference == 0;
}


void
TermFileTransfer::_End(const std::string& id)
{
	std::map<std::string, Session>::iterator found = fSessions.find(id);
	if (found == fSessions.end())
		return;
	Session& session = found->second;
	for (size_t i = 0; i < session.files.size(); i++) {
		File& file = session.files[i];
		if (file.fd >= 0)
			close(file.fd);
		if (file.inflater != NULL) {
			inflateEnd(file.inflater);
			delete file.inflater;
		}
		// what was not committed goes
		if (!file.temporaryPath.empty())
			unlink(file.temporaryPath.c_str());
	}
	while (!session.outgoing.empty())
		_EndOutgoing(session);
	fSessions.erase(found);
}


void
TermFileTransfer::_Expire()
{
	bigtime_t now = system_time();
	std::vector<std::string> expired;
	for (std::map<std::string, Session>::iterator it = fSessions.begin();
			it != fSessions.end(); it++) {
		if (now - it->second.lastActive > kSessionLifetime)
			expired.push_back(it->first);
	}
	for (size_t i = 0; i < expired.size(); i++)
		_End(expired[i]);
}


// #pragma mark - sending to here


void
TermFileTransfer::_SendFile(Session& session, const Fields& fields)
{
	File file;
	file.id = safe_string(field(fields, "fid"));
	file.type = field(fields, "ft");
	if (file.type.empty())
		file.type = "regular";
	if (fields.count("mod") != 0)
		file.modified = atoll(field(fields, "mod").c_str());
	if (fields.count("prm") != 0)
		file.permissions = atoi(field(fields, "prm").c_str());

	if (session.files.size() >= kMaxFiles) {
		_Status(session, file.id, "EINVAL:Too many files", -1, true);
		return;
	}
	if (!local_path(decode_base64(field(fields, "n")), file.path)) {
		_Status(session, file.id, "EINVAL:Not an absolute path", -1, true);
		return;
	}
	file.path = normalize_path(file.path);

	if (file.type == "directory") {
		int error = make_directories(file.path);
		if (error != 0) {
			_Status(session, file.id, error_text(error), -1, true);
			return;
		}
		file.done = true;
		session.files.push_back(file);
		_Status(session, file.id, "OK");
		return;
	}
	if (file.type != "regular" && file.type != "symlink"
		&& file.type != "link") {
		_Status(session, file.id, "EINVAL:Not a valid filetype", -1, true);
		return;
	}

	int error = make_directories(directory_of(file.path));
	if (error != 0) {
		_Status(session, file.id, error_text(error), -1, true);
		return;
	}
	if (file.type == "regular") {
		// written next to where it goes, and moved there at the end
		char suffix[64];
		snprintf(suffix, sizeof(suffix), ".transfer-%d-%d", (int)getpid(),
			(int)session.files.size());
		file.temporaryPath = file.path + suffix;
		file.fd = open(file.temporaryPath.c_str(),
			O_WRONLY | O_CREAT | O_TRUNC | O_EXCL, 0666);
		if (file.fd < 0) {
			file.temporaryPath.clear();
			_Status(session, file.id, error_text(errno), -1, true);
			return;
		}
		if (field(fields, "zip") == "zlib") {
			file.inflater = new z_stream;
			memset(file.inflater, 0, sizeof(z_stream));
			inflateInit(file.inflater);
		}
	}
	session.files.push_back(file);
	// without rsync: STARTED alone means the data comes as it is
	_Status(session, file.id, "STARTED");
}


void
TermFileTransfer::_SendData(Session& session, const Fields& fields, bool end)
{
	std::string id = safe_string(field(fields, "fid"));
	File* file = NULL;
	for (size_t i = 0; i < session.files.size(); i++) {
		if (session.files[i].id == id && !session.files[i].done)
			file = &session.files[i];
	}
	// data of files not STARTED, or that failed, is dropped
	if (file == NULL || file->failed)
		return;

	std::string data = decode_base64(field(fields, "d"));
	if (file->type == "symlink" || file->type == "link") {
		file->linkData += data;
		if (end) {
			file->done = true;
			_Status(session, id, "OK");
		}
		return;
	}

	if (file->inflater != NULL && !data.empty()) {
		std::string inflated;
		file->inflater->next_in = (Bytef*)data.data();
		file->inflater->avail_in = data.size();
		char buffer[16384];
		int result = Z_OK;
		while (file->inflater->avail_in > 0 && result == Z_OK) {
			file->inflater->next_out = (Bytef*)buffer;
			file->inflater->avail_out = sizeof(buffer);
			result = inflate(file->inflater, Z_NO_FLUSH);
			inflated.append(buffer,
				sizeof(buffer) - file->inflater->avail_out);
		}
		if (result != Z_OK && result != Z_STREAM_END) {
			file->failed = true;
			_Status(session, id, "EINVAL:Not valid zlib data", -1, true);
			return;
		}
		data.swap(inflated);
	}

	if (!data.empty()) {
		ssize_t written = write(file->fd, data.data(), data.size());
		if (written != (ssize_t)data.size()) {
			file->failed = true;
			_Status(session, id, error_text(written < 0 ? errno : ENOSPC,
				"Failed to write to file"), -1, true);
			return;
		}
		file->written += written;
	}

	if (end) {
		close(file->fd);
		file->fd = -1;
		file->done = true;
		_Status(session, id, "OK", file->written);
	} else
		_Status(session, id, "PROGRESS", file->written);
}


//!	finish: the files take their places and the links are made, then
//!	directories get their times and permissions.
void
TermFileTransfer::_Commit(Session& session)
{
	std::map<std::string, std::string> paths;
	for (size_t i = 0; i < session.files.size(); i++)
		paths[session.files[i].id] = session.files[i].path;

	std::string error;
	for (size_t i = 0; i < session.files.size(); i++) {
		File& file = session.files[i];
		if (file.type != "regular" || !file.done || file.failed)
			continue;
		if (rename(file.temporaryPath.c_str(), file.path.c_str()) != 0) {
			error = error_text(errno, file.path.c_str());
			continue;
		}
		file.temporaryPath.clear();
		if (file.permissions >= 0)
			chmod(file.path.c_str(), file.permissions & 07777);
		set_times(file.path, file.modified);
	}

	for (size_t i = 0; i < session.files.size(); i++) {
		File& file = session.files[i];
		if (!file.done || file.failed)
			continue;
		if (file.type == "symlink") {
			// fid: a relative link to a file sent, fid_abs: an absolute
			// one, path: what to store
			std::string target;
			const std::string& data = file.linkData;
			if (data.compare(0, 5, "path:") == 0)
				target = data.substr(5);
			else if (data.compare(0, 8, "fid_abs:") == 0)
				target = paths[data.substr(8)];
			else if (data.compare(0, 4, "fid:") == 0) {
				std::string path = paths[data.substr(4)];
				if (!path.empty())
					target = relative_path(directory_of(file.path), path);
			}
			unlink(file.path.c_str());
			if (target.empty()
				|| symlink(target.c_str(), file.path.c_str()) != 0) {
				error = error_text(target.empty() ? ENOENT : errno,
					file.path.c_str());
				continue;
			}
			set_times(file.path, file.modified, true);
		} else if (file.type == "link") {
			// the id of the file linked to; kitty's programs put "fid:"
			// before it
			std::string target;
			const std::string& data = file.linkData;
			if (data.compare(0, 5, "path:") == 0)
				target = data.substr(5);
			else if (data.compare(0, 8, "fid_abs:") == 0)
				target = paths[data.substr(8)];
			else if (data.compare(0, 4, "fid:") == 0)
				target = paths[data.substr(4)];
			else
				target = paths[data];
			unlink(file.path.c_str());
			// BFS has no hard links: a copy then
			int result = target.empty() ? ENOENT : 0;
			if (result == 0 && link(target.c_str(), file.path.c_str()) != 0)
				result = copy_file(target, file.path);
			if (result != 0)
				error = error_text(result, file.path.c_str());
		}
	}

	// the deepest first, as setting a time changes no parent's
	for (size_t i = session.files.size(); i-- > 0;) {
		File& file = session.files[i];
		if (file.type != "directory")
			continue;
		if (file.permissions >= 0)
			chmod(file.path.c_str(), file.permissions & 07777);
		set_times(file.path, file.modified);
	}

	if (!error.empty())
		_Status(session, "", error, -1, true);
}


// #pragma mark - receiving from here


/*!	After the user allowed it: every path asked for, directories with all
	in them, each with an id of its own (and its parent's), then OK with
	the home directory.
*/
void
TermFileTransfer::_List(Session& session)
{
	int32 next = 1;
	std::map<std::pair<dev_t, ino_t>, std::string> seen;
	std::map<std::string, std::string> ids;
	for (size_t i = 0; i < session.requests.size(); i++) {
		const std::string& requestID = session.requests[i].first;
		std::string path;
		if (!local_path(session.requests[i].second, path)) {
			_Status(session, requestID, "EINVAL:Not an absolute path", -1,
				true);
			continue;
		}
		path = normalize_path(path);
		struct stat st;
		if (lstat(path.c_str(), &st) != 0) {
			_Status(session, requestID, error_text(errno, "Does not exist"),
				-1, true);
			continue;
		}
		_ListPath(session, requestID, path, "", next, seen, ids);
	}
	if (session.files.empty()) {
		_Status(session, "", "ENOENT:No files found", -1, true);
		_End(session.id);
		return;
	}
	session.listed = true;
	const char* home = getenv("HOME");
	Fields fields;
	fields["ac"] = "status";
	fields["st"] = encode_base64("OK");
	fields["n"] = encode_base64(home != NULL ? home : "/boot/home");
	_Send(session, fields);
}


void
TermFileTransfer::_ListPath(Session& session, const std::string& requestID,
	const std::string& path, const std::string& parent, int32& next,
	std::map<std::pair<dev_t, ino_t>, std::string>& seen,
	std::map<std::string, std::string>& ids)
{
	struct stat st;
	if (session.files.size() >= kMaxFiles || lstat(path.c_str(), &st) != 0)
		return;
	if (!S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode) && !S_ISREG(st.st_mode))
		return;

	std::string id = std::to_string(next++);
	File file;
	file.id = id;
	file.path = path;
	file.type = S_ISDIR(st.st_mode) ? "directory"
		: S_ISLNK(st.st_mode) ? "symlink" : "regular";

	Fields fields;
	fields["ac"] = "file";
	fields["fid"] = requestID;
	fields["mod"] = std::to_string((int64)st.st_mtim.tv_sec * 1000000000LL
		+ st.st_mtim.tv_nsec);
	fields["prm"] = std::to_string(st.st_mode & 07777);
	fields["n"] = encode_base64(path);
	fields["st"] = encode_base64(id);
	fields["sz"] = std::to_string((int64)st.st_size);
	if (!parent.empty())
		fields["pr"] = parent;

	// a file seen before (by its node) is a hard link to it; a symbolic
	// link names its target if that is listed before it
	std::pair<dev_t, ino_t> node(st.st_dev, st.st_ino);
	if (S_ISREG(st.st_mode) && st.st_nlink > 1 && seen.count(node) != 0) {
		file.type = "link";
		fields["d"] = encode_base64(seen[node]);
	} else if (S_ISREG(st.st_mode))
		seen[node] = id;
	if (S_ISLNK(st.st_mode)) {
		char target[B_PATH_NAME_LENGTH];
		ssize_t length = readlink(path.c_str(), target, sizeof(target));
		if (length > 0) {
			std::string targetPath(target, length);
			if (targetPath[0] != '/')
				targetPath = directory_of(path) + "/" + targetPath;
			targetPath = normalize_path(targetPath);
			if (ids.count(targetPath) != 0)
				fields["d"] = encode_base64(ids[targetPath]);
		}
	}
	if (file.type != "regular")
		fields["ft"] = file.type;
	ids[path] = id;
	session.files.push_back(file);
	_Send(session, fields);

	if (!S_ISDIR(st.st_mode))
		return;
	DIR* dir = opendir(path.c_str());
	if (dir == NULL)
		return;
	std::vector<std::string> names;
	while (struct dirent* entry = readdir(dir)) {
		if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0)
			names.push_back(entry->d_name);
	}
	closedir(dir);
	std::sort(names.begin(), names.end());
	std::string prefix = path == "/" ? path : path + "/";
	for (size_t i = 0; i < names.size(); i++)
		_ListPath(session, requestID, prefix + names[i], id, next, seen, ids);
}


//!	A program asks for a file of the listing: it goes after the ones asked
//!	for before it, one at a time.
void
TermFileTransfer::_ReceiveFile(Session& session, const Fields& fields)
{
	std::string requestID = safe_string(field(fields, "fid"));
	std::string path = decode_base64(field(fields, "n"));
	const File* file = NULL;
	for (size_t i = 0; i < session.files.size(); i++) {
		if (session.files[i].path == path)
			file = &session.files[i];
	}
	if (file == NULL) {
		_Status(session, requestID, "ENOENT:Not in the listing", -1, true);
		return;
	}
	if (file->type == "directory") {
		_Status(session, requestID, "EINVAL:Cannot send a directory", -1,
			true);
		return;
	}
	if (field(fields, "tt") == "rsync") {
		_Status(session, requestID, "EINVAL:rsync is not supported", -1,
			true);
		return;
	}

	Outgoing outgoing;
	outgoing.requestID = requestID;
	outgoing.path = path;
	outgoing.symlink = file->type == "symlink";
	outgoing.compress = field(fields, "zip") == "zlib";
	session.outgoing.push_back(outgoing);
	_SchedulePump(false);
}


//!	Sends what the budget allows of the session's files; returns whether
//!	more is left.
bool
TermFileTransfer::_PumpSession(Session& session, size_t& budget)
{
	while (budget > 0) {
		if (session.outgoing.empty())
			return false;
		Outgoing& current = session.outgoing.front();

		// the file starts
		if (session.readFD < 0 && !session.readDone
			&& session.pending.empty()) {
			if (current.symlink) {
				char target[B_PATH_NAME_LENGTH];
				ssize_t length = readlink(current.path.c_str(), target,
					sizeof(target));
				if (length < 0) {
					_Status(session, current.requestID,
						error_text(errno, "Could not read"), -1, true);
					_EndOutgoing(session);
					continue;
				}
				session.pending.assign(target, length);
				session.readDone = true;
			} else {
				session.readFD = open(current.path.c_str(), O_RDONLY);
				if (session.readFD < 0) {
					_Status(session, current.requestID,
						error_text(errno, "Could not read"), -1, true);
					_EndOutgoing(session);
					continue;
				}
				if (current.compress) {
					session.deflater = new z_stream;
					memset(session.deflater, 0, sizeof(z_stream));
					deflateInit(session.deflater, Z_DEFAULT_COMPRESSION);
				}
			}
		}

		if (!session.readDone && session.pending.size() <= kChunkSize) {
			char buffer[65536];
			ssize_t bytesRead = read(session.readFD, buffer, sizeof(buffer));
			if (bytesRead < 0) {
				_Status(session, current.requestID,
					error_text(errno, "Could not read"), -1, true);
				_EndOutgoing(session);
				continue;
			}
			session.readDone = bytesRead == 0;
			if (session.deflater != NULL) {
				deflate_data(session.deflater, buffer, bytesRead,
					session.readDone, session.pending);
			} else
				session.pending.append(buffer, bytesRead);
		}

		// whole chunks, and the rest at the end
		while (budget > 0
			&& (session.pending.size() > kChunkSize || session.readDone)) {
			size_t length = std::min(kChunkSize, session.pending.size());
			bool last = session.readDone && length == session.pending.size();
			Fields fields;
			fields["ac"] = last ? "end_data" : "data";
			fields["fid"] = current.requestID;
			fields["d"] = encode_base64(session.pending.substr(0, length));
			_Send(session, fields);
			session.pending.erase(0, length);
			budget -= std::min(budget, length * 4 / 3 + 64);
			if (last) {
				_EndOutgoing(session);
				break;
			}
		}
	}
	return !session.outgoing.empty();
}


void
TermFileTransfer::_EndOutgoing(Session& session)
{
	if (session.readFD >= 0)
		close(session.readFD);
	session.readFD = -1;
	if (session.deflater != NULL) {
		deflateEnd(session.deflater);
		delete session.deflater;
		session.deflater = NULL;
	}
	session.pending.clear();
	session.readDone = false;
	if (!session.outgoing.empty())
		session.outgoing.pop_front();
}


void
TermFileTransfer::_SchedulePump(bool later)
{
	if (fPumpScheduled || fTarget->Looper() == NULL)
		return;
	fPumpScheduled = true;
	if (later) {
		BMessage message(kFileTransferPump);
		BMessageRunner::StartSending(BMessenger(fTarget), &message,
			kPumpDelay, 1);
	} else
		fTarget->Looper()->PostMessage(kFileTransferPump, fTarget);
}


// #pragma mark - answers


void
TermFileTransfer::_Send(Session& session, const Fields& fields)
{
	// kitty's order of the keys
	static const char* const kOrder[] = { "ac", "zip", "ft", "tt", "id",
		"fid", "pw", "q", "mod", "prm", "sz", "n", "st", "pr", "d" };
	std::string packet = "\x1b]5113";
	for (size_t i = 0; i < sizeof(kOrder) / sizeof(kOrder[0]); i++) {
		const char* key = kOrder[i];
		std::string value;
		if (strcmp(key, "id") == 0)
			value = session.id;
		else if (fields.count(key) != 0)
			value = field(fields, key);
		else
			continue;
		packet += ";";
		packet += key;
		packet += "=" + value;
	}
	packet += "\x1b\\";
	fWriter(packet);
}


//!	A status: acknowledgements unless quiet is 1 or more, errors unless 2.
void
TermFileTransfer::_Status(Session& session, const std::string& fileID,
	const std::string& status, int64 size, bool error)
{
	if (session.quiet >= 2 || (!error && session.quiet >= 1))
		return;
	Fields fields;
	fields["ac"] = "status";
	if (!fileID.empty())
		fields["fid"] = fileID;
	fields["st"] = encode_base64(status);
	if (size >= 0)
		fields["sz"] = std::to_string(size);
	_Send(session, fields);
}
