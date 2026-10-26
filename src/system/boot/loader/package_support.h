/*
 * Copyright 2014, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef PACKAGE_SUPPORT_H
#define PACKAGE_SUPPORT_H


#include <dirent.h>

#include <Referenceable.h>
#include <util/DoublyLinkedList.h>


class Directory;


class PackageVolumeState : public DoublyLinkedListLinkImpl<PackageVolumeState> {
public:
								PackageVolumeState();
								~PackageVolumeState();

			status_t			SetTo(const char* stateName);
			void				Unset();

			const char*			Name() const
									{ return fName; }
			const char*			DisplayName() const;

			const char*			SystemPackage() const
									{ return fSystemPackage; }
			status_t			SetSystemPackage(const char* package);
			bool				IsBootable() const
									{ return fSystemPackage != NULL; }

			const char*			ReleaseVersion() const
									{ return fReleaseVersion; }
			status_t			SetReleasePackage(const char* package);
									// air/OS: the state's airos_release
									// package, which names its release

			void				GetPackagePath(const char* name, char* path,
									size_t pathSize);

	static	bool				IsNewer(const PackageVolumeState* a,
									const PackageVolumeState* b);

private:
			void				_UpdateReleaseDisplayName();

private:
			char*				fName;
			char*				fDisplayName;
			char*				fSystemPackage;
			char*				fReleaseVersion;
};

typedef DoublyLinkedList<PackageVolumeState> PackageVolumeStateList;


class PackageVolumeInfo : public BReferenceable {
public:
			typedef PackageVolumeStateList StateList;

public:
								PackageVolumeInfo();
								~PackageVolumeInfo();

			status_t			SetTo(Directory* baseDirectory,
									const char* packagesPath);

			status_t			LoadOldStates();

			const StateList&	States() const
									{ return fStates; }

			// air/OS System Updater (airos_boot_control.h)
			PackageVolumeState*	FindState(const char* name) const;
			PackageVolumeState*	NewestBootableOldState() const;
			ssize_t				ReadBootControlFile(const char* name,
									char* buffer, size_t bufferSize);

private:
			PackageVolumeState*	_AddState(const char* stateName);
			status_t			_InitState(Directory* packagesDirectory,
									DIR* dir, PackageVolumeState* state);
			status_t			_ParseActivatedPackagesFile(
									Directory* packagesDirectory,
									PackageVolumeState* state,
									char* packageName, size_t packageNameSize,
									char* releaseName, size_t releaseNameSize);

private:
			StateList			fStates;
			DIR*				fPackagesDir;
			bool				fOldStatesLoaded;
};


#endif	// PACKAGE_SUPPORT_H
