/*
 * Copyright 2007-2014, Ingo Weinhold, ingo_weinhold@gmx.de.
 * Copyright 2002-2010, Axel Dörfler, axeld@pinc-software.de.
 * Distributed under the terms of the MIT License.
 *
 * Copyright 2001-2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#include "vfs_boot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <dirent.h>
#include <fcntl.h>
#include <fs_info.h>
#include <fs_volume.h>
#include <OS.h>

#include <AutoDeleter.h>
#include <airos_boot_control.h>
#include <boot/kernel_args.h>
#include <directories.h>
#include <disk_device_manager/KDiskDevice.h>
#include <disk_device_manager/KDiskDeviceManager.h>
#include <disk_device_manager/KPartitionVisitor.h>
#include <DiskDeviceTypes.h>
#include <file_cache.h>
#include <fs/KPath.h>
#include <kmodule.h>
#include <syscalls.h>
#include <util/KMessage.h>
#include <util/Stack.h>
#include <vfs.h>

#include "vfs_net_boot.h"


//#define TRACE_VFS
#ifdef TRACE_VFS
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


typedef Stack<KPartition *> PartitionStack;

static struct {
	const char *path;
	const char *target;
} sPredefinedLinks[] = {
	{ kGlobalSystemDirectory,		kSystemDirectory },
	{ kGlobalBinDirectory,			kSystemBinDirectory },
	{ kGlobalEtcDirectory,			kSystemEtcDirectory },
	{ kGlobalTempDirectory,			kSystemTempDirectory },
	{ kGlobalVarDirectory,			kSystemVarDirectory },
	{ kGlobalPackageLinksDirectory,	kSystemPackageLinksDirectory },
	{NULL}
};

// This can be used by other code to see if there is a boot file system already
dev_t gBootDevice = -1;
bool gReadOnlyBootDevice = false;


/*!	No image was chosen - prefer disks with names like "Haiku", or "System"
 */
int
compare_image_boot(const void* _a, const void* _b)
{
	KPartition* a = *(KPartition**)_a;
	KPartition* b = *(KPartition**)_b;

	if (a->ContentName() != NULL) {
		if (b->ContentName() == NULL)
			return 1;
	} else if (b->ContentName() != NULL) {
		return -1;
	} else
		return 0;

	int compare = strcasecmp(a->ContentName(), b->ContentName());
	if (!compare)
		return 0;

	if (!strcasecmp(a->ContentName(), "Haiku"))
		return 1;
	if (!strcasecmp(b->ContentName(), "Haiku"))
		return -1;
	if (!strncmp(a->ContentName(), "System", 6))
		return 1;
	if (!strncmp(b->ContentName(), "System", 6))
		return -1;

	return compare;
}


/*!	The system was booted from CD - prefer CDs over other entries. If there
	is no CD, fall back to the standard mechanism (as implemented by
	compare_image_boot().
*/
static int
compare_cd_boot(const void* _a, const void* _b)
{
	KPartition* a = *(KPartition**)_a;
	KPartition* b = *(KPartition**)_b;

	bool aIsCD = a->Type() != NULL
		&& !strcmp(a->Type(), kPartitionTypeDataSession);
	bool bIsCD = b->Type() != NULL
		&& !strcmp(b->Type(), kPartitionTypeDataSession);

	int compare = (int)aIsCD - (int)bIsCD;
	if (compare != 0)
		return compare;

	return compare_image_boot(_a, _b);
}


/*!	Computes a check sum for the specified block.
	The check sum is the sum of all data in that block interpreted as an
	array of uint32 values.
	Note, this must use the same method as the one used in
	boot/platform/bios_ia32/devices.cpp (or similar solutions).
*/
static uint32
compute_check_sum(KDiskDevice* device, off_t offset)
{
	char buffer[512];
	ssize_t bytesRead = read_pos(device->FD(), offset, buffer, sizeof(buffer));
	if (bytesRead < B_OK)
		return 0;

	if (bytesRead < (ssize_t)sizeof(buffer))
		memset(buffer + bytesRead, 0, sizeof(buffer) - bytesRead);

	uint32* array = (uint32*)buffer;
	uint32 sum = 0;

	for (uint32 i = 0;
			i < (bytesRead + sizeof(uint32) - 1) / sizeof(uint32); i++) {
		sum += array[i];
	}

	return sum;
}


