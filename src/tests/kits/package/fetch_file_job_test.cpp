/*
 * Copyright 2026, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	FetchFileJob (the package kit's downloads: pkgman, SoftwareUpdater,
	HaikuDepot, repository refreshes) against a small HTTP server on the
	loopback interface (airos-works/haiku#231): error answers are retried a
	few times at most, and only those that may change; error pages never end
	up in the file; a server that ignores the range or refuses it does not
	leave a mixed file.
*/


#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <Entry.h>
#include <File.h>
#include <OS.h>
#include <String.h>
#include <package/Context.h>

#include "FetchFileJob.h"


using BPackageKit::BContext;
using BPackageKit::BDecisionProvider;
using BPackageKit::BPrivate::FetchFileJob;


static const int kFileSize = 100000;
static const bigtime_t kDeadline = 40000000LL;


// #pragma mark - server


enum Mode {
	kAlways503,
	kNotFound,
	kTwice503ThenFile,
	kIgnoreRange,
	kRangeTooLarge,
	kAlways500,
	kOnce429ThenFile
};

static int sListenSocket = -1;
static int sPort = 0;
static volatile Mode sMode;
static int32 sRequests;
static char sRanges[8][64];


static char
file_byte(int index)
{
	return (char)((index * 7) % 251);
}


static void
send_all(int fd, const void* data, size_t size)
{
	const char* bytes = (const char*)data;
	while (size > 0) {
		ssize_t written = send(fd, bytes, size, 0);
		if (written <= 0)
			return;
		bytes += written;
		size -= written;
	}
}


static void
send_answer(int fd, int code, const char* reason, const char* extraHeaders,
	const char* body, size_t bodySize)
{
	char header[512];
	snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\n%s"
		"Content-Length: %zu\r\nConnection: close\r\n\r\n", code, reason,
		extraHeaders, bodySize);
	send_all(fd, header, strlen(header));
	send_all(fd, body, bodySize);
}


static void
send_file(int fd, int from)
{
	char* body = (char*)malloc(kFileSize);
	for (int i = 0; i < kFileSize; i++)
		body[i] = file_byte(i);
	if (from > 0) {
		char extra[128];
		snprintf(extra, sizeof(extra), "Content-Range: bytes %d-%d/%d\r\n",
			from, kFileSize - 1, kFileSize);
		send_answer(fd, 206, "Partial Content", extra, body + from,
			kFileSize - from);
	} else
		send_answer(fd, 200, "OK", "", body, kFileSize);
	free(body);
}


static status_t
server_thread(void*)
{
	static const char kErrorPage[] = "<html>an error page, not the file"
		"................................................</html>";
	while (true) {
		int fd = accept(sListenSocket, NULL, NULL);
		if (fd < 0)
			return B_OK;

		char request[4096];
		size_t length = 0;
		while (length < sizeof(request) - 1) {
			ssize_t bytes = recv(fd, request + length,
				sizeof(request) - 1 - length, 0);
			if (bytes <= 0)
				break;
			length += bytes;
			request[length] = '\0';
			if (strstr(request, "\r\n\r\n") != NULL)
				break;
		}
		request[length] = '\0';

		int32 number = atomic_add(&sRequests, 1);
		int rangeStart = -1;
		const char* range = strstr(request, "Range: bytes=");
		if (range != NULL)
			rangeStart = atoi(range + strlen("Range: bytes="));
		if (number < 8) {
			snprintf(sRanges[number], sizeof(sRanges[number]), "%d",
				rangeStart);
		}

		switch (sMode) {
			case kAlways503:
				send_answer(fd, 503, "Service Unavailable",
					"Retry-After: 0\r\n", kErrorPage, sizeof(kErrorPage) - 1);
				break;
			case kAlways500:
				send_answer(fd, 500, "Internal Server Error", "", kErrorPage,
					sizeof(kErrorPage) - 1);
				break;
			case kNotFound:
				send_answer(fd, 404, "Not Found", "", kErrorPage,
					sizeof(kErrorPage) - 1);
				break;
			case kTwice503ThenFile:
				if (number < 2) {
					send_answer(fd, 503, "Service Unavailable",
						"Retry-After: 0\r\n", kErrorPage,
						sizeof(kErrorPage) - 1);
				} else
					send_file(fd, rangeStart > 0 ? rangeStart : 0);
				break;
			case kIgnoreRange:
				send_file(fd, 0);
				break;
			case kOnce429ThenFile:
				if (number < 1) {
					send_answer(fd, 429, "Too Many Requests",
						"Retry-After: 1\r\n", kErrorPage,
						sizeof(kErrorPage) - 1);
				} else
					send_file(fd, rangeStart > 0 ? rangeStart : 0);
				break;
			case kRangeTooLarge:
				if (rangeStart >= kFileSize) {
					send_answer(fd, 416, "Range Not Satisfiable", "",
						kErrorPage, sizeof(kErrorPage) - 1);
				} else
					send_file(fd, rangeStart > 0 ? rangeStart : 0);
				break;
		}
		close(fd);
	}
}


// #pragma mark - client


class Decisions : public BDecisionProvider {
public:
	virtual bool YesNoDecisionNeeded(const BString&, const BString&,
		const BString&, const BString&, const BString&)
	{
		return true;
	}
};


struct JobRun {
	FetchFileJob*	job;
	status_t		result;
};


static status_t
job_thread(void* data)
{
	JobRun* run = (JobRun*)data;
	run->result = run->job->Run();
	return B_OK;
}


static int sFailures = 0;


static void
fail(const char* what)
{
	printf("FAIL: %s\n", what);
	sFailures++;
}


