/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */


#include "PiBoot.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <Directory.h>
#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <DiskDeviceTypes.h>
#include <DiskDeviceVisitor.h>
#include <File.h>
#include <Partition.h>
#include <Volume.h>
#include <VolumeRoster.h>

#include "SystemState.h"
#include "Updater.h"


static const char* const kSelectFile = "airos-boot.txt";
static const char* const kLoaderName = "airos-loader.img";
static const char* const kArchiveName = "airos-boot.tgz";


static bool
exists(const char* path)
{
	struct stat st;
	return stat(path, &st) == 0;
}


static status_t
read_file(const char* path, BString& _text)
{
	BFile file(path, B_READ_ONLY);
	status_t error = file.InitCheck();
	if (error != B_OK)
		return error;
	off_t size;
	if (file.GetSize(&size) != B_OK || size > 256 * 1024)
		return B_BAD_DATA;
	char* buffer = _text.LockBuffer(size + 1);
	ssize_t bytesRead = file.ReadAt(0, buffer, size);
	_text.UnlockBuffer(bytesRead > 0 ? bytesRead : 0);
	return bytesRead < 0 ? bytesRead : B_OK;
}


/*!	FAT has no rename over an existing file that survives a power cut for
	sure, so the old file goes first, then the new one takes its name.
*/
static status_t
replace_file(const char* path, const BString& content)
{
	BString temporary(path);
	temporary << ".new";
	BFile file(temporary, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
	status_t error = file.InitCheck();
	if (error != B_OK)
		return error;
	if (file.Write(content.String(), content.Length()) != content.Length())
		return B_IO_ERROR;
	file.Sync();
	file.Unset();
	chmod(path, 0644);
	if (rename(temporary, path) != 0) {
		unlink(path);
		if (rename(temporary, path) != 0)
			return errno;
	}
	sync();
	return B_OK;
}


namespace {

struct FatFinder : BDiskDeviceVisitor {
	partition_id	id;

	FatFinder() : id(-1) {}

	virtual bool Visit(BDiskDevice*)
	{
		return false;
	}

	virtual bool Visit(BPartition* partition, int32)
	{
		const char* type = partition->ContentType();
		if (type != NULL && (strcmp(type, kPartitionTypeFAT32) == 0
				|| strcmp(type, kPartitionTypeFAT16) == 0)) {
			id = partition->ID();
			return true;
		}
		return false;
	}
};

}	// namespace


PiBoot::PiBoot()
	:
	fDevice(NULL),
	fPartition(NULL),
	fMounted(false)
{
}


PiBoot::~PiBoot()
{
	Unmount();
	delete fDevice;
}


/*static*/ bool
PiBoot::IsPi()
{
	return Updater().Target() == "rpi4";
}


status_t
PiBoot::Mount(BString& _error)
{
	if (fPartition != NULL)
		return B_OK;

	BVolume bootVolume;
	BVolumeRoster().GetBootVolume(&bootVolume);
	BDiskDeviceRoster roster;
	fDevice = new BDiskDevice;
	BPartition* bootPartition;
	if (roster.FindPartitionByVolume(bootVolume, fDevice, &bootPartition)
			!= B_OK) {
		_error = "cannot find the disk air/OS runs from";
		return B_ENTRY_NOT_FOUND;
	}
	FatFinder finder;
	fDevice->VisitEachDescendant(&finder);
	fPartition = finder.id >= 0 ? fDevice->FindDescendant(finder.id) : NULL;
	if (fPartition == NULL) {
		_error = "the system disk has no FAT boot partition";
		return B_ENTRY_NOT_FOUND;
	}
	if (!fPartition->IsMounted()) {
		dev_t volume = fPartition->Mount();
		if (volume < 0) {
			_error.SetToFormat("cannot mount the boot partition: %s",
				strerror(volume));
			fPartition = NULL;
			return volume;
		}
		fMounted = true;
	}
	status_t error = fPartition->GetMountPoint(&fMountPoint);
	if (error == B_OK) {
		BString config(fMountPoint.Path());
		config << "/config.txt";
		if (!exists(config)) {
			_error = "the FAT partition has no config.txt";
			error = B_ENTRY_NOT_FOUND;
		}
	}
	if (error != B_OK)
		Unmount();
	return error;
}


void
PiBoot::Unmount()
{
	if (fPartition != NULL && fMounted)
		fPartition->Unmount();
	fPartition = NULL;
	fMounted = false;
}


status_t
PiBoot::Install(const char* version, const BObjectList<BootFile, true>& files,
	const BStringList& sources, BString& _error)
{
	BString directory;
	directory.SetToFormat("%s/airos/%s", Path(), version);
	status_t error = create_directory(directory, 0755);
	if (error != B_OK) {
		_error.SetToFormat("cannot create %s: %s", directory.String(),
			strerror(error));
		return error;
	}

	for (int32 i = 0; const BootFile* file = files.ItemAt(i); i++) {
		BString target(directory);
		target << '/' << file->name;
		BString temporary(target);
		temporary << ".new";

		BFile source(sources.StringAt(i), B_READ_ONLY);
		BFile copy(temporary, B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
		error = source.InitCheck() != B_OK ? source.InitCheck()
			: copy.InitCheck();
		char buffer[64 * 1024];
		while (error == B_OK) {
			ssize_t bytesRead = source.Read(buffer, sizeof(buffer));
			if (bytesRead <= 0) {
				error = bytesRead;
				break;
			}
			if (copy.Write(buffer, bytesRead) != bytesRead)
				error = B_IO_ERROR;
		}
		if (error == B_OK)
			error = copy.Sync();
		copy.Unset();

		BString sha;
		if (error == B_OK)
			error = Updater::ComputeSha256(temporary, sha);
		if (error == B_OK && sha != file->sha256)
			error = B_IO_ERROR;
		if (error == B_OK) {
			unlink(target);
			if (rename(temporary, target) != 0)
				error = errno;
		}
		if (error != B_OK) {
			unlink(temporary);
			_error.SetToFormat("cannot put %s on the boot partition: %s",
				file->name.String(), strerror(error));
			return error;
		}
	}
	sync();
	Updater::Log("boot partition: installed the boot files of %s", version);
	return B_OK;
}


bool
PiBoot::HasVersion(const char* version) const
{
	BString loader;
	loader.SetToFormat("%s/airos/%s/%s", Path(), version, kLoaderName);
	BString archive;
	archive.SetToFormat("%s/airos/%s/%s", Path(), version, kArchiveName);
	return exists(loader) && exists(archive);
}


/*!	Makes config.txt take the loader and boot archive from airos-boot.txt:
	an image from before System Updater names them itself.
*/
status_t
PiBoot::_EnsureInclude(BString& _error)
{
	BString path(Path());
	path << "/config.txt";
	BString config;
	status_t error = read_file(path, config);
	if (error != B_OK) {
		_error = "cannot read config.txt";
		return error;
	}
	if (config.FindFirst("include airos-boot.txt") >= 0)
		return B_OK;

	BStringList lines;
	config.Split("\n", false, lines);
	BString updated;
	for (int32 i = 0; i < lines.CountStrings(); i++) {
		BString line = lines.StringAt(i);
		BString trimmed(line);
		trimmed.Trim();
		if (trimmed.StartsWith("kernel=") || trimmed.StartsWith("initramfs ")) {
			updated << "# " << line << " (now in airos-boot.txt)\n";
			continue;
		}
		updated << line;
		if (i + 1 < lines.CountStrings())
			updated << "\n";
	}
	updated << "\n# System Updater: the loader and boot archive of the version "
		"that starts\ninclude airos-boot.txt\n";

	// keep the original
	BString backup(Path());
	backup << "/config-before-system-updater.txt";
	if (!exists(backup))
		replace_file(backup, config);
	return replace_file(path, updated);
}


status_t
PiBoot::Select(const char* version, BString& _error)
{
	BString loader(kLoaderName), archive(kArchiveName);
	if (version != NULL && version[0] != '\0') {
		if (!HasVersion(version)) {
			_error.SetToFormat("the boot partition has no boot files of %s",
				version);
			return B_ENTRY_NOT_FOUND;
		}
		loader.Prepend(BString().SetToFormat("airos/%s/", version));
		archive.Prepend(BString().SetToFormat("airos/%s/", version));
	}

	BString content;
	content.SetToFormat("# written by System Updater: the version that starts "
		"next (%s)\nkernel=%s\ninitramfs %s followkernel\n",
		version != NULL && version[0] != '\0' ? version : "the image's own",
		loader.String(), archive.String());

	BString path(Path());
	path << '/' << kSelectFile;
	BString current;
	if (read_file(path, current) == B_OK && current == content)
		return _EnsureInclude(_error);

	// airos-boot.txt first: config.txt must never include a missing file
	status_t error = replace_file(path, content);
	if (error != B_OK) {
		_error.SetToFormat("cannot write %s: %s", kSelectFile, strerror(error));
		return error;
	}
	error = _EnsureInclude(_error);
	if (error == B_OK) {
		Updater::Log("boot partition: %s starts next",
			version != NULL && version[0] != '\0' ? version
				: "the image's own version");
	}
	return error;
}


BString
PiBoot::SelectedVersion() const
{
	BString path(Path());
	path << '/' << kSelectFile;
	BString content;
	if (read_file(path, content) != B_OK)
		return "";
	int32 start = content.FindFirst("kernel=airos/");
	if (start < 0)
		return "";
	start += strlen("kernel=airos/");
	int32 end = content.FindFirst('/', start);
	BString version;
	if (end > start)
		content.CopyInto(version, start, end - start);
	return version;
}


status_t
PiBoot::Prune(const BStringList& keep)
{
	BString directory(Path());
	directory << "/airos";
	DIR* dir = opendir(directory);
	if (dir == NULL)
		return B_OK;
	BString selected = SelectedVersion();
	while (dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.' || keep.HasString(entry->d_name)
			|| selected == entry->d_name) {
			continue;
		}
		BString path(directory);
		path << '/' << entry->d_name;
		unlink(BString(path).Append("/").Append(kLoaderName));
		unlink(BString(path).Append("/").Append(kArchiveName));
		if (rmdir(path) == 0)
			Updater::Log("boot partition: removed the boot files of %s",
				entry->d_name);
	}
	closedir(dir);
	return B_OK;
}
