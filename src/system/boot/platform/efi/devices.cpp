/*
 * Copyright 2016-2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include <boot/partitions.h>
#include <boot/platform.h>
#include <boot/stage2.h>

#include <boot/net/Ethernet.h>
#include <boot/net/NetStack.h>
#include <boot/net/RemoteDisk.h>

#include <boot/stdio.h>

#include "Header.h"
#include "device_path_match.h"

#include "efi_platform.h"
#include <efi/protocol/block-io.h>
#include <efi/protocol/device-path-to-text.h>
#include <efi/protocol/loaded-image.h>

#include "gpt.h"
#include "gpt_known_guids.h"


//#define TRACE_DEVICES
#ifdef TRACE_DEVICES
#   define TRACE(x...) dprintf("efi/devices: " x)
#else
#   define TRACE(x...) ;
#endif


static efi_guid BlockIoGUID = EFI_BLOCK_IO_PROTOCOL_GUID;
static efi_guid sLoadedImageGUID = EFI_LOADED_IMAGE_PROTOCOL_GUID;
static efi_guid sDevicePathGUID = EFI_DEVICE_PATH_PROTOCOL_GUID;
static efi_guid sDevicePathToTextGUID = EFI_DEVICE_PATH_TO_TEXT_PROTOCOL_GUID;


// Diagnostic build: prints a device path, as text if the firmware can convert
// it, else as its nodes (type/subtype/length).
static void
diag_print_device_path(const char* label, efi_device_path_protocol* path)
{
	if (path == NULL) {
		dprintf("diag:   %s: (none)\n", label);
		return;
	}

	static efi_device_path_to_text_protocol* toText = NULL;
	static bool lookedUp = false;
	if (!lookedUp) {
		lookedUp = true;
		if (kBootServices->LocateProtocol(&sDevicePathToTextGUID, NULL,
				(void**)&toText) != EFI_SUCCESS)
			toText = NULL;
	}

	if (toText != NULL) {
		char16_t* text = toText->ConvertDevicePathToText(path, false, false);
		if (text != NULL) {
			char buffer[400];
			size_t i = 0;
			for (; text[i] != 0 && i < sizeof(buffer) - 1; i++)
				buffer[i] = text[i] < 0x80 ? (char)text[i] : '?';
			buffer[i] = '\0';
			kBootServices->FreePool(text);
			dprintf("diag:   %s: %s\n", label, buffer);
			return;
		}
	}

	char buffer[400];
	size_t length = 0;
	const uint8* node = (const uint8*)path;
	for (int i = 0; i < 24 && length < sizeof(buffer) - 16; i++) {
		uint16 nodeLength = node[2] | (node[3] << 8);
		length += snprintf(buffer + length, sizeof(buffer) - length,
			"%02x/%02x/%u ", node[0], node[1], nodeLength);
		if (node[0] == 0x7f || nodeLength < 4)
			break;
		node += nodeLength;
	}
	buffer[length] = '\0';
	dprintf("diag:   %s (nodes): %s\n", label, buffer);
}


class EfiDevice : public Node
{
	public:
		EfiDevice(efi_block_io_protocol *blockIo);
		virtual ~EfiDevice();

		virtual ssize_t ReadAt(void *cookie, off_t pos, void *buffer,
			size_t bufferSize);
		virtual ssize_t WriteAt(void *cookie, off_t pos, const void *buffer,
			size_t bufferSize) { return B_UNSUPPORTED; }
		virtual off_t Size() const {
			return (fBlockIo->Media->LastBlock + 1) * BlockSize(); }

		uint32 BlockSize() const { return fBlockIo->Media->BlockSize; }
	private:
		efi_block_io_protocol*		fBlockIo;
};


EfiDevice::EfiDevice(efi_block_io_protocol *blockIo)
	:
	fBlockIo(blockIo)
{
}


EfiDevice::~EfiDevice()
{
}


ssize_t
EfiDevice::ReadAt(void *cookie, off_t pos, void *buffer, size_t bufferSize)
{
	TRACE("%s called. pos: %" B_PRIdOFF ", %p, %" B_PRIuSIZE "\n", __func__,
		pos, buffer, bufferSize);

	off_t offset = pos % BlockSize();
	pos /= BlockSize();

	uint32 numBlocks = (offset + bufferSize + BlockSize() - 1) / BlockSize();

	// TODO: We really should implement memalign and align all requests to
	// fBlockIo->Media->IoAlign. This static alignment is large enough though
	// to catch most required alignments.
	char readBuffer[numBlocks * BlockSize()]
		__attribute__((aligned(2048)));

	efi_status status = fBlockIo->ReadBlocks(fBlockIo,
		fBlockIo->Media->MediaId, pos, sizeof(readBuffer), readBuffer);
	if (status != EFI_SUCCESS) {
		static int errors = 0;
		if (errors++ < 32) {
			dprintf("%s: blockIo error reading from device! blockIo %p, "
				"status %#" B_PRIx64 ", LBA %" B_PRIdOFF ", %" B_PRIuSIZE
				" bytes, buffer %p, media id %" B_PRIu32 ", present %d, "
				"IoAlign %" B_PRIu32 "\n", __func__, fBlockIo, (uint64)status,
				pos, sizeof(readBuffer), readBuffer, fBlockIo->Media->MediaId,
				fBlockIo->Media->MediaPresent, fBlockIo->Media->IoAlign);
		}
		return B_ERROR;
	}

	memcpy(buffer, readBuffer + offset, bufferSize);

	return bufferSize;
}


static bool
device_contains_partition(EfiDevice *device, boot::Partition *partition)
{
	EFI::Header *header = (EFI::Header*)partition->content_cookie;
	if (header != NULL && header->InitCheck() == B_OK) {
		// check if device is GPT, and contains partition entry
		uint32 blockSize = device->BlockSize();
		gpt_table_header *deviceHeader =
			(gpt_table_header*)malloc(blockSize);
		ssize_t bytesRead = device->ReadAt(NULL, blockSize, deviceHeader,
			blockSize);
		if (bytesRead != (ssize_t)blockSize) {
			dprintf("diag: device %p, GPT partition %p: reading the header "
				"failed\n", device, partition);
			return false;
		}

		if (memcmp(deviceHeader, &header->TableHeader(),
				sizeof(gpt_table_header)) != 0) {
			dprintf("diag: device %p, GPT partition %p: on another disk\n",
				device, partition);
			return false;
		}

		// partition->cookie == int partition entry index
		uint32 index = (uint32)(addr_t)partition->cookie;
		uint32 size = sizeof(gpt_partition_entry) * (index + 1);
		gpt_partition_entry *entries = (gpt_partition_entry*)malloc(size);
		bytesRead = device->ReadAt(NULL,
			deviceHeader->entries_block * blockSize, entries, size);
		if (bytesRead != (ssize_t)size)
			return false;

		if (memcmp(&entries[index], &header->EntryAt(index),
				sizeof(gpt_partition_entry)) != 0)
			return false;

		for (size_t i = 0; i < sizeof(kTypeMap) / sizeof(struct type_map); ++i)
			if (strcmp(kTypeMap[i].type, BFS_NAME) == 0)
				if (kTypeMap[i].guid == header->EntryAt(index).partition_type)
					return true;

		// Our partition has an EFI header, but we couldn't find one, so bail
		dprintf("diag: device %p, GPT partition %p: not BFS\n", device,
			partition);
		return false;
	}

	if ((partition->offset + partition->size) <= device->Size()) {
		dprintf("diag: device %p: partition %p (offset %" B_PRIdOFF ", size %"
			B_PRIdOFF ") is a boot partition candidate\n", device, partition,
			partition->offset, partition->size);
		return true;
	}

	dprintf("diag: device %p: partition %p (offset %" B_PRIdOFF ", size %"
		B_PRIdOFF ") ends past the device (%" B_PRIdOFF ")\n", device, partition,
		partition->offset, partition->size, device->Size());
	return false;
}


status_t
platform_add_boot_device(struct stage2_args *args, NodeList *devicesList)
{
	TRACE("%s: called\n", __func__);

	efi_block_io_protocol *blockIo;
	size_t memSize = 0;

	// If the bootloader was started from the network (net_stack_init will find
	// the ip= parameter in the LoadOptions), add network booting as the first
	// entry if available
	status_t error = net_stack_init();
	if (error != B_OK) {
		TRACE("Can't init network...\n");
	} else {
		TRACE("Network is initialized! Search for remote disk...\n");
		RemoteDisk *remoteDisk = RemoteDisk::FindAnyRemoteDisk();
		if (remoteDisk != NULL) {
			devicesList->Add(remoteDisk);
		}
	}

	// DeviceHandle identifies the volume from which EFI loaded this image.
	// Its device path extends the path of the containing whole disk.
	efi_loaded_image_protocol* loadedImage = NULL;
	efi_handle imageDevice = NULL;
	efi_device_path_protocol* imagePath = NULL;
	if (kBootServices->HandleProtocol(kImage, &sLoadedImageGUID,
			(void**)&loadedImage) == EFI_SUCCESS) {
		imageDevice = loadedImage->DeviceHandle;
		if (kBootServices->HandleProtocol(imageDevice, &sDevicePathGUID,
				(void**)&imagePath) != EFI_SUCCESS)
			imagePath = NULL;
	}
	EfiDevice* firstDisk = NULL;
	EfiDevice* preferredDisk = NULL;
	size_t bestPrefixLength = 0;

	{
		char vendor[64];
		size_t i = 0;
		for (; kSystemTable->FirmwareVendor != NULL
				&& kSystemTable->FirmwareVendor[i] != 0
				&& i < sizeof(vendor) - 1; i++) {
			vendor[i] = kSystemTable->FirmwareVendor[i] < 0x80
				? (char)kSystemTable->FirmwareVendor[i] : '?';
		}
		vendor[i] = '\0';
		dprintf("diag: firmware \"%s\" revision %#" B_PRIx32 ", UEFI %#"
			B_PRIx32 "\n", vendor, kSystemTable->FirmwareRevision,
			kSystemTable->Hdr.Revision);
	}
	dprintf("diag: loader started from device handle %p\n", imageDevice);
	diag_print_device_path("device", imagePath);
	diag_print_device_path("file", loadedImage != NULL
		? loadedImage->FilePath : NULL);

	// Read to zero sized buffer to get memory needed for handles
	if (kBootServices->LocateHandle(ByProtocol, &BlockIoGUID, 0, &memSize, 0)
			!= EFI_BUFFER_TOO_SMALL)
		panic("Cannot read size of block device handles!");

	uint32 noOfHandles = memSize / sizeof(efi_handle);

	efi_handle handles[noOfHandles];
	if (kBootServices->LocateHandle(ByProtocol, &BlockIoGUID, 0, &memSize,
			handles) != EFI_SUCCESS)
		panic("Failed to locate block devices!");

	// All block devices has one for the disk and one per partition
	// There is a special case for a device with one fixed partition
	// But we probably do not care about booting on that kind of device
	// So find all disk block devices and let Haiku do partition scan
	for (uint32 n = 0; n < noOfHandles; n++) {
		if (kBootServices->HandleProtocol(handles[n], &BlockIoGUID,
				(void**)&blockIo) != EFI_SUCCESS)
			panic("Cannot get block device handle!");

		dprintf("diag: block device %" B_PRIu32 "/%" B_PRIu32 ": handle %p, "
			"blockIo %p, rev %#" B_PRIx64 ", present %d, logical %d, "
			"removable %d, read-only %d, block size %" B_PRIu32 ", last block %"
			B_PRIu64 ", IoAlign %" B_PRIu32 ", media id %" B_PRIu32 "\n",
			n, noOfHandles, handles[n], blockIo, blockIo->Revision,
			blockIo->Media->MediaPresent, blockIo->Media->LogicalPartition,
			blockIo->Media->RemovableMedia, blockIo->Media->ReadOnly,
			blockIo->Media->BlockSize, blockIo->Media->LastBlock,
			blockIo->Media->IoAlign, blockIo->Media->MediaId);
		{
			efi_device_path_protocol* path = NULL;
			if (kBootServices->HandleProtocol(handles[n], &sDevicePathGUID,
					(void**)&path) != EFI_SUCCESS)
				path = NULL;
			diag_print_device_path("path", path);
		}

		if (!blockIo->Media->MediaPresent
			|| blockIo->Media->LogicalPartition
			|| blockIo->Media->BlockSize == 0) {
			dprintf("diag:   skipped (no media, a partition, or no block "
				"size)\n");
			continue;
		}

		// The qemu flash device with a 256K block sizes sometime show up
		// in edk2. If flash is unconfigured, bad things happen on arm.
		// edk2 bug: https://bugzilla.tianocore.org/show_bug.cgi?id=2856
		// We're not ready for flash devices in efi, so skip anything odd.
		if (blockIo->Media->BlockSize > 8192) {
			dprintf("diag:   skipped (block size)\n");
			continue;
		}

		EfiDevice *device = new(std::nothrow)EfiDevice(blockIo);
		if (device == NULL)
			panic("Can't allocate memory for block devices!");
		devicesList->Insert(device);
		if (firstDisk == NULL)
			firstDisk = device;
		dprintf("diag:   added as device %p (%" B_PRIdOFF " bytes)\n", device,
			device->Size());

		efi_device_path_protocol* diskPath = NULL;
		size_t prefixLength = 0;
		if (handles[n] == imageDevice)
			prefixLength = SIZE_MAX;
		else if (imagePath != NULL && kBootServices->HandleProtocol(handles[n],
				&sDevicePathGUID, (void**)&diskPath) == EFI_SUCCESS)
			prefixLength = efi_device_path_prefix_length(diskPath, imagePath);
		if (prefixLength > bestPrefixLength) {
			bestPrefixLength = prefixLength;
			preferredDisk = device;
			dprintf("EFI boot disk candidate: blockIo=%p, path prefix=%" B_PRIuSIZE "\n",
				blockIo, prefixLength);
		}
	}

	// Try the disk that supplied the loader before probing unrelated devices.
	// Keep an explicitly requested network boot first, and retain disk fallback.
	if (preferredDisk != NULL && preferredDisk != firstDisk) {
		devicesList->Remove(preferredDisk);
		devicesList->InsertBefore(firstDisk, preferredDisk);
	}
	dprintf("diag: %" B_PRId32 " disk(s); the loader's disk: %p, first: %p\n",
		devicesList->Count(), preferredDisk, firstDisk);

	return devicesList->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_add_block_devices(struct stage2_args *args, NodeList *devicesList)
{
	TRACE("%s: called\n", __func__);

	//TODO: Currently we add all in platform_add_boot_device
	return devicesList->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_get_boot_partitions(struct stage2_args *args, Node *bootDevice,
		NodeList *partitions, NodeList *bootPartitions)
{
	NodeIterator iterator = partitions->GetIterator();
	boot::Partition *partition = NULL;
	while ((partition = (boot::Partition*)iterator.Next()) != NULL) {
		if (device_contains_partition((EfiDevice*)bootDevice, partition)) {
			bootPartitions->Insert(partition);
		}
	}

	return bootPartitions->Count() > 0 ? B_OK : B_ENTRY_NOT_FOUND;
}


status_t
platform_register_boot_device(Node *device, disk_identifier* defaultDiskID)
{
	TRACE("%s: called\n", __func__);

	// TODO is there no better way to identify network boot?
	char buffer[11];
	device->GetName(buffer, sizeof(buffer));
	TRACE("Booting from %s\n", buffer);

	if (strcmp(buffer, "RemoteDisk") == 0) {
		TRACE("Booting from network\n");
		RemoteDisk* remoteDisk = static_cast<RemoteDisk*>(device);
		// Tell the kernel we're booting from network and forward the network
		// configuration
		auto ethernetInterface = NetStack::Default()->GetEthernetInterface();
		if (gBootParams.SetInt32(BOOT_METHOD, BOOT_METHOD_NET)
			|| gBootParams.AddInt64("client MAC",
				ethernetInterface->MACAddress().ToUInt64()) != B_OK
			|| gBootParams.AddInt32("client IP",
				ethernetInterface->IPAddress()) != B_OK
			|| gBootParams.AddInt32("server IP", remoteDisk->ServerIPAddress())
				!= B_OK
			|| gBootParams.AddInt32("server port", remoteDisk->ServerPort())
				!= B_OK) {
			return B_NO_MEMORY;
		}
	} else {
		// ...HARD_DISK, as we pick partition and have checksum (no need to use _CD)
		gBootParams.SetInt32(BOOT_METHOD, BOOT_METHOD_HARD_DISK);
	}

	gBootParams.SetData(BOOT_VOLUME_DISK_IDENTIFIER, B_RAW_TYPE,
		defaultDiskID, sizeof(disk_identifier));

	return B_OK;
}


void
platform_cleanup_devices()
{
}