/*!	Runs one download into \a path (with \a preexisting bytes of the file,
	or of garbage, in it already); returns the job's result, or B_TIMED_OUT
	if it was still running at the deadline.
*/
static status_t
fetch(Mode mode, const char* path, int preexisting, bool garbage,
	bigtime_t& _elapsed)
{
	sMode = mode;
	sRequests = 0;
	memset(sRanges, 0, sizeof(sRanges));

	unlink(path);
	if (preexisting > 0) {
		BFile file(path, B_CREATE_FILE | B_WRITE_ONLY | B_ERASE_FILE);
		for (int i = 0; i < preexisting; i++) {
			char byte = garbage ? 'G' : file_byte(i);
			file.Write(&byte, 1);
		}
	}

	BEntry entry(path);
	Decisions decisions;
	BSupportKit::BJobStateListener listener;
	BContext context(decisions, listener);
	BString url;
	url.SetToFormat("http://127.0.0.1:%d/file", sPort);
	FetchFileJob* job = new FetchFileJob(context, "test", url, entry);

	JobRun run = { job, B_ERROR };
	bigtime_t start = system_time();
	thread_id thread = spawn_thread(job_thread, "fetch", B_NORMAL_PRIORITY,
		&run);
	resume_thread(thread);
	status_t status = wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT,
		kDeadline, NULL);
	_elapsed = system_time() - start;
	if (status == B_TIMED_OUT) {
		job->Stop();
		kill_thread(thread);
		return B_TIMED_OUT;
	}
	// the job leaks on purpose: its request may still refer to it
	return run.result;
}


static bool
file_is_complete(const char* path)
{
	BFile file(path, B_READ_ONLY);
	off_t size;
	if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK) {
		printf("  no file\n");
		return false;
	}
	if (size != kFileSize) {
		printf("  file size %lld, expected %d\n", (long long)size, kFileSize);
		return false;
	}
	char* data = (char*)malloc(kFileSize);
	file.ReadAt(0, data, kFileSize);
	for (int i = 0; i < kFileSize; i++) {
		if (data[i] != file_byte(i)) {
			printf("  wrong byte at %d\n", i);
			free(data);
			return false;
		}
	}
	free(data);
	return true;
}


static void
report(const char* what, status_t result, bigtime_t elapsed)
{
	printf("%s: %s after %.1f s, %d request(s), ranges:", what,
		strerror(result), elapsed / 1e6, (int)sRequests);
	for (int i = 0; i < sRequests && i < 8; i++)
		printf(" %s", sRanges[i]);
	printf("\n");
}


static void
crash_handler(int signal)
{
	printf("FAIL: signal %d\n", signal);
	_exit(1);
}


int
main()
{
	signal(SIGSEGV, crash_handler);

	sListenSocket = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(sListenSocket, (sockaddr*)&address, sizeof(address)) != 0
		|| listen(sListenSocket, 8) != 0) {
		printf("FAIL: no server socket: %s\n", strerror(errno));
		return 1;
	}
	socklen_t length = sizeof(address);
	getsockname(sListenSocket, (sockaddr*)&address, &length);
	sPort = ntohs(address.sin_port);
	resume_thread(spawn_thread(server_thread, "server", B_NORMAL_PRIORITY,
		NULL));

	char path[] = "/tmp/fetch_file_job_test.XXXXXX";
	int fd = mkstemp(path);
	if (fd >= 0)
		close(fd);

	bigtime_t elapsed;
	status_t result = fetch(kAlways503, path, 0, false, elapsed);
	report("503 every time", result, elapsed);
	if (result == B_OK || result == B_TIMED_OUT)
		fail("503 every time: did not give up");
	if (sRequests > 5)
		fail("503 every time: more than 5 requests");

	result = fetch(kAlways500, path, 0, false, elapsed);
	report("500 every time", result, elapsed);
	if (result == B_OK || result == B_TIMED_OUT)
		fail("500 every time: did not give up");
	if (sRequests != 1)
		fail("500 every time: asked again");

	result = fetch(kNotFound, path, 0, false, elapsed);
	report("404", result, elapsed);
	if (result == B_OK || result == B_TIMED_OUT)
		fail("404: no error");
	if (sRequests != 1)
		fail("404: asked again");

	result = fetch(kTwice503ThenFile, path, 0, false, elapsed);
	report("503 twice, then the file", result, elapsed);
	if (result != B_OK)
		fail("503 twice, then the file: failed");
	else if (!file_is_complete(path))
		fail("503 twice, then the file: the file is not the file");

	result = fetch(kOnce429ThenFile, path, 0, false, elapsed);
	report("429 with Retry-After: 1, then the file", result, elapsed);
	if (result != B_OK)
		fail("429, then the file: failed");
	else if (!file_is_complete(path))
		fail("429, then the file: the file is not the file");
	else if (elapsed < 1000000)
		fail("429, then the file: Retry-After not honoured");

	result = fetch(kIgnoreRange, path, 1000, false, elapsed);
	report("resume, server ignores the range", result, elapsed);
	if (result != B_OK)
		fail("server ignores the range: failed");
	else if (!file_is_complete(path))
		fail("server ignores the range: the file is not the file");

	result = fetch(kRangeTooLarge, path, kFileSize + 500, true, elapsed);
	report("resume past the end (416)", result, elapsed);
	if (result != B_OK)
		fail("416: did not start over");
	else if (!file_is_complete(path))
		fail("416: the file is not the file");

	unlink(path);
	printf("%s\n", sFailures == 0 ? "PASS" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
