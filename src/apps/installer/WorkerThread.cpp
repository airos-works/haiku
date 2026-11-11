/*
 * Copyright 2009, Stephan Aßmus <superstippi@gmx.de>.
 * Copyright 2005-2008, Jérôme DUVAL.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "WorkerThread.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#include <functional>
#include <set>
#include <string>
#include <strings.h>
#include <syslog.h>

#include <Alert.h>
#include <Autolock.h>
#include <Catalog.h>
#include <Directory.h>
#include <DiskDeviceVisitor.h>
#include <DiskDeviceTypes.h>
#include <Drivers.h>
#include <File.h>
#include <FindDirectory.h>
#include <fs_index.h>
#include <fs_volume.h>
#include <Locale.h>
#include <Menu.h>
#include <MenuItem.h>
#include <Message.h>
#include <Messenger.h>
#include <PartitioningInfo.h>
#include <Path.h>
#include <SeparatorItem.h>
#include <String.h>
#include <VolumeRoster.h>

#include "AutoLocker.h"
#include "CopyEngine.h"
#include "InstallerDefs.h"
#include "PackageViews.h"
#include "PartitionMenuItem.h"
#include "ProgressReporter.h"
#include "StringForSize.h"
#include "UnzipEngine.h"


#define B_TRANSLATION_CONTEXT "InstallProgress"


//#define COPY_TRACE
#ifdef COPY_TRACE
#define CALLED() 		printf("CALLED %s\n",__PRETTY_FUNCTION__)
#define ERR2(x, y...)	fprintf(stderr, "WorkerThread: "x" %s\n", y, strerror(err))
#define ERR(x)			fprintf(stderr, "WorkerThread: "x" %s\n", strerror(err))
#else
#define CALLED()
#define ERR(x)
#define ERR2(x, y...)
#endif

const char BOOT_PATH[] = "/boot";

const uint32 MSG_START_INSTALLING = 'eSRT';


class SourceVisitor : public BDiskDeviceVisitor {
public:
	SourceVisitor(BMenu* menu);
	virtual bool Visit(BDiskDevice* device);
	virtual bool Visit(BPartition* partition, int32 level);

private:
	BMenu* fMenu;
};


class TargetVisitor : public BDiskDeviceVisitor {
public:
	TargetVisitor(BMenu* menu);
	virtual bool Visit(BDiskDevice* device);
	virtual bool Visit(BPartition* partition, int32 level);

private:
	BMenu* fMenu;
};


class EFIVisitor : public BDiskDeviceVisitor {
public:
	EFIVisitor(BMenu* menu, partition_id bootId);
	virtual bool Visit(BDiskDevice* device);
	virtual bool Visit(BPartition* partition, int32 level);

private:
	BMenu* fMenu;
	partition_id fBootId;
};


// #pragma mark - WorkerThread


class WorkerThread::EntryFilter : public CopyEngine::EntryFilter {
public:
	EntryFilter(const char* sourceDirectory)
		:
		fIgnorePaths(),
		fSourceDevice(-1)
	{
		try {
			fIgnorePaths.insert(kPackagesDirectoryPath);
			fIgnorePaths.insert(kSourcesDirectoryPath);
			fIgnorePaths.insert("rr_moved");
			fIgnorePaths.insert("boot.catalog");
			fIgnorePaths.insert("haiku-boot-floppy.image");
			fIgnorePaths.insert("system/var/swap");
			fIgnorePaths.insert("system/var/shared_memory");
			fIgnorePaths.insert("system/var/log/syslog");
			fIgnorePaths.insert("system/var/log/syslog.old");
			fIgnorePaths.insert("system/settings/ssh/ssh_host_ecdsa_key");
			fIgnorePaths.insert("system/settings/ssh/ssh_host_ecdsa_key.pub");
			fIgnorePaths.insert("system/settings/ssh/ssh_host_ed25519_key");
			fIgnorePaths.insert("system/settings/ssh/ssh_host_ed25519_key.pub");
			fIgnorePaths.insert("system/settings/ssh/ssh_host_rsa_key");
			fIgnorePaths.insert("system/settings/ssh/ssh_host_rsa_key.pub");

			fPackageFSRootPaths.insert("system");
			fPackageFSRootPaths.insert("home/config");
		} catch (std::bad_alloc&) {
		}

		struct stat st;
		if (stat(sourceDirectory, &st) == 0)
			fSourceDevice = st.st_dev;
	}

	virtual bool ShouldCopyEntry(const BEntry& entry, const char* path,
		const struct stat& statInfo) const
	{
		if (S_ISBLK(statInfo.st_mode) || S_ISCHR(statInfo.st_mode)
				|| S_ISFIFO(statInfo.st_mode) || S_ISSOCK(statInfo.st_mode)) {
			printf("skipping '%s', it is a special file.\n", path);
			return false;
		}

		if (fIgnorePaths.find(path) != fIgnorePaths.end()) {
			printf("ignoring '%s'.\n", path);
			return false;
		}

		if (_IsDriverStartMarker(path)) {
			printf("ignoring '%s', a driver start marker of this "
				"machine.\n", path);
			return false;
		}

		if (statInfo.st_dev != fSourceDevice) {
			// Allow that only for the root of the packagefs mounts, since
			// those contain directories that shine through from the
			// underlying volume.
			if (fPackageFSRootPaths.find(path) == fPackageFSRootPaths.end())
				return false;
		}

		return true;
	}

private:
	/*!	A kernel driver's start marker ("<driver>.starting" in the drivers'
		settings, see amdgpu_drm): it is left while the driver starts, and
		found by the next boot it keeps the driver off. One of the live
		session must not keep the driver off on the installed system: on some
		machines (the AMD Framework boards) that looks like a boot that hangs
		at the splash screen.
	*/
	static bool _IsDriverStartMarker(const char* path)
	{
		static const char kDirectory[] = "home/config/settings/kernel/drivers/";
		static const char kSuffix[] = ".starting";
		const size_t directoryLength = sizeof(kDirectory) - 1;
		const size_t suffixLength = sizeof(kSuffix) - 1;
		size_t length = strlen(path);
		return length > directoryLength + suffixLength
			&& strncmp(path, kDirectory, directoryLength) == 0
			&& strchr(path + directoryLength, '/') == NULL
			&& strcmp(path + length - suffixLength, kSuffix) == 0;
	}

	typedef std::set<std::string> StringSet;

			StringSet			fIgnorePaths;
			StringSet			fPackageFSRootPaths;
			dev_t				fSourceDevice;
};


// #pragma mark - WorkerThread


WorkerThread::WorkerThread(const BMessenger& owner)
	:
	BLooper("copy_engine"),
	fOwner(owner),
	fPackages(NULL),
	fSpaceRequired(0),
	fCancelSemaphore(-1)
{
	Run();
}


