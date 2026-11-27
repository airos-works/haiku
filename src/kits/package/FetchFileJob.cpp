/*
 * Copyright 2011-2021, Haiku, Inc. All Rights Reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		Axel Dörfler <axeld@pinc-software.de>
 *		Rene Gollent <rene@gollent.com>
 *		Oliver Tappe <zooey@hirschkaefer.de>
 *		Stephan Aßmus <superstippi@gmx.de>
 */


#include "FetchFileJob.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <Path.h>

#ifdef HAIKU_TARGET_PLATFORM_HAIKU
#	include <HttpRequest.h>
#	include <UrlRequest.h>
#	include <UrlProtocolRoster.h>
using namespace BPrivate::Network;
#endif

#include <algorithm>

#include "FetchUtils.h"


namespace BPackageKit {

namespace BPrivate {


#ifdef HAIKU_TARGET_PLATFORM_HAIKU

static const int32 kMaxFailures = 5;
	// attempts in a row that did not get any further
static const bigtime_t kStallTimeout = 60000000LL;
	// how long a connection may stay silent
static const bigtime_t kMaxRetryAfter = 60000000LL;
	// the longest "Retry-After" that is waited for


FetchFileJob::FetchFileJob(const BContext& context, const BString& title,
	const BString& fileURL, const BEntry& targetEntry)
	:
	inherited(context, title),
	fFileURL(fileURL),
	fTargetEntry(targetEntry),
	fTargetFile(&targetEntry, B_CREATE_FILE | B_WRITE_ONLY),
	fError(B_ERROR),
	fDownloadProgress(0.0),
	fBytes(0),
	fTotalBytes(0),
	fUrlRequest(NULL),
	fRangeStart(0),
	fStatusCode(0),
	fRetry(false),
	fRetryAfter(-1),
	fStopped(false)
{
}


FetchFileJob::~FetchFileJob()
{
}


float
FetchFileJob::DownloadProgress() const
{
	return fDownloadProgress;
}


const char*
FetchFileJob::DownloadURL() const
{
	return fFileURL.String();
}


const char*
FetchFileJob::DownloadFileName() const
{
	return fTargetEntry.Name();
}


off_t
FetchFileJob::DownloadBytes() const
{
	return fBytes;
}


off_t
FetchFileJob::DownloadTotalBytes() const
{
	return fTotalBytes;
}


/*!	Downloads the file, resuming after a broken connection. An attempt that
	gets no further than the one before (an error answer, a connection that
	fails or goes silent) is repeated after a pause of 1, 2, 4 and 8 seconds
	(or what the server's "Retry-After" asks for), at most kMaxFailures times
	in a row; an answer that will not change by asking again (404, 403, ...)
	ends the download at once. Before, every 4xx and 5xx answer was repeated
	at once and for ever, with its error page appended to the file each time,
	and a connection that went silent blocked the download for good.
*/
status_t
FetchFileJob::Execute()
{
	status_t result = fTargetFile.InitCheck();
	if (result != B_OK)
		return result;

	result = FetchUtils::SetFileType(fTargetFile,
		"application/x-vnd.haiku-package");
	if (result != B_OK) {
		fprintf(stderr, "failed to set file type for '%s': %s\n",
			DownloadFileName(), strerror(result));
	}

	int32 failures = 0;
	while (true) {
		off_t currentPosition = 0;
		if (fTargetFile.GetSize(&currentPosition) != B_OK)
			currentPosition = 0;

		fUrlRequest = BUrlProtocolRoster::MakeRequest(fFileURL.String(),
			&fTargetFile, this);
		if (fUrlRequest == NULL)
			return B_BAD_VALUE;

		fRangeStart = 0;
		fStatusCode = 0;
		fRetry = false;
		fRetryAfter = -1;

		BHttpRequest* http = dynamic_cast<BHttpRequest*>(fUrlRequest);
		if (http != NULL) {
			http->SetTimeout(kStallTimeout);

			// Try to resume the download where we left off
			if (currentPosition > 0) {
				http->SetRangeStart(currentPosition);
				fRangeStart = currentPosition;
			}
		}
		fTargetFile.Seek(currentPosition, SEEK_SET);

		thread_id thread = fUrlRequest->Run();
		wait_for_thread(thread, NULL);

		if (fError == B_OK)
			break;

		off_t size = currentPosition;
		fTargetFile.GetSize(&size);
		bool gotData = fStatusCode == B_HTTP_STATUS_OK
			|| fStatusCode == B_HTTP_STATUS_PARTIAL_CONTENT;
		if (fStatusCode != 0 && !gotData) {
			// The body of an error answer is not part of the file.
			fTargetFile.SetSize(fStatusCode
				== B_HTTP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE
					? 0 : currentPosition);
		}

		if (fStopped || !fRetry)
			break;

		if (gotData && size > currentPosition) {
			// The connection broke, but the file grew: resume at once.
			failures = 0;
			continue;
		}

		if (++failures >= kMaxFailures)
			break;

		bigtime_t pause = 1000000LL << (failures - 1);
		if (fRetryAfter >= 0)
			pause = std::min(fRetryAfter, kMaxRetryAfter);
		fprintf(stderr, "%s: %s, trying again in %" B_PRIdBIGTIME " s\n",
			DownloadFileName(), fStatusCode != 0 && !gotData
				? (BString("HTTP ") << fStatusCode).String()
				: strerror(fError), pause / 1000000);
		_Pause(pause);
		if (fStopped)
			break;
	}

	if (fStopped && fError == B_OK)
		fError = B_CANCELED;

	if (fError == B_OK) {
		result = FetchUtils::MarkDownloadComplete(fTargetFile);
		if (result != B_OK) {
			fprintf(stderr, "failed to mark download '%s' as complete: %s\n",
				DownloadFileName(), strerror(result));
		}
	}

	return fError;
}


void
FetchFileJob::_Pause(bigtime_t duration)
{
	bigtime_t until = system_time() + duration;
	while (!fStopped && system_time() < until)
		snooze(std::min(until - system_time(), (bigtime_t)100000));
}


/*!	Called before any of the body is written. A server that ignores the
	range sends the whole file again: start the file over.
*/
void
FetchFileJob::HeadersReceived(BUrlRequest* request)
{
	const BHttpResult* httpResult
		= dynamic_cast<const BHttpResult*>(&request->Result());
	if (httpResult == NULL)
		return;

	if (httpResult->StatusCode() == B_HTTP_STATUS_OK && fRangeStart > 0) {
		fTargetFile.SetSize(0);
		fTargetFile.Seek(0, SEEK_SET);
		fRangeStart = 0;
	}
}


void
FetchFileJob::DownloadProgress(BUrlRequest*, off_t bytesReceived,
	off_t bytesTotal)
{
	if (bytesTotal != 0) {
		fBytes = bytesReceived;
		fTotalBytes = bytesTotal;
		fDownloadProgress = (float)bytesReceived/bytesTotal;
		NotifyStateListeners();
	}
}


/*!	Whether the transfer failed in a way that another attempt may mend: the
	connection broke or went silent.
*/
static bool
is_transport_error(status_t error)
{
	switch (error) {
		case B_IO_ERROR:
		case B_DEV_TIMEOUT:
		case B_TIMED_OUT:
		case B_WOULD_BLOCK:
		case ECONNRESET:
		case ECONNABORTED:
		case ENETRESET:
		case EPIPE:
			return true;
		default:
			return false;
	}
}


/*!	The seconds of a "Retry-After" header, or -1. (The HTTP date form is
	rare and gets the default pause.)
*/
static bigtime_t
retry_after(const BHttpResult& result)
{
	const char* value = result.Headers()["Retry-After"];
	if (value == NULL || value[0] < '0' || value[0] > '9')
		return -1;
	char* end;
	long seconds = strtol(value, &end, 10);
	if (*end != '\0' && *end != ' ')
		return -1;
	return seconds * 1000000LL;
}


void
FetchFileJob::RequestCompleted(BUrlRequest* request, bool success)
{
	fError = request->Status();

	const BHttpResult* httpResult = dynamic_cast<const BHttpResult*>
		(&request->Result());
	if (httpResult != NULL)
		fStatusCode = httpResult->StatusCode();

	if (!success) {
		fRetry = !fStopped && is_transport_error(fError);
		return;
	}

	if (httpResult != NULL) {
		uint16 code = httpResult->StatusCode();

		// By the first digit: StatusCodeClass() knows only the codes of
		// its enum (up to 417 and 504) and calls 429, 451 or 507
		// "invalid", which made such an error page the downloaded file.
		if (code >= 400 && code < 600)
			fError = B_IO_ERROR;
		else
			fError = B_OK;
		switch (code) {
			case B_HTTP_STATUS_OK:
			case B_HTTP_STATUS_PARTIAL_CONTENT:
				fError = B_OK;
				break;
			case B_HTTP_STATUS_REQUEST_TIMEOUT:
			case B_HTTP_STATUS_GATEWAY_TIMEOUT:
				fError = B_DEV_TIMEOUT;
				fRetry = true;
				break;
			case B_HTTP_STATUS_SERVICE_UNAVAILABLE:
			case 429:
				// Too Many Requests
				fRetry = true;
				break;
			case B_HTTP_STATUS_NOT_IMPLEMENTED:
				fError = B_NOT_SUPPORTED;
				break;
			case B_HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE:
				fError = B_UNKNOWN_MIME_TYPE;
				break;
			case B_HTTP_STATUS_REQUESTED_RANGE_NOT_SATISFIABLE:
				fError = B_RESULT_NOT_REPRESENTABLE; // alias for ERANGE
				// the part we have is not part of this file: start over
				fRetry = fRangeStart > 0;
				break;
			case B_HTTP_STATUS_UNAUTHORIZED:
				fError = B_PERMISSION_DENIED;
				break;
			case B_HTTP_STATUS_FORBIDDEN:
			case B_HTTP_STATUS_METHOD_NOT_ALLOWED:
			case B_HTTP_STATUS_NOT_ACCEPTABLE:
				fError = B_NOT_ALLOWED;
				break;
			case B_HTTP_STATUS_NOT_FOUND:
				fError = B_NAME_NOT_FOUND;
				break;
			case B_HTTP_STATUS_BAD_GATEWAY:
				fError = B_BAD_DATA;
				break;
			default:
				break;
		}
		if (fRetry)
			fRetryAfter = retry_after(*httpResult);
	}
}


void
FetchFileJob::Cleanup(status_t jobResult)
{
	if (jobResult != B_OK)
		fTargetEntry.Remove();
}


status_t
FetchFileJob::Stop()
{
	fStopped = true;
	if (fUrlRequest == NULL)
		return B_OK;
	return fUrlRequest->Stop();
}


#else // HAIKU_TARGET_PLATFORM_HAIKU


FetchFileJob::FetchFileJob(const BContext& context, const BString& title,
	const BString& fileURL, const BEntry& targetEntry)
	:
	inherited(context, title),
	fFileURL(fileURL),
	fTargetEntry(targetEntry),
	fTargetFile(&targetEntry, B_CREATE_FILE | B_WRITE_ONLY),
	fDownloadProgress(0.0)
{
}


FetchFileJob::~FetchFileJob()
{
}


float
FetchFileJob::DownloadProgress() const
{
	return fDownloadProgress;
}


const char*
FetchFileJob::DownloadURL() const
{
	return fFileURL.String();
}


const char*
FetchFileJob::DownloadFileName() const
{
	return fTargetEntry.Name();
}


off_t
FetchFileJob::DownloadBytes() const
{
	return fBytes;
}


off_t
FetchFileJob::DownloadTotalBytes() const
{
	return fTotalBytes;
}


status_t
FetchFileJob::Execute()
{
	return B_UNSUPPORTED;
}


status_t
FetchFileJob::Stop()
{
	return B_UNSUPPORTED;
}


void
FetchFileJob::Cleanup(status_t jobResult)
{
	if (jobResult != B_OK)
		fTargetEntry.Remove();
}


#endif // HAIKU_TARGET_PLATFORM_HAIKU

}	// namespace BPrivate

}	// namespace BPackageKit
