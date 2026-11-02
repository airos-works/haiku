/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Mounts a share of an SMB2 or SMB3 server (Windows, Samba, most NAS).

	This is a FUSE file system for userlandfs, libsmb2 does the talking:

		mount -t userlandfs -p "smbfs server=<host> share=<name> ..." <where>

	The parameters are words of the form key=value, see parse_options(). A
	value has its special characters written %XX as in a URL, which is how a
	password with a blank in it gets here in one piece.

	userlandfs has one volume per FUSE file system. To mount several shares,
	give each an instance name: "smbfs:music server=...".
*/


#include <errno.h>
#include <fcntl.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <syslog.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>

#include <fuse.h>

#include <ctype.h>
#include <fs_info.h>
#include <MimeType.h>
#include <OS.h>

#include <smb2/smb2.h>
#include <smb2/libsmb2.h>
#include <smb2/libsmb2-raw.h>

#include "Connection.h"
#include "StatCache.h"


// What is asked of the server at once at most. It would take more, but a
// request this size is over soon enough for others to get their turn.
static const size_t kMaxTransferSize = 1024 * 1024;

// What is read of a file that is read from start to end, whatever was asked
// for. Players read a few kB at a time.
static const size_t kReadAheadSize = 512 * 1024;

// How long the server is not asked again how much room it has.
static const bigtime_t kVolumeInfoLifetime = 5 * 1000000LL;


struct FileHandle {
	Connection*	connection;
	smb2fh*		handle;
	uint32		generation;
	std::string	path;
	int			openMode;

	uint8*		buffer;
	off_t		bufferOffset;
	size_t		bufferSize;
	off_t		nextOffset;
		// where reading goes on if it is sequential

	FileHandle()
		:
		connection(NULL),
		handle(NULL),
		generation(0),
		openMode(O_RDONLY),
		buffer(NULL),
		bufferOffset(0),
		bufferSize(0),
		nextOffset(-1)
	{
	}

	~FileHandle()
	{
		free(buffer);
	}
};


static Options sOptions;
static ConnectionPool sPool;
static StatCache sStatCache;

static pthread_mutex_t sHandlesLock = PTHREAD_MUTEX_INITIALIZER;
static std::set<FileHandle*> sHandles;


//	#pragma mark - helpers


/*!	The path the server knows \a path by: relative to the share, with no
	slash in front.
*/
static std::string
server_path(const char* path)
{
	while (*path == '/')
		path++;

	std::string result = sOptions.path;
	if (*path != '\0') {
		if (!result.empty())
			result += '/';
		result += path;
	}

	while (!result.empty() && result[result.length() - 1] == '/')
		result.erase(result.length() - 1);

	return result;
}


static std::string
parent_path(const std::string& path)
{
	size_t slash = path.rfind('/');
	if (slash == std::string::npos)
		return "";
	return path.substr(0, slash);
}


static void
to_stat(const smb2_stat_64& info, struct stat& st)
{
	memset(&st, 0, sizeof(st));

	// The server decides who may do what and does not say in advance, so
	// allow what the mount allows and let it refuse.
	mode_t permissions;
	switch (info.smb2_type) {
		case SMB2_TYPE_DIRECTORY:
			st.st_mode = S_IFDIR;
			permissions = 0755;
			break;
		case SMB2_TYPE_LINK:
			st.st_mode = S_IFLNK;
			permissions = 0777;
			break;
		default:
			st.st_mode = S_IFREG;
			permissions = 0644;
			break;
	}
	if (sOptions.readOnly)
		permissions &= ~0222;
	st.st_mode |= permissions;

	st.st_ino = info.smb2_ino;
	st.st_nlink = info.smb2_nlink > 0 ? info.smb2_nlink : 1;
	st.st_uid = geteuid();
	st.st_gid = getegid();
	st.st_size = info.smb2_size;
	st.st_blksize = 64 * 1024;
	st.st_blocks = (info.smb2_size + 511) / 512;

	st.st_atim.tv_sec = info.smb2_atime;
	st.st_atim.tv_nsec = info.smb2_atime_nsec;
	st.st_mtim.tv_sec = info.smb2_mtime;
	st.st_mtim.tv_nsec = info.smb2_mtime_nsec;
	st.st_ctim.tv_sec = info.smb2_ctime;
	st.st_ctim.tv_nsec = info.smb2_ctime_nsec;
	st.st_crtim.tv_sec = info.smb2_btime;
	st.st_crtim.tv_nsec = info.smb2_btime_nsec;
}


/*!	Waits for the answer to a request that was handed to libsmb2 with the
	result \a queued, and returns what came of it: 0 or more, or an error.

	When there is no answer, the session ends here and now. libsmb2 keeps a
	pointer to \a request until it has answered or the session is over, and
	the request is on the caller's stack.
*/
static int
complete(Connection* connection, int queued, Request& request)
{
	if (queued != 0)
		return queued > 0 ? -queued : queued;

	if (!connection->Wait(request)) {
		connection->Disconnect();
		return -ETIMEDOUT;
	}

	if (Connection::IsLost(request.status)) {
		connection->Disconnect();
		return -ENOTCONN;
	}

	return request.status;
}


