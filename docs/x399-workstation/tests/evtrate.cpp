// evtrate <vendor:device> <ms> [slot ep]: watch an xHCI controller's event
// ring (read only, through /dev/misc/poke) for a while and report transfer
// events per second and the distribution of the gaps between them, optionally
// for one slot and endpoint (DCI) only. Run it while a transfer test is going:
// evenly spaced completions mean the device or the bus sets the pace, bursts
// with long gaps mean the host stops queueing work in time.
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
};
#define POKE_SIGNATURE 'wltp'
typedef struct { uint32 signature; uint8 index; pci_info* info; status_t status; }
	pci_info_args;
typedef struct { uint32 signature; area_id area; const char* name;
	phys_addr_t physical_address; size_t size; uint32 flags; uint32 protection;
	void* address; } mem_map_args;

static int sPoke;

static volatile uint32*
map(phys_addr_t address, size_t size)
{
	phys_addr_t page = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	size_t length = ((address - page) + size + B_PAGE_SIZE - 1)
		& ~(size_t)(B_PAGE_SIZE - 1);
	mem_map_args args = { POKE_SIGNATURE, -1, "evtrate", page, length,
		B_ANY_ADDRESS, B_READ_AREA, NULL };
	if (ioctl(sPoke, POKE_MAP_MEMORY, &args, sizeof(args)) < 0 || args.area < 0)
		return NULL;
	return (volatile uint32*)((uint8*)args.address + (address - page));
}

int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <vendor:device> <ms> [slot dci]\n", argv[0]);
		return 1;
	}
	unsigned vendor, deviceID;
	sscanf(argv[1], "%x:%x", &vendor, &deviceID);
	bigtime_t duration = atoi(argv[2]) * 1000LL;
	int wantSlot = argc > 4 ? atoi(argv[3]) : -1;
	int wantDci = argc > 4 ? atoi(argv[4]) : -1;

	sPoke = open("/dev/misc/poke", O_RDWR);
	pci_info info;
	bool found = false;
	for (int index = 0; index < 256 && !found; index++) {
		pci_info_args args = { POKE_SIGNATURE, (uint8)index, &info, B_OK };
		if (ioctl(sPoke, POKE_GET_NTH_PCI_INFO, &args, sizeof(args)) != 0
			|| args.status != B_OK)
			break;
		found = info.vendor_id == vendor && info.device_id == deviceID;
	}
	if (!found)
		return 1;
	phys_addr_t physical = info.u.h0.base_registers[0];
	if ((info.u.h0.base_register_flags[0] & 0x06) == 0x04)
		physical |= (phys_addr_t)info.u.h0.base_registers[1] << 32;
	volatile uint8* regs = (volatile uint8*)map(physical, 0x10000);
	uint32 runtime = *(volatile uint32*)(regs + 0x18) & ~0x1f;
	phys_addr_t erst = *(volatile uint32*)(regs + runtime + 0x30)
		| (phys_addr_t)*(volatile uint32*)(regs + runtime + 0x34) << 32;
	volatile uint32* segment = map(erst, 16);
	phys_addr_t ringAddress = segment[0] | (phys_addr_t)segment[1] << 32;
	uint32 count = segment[2] & 0xffff;
	volatile uint32* ring = map(ringAddress, count * 16);

	uint32 last[4096];
	for (uint32 i = 0; i < count; i++)
		last[i] = ring[i * 4 + 3];

	// gap histogram in microseconds: <5, <10, <20, <40, <80, <160, <320, more
	static const int kLimits[] = { 5, 10, 20, 40, 80, 160, 320 };
	int64 histogram[8] = {};
	int64 events = 0, bytesShort = 0;
	bigtime_t previous = 0, start = system_time(), maxGap = 0;
	uint32 index = 0;
	// find where the controller writes next: the first entry whose cycle bit
	// differs from the one before it
	for (uint32 i = 1; i < count; i++) {
		if ((ring[i * 4 + 3] & 1) != (ring[(i - 1) * 4 + 3] & 1)) {
			index = i;
			break;
		}
	}
	while (system_time() - start < duration) {
		uint32 flags = ring[index * 4 + 3];
		if (flags == last[index])
			continue;
		bigtime_t now = system_time();
		last[index] = flags;
		uint32 status = ring[index * 4 + 2];
		index = (index + 1) % count;
		if (((flags >> 10) & 0x3f) != 32)
			continue;
		if (wantSlot >= 0 && ((int)(flags >> 24) != wantSlot
				|| (int)((flags >> 16) & 0x1f) != wantDci))
			continue;
		events++;
		bytesShort += status & 0xffffff;
		if (previous != 0) {
			bigtime_t gap = now - previous;
			int bucket = 0;
			while (bucket < 7 && gap >= kLimits[bucket])
				bucket++;
			histogram[bucket]++;
			if (gap > maxGap)
				maxGap = gap;
		}
		previous = now;
	}
	double seconds = (system_time() - start) / 1e6;
	printf("%lld transfer events in %.2f s: %.0f/s, max gap %lld us\n",
		(long long)events, seconds, events / seconds, (long long)maxGap);
	const char* names[] = { "<5", "<10", "<20", "<40", "<80", "<160", "<320",
		">=320" };
	for (int i = 0; i < 8; i++)
		printf("  gap %5s us: %lld\n", names[i], (long long)histogram[i]);
	return 0;
}