void
WorkerThread::MessageReceived(BMessage* message)
{
	CALLED();

	switch (message->what) {
		case MSG_START_INSTALLING:
			_PerformInstall(message->GetInt32("source", -1),
				message->GetInt32("target", -1),
				message->GetBool("whole disk", false));
			break;

		case MSG_WRITE_BOOT_SECTOR:
		{
			int32 id;
			if (message->FindInt32("id", &id) != B_OK) {
				_SetStatusMessage(B_TRANSLATE("Boot sector not written "
					"because of an internal error."));
				break;
			}

			// TODO: Refactor with _PerformInstall()
			BPath targetDirectory;
			BDiskDevice device;
			BPartition* partition;

			if (fDDRoster.GetPartitionWithID(id, &device, &partition) == B_OK) {
				if (!partition->IsMounted()) {
					if (partition->Mount() < B_OK) {
						_SetStatusMessage(B_TRANSLATE("The partition can't be "
							"mounted. Please choose a different partition."));
						break;
					}
				}
				if (partition->GetMountPoint(&targetDirectory) != B_OK) {
					_SetStatusMessage(B_TRANSLATE("The mount point could not "
						"be retrieved."));
					break;
				}
			} else if (fDDRoster.GetDeviceWithID(id, &device) == B_OK) {
				if (!device.IsMounted()) {
					if (device.Mount() < B_OK) {
						_SetStatusMessage(B_TRANSLATE("The disk can't be "
							"mounted. Please choose a different disk."));
						break;
					}
				}
				if (device.GetMountPoint(&targetDirectory) != B_OK) {
					_SetStatusMessage(B_TRANSLATE("The mount point could not "
						"be retrieved."));
					break;
				}
			}

			if (_WriteBootSector(targetDirectory) != B_OK) {
				_SetStatusMessage(
					B_TRANSLATE("Error writing boot sector."));
				break;
			}
			_SetStatusMessage(
				B_TRANSLATE("Boot sector successfully written."));
		}
		default:
			BLooper::MessageReceived(message);
	}
}


static BString
arch_efi_default_prefix()
{
#if defined(__i386__)
	return BString("BOOTIA32");
#elif defined(__x86_64__)
	return BString("BOOTX64");
#elif defined(__arm__) || defined(__ARM__)
	return BString("BOOTARM");
#elif defined(__aarch64__) || defined(__arm64__)
	return BString("BOOTAA64");
#elif defined(__riscv) && __riscv_xlen == 32
	return BString("BOOTRISCV32");
#elif defined(__riscv) && __riscv_xlen == 64
	return BString("BOOTRISCV64");
#elif defined(__powerpc__)
	return BString("BOOTPPC");
#else
	#error "Error: Unknown EFI Architecture!"
#endif
}


void
WorkerThread::InstallEFILoader(partition_id id, bool rename)
{
	// Executed in window thread.
	BDiskDevice device;
	BPartition* partition;
	BDirectory destDir;
	BPath loaderPath;
	BFile loaderToCopy;
	BFile loaderDest;
	BPath destPath;
	BEntry existingEntry;
	off_t size;
	BString errText;
	status_t err = B_OK;

	BString archLoader = arch_efi_default_prefix();
	archLoader.Append(".EFI");
	BString archLoaderBackup = arch_efi_default_prefix();
	archLoaderBackup.Append("_old.EFI");

	if (find_directory(B_SYSTEM_DATA_DIRECTORY, &loaderPath) != B_OK
		|| loaderPath.Append("platform_loaders/haiku_loader.efi") != B_OK
		|| loaderToCopy.SetTo(loaderPath.Path(), B_READ_ONLY) != B_OK
		|| loaderToCopy.InitCheck() != B_OK
		|| loaderToCopy.GetSize(&size) != B_OK)
		errText.SetTo(B_TRANSLATE("Failed to find EFI loader file!"));

	char* buffer = new char[size];
	if (errText.IsEmpty() && loaderToCopy.Read(buffer, size) != size)
		errText.SetTo(B_TRANSLATE("Failed to read EFI loader file!"));

	if (errText.IsEmpty()
		&& (fDDRoster.GetPartitionWithID(id, &device, &partition) != B_OK
		|| (!partition->IsMounted() && partition->Mount() != B_OK)
		|| partition->GetMountPoint(&destPath) != B_OK))
		errText.SetTo(B_TRANSLATE("Failed to access installation destination!"));

	if (errText.IsEmpty()
		&& (destPath.Append("EFI/BOOT") != B_OK
		|| create_directory(destPath.Path(), 0755) != B_OK
		|| destDir.SetTo(destPath.Path()) != B_OK
		|| destDir.InitCheck() != B_OK))
		errText.SetTo(B_TRANSLATE("Failed to create EFI loader directory!"));

	if (errText.IsEmpty() && rename
		&& (destDir.FindEntry(archLoader, &existingEntry) != B_OK
		|| existingEntry.Rename(archLoaderBackup, true) != B_OK))
		errText.SetTo(B_TRANSLATE("Failed to rename existing loader!"));

	if (errText.IsEmpty()
		&& (err = destDir.CreateFile(archLoader, &loaderDest, true)) == B_FILE_EXISTS) {
		BAlert* confirmAlert = new BAlert("", B_TRANSLATE("An EFI loader is already installed "
			"on the selected partition! Would you like to rename it?"),
			B_TRANSLATE("Rename"), B_TRANSLATE("Cancel"));
		confirmAlert->SetFlags(confirmAlert->Flags() | B_CLOSE_ON_ESCAPE);
		if (confirmAlert->Go() == 0)
			InstallEFILoader(id, true);
		delete[] buffer;
		return;
	} else if (errText.IsEmpty() && (err != B_OK || loaderDest.Write(buffer, size) != size))
		errText.SetTo(B_TRANSLATE("Failed to copy EFI loader to selected partition!"));

	delete[] buffer;
	BAlert* alert = new BAlert("", B_TRANSLATE("EFI loader successfully installed!"),
		B_TRANSLATE("OK"));
	alert->SetFlags(alert->Flags() | B_CLOSE_ON_ESCAPE);
	alert->SetType(B_INFO_ALERT);
	if (!errText.IsEmpty()) {
		alert->SetType(B_STOP_ALERT);
		alert->SetText(errText);
	}
	alert->Go();
}


/*!	What to call a disk: the name its driver reports, or else the kind of
	disk it is, from the bus in its path.
*/
static BString
disk_model_name(const char* path)
{
	char name[B_FILE_NAME_LENGTH] = "";
	int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		if (ioctl(fd, B_GET_DEVICE_NAME, name, sizeof(name)) != 0)
			name[0] = '\0';
		close(fd);
	}
	BString model(name);
	model.Trim();
	if (!model.IsEmpty())
		return model;

	BString bus(path);
	bus.RemoveFirst("/dev/disk/");
	int32 slash = bus.FindFirst('/');
	if (slash >= 0)
		bus.Truncate(slash);
	if (bus == "nvme")
		return B_TRANSLATE("NVMe disk");
	if (bus == "mmc")
		return B_TRANSLATE_COMMENT("MMC disk", "An eMMC chip or an SD card");
	if (bus == "usb")
		return B_TRANSLATE("USB disk");
	if (bus == "virtual")
		return B_TRANSLATE("Virtual disk");
	return B_TRANSLATE("Disk");
}