/*!	Like complete(), for requests made with the functions of libsmb2-raw.h,
	which get the server's status as it is.
*/
static int
complete_raw(Connection* connection, Request& request)
{
	if (!connection->Wait(request)) {
		connection->Disconnect();
		return -ETIMEDOUT;
	}

	if ((uint32)request.status == SMB2_STATUS_SHUTDOWN
		|| (uint32)request.status == SMB2_STATUS_CANCELLED) {
		connection->Disconnect();
		return -ENOTCONN;
	}

	return -nterror_to_errno(request.status);
}


/*!	Does \a operation with a session to the server, and once more with a new
	session if the one it got turned out to be gone. That is what happens
	after the server was restarted, or this machine has slept.
*/
template<typename Operation>
static int
with_connection(Operation operation)
{
	Connection* connection = sPool.Acquire();

	int status = 0;
	for (int attempt = 0; attempt < 2; attempt++) {
		status = connection->EnsureConnected();
		if (status != 0)
			break;

		status = operation(connection);
		if (!Connection::IsLost(status))
			break;

		connection->Disconnect();
	}

	connection->Unlock();
	return status;
}


/*!	Like with_connection(), with the session \a handle was opened in. If that
	has been replaced since, the file is opened again first.
*/
template<typename Operation>
static int
with_handle(FileHandle* handle, Operation operation)
{
	Connection* connection = handle->connection;
	connection->Lock();

	int status = 0;
	for (int attempt = 0; attempt < 2; attempt++) {
		status = connection->EnsureConnected();
		if (status != 0)
			break;

		if (handle->generation != connection->Generation()) {
			pthread_mutex_lock(&sHandlesLock);
			std::string path = handle->path;
			pthread_mutex_unlock(&sHandlesLock);

			Request request;
			status = complete(connection,
				smb2_open_async(connection->Context(), path.c_str(),
					handle->openMode, &Request::Callback, &request),
				request);
			if (status == 0) {
				handle->handle = (smb2fh*)request.data;
				handle->generation = connection->Generation();
			}
		}

		if (status == 0)
			status = operation(connection, handle->handle);
		if (!Connection::IsLost(status))
			break;

		connection->Disconnect();
	}

	connection->Unlock();
	return status;
}


static int
stat_path(const std::string& path, struct stat* st)
{
	if (sStatCache.Lookup(path, *st))
		return 0;

	smb2_stat_64 info;
	int status = with_connection([&](Connection* connection) -> int {
		Request request;
		return complete(connection,
			smb2_stat_async(connection->Context(), path.c_str(), &info,
				&Request::Callback, &request),
			request);
	});
	if (status != 0)
		return status;

	to_stat(info, *st);
	sStatCache.Insert(path, *st);
	return 0;
}


static void
forget(const std::string& path)
{
	sStatCache.Remove(path);
	sStatCache.Remove(parent_path(path));
}


static int
open_file(const std::string& path, int openMode, int createFlags,
	struct fuse_file_info* info)
{
	FileHandle* handle = new(std::nothrow) FileHandle;
	if (handle == NULL)
		return B_NO_MEMORY;

	handle->path = path;
	handle->openMode = openMode;

	int status = with_connection([&](Connection* connection) -> int {
		Request request;
		int status = complete(connection,
			smb2_open_async(connection->Context(), path.c_str(),
				openMode | createFlags, &Request::Callback, &request),
			request);
		if (status != 0)
			return status;

		handle->connection = connection;
		handle->handle = (smb2fh*)request.data;
		handle->generation = connection->Generation();
		return 0;
	});

	if (status != 0) {
		delete handle;
		return status;
	}

	pthread_mutex_lock(&sHandlesLock);
	sHandles.insert(handle);
	pthread_mutex_unlock(&sHandlesLock);

	info->fh = (uint64_t)(addr_t)handle;
	return 0;
}


static int
read_from_server(Connection* connection, smb2fh* file, uint8* buffer,
	size_t size, off_t offset)
{
	const size_t maxSize = std::min((size_t)smb2_get_max_read_size(
		connection->Context()), kMaxTransferSize);

	size_t total = 0;
	while (total < size) {
		const size_t count = std::min(size - total, maxSize);

		Request request;
		int status = complete(connection,
			smb2_pread_async(connection->Context(), file, buffer + total,
				count, offset + total, &Request::Callback, &request),
			request);
		if (status < 0)
			return status;

		total += status;
		if ((size_t)status < count)
			break;
	}

	return total;
}


