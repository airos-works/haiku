/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Feed.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <DataIO.h>
#include <File.h>
#include <HttpRequest.h>
#include <HttpResult.h>
#include <OS.h>
#include <Url.h>
#include <UrlProtocolListener.h>
#include <UrlProtocolRoster.h>
#include <UrlRequest.h>

#include "Release.h"


using namespace BPrivate::Network;


// a transfer that has not moved for this long is given up
static const bigtime_t kStallTimeout = 60000000;
// how often a transfer that failed on the way is tried in all
static const int32 kAttempts = 5;


/*!	Whether trying again may help: anything but what the server answered
	(HTTP 4xx), a bad address and cancelling. A transfer can also fail
	half-way with a reset connection or an interrupted read (seen on the
	Raspberry Pi: "Interrupted system call").
*/
static bool
is_transient(status_t error)
{
	switch (error) {
		case B_OK:
		case B_CANCELED:
		case B_ERROR:
		case B_ENTRY_NOT_FOUND:
		case B_BAD_VALUE:
		case B_NOT_SUPPORTED:
		case B_RESULT_NOT_REPRESENTABLE:
		case B_BAD_DATA:
			return false;
		default:
			return true;
	}
}


DownloadListener::~DownloadListener()
{
}


// #pragma mark - Fetcher::Listener


class Fetcher::Listener : public BUrlProtocolListener {
public:
	Listener(DownloadListener* listener, off_t rangeStart)
		:
		fListener(listener),
		fRangeStart(rangeStart),
		fLastActivity(system_time())
	{
	}

	virtual void DownloadProgress(BUrlRequest*, off_t bytesReceived,
		off_t bytesTotal)
	{
		fLastActivity = system_time();
		if (fListener != NULL) {
			fListener->DownloadProgress(fRangeStart + bytesReceived,
				bytesTotal > 0 ? fRangeStart + bytesTotal : -1);
		}
	}

	virtual void BytesWritten(BUrlRequest*, size_t)
	{
		fLastActivity = system_time();
	}

	virtual void HeadersReceived(BUrlRequest*)
	{
		fLastActivity = system_time();
	}

	bigtime_t LastActivity() const
	{
		return fLastActivity;
	}

private:
	DownloadListener*	fListener;
	off_t				fRangeStart;
	volatile bigtime_t	fLastActivity;
};


// #pragma mark - Fetcher


Fetcher::Fetcher()
	:
	fCancelled(0),
	fRequest(NULL),
	fServerTime(0)
{
}


