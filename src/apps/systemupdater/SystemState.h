/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SYSTEM_STATE_H
#define SYSTEM_STATE_H


#include <ObjectList.h>
#include <Path.h>
#include <String.h>
#include <StringList.h>


// One package state of the system volume: the latest one in
// system/packages, or an old one in administrative/state_<date>.
struct InstalledVersion {
	BString		state;
		// directory name, empty for the latest state
	BString		releaseVersion;
		// of its airos_release package, "20261026.0915"
	BString		releaseFile;
	BString		systemPackage;
		// file name of its haiku package
	BStringList	packages;
		// its activated-packages
	bool		latest;
	bool		running;
	bool		bootable;

	BString		Title() const;
		// "air/OS 26 October 2026, 09:15"
	BString		Revision() const;
		// "hrev60207+1930"
	BString		StateTitle() const;
		// "26 Oct 2026 09:20:11" from the state's name
	bool		SameRelease(const InstalledVersion& other) const;
};


// System Updater's boot control files (airos_boot_control.h).
struct TrialInfo {
	bool		exists;
	BString		release;
	BString		fallback;
	int32		allowed;
	int32		attempts;
};


struct LastBoot {
	BString		mode;
		// AIROS_BOOT_MODE_*, empty for an ordinary start
	BString		state;
};


class SystemState {
public:
								SystemState();

			status_t			Load();

			int32				CountVersions() const
									{ return fVersions.CountItems(); }
			const InstalledVersion* VersionAt(int32 index) const
									{ return fVersions.ItemAt(index); }
			const InstalledVersion* Latest() const;
			const InstalledVersion* Running() const;
			const InstalledVersion* FindState(const char* state) const;
			const InstalledVersion* NewestOldState() const;

			bool				RunningLatest() const
									{ return fRunningState.IsEmpty(); }
			const BString&		RunningState() const
									{ return fRunningState; }
			bool				RestartPending() const
									{ return fRestartPending; }
									// a deferred transaction waits for the
									// next start
			int64				ChangeCount() const
									{ return fChangeCount; }

			status_t			FindPackageFile(const InstalledVersion& version,
									const char* fileName, BPath& _path) const;
									// where a package of that state lives

	static	status_t			ReadActivationFile(const char* path,
									BStringList& _packages);

	// boot control
	static	status_t			ReadTrial(TrialInfo& _trial);
	static	status_t			StartTrial(const char* release,
									const char* fallback, int32 allowed);
	static	status_t			ResetTrialAttempts();
	static	status_t			EndTrial();
	static	status_t			RequestNextBoot(const char* state);
									// NULL or "" for the latest state
	static	status_t			CancelNextBoot();
	static	BString				NextBoot();
	static	bool				HasNextBoot();
	static	LastBoot			ReadLastBoot();
	static	bool				IsFailedRelease(const char* version);
	static	status_t			AddFailedRelease(const char* version);

	static	status_t			WriteFileAtomically(const char* path,
									const BString& content);
	static	status_t			EnsureDirectory(const char* path);

private:
			status_t			_AddVersion(const char* state,
									const char* activationFile);

private:
			BObjectList<InstalledVersion, true> fVersions;
				// newest first; the latest state first
			BString				fRunningState;
			bool				fRestartPending;
			int64				fChangeCount;
};


#endif	// SYSTEM_STATE_H
