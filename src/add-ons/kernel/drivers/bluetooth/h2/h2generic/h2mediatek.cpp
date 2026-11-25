/*
 * Firmware for MediaTek Bluetooth radios.
 *
 * A MT7922 and its relatives come out of reset running a bootloader rather
 * than a Bluetooth controller. The bootloader answers reads of its own
 * registers and its own download protocol, and nothing else: send it the HCI
 * Reset that every Bluetooth stack opens with and it will take the packet and
 * never reply, leaving the stack waiting on an answer that is not coming.
 *
 * So before the stack is told there is an adapter here, the radio is handed
 * its firmware. The vendor's image is read from disk, and the sections of it
 * meant for the radio are fed over in small pieces wrapped in MediaTek's WMT
 * protocol, which travels inside a vendor HCI command. When the last piece is
 * in, the radio is told to stop being a bootloader.
 *
 * The commands go out the way all HCI commands go out on USB, as class control
 * transfers on endpoint zero. The answers are collected by a vendor control
 * read rather than from the event endpoint, and that read returns nothing at
 * all until the radio has something to say.
 *
 * Distributed under the terms of the MIT License.
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <FindDirectory.h>
#include <StorageDefs.h>

#include "h2generic.h"
#include "h2debug.h"
#include "h2mediatek.h"


/* Registers in the bootloader's own window. */
#define MTK_REG_LEGACY_ID		0x80000008
#define MTK_REG_CHIP_ID			0x70010200
#define MTK_REG_CHIP_REV		0x70010204
#define MTK_REG_FW_VERSION		0x80021004
#define MTK_REG_EP_RST_OPT		0x74011890
#define MTK_EP_RST_IN_OUT_OPT		0x00010001

#define MTK_LEGACY_PART			0x7663
	/* An older part that loads a different way; left alone here. */

#define MTK_VENDOR_READ			0x63	/* with request type 0xc0 */
#define MTK_RESET_OPTION_WRITE		0x02	/* with request type 0x5e */
#define MTK_WMT_READ			0x01	/* with type 0xc0, value 48 */
#define MTK_WMT_READ_VALUE		48

#define USB_VENDOR_IN			0xc0
#define USB_RESET_OPTION_OUT		0x5e
#define USB_CLASS_OUT			0x20

#define HCI_OPCODE_WMT			0xfc6f
#define HCI_EVENT_WMT			0xe4

#define WMT_PATCH_DOWNLOAD		0x01
#define WMT_FUNCTION_CONTROL		0x06

/* A section whose type's low half is this carries code for the radio. */
#define SECTION_TYPE_BIN		0x0002

#define PATCH_HEADER_SIZE		32
#define GLOBAL_DESC_SIZE		64
#define SECTION_MAP_SIZE		64
#define SECTION_MAP_COMMON		12
	/* Type, offset and size stay behind; the rest is for the radio. */
#define SECTION_HEADER_BYTES		52

#define SECTION_COUNT_OFFSET		0x2c
#define HARDWARE_VERSION_OFFSET		0x14

#define CHUNK_SIZE			250
	/* All that an HCI packet's single length byte leaves room for. */

#define WMT_REPLY_MAX			64
#define WMT_POLL_GAP			500		/* microseconds */
#define WMT_TIMEOUT			10000000	/* ten seconds */

#define MAX_FIRMWARE_SIZE		(4 * 1024 * 1024)
#define MAX_SECTIONS			16

#define FIRMWARE_DIRECTORY		"/firmware/h2generic/"


static uint32
read_le32(const uint8* bytes)
{
	return bytes[0] | (bytes[1] << 8) | (bytes[2] << 16)
		| ((uint32)bytes[3] << 24);
}


static status_t
read_register(bt_usb_dev* bdev, uint32 reg, uint32* value)
{
	uint8 buffer[4] = { 0, 0, 0, 0 };
	size_t actual = 0;

	status_t status = usb->send_request(bdev->dev, USB_VENDOR_IN,
		MTK_VENDOR_READ, reg >> 16, reg & 0xffff, sizeof(buffer), buffer,
		&actual);

	if (status != B_OK || actual != sizeof(buffer))
		return status != B_OK ? status : B_IO_ERROR;

	*value = read_le32(buffer);
	return B_OK;
}


