/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef FEED_H
#define FEED_H


#include <Message.h>
#include <ObjectList.h>
#include <String.h>


class BDataIO;


// Progress of a download: bytes so far and in all (-1 when unknown).
class DownloadListener {
public:
	virtual						~DownloadListener();
	virtual	void				DownloadProgress(off_t bytes, off_t total) = 0;
};


// HTTP(S) transfers with the Network Kit, run synchronously in the calling
// thread. Cancel() may be called from any thread.
class Fetcher {
public:
								Fetcher();
								~Fetcher();

			status_t			FetchText(const BString& url, BString& _text,
									BString& _error, size_t maxSize
										= 16 * 1024 * 1024);
			status_t			FetchFile(const BString& url, const char* path,
									off_t expectedSize, BString& _error,
									DownloadListener* listener = NULL);
									// resumes a partial file

			void				Cancel();
			bool				IsCancelled() const;
			void				Reset()
									{ fCancelled = 0; }
			time_t				ServerTime() const
									{ return fServerTime; }
									// from the last answer's Date header

private:
			void				_Pause(int32 attempt);
			status_t			_Fetch(const BString& url, BDataIO* output,
									off_t rangeStart, off_t outputSize,
									BString& _error,
									DownloadListener* listener);

private:
			class Listener;

			int32				fCancelled;
			void*				fRequest;
			time_t				fServerTime;
				// the running BUrlRequest
};


// A release the feed offers.
struct FeedBuild {
	BString		id;
	BString		built;
	BString		version;
	BString		manifest;
		// address of its update.json, resolved
	BString		manifestSha256;
	off_t		size;
	BString		haikuRevision;
	BMessage	changes;
};


// The feed's index.json: the releases of one target, newest first.
class Feed {
public:
								Feed();

			status_t			Fetch(const BString& baseURL, Fetcher& fetcher,
									BString& _error);

			const BString&		BaseURL() const { return fBaseURL; }
			const BString&		Target() const { return fTarget; }
			int32				CountBuilds() const
									{ return fBuilds.CountItems(); }
			const FeedBuild*	BuildAt(int32 index) const
									{ return fBuilds.ItemAt(index); }
			const FeedBuild*	Latest() const
									{ return fBuilds.ItemAt(0); }
			const FeedBuild*	FindBuild(const char* id) const;

	static	BString				Resolve(const BString& base,
									const BString& relative);

private:
			BString				fBaseURL;
			BString				fTarget;
			BObjectList<FeedBuild, true> fBuilds;
};


#endif	// FEED_H