/*!	Opens \a path, sets \a info of the kind \a infoClass and closes it
	again, all in one request. There are libsmb2 functions for some of what
	can be set, and none for the rest.
*/
static int
set_info(Connection* connection, const std::string& path, uint32 access,
	uint32 createOptions, uint8 infoClass, void* info)
{
	smb2_context* context = connection->Context();

	// stands for the file the first of the requests opens
	smb2_file_id compoundID;
	memset(compoundID, 0xff, sizeof(compoundID));

	Request opened;
	Request set;
	Request closed;

	struct smb2_create_request createRequest;
	memset(&createRequest, 0, sizeof(createRequest));
	createRequest.requested_oplock_level = SMB2_OPLOCK_LEVEL_NONE;
	createRequest.impersonation_level = SMB2_IMPERSONATION_IMPERSONATION;
	createRequest.desired_access = access;
	createRequest.share_access = SMB2_FILE_SHARE_READ | SMB2_FILE_SHARE_WRITE
		| SMB2_FILE_SHARE_DELETE;
	createRequest.create_disposition = SMB2_FILE_OPEN;
	createRequest.create_options = createOptions;
	createRequest.name = path.c_str();

	struct smb2_pdu* pdu = smb2_cmd_create_async(context, &createRequest,
		&Request::Callback, &opened);
	if (pdu == NULL)
		return B_NO_MEMORY;

	struct smb2_set_info_request setRequest;
	memset(&setRequest, 0, sizeof(setRequest));
	setRequest.info_type = SMB2_0_INFO_FILE;
	setRequest.file_info_class = infoClass;
	memcpy(setRequest.file_id, compoundID, SMB2_FD_SIZE);
	setRequest.input_data = info;

	struct smb2_pdu* next = smb2_cmd_set_info_async(context, &setRequest,
		&Request::Callback, &set);
	if (next == NULL) {
		smb2_free_pdu(context, pdu);
		return B_NO_MEMORY;
	}
	smb2_add_compound_pdu(context, pdu, next);

	struct smb2_close_request closeRequest;
	memset(&closeRequest, 0, sizeof(closeRequest));
	memcpy(closeRequest.file_id, compoundID, SMB2_FD_SIZE);

	next = smb2_cmd_close_async(context, &closeRequest, &Request::Callback,
		&closed);
	if (next == NULL) {
		smb2_free_pdu(context, pdu);
		return B_NO_MEMORY;
	}
	smb2_add_compound_pdu(context, pdu, next);

	smb2_queue_pdu(context, pdu);

	int status = complete_raw(connection, closed);
	if (Connection::IsLost(status))
		return status;

	if (opened.status != 0)
		return -nterror_to_errno(opened.status);
	if (set.status != 0)
		return -nterror_to_errno(set.status);
	return 0;
}


/*!	Removes the file or the directory \a path.

	libsmb2 does that by opening what is to go with the wish that it be
	deleted when closed, which the server grants without looking and then
	does not do if it cannot: a folder that is not empty was reported gone
	and stayed. Asked to mark it for deletion, the server says no.
*/
static int
remove_entry(const std::string& path, bool directory)
{
	if (sOptions.readOnly)
		return B_READ_ONLY_DEVICE;

	struct smb2_file_disposition_info info;
	info.delete_pending = 1;

	// a link is what is removed, and not what it points to
	const uint32 options = directory ? SMB2_FILE_DIRECTORY_FILE
		: SMB2_FILE_NON_DIRECTORY_FILE | SMB2_FILE_OPEN_REPARSE_POINT;

	int status = with_connection([&](Connection* connection) -> int {
		return set_info(connection, path,
			SMB2_DELETE | SMB2_FILE_READ_ATTRIBUTES, options,
			SMB2_FILE_DISPOSITION_INFORMATION, &info);
	});

	if (directory)
		sStatCache.RemoveTree(path);
	forget(path);
	return status;
}


//	#pragma mark - FUSE operations


static int
smbfs_getattr(const char* path, struct stat* st)
{
	return stat_path(server_path(path), st);
}


static int
smbfs_readlink(const char* path, char* buffer, size_t size)
{
	if (size == 0)
		return B_BAD_VALUE;

	const std::string serverPath = server_path(path);

	return with_connection([&](Connection* connection) -> int {
		Request request;
		int status = complete(connection,
			smb2_readlink_async(connection->Context(), serverPath.c_str(),
				&Request::Callback, &request),
			request);
		if (status != 0)
			return status;

		strlcpy(buffer, request.data != NULL ? (const char*)request.data : "",
			size);
		for (char* c = buffer; *c != '\0'; c++) {
			if (*c == '\\')
				*c = '/';
		}
		return 0;
	});
}


