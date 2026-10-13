/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*	The AIC8800D80 ROM loader, see aic_loader.h. The sequence, the
	addresses and the patch values come from cubie/evidence/wifi/DESIGN.md
	section 1 and bt-notes.md section 3; a host-side dry run of the shipped
	firmware files (loader-trace-d80-u02.txt there) is what it must
	reproduce: 560 messages for a normal U02/U03 chip. */


#include "aic_loader.h"

#include <string.h>


#define LOG(x...) \
	do { if (ops->log != NULL) ops->log(cookie, x); } while (0)

/* the firmware files of a U02/U03 chip in normal mode */
#define FILE_PATCH_TABLE	"fw_patch_table_8800d80_u02.bin"
#define FILE_ADID			"fw_adid_8800d80_u02.bin"
#define FILE_PATCH			"fw_patch_8800d80_u02.bin"
#define FILE_PATCH_EXT		"fw_patch_8800d80_u02_ext"	/* + "<id>.bin" */
#define FILE_WIFI			"fmacfw_8800d80_u02.bin"
#define FILE_WIFI_H			"fmacfw_8800d80_h_u02.bin"

#define WIFI_FIRMWARE_BASE	0x120000
#define ADID_BASE_DEFAULT	0x00201940
#define PATCH_BASE_DEFAULT	0x0020b43c

#define CHIP_REV_U01		0x1

/* the record types of the patch table */
#define TABLE_INF			0
#define TABLE_BTMODE		3
#define TABLE_PWRON			4
#define TABLE_VER			6
#define TABLE_MAX_RECORDS	16

/* The first nine values of the BTMODE record are the host's Bluetooth
   configuration, not the file's: hardware info invalid (1, then -1),
   working mode 0, the Bluetooth mode (sBluetoothMode), HCI over the USB
   mailbox (1; the file says UART), 1500000 baud with flow control, no low
   power mode, and the D80's TX power levels. */
#define BT_CONFIG_MODE		3
static const uint32_t kBluetoothConfig[9] = {
	1, 0xffffffff, 0, AIC_BT_MODE_DEFAULT, 1, 1500000, 1, 0, 0x00006f2f
};
static uint32_t sBluetoothMode = AIC_BT_MODE_DEFAULT;

/* the Wi-Fi firmware's patch block */
#define PATCH_MAGIC			0x48435450	/* "PTCH" */
#define PATCH_MAGIC_2		0x50544348
#define PATCH_OFFSET_MAGIC	0x00
#define PATCH_OFFSET_PAIRS	0x04
#define PATCH_OFFSET_MAGIC_2	0x08
#define PATCH_OFFSET_COUNT	0x0c
#define PATCH_OFFSET_BLOCKS	0x30	/* four block sizes */
#define PAIRS_DEFAULT		0x001d7000

/* {offset from the firmware's configuration base, value}: no 5 GHz radar
   detection, the receive aggregation counter, and the power offsets
   covering the calibration */
static const uint32_t kWifiPatches[][2] = {
	{ 0x00b4, 0xf3010000 },
	{ 0x0170, 0x0001000a },
	{ 0x0188, 0x00000001 },
};
#define WIFI_PATCH_COUNT (sizeof(kWifiPatches) / sizeof(kWifiPatches[0]))


struct loader {
	const struct aic_loader_ops*	ops;
	void*							cookie;
	uint8_t							tx[AIC_MESSAGE_MAX];
	uint8_t							rx[AIC_LOADER_RX_SIZE];
	unsigned						messages;
};

struct table_record {
	uint32_t		type;
	uint32_t		count;
	const uint8_t*	pairs;
};


static void
put16(uint8_t* p, uint16_t value)
{
	p[0] = value & 0xff;
	p[1] = value >> 8;
}


static void
put32(uint8_t* p, uint32_t value)
{
	p[0] = value & 0xff;
	p[1] = (value >> 8) & 0xff;
	p[2] = (value >> 16) & 0xff;
	p[3] = value >> 24;
}


static uint16_t
get16(const uint8_t* p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}