/*!	Lists the names of the volumes on a disk, so that the user can tell the
	disks apart before choosing one to erase.
*/
static void
append_volume_names(BPartition* partition, BString& names, int32& count)
{
	if (partition->ContainsFileSystem() && partition->ContentName().Length() > 0) {
		if (count < 3) {
			if (count > 0)
				names << ", ";
			names << partition->ContentName();
		} else if (count == 3)
			names << ", " B_UTF8_ELLIPSIS;
		count++;
	}
	for (int32 i = 0; BPartition* child = partition->ChildAt(i); i++)
		append_volume_names(child, names, count);
}


void
WorkerThread::ScanDisksPartitions(BMenu *srcMenu, BMenu *targetMenu, BMenu* EFIMenu)
{
	// NOTE: This is actually executed in the window thread.
	BDiskDevice device;
	BPartition *partition = NULL;

	SourceVisitor srcVisitor(srcMenu);
	fDDRoster.VisitEachMountedPartition(&srcVisitor, &device, &partition);

	BDiskDevice bootDevice;
	BPartition* bootPartition;
	partition_id bootId = -1;
	partition_id bootDiskId = -1;
	if (fDDRoster.FindPartitionByMountPoint(BOOT_PATH, &bootDevice,
			&bootPartition) == B_OK) {
		bootDiskId = bootDevice.ID();
		if (bootPartition->Parent() != NULL)
			bootId = bootPartition->Parent()->ID();
	}

	// Whole disks come first: installing onto one needs nothing prepared.
	// The disk must hold what is on the boot volume, the EFI system
	// partition, and some room to spare.
	off_t requiredSize = 2LL * 1024 * 1024 * 1024;
	BVolume bootVolume;
	if (BVolumeRoster().GetBootVolume(&bootVolume) == B_OK) {
		off_t used = bootVolume.Capacity() - bootVolume.FreeBytes();
		off_t needed = used + kEFISystemPartitionSize + 512LL * 1024 * 1024;
		if (used > 0 && needed > requiredSize)
			requiredSize = needed;
	}

	int32 diskCount = 0;
	BDiskDevice disk;
	fDDRoster.RewindDevices();
	while (fDDRoster.GetNextDevice(&disk) == B_OK) {
		if (disk.IsReadOnlyMedia() || !disk.HasMedia() || disk.Size() <= 0)
			continue;

		BPath path;
		disk.GetPath(&path);
		BString model = disk_model_name(path.Path());
		char size[20];
		string_for_size(disk.Size(), size, sizeof(size));

		const char* reason = NULL;
		if (disk.ID() == bootDiskId)
			reason = B_TRANSLATE("the disk this system runs from");
		else if (disk.IsReadOnly())
			reason = B_TRANSLATE("read-only");
		else if (disk.Size() < requiredSize)
			reason = B_TRANSLATE("too small");

		BString label;
		label.SetToFormat("%s - %s [%s]", model.String(), size, path.Path());
		BString volumes;
		int32 volumeCount = 0;
		append_volume_names(&disk, volumes, volumeCount);
		if (!volumes.IsEmpty())
			label << " - " << volumes;
		if (reason != NULL)
			label << " (" << reason << ")";

		BString menuLabel;
		menuLabel.SetToFormat(B_TRANSLATE("Whole disk: %s - %s"),
			model.String(), size);
		BString name;
		name.SetToFormat("%s (%s)", model.String(), size);

		if (diskCount++ == 0) {
			BMenuItem* header = new BMenuItem(B_TRANSLATE(
				"Erase a whole disk and set it up for UEFI:"), NULL);
			header->SetEnabled(false);
			targetMenu->AddItem(header);
		}
		PartitionMenuItem* item = new PartitionMenuItem(name.String(),
			label.String(), menuLabel.String(),
			new BMessage(TARGET_PARTITION), disk.ID());
		item->SetIsWholeDisk(true);
		item->SetIsValidTarget(reason == NULL);
		targetMenu->AddItem(item);
	}

	BMenuItem* separator = NULL;
	BMenuItem* partitionsHeader = NULL;
	if (diskCount > 0) {
		separator = new BSeparatorItem();
		targetMenu->AddItem(separator);
		partitionsHeader = new BMenuItem(B_TRANSLATE(
			"Install onto an existing partition:"), NULL);
		partitionsHeader->SetEnabled(false);
		targetMenu->AddItem(partitionsHeader);
	}
	int32 itemCount = targetMenu->CountItems();

	TargetVisitor targetVisitor(targetMenu);
	fDDRoster.VisitEachPartition(&targetVisitor, &device, &partition);

	if (partitionsHeader != NULL && targetMenu->CountItems() == itemCount) {
		// no partitions to offer
		targetMenu->RemoveItem(partitionsHeader);
		targetMenu->RemoveItem(separator);
		delete partitionsHeader;
		delete separator;
	}

	EFIVisitor EFIVisitor(EFIMenu, bootId);
	fDDRoster.VisitEachPartition(&EFIVisitor, &device, &partition);
}


void
WorkerThread::SetPackagesList(BList *list)
{
	// Executed in window thread.
	BAutolock _(this);

	delete fPackages;
	fPackages = list;
}


void
WorkerThread::StartInstall(partition_id sourcePartitionID,
	partition_id targetPartitionID, bool wholeDisk)
{
	// Executed in window thread.
	BMessage message(MSG_START_INSTALLING);
	message.AddInt32("source", sourcePartitionID);
	message.AddInt32("target", targetPartitionID);
	message.AddBool("whole disk", wholeDisk);

	PostMessage(&message, this);
}


void
WorkerThread::WriteBootSector(BMenu* targetMenu)
{
	// Executed in window thread.
	CALLED();

	PartitionMenuItem* item
		= dynamic_cast<PartitionMenuItem*>(targetMenu->FindMarked());
	if (item == NULL || item->IsWholeDisk()) {
		ERR("bad menu items\n");
		return;
	}

	BMessage message(MSG_WRITE_BOOT_SECTOR);
	message.AddInt32("id", item->ID());
	PostMessage(&message, this);
}


// #pragma mark -


status_t
WorkerThread::_WriteBootSector(BPath &path)
{
	BPath bootPath;
	find_directory(B_BEOS_BOOT_DIRECTORY, &bootPath);
	BString command;
	command.SetToFormat("makebootable \"%s\"", path.Path());
	_SetStatusMessage(B_TRANSLATE("Writing bootsector."));
	return system(command.String());
}


