/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Feed.h"

#include <stdio.h>
#include <string.h>

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
	fRequest(NULL)
{
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
	status_t error = _Fetch(url, &output, 0, -1, _error, NULL);
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

	for (int attempt = 0; attempt < 5; attempt++) {
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
		if (IsCancelled() || error == B_RESULT_NOT_REPRESENTABLE) {
			// cancelled, or the server refused the range: start over next time
			if (error == B_RESULT_NOT_REPRESENTABLE)
				file.SetSize(0);
			break;
		}
		if (error != B_IO_ERROR && error != B_DEV_TIMEOUT)
			break;
	}
	return error;
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
	if (result != NULL)
		code = result->StatusCode();
	delete request;

	if (IsCancelled()) {
		_error = "cancelled";
		return B_CANCELED;
	}
	if (stalled) {
		_error.SetToFormat("%s: no data for a minute", url.String());
		return B_DEV_TIMEOUT;
	}
	if (error != B_OK) {
		_error.SetToFormat("%s: %s", url.String(), strerror(error));
		return error == B_ERROR ? B_IO_ERROR : error;
	}

	if (http != NULL) {
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