static uint32_t
get32(const uint8_t* p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}


/*	A message on the bus: a 4-byte bus header (12-bit length of what
	follows it, type 0x11, 0), a zero word, then the message header (id,
	destination task, source task, parameter length) and the parameters. */
size_t
aic_build_message(uint8_t* buffer, uint16_t id, uint16_t task,
	const void* parameters, uint16_t parameterLength)
{
	memset(buffer, 0, AIC_MESSAGE_HEADER);
	put16(buffer, (uint16_t)((12 + parameterLength) & 0x0fff));
	buffer[2] = AIC_TYPE_MESSAGE;
	put16(buffer + 8, id);
	put16(buffer + 10, task);
	put16(buffer + 12, AIC_TASK_HOST);
	put16(buffer + 14, parameterLength);
	if (parameterLength > 0)
		memcpy(buffer + AIC_MESSAGE_HEADER, parameters, parameterLength);
	return AIC_MESSAGE_HEADER + parameterLength;
}


/*	Reads until the confirmation \a confirmId arrives. One IN transfer can
	carry several packets, each with a 4-byte header (16-bit length, type):
	messages are padded to 4 bytes, data frames carry 60 bytes of header
	besides their length. A message's parameters start 16 bytes into its
	packet (its header ends with a pattern word). */
static int
wait_confirm(struct loader* loader, uint16_t confirmId, uint8_t* confirm,
	size_t confirmCapacity)
{
	const struct aic_loader_ops* ops = loader->ops;
	void* cookie = loader->cookie;
	int tries;

	for (tries = 0; tries < 8; tries++) {
		int length = ops->bulk_in(cookie, loader->rx, sizeof(loader->rx),
			2000);
		size_t offset = 0;
		if (length < 0)
			return length;

		while (offset + 4 <= (size_t)length) {
			const uint8_t* packet = loader->rx + offset;
			uint16_t packetLength = get16(packet);
			uint8_t type = packet[2];
			size_t advance;

			if (packetLength == 0)
				break;
			if ((type & AIC_TYPE_CONFIG) == 0) {
				advance = ((size_t)packetLength + 60 + 3) & ~(size_t)3;
				LOG("loader: unexpected data packet (%u bytes)\n",
					packetLength);
			} else {
				advance = (((size_t)packetLength + 3) & ~(size_t)3) + 4;
				if ((type & 0x7f) == AIC_TYPE_MESSAGE
					&& offset + 16 <= (size_t)length) {
					uint16_t id = get16(packet + 4);
					uint16_t parameterLength = get16(packet + 10);
					if (id == confirmId) {
						if (confirm != NULL) {
							size_t count = parameterLength;
							if (count > confirmCapacity)
								count = confirmCapacity;
							if (offset + 16 + count > (size_t)length)
								count = (size_t)length - offset - 16;
							memcpy(confirm, packet + 16, count);
						}
						return 0;
					}
					LOG("loader: message %#06x while waiting for %#06x\n",
						id, confirmId);
				}
			}
			offset += advance;
		}
	}
	return -1;
}


static int
send_message(struct loader* loader, uint16_t id, uint16_t task,
	const void* parameters, uint16_t parameterLength, uint16_t confirmId,
	uint8_t* confirm, size_t confirmCapacity)
{
	size_t length;
	int status;

	if ((size_t)parameterLength + AIC_MESSAGE_HEADER > sizeof(loader->tx))
		return -3;

	length = aic_build_message(loader->tx, id, task, parameters,
		parameterLength);
	status = loader->ops->bulk_out(loader->cookie, loader->tx, length);
	loader->messages++;
	if (status != 0)
		return status;
	if (confirmId == 0)
		return 0;
	return wait_confirm(loader, confirmId, confirm, confirmCapacity);
}