/*!	An HTTP date, "Mon, 26 Oct 2026 03:04:05 GMT", or 0.
*/
static time_t
parse_http_date(const char* text)
{
	static const char* const kMonths[] = { "Jan", "Feb", "Mar", "Apr", "May",
		"Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	char weekday[8], month[8];
	struct tm tm = {};
	if (text == NULL || sscanf(text, "%7[^,], %d %7s %d %d:%d:%d", weekday,
			&tm.tm_mday, month, &tm.tm_year, &tm.tm_hour, &tm.tm_min,
			&tm.tm_sec) != 7) {
		return 0;
	}
	tm.tm_mon = -1;
	for (int i = 0; i < 12; i++) {
		if (strcmp(month, kMonths[i]) == 0)
			tm.tm_mon = i;
	}
	if (tm.tm_mon < 0)
		return 0;
	return utc_time(tm.tm_year, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
		tm.tm_min, tm.tm_sec);
}


Fetcher::~Fetcher()
{
}


void
Fetcher::Cancel()
{
	atomic_set(&fCancelled, 1);
	BUrlRequest* request = (BUrlRequest*)fRequest;
	if (request != NULL)
		request->Stop();
}


bool
Fetcher::IsCancelled() const
{
	return atomic_get((int32*)&fCancelled) != 0;
}


status_t
Fetcher::FetchText(const BString& url, BString& _text, BString& _error,
	size_t maxSize)
{
	BMallocIO output;
	status_t error;
	for (int32 attempt = 0; attempt < kAttempts; attempt++) {
		if (attempt > 0)
			_Pause(attempt);
		output.SetSize(0);
		output.Seek(0, SEEK_SET);
		error = _Fetch(url, &output, 0, -1, _error, NULL);
		if (!is_transient(error) || IsCancelled())
			break;
	}
	if (error != B_OK)
		return error;
	if (output.BufferLength() > maxSize) {
		_error.SetToFormat("%s is too large", url.String());
		return B_BAD_DATA;
	}
	_text.SetTo((const char*)output.Buffer(), output.BufferLength());
	return B_OK;
}


status_t
Fetcher::FetchFile(const BString& url, const char* path, off_t expectedSize,
	BString& _error, DownloadListener* listener)
{
	BFile file(path, B_READ_WRITE | B_CREATE_FILE);
	status_t error = file.InitCheck();
	if (error != B_OK) {
		_error.SetToFormat("cannot write %s: %s", path, strerror(error));
		return error;
	}

	for (int32 attempt = 0; attempt < kAttempts; attempt++) {
		if (attempt > 0)
			_Pause(attempt);
		off_t size = 0;
		file.GetSize(&size);
		if (expectedSize >= 0 && size > expectedSize) {
			file.SetSize(0);
			size = 0;
		}
		if (expectedSize >= 0 && size == expectedSize)
			return B_OK;

		file.Seek(size, SEEK_SET);
		error = _Fetch(url, &file, size, size, _error, listener);
		if (error == B_OK) {
			if (expectedSize >= 0) {
				file.GetSize(&size);
				if (size != expectedSize) {
					_error.SetToFormat("%s: got %" B_PRIdOFF " of %" B_PRIdOFF
						" bytes", url.String(), size, expectedSize);
					error = B_IO_ERROR;
					continue;
				}
			}
			return B_OK;
		}
		if (IsCancelled())
			break;
		if (error == B_RESULT_NOT_REPRESENTABLE) {
			// the server refused the range: start over
			file.SetSize(0);
			continue;
		}
		if (!is_transient(error))
			break;
	}
	return error;
}


/*!	The time of the server of \a url, from the Date header of what its host
	answers over plain HTTP (github.com redirects to HTTPS, with a Date).
	For a clock that is too far off to check the server's certificate: the
	time is no more trusted than before, the downloads are still checked
	against the feed's SHA-256s.
*/
time_t
Fetcher::FetchServerTime(const BString& url)
{
	BUrl address(url.String(), true);
	if (!address.IsValid() || !address.HasHost())
		return 0;
	BUrl plain;
	plain.SetProtocol("http");
	plain.SetHost(address.Host());
	plain.SetPath("/");

	BMallocIO output;
	BUrlRequest* request = BUrlProtocolRoster::MakeRequest(plain, &output);
	BHttpRequest* http = dynamic_cast<BHttpRequest*>(request);
	if (http == NULL) {
		delete request;
		return 0;
	}
	http->SetMethod(B_HTTP_HEAD);
	http->SetFollowLocation(false);
	http->SetDiscardData(true);

	time_t serverTime = 0;
	fRequest = request;
	thread_id thread = request->Run();
	if (thread >= 0) {
		// a few seconds at most
		bigtime_t until = system_time() + 10000000;
		while (wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 500000, NULL)
				== B_TIMED_OUT) {
			if (IsCancelled() || system_time() > until)
				request->Stop();
		}
		const BHttpResult* result = dynamic_cast<const BHttpResult*>(
			&request->Result());
		if (result != NULL) {
			serverTime = parse_http_date(
				result->Headers().HeaderValue("Date"));
		}
	}
	fRequest = NULL;
	delete request;
	if (serverTime > 0)
		fServerTime = serverTime;
	return serverTime;
}


/*!	A second or more before the next attempt, unless cancelled.
*/
void
Fetcher::_Pause(int32 attempt)
{
	bigtime_t until = system_time() + attempt * 1000000LL;
	while (!IsCancelled() && system_time() < until)
		snooze(100000);
}


status_t
Fetcher::_Fetch(const BString& url, BDataIO* output, off_t rangeStart,
	off_t outputSize, BString& _error, DownloadListener* downloadListener)
{
	if (IsCancelled()) {
		_error = "cancelled";
		return B_CANCELED;
	}

	BUrl address(url.String(), true);
	if (!address.IsValid()) {
		_error.SetToFormat("not an address: %s", url.String());
		return B_BAD_VALUE;
	}

	Listener listener(downloadListener, rangeStart);
	BUrlRequest* request = BUrlProtocolRoster::MakeRequest(address, output,
		&listener);
	if (request == NULL) {
		_error.SetToFormat("cannot fetch %s", url.String());
		return B_NOT_SUPPORTED;
	}

	BHttpRequest* http = dynamic_cast<BHttpRequest*>(request);
	if (http != NULL) {
		http->SetFollowLocation(true);
		http->SetMaxRedirections(8);
		http->SetStopOnError(true);
		if (rangeStart > 0)
			http->SetRangeStart(rangeStart);
	}

	fRequest = request;
	thread_id thread = request->Run();
	if (thread < 0) {
		fRequest = NULL;
		delete request;
		_error.SetToFormat("cannot start the transfer: %s", strerror(thread));
		return thread;
	}

	bool stalled = false;
	while (wait_for_thread_etc(thread, B_RELATIVE_TIMEOUT, 1000000, NULL)
			== B_TIMED_OUT) {
		if (!stalled && system_time() - listener.LastActivity()
				> kStallTimeout) {
			stalled = true;
			request->Stop();
		}
	}
	fRequest = NULL;

	status_t error = request->Status();
	uint16 code = 0;
	const BHttpResult* result = dynamic_cast<const BHttpResult*>(
		&request->Result());
	if (result != NULL) {
		code = result->StatusCode();
		time_t serverTime = parse_http_date(
			result->Headers().HeaderValue("Date"));
		if (serverTime > 0)
			fServerTime = serverTime;
	}
	delete request;

	if (IsCancelled()) {
		_error = "cancelled";
		return B_CANCELED;
	}
	if (stalled) {
		_error.SetToFormat("%s: no data for a minute", url.String());
		return B_DEV_TIMEOUT;
	}
	// An error answer stops the request (SetStopOnError()), which then
	// reports itself interrupted: the answer is what counts.
	if (error != B_OK && (http == NULL || code < 300)) {
		_error.SetToFormat("%s: %s", url.String(), strerror(error));
		return error == B_ERROR ? B_IO_ERROR : error;
	}

	if (http != NULL) {
		if (code >= 300 && code < 400) {
			// A redirection is followed (GitHub sends every download on to
			// its CDN): stopping at one means the transfer broke on the way.
			// Seen on QEMU: worth another attempt.
			_error.SetToFormat("%s: the transfer stopped at a redirection "
				"(HTTP %u, %s)", url.String(), code, strerror(error));
			return B_IO_ERROR;
		}
		if (code == 416) {
			_error.SetToFormat("%s: the server refused to resume",
				url.String());
			return B_RESULT_NOT_REPRESENTABLE;
		}
		if (code < 200 || code >= 300) {
			_error.SetToFormat("%s: HTTP %u", url.String(), code);
			return code == 404 ? B_ENTRY_NOT_FOUND
				: code >= 500 ? B_IO_ERROR : B_ERROR;
		}
		if (rangeStart > 0 && code != 206) {
			// the server sent the whole file again
			_error.SetToFormat("%s: the server cannot resume", url.String());
			if (BFile* file = dynamic_cast<BFile*>(output))
				file->SetSize(0);
			return B_IO_ERROR;
		}
	}

	return B_OK;
}