status_t
WorkerThread::_LaunchFinishScript(BPath &path)
{
	_SetStatusMessage(B_TRANSLATE("Finishing installation."));

	BString command;
	command.SetToFormat("mkdir -p \"%s/system/cache/tmp\"", path.Path());
	if (system(command.String()) != 0)
		return B_ERROR;
	command.SetToFormat("mkdir -p \"%s/system/packages/administrative\"",
		path.Path());
	if (system(command.String()) != 0)
		return B_ERROR;

	// Ask for first boot processing of all the packages copied into the new
	// installation, since by just copying them the normal package processing
	// isn't done.  package_daemon will detect the magic file and do it.
	command.SetToFormat("echo 'First Boot written by Installer.' > "
		"\"%s/system/packages/administrative/FirstBootProcessingNeeded\"",
		path.Path());
	if (system(command.String()) != 0)
		return B_ERROR;

	command.SetToFormat("rm -f \"%s/home/Desktop/Installer\"", path.Path());
	return system(command.String());
}


status_t
WorkerThread::_PerformInstall(partition_id sourcePartitionID,
	partition_id targetPartitionID, bool wholeDisk)
{
	CALLED();

	BPath targetDirectory;
	BPath srcDirectory;
	BPath trashPath;
	BPath testPath;
	BDirectory targetDir;
	BDiskDevice device;
	BPartition* partition;
	BVolume targetVolume;
	status_t err = B_OK;
	int32 entries = 0;
	entry_ref testRef;
	const char* mountError = B_TRANSLATE("The disk can't be mounted. Please "
		"choose a different disk.");

	if (sourcePartitionID < 0 || targetPartitionID < 0) {
		ERR("bad source or target partition ID\n");
		return _InstallationError(err);
	}

	// A whole disk is erased and partitioned first; the installation then
	// goes to its new BFS partition. The user has already confirmed this.
	partition_id espID = -1;
	if (wholeDisk) {
		err = _PrepareWholeDisk(targetPartitionID, targetPartitionID, espID);
		if (err != B_OK)
			return _InstallationError(err);
	}

	// check if target is initialized
	// ask if init or mount as is
	if (fDDRoster.GetPartitionWithID(targetPartitionID, &device,
			&partition) == B_OK) {
		if (!partition->IsMounted()) {
			if ((err = partition->Mount()) < B_OK) {
				_SetStatusMessage(mountError);
				ERR("BPartition::Mount");
				return _InstallationError(err);
			}
		}
		if ((err = partition->GetVolume(&targetVolume)) != B_OK) {
			ERR("BPartition::GetVolume");
			return _InstallationError(err);
		}
		if ((err = partition->GetMountPoint(&targetDirectory)) != B_OK) {
			ERR("BPartition::GetMountPoint");
			return _InstallationError(err);
		}
	} else if (fDDRoster.GetDeviceWithID(targetPartitionID, &device) == B_OK) {
		if (!device.IsMounted()) {
			if ((err = device.Mount()) < B_OK) {
				_SetStatusMessage(mountError);
				ERR("BDiskDevice::Mount");
				return _InstallationError(err);
			}
		}
		if ((err = device.GetVolume(&targetVolume)) != B_OK) {
			ERR("BDiskDevice::GetVolume");
			return _InstallationError(err);
		}
		if ((err = device.GetMountPoint(&targetDirectory)) != B_OK) {
			ERR("BDiskDevice::GetMountPoint");
			return _InstallationError(err);
		}
	} else
		return _InstallationError(err);  // shouldn't happen

	// check if target has enough space
	if (fSpaceRequired > 0 && targetVolume.FreeBytes() < fSpaceRequired) {
		BAlert* alert = new BAlert("", B_TRANSLATE("The destination disk may "
			"not have enough space. Try choosing a different disk or choose "
			"to not install optional items."),
			B_TRANSLATE("Try installing anyway"), B_TRANSLATE("Cancel"), 0,
			B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->SetShortcut(1, B_ESCAPE);
		if (alert->Go() != 0)
			return _InstallationError(err);
	}

	if (fDDRoster.GetPartitionWithID(sourcePartitionID, &device, &partition)
			== B_OK) {
		if ((err = partition->GetMountPoint(&srcDirectory)) != B_OK) {
			ERR("BPartition::GetMountPoint");
			return _InstallationError(err);
		}
	} else if (fDDRoster.GetDeviceWithID(sourcePartitionID, &device) == B_OK) {
		if ((err = device.GetMountPoint(&srcDirectory)) != B_OK) {
			ERR("BDiskDevice::GetMountPoint");
			return _InstallationError(err);
		}
	} else
		return _InstallationError(err); // shouldn't happen

	// check not installing on itself
	if (strcmp(srcDirectory.Path(), targetDirectory.Path()) == 0) {
		_SetStatusMessage(B_TRANSLATE("You can't install the contents of a "
			"disk onto itself. Please choose a different disk."));
		return _InstallationError(err);
	}

	// check not installing on boot volume
	if (strncmp(BOOT_PATH, targetDirectory.Path(), strlen(BOOT_PATH)) == 0) {
		BString text(B_TRANSLATE("Are you sure you want to "
		"install onto the current boot disk? The %appname% will have to "
		"reboot your machine if you proceed."));
		text.ReplaceFirst("%appname%", B_TRANSLATE_SYSTEM_NAME("Installer"));
		BAlert* alert = new BAlert("", text, B_TRANSLATE("OK"),
			B_TRANSLATE("Cancel"), 0, B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->SetShortcut(1, B_ESCAPE);
		if (alert->Go() != 0) {
			_SetStatusMessage("Installation stopped.");
			return _InstallationError(err);
		}
	}

	// check if target volume's trash dir has anything in it
	// (target volume w/ only an empty trash dir is considered
	// an empty volume)
	if (find_directory(B_TRASH_DIRECTORY, &trashPath, false,
		&targetVolume) == B_OK && targetDir.SetTo(trashPath.Path()) == B_OK) {
			while (targetDir.GetNextRef(&testRef) == B_OK) {
				// Something in the Trash
				entries++;
				break;
			}
	}

	targetDir.SetTo(targetDirectory.Path());

	// check if target volume otherwise has any entries
	while (entries == 0 && targetDir.GetNextRef(&testRef) == B_OK) {
		if (testPath.SetTo(&testRef) == B_OK && testPath != trashPath)
			entries++;
	}

	if (entries != 0) {
		BAlert* alert = new BAlert("", B_TRANSLATE("The target volume is not "
			"empty. If it already contains a Haiku installation, it will be "
			"overwritten. This will remove all installed software.\n\n"
			"If you want to upgrade your system without removing installed "
			"software, see the Haiku User Guide's topic on the application "
			"\"SoftwareUpdater\" for update instructions.\n\n"
			"Are you sure you want to continue the installation?"),
			B_TRANSLATE("Install anyway"), B_TRANSLATE("Cancel"), 0,
			B_WIDTH_AS_USUAL, B_STOP_ALERT);
		alert->SetShortcut(1, B_ESCAPE);
		if (alert->Go() != 0) {
		// TODO: Would be cool to offer the option here to clean additional
		// folders at the user's choice.
			return _InstallationError(B_CANCELED);
		}
		err = _PrepareCleanInstall(targetDirectory);
		if (err != B_OK)
			return _InstallationError(err);
	}

	// Begin actual installation

	ProgressReporter reporter(fOwner, new BMessage(MSG_STATUS_MESSAGE));
	EntryFilter entryFilter(srcDirectory.Path());
	CopyEngine engine(&reporter, &entryFilter);
	BList unzipEngines;

	// Create the default indices which should always be present on a proper
	// boot volume. We don't care if the source volume does not have them.
	// After all, the user might be re-installing to another drive and may
	// want problems fixed along the way...
	err = _CreateDefaultIndices(targetDirectory);
	if (err != B_OK)
		return _InstallationError(err);
	// Mirror all the indices which are present on the source volume onto
	// the target volume.
	err = _MirrorIndices(srcDirectory, targetDirectory);
	if (err != B_OK)
		return _InstallationError(err);

	// Let the engine collect information for the progress bar later on
	engine.ResetTargets(srcDirectory.Path());
	err = engine.CollectTargets(srcDirectory.Path(), fCancelSemaphore);
	if (err != B_OK)
		return _InstallationError(err);

	// Collect selected packages also
	if (fPackages) {
		int32 count = fPackages->CountItems();
		for (int32 i = 0; i < count; i++) {
			Package *p = static_cast<Package*>(fPackages->ItemAt(i));
			const BPath& pkgPath = p->Path();
			err = pkgPath.InitCheck();
			if (err != B_OK)
				return _InstallationError(err);
			err = engine.CollectTargets(pkgPath.Path(), fCancelSemaphore);
			if (err != B_OK)
				return _InstallationError(err);
		}
	}

	// collect information about all zip packages
	err = _ProcessZipPackages(srcDirectory.Path(), targetDirectory.Path(),
		&reporter, unzipEngines);
	if (err != B_OK)
		return _InstallationError(err);

	reporter.StartTimer();

	// copy source volume
	err = engine.Copy(srcDirectory.Path(), targetDirectory.Path(),
		fCancelSemaphore);
	if (err != B_OK)
		return _InstallationError(err);

	// copy selected packages
	if (fPackages) {
		int32 count = fPackages->CountItems();
		// FIXME: find_directory doesn't return the folder in the target volume,
		// so we are hard coding this for now.
		BPath targetPkgDir(targetDirectory.Path(), "system/packages");
		err = targetPkgDir.InitCheck();
		if (err != B_OK)
			return _InstallationError(err);
		for (int32 i = 0; i < count; i++) {
			Package *p = static_cast<Package*>(fPackages->ItemAt(i));
			const BPath& pkgPath = p->Path();
			err = pkgPath.InitCheck();
			if (err != B_OK)
				return _InstallationError(err);
			BPath targetPath(targetPkgDir.Path(), pkgPath.Leaf());
			err = targetPath.InitCheck();
			if (err != B_OK)
				return _InstallationError(err);
			err = engine.Copy(pkgPath.Path(), targetPath.Path(),
				fCancelSemaphore);
			if (err != B_OK)
				return _InstallationError(err);
		}
	}

	// Extract all zip packages. If an error occured, delete the rest of
	// the engines, but stop extracting.
	for (int32 i = 0; i < unzipEngines.CountItems(); i++) {
		UnzipEngine* engine = reinterpret_cast<UnzipEngine*>(
			unzipEngines.ItemAtFast(i));
		if (err == B_OK)
			err = engine->UnzipPackage();
		delete engine;
	}
	if (err != B_OK)
		return _InstallationError(err);

	err = _WriteBootSector(targetDirectory);
	if (err != B_OK)
		return _InstallationError(err);

	if (wholeDisk) {
		err = _InstallEFILoader(espID);
		if (err != B_OK)
			return _InstallationError(err);
	}

	err = _LaunchFinishScript(targetDirectory);
	if (err != B_OK)
		return _InstallationError(err);

	// Write everything out before saying that it is done: the computer may
	// well be switched off right then.
	sync();

	fOwner.SendMessage(MSG_INSTALL_FINISHED);
	return B_OK;
}


static status_t
unmount_all(BPartition* partition)
{
	for (int32 i = 0; BPartition* child = partition->ChildAt(i); i++) {
		status_t status = unmount_all(child);
		if (status != B_OK)
			return status;
	}

	if (!partition->IsMounted())
		return B_OK;

	// The user agreed to erase the disk, so a volume that is still in use
	// (a Tracker window on it, say) is unmounted anyway.
	status_t status = partition->Unmount();
	if (status != B_OK)
		status = partition->Unmount(B_FORCE_UNMOUNT);
	return status;
}


static off_t
align_up(off_t value, off_t alignment)
{
	return (value + alignment - 1) / alignment * alignment;
}


static off_t
align_down(off_t value, off_t alignment)
{
	return value / alignment * alignment;
}


/*!	Applies one change to the disk \a diskID and writes it: \a modify gets
	a freshly read and prepared device. The kernel rescans a disk after it
	has been written to, and a change prepared against the state from before
	that rescan is refused (B_BAD_VALUE for a stale change counter, or
	B_BUSY), so such a change is tried again, on the disk as it is now.
	\a modify must therefore work from whatever state it finds.
*/
status_t
WorkerThread::_ModifyDisk(partition_id diskID,
	const std::function<status_t(BDiskDevice&)>& modify)
{
	status_t status = B_ERROR;
	for (int32 attempt = 0; attempt < 10; attempt++) {
		if (attempt > 0)
			snooze(500000);

		BDiskDevice device;
		status = fDDRoster.GetDeviceWithID(diskID, &device);
		if (status != B_OK)
			return status;
		status = device.PrepareModifications();
		if (status == B_OK) {
			status = modify(device);
			if (status == B_OK)
				status = device.CommitModifications();
			else
				device.CancelModifications();
		}
		if (status == B_OK)
			return B_OK;
		syslog(LOG_NOTICE, "Installer: changing disk %" B_PRId32 " failed "
			"(attempt %" B_PRId32 "): %s\n", diskID, attempt + 1,
			strerror(status));
		if (status != B_BAD_VALUE && status != B_BUSY)
			break;
	}
	return status;
}


/*!	Erases the disk \a diskID and sets it up to start with UEFI: a GUID
	partition map, an EFI system partition formatted with FAT32, and the rest
	of the disk as one BFS partition, which is left mounted. Each step is
	written on its own, as DriveSetup does.
*/
status_t
WorkerThread::_PrepareWholeDisk(partition_id diskID, partition_id& _bootID,
	partition_id& _espID)
{
	static const off_t kAlignment = 1024 * 1024;

	BDiskDevice device;
	status_t status = fDDRoster.GetDeviceWithID(diskID, &device);
	if (status != B_OK)
		return status;
	if (device.IsReadOnly())
		return B_READ_ONLY_DEVICE;

	_SetStatusMessage(B_TRANSLATE("Erasing the disk" B_UTF8_ELLIPSIS));

	status = unmount_all(&device);
	if (status != B_OK) {
		fErrorContext = B_TRANSLATE("A volume on the disk could not be "
			"unmounted. Close the applications that use it and try again.");
		return status;
	}

	// replace whatever is on the disk with an empty GUID partition map
	status = _ModifyDisk(diskID, [](BDiskDevice& disk) {
		if (disk.ContentType() != NULL)
			disk.Uninitialize();
		status_t status = disk.ValidateInitialize(kPartitionTypeEFI, NULL,
			NULL);
		if (status == B_OK)
			status = disk.Initialize(kPartitionTypeEFI, NULL, NULL);
		return status;
	});
	if (status != B_OK) {
		fErrorContext = B_TRANSLATE("The disk could not be erased.");
		return status;
	}

	_SetStatusMessage(B_TRANSLATE("Creating the partitions" B_UTF8_ELLIPSIS));

	off_t espOffset = -1;
	off_t bootOffset = -1;
	status = _ModifyDisk(diskID, [&](BDiskDevice& disk) {
		// start from an empty partition map, also after a failed attempt
		for (int32 i = disk.CountChildren() - 1; i >= 0; i--) {
			status_t status = disk.DeleteChild(i);
			if (status != B_OK)
				return status;
		}

		BPartitioningInfo info;
		status_t status = disk.GetPartitioningInfo(&info);
		if (status != B_OK)
			return status;
		off_t spaceOffset = 0;
		off_t spaceSize = 0;
		for (int32 i = 0; i < info.CountPartitionableSpaces(); i++) {
			off_t offset;
			off_t size;
			if (info.GetPartitionableSpaceAt(i, &offset, &size) == B_OK
				&& size > spaceSize) {
				spaceOffset = offset;
				spaceSize = size;
			}
		}
		off_t start = align_up(spaceOffset, kAlignment);
		off_t end = align_down(spaceOffset + spaceSize, kAlignment);
		if (end - start < 2 * kEFISystemPartitionSize)
			return (status_t)B_DEVICE_FULL;

		espOffset = start;
		off_t espSize = kEFISystemPartitionSize;
		BString espName("EFI system partition");
		// The kernel refuses to create a child without a parameter string.
		status = disk.ValidateCreateChild(&espOffset, &espSize,
			"EFI system data", &espName, "");
		if (status == B_OK) {
			status = disk.CreateChild(espOffset, espSize, "EFI system data",
				espName.String(), "");
		}
		if (status != B_OK)
			return status;

		bootOffset = espOffset + espSize;
		off_t bootSize = end - bootOffset;
		BString bootName(kWholeDiskVolumeName);
		status = disk.ValidateCreateChild(&bootOffset, &bootSize,
			kPartitionTypeBFS, &bootName, "");
		if (status == B_OK) {
			status = disk.CreateChild(bootOffset, bootSize, kPartitionTypeBFS,
				bootName.String(), "");
		}
		return status;
	});
	if (status != B_OK) {
		fErrorContext = B_TRANSLATE("The partitions could not be created.");
		return status;
	}

	// find the new partitions again, by where they start
	status = fDDRoster.GetDeviceWithID(diskID, &device);
	if (status != B_OK)
		return status;
	_espID = -1;
	_bootID = -1;
	for (int32 i = 0; BPartition* child = device.ChildAt(i); i++) {
		if (child->Offset() == espOffset)
			_espID = child->ID();
		else if (child->Offset() == bootOffset)
			_bootID = child->ID();
	}
	if (_espID < 0 || _bootID < 0) {
		fErrorContext = B_TRANSLATE("The partitions could not be created.");
		return B_ENTRY_NOT_FOUND;
	}

	_SetStatusMessage(B_TRANSLATE("Formatting the partitions" B_UTF8_ELLIPSIS));

	status = _InitializePartition(diskID, _espID, kPartitionTypeFAT32, "EFI",
		"fat 32;\n");
	if (status == B_OK) {
		status = _InitializePartition(diskID, _bootID, kPartitionTypeBFS,
			kWholeDiskVolumeName, "block_size 2048\n");
	}
	if (status != B_OK) {
		fErrorContext = B_TRANSLATE("The partitions could not be "
			"formatted.");
		return status;
	}

	BPartition* boot;
	status = fDDRoster.GetPartitionWithID(_bootID, &device, &boot);
	if (status == B_OK && !boot->IsMounted())
		status = boot->Mount();
	if (status < B_OK) {
		syslog(LOG_ERR, "Installer: mounting the new partition failed: %s\n",
			strerror(status));
		fErrorContext = B_TRANSLATE("The new partition could not be "
			"mounted.");
		return status;
	}
	return B_OK;
}


status_t
WorkerThread::_InitializePartition(partition_id diskID,
	partition_id partitionID, const char* diskSystem, const char* name,
	const char* parameters)
{
	return _ModifyDisk(diskID, [=](BDiskDevice& disk) {
		BPartition* partition = disk.FindDescendant(partitionID);
		if (partition == NULL)
			return (status_t)B_ENTRY_NOT_FOUND;
		BString validatedName(name);
		status_t status = partition->ValidateInitialize(diskSystem,
			&validatedName, parameters);
		if (status == B_OK) {
			status = partition->Initialize(diskSystem, validatedName.String(),
				parameters);
		}
		return status;
	});
}


/*!	Copies the system's EFI loader to the fallback path of the EFI system
	partition \a espID, which UEFI firmware starts from a disk without being
	told about it first.
*/
status_t
WorkerThread::_InstallEFILoader(partition_id espID)
{
	_SetStatusMessage(B_TRANSLATE("Installing the EFI loader."));

	BPath loaderPath;
	status_t status = find_directory(B_SYSTEM_DATA_DIRECTORY, &loaderPath);
	if (status == B_OK)
		status = loaderPath.Append("platform_loaders/haiku_loader.efi");
	BFile loader;
	if (status == B_OK)
		status = loader.SetTo(loaderPath.Path(), B_READ_ONLY);
	if (status != B_OK) {
		syslog(LOG_ERR, "Installer: no EFI loader at %s: %s\n", loaderPath.Path(),
			strerror(status));
		fErrorContext = B_TRANSLATE("The EFI loader was not found.");
		return status;
	}

	BDiskDevice device;
	BPartition* esp;
	status = fDDRoster.GetPartitionWithID(espID, &device, &esp);
	if (status == B_OK && !esp->IsMounted())
		status = esp->Mount();
	BPath bootDirectory;
	if (status >= B_OK)
		status = esp->GetMountPoint(&bootDirectory);
	if (status == B_OK)
		status = bootDirectory.Append("EFI/BOOT");
	if (status == B_OK)
		status = create_directory(bootDirectory.Path(), 0755);
	if (status != B_OK) {
		syslog(LOG_ERR, "Installer: cannot prepare the EFI system partition: %s\n",
			strerror(status));
		fErrorContext = B_TRANSLATE("The EFI loader could not be installed.");
		return status;
	}

	BString name(arch_efi_default_prefix());
	name << ".EFI";
	BPath targetPath(bootDirectory.Path(), name.String());
	BFile target;
	status = target.SetTo(targetPath.Path(),
		B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);

	char buffer[64 * 1024];
	while (status == B_OK) {
		ssize_t bytesRead = loader.Read(buffer, sizeof(buffer));
		if (bytesRead <= 0) {
			if (bytesRead < 0)
				status = bytesRead;
			break;
		}
		ssize_t bytesWritten = target.Write(buffer, bytesRead);
		if (bytesWritten != bytesRead)
			status = bytesWritten < 0 ? bytesWritten : B_IO_ERROR;
	}
	if (status == B_OK)
		status = target.Sync();
	target.Unset();
	if (status != B_OK) {
		syslog(LOG_ERR, "Installer: writing %s failed: %s\n", targetPath.Path(),
			strerror(status));
		fErrorContext = B_TRANSLATE("The EFI loader could not be installed.");
		return status;
	}

	// Nothing else needs the partition; unmounting it writes everything out.
	esp->Unmount();
	return B_OK;
}


status_t
WorkerThread::_PrepareCleanInstall(const BPath& targetDirectory) const
{
	// When a target volume has files (other than the trash), the /system
	// folder will be purged, except for the /system/settings subdirectory.
	BPath systemPath(targetDirectory.Path(), "system", true);
	status_t ret = systemPath.InitCheck();
	if (ret != B_OK)
		return ret;

	BEntry systemEntry(systemPath.Path());
	ret = systemEntry.InitCheck();
	if (ret != B_OK)
		return ret;
	if (!systemEntry.Exists())
		// target does not exist, done
		return B_OK;
	if (!systemEntry.IsDirectory())
		// the system entry is a file or a symlink
		return systemEntry.Remove();

	BDirectory systemDirectory(&systemEntry);
	ret = systemDirectory.InitCheck();
	if (ret != B_OK)
		return ret;

	BEntry subEntry;
	char fileName[B_FILE_NAME_LENGTH];
	while (systemDirectory.GetNextEntry(&subEntry) == B_OK) {
		ret = subEntry.GetName(fileName);
		if (ret != B_OK)
			return ret;

		if (subEntry.IsDirectory() && strcmp(fileName, "settings") == 0) {
			// Keep the settings folder
			continue;
		} else if (subEntry.IsDirectory()) {
			ret = CopyEngine::RemoveFolder(subEntry);
			if (ret != B_OK)
				return ret;
		} else {
			ret = subEntry.Remove();
			if (ret != B_OK)
				return ret;
		}
	}

	return B_OK;
}


status_t
WorkerThread::_InstallationError(status_t error)
{
	BMessage statusMessage(MSG_RESET);
	if (error == B_CANCELED)
		_SetStatusMessage(B_TRANSLATE("Installation canceled."));
	else {
		statusMessage.AddInt32("error", error);
		if (!fErrorContext.IsEmpty())
			statusMessage.AddString("context", fErrorContext);
	}
	fErrorContext.Truncate(0);
	ERR("_PerformInstall failed");
	fOwner.SendMessage(&statusMessage);
	return error;
}


status_t
WorkerThread::_MirrorIndices(const BPath& sourceDirectory,
	const BPath& targetDirectory) const
{
	dev_t sourceDevice = dev_for_path(sourceDirectory.Path());
	if (sourceDevice < 0)
		return (status_t)sourceDevice;
	dev_t targetDevice = dev_for_path(targetDirectory.Path());
	if (targetDevice < 0)
		return (status_t)targetDevice;
	DIR* indices = fs_open_index_dir(sourceDevice);
	if (indices == NULL) {
		printf("%s: fs_open_index_dir(): (%d) %s\n", sourceDirectory.Path(),
			errno, strerror(errno));
		// Opening the index directory will fail for example on ISO-Live
		// CDs. The default indices have already been created earlier, so
		// we simply bail.
		return B_OK;
	}
	while (dirent* index = fs_read_index_dir(indices)) {
		if (strcmp(index->d_name, "name") == 0
			|| strcmp(index->d_name, "size") == 0
			|| strcmp(index->d_name, "last_modified") == 0) {
			continue;
		}

		index_info info;
		if (fs_stat_index(sourceDevice, index->d_name, &info) != B_OK) {
			printf("Failed to mirror index %s: fs_stat_index(): (%d) %s\n",
				index->d_name, errno, strerror(errno));
			continue;
		}

		uint32 flags = 0;
			// Flags are always 0 for the moment.
		if (fs_create_index(targetDevice, index->d_name, info.type, flags)
			!= B_OK) {
			if (errno == B_FILE_EXISTS)
				continue;
			printf("Failed to mirror index %s: fs_create_index(): (%d) %s\n",
				index->d_name, errno, strerror(errno));
			continue;
		}
	}
	fs_close_index_dir(indices);
	return B_OK;
}


status_t
WorkerThread::_CreateDefaultIndices(const BPath& targetDirectory) const
{
	dev_t targetDevice = dev_for_path(targetDirectory.Path());
	if (targetDevice < 0)
		return (status_t)targetDevice;

	struct IndexInfo {
		const char* name;
		uint32_t	type;
	};

	const IndexInfo defaultIndices[] = {
		{ "BEOS:APP_SIG", B_STRING_TYPE },
		{ "BEOS:LOCALE_LANGUAGE", B_STRING_TYPE },
		{ "BEOS:LOCALE_SIGNATURE", B_STRING_TYPE },
		{ "_trk/qrylastchange", B_INT32_TYPE },
		{ "_trk/recentQuery", B_INT32_TYPE },
		{ "be:deskbar_item_status", B_STRING_TYPE }
	};

	uint32 flags = 0;
		// Flags are always 0 for the moment.

	for (uint32 i = 0; i < sizeof(defaultIndices) / sizeof(IndexInfo); i++) {
		const IndexInfo& info = defaultIndices[i];
		if (fs_create_index(targetDevice, info.name, info.type, flags)
			!= B_OK) {
			if (errno == B_FILE_EXISTS)
				continue;
			printf("Failed to create index %s: fs_create_index(): (%d) %s\n",
				info.name, errno, strerror(errno));
			return errno;
		}
	}

	return B_OK;
}


status_t
WorkerThread::_ProcessZipPackages(const char* sourcePath,
	const char* targetPath, ProgressReporter* reporter, BList& unzipEngines)
{
	// TODO: Put those in the optional packages list view
	// TODO: Implement mechanism to handle dependencies between these
	// packages. (Selecting one will auto-select others.)
	BPath pkgRootDir(sourcePath, kPackagesDirectoryPath);
	BDirectory directory(pkgRootDir.Path());
	BEntry entry;
	while (directory.GetNextEntry(&entry) == B_OK) {
		char name[B_FILE_NAME_LENGTH];
		if (entry.GetName(name) != B_OK)
			continue;
		int nameLength = strlen(name);
		if (nameLength <= 0)
			continue;
		char* nameExtension = name + nameLength - 4;
		if (strcasecmp(nameExtension, ".zip") != 0)
			continue;
		printf("found .zip package: %s\n", name);

		UnzipEngine* unzipEngine = new(std::nothrow) UnzipEngine(reporter,
			fCancelSemaphore);
		if (unzipEngine == NULL || !unzipEngines.AddItem(unzipEngine)) {
			delete unzipEngine;
			return B_NO_MEMORY;
		}
		BPath path;
		entry.GetPath(&path);
		status_t ret = unzipEngine->SetTo(path.Path(), targetPath);
		if (ret != B_OK)
			return ret;

		reporter->AddItems(unzipEngine->ItemsToUncompress(),
			unzipEngine->BytesToUncompress());
	}

	return B_OK;
}


void
WorkerThread::_SetStatusMessage(const char *status)
{
	BMessage msg(MSG_STATUS_MESSAGE);
	msg.AddString("status", status);
	fOwner.SendMessage(&msg);
}


static void
make_partition_label(BPartition* partition, char* label, char* menuLabel,
	bool showContentType, bool markBootDisk)
{
	char size[20];
	string_for_size(partition->Size(), size, sizeof(size));

	BPath path;
	partition->GetPath(&path);

	BString bootMark("");
	if (markBootDisk)
		bootMark.SetTo(B_TRANSLATE_COMMENT(" (boot disk)",
			"Marks EFI partitions on boot disk - preserve leading space"));

	if (showContentType) {
		const char* type = partition->ContentType();
		if (type == NULL)
			type = B_TRANSLATE_COMMENT("Unknown type", "Partition content type");

		sprintf(label, "%s%s - %s [%s] (%s)", partition->ContentName().String(), bootMark.String(),
			size, path.Path(), type);
	} else {
		sprintf(label, "%s%s - %s [%s]", partition->ContentName().String(), bootMark.String(),
			size, path.Path());
	}

	sprintf(menuLabel, "%s%s - %s", partition->ContentName().String(), bootMark.String(), size);
}


// #pragma mark - SourceVisitor


SourceVisitor::SourceVisitor(BMenu *menu)
	: fMenu(menu)
{
}

bool
SourceVisitor::Visit(BDiskDevice *device)
{
	return Visit(device, 0);
}


bool
SourceVisitor::Visit(BPartition *partition, int32 level)
{
	BPath path;

	if (partition->ContentType() == NULL)
		return false;

	bool isBootPartition = false;
	if (partition->IsMounted()) {
		BPath mountPoint;
		if (partition->GetMountPoint(&mountPoint) != B_OK)
			return false;
		isBootPartition = strcmp(BOOT_PATH, mountPoint.Path()) == 0;
	}

	if (!isBootPartition
		&& strcmp(partition->ContentType(), kPartitionTypeBFS) != 0) {
		// Except only BFS partitions, except this is the boot partition
		// (ISO9660 with write overlay for example).
		return false;
	}

	// TODO: We could probably check if this volume contains
	// the Haiku kernel or something. Does it make sense to "install"
	// from your BFS volume containing the music collection?
	// TODO: Then the check for BFS could also be removed above.

	char label[255];
	char menuLabel[255];
	make_partition_label(partition, label, menuLabel, false, false);
	PartitionMenuItem* item = new PartitionMenuItem(partition->ContentName(),
		label, menuLabel, new BMessage(SOURCE_PARTITION), partition->ID());
	item->SetMarked(isBootPartition);
	fMenu->AddItem(item);
	return false;
}


// #pragma mark - TargetVisitor


TargetVisitor::TargetVisitor(BMenu *menu)
	: fMenu(menu)
{
}


bool
TargetVisitor::Visit(BDiskDevice *device)
{
	if (device->IsReadOnlyMedia())
		return false;
	return Visit(device, 0);
}


bool
TargetVisitor::Visit(BPartition *partition, int32 level)
{
	if (partition->ContentSize() < 20 * 1024 * 1024) {
		// reject partitions which are too small anyway
		// TODO: Could depend on the source size
		return false;
	}

	if (partition->CountChildren() > 0) {
		// Looks like an extended partition, or the device itself.
		// Do not accept this as target...
		return false;
	}

	// TODO: After running DriveSetup and doing another scan, it would
	// be great to pick the partition which just appeared!

	bool isBootPartition = false;
	if (partition->IsMounted()) {
		BPath mountPoint;
		partition->GetMountPoint(&mountPoint);
		isBootPartition = strcmp(BOOT_PATH, mountPoint.Path()) == 0;
	}

	// Only writable non-boot BFS partitions are valid targets, but we want to
	// display the other partitions as well, to inform the user that they are
	// detected but somehow not appropriate.
	bool isValidTarget = isBootPartition == false
		&& !partition->IsReadOnly()
		&& partition->ContentType() != NULL
		&& strcmp(partition->ContentType(), kPartitionTypeBFS) == 0;

	char label[255];
	char menuLabel[255];
	make_partition_label(partition, label, menuLabel, !isValidTarget, false);
	PartitionMenuItem* item = new PartitionMenuItem(partition->ContentName(),
		label, menuLabel, new BMessage(TARGET_PARTITION), partition->ID());

	item->SetIsValidTarget(isValidTarget);


	fMenu->AddItem(item);
	return false;
}


// #pragma mark - EFIVisitor


EFIVisitor::EFIVisitor(BMenu *menu, partition_id bootId)
	:
	fMenu(menu),
	fBootId(bootId)
{
}


bool
EFIVisitor::Visit(BDiskDevice *device)
{
	if (device->IsReadOnlyMedia())
		return false;
	return Visit(device, 0);
}


bool
EFIVisitor::Visit(BPartition *partition, int32 level)
{
	// Makes sure this is a large enough writeable FAT32 non-extended EFI partition on a GUID disk
	if (partition->IsReadOnly()
		|| partition->ContentSize() < 1024 * 1024
		|| partition->CountChildren() > 0
		|| partition->Type() == NULL
		|| strcmp(partition->Type(), "EFI system data") != 0
		|| partition->ContentType() == NULL
		|| strcmp(partition->ContentType(), kPartitionTypeFAT32) != 0
		|| partition->Parent() == NULL
		|| partition->Parent()->ContentType() == NULL
		|| strcmp(partition->Parent()->ContentType(), kPartitionTypeEFI) != 0)
		return false;

	char label[255];
	char menuLabel[255];
	make_partition_label(partition, label, menuLabel, false, partition->Parent()->ID() == fBootId);
	BMessage* message = new BMessage(EFI_PARTITION);
	message->AddInt32("id", partition->ID());
	BMenuItem* item = new BMenuItem(label, message);
	fMenu->AddItem(item);
	return false;
}
