/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "Release.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <File.h>
#include <Json.h>


// #pragma mark - json


BString
json::String(const BMessage& object, const char* name, const char* defaultValue)
{
	const char* value;
	if (object.FindString(name, &value) == B_OK)
		return value;
	return defaultValue;
}


double
json::Number(const BMessage& object, const char* name, double defaultValue)
{
	double value;
	if (object.FindDouble(name, &value) == B_OK)
		return value;
	return defaultValue;
}


bool
json::Object(const BMessage& object, const char* name, BMessage& _value)
{
	return object.FindMessage(name, &_value) == B_OK;
}


int32
json::Count(const BMessage& array)
{
	return array.CountNames(B_ANY_TYPE);
}


bool
json::At(const BMessage& array, int32 index, BMessage& _value)
{
	BString name;
	name << index;
	return array.FindMessage(name, &_value) == B_OK;
}


BString
json::StringAt(const BMessage& array, int32 index)
{
	BString name;
	name << index;
	return String(array, name);
}


status_t
json::ParseFile(const char* path, BMessage& _object)
{
	BFile file(path, B_READ_ONLY);
	status_t error = file.InitCheck();
	if (error != B_OK)
		return error;

	off_t size;
	error = file.GetSize(&size);
	if (error != B_OK)
		return error;
	if (size > 64 * 1024 * 1024)
		return B_BAD_DATA;

	BString text;
	char* buffer = text.LockBuffer(size + 1);
	if (buffer == NULL)
		return B_NO_MEMORY;
	ssize_t bytesRead = file.ReadAt(0, buffer, size);
	text.UnlockBuffer(bytesRead > 0 ? bytesRead : 0);
	if (bytesRead != size)
		return bytesRead < 0 ? bytesRead : B_IO_ERROR;

	return Parse(text, _object);
}


status_t
json::Parse(const BString& text, BMessage& _object)
{
	_object.MakeEmpty();
	return BPrivate::BJson::Parse(text, _object);
}


BString
json::Quote(const char* text)
{
	BString quoted("\"");
	for (const char* c = text; *c != '\0'; c++) {
		switch (*c) {
			case '"':
				quoted << "\\\"";
				break;
			case '\\':
				quoted << "\\\\";
				break;
			case '\n':
				quoted << "\\n";
				break;
			case '\t':
				quoted << "\\t";
				break;
			case '\r':
				break;
			default:
				if ((uint8)*c < 0x20) {
					char escaped[8];
					snprintf(escaped, sizeof(escaped), "\\u%04x", (uint8)*c);
					quoted << escaped;
				} else
					quoted << *c;
		}
	}
	quoted << '"';
	return quoted;
}


time_t
utc_time(int year, int month, int day, int hour, int minute, int second)
{
	// days since 1970-01-01 of a proleptic Gregorian date
	year -= month <= 2;
	int era = (year >= 0 ? year : year - 399) / 400;
	int yearOfEra = year - era * 400;
	int dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
	int dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100
		+ dayOfYear;
	int64 days = (int64)era * 146097 + dayOfEra - 719468;
	return (time_t)(days * 86400 + hour * 3600 + minute * 60 + second);
}


// #pragma mark - Release


Release::Release()
	:
	fPackages(20),
	fPayloadLinks(20),
	fBootFiles(4),
	fBootstrap(false)
{
}