// #pragma mark - Feed


Feed::Feed()
	:
	fBuilds(20)
{
}


status_t
Feed::Fetch(const BString& baseURL, Fetcher& fetcher, BString& _error)
{
	fBaseURL = baseURL;
	if (!fBaseURL.EndsWith("/"))
		fBaseURL << '/';
	fBuilds.MakeEmpty();

	BString text;
	status_t error = fetcher.FetchText(Resolve(fBaseURL, "index.json"), text,
		_error);
	if (error != B_OK)
		return error;

	BMessage index;
	error = json::Parse(text, index);
	if (error != B_OK) {
		_error.SetToFormat("the feed's index.json is not valid JSON");
		return error;
	}

	if (json::Number(index, "format", 0) != 1) {
		_error = "the feed has a format this System Updater does not know";
		return B_NOT_SUPPORTED;
	}
	fTarget = json::String(index, "target");

	BMessage builds;
	if (!json::Object(index, "builds", builds)) {
		_error = "the feed lists no releases";
		return B_BAD_DATA;
	}

	BMessage entry;
	for (int32 i = 0; json::At(builds, i, entry); i++) {
		FeedBuild* build = new FeedBuild;
		build->id = json::String(entry, "id");
		build->built = json::String(entry, "built");
		build->version = json::String(entry, "version",
			Release::VersionForId(build->id));
		build->manifest = Resolve(fBaseURL, json::String(entry, "manifest"));
		build->manifestSha256 = json::String(entry, "manifest_sha256");
		build->size = (off_t)json::Number(entry, "size", -1);
		BMessage haiku;
		if (json::Object(entry, "haiku", haiku))
			build->haikuRevision = json::String(haiku, "revision");
		json::Object(entry, "changes", build->changes);
		if (build->id.IsEmpty()) {
			delete build;
			continue;
		}
		fBuilds.AddItem(build);
	}

	// newest first, whatever the file's order
	fBuilds.SortItems([](const FeedBuild* a, const FeedBuild* b) {
		return -Release::CompareIds(a->id, b->id);
	});

	return B_OK;
}


const FeedBuild*
Feed::FindBuild(const char* id) const
{
	for (int32 i = 0; const FeedBuild* build = fBuilds.ItemAt(i); i++) {
		if (build->id == id)
			return build;
	}
	return NULL;
}


/*static*/ BString
Feed::Resolve(const BString& base, const BString& relative)
{
	if (relative.FindFirst("://") >= 0)
		return relative;

	BString result = base;
	if (relative.StartsWith("/")) {
		// relative to the server
		int32 scheme = result.FindFirst("://");
		int32 path = scheme >= 0 ? result.FindFirst('/', scheme + 3) : -1;
		if (path >= 0)
			result.Truncate(path);
		return result << relative;
	}

	int32 slash = result.FindLast('/');
	if (slash >= 0)
		result.Truncate(slash + 1);

	BString rest = relative;
	while (rest.StartsWith("../")) {
		rest.Remove(0, 3);
		// drop the last directory of the result
		if (result.EndsWith("/"))
			result.Truncate(result.Length() - 1);
		int32 previous = result.FindLast('/');
		int32 scheme = result.FindFirst("://");
		if (previous > scheme + 2)
			result.Truncate(previous + 1);
		else
			result << '/';
	}
	while (rest.StartsWith("./"))
		rest.Remove(0, 2);
	return result << rest;
}