/* The endpoint reset option lives behind a second control channel whose
 * request types are not any standard combination. The bytes go out as given.
 */
static status_t
write_reset_option(bt_usb_dev* bdev, uint32 reg, uint32 value)
{
	uint8 buffer[4];
	size_t actual = 0;

	buffer[0] = value & 0xff;
	buffer[1] = (value >> 8) & 0xff;
	buffer[2] = (value >> 16) & 0xff;
	buffer[3] = (value >> 24) & 0xff;

	return usb->send_request(bdev->dev, USB_RESET_OPTION_OUT,
		MTK_RESET_OPTION_WRITE, reg >> 16, reg & 0xffff, sizeof(buffer),
		buffer, &actual);
}


/* One WMT command. It is an HCI command whose payload is a WMT header and
 * then the command's own data:
 *
 *   6f fc <plen> | <dir> <op> <dlen lo> <dlen hi> <flag> | <data...>
 *
 * where plen counts everything after it and dlen counts the flag byte and the
 * data. The two lengths overlap, which is easy to get wrong.
 */
static status_t
send_wmt(bt_usb_dev* bdev, uint8 op, uint8 flag, const uint8* data,
	size_t dataLength)
{
	uint8 packet[3 + 5 + CHUNK_SIZE];
	size_t total = 8 + dataLength;
	size_t actual = 0;

	if (dataLength > CHUNK_SIZE)
		return B_BAD_VALUE;

	packet[0] = HCI_OPCODE_WMT & 0xff;
	packet[1] = HCI_OPCODE_WMT >> 8;
	packet[2] = (uint8)(5 + dataLength);
	packet[3] = 0x01;				/* host to device */
	packet[4] = op;
	packet[5] = (uint8)((dataLength + 1) & 0xff);
	packet[6] = (uint8)((dataLength + 1) >> 8);
	packet[7] = flag;

	if (dataLength > 0)
		memcpy(packet + 8, data, dataLength);

	return usb->send_request(bdev->dev, USB_CLASS_OUT, 0x00, 0, 0,
		(uint16)total, packet, &actual);
}


/* Fetch one WMT event, if the radio has one: true with the event in buffer,
 * false when it handed back nothing or a buffer it has not written an event
 * into (its "not yet").
 */
static bool
fetch_wmt(bt_usb_dev* bdev, uint8* buffer, status_t* _status)
{
	size_t actual = 0;
	memset(buffer, 0, WMT_REPLY_MAX);

	*_status = usb->send_request(bdev->dev, USB_VENDOR_IN, MTK_WMT_READ,
		MTK_WMT_READ_VALUE, 0, WMT_REPLY_MAX, buffer, &actual);
	return *_status == B_OK && actual >= 7 && buffer[0] == HCI_EVENT_WMT;
}


/* The radio keeps its answers queued until they are read, also across a
 * device_open() that gave up and a warm reboot. An answer left over from such
 * an exchange would be taken for the answer to our first command, and every
 * answer after it for the command before. So what is queued before we start
 * is read and dropped.
 */
static void
drain_wmt(bt_usb_dev* bdev)
{
	uint8 buffer[WMT_REPLY_MAX];
	status_t status;

	for (int i = 0; i < 16 && fetch_wmt(bdev, buffer, &status); i++) {
		ERROR("%s: dropped an earlier answer for WMT %#x (flag %#x)\n",
			__func__, buffer[3], buffer[6]);
	}
}


/* Collect the answer to the last command. The radio says "not yet" by handing
 * back nothing, or by handing back a buffer it has not written an event into,
 * which it does while it is finishing a section. An event for another
 * operation is not the answer: it is late, from an exchange given up on
 * before, or one the radio sent by itself. It is skipped, and the answer is
 * still to come.
 */