status_t
Release::SetTo(const BMessage& object)
{
	fObject = object;
	fId = json::String(object, "id");
	fTarget = json::String(object, "target");
	fArchitecture = json::String(object, "arch");
	fBuilt = json::String(object, "built");
	fVersion = json::String(object, "version", VersionForId(fId));
	fFeed = json::String(object, "feed");
	fBootstrap = false;
	object.FindBool("bootstrap", &fBootstrap);

	BMessage haiku;
	if (json::Object(object, "haiku", haiku)) {
		fHaikuRevision = json::String(haiku, "revision");
		fHaikuCommit = json::String(haiku, "commit");
	}

	fPackages.MakeEmpty();
	BMessage packages;
	if (json::Object(object, "packages", packages)) {
		BMessage entry;
		for (int32 i = 0; json::At(packages, i, entry); i++) {
			ReleasePackage* package = new ReleasePackage;
			package->name = json::String(entry, "name");
			package->version = json::String(entry, "version");
			package->file = json::String(entry, "file");
			package->sha256 = json::String(entry, "sha256");
			package->url = json::String(entry, "url");
			package->size = (off_t)json::Number(entry, "size", -1);
			package->optional = false;
			entry.FindBool("optional", &package->optional);
			if (package->name.IsEmpty() || package->file.IsEmpty()
				|| package->file.FindFirst('/') >= 0) {
				delete package;
				return B_BAD_DATA;
			}
			fPackages.AddItem(package);
		}
	}

	fPayloadLinks.MakeEmpty();
	BMessage payload;
	BMessage links;
	if (json::Object(object, "payload", payload)
		&& json::Object(payload, "links", links)) {
		BMessage entry;
		for (int32 i = 0; json::At(links, i, entry); i++) {
			PayloadLink* link = new PayloadLink;
			link->path = json::String(entry, "path");
			link->type = json::String(entry, "type", "file");
			link->target = json::String(entry, "target");
			if (link->path.IsEmpty() || link->path.StartsWith("/")
				|| link->path.FindFirst("..") >= 0) {
				delete link;
				return B_BAD_DATA;
			}
			fPayloadLinks.AddItem(link);
		}
	}

	fBootFiles.MakeEmpty();
	BMessage bootFiles;
	if (json::Object(object, "boot_files", bootFiles)) {
		BMessage entry;
		for (int32 i = 0; json::At(bootFiles, i, entry); i++) {
			BootFile* file = new BootFile;
			file->name = json::String(entry, "name");
			file->sha256 = json::String(entry, "sha256");
			file->url = json::String(entry, "url");
			file->size = (off_t)json::Number(entry, "size", -1);
			if (file->name.IsEmpty() || file->name.FindFirst('/') >= 0
				|| file->sha256.Length() != 64) {
				delete file;
				return B_BAD_DATA;
			}
			fBootFiles.AddItem(file);
		}
	}

	if (!json::Object(object, "changes", fChanges))
		fChanges.MakeEmpty();

	return fId.IsEmpty() ? B_BAD_DATA : B_OK;
}


status_t
Release::Load(const char* path)
{
	BMessage object;
	status_t error = json::ParseFile(path, object);
	if (error != B_OK)
		return error;
	return SetTo(object);
}


const ReleasePackage*
Release::FindPackage(const char* name) const
{
	for (int32 i = 0; const ReleasePackage* package = fPackages.ItemAt(i);
			i++) {
		if (package->name == name)
			return package;
	}
	return NULL;
}


BString
Release::Title() const
{
	return TitleForVersion(fVersion);
}


BString
Release::ShortRevision() const
{
	// "hrev60207-1930-gabcdef0123" -> "hrev60207+1930"
	BString revision = fHaikuRevision;
	int32 dash = revision.FindFirst('-');
	if (dash > 0) {
		int32 secondDash = revision.FindFirst('-', dash + 1);
		if (secondDash > 0)
			revision.Truncate(secondDash);
		revision.SetByteAt(dash, '+');
	}
	return revision;
}


/*static*/ BString
Release::TitleForVersion(const char* version)
{
	// the airos_release version is the build time, "20261026.0915"
	static const char* const kMonths[] = { "January", "February", "March",
		"April", "May", "June", "July", "August", "September", "October",
		"November", "December" };

	int year, month, day, hour, minute;
	if (version != NULL && strlen(version) == 13
		&& sscanf(version, "%4d%2d%2d.%2d%2d", &year, &month, &day, &hour,
			&minute) == 5 && month >= 1 && month <= 12) {
		BString title;
		title.SetToFormat("air/OS %d %s %d, %02d:%02d", day, kMonths[month - 1],
			year, hour, minute);
		return title;
	}

	BString title("air/OS");
	if (version != NULL && version[0] != '\0')
		title << " " << version;
	return title;
}


/*static*/ BString
Release::VersionForId(const char* id)
{
	// "20261026-0915-hrev..." -> "20261026.0915"
	if (id == NULL || strlen(id) < 13 || id[8] != '-')
		return "";
	BString version(id, 13);
	version.SetByteAt(8, '.');
	return version;
}


/*static*/ int
Release::CompareIds(const char* a, const char* b)
{
	// ids start with the build time, which sorts as text
	return strncmp(a, b, 13) != 0 ? strncmp(a, b, 13) : strcmp(a, b);
}
