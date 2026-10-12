// xhciimod <vendor:device> [value]: read or set an xHCI controller's interrupt
// moderation interval (IMOD of interrupter 0, in 250 ns units) through
// /dev/misc/poke, to compare settings without rebuilding the driver. The
// driver sets it again when the controller starts or resumes.
#include <Drivers.h>
#include <OS.h>
#include <PCI.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum {
	POKE_GET_NTH_PCI_INFO = B_DEVICE_OP_CODES_END + 7,
	POKE_MAP_MEMORY = B_DEVICE_OP_CODES_END + 9,
};
#define POKE_SIGNATURE 'wltp'
typedef struct { uint32 signature; uint8 index; pci_info* info; status_t status; }
	pci_info_args;
typedef struct { uint32 signature; area_id area; const char* name;
	phys_addr_t physical_address; size_t size; uint32 flags; uint32 protection;
	void* address; } mem_map_args;

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <vendor:device> [value]\n", argv[0]);
		return 1;
	}
	unsigned vendor, deviceID;
	sscanf(argv[1], "%x:%x", &vendor, &deviceID);
	int fd = open("/dev/misc/poke", O_RDWR);
	pci_info info;
	bool found = false;
	for (int index = 0; index < 256 && !found; index++) {
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(fd, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		found = info.vendor_id == vendor && info.device_id == deviceID;
	}
	if (!found)
		return 1;
	phys_addr_t physical = info.u.h0.base_registers[0];
	if ((info.u.h0.base_register_flags[0] & 0x06) == 0x04)
		physical |= (phys_addr_t)info.u.h0.base_registers[1] << 32;
	mem_map_args map = { POKE_SIGNATURE, -1, "xhciimod", physical, 0x10000,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, NULL };
	if (ioctl(fd, POKE_MAP_MEMORY, &map, sizeof(map)) < 0 || map.area < 0)
		return 1;
	volatile uint8* regs = (volatile uint8*)map.address;
	uint32 runtime = *(volatile uint32*)(regs + 0x18) & ~0x1f;
	volatile uint32* imod = (volatile uint32*)(regs + runtime + 0x24);
	if (argc > 2)
		*imod = strtoul(argv[2], NULL, 0) & 0xffff;
	printf("IMOD %#x (%.1f us)\n", *imod & 0xffff, (*imod & 0xffff) * 0.25);
	return 0;
}