static status_t
read_wmt(bt_usb_dev* bdev, uint8 expectedOp, uint8* outFlag)
{
	uint8 buffer[WMT_REPLY_MAX];
	bigtime_t deadline = system_time() + WMT_TIMEOUT;

	while (system_time() < deadline) {
		status_t status;
		if (!fetch_wmt(bdev, buffer, &status)) {
			if (status != B_OK)
				return status;
			snooze(WMT_POLL_GAP);
			continue;
		}

		if (buffer[3] != expectedOp) {
			ERROR("%s: skipped an event for WMT %#x (%02x %02x %02x %02x %02x"
				" %02x) while waiting for %#x\n", __func__, buffer[3],
				buffer[1], buffer[2], buffer[3], buffer[4], buffer[5],
				buffer[6], expectedOp);
			continue;
		}

		*outFlag = buffer[6];
		return B_OK;
	}

	ERROR("%s: the radio never answered WMT %#x\n", __func__, expectedOp);
	return B_TIMED_OUT;
}


static status_t
do_wmt(bt_usb_dev* bdev, uint8 op, uint8 flag, const uint8* data,
	size_t dataLength, uint8* outFlag)
{
	status_t status = send_wmt(bdev, op, flag, data, dataLength);
	if (status != B_OK)
		return status;

	return read_wmt(bdev, op, outFlag);
}


/* Offer one section to the radio. It may answer that it already holds this
 * one, and then its contents are not sent at all - which is what makes coming
 * back to an already running radio cheap.
 */
static status_t
download_section(bt_usb_dev* bdev, const uint8* image, size_t imageSize,
	uint32 index)
{
	size_t mapOffset = PATCH_HEADER_SIZE + GLOBAL_DESC_SIZE
		+ index * SECTION_MAP_SIZE;
	const uint8* map = image + mapOffset;

	uint32 type = read_le32(map);
	uint32 offset = read_le32(map + 4);
	uint32 size = read_le32(map + 8);
	uint32 loadSize = read_le32(map + 16);

	if ((type & 0xffff) != SECTION_TYPE_BIN || loadSize == 0)
		return B_OK;

	if ((uint64)offset + size > imageSize || loadSize > size) {
		ERROR("%s: section %" B_PRIu32 " runs past the end of the image\n",
			__func__, index);
		return B_BAD_DATA;
	}

	/* The section's own descriptor goes first, and the answer to it says
	 * whether the section is wanted. A zero leads it, choosing plain
	 * download mode.
	 */
	uint8 header[1 + SECTION_HEADER_BYTES];
	header[0] = 0;
	memcpy(header + 1, map + SECTION_MAP_COMMON, SECTION_HEADER_BYTES);

	uint8 flag = 0;
	int retries = 20;

	while (true) {
		status_t status = do_wmt(bdev, WMT_PATCH_DOWNLOAD, 0, header,
			sizeof(header), &flag);
		if (status != B_OK) {
			ERROR("%s: offering section %" B_PRIu32 ": %s\n", __func__, index,
				strerror(status));
			return status;
		}

		/* One means it is still busy with what it was given before. */
		if (flag != 1)
			break;

		if (--retries <= 0) {
			ERROR("%s: section %" B_PRIu32 " never settled\n", __func__,
				index);
			return B_TIMED_OUT;
		}
		snooze(100000);
	}

	if (flag == 2) {
		TRACE("%s: the radio already holds section %" B_PRIu32 "\n", __func__,
			index);
		return B_OK;
	}

	const uint8* data = image + offset;
	uint32 remaining = loadSize;
	bool first = true;

	while (remaining > 0) {
		uint32 length = remaining < CHUNK_SIZE ? remaining : CHUNK_SIZE;
		uint8 chunkFlag;

		/* First, middle, last. A section that fits in one piece is still
		 * marked first, which is what the radio expects.
		 */
		if (first) {
			chunkFlag = 1;
			first = false;
		} else if (remaining - length == 0)
			chunkFlag = 3;
		else
			chunkFlag = 2;

		uint8 answer = 0;
		status_t status = do_wmt(bdev, WMT_PATCH_DOWNLOAD, chunkFlag, data,
			length, &answer);
		if (status != B_OK) {
			ERROR("%s: sending section %" B_PRIu32 ": %s\n", __func__, index,
				strerror(status));
			return status;
		}
		if (answer == 1) {
			ERROR("%s: the radio stalled partway through section %" B_PRIu32
				"\n", __func__, index);
			return B_IO_ERROR;
		}

		data += length;
		remaining -= length;
	}

	return B_OK;
}


