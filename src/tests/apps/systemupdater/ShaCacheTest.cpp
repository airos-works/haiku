/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


/*!	The System Updater's cache of the installed packages' SHA-256
	(airos-works/haiku#203). The cache file got a line for every package
	file ever seen and never lost one, it was read whole once per package,
	and beyond 4 MiB it was not read at all anymore: every check then hashed
	all installed packages again, and appended their lines once more.

	This runs checks ("plans") over a directory of fake packages that get
	updated like a system, and over cache files as older versions left them.
	It counts the packages that had to be hashed, and the lines of the file.
*/


#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>
#include <String.h>

#include "ShaCache.h"


static BString sDirectory;
static BString sPackages;
static BString sCacheFile;
static int sFailures = 0;


static void
report(bool ok, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("%s ", ok ? "ok  " : "FAIL");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	if (!ok)
		sFailures++;
}


/*!	Stands for the SHA-256 of a package file: 64 hex digits from its name
	and size.
*/
static BString
fake_sha(const char* fileName)
{
	BString path(sPackages);
	path << '/' << fileName;
	struct stat st;
	if (stat(path, &st) != 0)
		st.st_size = -1;

	uint64 hash = 1469598103934665603ULL;
	for (const char* c = fileName; *c != '\0'; c++)
		hash = (hash ^ (uint8)*c) * 1099511628211ULL;
	hash ^= st.st_size;

	BString sha;
	for (int i = 0; i < 4; i++) {
		BString part;
		part.SetToFormat("%016" B_PRIx64, hash);
		sha << part;
		hash = hash * 6364136223846793005ULL + 1442695040888963407ULL;
	}
	return sha;
}


static void
write_package(const char* name, int version, size_t size = 100)
{
	BString path;
	path.SetToFormat("%s/%s-%d-x86_64.hpkg", sPackages.String(), name,
		version);
	FILE* file = fopen(path, "w");
	if (file == NULL)
		return;
	for (size_t i = 0; i < size; i++)
		fputc('p', file);
	fclose(file);
}


static void
remove_package(const char* name, int version)
{
	BString path;
	path.SetToFormat("%s/%s-%d-x86_64.hpkg", sPackages.String(), name,
		version);
	unlink(path);
}


/*!	A check for updates: the hash of every installed package is wanted.
	Returns how many had to be computed, -1 when the cache gave a wrong one.
*/
static int32
plan(int32* _dropped = NULL)
{
	ShaCache cache(sCacheFile, sPackages);
	cache.Load();
	if (_dropped != NULL)
		*_dropped = cache.CountDropped();

	int32 computed = 0;
	bool wrong = false;
	DIR* directory = opendir(sPackages);
	while (struct dirent* entry = readdir(directory)) {
		if (entry->d_name[0] == '.')
			continue;
		BString sha;
		if (cache.Lookup(entry->d_name, sha)) {
			if (sha != fake_sha(entry->d_name))
				wrong = true;
			continue;
		}
		computed++;
		cache.Add(entry->d_name, fake_sha(entry->d_name));
	}
	closedir(directory);
	cache.Flush();
	return wrong ? -1 : computed;
}


static void
cache_file(int32& _lines, off_t& _size)
{
	_lines = 0;
	_size = 0;
	FILE* file = fopen(sCacheFile, "r");
	if (file == NULL)
		return;
	int c;
	while ((c = fgetc(file)) != EOF) {
		_size++;
		if (c == '\n')
			_lines++;
	}
	fclose(file);
}


/*!	Appends lines of packages that are not installed anymore, as every
	update left them behind.
*/
static void
append_stale_lines(int32 count)
{
	FILE* file = fopen(sCacheFile, "a");
	for (int32 i = 0; i < count; i++) {
		fprintf(file, "%064d %d 1048576 1700000000 "
			"some_package_of_an_older_release-%d-x86_64.hpkg\n", (int)i,
			(int)(100000 + i), (int)i);
	}
	fclose(file);
}


