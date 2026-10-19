// nvpoll: sample a falcon's registers through /dev/misc/poke and print every
// change (interrupt status, mailboxes, idle state, CPU control...), to see
// what an engine does before resman resets it.
//   nvpoll <falcon phys base> <seconds> [dump file [microseconds]]
// With a dump file: once the engine's units have been busy for that long
// (default 50 ms), write its 16 KB of registers there - a hang, that is.
// Reading them all while a picture is still being encoded breaks it.
// NVENC0 on a GP104 is BAR0 + 0x1c8000 (NV_PTOP_DEVICE_INFO says where).
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <OS.h>
#include "poke.h"

int
main(int argc, char** argv)
{
	if (argc < 3)
		return 2;
	uint64 phys = strtoull(argv[1], NULL, 0);
	double seconds = atof(argv[2]);
	int fd = open("/dev/misc/poke", O_RDWR);
	if (fd < 0) {
		perror("poke");
		return 1;
	}
	mem_map_args a = {};
	a.signature = POKE_SIGNATURE;
	a.name = "nvpoll";
	a.physical_address = phys & ~(B_PAGE_SIZE - 1);
	a.size = 0x4000;
	a.protection = B_READ_AREA | B_WRITE_AREA;
	a.flags = B_ANY_ADDRESS;
	if (ioctl(fd, POKE_MAP_MEMORY, &a, sizeof(a)) < 0) {
		perror("map");
		return 1;
	}
	volatile uint32* r = (volatile uint32*)((uint8*)a.address + (phys - a.physical_address));
	static const int kOffsets[] = { 0x008, 0x010, 0x040, 0x044, 0x04c, 0x100, 0x104,
		0x0a4, 0x0e8, 0x0ec };
	const int count = sizeof(kOffsets) / sizeof(kOffsets[0]);
	uint32 last[count];
	memset(last, 0xff, sizeof(last));
	bigtime_t start = system_time(), end = start + (bigtime_t)(seconds * 1e6);
	int changes = 0;
	const char* dumpPath = argc > 3 ? argv[3] : NULL;
	bigtime_t stuckSince = 0;
	bool dumped = false;
	while (system_time() < end && changes < 400) {
		uint32 now[count];
		for (int i = 0; i < count; i++)
			now[i] = r[kOffsets[i] / 4];
		if (dumpPath != NULL && !dumped) {
			bool busy = now[4] != 0 && (now[4] & 0xffff0000) != 0xbadf0000;
			if (busy) {
				if (stuckSince == 0)
					stuckSince = system_time();
				else if (system_time() - stuckSince > (argc > 4 ? atoi(argv[4]) : 50000)) {
					FILE* f = fopen(dumpPath, "wb");
					for (int i = 0; i < 0x1000; i++) {
						uint32 v = r[i];
						fwrite(&v, 4, 1, f);
					}
					fclose(f);
					dumped = true;
					printf("dumped at %.3f ms\n", (system_time() - start) / 1000.0);
				}
			} else
				stuckSince = 0;
		}
		if (memcmp(now, last, sizeof(now)) != 0) {
			printf("%8.3f ms", (system_time() - start) / 1000.0);
			for (int i = 0; i < count; i++)
				printf(" %03x=%08x", kOffsets[i], now[i]);
			printf("\n");
			memcpy(last, now, sizeof(now));
			changes++;
		}
	}
	return 0;
}