static int
smbfs_mkdir(const char* path, mode_t mode)
{
	if (sOptions.readOnly)
		return B_READ_ONLY_DEVICE;

	const std::string serverPath = server_path(path);

	int status = with_connection([&](Connection* connection) -> int {
		Request request;
		return complete(connection,
			smb2_mkdir_async(connection->Context(), serverPath.c_str(),
				&Request::Callback, &request),
			request);
	});

	forget(serverPath);
	return status;
}


static int
smbfs_unlink(const char* path)
{
	return remove_entry(server_path(path), false);
}


static int
smbfs_rmdir(const char* path)
{
	return remove_entry(server_path(path), true);
}


static int
smbfs_rename(const char* from, const char* to)
{
	if (sOptions.readOnly)
		return B_READ_ONLY_DEVICE;

	const std::string fromPath = server_path(from);
	const std::string toPath = server_path(to);

	int status = with_connection([&](Connection* connection) -> int {
		Request request;
		return complete(connection,
			smb2_rename_async(connection->Context(), fromPath.c_str(),
				toPath.c_str(), &Request::Callback, &request),
			request);
	});

	sStatCache.RemoveTree(fromPath);
	sStatCache.RemoveTree(toPath);
	sStatCache.Remove(parent_path(fromPath));
	sStatCache.Remove(parent_path(toPath));

	if (status == 0) {
		// files that are open have to be found again under the new name
		// should the session be lost
		const std::string prefix = fromPath + "/";

		pthread_mutex_lock(&sHandlesLock);
		for (std::set<FileHandle*>::iterator it = sHandles.begin();
				it != sHandles.end(); ++it) {
			FileHandle* handle = *it;
			if (handle->path == fromPath)
				handle->path = toPath;
			else if (handle->path.compare(0, prefix.length(), prefix) == 0) {
				handle->path = toPath + "/"
					+ handle->path.substr(prefix.length());
			}
		}
		pthread_mutex_unlock(&sHandlesLock);
	}

	return status;
}


static int
smbfs_chmod(const char* path, mode_t mode)
{
	// The server has no such thing. Saying so would fail every copy that
	// means to keep the permissions.
	return 0;
}


static int
smbfs_chown(const char* path, uid_t user, gid_t group)
{
	return 0;
}


static int
smbfs_truncate(const char* path, off_t size)
{
	if (sOptions.readOnly)
		return B_READ_ONLY_DEVICE;

	const std::string serverPath = server_path(path);

	int status = with_connection([&](Connection* connection) -> int {
		Request request;
		return complete(connection,
			smb2_truncate_async(connection->Context(), serverPath.c_str(),
				size, &Request::Callback, &request),
			request);
	});

	forget(serverPath);
	return status;
}


static int
smbfs_ftruncate(const char* path, off_t size, struct fuse_file_info* info)
{
	FileHandle* handle = (FileHandle*)(addr_t)info->fh;
	if (handle == NULL)
		return smbfs_truncate(path, size);

	if (sOptions.readOnly)
		return B_READ_ONLY_DEVICE;

	int status = with_handle(handle,
		[&](Connection* connection, smb2fh* file) -> int {
			handle->bufferSize = 0;

			Request request;
			return complete(connection,
				smb2_ftruncate_async(connection->Context(), file, size,
					&Request::Callback, &request),
				request);
		});

	forget(server_path(path));
	return status;
}


static int
smbfs_utimens(const char* path, const struct timespec times[2])
{
	if (sOptions.readOnly)
		return B_READ_ONLY_DEVICE;

	// A time of zero leaves what the file has. libsmb2 takes the times since
	// 1970 and sends them since 1601, so zero is this; where time_t cannot
	// hold it, the times stay what they are.
	if (sizeof(time_t) < 8)
		return 0;
	const time_t kUnchanged = (time_t)-11644473600LL;

	struct smb2_file_basic_info basicInfo;
	memset(&basicInfo, 0, sizeof(basicInfo));
	basicInfo.creation_time.tv_sec = kUnchanged;
	basicInfo.change_time.tv_sec = kUnchanged;
	basicInfo.last_access_time.tv_sec = times[0].tv_sec;
	basicInfo.last_access_time.tv_usec = times[0].tv_nsec / 1000;
	basicInfo.last_write_time.tv_sec = times[1].tv_sec;
	basicInfo.last_write_time.tv_usec = times[1].tv_nsec / 1000;

	const std::string serverPath = server_path(path);

	int status = with_connection([&](Connection* connection) -> int {
		return set_info(connection, serverPath, SMB2_FILE_WRITE_ATTRIBUTES,
			0, SMB2_FILE_BASIC_INFORMATION, &basicInfo);
	});

	forget(serverPath);
	return status;
}


