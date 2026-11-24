/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "DirectoryWatcher.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <syslog.h>
#include <unistd.h>

#include <algorithm>
#include <set>

#include <ByteOrder.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>


// How many directories are watched at most: the ones read last. A watch is an
// open handle on the server and a request it holds, nothing on this side.
static const size_t kMaxWatches = 32;

// What the server may answer with at once; more changes than fit make it say
// that the directory has to be read again. Up to 64 kB cost one credit.
static const uint32 kNotifyBufferSize = 16 * 1024;

// How long changes are collected before they are told, so that a file being
// written is one change and not one for each piece of it.
static const bigtime_t kEventDelay = 200000;

// How long to wait before trying again when the server cannot be reached;
// doubled each time up to the maximum.
static const bigtime_t kMinConnectDelay = 10 * 1000000LL;
static const bigtime_t kMaxConnectDelay = 5 * 60 * 1000000LL;

// Samba tells of a file written to on its own machine (inotify IN_MODIFY) as
// a change of attributes only, Windows as one of size and time.
static const uint32 kNotifyFilter
	= SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_FILE_NAME
	| SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_DIR_NAME
	| SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_ATTRIBUTES
	| SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_SIZE
	| SMB2_CHANGE_NOTIIFY_FILE_NOTIFY_CHANGE_LAST_WRITE;

// not in libsmb2's list
static const uint32 kStatusNotifyCleanup = 0x0000010b;
static const uint32 kStatusNotifyEnumDir = 0x0000010c;
static const uint32 kStatusNetworkSessionExpired = 0xc000035c;

// FILE_NOTIFY_INFORMATION actions
enum {
	kActionAdded = 1,
	kActionRemoved,
	kActionModified,
	kActionRenamedOldName,
	kActionRenamedNewName
};


struct DirectoryWatcher::Watch {
	enum State {
		kClosed,
		kOpening,
		kOpen,
		kClosing
	};

	DirectoryWatcher*	watcher;
	std::string			directory;
	std::string			serverDirectory;
	smb2_file_id		fileID;
	State				state;
	bool				wanted;
		// one of the directories read last
	bool				failed;
		// the server would not watch it; not tried again until it is read
	bool				wasOpen;
		// was watched in a session that is gone: there may have been
		// changes nobody heard of
	int					pending;
		// requests libsmb2 has that point here

	Watch(DirectoryWatcher* watcher, const std::string& directory,
			const std::string& serverDirectory)
		:
		watcher(watcher),
		directory(directory),
		serverDirectory(serverDirectory),
		state(kClosed),
		wanted(true),
		failed(false),
		wasOpen(false),
		pending(0)
	{
		memset(fileID, 0, sizeof(fileID));
	}
};


DirectoryWatcher::DirectoryWatcher()
	:
	fOptions(NULL),
	fHook(NULL),
	fThread(-1),
	fQuitting(false),
	fFirstEventTime(0),
	fLost(false),
	fNextConnect(0),
	fConnectDelay(kMinConnectDelay),
	fBusy(0)
{
	pthread_mutex_init(&fLock, NULL);
	fWakePipe[0] = fWakePipe[1] = -1;
	fQuitPipe[0] = fQuitPipe[1] = -1;
}


DirectoryWatcher::~DirectoryWatcher()
{
	Shutdown();
	pthread_mutex_destroy(&fLock);
}


/*!	Starts the thread. As the file system may be mounted again in the same
	process, this may come again after Shutdown().
*/
void
DirectoryWatcher::Init(const Options* options, ChangeHook hook)
{
	Shutdown();

	fOptions = options;
	fHook = hook;
	fConnection.Init(options, options->connections);
	fQuitting = false;
	fCommands.clear();
	fEvents.clear();
	fLost = false;
	fNextConnect = 0;
	fConnectDelay = kMinConnectDelay;
	fBusy = 0;

	if (pipe(fWakePipe) != 0 || pipe(fQuitPipe) != 0) {
		syslog(LOG_ERR, "smbfs: no pipes for the directory watcher: %s\n",
			strerror(errno));
		return;
	}
	fcntl(fWakePipe[0], F_SETFL, O_NONBLOCK);
	fcntl(fWakePipe[1], F_SETFL, O_NONBLOCK);
	fConnection.SetCancelFD(fQuitPipe[0]);

	fThread = spawn_thread(&_ThreadEntry, "smbfs directory watcher",
		B_LOW_PRIORITY, this);
	if (fThread < 0) {
		syslog(LOG_ERR, "smbfs: no directory watcher thread: %s\n",
			strerror(fThread));
		return;
	}
	resume_thread(fThread);
}


