// asmreg <vendor:device> r <address> [words]: read the internal memory of an
// ASMedia xHCI controller (its 8051's XDATA space, where its registers are)
// through the vendor window in PCI configuration space; "w <address>
// <byte>" writes one byte, the way Linux sets the ASM1042A's flow control
// (usb_asmedia_modifyflowcontrol(): 0xba to 0xfa30). The protocol is the one
// Linux and ASMTool (github.com/smx-smx/ASMTool) use:
//   0xe0 control: bit 0 set by the chip when read data waits in 0xf0/0xf4,
//        cleared by writing 1; bit 1 set by the host to send 0xf8/0xfc,
//        cleared by the chip once it took them.
//   0xf8/0xfc: a command (byte 0 operation 0x23 write / 0x40 read, byte 1
//        0x04 memory, byte 2 size) and its address, then the data.
// For experiments: nothing checks that the chip is an ASMedia one.
#include <Drivers.h>
#include <OS.h>
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
static pci_info sInfo;

static uint32
config(uint8 offset, uint8 size)
{
	pci_io_args args = { POKE_SIGNATURE, sInfo.bus, sInfo.device,
		sInfo.function, size, offset, 0 };
	ioctl(sFd, POKE_PCI_READ_CONFIG, &args, sizeof(args));
	return args.value;
}

static void
write_config(uint8 offset, uint8 size, uint32 value)
{
	pci_io_args args = { POKE_SIGNATURE, sInfo.bus, sInfo.device,
		sInfo.function, size, offset, value };
	ioctl(sFd, POKE_PCI_WRITE_CONFIG, &args, sizeof(args));
}

static bool
wait_control(uint8 mask, bool set)
{
	bigtime_t until = system_time() + 100000;
	while (system_time() < until) {
		uint8 control = config(0xe0, 1);
		if (control == 0xff) {
			fprintf(stderr, "control register reads 0xff\n");
			return false;
		}
		if (((control & mask) != 0) == set)
			return true;
		snooze(20);
	}
	fprintf(stderr, "timed out waiting for control bit %#x %s (now %#x)\n",
		mask, set ? "set" : "clear", (unsigned)config(0xe0, 1));
	return false;
}

static bool
send(uint32 select, uint32 data)
{
	if (!wait_control(2, false))
		return false;
	write_config(0xf8, 4, select);
	write_config(0xfc, 4, data);
	write_config(0xe0, 1, 2);
	return true;
}

static bool
receive(uint32* low, uint32* high)
{
	if (!wait_control(1, true))
		return false;
	*low = config(0xf0, 4);
	*high = config(0xf4, 4);
	write_config(0xe0, 1, 1);
	return true;
}

int
main(int argc, char** argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: %s <vendor:device> r <address> [words]\n"
			"       %s <vendor:device> w <address> <byte>\n", argv[0], argv[0]);
		return 1;
	}
	unsigned vendor, deviceID;
	sscanf(argv[1], "%x:%x", &vendor, &deviceID);
	uint32 address = strtoul(argv[3], NULL, 0);

	sFd = open("/dev/misc/poke", O_RDWR);
	if (sFd < 0) {
		perror("poke");
		return 1;
	}
	bool found = false;
	for (int index = 0; index < 256 && !found; index++) {
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &sInfo, B_OK };
		if (ioctl(sFd, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		found = sInfo.vendor_id == vendor && sInfo.device_id == deviceID;
	}
	if (!found) {
		fprintf(stderr, "no %04x:%04x\n", vendor, deviceID);
		return 1;
	}
	printf("%04x:%04x at %02x:%02x.%x, control %#x\n", vendor, deviceID,
		sInfo.bus, sInfo.device, sInfo.function, (unsigned)config(0xe0, 1));

	if (strcmp(argv[2], "w") == 0 && argc > 4) {
		uint32 value = strtoul(argv[4], NULL, 0) & 0xff;
		if (!send(0x010423, address) || !send(value, 0)
			|| !wait_control(2, false)) {
			return 1;
		}
		printf("wrote %#04x to %#06x\n", value, address);
		return 0;
	}

	int words = argc > 4 ? atoi(argv[4]) : 1;
	for (int i = 0; i < words; i++, address += 4) {
		uint32 ackLow, ackHigh, low, high;
		if (!send(0x040440, address) || !send(0, 0)
			|| !receive(&ackLow, &ackHigh) || !receive(&low, &high)) {
			return 1;
		}
		printf("%#06x: %02x %02x %02x %02x   (ack %08x %08x, high %08x)\n",
			address, low & 0xff, (low >> 8) & 0xff, (low >> 16) & 0xff,
			low >> 24, ackLow, ackHigh, high);
	}
	return 0;
}
