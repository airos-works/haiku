/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Device memory a driver mapped into a team, through fork(), mprotect()
	and madvise() (airos-works/haiku#163, #162).

	fork() treated such an area as private memory unless it was a clone: the
	child got a copy-on-write cache over the device's (with nothing mapped, so
	it read zeros), and the parent's mapping became copy-on-write too, so
	that its next write put a zeroed page of RAM where the device was. Device
	memory has no pages to copy; the child now gets the mapping itself.
	mprotect() looked up the vm_page of every mapped page, which device memory
	does not have (a panic), or which belongs to no cache where the "device"
	is RAM (the page stayed read-only, and the write that followed mapped a
	page of RAM as well).

	The "device" here is RAM of the test's own: a contiguous area, whose
	physical address the poke driver maps a second time (POKE_MAP_MEMORY, as
	a driver would map a frame buffer). What is written through the mapping
	must show in the area and the other way round. Needs /dev/misc/poke
	(root); without it the test says so and passes.

	device_area_fork_test [--no-ram <physical address>]

	--no-ram also maps a page at a physical address the kernel has no
	vm_page for (give one beyond the end of RAM, hexadecimal) and changes
	its protection; the page is never accessed. This is the case that
	panicked ("looking up page failed"). Not done by default, as only who
	runs the test knows such an address.
*/


#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>

#include <poke.h>


static const size_t kPages = 8;
static const size_t kSize = kPages * B_PAGE_SIZE;
static const bigtime_t kDeadline = 10 * 1000000LL;

static int sPoke = -1;
static uint8* sRAM;
	// the "device": what the mappings must show
static phys_addr_t sPhysicalAddress;
static int sFailures = 0;


