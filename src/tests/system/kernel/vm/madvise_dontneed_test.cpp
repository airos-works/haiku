/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	MADV_DONTNEED frees private memory, which then reads as zeros (anonymous)
	or as the file (private file mappings); shared memory keeps its data, as
	does memory given POSIX_MADV_DONTNEED.
*/


#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <OS.h>


static const size_t kPages = 256;
static const size_t kSize = kPages * B_PAGE_SIZE;

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


static size_t
ram_size(void* address)
{
	area_info info;
	if (get_area_info(area_for(address), &info) != B_OK)
		return (size_t)-1;
	return info.ram_size;
}


static bool
all_bytes(const uint8* data, size_t size, uint8 value)
{
	for (size_t i = 0; i < size; i++) {
		if (data[i] != value)
			return false;
	}
	return true;
}


static void
test_private_anonymous()
{
	uint8* data = (uint8*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(data, 0x5a, kSize);
	CHECK(ram_size(data) == kSize, "ram_size %zu before", ram_size(data));

	// free the middle half
	CHECK(madvise(data + kSize / 4, kSize / 2, MADV_DONTNEED) == 0, "madvise");
	CHECK(ram_size(data) == kSize / 2, "ram_size %zu after",
		ram_size(data));
	CHECK(all_bytes(data + kSize / 4, kSize / 2, 0), "freed part not zero");
	CHECK(all_bytes(data, kSize / 4, 0x5a), "start changed");
	CHECK(all_bytes(data + kSize * 3 / 4, kSize / 4, 0x5a), "end changed");

	// the memory is usable again
	memset(data, 0x33, kSize);
	CHECK(all_bytes(data, kSize, 0x33), "rewrite");

	munmap(data, kSize);
}


static void
test_private_file()
{
	char path[] = "/tmp/madvise_dontneed_XXXXXX";
	int fd = mkstemp(path);
	uint8* page = (uint8*)malloc(B_PAGE_SIZE);
	memset(page, 0x11, B_PAGE_SIZE);
	for (size_t i = 0; i < kPages; i++)
		write(fd, page, B_PAGE_SIZE);
	free(page);

	uint8* data = (uint8*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE, fd, 0);
	memset(data, 0x22, kSize);

	CHECK(madvise(data, kSize, MADV_DONTNEED) == 0, "madvise");
	CHECK(all_bytes(data, kSize, 0x11), "private copy not dropped");

	munmap(data, kSize);
	close(fd);
	unlink(path);
}


static void
test_shared_anonymous()
{
	uint8* data = (uint8*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	memset(data, 0x44, kSize);

	CHECK(madvise(data, kSize, MADV_DONTNEED) == 0, "madvise");
	CHECK(all_bytes(data, kSize, 0x44), "shared memory lost its data");

	munmap(data, kSize);
}


static void
test_posix_madvise()
{
	uint8* data = (uint8*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(data, 0x66, kSize);

	CHECK(posix_madvise(data, kSize, POSIX_MADV_DONTNEED) == 0,
		"posix_madvise");
	CHECK(all_bytes(data, kSize, 0x66), "POSIX_MADV_DONTNEED changed data");

	// posix_madvise() returns the error rather than -1
	CHECK(posix_madvise(data + 1, kSize, POSIX_MADV_NORMAL) == EINVAL,
		"unaligned address");

	munmap(data, kSize);
}


static void
test_after_fork()
{
	uint8* data = (uint8*)mmap(NULL, kSize, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	memset(data, 0x77, kSize);
	int toChild[2], toParent[2];
	pipe(toChild);
	pipe(toParent);

	fflush(stdout);
	pid_t child = fork();
	if (child == 0) {
		char byte;
		write(toParent[1], "r", 1);
		read(toChild[0], &byte, 1);
		// the parent's madvise() does not touch the child's copy
		exit(all_bytes(data, kSize, 0x77) ? 0 : 1);
	}

	char byte;
	read(toParent[0], &byte, 1);
	// one page copied after the fork, the rest still shared with the child
	data[0] = 0x78;

	CHECK(madvise(data, kSize, MADV_DONTNEED) == 0, "madvise");
	CHECK(all_bytes(data, kSize, 0), "pages shared with the child not zero");

	write(toChild[1], "g", 1);
	int status;
	waitpid(child, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"child's copy changed");

	// with the child gone, the memory is freed again (its address space may
	// go away a little after waitpid())
	int attempts = 0;
	do {
		if (attempts > 0)
			snooze(10000);
		memset(data, 0x79, kSize);
		CHECK(madvise(data, kSize, MADV_DONTNEED) == 0, "madvise");
	} while (ram_size(data) != 0 && ++attempts < 100);
	CHECK(ram_size(data) == 0, "ram_size %zu after the child left",
		ram_size(data));
	CHECK(all_bytes(data, kSize, 0), "not zero after the child left");

	munmap(data, kSize);
}


int
main()
{
	test_private_anonymous();
	test_private_file();
	test_shared_anonymous();
	test_posix_madvise();
	test_after_fork();

	if (sFailures != 0) {
		printf("%d checks failed\n", sFailures);
		return 1;
	}

	printf("all checks passed\n");
	return 0;
}
