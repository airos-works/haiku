/*
 * Copyright 2014, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Distributed under the terms of the MIT License.
 */


#include "package_support.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <AutoDeleter.h>
#include <airos_boot_control.h>
#include <boot/vfs.h>
#include <package/PackagesDirectoryDefs.h>


#define TRACE_PACKAGE_SUPPORT
#ifdef TRACE_PACKAGE_SUPPORT
#	define TRACE(...) dprintf(__VA_ARGS__)
#else
#	define TRACE(...) do {} while (false)
#endif

static const char* const kAdministrativeDirectory
	= PACKAGES_DIRECTORY_ADMIN_DIRECTORY;
static const char* const kActivatedPackagesFile
	= PACKAGES_DIRECTORY_ACTIVATION_FILE;


static inline bool
is_system_package(const char* name)
{
	// The name must end with ".hpkg".
	size_t nameLength = strlen(name);
	if (nameLength < 6 || strcmp(name + nameLength - 5, ".hpkg") != 0)
		return false;

	// The name must either be "haiku.hpkg" or start with "haiku-".
	return strcmp(name, "haiku.hpkg") == 0 || strncmp(name, "haiku-", 6) == 0;
}


static inline bool
is_release_package(const char* name)
{
	// air/OS: "airos_release-<version>-<revision>-<arch>.hpkg"
	return strncmp(name, "airos_release-", 14) == 0;
}


/*!	Copies the version part of a package file name ("<name>-<version>-...")
	into \a version.
*/
static void
get_package_file_version(const char* path, char* version, size_t versionSize)
{
	const char* name = strrchr(path, '/');
	name = name != NULL ? name + 1 : path;
	const char* start = strchr(name, '-');
	if (start == NULL) {
		version[0] = '\0';
		return;
	}
	start++;
	const char* end = strchr(start, '-');
	size_t length = end != NULL ? (size_t)(end - start) : strlen(start);
	if (length >= versionSize)
		length = versionSize - 1;
	memcpy(version, start, length);
	version[length] = '\0';
}


// #pragma mark - PackageVolumeState


PackageVolumeState::PackageVolumeState()
	:
	fName(NULL),
	fDisplayName(NULL),
	fSystemPackage(NULL),
	fReleaseVersion(NULL)
{
}


PackageVolumeState::~PackageVolumeState()
{
	Unset();
}


status_t
PackageVolumeState::SetTo(const char* stateName)
{
	Unset();

	if (stateName != NULL) {
		fName = strdup(stateName);
		if (fName == NULL)
			return B_NO_MEMORY;

		// Derive the display name from the directory name: Chop off the leading
		// "state_" and replace underscores by spaces.
		fDisplayName = strncmp(stateName, "state_", 6) == 0
			? strdup(stateName + 6) : strdup(stateName);
		if (fDisplayName == NULL)
			return B_NO_MEMORY;

		char* remainder = fDisplayName;
		while (char* underscore = strchr(remainder, '_')) {
			*underscore = ' ';
			remainder = underscore + 1;
		}
	}

	return B_OK;
}


void
PackageVolumeState::Unset()
{
	free(fName);
	fName = NULL;

	free(fDisplayName);
	fDisplayName = NULL;

	free(fSystemPackage);
	fSystemPackage = NULL;

	free(fReleaseVersion);
	fReleaseVersion = NULL;
}


const char*
PackageVolumeState::DisplayName() const
{
	if (fDisplayName == NULL)
		return fName == NULL ? "Latest state (cannot start)" : fName;
	return fDisplayName;
}


status_t
PackageVolumeState::SetSystemPackage(const char* package)
{
	if (fSystemPackage != NULL)
		free(fSystemPackage);

	fSystemPackage = strdup(package);
	if (fSystemPackage == NULL)
		return B_NO_MEMORY;

	if (fName == NULL) {
		free(fDisplayName);
		fDisplayName = NULL;

		const char* packageVersion = strchr(package, '-');
		if (packageVersion == NULL) {
			fDisplayName = strdup("Latest state");
		} else {
			ArrayDeleter<char> newDisplayName(new(std::nothrow) char[B_FILE_NAME_LENGTH]);
			if (!newDisplayName.IsSet())
				return B_NO_MEMORY;

			packageVersion++;
			const char* packageVersionEnd = strchr(packageVersion, '-');
			int length = -1;
			if (packageVersionEnd != NULL)
				length = packageVersionEnd - packageVersion;

			snprintf(newDisplayName.Get(), B_FILE_NAME_LENGTH,
				"Latest state (%.*s)", length, packageVersion);
			fDisplayName = newDisplayName.Detach();
		}
	}

	_UpdateReleaseDisplayName();
	return B_OK;
}