/*!	Stops watching and waits for the thread to be gone, which it is before
	this returns: the volume it reports to goes after this.
*/
void
DirectoryWatcher::Shutdown()
{
	if (fThread >= 0) {
		pthread_mutex_lock(&fLock);
		fQuitting = true;
		pthread_mutex_unlock(&fLock);

		char quit = 1;
		write(fQuitPipe[1], &quit, 1);

		status_t result;
		wait_for_thread(fThread, &result);
		fThread = -1;
	}

	for (int i = 0; i < 2; i++) {
		if (fWakePipe[i] >= 0)
			close(fWakePipe[i]);
		if (fQuitPipe[i] >= 0)
			close(fQuitPipe[i]);
		fWakePipe[i] = fQuitPipe[i] = -1;
	}
	fConnection.SetCancelFD(-1);
}


/*!	Somebody read \a directory: they will want to know when it changes. */
void
DirectoryWatcher::WatchDirectory(const std::string& directory,
	const std::string& serverDirectory)
{
	pthread_mutex_lock(&fLock);
	if (fThread < 0 || fQuitting) {
		pthread_mutex_unlock(&fLock);
		return;
	}

	// a folder is read more than once in a row when it is opened
	bool known = false;
	for (size_t i = 0; i < fCommands.size(); i++) {
		if (fCommands[i].directory == directory) {
			known = true;
			break;
		}
	}
	if (!known) {
		Command command;
		command.directory = directory;
		command.serverDirectory = serverDirectory;
		fCommands.push_back(command);
	}
	pthread_mutex_unlock(&fLock);

	if (!known) {
		char wake = 1;
		write(fWakePipe[1], &wake, 1);
	}
}


/*static*/ status_t
DirectoryWatcher::_ThreadEntry(void* data)
{
	((DirectoryWatcher*)data)->_Thread();
	return B_OK;
}