static int
smbfs_open(const char* path, struct fuse_file_info* info)
{
	const std::string serverPath = server_path(path);
	const int openMode = info->flags & O_ACCMODE;

	info->fh = 0;

	// Directories are opened too, to look at their attributes for one, and
	// there is nothing to do about that here.
	struct stat st;
	if (sStatCache.Lookup(serverPath, st) && S_ISDIR(st.st_mode))
		return 0;

	if (sOptions.readOnly && openMode != O_RDONLY)
		return B_READ_ONLY_DEVICE;

	int status = open_file(serverPath, openMode, 0, info);
	if (status != 0 && !Connection::IsLost(status)
		&& stat_path(serverPath, &st) == 0 && S_ISDIR(st.st_mode)) {
		return 0;
	}

	return status;
}


static int
smbfs_create(const char* path, mode_t mode, struct fuse_file_info* info)
{
	if (sOptions.readOnly)
		return B_READ_ONLY_DEVICE;

	const std::string serverPath = server_path(path);

	int status = open_file(serverPath, info->flags & O_ACCMODE,
		O_CREAT | (info->flags & (O_EXCL | O_TRUNC)), info);

	forget(serverPath);
	return status;
}


static int
smbfs_read(const char* path, char* buffer, size_t size, off_t offset,
	struct fuse_file_info* info)
{
	FileHandle* handle = (FileHandle*)(addr_t)info->fh;
	if (handle == NULL)
		return B_IS_A_DIRECTORY;

	return with_handle(handle,
		[&](Connection* connection, smb2fh* file) -> int {
			// is it what was read ahead?
			if (handle->bufferSize > 0 && offset >= handle->bufferOffset
				&& offset < handle->bufferOffset
					+ (off_t)handle->bufferSize) {
				const size_t skip = offset - handle->bufferOffset;
				const size_t count = std::min(size,
					handle->bufferSize - skip);

				// What is left of a request at the end of the buffer is asked
				// for next, unless the file has ended, which a buffer that
				// was not filled tells.
				if (count == size || handle->bufferSize < kReadAheadSize) {
					memcpy(buffer, handle->buffer + skip, count);
					handle->nextOffset = offset + count;
					return count;
				}
			}

			const bool sequential = offset == handle->nextOffset;

			if (sequential && size < kReadAheadSize) {
				if (handle->buffer == NULL)
					handle->buffer = (uint8*)malloc(kReadAheadSize);

				if (handle->buffer != NULL) {
					handle->bufferSize = 0;

					int bytesRead = read_from_server(connection, file,
						handle->buffer, kReadAheadSize, offset);
					if (bytesRead < 0)
						return bytesRead;

					handle->bufferOffset = offset;
					handle->bufferSize = bytesRead;

					const size_t count = std::min(size, (size_t)bytesRead);
					memcpy(buffer, handle->buffer, count);
					handle->nextOffset = offset + count;
					return count;
				}
			}

			int bytesRead = read_from_server(connection, file,
				(uint8*)buffer, size, offset);
			if (bytesRead >= 0)
				handle->nextOffset = offset + bytesRead;
			return bytesRead;
		});
}


static int
smbfs_write(const char* path, const char* buffer, size_t size, off_t offset,
	struct fuse_file_info* info)
{
	FileHandle* handle = (FileHandle*)(addr_t)info->fh;
	if (handle == NULL)
		return B_IS_A_DIRECTORY;
	if (sOptions.readOnly)
		return B_READ_ONLY_DEVICE;

	int status = with_handle(handle,
		[&](Connection* connection, smb2fh* file) -> int {
			handle->bufferSize = 0;
			handle->nextOffset = -1;

			const size_t maxSize = std::min(
				(size_t)smb2_get_max_write_size(connection->Context()),
				kMaxTransferSize);

			// Where the end of the file is when writing is to go there is
			// left to the file system to find out. The file is asked for by
			// its name: one that is open for writing only does not allow
			// for asking about it.
			if ((info->flags & O_APPEND) != 0) {
				pthread_mutex_lock(&sHandlesLock);
				const std::string filePath = handle->path;
				pthread_mutex_unlock(&sHandlesLock);

				smb2_stat_64 fileInfo;
				Request request;
				int status = complete(connection,
					smb2_stat_async(connection->Context(), filePath.c_str(),
						&fileInfo, &Request::Callback, &request),
					request);
				if (status != 0)
					return status;

				offset = fileInfo.smb2_size;
			}

			size_t total = 0;
			while (total < size) {
				const size_t count = std::min(size - total, maxSize);

				Request request;
				int status = complete(connection,
					smb2_pwrite_async(connection->Context(), file,
						(const uint8*)buffer + total, count, offset + total,
						&Request::Callback, &request),
					request);
				if (status < 0)
					return status;
				if (status == 0)
					break;

				total += status;
			}

			return total;
		});

	sStatCache.Remove(server_path(path));
	return status;
}