status_t
PackageVolumeState::SetReleasePackage(const char* package)
{
	char version[B_FILE_NAME_LENGTH];
	get_package_file_version(package, version, sizeof(version));
	if (version[0] == '\0')
		return B_BAD_VALUE;

	free(fReleaseVersion);
	fReleaseVersion = strdup(version);
	if (fReleaseVersion == NULL)
		return B_NO_MEMORY;

	_UpdateReleaseDisplayName();
	return B_OK;
}


/*!	air/OS: names a state after its release, "air/OS 2026-10-26 09:15
	(hrev60207+1930)", once both its release and its system package are known.
*/
void
PackageVolumeState::_UpdateReleaseDisplayName()
{
	if (fReleaseVersion == NULL || fSystemPackage == NULL)
		return;

	// the release version is the build time, "20261026.0915"
	char when[32];
	const char* version = fReleaseVersion;
	if (strlen(version) == 13 && version[8] == '.') {
		snprintf(when, sizeof(when), "%.4s-%.2s-%.2s %.2s:%.2s", version,
			version + 4, version + 6, version + 9, version + 11);
	} else
		strlcpy(when, version, sizeof(when));

	// the Haiku revision from the system package's version,
	// "r1~beta5_hrev60207_1930"
	char systemVersion[B_FILE_NAME_LENGTH];
	get_package_file_version(fSystemPackage, systemVersion,
		sizeof(systemVersion));
	// (the boot loader's libc has no strstr())
	char* revision = NULL;
	for (char* c = systemVersion; *c != '\0'; c++) {
		if (strncmp(c, "hrev", 4) == 0) {
			revision = c;
			break;
		}
	}
	if (revision != NULL) {
		for (char* c = revision; *c != '\0'; c++) {
			if (*c == '_')
				*c = '+';
		}
	}

	char displayName[B_FILE_NAME_LENGTH];
	snprintf(displayName, sizeof(displayName), "air/OS %s (%s%s%s)", when,
		fName == NULL ? "latest" : "",
		fName == NULL && revision != NULL ? ", " : "",
		revision != NULL ? revision : "");
	if (fName != NULL && revision == NULL)
		snprintf(displayName, sizeof(displayName), "air/OS %s", when);

	char* newDisplayName = strdup(displayName);
	if (newDisplayName == NULL)
		return;
	free(fDisplayName);
	fDisplayName = newDisplayName;
}


void
PackageVolumeState::GetPackagePath(const char* name, char* path,
	size_t pathSize)
{
	if (fName == NULL) {
		// the current state -- packages are directly in the packages directory
		strlcpy(path, name, pathSize);
	} else {
		// an old state
		snprintf(path, pathSize, "%s/%s/%s", kAdministrativeDirectory, fName,
			name);
	}
}


/*static*/ bool
PackageVolumeState::IsNewer(const PackageVolumeState* a,
	const PackageVolumeState* b)
{
	if (b->fName == NULL)
		return false;
	if (a->fName == NULL)
		return true;
	return strcmp(a->fName, b->fName) > 0;
}


// #pragma mark - PackageVolumeInfo


PackageVolumeInfo::PackageVolumeInfo()
	:
	BReferenceable(),
	fStates(),
	fPackagesDir(NULL),
	fOldStatesLoaded(false)
{
}


PackageVolumeInfo::~PackageVolumeInfo()
{
	while (PackageVolumeState* state = fStates.RemoveHead())
		delete state;

	if (fPackagesDir != NULL)
		closedir(fPackagesDir);
}


status_t
PackageVolumeInfo::SetTo(Directory* baseDirectory, const char* packagesPath)
{
	TRACE("PackageVolumeInfo::SetTo()\n");

	if (fPackagesDir != NULL)
		closedir(fPackagesDir);

	// get the packages directory
	fPackagesDir = open_directory(baseDirectory, packagesPath);
	if (fPackagesDir == NULL) {
		TRACE("PackageVolumeInfo::SetTo(): failed to open packages directory: "
			"%s\n", strerror(errno));
		return errno;
	}

	Directory* packagesDirectory = directory_from(fPackagesDir);
	packagesDirectory->Acquire();

	// add the current state
	PackageVolumeState* state = _AddState(NULL);
	if (state == NULL)
		return B_NO_MEMORY;
	status_t error = _InitState(packagesDirectory, fPackagesDir, state);
	if (error != B_OK) {
		TRACE("PackageVolumeInfo::SetTo(): failed to init current state: "
			"%s\n", strerror(error));

		// air/OS: a package transaction that was cut short can leave the
		// latest state without its system package, while the old state it
		// made is complete. Keep the volume if an old state can start.
		LoadOldStates();
		if (NewestBootableOldState() == NULL)
			return error;
		dprintf("PackageVolumeInfo::SetTo(): the latest state cannot start, "
			"old states can\n");
	}

	return B_OK;
}