#define CHECK(condition, ...) \
	do { \
		if (!(condition)) { \
			printf("FAILED %s:%d: ", __func__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
			sFailures++; \
		} \
	} while (false)


static void
crash_handler(int signal)
{
	// without a handler the crash dialog comes up and the team stays
	static const char kMessage[] = "FAILED: crashed\n";
	write(STDOUT_FILENO, kMessage, sizeof(kMessage) - 1);
	_exit(2);
}


/*!	The "device": RAM that stays where it is, and its physical address.
	A forked child has to make its own (it gets a copy of the area).
*/
static void
setup_ram()
{
	area_id ramArea = create_area("device test ram", (void**)&sRAM,
		B_ANY_ADDRESS, kSize, B_CONTIGUOUS, B_READ_AREA | B_WRITE_AREA);
	if (ramArea < 0) {
		printf("FAILED: create_area: %s\n", strerror(ramArea));
		exit(1);
	}
	memset(sRAM, 'a', kSize);

	mem_map_args args;
	memset(&args, 0, sizeof(args));
	args.signature = POKE_SIGNATURE;
	args.address = sRAM;
	args.size = kSize;
	if (ioctl(sPoke, POKE_GET_PHYSICAL_ADDRESS, &args, sizeof(args)) < 0
		|| args.size < kSize) {
		printf("FAILED: POKE_GET_PHYSICAL_ADDRESS: %s\n", strerror(errno));
		exit(1);
	}
	sPhysicalAddress = args.physical_address;
}


static uint8*
map_device(phys_addr_t physicalAddress, size_t size, uint32 protection,
	area_id* _area = NULL)
{
	mem_map_args args;
	memset(&args, 0, sizeof(args));
	args.signature = POKE_SIGNATURE;
	args.name = "device test mapping";
	args.physical_address = physicalAddress;
	args.size = size;
	args.flags = B_ANY_ADDRESS;
	args.protection = protection;
	area_id area = ioctl(sPoke, POKE_MAP_MEMORY, &args, sizeof(args));
	if (area < 0) {
		printf("FAILED: POKE_MAP_MEMORY: %s\n", strerror(errno));
		exit(1);
	}
	if (_area != NULL)
		*_area = area;
	return (uint8*)args.address;
}


static uint8*
map_ram(uint32 protection = B_READ_AREA | B_WRITE_AREA, area_id* _area = NULL)
{
	memset(sRAM, 'a', kSize);
	return map_device(sPhysicalAddress, kSize, protection, _area);
}


static void
unmap_device(uint8* address)
{
	delete_area(area_for(address));
}


static uint8&
page(uint8* base, size_t index)
{
	return base[index * B_PAGE_SIZE + 16];
}


/*!	After a fork() the child and the parent still have the device. */
static void
test_fork()
{
	uint8* device = map_ram();
	page(device, 0) = 'b';
	CHECK(page(sRAM, 0) == 'b', "the mapping is not the area's memory");

	pid_t child = fork();
	if (child == 0) {
		int result = page(device, 0) == 'b' ? 0 : 1;
		page(device, 1) = 'c';
		_exit(result);
	}
	int status;
	waitpid(child, &status, 0);
	const bool childRead = WIFEXITED(status) && WEXITSTATUS(status) == 0;

	page(device, 2) = 'd';

	printf("fork(): the child read the device: %s; device has '%c' of the "
		"child's 'c' and '%c' of the parent's 'd'\n", childRead ? "yes" : "no",
		page(sRAM, 1), page(sRAM, 2));
	CHECK(childRead, "the child does not see the device's memory");
	CHECK(page(sRAM, 1) == 'c', "the child's write did not reach the device");
	CHECK(page(sRAM, 2) == 'd',
		"the parent's write after fork() did not reach the device");

	area_info info;
	CHECK(get_area_info(area_for(device), &info) == B_OK && info.ram_size == 0,
		"the mapping holds %" B_PRIu32 " bytes of RAM now", info.ram_size);

	unmap_device(device);
}


/*!	A read-only mapping, which fork() does not need to make copy-on-write:
	the child still has to get the device, not an empty cache.
*/
static void
test_fork_read_only()
{
	uint8* device = map_ram(B_READ_AREA);

	int go[2];
	if (pipe(go) != 0) {
		CHECK(false, "pipe");
		return;
	}
	pid_t child = fork();
	if (child == 0) {
		close(go[1]);
		char c;
		read(go[0], &c, 1);
		_exit(page(device, 0) == 'a' && page(device, 3) == 'x' ? 0 : 1);
	}
	close(go[0]);
	page(sRAM, 3) = 'x';
	close(go[1]);

	int status;
	waitpid(child, &status, 0);
	const bool childRead = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	printf("fork() with a read-only mapping: the child read the device: %s\n",
		childRead ? "yes" : "no");
	CHECK(childRead, "the child does not see the device's memory");

	unmap_device(device);
}


/*!	mprotect() of the mapping and of a part of it, and a fork() after it. */
static void
test_mprotect()
{
	uint8* device = map_ram();

	CHECK(mprotect(device, kSize, PROT_READ) == 0, "mprotect: %s",
		strerror(errno));
	CHECK(page(device, 0) == 'a', "read-only mapping reads '%c'",
		page(device, 0));
	CHECK(mprotect(device, kSize, PROT_READ | PROT_WRITE) == 0, "mprotect: %s",
		strerror(errno));
	page(device, 3) = 'e';
	printf("mprotect() to read-only and back, 'e' written: device has '%c'\n",
		page(sRAM, 3));
	CHECK(page(sRAM, 3) == 'e', "the write did not reach the device");

	// one page without access and back
	CHECK(mprotect(device + 4 * B_PAGE_SIZE, B_PAGE_SIZE, PROT_NONE) == 0,
		"mprotect: %s", strerror(errno));
	CHECK(mprotect(device + 4 * B_PAGE_SIZE, B_PAGE_SIZE,
			PROT_READ | PROT_WRITE) == 0, "mprotect: %s", strerror(errno));
	page(device, 4) = 'f';
	page(device, 5) = 'g';
	printf("mprotect() of one page to none and back, 'f' and 'g' written: "
		"device has '%c' and '%c'\n", page(sRAM, 4), page(sRAM, 5));
	CHECK(page(sRAM, 4) == 'f' && page(sRAM, 5) == 'g',
		"the write did not reach the device");

	// the mapping has protections per page now: fork() again
	CHECK(mprotect(device + 6 * B_PAGE_SIZE, B_PAGE_SIZE, PROT_READ) == 0,
		"mprotect: %s", strerror(errno));
	pid_t child = fork();
	if (child == 0) {
		int result = page(device, 4) == 'f' && page(device, 6) == 'a' ? 0 : 1;
		page(device, 7) = 'h';
		_exit(result);
	}
	int status;
	waitpid(child, &status, 0);
	const bool childRead = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	page(device, 0) = 'i';
	printf("fork() with protections per page: the child read the device: %s; "
		"device has '%c' of the child's 'h' and '%c' of the parent's 'i'\n",
		childRead ? "yes" : "no", page(sRAM, 7), page(sRAM, 0));
	CHECK(childRead, "the child does not see the device's memory");
	CHECK(page(sRAM, 7) == 'h', "the child's write did not reach the device");
	CHECK(page(sRAM, 0) == 'i',
		"the parent's write after fork() did not reach the device");

	unmap_device(device);
}


/*!	A clone of the mapping, which is how most programs get device memory. */
static void
test_clone()
{
	uint8* device = map_ram();

	uint8* clone;
	area_id cloneArea = clone_area("device test clone", (void**)&clone,
		B_ANY_ADDRESS, B_READ_AREA | B_WRITE_AREA, area_for(device));
	if (cloneArea < 0) {
		CHECK(false, "clone_area: %s", strerror(cloneArea));
		unmap_device(device);
		return;
	}

	page(clone, 1) = 'j';
	pid_t child = fork();
	if (child == 0) {
		int result = page(clone, 1) == 'j' && page(device, 1) == 'j' ? 0 : 1;
		page(clone, 2) = 'k';
		page(device, 3) = 'l';
		_exit(result);
	}
	int status;
	waitpid(child, &status, 0);
	const bool childRead = WIFEXITED(status) && WEXITSTATUS(status) == 0;
	page(clone, 4) = 'm';
	page(device, 5) = 'n';
	printf("clone and fork(): the child read the device: %s; device has "
		"'%c%c' of the child's 'kl' and '%c%c' of the parent's 'mn'\n",
		childRead ? "yes" : "no", page(sRAM, 2), page(sRAM, 3), page(sRAM, 4),
		page(sRAM, 5));
	CHECK(childRead, "the child does not see the device's memory");
	CHECK(page(sRAM, 1) == 'j' && page(sRAM, 2) == 'k' && page(sRAM, 3) == 'l'
			&& page(sRAM, 4) == 'm' && page(sRAM, 5) == 'n',
		"a write did not reach the device");

	delete_area(cloneArea);
	unmap_device(device);
}


/*!	madvise() has nothing to free in device memory and has to leave the
	mapping usable (#162: MADV_FREE left its cache locked, and the team hung
	when it deleted the area). In a child with a deadline.
*/
static void
test_madvise(int advice)
{
	const char* adviceName = advice == MADV_FREE ? "MADV_FREE" : "MADV_DONTNEED";
	fflush(stdout);

	pid_t child = fork();
	if (child == 0) {
		setup_ram();
		uint8* device = map_ram();
		int result = 0;
		if (madvise(device, kSize, advice) != 0)
			result = 1;
		if (page(device, 0) != 'a')
			result = 2;
		page(device, 1) = 'o';
		if (page(sRAM, 1) != 'o')
			result = 3;
		unmap_device(device);
		_exit(result);
	}

	const bigtime_t deadline = system_time() + kDeadline;
	int status;
	while (waitpid(child, &status, WNOHANG) != child) {
		if (system_time() > deadline) {
			// not killed: see madvise_shared_cache_test
			printf("madvise(%s): no answer after %d s (team %d is left "
				"behind)\n", adviceName, (int)(kDeadline / 1000000), (int)child);
			CHECK(false, "madvise() on device memory hangs the team");
			return;
		}
		snooze(20000);
	}

	printf("madvise(%s) on the mapping, then a write and unmapping: result "
		"%d\n", adviceName, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"madvise() failed or the mapping lost the device");
}


/*!	Memory the kernel has no vm_page for: the protection can be changed. */
static void
test_no_ram(phys_addr_t physicalAddress)
{
	printf("mapping %#" B_PRIxPHYSADDR " (not accessed) and changing its "
		"protection\n", physicalAddress);
	uint8* device = map_device(physicalAddress, 2 * B_PAGE_SIZE, B_READ_AREA);
	CHECK(mprotect(device, 2 * B_PAGE_SIZE, PROT_NONE) == 0, "mprotect: %s",
		strerror(errno));
	CHECK(mprotect(device, B_PAGE_SIZE, PROT_READ) == 0, "mprotect: %s",
		strerror(errno));

	pid_t child = fork();
	if (child == 0)
		_exit(0);
	int status;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "fork");

	unmap_device(device);
	printf("mprotect() and fork() with memory without pages: done\n");
}