/*!	Whether \a partition is the file system of a hybrid ISO image, an
	installation medium made by anyboot: the device it is on starts with an
	ISO 9660 volume descriptor. Such a medium is always used live, mounted
	read-only beneath a write overlay, also when it is not a CD: a USB disk
	does not report that it is write protected (see usb_disk), and a medium
	that is writable is not changed either.
*/
static bool
is_hybrid_iso_partition(KPartition* partition)
{
	KDiskDevice* device = partition->Device();
	if (device == NULL || partition == device || device->FD() < 0
		|| partition->Offset() < 64 * 1024) {
		return false;
	}

	// the primary volume descriptor is in the 2 KiB block at 32 KiB
	uint8* block = (uint8*)malloc(2048);
	if (block == NULL)
		return false;
	ssize_t bytesRead = read_pos(device->FD(), 32 * 1024, block, 2048);
	bool hybrid = bytesRead == 2048 && block[0] == 1
		&& memcmp(block + 1, "CD001", 5) == 0;
	free(block);
	return hybrid;
}


// #pragma mark - BootMethod


BootMethod::BootMethod(const KMessage& bootVolume, int32 method)
	:
	fBootVolume(bootVolume),
	fMethod(method)
{
}


BootMethod::~BootMethod()
{
}


status_t
BootMethod::Init()
{
	return B_OK;
}


// #pragma mark - DiskBootMethod


class DiskBootMethod : public BootMethod {
public:
	DiskBootMethod(const KMessage& bootVolume, int32 method)
		: BootMethod(bootVolume, method)
	{
	}

	virtual bool IsBootDevice(KDiskDevice* device, bool strict);
	virtual bool IsBootPartition(KPartition* partition, bool& foundForSure);
	virtual void SortPartitions(KPartition** partitions, int32 count);
};


bool
DiskBootMethod::IsBootDevice(KDiskDevice* device, bool strict)
{
	disk_identifier* disk;
	int32 diskIdentifierSize;
	if (fBootVolume.FindData(BOOT_VOLUME_DISK_IDENTIFIER, B_RAW_TYPE,
			(const void**)&disk, &diskIdentifierSize) != B_OK) {
		dprintf("DiskBootMethod::IsBootDevice(): no disk identifier!\n");
		return false;
	}

	TRACE(("boot device: bus %" B_PRId32 ", device %" B_PRId32 "\n",
		disk->bus_type, disk->device_type));

	// Assume that CD boots only happen off removable media.
	if (fMethod == BOOT_METHOD_CD && !device->IsRemovable())
		return false;

	switch (disk->bus_type) {
		case PCI_BUS:
		case LEGACY_BUS:
			// TODO: implement this! (and then enable this feature in the boot
			// loader)
			// (we need a way to get the device_node of a device, then)
			break;

		case UNKNOWN_BUS:
			// nothing to do here
			break;
	}

	switch (disk->device_type) {
		case UNKNOWN_DEVICE:
			// test if the size of the device matches
			// (the BIOS might have given us the wrong value here, though)
			if (strict && device->Size() != disk->device.unknown.size)
				return false;

			// Skip the check sum test for CDs, since we didn't read anything
			// useful from the disk in the boot loader.
			if (fMethod == BOOT_METHOD_CD)
				break;

			// check if the check sums match, too
			for (int32 i = 0; i < NUM_DISK_CHECK_SUMS; i++) {
				if (disk->device.unknown.check_sums[i].offset == -1)
					continue;

				if (compute_check_sum(device,
						disk->device.unknown.check_sums[i].offset)
							!= disk->device.unknown.check_sums[i].sum) {
					return false;
				}
			}
			break;

		case ATA_DEVICE:
		case ATAPI_DEVICE:
		case SCSI_DEVICE:
		case USB_DEVICE:
		case FIREWIRE_DEVICE:
		case FIBRE_DEVICE:
			// TODO: implement me!
			break;
	}

	return true;
}