void
DirectoryWatcher::_Thread()
{
	while (true) {
		std::deque<Command> commands;
		pthread_mutex_lock(&fLock);
		bool quitting = fQuitting;
		commands.swap(fCommands);
		pthread_mutex_unlock(&fLock);
		if (quitting)
			break;

		_TakeCommands(commands);

		// what has to be opened (again)
		bool toOpen = false;
		for (std::list<Watch*>::iterator it = fWatches.begin();
				it != fWatches.end(); it++) {
			Watch* watch = *it;
			if (watch->wanted && !watch->failed
				&& watch->state == Watch::kClosed) {
				toOpen = true;
				break;
			}
		}

		if (toOpen && !fConnection.IsConnected()
			&& system_time() >= fNextConnect) {
			_Connect();
		}

		if (toOpen && fConnection.IsConnected()) {
			for (std::list<Watch*>::iterator it = fWatches.begin();
					it != fWatches.end(); it++) {
				Watch* watch = *it;
				if (watch->wanted && !watch->failed
					&& watch->state == Watch::kClosed) {
					_Open(watch);
				}
			}
		}

		if (!fEvents.empty()
			&& system_time() >= fFirstEventTime + kEventDelay) {
			_DeliverEvents();
		}

		// Wait for the server, for news from the volume, or for the time to
		// tell what changed. Nothing else wakes this up: when nothing
		// changes, nothing happens.
		bigtime_t timeout = -1;
		const bigtime_t now = system_time();
		if (!fEvents.empty())
			timeout = std::max((bigtime_t)0, fFirstEventTime + kEventDelay - now);
		if (fConnection.IsConnected() && fBusy > 0) {
			// libsmb2 finds the requests that took too long when called
			timeout = timeout < 0 ? 1000000 : std::min(timeout, (bigtime_t)1000000);
		}
		if (toOpen && !fConnection.IsConnected()) {
			bigtime_t untilConnect = std::max((bigtime_t)0, fNextConnect - now);
			timeout = timeout < 0 ? untilConnect : std::min(timeout, untilConnect);
		}

		struct pollfd pollFDs[3];
		pollFDs[0].fd = fQuitPipe[0];
		pollFDs[0].events = POLLIN;
		pollFDs[1].fd = fWakePipe[0];
		pollFDs[1].events = POLLIN;
		int count = 2;
		if (fConnection.IsConnected()) {
			pollFDs[2].fd = smb2_get_fd(fConnection.Context());
			pollFDs[2].events = smb2_which_events(fConnection.Context());
			count = 3;
		}
		for (int i = 0; i < count; i++)
			pollFDs[i].revents = 0;

		int result = poll(pollFDs, count,
			timeout < 0 ? -1 : (int)((timeout + 999) / 1000));
		if (result < 0 && errno != EINTR) {
			syslog(LOG_ERR, "smbfs: directory watcher: poll failed: %s\n",
				strerror(errno));
			snooze(1000000);
			continue;
		}

		if ((pollFDs[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
			break;

		if ((pollFDs[1].revents & POLLIN) != 0) {
			char buffer[64];
			while (read(fWakePipe[0], buffer, sizeof(buffer)) > 0)
				;
		}

		if (count == 3 && fConnection.IsConnected()
			&& smb2_service(fConnection.Context(),
				result > 0 ? pollFDs[2].revents : 0) < 0) {
			fLost = true;
		}

		if (fLost) {
			syslog(LOG_INFO, "smbfs: //%s/%s: directory watcher lost the "
				"server\n", fOptions->server.c_str(),
				fOptions->share.c_str());
			_Disconnect();
			fNextConnect = 0;
		}
	}

	// The session ends with the handles, the server closes them.
	_Disconnect();

	for (std::list<Watch*>::iterator it = fWatches.begin();
			it != fWatches.end(); it++) {
		delete *it;
	}
	fWatches.clear();
	fEvents.clear();
}


void
DirectoryWatcher::_TakeCommands(std::deque<Command>& commands)
{
	for (size_t i = 0; i < commands.size(); i++) {
		const Command& command = commands[i];
		Watch* watch = _FindWatch(command.directory);
		if (watch != NULL) {
			fWatches.remove(watch);
			watch->wanted = true;
			watch->failed = false;
			watch->serverDirectory = command.serverDirectory;
		} else {
			watch = new(std::nothrow) Watch(this, command.directory,
				command.serverDirectory);
			if (watch == NULL)
				continue;
		}
		fWatches.push_front(watch);
	}

	_Prune();
}


DirectoryWatcher::Watch*
DirectoryWatcher::_FindWatch(const std::string& directory)
{
	for (std::list<Watch*>::iterator it = fWatches.begin();
			it != fWatches.end(); it++) {
		if ((*it)->directory == directory)
			return *it;
	}
	return NULL;
}


/*!	Stops watching what was read longest ago when there are too many, and
	forgets watches that are over.
*/
void
DirectoryWatcher::_Prune()
{
	size_t wanted = 0;
	std::list<Watch*>::iterator it = fWatches.begin();
	while (it != fWatches.end()) {
		Watch* watch = *it;
		if (watch->wanted && ++wanted > kMaxWatches)
			watch->wanted = false;

		if (!watch->wanted && watch->state == Watch::kOpen)
			_Close(watch);

		if (!watch->wanted && watch->pending == 0
			&& watch->state == Watch::kClosed) {
			it = fWatches.erase(it);
			delete watch;
			continue;
		}
		it++;
	}
}


bool
DirectoryWatcher::_Connect()
{
	int status = fConnection.EnsureConnected();
	if (status != 0) {
		fNextConnect = system_time() + fConnectDelay;
		fConnectDelay = std::min(fConnectDelay * 2, kMaxConnectDelay);
		return false;
	}

	fConnectDelay = kMinConnectDelay;

	// The session is quiet for as long as nothing changes. Have TCP tell
	// when the server is gone without saying so.
	int fd = smb2_get_fd(fConnection.Context());
	int value = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &value, sizeof(value));
	value = 60;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &value, sizeof(value));
	value = 20;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &value, sizeof(value));
	value = 3;
	setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &value, sizeof(value));

	return true;
}


/*!	Ends the session. libsmb2 calls back for every request it still has
	before this returns.
*/
void
DirectoryWatcher::_Disconnect()
{
	fConnection.Disconnect();
	fLost = false;
	fBusy = 0;

	std::list<Watch*>::iterator it = fWatches.begin();
	while (it != fWatches.end()) {
		Watch* watch = *it;
		watch->state = Watch::kClosed;
		watch->pending = 0;
		if (!watch->wanted) {
			it = fWatches.erase(it);
			delete watch;
			continue;
		}
		it++;
	}
}


void
DirectoryWatcher::_Open(Watch* watch)
{
	struct smb2_create_request request;
	memset(&request, 0, sizeof(request));
	request.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
	request.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
	request.desired_access = SMB2_FILE_LIST_DIRECTORY
		| SMB2_FILE_READ_ATTRIBUTES;
	request.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE
		| SMB2_FILE_SHARE_DELETE;
		// nobody is kept from renaming or deleting it
	request.create_disposition = SMB2_FILE_OPEN;
	request.create_options = SMB2_FILE_DIRECTORY_FILE;
	request.name = watch->serverDirectory.c_str();

	smb2_context* context = fConnection.Context();
	struct smb2_pdu* pdu = smb2_cmd_create_async(context, &request, &_Opened,
		watch);
	if (pdu == NULL) {
		watch->failed = true;
		return;
	}
	smb2_queue_pdu(context, pdu);

	watch->state = Watch::kOpening;
	watch->pending++;
	fBusy++;
}