int
aic_request(const struct aic_loader_ops* ops, void* cookie, uint16_t id,
	uint16_t task, const void* parameters, uint16_t parameterLength,
	uint16_t confirmId, void* confirm, size_t confirmCapacity)
{
	static struct loader loader;

	memset(&loader, 0, sizeof(loader));
	loader.ops = ops;
	loader.cookie = cookie;
	return send_message(&loader, id, task, parameters, parameterLength,
		confirmId, (uint8_t*)confirm, confirmCapacity);
}


static int
memory_read(struct loader* loader, uint32_t address, uint32_t* _value)
{
	uint8_t parameters[4], confirm[8];
	int status;

	put32(parameters, address);
	status = send_message(loader, AIC_DBG_MEM_READ_REQ, AIC_TASK_DBG,
		parameters, sizeof(parameters), AIC_DBG_MEM_READ_CFM, confirm,
		sizeof(confirm));
	if (status != 0)
		return status;
	if (get32(confirm) != address)
		return -2;
			// the answer to another read: out of step

	*_value = get32(confirm + 4);
	return 0;
}


static int
memory_write(struct loader* loader, uint32_t address, uint32_t value)
{
	uint8_t parameters[8];

	put32(parameters, address);
	put32(parameters + 4, value);
	return send_message(loader, AIC_DBG_MEM_WRITE_REQ, AIC_TASK_DBG,
		parameters, sizeof(parameters), AIC_DBG_MEM_WRITE_CFM, NULL, 0);
}


/*	The block write's parameters are always sent whole, 1024 bytes of data
	whatever the size says: every block is 1048 bytes on the bus. */
static int
memory_block_write(struct loader* loader, uint32_t address,
	const uint8_t* data, uint32_t size)
{
	uint8_t parameters[8 + 1024];

	memset(parameters, 0, sizeof(parameters));
	put32(parameters, address);
	put32(parameters + 4, size);
	memcpy(parameters + 8, data, size);
	return send_message(loader, AIC_DBG_MEM_BLOCK_WRITE_REQ, AIC_TASK_DBG,
		parameters, sizeof(parameters), AIC_DBG_MEM_BLOCK_WRITE_CFM, NULL, 0);
}


/*	A file in 1 KiB blocks, the remainder (1 to 1024 bytes) last. */
static int
upload_file(struct loader* loader, uint32_t address, const char* name)
{
	const struct aic_loader_ops* ops = loader->ops;
	void* cookie = loader->cookie;
	uint8_t* data = NULL;
	long size = ops->load_file(cookie, name, &data);
	long offset = 0;
	int status = 0;

	if (size <= 0) {
		LOG("loader: firmware file %s is missing\n", name);
		return -1;
	}

	LOG("loader: %s (%ld bytes) to %#x\n", name, size, address);
	if (size > 1024) {
		for (; offset < size - 1024; offset += 1024) {
			status = memory_block_write(loader, address + (uint32_t)offset,
				data + offset, 1024);
			if (status != 0)
				break;
		}
	}
	if (status == 0 && offset < size) {
		status = memory_block_write(loader, address + (uint32_t)offset,
			data + offset, (uint32_t)(size - offset));
	}

	ops->release_file(cookie, data);
	return status;
}


/*	The patch table: a 16-byte tag "AICBT_PT_TAG", then records of a
	16-byte name, a type, a count and count {address, value} pairs. Types
	of 1000 and up, and empty records, carry no pairs. */
static int
parse_patch_table(const uint8_t* table, long size,
	struct table_record* records, int* _count)
{
	long offset = 16;
	int count = 0;

	if (size < 16 || memcmp(table, "AICBT_PT_TAG", 12) != 0)
		return -1;

	while (offset + 24 <= size && count < TABLE_MAX_RECORDS) {
		struct table_record* record = &records[count];
		record->type = get32(table + offset + 16);
		record->count = get32(table + offset + 20);
		offset += 24;
		if (record->type >= 1000 || record->count == 0) {
			record->count = 0;
			record->pairs = NULL;
		} else {
			if (offset + (long)record->count * 8 > size)
				return -1;
			record->pairs = table + offset;
			offset += (long)record->count * 8;
		}
		count++;
	}

	*_count = count;
	return 0;
}