static int
smbfs_statfs(const char* path, struct statvfs* info)
{
	// Tracker asks whenever it draws the volume's icon, for how full it is.
	// The server is not asked as often as that.
	static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
	static struct smb2_statvfs sServerInfo;
	static bigtime_t sValidUntil = 0;

	struct smb2_statvfs serverInfo;

	pthread_mutex_lock(&sLock);
	const bool known = system_time() < sValidUntil;
	if (known)
		serverInfo = sServerInfo;
	pthread_mutex_unlock(&sLock);

	if (!known) {
		memset(&serverInfo, 0, sizeof(serverInfo));

		int status = with_connection([&](Connection* connection) -> int {
			Request request;
			return complete(connection,
				smb2_statvfs_async(connection->Context(),
					sOptions.path.c_str(), &serverInfo, &Request::Callback,
					&request),
				request);
		});
		if (status != 0)
			return status;

		pthread_mutex_lock(&sLock);
		sServerInfo = serverInfo;
		sValidUntil = system_time() + kVolumeInfoLifetime;
		pthread_mutex_unlock(&sLock);
	}

	memset(info, 0, sizeof(*info));
	info->f_bsize = serverInfo.f_bsize;
	info->f_frsize = serverInfo.f_frsize;
	info->f_blocks = serverInfo.f_blocks;
	info->f_bfree = serverInfo.f_bfree;
	info->f_bavail = serverInfo.f_bavail;
	info->f_namemax = 255;
	if (sOptions.readOnly)
		info->f_flag |= ST_RDONLY;

	return 0;
}


static int
smbfs_flush(const char* path, struct fuse_file_info* info)
{
	return 0;
}


static int
smbfs_release(const char* path, struct fuse_file_info* info)
{
	FileHandle* handle = (FileHandle*)(addr_t)info->fh;
	if (handle == NULL)
		return 0;

	pthread_mutex_lock(&sHandlesLock);
	sHandles.erase(handle);
	pthread_mutex_unlock(&sHandlesLock);

	// The server closes what a session had open when the session ends, so
	// there only is something to close as long as that still is around.
	Connection* connection = handle->connection;
	connection->Lock();

	if (connection->IsConnected()
		&& handle->generation == connection->Generation()) {
		Request request;
		complete(connection,
			smb2_close_async(connection->Context(), handle->handle,
				&Request::Callback, &request),
			request);
	}

	connection->Unlock();

	if ((handle->openMode & O_ACCMODE) != O_RDONLY)
		forget(handle->path);

	delete handle;
	info->fh = 0;
	return 0;
}


static int
smbfs_fsync(const char* path, int dataOnly, struct fuse_file_info* info)
{
	FileHandle* handle = (FileHandle*)(addr_t)info->fh;
	if (handle == NULL || (handle->openMode & O_ACCMODE) == O_RDONLY)
		return 0;

	return with_handle(handle,
		[&](Connection* connection, smb2fh* file) -> int {
			Request request;
			return complete(connection,
				smb2_fsync_async(connection->Context(), file,
					&Request::Callback, &request),
				request);
		});
}


/*!	The MIME type of a regular file at \a path, by the extension of its name
	alone, or an empty string when there is none to tell. The share cannot
	store attributes, and since the volume says it knows MIME types, Tracker
	takes what BEOS:TYPE says and does not read each file over the network to
	sniff it. The registrar's answers are kept per extension.
*/
static std::string
mime_type_for(const char* path)
{
	const char* name = strrchr(path, '/');
	name = name != NULL ? name + 1 : path;
	const char* dot = strrchr(name, '.');
	if (dot == NULL || dot == name || dot[1] == '\0')
		return std::string();

	struct stat st;
	if (stat_path(server_path(path), &st) != 0 || !S_ISREG(st.st_mode))
		return std::string();

	std::string extension(dot + 1);
	std::transform(extension.begin(), extension.end(), extension.begin(),
		[](unsigned char c) { return (char)tolower(c); });

	static pthread_mutex_t sLock = PTHREAD_MUTEX_INITIALIZER;
	static std::map<std::string, std::string> sTypes;

	pthread_mutex_lock(&sLock);
	std::map<std::string, std::string>::const_iterator found
		= sTypes.find(extension);
	if (found != sTypes.end()) {
		std::string type = found->second;
		pthread_mutex_unlock(&sLock);
		return type;
	}
	pthread_mutex_unlock(&sLock);

	BMimeType guessed;
	std::string fileName = "file." + extension;
	if (BMimeType::GuessMimeType(fileName.c_str(), &guessed) != B_OK) {
		// the registrar may not be there yet: don't remember that
		return std::string();
	}

	std::string type;
	if (guessed.Type() != NULL && strcmp(guessed.Type(), B_FILE_MIME_TYPE) != 0)
		type = guessed.Type();

	pthread_mutex_lock(&sLock);
	sTypes[extension] = type;
	pthread_mutex_unlock(&sLock);
	return type;
}