static status_t
load_image(const char* name, uint8** _image, size_t* _size)
{
	char path[B_PATH_NAME_LENGTH];
	directory_which places[] = { B_SYSTEM_NONPACKAGED_DATA_DIRECTORY,
		B_SYSTEM_DATA_DIRECTORY };
	int fd = -1;

	for (size_t i = 0; i < B_COUNT_OF(places); i++) {
		if (find_directory(places[i], -1, false, path, sizeof(path)) != B_OK)
			continue;

		strlcat(path, FIRMWARE_DIRECTORY, sizeof(path));
		strlcat(path, name, sizeof(path));

		fd = open(path, B_READ_ONLY);
		if (fd >= 0)
			break;
	}

	if (fd < 0) {
		ERROR("%s: no %s to be found\n", __func__, name);
		return B_ENTRY_NOT_FOUND;
	}

	off_t size = lseek(fd, 0, SEEK_END);
	if (size <= 0 || size > MAX_FIRMWARE_SIZE) {
		ERROR("%s: %s is %" B_PRIdOFF " bytes, which cannot be right\n",
			__func__, path, size);
		close(fd);
		return B_BAD_DATA;
	}
	lseek(fd, 0, SEEK_SET);

	uint8* image = (uint8*)malloc(size);
	if (image == NULL) {
		close(fd);
		return B_NO_MEMORY;
	}

	ssize_t read_bytes = read(fd, image, size);
	close(fd);

	if (read_bytes != (ssize_t)size) {
		ERROR("%s: could only read %" B_PRIdSSIZE " of %" B_PRIdOFF
			" bytes of %s\n", __func__, read_bytes, size, path);
		free(image);
		return B_IO_ERROR;
	}

	TRACE("%s: read %s\n", __func__, path);

	*_image = image;
	*_size = (size_t)size;
	return B_OK;
}


/* The USB vendors of MediaTek based modules in Linux btusb (BTUSB_MEDIATEK).
 * Only these are asked for MediaTek registers; an Intel or Realtek radio is
 * never sent a MediaTek vendor request.
 */
static const uint16 kMediaTekModuleVendors[] = {
	0x043e,		/* LG */
	0x0489,		/* Foxconn */
	0x04ca,		/* Lite-On */
	0x0e8d,		/* MediaTek */
	0x13d3,		/* IMC Networks / AzureWave */
	0x2c7c,		/* Quectel */
	0x35f5,
};


static bool
may_be_mediatek(bt_usb_dev* bdev)
{
	const usb_device_descriptor* descriptor
		= usb->get_device_descriptor(bdev->dev);
	if (descriptor == NULL)
		return false;

	for (size_t i = 0; i < B_COUNT_OF(kMediaTekModuleVendors); i++) {
		if (descriptor->vendor_id == kMediaTekModuleVendors[i])
			return true;
	}
	return false;
}


