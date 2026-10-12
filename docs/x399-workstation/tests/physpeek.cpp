// physpeek <physical address> [dwords]: read-only hex dump of physical memory
// or memory mapped registers through /dev/misc/poke, 32 bits at a time.
#include <Drivers.h>
#include <OS.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

enum {
	POKE_MAP_MEMORY = B_DEVICE_OP_CODES_END + 9,
	POKE_UNMAP_MEMORY
};
#define POKE_SIGNATURE 'wltp'
typedef struct { uint32 signature; area_id area; const char* name;
	phys_addr_t physical_address; size_t size; uint32 flags; uint32 protection;
	void* address; } mem_map_args;

int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <physical address> [dwords]\n", argv[0]);
		return 1;
	}
	phys_addr_t address = strtoull(argv[1], NULL, 0) & ~(phys_addr_t)3;
	int count = argc > 2 ? atoi(argv[2]) : 4;
	int fd = open("/dev/misc/poke", O_RDWR);
	phys_addr_t page = address & ~(phys_addr_t)(B_PAGE_SIZE - 1);
	size_t length = ((address - page) + count * 4 + B_PAGE_SIZE - 1)
		& ~(size_t)(B_PAGE_SIZE - 1);
	mem_map_args map = { POKE_SIGNATURE, -1, "physpeek", page, length,
		B_ANY_ADDRESS, B_READ_AREA, NULL };
	if (fd < 0 || ioctl(fd, POKE_MAP_MEMORY, &map, sizeof(map)) < 0
		|| map.area < 0) {
		fprintf(stderr, "cannot map %#llx\n", (unsigned long long)address);
		return 1;
	}
	volatile uint32* words = (volatile uint32*)((uint8*)map.address
		+ (address - page));
	for (int i = 0; i < count; i++) {
		if (i % 4 == 0)
			printf("%s%#llx:", i ? "\n" : "", (unsigned long long)address + i * 4);
		printf(" %08x", words[i]);
	}
	printf("\n");
	ioctl(fd, POKE_UNMAP_MEMORY, &map, sizeof(map));
	return 0;
}