bool
DiskBootMethod::IsBootPartition(KPartition* partition, bool& foundForSure)
{
	off_t bootPartitionOffset = fBootVolume.GetInt64(
		BOOT_VOLUME_PARTITION_OFFSET, 0);

	if (!fBootVolume.GetBool(BOOT_VOLUME_BOOTED_FROM_IMAGE, false)) {
		// the simple case: we can just boot from the selected boot
		// device
		if (partition->Offset() == bootPartitionOffset) {
			dprintf("Identified boot partition by partition offset.\n");
			foundForSure = true;
			return true;
		}
	} else {
		// For now, unless we can positively identify an anyboot CD, we will
		// just collect all BFS/ISO9660 volumes.

		if (fMethod == BOOT_METHOD_CD) {
			// Check for the boot partition of an anyboot CD. We identify it as
			// such if it is a partition on the CD, has type BFS, and the boot
			// partition offset is 0
			KDiskDevice* device = partition->Device();
			if (IsBootDevice(device, false)
				&& bootPartitionOffset == 0 && partition->Parent() == device
				&& device->ContentType() != NULL
				&& strcmp(device->ContentType(), kPartitionTypeIntel) == 0
				&& partition->ContentType() != NULL
				&& strcmp(partition->ContentType(), kPartitionTypeBFS) == 0) {
				dprintf("Identified anyboot CD.\n");
				foundForSure = true;
				return true;
			}

			// Ignore non-session partitions, if a boot partition was selected
			// by the user.
			if (fBootVolume.GetBool(BOOT_VOLUME_USER_SELECTED, false)
				&& partition->Type() != NULL
				&& strcmp(partition->Type(), kPartitionTypeDataSession) != 0) {
				return false;
			}
		}

		if (partition->ContentType() != NULL
			&& (strcmp(partition->ContentType(), kPartitionTypeBFS) == 0
			|| strcmp(partition->ContentType(), kPartitionTypeISO9660) == 0)) {
			return true;
		}
	}

	return false;
}


void
DiskBootMethod::SortPartitions(KPartition** partitions, int32 count)
{
	qsort(partitions, count, sizeof(KPartition*),
		fMethod == BOOT_METHOD_CD ? compare_cd_boot : compare_image_boot);
}


// #pragma mark -