static int
smbfs_getxattr(const char* path, const char* name, char* value, size_t size)
{
	// The only attribute is the MIME type, from the file's name.
	if (strcmp(name, "BEOS:TYPE") != 0)
		return B_ENTRY_NOT_FOUND;

	std::string type = mime_type_for(path);
	if (type.empty())
		return B_ENTRY_NOT_FOUND;

	// with its terminating null, as BEOS:TYPE is stored elsewhere
	size_t length = type.length() + 1;
	if (value == NULL || size == 0)
		return (int)length;
	if (size < length)
		return B_BUFFER_OVERFLOW;
	memcpy(value, type.c_str(), length);
	return (int)length;
}


static int
smbfs_listxattr(const char* path, char* list, size_t size)
{
	static const char kTypeAttribute[] = "BEOS:TYPE";
	if (mime_type_for(path).empty())
		return 0;

	if (list == NULL || size == 0)
		return (int)sizeof(kTypeAttribute);
	if (size < sizeof(kTypeAttribute))
		return B_BUFFER_OVERFLOW;
	memcpy(list, kTypeAttribute, sizeof(kTypeAttribute));
	return (int)sizeof(kTypeAttribute);
}


static int
smbfs_readdir(const char* path, void* buffer, fuse_fill_dir_t filler,
	off_t offset, struct fuse_file_info* info)
{
	const std::string serverPath = server_path(path);

	return with_connection([&](Connection* connection) -> int {
		smb2_context* context = connection->Context();

		// the answer is the whole directory
		Request request;
		int status = complete(connection,
			smb2_opendir_async(context, serverPath.c_str(),
				&Request::Callback, &request),
			request);
		if (status != 0)
			return status;

		struct smb2dir* directory = (struct smb2dir*)request.data;
		if (directory == NULL)
			return B_IO_ERROR;

		while (struct smb2dirent* entry = smb2_readdir(context, directory)) {
			struct stat st;
			to_stat(entry->st, st);

			if (strcmp(entry->name, ".") != 0
				&& strcmp(entry->name, "..") != 0) {
				sStatCache.Insert(serverPath.empty() ? std::string(entry->name)
					: serverPath + "/" + entry->name, st);
			}

			if (filler(buffer, entry->name, &st, 0) != 0)
				break;
		}

		smb2_closedir(context, directory);
		return 0;
	});
}


static void*
smbfs_init(struct fuse_conn_info* info)
{
	// for smbfs_ioctl() to be asked what the volume is called
	info->want |= FUSE_CAP_HAIKU_FUSE_EXTENSIONS;
	return NULL;
}


static void
smbfs_destroy(void* data)
{
	sPool.DisconnectAll();
	syslog(LOG_INFO, "smbfs: //%s/%s unmounted\n", sOptions.server.c_str(),
		sOptions.share.c_str());
}


static int
smbfs_ioctl(const char* path, int command, void* argument,
	struct fuse_file_info* fileInfo, unsigned int flags, void* data)
{
	if ((uint32)command != FUSE_HAIKU_GET_DRIVE_INFO)
		return B_BAD_VALUE;

	fs_info* info = (fs_info*)argument;
	memset(info, 0, sizeof(*info));

	info->flags = B_FS_IS_PERSISTENT | B_FS_IS_SHARED | B_FS_HAS_MIME;
	if (sOptions.readOnly)
		info->flags |= B_FS_IS_READONLY;
	info->io_size = kMaxTransferSize;
	info->block_size = 4096;

	// The name has to be there whether or not the server is, or else nobody
	// can tell what it is they cannot reach.
	struct statvfs volumeInfo;
	if (smbfs_statfs("/", &volumeInfo) == 0) {
		info->block_size = volumeInfo.f_frsize != 0
			? volumeInfo.f_frsize : volumeInfo.f_bsize;
		info->total_blocks = volumeInfo.f_blocks;
		info->free_blocks = volumeInfo.f_bavail;
	}

	strlcpy(info->volume_name, sOptions.name.c_str(),
		sizeof(info->volume_name));
	snprintf(info->device_name, sizeof(info->device_name), "//%s/%s",
		sOptions.server.c_str(), sOptions.share.c_str());
	strlcpy(info->fsh_name, "smbfs", sizeof(info->fsh_name));

	return 0;
}


//	#pragma mark - mounting


static std::string
decode(const char* value)
{
	std::string result;
	for (; *value != '\0'; value++) {
		if (value[0] == '%' && isxdigit(value[1]) && isxdigit(value[2])) {
			const char hex[3] = { value[1], value[2], '\0' };
			result += (char)strtol(hex, NULL, 16);
			value += 2;
		} else
			result += *value;
	}
	return result;
}


