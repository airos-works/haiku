// pcitree: every PCI function with its command and status registers, and for
// bridges the bus range they forward, through /dev/misc/poke (read only).
// Bus Master Enable (command bit 2) on a bridge is what lets the devices
// behind it reach memory; status bits 11-15 record aborts and errors.
#include <Drivers.h>
#include <PCI.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

enum {
	POKE_PCI_READ_CONFIG = B_DEVICE_OP_CODES_END + 5,
	POKE_GET_NTH_PCI_INFO = B_DEVICE_OP_CODES_END + 7,
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

int
main()
{
	sFd = open("/dev/misc/poke", O_RDWR);
	for (int index = 0; index < 256; index++) {
		pci_info info;
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(sFd, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		uint16 command = config(info, 0x04, 2);
		uint16 status = config(info, 0x06, 2);
		printf("%02x:%02x.%x %04x:%04x class %02x%02x%02x cmd %04x%s%s status"
			" %04x", info.bus, info.device, info.function, info.vendor_id,
			info.device_id, info.class_base, info.class_sub, info.class_api,
			command, (command & 4) ? " BM" : " --", (command & 2) ? " MEM" : "",
			status);
		if ((status & 0xf900) != 0)
			printf(" ERR(%s%s%s%s%s)", (status & 0x8000) ? " parity" : "",
				(status & 0x4000) ? " SERR" : "",
				(status & 0x2000) ? " master-abort" : "",
				(status & 0x1000) ? " target-abort-rcvd" : "",
				(status & 0x0800) ? " target-abort-sent" : "");
		if ((info.header_type & 0x7f) == 1) {
			uint16 bridgeControl = config(info, 0x3e, 2);
			uint16 secondaryStatus = config(info, 0x1e, 2);
			printf("  bridge %02x->%02x..%02x secstatus %04x bctl %04x",
				info.u.h1.primary_bus, info.u.h1.secondary_bus,
				info.u.h1.subordinate_bus, secondaryStatus, bridgeControl);
		}
		// PCI Express device status (errors detected)
		for (uint8 cap = config(info, 0x34, 1); cap != 0 && cap != 0xff;
				cap = config(info, cap + 1, 1)) {
			if (config(info, cap, 1) == 0x10) {
				uint16 devStatus = config(info, cap + 0x0a, 2);
				uint16 devControl = config(info, cap + 0x08, 2);
				if ((devStatus & 0xf) != 0)
					printf("  pcie devstatus %#x", devStatus);
				printf("  devctl %#x", devControl);
				break;
			}
		}
		printf("\n");
	}
	return 0;
}