/*!	Make the boot partition (and probably others) available.
	The partitions that are a boot candidate a put into the /a partitions
	stack. If the user selected a boot device, there is will only be one
	entry in this stack; if not, the most likely is put up first.
	The boot code should then just try them one by one.
*/
static status_t
get_boot_partitions(KMessage& bootVolume, PartitionStack& partitions)
{
	dprintf("get_boot_partitions(): boot volume message:\n");
	bootVolume.Dump(&dprintf);

	// create boot method
	int32 bootMethodType = bootVolume.GetInt32(BOOT_METHOD, BOOT_METHOD_DEFAULT);
	dprintf("get_boot_partitions(): boot method type: %" B_PRId32 "\n",
		bootMethodType);

	BootMethod* bootMethod = NULL;
	switch (bootMethodType) {
		case BOOT_METHOD_NET:
			bootMethod = new(nothrow) NetBootMethod(bootVolume, bootMethodType);
			break;

		case BOOT_METHOD_HARD_DISK:
		case BOOT_METHOD_CD:
		default:
			bootMethod = new(nothrow) DiskBootMethod(bootVolume,
				bootMethodType);
			break;
	}

	status_t status = bootMethod != NULL ? bootMethod->Init() : B_NO_MEMORY;
	if (status != B_OK)
		return status;

	KDiskDeviceManager::CreateDefault();
	KDiskDeviceManager *manager = KDiskDeviceManager::Default();

	status = manager->InitialDeviceScan();
	if (status != B_OK) {
		dprintf("KDiskDeviceManager::InitialDeviceScan() returned error: %s\n",
			strerror(status));
		// InitialDeviceScan returns error if one (or more) partitions are
		// determined to be invalid. The partition we are trying to boot from
		// may be usuable anyway, so don't fail here.
	}

#if KDEBUG
	// dump devices and partitions
	KDiskDevice *device;
	int32 cookie = 0;
	while ((device = manager->NextDevice(&cookie)) != NULL) {
		device->Dump(true, 0);
	}
#endif

	struct BootPartitionVisitor : KPartitionVisitor {
		BootPartitionVisitor(BootMethod* bootMethod, PartitionStack &stack)
			: fPartitions(stack),
			  fBootMethod(bootMethod)
		{
		}

		virtual bool VisitPre(KPartition *partition)
		{
			if (!partition->ContainsFileSystem())
				return false;

			bool foundForSure = false;
			if (fBootMethod->IsBootPartition(partition, foundForSure))
				fPartitions.Push(partition);

			// if found for sure, we can terminate the search
			return foundForSure;
		}

		private:
			PartitionStack	&fPartitions;
			BootMethod*		fBootMethod;
	} visitor(bootMethod, partitions);

	bool strict = true;

	while (true) {
		KDiskDevice *device;
		int32 cookie = 0;
		while ((device = manager->NextDevice(&cookie)) != NULL) {
			if (!bootMethod->IsBootDevice(device, strict))
				continue;

			if (device->VisitEachDescendant(&visitor) != NULL)
				break;
		}

		if (!partitions.IsEmpty() || !strict)
			break;

		// we couldn't find any potential boot devices, try again less strict
		strict = false;
	}

	// sort partition list (e.g.. when booting from CD, CDs should come first in
	// the list)
	if (!bootVolume.GetBool(BOOT_VOLUME_USER_SELECTED, false))
		bootMethod->SortPartitions(partitions.Array(), partitions.CountItems());

	return B_OK;
}


/*!	The boot control files and package states of the system volume, for
	airos_choose_boot_state() where the boot loader could not read the boot
	volume (the Raspberry Pi's loader only has the boot archive).
*/
struct KernelBootControlReader {
	KPath	directory;
	char	newest[B_FILE_NAME_LENGTH];

	KernelBootControlReader()
		:
		directory(kSystemPackagesDirectory)
	{
		newest[0] = '\0';
	}

	ssize_t ReadFile(const char* name, char* buffer, size_t size)
	{
		KPath path(directory);
		if (path.Append(AIROS_BOOT_CONTROL_DIRECTORY) != B_OK
			|| path.Append(name) != B_OK) {
			return B_NO_MEMORY;
		}
		int fd = _kern_open(-1, path.Path(), O_RDONLY, 0);
		if (fd < 0)
			return fd;
		ssize_t length = _kern_read(fd, 0, buffer, size);
		_kern_close(fd);
		if (length >= 0 && (size_t)length < size)
			buffer[length] = '\0';
		return length;
	}

	// whether the system package an activation file names is there
	bool HasSystemPackage(const char* activationFile, const char* stateName)
	{
		struct stat st;
		if (stat(activationFile, &st) != 0 || st.st_size <= 0
			|| st.st_size > 1024 * 1024) {
			return false;
		}
		char* buffer = (char*)malloc(st.st_size + 1);
		if (buffer == NULL)
			return false;
		MemoryDeleter bufferDeleter(buffer);
		int fd = _kern_open(-1, activationFile, O_RDONLY, 0);
		if (fd < 0)
			return false;
		ssize_t length = _kern_read(fd, 0, buffer, st.st_size);
		_kern_close(fd);
		if (length <= 0)
			return false;
		buffer[length] = '\0';

		char* line = buffer;
		while (line != NULL && *line != '\0') {
			char* end = strchr(line, '\n');
			if (end != NULL)
				*end = '\0';
			if (strncmp(line, "haiku-", 6) == 0
				|| strcmp(line, "haiku.hpkg") == 0) {
				// in the state's own directory, or in the packages directory
				struct stat st;
				KPath path(directory);
				if (stateName != NULL) {
					path.Append("administrative");
					path.Append(stateName);
				}
				path.Append(line);
				if (stat(path.Path(), &st) == 0)
					return true;
				KPath latest(directory);
				latest.Append(line);
				return stat(latest.Path(), &st) == 0;
			}
			line = end != NULL ? end + 1 : NULL;
		}
		return false;
	}