status_t
PackageVolumeInfo::LoadOldStates()
{
	if (fPackagesDir == NULL) {
		TRACE("PackageVolumeInfo::LoadOldStates(): package directory is NULL");
		return B_ERROR;
	}

	if (fOldStatesLoaded)
		return B_OK;
	fOldStatesLoaded = true;

	Directory* packagesDirectory = directory_from(fPackagesDir);
	packagesDirectory->Acquire();

	if (DIR* administrativeDir = open_directory(packagesDirectory,
			kAdministrativeDirectory)) {
		while (dirent* entry = readdir(administrativeDir)) {
			if (strncmp(entry->d_name, "state_", 6) == 0) {
				TRACE("  old state directory \"%s\"\n", entry->d_name);
				_AddState(entry->d_name);
			}
		}

		closedir(administrativeDir);

		fStates.Sort(&PackageVolumeState::IsNewer);

		// initialize the old states
		PackageVolumeState* state = fStates.Head();
		status_t error;
		for (state = fStates.GetNext(state); state != NULL;) {
			PackageVolumeState* nextState = fStates.GetNext(state);
			if (state->Name() != NULL) {
				error = _InitState(packagesDirectory, fPackagesDir, state);
				if (error != B_OK) {
					TRACE("PackageVolumeInfo::LoadOldStates(): failed to "
						"init state \"%s\": %s\n", state->Name(),
						strerror(error));
					fStates.Remove(state);
					delete state;
				}
			}
			state = nextState;
		}
	} else {
		TRACE("PackageVolumeInfo::LoadOldStates(): failed to open "
			"administrative directory: %s\n", strerror(errno));
	}

	return B_OK;
}


PackageVolumeState*
PackageVolumeInfo::_AddState(const char* stateName)
{
	ObjectDeleter<PackageVolumeState> state(new(std::nothrow) PackageVolumeState);
	if (!state.IsSet())
		return NULL;

	if (state->SetTo(stateName) != B_OK) {
		return NULL;
	}

	fStates.Add(state.Get());
	return state.Detach();
}


status_t
PackageVolumeInfo::_InitState(Directory* packagesDirectory, DIR* dir,
	PackageVolumeState* state)
{
	// find the system package
	ArrayDeleter<char> systemPackageName(new(std::nothrow) char[B_FILE_NAME_LENGTH]);
	if (!systemPackageName.IsSet())
		return B_NO_MEMORY;
	ArrayDeleter<char> packagePath(new(std::nothrow) char[B_PATH_NAME_LENGTH]);
	if (!packagePath.IsSet())
		return B_NO_MEMORY;

	char releasePackageName[B_FILE_NAME_LENGTH];
	releasePackageName[0] = '\0';
	status_t error = _ParseActivatedPackagesFile(packagesDirectory, state,
		systemPackageName.Get(), B_FILE_NAME_LENGTH, releasePackageName,
		sizeof(releasePackageName));
	if (error == B_OK) {
		if (releasePackageName[0] != '\0')
			state->SetReleasePackage(releasePackageName);

		// check, if package exists
		for (PackageVolumeState* otherState = state; otherState != NULL;
				otherState = fStates.GetPrevious(otherState)) {
			otherState->GetPackagePath(systemPackageName.Get(), packagePath.Get(),
				B_PATH_NAME_LENGTH);
			struct stat st;
			if (get_stat(packagesDirectory, packagePath.Get(), st) == B_OK
					&& S_ISREG(st.st_mode)) {
				state->SetSystemPackage(packagePath.Get());
				break;
			}
		}
	} else {
		TRACE("PackageVolumeInfo::_InitState(): failed to parse "
			"activated-packages: %s\n", strerror(error));

		// No or invalid activated-packages file. That is OK for the current
		// state. We'll iterate through the packages directory to find the
		// system package. We don't do that for old states, though.
		if (state->Name() != NULL)
			return B_ENTRY_NOT_FOUND;

		while (dirent* entry = readdir(dir)) {
			// The name must end with ".hpkg".
			if (is_system_package(entry->d_name)) {
				state->SetSystemPackage(entry->d_name);
				break;
			}
		}
	}

	if (state->SystemPackage() == NULL)
		return B_ENTRY_NOT_FOUND;

	return B_OK;
}