int
aic_loader_set_bt_mode(int mode)
{
	sBluetoothMode = mode >= 0 && mode <= 5 ? (uint32_t)mode
		: AIC_BT_MODE_DEFAULT;
	return (int)sBluetoothMode;
}


int
aic_load_firmware_d80(const struct aic_loader_ops* ops, void* cookie,
	struct aic_loader_result* result)
{
	static struct loader loader;
		// 3.5 KiB: not on a kernel stack
	struct table_record records[TABLE_MAX_RECORDS];
	int recordCount = 0, i, status;
	uint8_t* table = NULL;
	long tableSize;
	uint32_t chipRegister, adidBase, patchBase, extensionCount = 0;
	const uint8_t* extensions = NULL;
	const char* wifiFile;
	uint32_t version, configBase, patchBlock, pairs;
	uint8_t start[8];

	memset(&loader, 0, sizeof(loader));
	loader.ops = ops;
	loader.cookie = cookie;
	memset(result, 0, sizeof(*result));

	// the chip revision
	status = memory_read(&loader, AIC_CHIP_ID_REGISTER, &chipRegister);
	if (status != 0) {
		LOG("loader: reading the chip ID failed (%d)\n", status);
		return status;
	}
	result->chip_register = chipRegister;
	result->chip_id = (uint8_t)(chipRegister >> 16);
	result->chip_mcu = ((chipRegister >> 25) & 1) == 0;
	LOG("loader: chip register %#010x, chip ID %#x\n", chipRegister,
		result->chip_id);
	if (result->chip_id == CHIP_REV_U01) {
		LOG("loader: U01 silicon has other firmware files\n");
		return -1;
	}

	// The patch table's first record (INF) holds, as word pairs: where the
	// ADID patch and the ROM patch go, a reset register and its value, and
	// an ADID flag (four pairs, all the chip gets of it); then the number
	// of extension patches and an {id, address} pair for each.
	tableSize = ops->load_file(cookie, FILE_PATCH_TABLE, &table);
	if (tableSize <= 0
		|| parse_patch_table(table, tableSize, records, &recordCount) != 0
		|| recordCount == 0 || records[0].type != TABLE_INF) {
		LOG("loader: the patch table is missing or bad\n");
		if (table != NULL)
			ops->release_file(cookie, table);
		return -1;
	}

	adidBase = ADID_BASE_DEFAULT;
	patchBase = PATCH_BASE_DEFAULT;
	if (records[0].count >= 1)
		adidBase = get32(records[0].pairs + 4);
	if (records[0].count >= 2)
		patchBase = get32(records[0].pairs + 12);
	if (records[0].count > 4) {
		extensionCount = get32(records[0].pairs + 36);
		extensions = records[0].pairs + 40;
		if (records[0].count < 5 + extensionCount)
			extensionCount = records[0].count - 5;
		records[0].count = 4;
	}
	LOG("loader: ADID at %#x, patch at %#x, %u extension(s)\n", adidBase,
		patchBase, extensionCount);

	// the Bluetooth ROM patches
	status = upload_file(&loader, adidBase, FILE_ADID);
	if (status == 0)
		status = upload_file(&loader, patchBase, FILE_PATCH);
	for (i = 0; status == 0 && i < (int)extensionCount; i++) {
		char name[48];
		uint32_t id = get32(extensions + i * 8);
		uint32_t address = get32(extensions + i * 8 + 4);
		size_t length = strlen(FILE_PATCH_EXT);

		memcpy(name, FILE_PATCH_EXT, length);
		name[length++] = (char)('0' + id % 10);
		memcpy(name + length, ".bin", 5);
		status = upload_file(&loader, address, name);
	}
	if (status != 0)
		goto out;

