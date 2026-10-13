// portwatch <vendor:device>[#n] <seconds>: poll every PORTSC of one xHCI
// controller (read only, through /dev/misc/poke) and print each change with
// the time it was seen, decoded: connect, enable, reset, link state, speed
// and the change bits. Shows what a port does around a connect, a reset or a
// device that drops off, at a resolution of a few microseconds.
#include <Drivers.h>
#include <OS.h>
#include <PCI.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
	POKE_GET_NTH_PCI_INFO = B_DEVICE_OP_CODES_END + 7,
	POKE_MAP_MEMORY = B_DEVICE_OP_CODES_END + 9,
	POKE_UNMAP_MEMORY
};
#define POKE_SIGNATURE 'wltp'
typedef struct { uint32 signature; uint8 index; pci_info* info; status_t status; }
	pci_info_args;
typedef struct { uint32 signature; area_id area; const char* name;
	phys_addr_t physical_address; size_t size; uint32 flags; uint32 protection;
	void* address; } mem_map_args;

static void
decode(uint32 sc, char* out)
{
	static const char* kLink[16] = { "U0", "U1", "U2", "U3", "Disabled",
		"RxDetect", "Inactive", "Polling", "Recovery", "HotReset",
		"Compliance", "Test", "12", "13", "14", "Resume" };
	sprintf(out, "%s%s%s%s%s link %s speed %u%s%s%s%s%s%s%s",
		(sc & 1) ? "CCS " : "", (sc & 2) ? "PED " : "", (sc & 8) ? "OCA " : "",
		(sc & 0x10) ? "PR " : "", (sc & 0x200) ? "PP" : "-",
		kLink[(sc >> 5) & 0xf], (sc >> 10) & 0xf,
		(sc & (1 << 17)) ? " CSC" : "", (sc & (1 << 18)) ? " PEC" : "",
		(sc & (1 << 19)) ? " WRC" : "", (sc & (1 << 20)) ? " OCC" : "",
		(sc & (1 << 21)) ? " PRC" : "", (sc & (1 << 22)) ? " PLC" : "",
		(sc & (1 << 23)) ? " CEC" : "");
}

int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <vendor:device>[#n] <seconds>\n", argv[0]);
		return 1;
	}
	unsigned vendor, deviceID;
	int wanted = 0;
	if (sscanf(argv[1], "%x:%x#%d", &vendor, &deviceID, &wanted) < 2)
		return 1;
	bigtime_t duration = atoi(argv[2]) * 1000000LL;
	int fd = open("/dev/misc/poke", O_RDWR);
	pci_info info;
	bool found = false;
	for (int index = 0; index < 256 && !found; index++) {
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(fd, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		found = info.vendor_id == vendor && info.device_id == deviceID
			&& wanted-- == 0;
	}
	if (!found)
		return 1;
	phys_addr_t physical = info.u.h0.base_registers[0];
	if ((info.u.h0.base_register_flags[0] & 0x06) == 0x04)
		physical |= (phys_addr_t)info.u.h0.base_registers[1] << 32;
	mem_map_args map = { POKE_SIGNATURE, -1, "portwatch", physical, 0x10000,
		B_ANY_ADDRESS, B_READ_AREA, NULL };
	if (ioctl(fd, POKE_MAP_MEMORY, &map, sizeof(map)) < 0 || map.area < 0)
		return 1;
	volatile uint8* regs = (volatile uint8*)map.address;
	uint32 capLength = *(volatile uint32*)regs & 0xff;
	uint32 ports = *(volatile uint32*)(regs + 4) >> 24;
	uint32 last[256];
	char text[160];
	bigtime_t start = system_time();
	for (uint32 p = 0; p < ports; p++) {
		last[p] = *(volatile uint32*)(regs + capLength + 0x400 + 0x10 * p);
		decode(last[p], text);
		printf("%9.3f ms port %2u: %#010x %s\n", 0.0, p + 1, last[p], text);
	}
	while (system_time() - start < duration) {
		for (uint32 p = 0; p < ports; p++) {
			uint32 sc = *(volatile uint32*)(regs + capLength + 0x400 + 0x10 * p);
			if (sc == last[p])
				continue;
			last[p] = sc;
			decode(sc, text);
			printf("%9.3f ms port %2u: %#010x %s\n",
				(system_time() - start) / 1000.0, p + 1, sc, text);
			fflush(stdout);
		}
	}
	return 0;
}