PackageVolumeState*
PackageVolumeInfo::FindState(const char* name) const
{
	for (PackageVolumeStateList::ConstIterator it = fStates.GetIterator();
			PackageVolumeState* state = it.Next();) {
		if (state->Name() != NULL && strcmp(state->Name(), name) == 0)
			return state;
	}
	return NULL;
}


PackageVolumeState*
PackageVolumeInfo::NewestBootableOldState() const
{
	// the list is sorted newest first, the latest state at its head
	for (PackageVolumeStateList::ConstIterator it = fStates.GetIterator();
			PackageVolumeState* state = it.Next();) {
		if (state->Name() != NULL && state->IsBootable())
			return state;
	}
	return NULL;
}


/*!	Reads a file of System Updater's boot control directory
	(airos_boot_control.h). Returns its length, or an error when there is none.
*/
ssize_t
PackageVolumeInfo::ReadBootControlFile(const char* name, char* buffer,
	size_t bufferSize)
{
	if (fPackagesDir == NULL)
		return B_NO_INIT;

	char path[B_PATH_NAME_LENGTH];
	snprintf(path, sizeof(path), "%s/%s", AIROS_BOOT_CONTROL_DIRECTORY, name);
	FileDescriptorCloser fd(open_from(directory_from(fPackagesDir), path,
		O_RDONLY));
	if (!fd.IsSet())
		return fd.Get();

	ssize_t length = read(fd.Get(), buffer, bufferSize);
	if (length >= 0 && (size_t)length < bufferSize)
		buffer[length] = '\0';
	return length;
}


status_t
PackageVolumeInfo::_ParseActivatedPackagesFile(Directory* packagesDirectory,
	PackageVolumeState* state, char* packageName, size_t packageNameSize,
	char* releaseName, size_t releaseNameSize)
{
	// open the activated-packages file
	static const size_t kBufferSize = 3 * B_FILE_NAME_LENGTH + 2;
	ArrayDeleter<char> path(new(std::nothrow) char[kBufferSize]);
	if (!path.IsSet())
		return B_NO_MEMORY;
	snprintf(path.Get(), kBufferSize, "%s/%s/%s",
		kAdministrativeDirectory, state->Name() != NULL ? state->Name() : "",
		kActivatedPackagesFile);
	FileDescriptorCloser fd(open_from(packagesDirectory, path.Get(), O_RDONLY));
	if (!fd.IsSet())
		return fd.Get();

	struct stat st;
	if (fstat(fd.Get(), &st) != 0)
		return errno;
	if (!S_ISREG(st.st_mode))
		return B_ENTRY_NOT_FOUND;

	// Read the file for the system package line and (air/OS) the release
	// package line.
	status_t result = B_ENTRY_NOT_FOUND;
	releaseName[0] = '\0';
	size_t remainingBytes = 0;
	for (;;) {
		ssize_t bytesRead = read(fd.Get(), path.Get() + remainingBytes,
			kBufferSize - remainingBytes - 1);
		if (bytesRead <= 0) {
			// the last line may lack its line feed
			if (remainingBytes > 0) {
				path[remainingBytes] = '\0';
				char* line = path.Get();
				if (result != B_OK && is_system_package(line)) {
					result = strlcpy(packageName, line, packageNameSize)
							< packageNameSize ? B_OK : B_NAME_TOO_LONG;
				} else if (is_release_package(line))
					strlcpy(releaseName, line, releaseNameSize);
			}
			return result;
		}

		remainingBytes += bytesRead;
		path[remainingBytes] = '\0';

		char* line = path.Get();
		while (char* lineEnd = strchr(line, '\n')) {
			*lineEnd = '\0';
			if (result != B_OK && is_system_package(line)) {
				result = strlcpy(packageName, line, packageNameSize)
						< packageNameSize ? B_OK : B_NAME_TOO_LONG;
				if (result != B_OK)
					return result;
			} else if (is_release_package(line))
				strlcpy(releaseName, line, releaseNameSize);

			if (result == B_OK && releaseName[0] != '\0')
				return B_OK;

			line = lineEnd + 1;
		}

		// move the remainder to the start of the buffer
		if (line < path.Get() + remainingBytes) {
			size_t left = path.Get() + remainingBytes - line;
			memmove(path.Get(), line, left);
			remainingBytes = left;
		} else
			remainingBytes = 0;
	}

	return result;
}
