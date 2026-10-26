/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef RELEASE_H
#define RELEASE_H


#include <Message.h>
#include <ObjectList.h>
#include <String.h>


// Accessors for what BJson makes of JSON: objects are messages, arrays are
// messages with the fields "0", "1", ..., numbers are doubles.
namespace json {
	BString		String(const BMessage& object, const char* name,
					const char* defaultValue = "");
	double		Number(const BMessage& object, const char* name,
					double defaultValue = 0);
	bool		Object(const BMessage& object, const char* name,
					BMessage& _value);
	int32		Count(const BMessage& array);
	bool		At(const BMessage& array, int32 index, BMessage& _value);
	BString		StringAt(const BMessage& array, int32 index);
	status_t	ParseFile(const char* path, BMessage& _object);
	status_t	Parse(const BString& text, BMessage& _object);
	BString		Quote(const char* text);
}


// A package of a release, as update.json lists it.
struct ReleasePackage {
	BString		name;
	BString		version;
	BString		file;
	BString		sha256;
	BString		url;
	off_t		size;
	bool		optional;
		// one the Installer offers (_packages_): updated where installed
};


// A file the release needs outside the system volume: the Raspberry Pi's
// loader and boot archive, which its firmware loads from the FAT partition.
struct BootFile {
	BString		name;
	BString		sha256;
	BString		url;
	off_t		size;
};


// An entry of system/non-packaged that links into the airos_payload package
// (kPayloadDirectory): a file or directory symlinked there, or a symlink of
// the payload itself recreated as it is.
struct PayloadLink {
	BString		path;
		// relative to kNonPackagedDirectory
	BString		type;
		// "file", "dir" or "symlink"
	BString		target;
		// the symlink's own target for "symlink"
};


// One release: an image build of one target, what installs it, and what
// changed since the release before it.
class Release {
public:
								Release();

			status_t			SetTo(const BMessage& object);
			status_t			Load(const char* path);
									// update.json, or the release file the
									// airos_release package installs
			bool				IsValid() const
									{ return !fId.IsEmpty(); }

			const BString&		Id() const { return fId; }
			const BString&		Target() const { return fTarget; }
			const BString&		Architecture() const { return fArchitecture; }
			const BString&		Built() const { return fBuilt; }
			const BString&		Version() const { return fVersion; }
			const BString&		HaikuRevision() const
									{ return fHaikuRevision; }
			const BString&		HaikuCommit() const { return fHaikuCommit; }
			const BString&		Feed() const { return fFeed; }

			BString				Title() const;
									// "air/OS 26 October 2026, 09:15"
			BString				ShortRevision() const;
									// "hrev60207+1930"

			const BObjectList<ReleasePackage, true>& Packages() const
									{ return fPackages; }
			const ReleasePackage* FindPackage(const char* name) const;
			const BObjectList<PayloadLink, true>& PayloadLinks() const
									{ return fPayloadLinks; }
			const BObjectList<BootFile, true>& BootFiles() const
									{ return fBootFiles; }
			const BMessage&		Changes() const { return fChanges; }
			bool				IsBootstrap() const { return fBootstrap; }
									// a record made from what was
									// installed, not from the feed
			const BMessage&		Object() const { return fObject; }

	static	BString				TitleForVersion(const char* version);
	static	BString				VersionForId(const char* id);
	static	int					CompareIds(const char* a, const char* b);
									// by build time, the start of the id

private:
			BMessage			fObject;
			BString				fId;
			BString				fTarget;
			BString				fArchitecture;
			BString				fBuilt;
			BString				fVersion;
			BString				fHaikuRevision;
			BString				fHaikuCommit;
			BString				fFeed;
			BObjectList<ReleasePackage, true> fPackages;
			BObjectList<PayloadLink, true> fPayloadLinks;
			BObjectList<BootFile, true> fBootFiles;
			BMessage			fChanges;
			bool				fBootstrap;
};


#endif	// RELEASE_H
