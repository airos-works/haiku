// usbmsbench <usb device> [MiB [KiB per command [write]]]: measure a USB
// mass storage device through usb_raw, without usb_disk: Bulk-Only Transport
// with SCSI INQUIRY, READ CAPACITY and READ(10) (WRITE(10) with "write",
// which destroys the data at the start of the medium). Prints the time of a
// single command too, so the cost per command shows apart from the rate.
// Only for devices no driver has claimed: two masters on one bulk pipe would
// corrupt each other's commands.
#include <USBKit.h>
#include <OS.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cbw {
	uint32	signature;
	uint32	tag;
	uint32	length;
	uint8	flags;
	uint8	lun;
	uint8	commandLength;
	uint8	command[16];
} _PACKED;

struct csw {
	uint32	signature;
	uint32	tag;
	uint32	residue;
	uint8	status;
} _PACKED;

static const BUSBEndpoint* sIn;
static const BUSBEndpoint* sOut;
static uint32 sTag = 1;

static ssize_t
command(const uint8* scsi, uint8 scsiLength, void* data, uint32 length,
	bool in)
{
	cbw wrapper;
	memset(&wrapper, 0, sizeof(wrapper));
	wrapper.signature = 0x43425355;
	wrapper.tag = sTag++;
	wrapper.length = length;
	wrapper.flags = in ? 0x80 : 0;
	wrapper.commandLength = scsiLength;
	memcpy(wrapper.command, scsi, scsiLength);
	ssize_t result = sOut->BulkTransfer(&wrapper, 31);
	if (result != 31) {
		fprintf(stderr, "CBW: %s\n", strerror(result < 0 ? result : B_ERROR));
		return B_ERROR;
	}
	ssize_t moved = 0;
	if (length > 0) {
		moved = in ? sIn->BulkTransfer(data, length)
			: sOut->BulkTransfer(data, length);
		if (moved < 0) {
			fprintf(stderr, "data: %s\n", strerror(moved));
			return moved;
		}
	}
	csw status;
	result = sIn->BulkTransfer(&status, sizeof(status));
	if (result != 13 || status.signature != 0x53425355
		|| status.tag != wrapper.tag) {
		fprintf(stderr, "CSW: %zd bytes, signature %#x\n", result,
			status.signature);
		return B_ERROR;
	}
	if (status.status != 0) {
		fprintf(stderr, "command %#x failed: status %u\n", scsi[0],
			status.status);
		return B_ERROR;
	}
	return moved;
}

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <usb device> [MiB [KiB per command"
			" [write]]]\n", argv[0]);
		return 1;
	}
	int mebibytes = argc > 2 ? atoi(argv[2]) : 64;
	uint32 chunk = (argc > 3 ? atoi(argv[3]) : 64) * 1024;
	bool write = argc > 4 && strcmp(argv[4], "write") == 0;

	BUSBDevice device(argv[1]);
	if (device.InitCheck() != B_OK) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	const BUSBConfiguration* configuration = device.ActiveConfiguration();
	for (uint32 i = 0; configuration != NULL
			&& i < configuration->CountInterfaces(); i++) {
		const BUSBInterface* interface = configuration->InterfaceAt(i);
		if (interface->Class() != 8 || interface->Protocol() != 0x50)
			continue;
		for (uint32 e = 0; e < interface->CountEndpoints(); e++) {
			const BUSBEndpoint* endpoint = interface->EndpointAt(e);
			if (!endpoint->IsBulk())
				continue;
			if (endpoint->IsInput())
				sIn = endpoint;
			else
				sOut = endpoint;
		}
		break;
	}
	if (sIn == NULL || sOut == NULL) {
		fprintf(stderr, "no bulk-only mass storage interface\n");
		return 1;
	}
	printf("%s: %s %s, USB %x.%02x, bulk max packet %u\n", argv[1],
		device.ManufacturerString(), device.ProductString(),
		device.USBVersion() >> 8, device.USBVersion() & 0xff,
		sIn->MaxPacketSize());

	uint8 inquiry[6] = { 0x12, 0, 0, 0, 36, 0 };
	uint8 answer[36];
	if (command(inquiry, 6, answer, 36, true) < 0)
		return 1;
	printf("inquiry: %.8s %.16s\n", answer + 8, answer + 16);

	uint8 capacity[10] = { 0x25 };
	uint8 size[8];
	for (int attempt = 0; command(capacity, 10, size, 8, true) < 0; attempt++) {
		if (attempt == 5)
			return 1;
		// a medium that is not ready yet answers with a check condition
		uint8 sense[6] = { 0x03, 0, 0, 0, 18, 0 };
		uint8 senseData[18];
		command(sense, 6, senseData, 18, true);
		snooze(200000);
	}
	uint32 blocks = ((uint32)size[0] << 24 | size[1] << 16 | size[2] << 8
		| size[3]) + 1;
	uint32 blockSize = (uint32)size[4] << 24 | size[5] << 16 | size[6] << 8
		| size[7];
	printf("capacity: %u blocks of %u bytes (%.1f MiB)\n", blocks, blockSize,
		(double)blocks * blockSize / 1048576);

	uint8* buffer = (uint8*)malloc(chunk);
	memset(buffer, 0x5a, chunk);
	uint32 perCommand = chunk / blockSize;
	uint64 total = (uint64)mebibytes * 1048576;
	uint32 commands = total / chunk;

	// one small command alone, a few times: the fixed cost of a command
	bigtime_t best = B_INFINITE_TIMEOUT;
	for (int i = 0; i < 20; i++) {
		uint8 read10[10] = { 0x28, 0, 0, 0, 0, (uint8)i, 0, 0, 1, 0 };
		bigtime_t start = system_time();
		if (command(read10, 10, buffer, blockSize, true) < 0)
			return 1;
		bigtime_t took = system_time() - start;
		if (took < best)
			best = took;
	}
	printf("one %u byte read: best %lld us\n", blockSize, (long long)best);

	bigtime_t start = system_time();
	for (uint32 i = 0; i < commands; i++) {
		uint32 lba = (i * perCommand) % (blocks - perCommand);
		uint8 rw10[10] = { (uint8)(write ? 0x2a : 0x28), 0, (uint8)(lba >> 24),
			(uint8)(lba >> 16), (uint8)(lba >> 8), (uint8)lba, 0,
			(uint8)(perCommand >> 8), (uint8)perCommand, 0 };
		if (command(rw10, 10, buffer, chunk, !write) != (ssize_t)chunk) {
			fprintf(stderr, "transfer %u failed\n", i);
			return 1;
		}
	}
	bigtime_t took = system_time() - start;
	printf("%s %u x %u KiB: %.1f MiB/s (%.0f us per command)\n",
		write ? "wrote" : "read", commands, chunk / 1024,
		(double)total / took * 1000000 / 1048576, (double)took / commands);
	return 0;
}