int
main(int argc, char** argv)
{
	phys_addr_t noRAMAddress = 0;
	if (argc == 3 && strcmp(argv[1], "--no-ram") == 0) {
		noRAMAddress = strtoull(argv[2], NULL, 16);
		if (noRAMAddress == 0 || noRAMAddress % B_PAGE_SIZE != 0) {
			fprintf(stderr, "%s: not a page's physical address\n", argv[2]);
			return 1;
		}
	} else if (argc != 1) {
		fprintf(stderr, "usage: %s [--no-ram <physical address>]\n", argv[0]);
		return 1;
	}

	setvbuf(stdout, NULL, _IONBF, 0);
	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);

	sPoke = open(POKE_DEVICE_FULLNAME, O_RDWR);
	if (sPoke < 0) {
		printf("SKIPPED: %s: %s\n", POKE_DEVICE_FULLNAME, strerror(errno));
		return 0;
	}

	setup_ram();

	test_fork();
	test_fork_read_only();
	test_mprotect();
	test_clone();
	test_madvise(MADV_DONTNEED);
	test_madvise(MADV_FREE);
	if (noRAMAddress != 0)
		test_no_ram(noRAMAddress);

	if (sFailures != 0) {
		printf("%d check(s) FAILED\n", sFailures);
		return 1;
	}

	printf("all checks passed\n");
	return 0;
}