	bool StateExists(const char* name)
	{
		if (strncmp(name, "state_", 6) != 0 || strchr(name, '/') != NULL)
			return false;
		KPath path(directory);
		if (path.Append("administrative") != B_OK || path.Append(name) != B_OK
			|| path.Append("activated-packages") != B_OK) {
			return false;
		}
		return HasSystemPackage(path.Path(), name);
	}

	bool LatestBootable()
	{
		KPath path(directory);
		if (path.Append("administrative/activated-packages") != B_OK)
			return true;
		struct stat st;
		if (stat(path.Path(), &st) != 0)
			return true;
				// without an activation file packagefs takes every package
		return HasSystemPackage(path.Path(), NULL);
	}

	const char* NewestOldState()
	{
		KPath path(directory);
		if (path.Append("administrative") != B_OK)
			return NULL;
		DIR* dir = opendir(path.Path());
		if (dir == NULL)
			return NULL;
		newest[0] = '\0';
		while (dirent* entry = readdir(dir)) {
			if (strncmp(entry->d_name, "state_", 6) == 0
				&& strcmp(entry->d_name, newest) > 0
				&& StateExists(entry->d_name)) {
				strlcpy(newest, entry->d_name, sizeof(newest));
			}
		}
		closedir(dir);
		return newest[0] != '\0' ? newest : NULL;
	}
};