int
main()
{
	sDirectory.SetToFormat("/tmp/sha-cache-test-%d", (int)getpid());
	sPackages = BString(sDirectory) << "/packages";
	sCacheFile = BString(sDirectory) << "/sha256-cache";
	mkdir(sDirectory, 0755);
	mkdir(sPackages, 0755);

	const int kPackages = 200;
	int32 lines;
	off_t size;

	// a new system
	for (int i = 0; i < kPackages; i++)
		write_package(BString().SetToFormat("package%03d", i), 1);
	int32 computed = plan();
	cache_file(lines, size);
	report(computed == kPackages && lines == kPackages, "first check of %d "
		"packages: %" B_PRId32 " hashed, the cache has %" B_PRId32 " lines",
		kPackages, computed, lines);

	computed = plan();
	cache_file(lines, size);
	report(computed == 0 && lines == kPackages, "second check: %" B_PRId32
		" hashed, %" B_PRId32 " lines", computed, lines);

	// 100 updates that replace 30 packages each (new file, new node)
	int32 maxLines = 0;
	int32 hashed = 0;
	bool onlyNew = true;
	for (int update = 2; update <= 101; update++) {
		for (int i = 0; i < 30; i++) {
			int package = (update * 7 + i) % kPackages;
			BString name;
			name.SetToFormat("package%03d", package);
			// whatever version it has now
			for (int version = 1; version < update; version++)
				remove_package(name, version);
			write_package(name, update);
		}
		computed = plan();
		if (computed != 30)
			onlyNew = false;
		hashed += computed;
		computed = plan();
		if (computed != 0)
			onlyNew = false;
		cache_file(lines, size);
		if (lines > maxLines)
			maxLines = lines;
	}
	report(onlyNew && maxLines <= kPackages + 30, "100 updates of 30 "
		"packages, a check after each: %" B_PRId32 " hashed (3000 changed), "
		"the cache never had more than %" B_PRId32 " lines, %" B_PRId32
		" (%" B_PRIdOFF " bytes) at the end", hashed, maxLines, lines, size);

	// a package changed in place: same file (name and node), other size
	if (DIR* directory = opendir(sPackages)) {
		while (struct dirent* entry = readdir(directory)) {
			if (strncmp(entry->d_name, "package000-", 11) != 0)
				continue;
			BString path(sPackages);
			path << '/' << entry->d_name;
			if (FILE* file = fopen(path, "w")) {
				for (int i = 0; i < 4096; i++)
					fputc('q', file);
				fclose(file);
			}
			break;
		}
		closedir(directory);
	}
	computed = plan();
	cache_file(lines, size);
	report(computed == 1 && lines == kPackages, "a package rewritten in "
		"place: %" B_PRId32 " hashed, its old line is gone (%" B_PRId32
		" lines)", computed, lines);
	computed = plan();
	report(computed == 0, "the check after it: %" B_PRId32 " hashed",
		computed);

	// The file of a system that has been updated for long: its lines of
	// today's packages, behind 60000 lines of packages that are gone. Beyond
	// 4 MiB the file was not read anymore.
	BString valid;
	{
		FILE* file = fopen(sCacheFile, "r");
		char buffer[512];
		while (fgets(buffer, sizeof(buffer), file) != NULL)
			valid << buffer;
		fclose(file);
		unlink(sCacheFile);
	}
	append_stale_lines(60000);
	{
		FILE* file = fopen(sCacheFile, "a");
		fputs(valid, file);
		fclose(file);
	}
	off_t oldSize;
	int32 oldLines;
	cache_file(oldLines, oldSize);
	int32 dropped;
	bigtime_t start = system_time();
	computed = plan(&dropped);
	bigtime_t took = system_time() - start;
	cache_file(lines, size);
	report(computed == 0 && lines == kPackages && dropped == 60000,
		"a cache file of %.1f MiB (%" B_PRId32 " lines, 60000 of old "
		"packages): %" B_PRId32 " hashed, the file now has %" B_PRId32
		" lines (%" B_PRIdOFF " bytes); the check took %.0f ms",
		oldSize / 1048576.0, oldLines, computed, lines, size, took / 1000.0);

	// a file that is no cache: binary, without line breaks
	{
		FILE* file = fopen(sCacheFile, "w");
		for (int i = 0; i < 300000; i++)
			fputc(i % 251, file);
		fclose(file);
	}
	computed = plan();
	int32 again = plan();
	cache_file(lines, size);
	report(computed == kPackages && again == 0 && lines == kPackages,
		"a file of garbage: %" B_PRId32 " hashed, then %" B_PRId32 ", the "
		"file has %" B_PRId32 " lines", computed, again, lines);

	// a line cut off (a write that did not finish), and an empty file
	{
		FILE* file = fopen(sCacheFile, "a");
		fputs("0123456789abcdef0123456789abcdef"
			"0123456789abcdef0123456789abcdef 12 34", file);
		fclose(file);
	}
	computed = plan();
	cache_file(lines, size);
	report(computed == 0 && lines == kPackages, "a last line cut short: %"
		B_PRId32 " hashed, %" B_PRId32 " lines", computed, lines);

	truncate(sCacheFile, 0);
	computed = plan();
	cache_file(lines, size);
	report(computed == kPackages && lines == kPackages, "an empty file: %"
		B_PRId32 " hashed, %" B_PRId32 " lines", computed, lines);

	// no file with the name of the temporary one stays
	BString temporary(sCacheFile);
	temporary << ".new";
	struct stat st;
	report(stat(temporary, &st) != 0, "no temporary file is left");

	BString command;
	command.SetToFormat("rm -rf %s", sDirectory.String());
	system(command);

	printf("SystemUpdaterShaCacheTest: %s\n",
		sFailures == 0 ? "PASSED" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