/*!	The parameters:
		server=<host>[:<port>]
		share=<name>
		path=<directory>		the part of the share to mount, all of it if
								not given
		user=<name>				guest if not given
		password=<password>
		domain=<name>
		name=<name>				what to call the volume, the share's name if
								not given
		ro						no writing
		encrypt					have the traffic encrypted, SMB3 only
		timeout=<seconds>		how long the server may take to answer
		connections=<count>		how much may go on at once
*/
static status_t
parse_options(int argc, const char* const* argv)
{
	sOptions = Options();

	for (int i = 1; i < argc; i++) {
		const char* argument = argv[i];
		const char* equals = strchr(argument, '=');
		const std::string key = equals != NULL
			? std::string(argument, equals - argument) : argument;
		const std::string value = equals != NULL ? decode(equals + 1) : "";

		if (key == "server")
			sOptions.server = value;
		else if (key == "share")
			sOptions.share = value;
		else if (key == "path")
			sOptions.path = value;
		else if (key == "user")
			sOptions.user = value;
		else if (key == "password")
			sOptions.password = value;
		else if (key == "domain")
			sOptions.domain = value;
		else if (key == "name")
			sOptions.name = value;
		else if (key == "ro")
			sOptions.readOnly = true;
		else if (key == "encrypt")
			sOptions.encrypt = true;
		else if (key == "timeout")
			sOptions.timeout = atoi(value.c_str());
		else if (key == "connections")
			sOptions.connections = atoi(value.c_str());
		else {
			syslog(LOG_ERR, "smbfs: unknown parameter \"%s\"\n", key.c_str());
			return B_BAD_VALUE;
		}
	}

	if (sOptions.server.empty() || sOptions.share.empty()) {
		syslog(LOG_ERR, "smbfs: a server and a share have to be given\n");
		return B_BAD_VALUE;
	}

	// the server has it with backslashes, which libsmb2 sees to
	std::replace(sOptions.path.begin(), sOptions.path.end(), '\\', '/');
	while (!sOptions.path.empty() && sOptions.path[0] == '/')
		sOptions.path.erase(0, 1);
	while (!sOptions.path.empty()
		&& sOptions.path[sOptions.path.length() - 1] == '/') {
		sOptions.path.erase(sOptions.path.length() - 1);
	}

	if (sOptions.name.empty())
		sOptions.name = sOptions.share;
	std::replace(sOptions.name.begin(), sOptions.name.end(), '/', '-');

	sOptions.timeout = std::max(5, std::min(sOptions.timeout, 300));
	sOptions.connections = std::max(1, std::min(sOptions.connections, 8));

	return B_OK;
}


extern "C" __attribute__((visibility("default"))) int
main(int argc, char** argv)
{
	status_t status = parse_options(argc, argv);
	if (status != B_OK)
		return status;

	sStatCache.Clear();
	sPool.Init(&sOptions);

	// Mounting is when to find out that the share cannot be had, and not
	// whenever somebody first looks at the volume.
	Connection* connection = sPool.Acquire();
	status = connection->EnsureConnected();
	if (status == 0 && !sOptions.path.empty()) {
		smb2_stat_64 info;
		Request request;
		status = complete(connection,
			smb2_stat_async(connection->Context(), sOptions.path.c_str(),
				&info, &Request::Callback, &request),
			request);
		if (status == 0 && info.smb2_type != SMB2_TYPE_DIRECTORY)
			status = B_NOT_A_DIRECTORY;
	}
	connection->Unlock();

	if (status != 0) {
		sPool.DisconnectAll();
		return status < 0 ? status : B_ERROR;
	}

	syslog(LOG_INFO, "smbfs: //%s/%s mounted as \"%s\"%s\n",
		sOptions.server.c_str(), sOptions.share.c_str(),
		sOptions.name.c_str(), sOptions.readOnly ? ", read-only" : "");

	static struct fuse_operations operations;
	memset(&operations, 0, sizeof(operations));
	operations.getattr = smbfs_getattr;
	operations.readlink = smbfs_readlink;
	operations.mkdir = smbfs_mkdir;
	operations.unlink = smbfs_unlink;
	operations.rmdir = smbfs_rmdir;
	operations.rename = smbfs_rename;
	operations.chmod = smbfs_chmod;
	operations.chown = smbfs_chown;
	operations.truncate = smbfs_truncate;
	operations.ftruncate = smbfs_ftruncate;
	operations.utimens = smbfs_utimens;
	operations.open = smbfs_open;
	operations.create = smbfs_create;
	operations.read = smbfs_read;
	operations.write = smbfs_write;
	operations.statfs = smbfs_statfs;
	operations.flush = smbfs_flush;
	operations.release = smbfs_release;
	operations.fsync = smbfs_fsync;
	operations.getxattr = smbfs_getxattr;
	operations.listxattr = smbfs_listxattr;
	operations.readdir = smbfs_readdir;
	operations.init = smbfs_init;
	operations.destroy = smbfs_destroy;
	operations.ioctl = smbfs_ioctl;

	char* fuseArguments[] = { argv[0], NULL };
	return fuse_main(1, fuseArguments, &operations, NULL);
}