void
DirectoryWatcher::_Arm(Watch* watch)
{
	struct smb2_change_notify_request request;
	memset(&request, 0, sizeof(request));
	request.flags = 0;
		// this directory, not what is below it
	request.output_buffer_length = kNotifyBufferSize;
	memcpy(request.file_id, watch->fileID, SMB2_FD_SIZE);
	request.completion_filter = kNotifyFilter;

	// The answer comes when something changes, which can be never: libsmb2
	// must not give up on it after the time any other request may take.
	smb2_context* context = fConnection.Context();
	smb2_set_timeout(context, 0);
	struct smb2_pdu* pdu = smb2_cmd_change_notify_async(context, &request,
		&_Notified, watch);
	smb2_set_timeout(context, fOptions->timeout);
	if (pdu == NULL) {
		watch->failed = true;
		_Close(watch);
		return;
	}
	smb2_queue_pdu(context, pdu);

	watch->pending++;
}


void
DirectoryWatcher::_Close(Watch* watch)
{
	struct smb2_close_request request;
	memset(&request, 0, sizeof(request));
	memcpy(request.file_id, watch->fileID, SMB2_FD_SIZE);

	smb2_context* context = fConnection.Context();
	struct smb2_pdu* pdu = smb2_cmd_close_async(context, &request, &_Closed,
		watch);
	if (pdu == NULL) {
		// the handle is closed with the session
		watch->state = Watch::kClosed;
		return;
	}
	smb2_queue_pdu(context, pdu);

	watch->state = Watch::kClosing;
	watch->pending++;
	fBusy++;
}


/*static*/ void
DirectoryWatcher::_Opened(struct smb2_context* context, int status,
	void* data, void* cookie)
{
	Watch* watch = (Watch*)cookie;
	DirectoryWatcher* self = watch->watcher;
	watch->pending--;
	self->fBusy--;

	if (status == SMB2_STATUS_SUCCESS && data != NULL) {
		struct smb2_create_reply* reply = (struct smb2_create_reply*)data;
		memcpy(watch->fileID, reply->file_id, SMB2_FD_SIZE);
		watch->state = Watch::kOpen;

		if (!watch->wanted) {
			self->_Close(watch);
			return;
		}

		if (watch->wasOpen) {
			// what happened while there was no session is unknown
			self->_AddEvent(kDirectoryChanged, watch, std::string());
		}
		watch->wasOpen = true;

		self->_Arm(watch);
		return;
	}

	watch->state = Watch::kClosed;
	if (self->_IsLost(status))
		self->fLost = true;
	else
		watch->failed = true;
}


/*static*/ void
DirectoryWatcher::_Notified(struct smb2_context* context, int status,
	void* data, void* cookie)
{
	Watch* watch = (Watch*)cookie;
	DirectoryWatcher* self = watch->watcher;
	watch->pending--;

	if ((uint32)status == SMB2_STATUS_SUCCESS
		|| (uint32)status == kStatusNotifyEnumDir) {
		struct smb2_change_notify_reply* reply
			= (struct smb2_change_notify_reply*)data;
		if ((uint32)status == SMB2_STATUS_SUCCESS && reply != NULL
			&& reply->output != NULL && reply->output_buffer_length > 0) {
			self->_AddEvents(watch, reply->output,
				reply->output_buffer_length);
		} else {
			// more changed than could be told
			self->_AddEvent(kDirectoryChanged, watch, std::string());
		}

		if (watch->wanted && watch->state == Watch::kOpen)
			self->_Arm(watch);
		else if (watch->state == Watch::kOpen)
			self->_Close(watch);
		return;
	}

	if ((uint32)status == kStatusNotifyCleanup)
		return;
			// the handle was closed

	if (self->_IsLost(status)) {
		self->fLost = true;
		return;
	}

	// The directory is gone, or the server does not do this.
	if (watch->state == Watch::kOpen) {
		watch->failed = true;
		self->_Close(watch);
	}
}


/*static*/ void
DirectoryWatcher::_Closed(struct smb2_context* context, int status,
	void* data, void* cookie)
{
	Watch* watch = (Watch*)cookie;
	watch->pending--;
	watch->watcher->fBusy--;
	watch->state = Watch::kClosed;
}


