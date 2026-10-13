// pcicfg <vendor:device> [n]: hex dump of the first 256 bytes of PCI
// configuration space of the n-th (default first) function with that ID,
// through /dev/misc/poke. "pcicfg <vendor:device> [n] w <offset> <size>
// <value>" writes one register and reads it back - for experiments only.
#include <Drivers.h>
#include <PCI.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	POKE_PCI_READ_CONFIG = B_DEVICE_OP_CODES_END + 5,
	POKE_PCI_WRITE_CONFIG,
	POKE_GET_NTH_PCI_INFO,
};
#define POKE_SIGNATURE 'wltp'
typedef struct { uint32 signature; uint8 index; pci_info* info; status_t status; }
	pci_info_args;
typedef struct { uint32 signature; uint8 bus; uint8 device; uint8 function;
	uint8 size; uint8 offset; uint32 value; } pci_io_args;

static int sFd;

static uint32
config(const pci_info& info, uint8 offset, uint8 size)
{
	pci_io_args args = { POKE_SIGNATURE, info.bus, info.device, info.function,
		size, offset, 0 };
	ioctl(sFd, POKE_PCI_READ_CONFIG, &args, sizeof(args));
	return args.value;
}

static void
write_config(const pci_info& info, uint8 offset, uint8 size, uint32 value)
{
	pci_io_args args = { POKE_SIGNATURE, info.bus, info.device, info.function,
		size, offset, value };
	ioctl(sFd, POKE_PCI_WRITE_CONFIG, &args, sizeof(args));
}

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <vendor:device> [n] [w <offset> <size>"
			" <value>]\n", argv[0]);
		return 1;
	}
	unsigned vendor, deviceID;
	sscanf(argv[1], "%x:%x", &vendor, &deviceID);
	int arg = 2;
	int wanted = 0;
	if (argc > arg && strcmp(argv[arg], "w") != 0)
		wanted = atoi(argv[arg++]);

	sFd = open("/dev/misc/poke", O_RDWR);
	if (sFd < 0) {
		perror("poke");
		return 1;
	}
	pci_info info;
	int found = -1;
	for (int index = 0; index < 256; index++) {
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(sFd, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		if (info.vendor_id == vendor && info.device_id == deviceID
			&& ++found == wanted)
			break;
	}
	if (found != wanted) {
		fprintf(stderr, "no function %04x:%04x #%d\n", vendor, deviceID, wanted);
		return 1;
	}
	printf("%04x:%04x at %02x:%02x.%x\n", vendor, deviceID, info.bus,
		info.device, info.function);

	if (argc > arg + 3 && strcmp(argv[arg], "w") == 0) {
		uint8 offset = strtoul(argv[arg + 1], NULL, 0);
		uint8 size = strtoul(argv[arg + 2], NULL, 0);
		uint32 value = strtoul(argv[arg + 3], NULL, 0);
		uint32 before = config(info, offset, size);
		write_config(info, offset, size, value);
		printf("%#04x: %#x -> wrote %#x, reads %#x\n", offset, before, value,
			config(info, offset, size));
		return 0;
	}

	for (int row = 0; row < 256; row += 16) {
		printf("%02x:", row);
		for (int i = 0; i < 16; i += 4)
			printf(" %08x", config(info, row + i, 4));
		printf("\n");
	}
	return 0;
}