	// The table's word writes, in file order: this is what turns the
	// Bluetooth controller on (PWRON wants 100 ms before anything else).
	for (i = 0; i < recordCount; i++) {
		uint32_t k;
		if (records[i].type == TABLE_VER)
			continue;
		for (k = 0; k < records[i].count; k++) {
			uint32_t address = get32(records[i].pairs + k * 8);
			uint32_t value = get32(records[i].pairs + k * 8 + 4);
			if (records[i].type == TABLE_BTMODE && k < 9) {
				value = k == BT_CONFIG_MODE
					? sBluetoothMode : kBluetoothConfig[k];
			}
			status = memory_write(&loader, address, value);
			if (status != 0)
				goto out;
		}
		if (records[i].type == TABLE_PWRON && ops->sleep_ms != NULL)
			ops->sleep_ms(cookie, 100);
	}

	// The Wi-Fi firmware: "H" silicon has bits 7 and 6 of the ID set.
	wifiFile = (result->chip_id & 0xc0) == 0xc0 ? FILE_WIFI_H : FILE_WIFI;
	result->firmware_name = wifiFile;
	status = upload_file(&loader, WIFI_FIRMWARE_BASE, wifiFile);
	if (status != 0)
		goto out;

	// The firmware's patch block: its location and that of its
	// configuration are words in the image's header.
	status = memory_read(&loader, WIFI_FIRMWARE_BASE + 0x198, &configBase);
	if (status == 0) {
		status = memory_read(&loader, WIFI_FIRMWARE_BASE + 0x1a0,
			&patchBlock);
	}
	if (status == 0)
		status = memory_read(&loader, WIFI_FIRMWARE_BASE + 0x1c, &version);
	if (status != 0)
		goto out;
	result->firmware_version = version;
	pairs = PAIRS_DEFAULT;
	if (version > 0x06090100) {
		status = memory_read(&loader, WIFI_FIRMWARE_BASE + 0x1a4, &pairs);
		if (status != 0)
			goto out;
	}
	LOG("loader: firmware version %#010x, configuration at %#x, patch "
		"block at %#x, pairs at %#x\n", version, configBase, patchBlock,
		pairs);

	status = memory_write(&loader, patchBlock + PATCH_OFFSET_MAGIC,
		PATCH_MAGIC);
	if (status == 0) {
		status = memory_write(&loader, patchBlock + PATCH_OFFSET_MAGIC_2,
			PATCH_MAGIC_2);
	}
	if (status == 0) {
		status = memory_write(&loader, patchBlock + PATCH_OFFSET_PAIRS,
			pairs);
	}
	if (status == 0) {
		status = memory_write(&loader, patchBlock + PATCH_OFFSET_COUNT,
			WIFI_PATCH_COUNT);
	}
	for (i = 0; status == 0 && i < (int)WIFI_PATCH_COUNT; i++) {
		status = memory_write(&loader, pairs + 8 * i,
			configBase + kWifiPatches[i][0]);
		if (status == 0) {
			status = memory_write(&loader, pairs + 8 * i + 4,
				kWifiPatches[i][1]);
		}
	}
	for (i = 0; status == 0 && i < 4; i++) {
		status = memory_write(&loader,
			patchBlock + PATCH_OFFSET_BLOCKS + 4 * i, 0);
	}
	if (status != 0)
		goto out;

	// Start it. Nothing answers: the chip leaves the bus.
	put32(start, WIFI_FIRMWARE_BASE);
	put32(start + 4, AIC_START_APP_AUTO);
	status = send_message(&loader, AIC_DBG_START_APP_REQ, AIC_TASK_DBG, start,
		sizeof(start), 0, NULL, 0);

out:
	result->messages = loader.messages;
	ops->release_file(cookie, table);
	return status;
}


int
aic_send_reboot(const struct aic_loader_ops* ops, void* cookie)
{
	uint8_t buffer[AIC_MESSAGE_HEADER + 8], parameters[8];
	size_t length;

	put32(parameters, 2000);
		// the boot address field carries a delay here
	put32(parameters + 4, AIC_START_APP_REBOOT);
	length = aic_build_message(buffer, AIC_DBG_START_APP_REQ, AIC_TASK_DBG,
		parameters, sizeof(parameters));
	return ops->bulk_out(cookie, buffer, length);
}
