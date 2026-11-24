/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "ShaCache.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


static const int32 kShaLength = 64;

// A line has some 150 bytes, a system a few hundred packages: a file beyond
// this size is not a cache of this class. It is not read, but replaced.
static const off_t kMaxFileSize = 64 * 1024 * 1024;


ShaCache::ShaCache(const char* cacheFile, const char* packagesDirectory)
	:
	fCacheFile(cacheFile),
	fPackagesDirectory(packagesDirectory),
	fDropped(0),
	fLoaded(false)
{
}


/*!	Reads the cache, once for all lookups that follow. May be called again to
	start over (a new check for updates).
*/
void
ShaCache::Load()
{
	fKeys.clear();
	fEntries.clear();
	fDropped = 0;
	fLoaded = true;

	// what is installed now
	if (DIR* directory = opendir(fPackagesDirectory)) {
		while (struct dirent* entry = readdir(directory)) {
			if (entry->d_name[0] == '.')
				continue;
			BString key;
			_KeyFor(entry->d_name, key);
		}
		closedir(directory);
	}

	// The lines of those files. The others are of packages that have been
	// replaced or removed since (their node, size or time differs); they are
	// only counted, so that it does not matter how many there are.
	FILE* file = fopen(fCacheFile, "r");
	if (file == NULL)
		return;

	struct stat st;
	if (fstat(fileno(file), &st) != 0 || st.st_size > kMaxFileSize) {
		fDropped++;
		fclose(file);
		return;
	}

	char* line = NULL;
	size_t capacity = 0;
	ssize_t length;
	while ((length = getline(&line, &capacity, file)) >= 0) {
		if (length > 0 && line[length - 1] == '\n')
			line[--length] = '\0';
		if (length == 0)
			continue;

		// "<sha256> <node> <size> <time> <name>"
		bool valid = length > kShaLength + 1 && line[kShaLength] == ' '
			&& (ssize_t)strlen(line) == length;
		const char* name = NULL;
		if (valid) {
			name = line + kShaLength + 1;
			for (int32 i = 0; i < 3 && name != NULL; i++) {
				name = strchr(name, ' ');
				if (name != NULL)
					name++;
			}
		}
		if (name == NULL) {
			fDropped++;
			continue;
		}

		const char* key = line + kShaLength + 1;
		StringMap::const_iterator installed = fKeys.find(name);
		if (installed == fKeys.end() || installed->second != key
			|| fEntries.find(key) != fEntries.end()) {
			fDropped++;
			continue;
		}
		fEntries[key] = BString(line, kShaLength);
	}
	if (ferror(file) != 0)
		fDropped++;
	free(line);
	fclose(file);
}


bool
ShaCache::Lookup(const char* fileName, BString& _sha256)
{
	if (!fLoaded)
		Load();

	BString key;
	if (!_KeyFor(fileName, key))
		return false;
	StringMap::const_iterator found = fEntries.find(key);
	if (found == fEntries.end())
		return false;
	_sha256 = found->second;
	return true;
}


void
ShaCache::Add(const char* fileName, const BString& sha256)
{
	if (!fLoaded)
		Load();

	BString key;
	if (sha256.Length() != kShaLength || !_KeyFor(fileName, key))
		return;
	fEntries[key] = sha256;

	// at once: computing it may have taken long, and the check can be
	// interrupted before Flush()
	if (FILE* file = fopen(fCacheFile, "a")) {
		fprintf(file, "%s %s\n", sha256.String(), key.String());
		fclose(file);
	}
}


/*!	Replaces the file by one with the entries of the installed packages only,
	if it holds anything else. The new file is written next to it, synced and
	renamed over it, so that there is a complete file at all times.
*/
status_t
ShaCache::Flush()
{
	if (!fLoaded || fDropped == 0)
		return B_OK;

	BString temporary(fCacheFile);
	temporary << ".new";
	FILE* file = fopen(temporary, "w");
	if (file == NULL)
		return errno;

	status_t error = B_OK;
	for (StringMap::const_iterator it = fEntries.begin();
			it != fEntries.end(); ++it) {
		if (fprintf(file, "%s %s\n", it->second.String(), it->first.String())
				< 0) {
			error = errno;
			break;
		}
	}
	if (error == B_OK && (fflush(file) != 0 || fsync(fileno(file)) != 0))
		error = errno;
	if (fclose(file) != 0 && error == B_OK)
		error = errno;
	if (error == B_OK && rename(temporary, fCacheFile) != 0)
		error = errno;
	if (error != B_OK) {
		unlink(temporary);
		return error;
	}

	fDropped = 0;
	return B_OK;
}


/*!	The key of a file in the packages directory as it is now; \c false if
	there is no such file.
*/
bool
ShaCache::_KeyFor(const char* fileName, BString& _key)
{
	StringMap::const_iterator found = fKeys.find(fileName);
	if (found != fKeys.end()) {
		_key = found->second;
		return true;
	}

	BString path(fPackagesDirectory);
	path << '/' << fileName;
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
		return false;

	_key.SetToFormat("%" B_PRIdINO " %" B_PRIdOFF " %" B_PRIdTIME " %s",
		st.st_ino, st.st_size, st.st_mtime, fileName);
	fKeys[fileName] = _key;
	return true;
}