status_t
mediatek_setup(bt_usb_dev* bdev)
{
	uint32 legacyId = 0;
	uint32 chipId = 0;
	uint32 firmwareVersion = 0;

	if (!may_be_mediatek(bdev))
		return B_OK;

	/* Ask the bootloader who it is. A module from these vendors built on
	 * another chip will not answer this at all, which is how we tell them
	 * apart without keeping a list of every part number ever sold.
	 */
	if (read_register(bdev, MTK_REG_CHIP_ID, &chipId) != B_OK)
		return B_OK;

	if ((chipId & 0xffff0000) != 0 || (chipId & 0xffff) == 0) {
		TRACE("%s: %#" B_PRIx32 " is not a part number we know\n", __func__,
			chipId);
		return B_OK;
	}

	if (read_register(bdev, MTK_REG_LEGACY_ID, &legacyId) == B_OK
		&& legacyId == MTK_LEGACY_PART) {
		ERROR("%s: an MT%04" B_PRIx32 " loads differently and is not handled"
			"\n", __func__, legacyId);
		return B_OK;
	}

	if (read_register(bdev, MTK_REG_FW_VERSION, &firmwareVersion) != B_OK)
		return B_OK;

	/* The radio names the file it wants: its part number, and a revision
	 * that counts from one where the register counts from zero.
	 */
	char name[64];
	snprintf(name, sizeof(name), "BT_RAM_CODE_MT%04" B_PRIx32 "_1_%" B_PRIx32
		"_hdr.bin", chipId & 0xffff, (firmwareVersion & 0xff) + 1);

	uint8* image = NULL;
	size_t imageSize = 0;
	status_t status = load_image(name, &image, &imageSize);
	if (status != B_OK) {
		ERROR("%s: an MT%04" B_PRIx32 " needs %s, which is not installed."
			" Bluetooth will not work without it.\n", __func__,
			chipId & 0xffff, name);
		return status;
	}

	if (imageSize < PATCH_HEADER_SIZE + GLOBAL_DESC_SIZE + SECTION_MAP_SIZE) {
		ERROR("%s: %s is too small to be a firmware image\n", __func__, name);
		free(image);
		return B_BAD_DATA;
	}

	/* The image says which revision of the part it was built for, and the
	 * part says which it is. Handing a radio the wrong firmware is not worth
	 * finding out about afterwards.
	 */
	uint16 imageHardware = image[HARDWARE_VERSION_OFFSET]
		| (image[HARDWARE_VERSION_OFFSET + 1] << 8);
	uint32 revision = 0;

	if (read_register(bdev, MTK_REG_CHIP_REV, &revision) == B_OK
		&& imageHardware != ((revision >> 8) & 0xff)) {
		ERROR("%s: %s is for hardware %#x, this radio is %#x\n", __func__,
			name, imageHardware, (unsigned)((revision >> 8) & 0xff));
		free(image);
		return B_BAD_DATA;
	}

	uint32 sectionCount = read_le32(image + SECTION_COUNT_OFFSET);

	if (sectionCount == 0 || sectionCount > MAX_SECTIONS
		|| PATCH_HEADER_SIZE + GLOBAL_DESC_SIZE
			+ sectionCount * SECTION_MAP_SIZE > imageSize) {
		ERROR("%s: %s says it has %" B_PRIu32 " sections, which cannot be"
			" right\n", __func__, name, sectionCount);
		free(image);
		return B_BAD_DATA;
	}

	TRACE("%s: MT%04" B_PRIx32 ", firmware for hardware %#x in %" B_PRIu32
		" sections\n", __func__, chipId & 0xffff, imageHardware, sectionCount);

	bigtime_t started = system_time();

	drain_wmt(bdev);

	for (uint32 i = 0; i < sectionCount; i++) {
		status = download_section(bdev, image, imageSize, i);
		if (status != B_OK) {
			free(image);
			return status;
		}
	}

	free(image);

	/* Let what was sent settle before asking the radio to use it. */
	snooze(100000);

	status = write_reset_option(bdev, MTK_REG_EP_RST_OPT,
		MTK_EP_RST_IN_OUT_OPT);
	if (status != B_OK) {
		ERROR("%s: the radio would not take the endpoint reset option: %s\n",
			__func__, strerror(status));
		return status;
	}

	/* And now: stop being a bootloader, start being a Bluetooth controller. */
	uint8 on = 1;
	uint8 answer = 0;
	status = do_wmt(bdev, WMT_FUNCTION_CONTROL, 0, &on, 1, &answer);
	if (status != B_OK) {
		ERROR("%s: the radio would not turn Bluetooth on: %s\n", __func__,
			strerror(status));
		return status;
	}

	ERROR("%s: MT%04" B_PRIx32 " ready in %" B_PRIdBIGTIME " ms\n", __func__,
		chipId & 0xffff, (system_time() - started) / 1000);

	return B_OK;
}