/*!	Takes what the server sent: FILE_NOTIFY_INFORMATION records. */
void
DirectoryWatcher::_AddEvents(Watch* watch, const uint8* buffer,
	uint32 length)
{
	uint32 offset = 0;
	while (offset + 12 <= length) {
		uint32 next;
		uint32 action;
		uint32 nameLength;
		memcpy(&next, buffer + offset, 4);
		memcpy(&action, buffer + offset + 4, 4);
		memcpy(&nameLength, buffer + offset + 8, 4);
		next = B_LENDIAN_TO_HOST_INT32(next);
		action = B_LENDIAN_TO_HOST_INT32(action);
		nameLength = B_LENDIAN_TO_HOST_INT32(nameLength);

		if (nameLength > length - offset - 12) {
			_AddEvent(kDirectoryChanged, watch, std::string());
			return;
		}

		// the name is UTF-16 and need not be aligned for it
		std::vector<uint16_t> utf16(nameLength / 2);
		if (!utf16.empty())
			memcpy(&utf16[0], buffer + offset + 12, utf16.size() * 2);
		const char* utf8 = utf16.empty() ? NULL
			: smb2_utf16_to_utf8(&utf16[0], utf16.size());
		std::string name = utf8 != NULL ? utf8 : "";
		free((void*)utf8);

		if (name.empty() || name.find('\\') != std::string::npos
			|| name.find('/') != std::string::npos) {
			_AddEvent(kDirectoryChanged, watch, std::string());
		} else {
			switch (action) {
				case kActionAdded:
				case kActionRenamedNewName:
					_AddEvent(kEntryCreated, watch, name);
					break;
				case kActionRemoved:
				case kActionRenamedOldName:
					_AddEvent(kEntryRemoved, watch, name);
					break;
				default:
					_AddEvent(kEntryChanged, watch, name);
					break;
			}
		}

		if (next == 0)
			break;

		// Where the next record starts is the server's word, too. One that
		// does not lie further on in the reply would have us read outside of
		// it, or the same records over and over.
		if (next < 12 || next > length - offset) {
			_AddEvent(kDirectoryChanged, watch, std::string());
			return;
		}
		offset += next;
	}
}


void
DirectoryWatcher::_AddEvent(int what, Watch* watch, const std::string& name)
{
	if (fEvents.empty())
		fFirstEventTime = system_time();

	// by name: the watch may be gone when this is told
	Event event;
	event.what = what;
	event.directory = watch->directory;
	event.serverDirectory = watch->serverDirectory;
	event.name = name;
	fEvents.push_back(event);
}


/*!	Tells the volume what changed, a little of it at least: a change after a
	creation or a change of the same entry has nothing to add, and when a
	directory is read again anyway, the rest of what is known of it is not
	needed.
*/
void
DirectoryWatcher::_DeliverEvents()
{
	std::vector<Event> events;
	events.swap(fEvents);

	std::set<std::string> rescanned;
	for (size_t i = 0; i < events.size(); i++) {
		if (events[i].what == kDirectoryChanged)
			rescanned.insert(events[i].directory);
	}

	std::vector<const Event*> changes;
	std::set<std::string> rescanAdded;
	for (size_t i = 0; i < events.size(); i++) {
		const Event& event = events[i];

		if (rescanned.find(event.directory) != rescanned.end()) {
			if (event.what == kDirectoryChanged
				&& rescanAdded.insert(event.directory).second) {
				changes.push_back(&event);
			}
			continue;
		}

		if (event.what == kEntryChanged) {
			bool redundant = false;
			for (size_t j = changes.size(); j-- > 0;) {
				if (changes[j]->directory == event.directory
					&& changes[j]->name == event.name) {
					redundant = changes[j]->what != kEntryRemoved;
					break;
				}
			}
			if (redundant)
				continue;
		}

		changes.push_back(&event);
	}

	// The hook asks the server, and the volume: neither waits for this
	// thread.
	for (size_t i = 0; i < changes.size(); i++) {
		fHook(changes[i]->what, changes[i]->directory,
			changes[i]->serverDirectory, changes[i]->name);
	}
}


bool
DirectoryWatcher::_IsLost(int status) const
{
	return Connection::IsLost(status)
		|| (uint32)status == SMB2_STATUS_IO_TIMEOUT
		|| (uint32)status == SMB2_STATUS_USER_SESSION_DELETED
		|| (uint32)status == kStatusNetworkSessionExpired;
}
