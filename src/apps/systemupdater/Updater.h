/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef UPDATER_H
#define UPDATER_H


#include <ObjectList.h>
#include <String.h>
#include <StringList.h>

#include "Feed.h"
#include "Release.h"
#include "SystemState.h"


class UpdaterListener {
public:
	virtual						~UpdaterListener();
	virtual	void				UpdaterStatus(const char* text) = 0;
	virtual	void				UpdaterProgress(float fraction) = 0;
									// 0..1, < 0 when there is none
};


// A package the transaction activates and where it comes from.
struct PlannedPackage {
	BString		file;
		// its name in system/packages
	BString		source;
		// a download (filled in by Download()) or a file of an old state
	BString		sha256;
	BString		url;
	off_t		size;
	bool		download;
	bool		copy;
		// copy the source (a file of an old state) instead of moving it
};


// What installing a release, or going back to an old state, changes.
struct UpdatePlan {
	Release		release;
		// the release installed; for going back, what is known of it
	BString		goBackState;
		// the old state gone back to, if that is the plan
	BObjectList<PlannedPackage, true> activate;
	BStringList	deactivate;
	BStringList	kept;
		// packages of the user's own that stay
	off_t		downloadBytes;
	off_t		copyBytes;
	BStringList	bootSources;
		// the downloaded boot files (Raspberry Pi), as release.BootFiles()

				UpdatePlan();
	bool		IsEmpty() const
					{ return activate.IsEmpty() && deactivate.IsEmpty(); }
};


class Updater {
public:
								Updater(UpdaterListener* listener = NULL);
								~Updater();

			void				SetListener(UpdaterListener* listener)
									{ fListener = listener; }
			void				SetFeedOverride(const char* url)
									{ fFeedOverride = url; }

	// what runs
			status_t			LoadInstalledRelease(Release& _release) const;
			BString				Target() const;
			BString				FeedURL() const;

	// what the feed offers
			status_t			Check(Feed& _feed, BString& _error);
			status_t			FetchRelease(const FeedBuild& build,
									Release& _release, BString& _error);
			bool				IsNewer(const FeedBuild& build) const;

	// installing
			status_t			PlanUpdate(const Release& release,
									const SystemState& state,
									UpdatePlan& _plan, BString& _error,
									const Feed* feed = NULL);
									// the feed offers the record of the
									// installed release if there is none
			status_t			PlanGoBack(const InstalledVersion& version,
									const SystemState& state,
									UpdatePlan& _plan, BString& _error);
			status_t			CheckPreconditions(const SystemState& state,
									BString& _error) const;
			status_t			Download(UpdatePlan& plan, BString& _error);
			status_t			Apply(UpdatePlan& plan, BString& _error);
			void				Cancel();
			bool				IsCancelled() const
									{ return atomic_get(
										(int32*)&fCancelled) != 0; }
			void				Reset();
									// after a cancelled operation

	// after a start
			status_t			BootCheck(BString& _report,
									bool waitForDesktop = true);
			status_t			Confirm(BString& _report);
			status_t			RemoveFailedUpdate(BString& _error);
			status_t			Prune(int32 keep, BString& _report);
			status_t			UpdateEfiLoader(BString& _report);
			status_t			SyncBootFiles(BString& _report);
									// Raspberry Pi: the kernel of the state
									// that starts next
			BString				NextBootState(const SystemState& state)
									const;
			status_t			ApplyPayloadLinks(const Release& release,
									const Release* previous,
									BString& _report);
									// links the files new in the release

	static	void				Log(const char* format, ...);
	static	status_t			ComputeSha256(const char* path,
									BString& _sha256,
									UpdaterListener* listener = NULL,
									int32* cancelled = NULL);
	static	BString				PackageNameOf(const char* fileName);
	static	BString				VersionOf(const char* fileName);
	static	BString				FormatSize(off_t size);

private:
			status_t			_InstalledSha256(const char* fileName,
									BString& _sha256);
			status_t			_LoadReleaseRecord(const char* id,
									Release& _release) const;
			status_t			_LoadReleaseRecordForVersion(
									const BString& version,
									Release& _release) const;
			status_t			_SaveReleaseRecord(const Release& release);
			void				_AddRunningPayloadLinks(const char* path,
									const char* relative, BString& _json,
									int32& _count);
			status_t			_VerifyPackage(const char* path,
									const ReleasePackage& expected,
									BString& _error);
			void				_Status(const char* format, ...);
			void				_Progress(float fraction);
			status_t			_RemoveDirectory(const char* path);
			bool				_WaitForDesktop();
			void				_CheckClock();
			void				_CheckClockForSecureFeed(const BString& url);

private:
			UpdaterListener*	fListener;
			BString				fFeedOverride;
			Fetcher				fFetcher;
			int32				fCancelled;
};


#endif	// UPDATER_H
