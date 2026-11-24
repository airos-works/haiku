/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef SHA_CACHE_H
#define SHA_CACHE_H


#include <map>

#include <String.h>


/*!	The SHA-256 of the installed packages, remembered by node, size and
	modification time, so that a check for updates reads only the packages
	that changed since the last one.

	The file has one line per package, "<sha256> <node> <size> <time> <name>".
	It is read once (Load()), and only the lines of files that are in the
	packages directory now, unchanged, are kept: the file may be of any size.
	A new hash is appended at once, so that an interrupted check keeps what it
	computed. Flush() replaces a file that holds anything else than the kept
	lines, which is what keeps it from growing with every update.
*/
class ShaCache {
public:
								ShaCache(const char* cacheFile,
									const char* packagesDirectory);

			void				Load();
			bool				Lookup(const char* fileName,
									BString& _sha256);
			void				Add(const char* fileName,
									const BString& sha256);
			status_t			Flush();

			int32				CountEntries() const
									{ return (int32)fEntries.size(); }
			int32				CountDropped() const
									{ return fDropped; }

private:
			bool				_KeyFor(const char* fileName, BString& _key);

private:
	typedef std::map<BString, BString> StringMap;

			BString				fCacheFile;
			BString				fPackagesDirectory;
			StringMap			fKeys;
				// file name -> "<node> <size> <time> <name>" as installed
			StringMap			fEntries;
				// key -> SHA-256
			int32				fDropped;
				// lines of the file that are of no use anymore
			bool				fLoaded;
};


#endif	// SHA_CACHE_H