static void
write_airos_boot_file(const KPath& directory, const char* name,
	const char* content, bool append)
{
	KPath path(directory);
	if (path.Append(name) != B_OK)
		return;

	int fd = _kern_open(-1, path.Path(),
		O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
	if (fd < 0) {
		dprintf("airos boot: cannot write %s: %s\n", name, strerror(fd));
		return;
	}
	_kern_write(fd, -1, content, strlen(content));
	_kern_close(fd);
}


/*!	Records a start the boot loader chose for System Updater (see
	airos_boot_control.h): a start requested once is consumed, a start of an
	update on trial is counted, and "last-boot" says how this start was
	chosen. Done as soon as the boot volume is mounted, so that a start that
	ends in a panic or a hang later on counts too.
	Where the loader did not choose (it could not read the boot volume), the
	kernel does; \a _state is then set to the state to start, if not the
	latest one.
*/
static void
record_airos_boot(const KMessage& bootVolume, const char*& _state,
	char* stateBuffer, size_t stateBufferSize)
{
	if (gReadOnlyBootDevice)
		return;

	KPath directory(kSystemPackagesDirectory);
	if (directory.InitCheck() != B_OK
		|| directory.Append(AIROS_BOOT_CONTROL_DIRECTORY) != B_OK) {
		return;
	}
	struct stat st;
	if (stat(directory.Path(), &st) != 0)
		return;

	const char* mode = bootVolume.GetString(BOOT_VOLUME_AIROS_BOOT, NULL);
	if (mode == NULL && _state == NULL
		&& !bootVolume.GetBool(BOOT_VOLUME_PACKAGED, false)
		&& bootVolume.GetBool(BOOT_VOLUME_BOOTED_FROM_IMAGE, false)) {
		KernelBootControlReader reader;
		airos_boot_mode choice = airos_choose_boot_state(reader, stateBuffer,
			stateBufferSize);
		mode = airos_boot_mode_name(choice);
		if (stateBuffer[0] != '\0')
			_state = stateBuffer;
	}

	if (mode == NULL) {
		// an ordinary start of the latest state
		KPath path(directory);
		if (path.Append(AIROS_BOOT_LAST_FILE) == B_OK)
			_kern_unlink(-1, path.Path());
		return;
	}

	const char* state = _state != NULL ? _state : "";
	char line[B_FILE_NAME_LENGTH + 32];
	snprintf(line, sizeof(line), "%s %s\n", mode, state);
	write_airos_boot_file(directory, AIROS_BOOT_LAST_FILE, line, false);

	if (strcmp(mode, AIROS_BOOT_MODE_NEXT) == 0) {
		KPath from(directory), to(directory);
		if (from.Append(AIROS_BOOT_NEXT_FILE) != B_OK
			|| to.Append(AIROS_BOOT_NEXT_DONE_FILE) != B_OK) {
			return;
		}
		status_t error = _kern_rename(-1, from.Path(), -1, to.Path());
		dprintf("airos boot: started \"%s\" once as requested: %s\n", state,
			strerror(error));
	} else if (strcmp(mode, AIROS_BOOT_MODE_TRIAL) == 0) {
		write_airos_boot_file(directory, AIROS_BOOT_TRIAL_ATTEMPTS_FILE,
			"start\n", true);
		dprintf("airos boot: counted a start of the update on trial\n");
	} else
		dprintf("airos boot: %s start of \"%s\"\n", mode, state);

	// The boot loader reads the file system without replaying its journal,
	// and the start may end in a hang any moment now: write the changes and
	// the metadata they need to their places on the disk. (BFS's fsync()
	// writes a file's data only.)
	_kern_sync();
}


//	#pragma mark -


status_t
vfs_bootstrap_file_systems(void)
{
	status_t status;

	// bootstrap the root filesystem
	status = _kern_mount("/", NULL, "rootfs", 0, NULL, 0);
	if (status < B_OK)
		panic("error mounting rootfs!\n");

	_kern_setcwd(-1, "/");

	// bootstrap the devfs
	_kern_create_dir(-1, "/dev", 0755);
	status = _kern_mount("/dev", NULL, "devfs", 0, NULL, 0);
	if (status < B_OK)
		panic("error mounting devfs\n");

	// create directory for the boot volume
	_kern_create_dir(-1, "/boot", 0755);

	// create some standard links on the rootfs

	for (int32 i = 0; sPredefinedLinks[i].path != NULL; i++) {
		_kern_create_symlink(-1, sPredefinedLinks[i].path,
			sPredefinedLinks[i].target, 0777);
			// we don't care if it will succeed or not
	}

	return B_OK;
}


// How long to keep rescanning for the boot device before giving up, and how
// long to wait between passes.
static const bigtime_t kBootDeviceTimeout = 10000000;
static const bigtime_t kBootDeviceRetryInterval = 250000;

void
vfs_mount_boot_file_system(kernel_args* args)
{
	KMessage bootVolume;
	bootVolume.SetTo(args->boot_volume, args->boot_volume_size);

	PartitionStack partitions;
	status_t status = B_OK;

	// Some buses publish their disks from a worker thread rather than from
	// register_child_devices(), so the boot device can appear after the first
	// scan: the MMC bus manager initialises the card and registers its node
	// on MMCBus::_WorkerThread. Rescan until the boot device shows up rather
	// than giving up on the first pass; a device that is already there is
	// still found immediately.
	const bigtime_t deadline = system_time() + kBootDeviceTimeout;
	while (true) {
		status = get_boot_partitions(bootVolume, partitions);
		if (status < B_OK)
			panic("get_boot_partitions failed!");
		if (!partitions.IsEmpty())
			break;
		if (system_time() >= deadline) {
			panic("did not find any boot partitions! @! syslog | tail 15");
			break;
		}
		snooze(kBootDeviceRetryInterval);
	}

	dev_t bootDevice = -1;

	KPartition* bootPartition;
	while (partitions.Pop(&bootPartition)) {
		KPath path;
		if (bootPartition->GetPath(&path) != B_OK)
			panic("could not get boot device!\n");

		const char* fsName = NULL;
		bool readOnly = false;
		uint32 mountFlags = 0;
		if (strcmp(bootPartition->ContentType(), kPartitionTypeISO9660) == 0) {
			fsName = "iso9660:write_overlay:attribute_overlay";
			readOnly = true;
		} else if (bootPartition->IsReadOnly()
			&& strcmp(bootPartition->ContentType(), kPartitionTypeBFS) == 0) {
			fsName = "bfs:write_overlay";
			readOnly = true;
		} else if (strcmp(bootPartition->ContentType(), kPartitionTypeBFS) == 0
			&& is_hybrid_iso_partition(bootPartition)) {
			// BFS itself read-only, the overlay above it writable
			fsName = "bfs:write_overlay";
			readOnly = true;
			mountFlags = B_MOUNT_READ_ONLY;
			dprintf("Boot partition is on a hybrid ISO image, using it "
				"read-only.\n");
		}

		TRACE(("trying to mount boot partition: %s\n", path.Path()));

		bootDevice = _kern_mount("/boot", path.Path(), fsName, mountFlags,
			NULL, 0);
		if (bootDevice >= 0) {
			dprintf("Mounted boot partition: %s\n", path.Path());
			gReadOnlyBootDevice = readOnly;
			break;
		}
	}

	if (bootDevice < B_OK)
		panic("could not mount boot device!\n");

	// create link for the name of the boot device

	fs_info info;
	if (_kern_read_fs_info(bootDevice, &info) == B_OK) {
		char path[B_FILE_NAME_LENGTH + 1];
		snprintf(path, sizeof(path), "/%s", info.volume_name);

		_kern_create_symlink(-1, path, "/boot", 0777);
	}

	// the state System Updater asked for (airos_boot_control.h)
	const char* stateName = bootVolume.GetString(BOOT_VOLUME_PACKAGES_STATE,
		NULL);
	char chosenState[B_FILE_NAME_LENGTH];
	chosenState[0] = '\0';
	record_airos_boot(bootVolume, stateName, chosenState, sizeof(chosenState));

	// If we're booting off a packaged system, mount packagefs.
	struct stat st;
	if (bootVolume.GetBool(BOOT_VOLUME_PACKAGED, false)
		|| (bootVolume.GetBool(BOOT_VOLUME_BOOTED_FROM_IMAGE, false)
			&& lstat(kSystemPackagesDirectory, &st) == 0)) {
		static const char* const kPackageFSName = "packagefs";

		char arguments[256];
		strlcpy(arguments, "packages /boot/system/packages; type system",
			sizeof(arguments));
		if (stateName != NULL) {
			strlcat(arguments, "; state ", sizeof(arguments));
			strlcat(arguments, stateName, sizeof(arguments));
		}

		dev_t packageMount = _kern_mount("/boot/system", NULL, kPackageFSName,
			0, arguments, 0 /* unused argument length */);
		if (packageMount < 0) {
			panic("Failed to mount system packagefs: %s",
				strerror(packageMount));
		}

		packageMount = _kern_mount("/boot/home/config", NULL, kPackageFSName, 0,
			"packages /boot/home/config/packages; type home",
			0 /* unused argument length */);
		if (packageMount < 0) {
			dprintf("Failed to mount home packagefs: %s\n",
				strerror(packageMount));
		}
	}

	// Now that packagefs is mounted, the boot volume is really ready.
	gBootDevice = bootDevice;

	// Do post-boot-volume module initialization. The module code wants to know
	// whether the module images the boot loader has pre-loaded are the same as
	// on the boot volume. That is the case when booting from hard disk or CD,
	// but not via network.
	int32 bootMethodType = bootVolume.GetInt32(BOOT_METHOD, BOOT_METHOD_DEFAULT);
	bool bootingFromBootLoaderVolume = bootMethodType == BOOT_METHOD_HARD_DISK
		|| bootMethodType == BOOT_METHOD_CD;
	module_init_post_boot_device(bootingFromBootLoaderVolume);

	file_cache_init_post_boot_device();

	// search for other disk systems
	KDiskDeviceManager *manager = KDiskDeviceManager::Default();
	manager->RescanDiskSystems();
	manager->StartMonitoring();
}

