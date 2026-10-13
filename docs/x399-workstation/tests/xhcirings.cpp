// xhcirings <vendor:device> [n]: read-only dump of an xHCI controller's event
// ring and of Haiku's command ring (which the xhci driver allocates right
// after the event ring, in the same area) through /dev/misc/poke: every TRB
// that is not all zero, with its type, completion code, slot and endpoint.
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

static int sPoke;

static void*
map(phys_addr_t address, size_t size)
{
	phys_addr_t page = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	size_t length = ((address - page) + size + B_PAGE_SIZE - 1)
		& ~(size_t)(B_PAGE_SIZE - 1);
	mem_map_args args = { POKE_SIGNATURE, -1, "xhcirings", page, length,
		B_ANY_ADDRESS, B_READ_AREA, NULL };
	if (ioctl(sPoke, POKE_MAP_MEMORY, &args, sizeof(args)) < 0 || args.area < 0)
		return NULL;
	return (uint8*)args.address + (address - page);
}

static void
dump(const char* what, volatile uint32* trbs, uint32 count)
{
	printf("%s:\n", what);
	for (uint32 i = 0; i < count; i++) {
		volatile uint32* t = trbs + i * 4;
		if ((t[0] | t[1] | t[2] | t[3]) == 0)
			continue;
		printf("  [%3u] %08x%08x %08x %08x  type %2u code %3u slot %3u ep %2u"
			" cycle %u\n", i, t[1], t[0], t[2], t[3], (t[3] >> 10) & 0x3f,
			t[2] >> 24, t[3] >> 24, (t[3] >> 16) & 0x1f, t[3] & 1);
	}
}

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <vendor:device> [n]\n", argv[0]);
		return 1;
	}
	unsigned vendor, deviceID;
	sscanf(argv[1], "%x:%x", &vendor, &deviceID);
	int wanted = argc > 2 ? atoi(argv[2]) : 0;
	sPoke = open("/dev/misc/poke", O_RDWR);
	pci_info info;
	int found = -1;
	for (int index = 0; index < 256; index++) {
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(sPoke, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		if (info.vendor_id == vendor && info.device_id == deviceID
			&& ++found == wanted)
			break;
	}
	if (found != wanted)
		return 1;
	phys_addr_t physical = info.u.h0.base_registers[0];
	if ((info.u.h0.base_register_flags[0] & 0x06) == 0x04)
		physical |= (phys_addr_t)info.u.h0.base_registers[1] << 32;
	volatile uint8* regs = (volatile uint8*)map(physical, 0x10000);
	uint32 runtime = *(volatile uint32*)(regs + 0x18) & ~0x1f;
	phys_addr_t erst = *(volatile uint32*)(regs + runtime + 0x30)
		| (phys_addr_t)*(volatile uint32*)(regs + runtime + 0x34) << 32;
	volatile uint32* segment = (volatile uint32*)map(erst, 16);
	phys_addr_t ring = segment[0] | (phys_addr_t)segment[1] << 32;
	uint32 events = segment[2] & 0xffff;
	printf("ERST %#llx: ring %#llx, %u events\n", (unsigned long long)erst,
		(unsigned long long)ring, events);
	volatile uint32* trbs = (volatile uint32*)map(ring, (events + 16) * 16);
	dump("event ring", trbs, events);
	dump("command ring (Haiku: right after the events)", trbs + events * 4, 16);
	return 0;
}
