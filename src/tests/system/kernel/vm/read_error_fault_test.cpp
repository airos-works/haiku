/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	A page fault next to a page of a mapped file that cannot be read
	(airos-works/haiku#197).

	A fault on file data reads the missing pages around the faulting one in
	the same request (#98). When that request failed, the fault failed, even
	though the page it was for could be read: a program with one bad block
	in its file crashed on up to 255 good pages around it. The fault now reads
	its own page alone when the larger read fails.

	The test needs a file with exactly one unreadable page, on a volume that
	was just mounted (nothing of the file in the cache), which takes a rig:

	  read_error_fault_test --create <file>
	      writes the file (4 MiB; every page starts with the text
	      "read-error-page-<index>;"), on a scratch volume.
	  Unmount the volume, find the block of page 600 in the image
	  (grep -abo 'read-error-page-00000600;'), and attach the image again
	  through something that fails reads of that block. With QEMU:
	      [inject-error]
	      event = "read_aio"
	      errno = "5"
	      sector = "<offset / 512>"
	  in a file, and -drive file=blkdebug:<that file>:<image>,format=raw.
	  read_error_fault_test <file> 600
	      reads the pages around page 600 through a mapping, then page 600.

	It passes when every page but the bad one reads with its contents, and
	the bad one fails. When the bad page reads as well no error was injected,
	and the test fails as it has tested nothing.
*/


#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>


static const size_t kFileSize = 4 * 1024 * 1024;
static const size_t kMarkerLength = 25;

static sigjmp_buf sJumpBuffer;


static void
fault_handler(int signal)
{
	siglongjmp(sJumpBuffer, signal);
}


static void
page_marker(size_t index, char* marker)
{
	snprintf(marker, kMarkerLength + 1, "read-error-page-%08zu;", index);
}


static int
create_file(const char* path)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}

	char page[B_PAGE_SIZE];
	for (size_t index = 0; index < kFileSize / B_PAGE_SIZE; index++) {
		memset(page, 'a' + index % 26, sizeof(page));
		page_marker(index, page);
		page[kMarkerLength] = '\n';
		if (write(fd, page, sizeof(page)) != (ssize_t)sizeof(page)) {
			fprintf(stderr, "writing %s: %s\n", path, strerror(errno));
			return 1;
		}
	}

	if (fsync(fd) != 0) {
		fprintf(stderr, "fsync: %s\n", strerror(errno));
		return 1;
	}
	close(fd);
	printf("%s: %zu pages written\n", path, kFileSize / B_PAGE_SIZE);
	return 0;
}


/*!	Reads the page through the mapping. Returns 1 when its contents are
	right, 0 when they are wrong, and -1 when the access faulted.
*/
static int
read_page(const char* data, size_t index)
{
	char marker[kMarkerLength + 1];
	page_marker(index, marker);

	if (sigsetjmp(sJumpBuffer, 1) != 0)
		return -1;

	const char* page = data + index * B_PAGE_SIZE;
	volatile char last = page[B_PAGE_SIZE - 1];
	if (memcmp(page, marker, kMarkerLength) != 0
		|| last != (char)('a' + index % 26)) {
		return 0;
	}
	return 1;
}


int
main(int argc, char** argv)
{
	if (argc == 3 && strcmp(argv[1], "--create") == 0)
		return create_file(argv[2]);
	if (argc != 3) {
		fprintf(stderr, "usage: %s --create <file>\n"
			"       %s <file> <index of the unreadable page>\n", argv[0],
			argv[0]);
		return 1;
	}

	const char* path = argv[1];
	const size_t badPage = strtoul(argv[2], NULL, 0);
	const size_t pageCount = kFileSize / B_PAGE_SIZE;
	if (badPage < 32 || badPage + 32 >= pageCount) {
		fprintf(stderr, "the unreadable page has to be between 32 and %zu\n",
			pageCount - 32);
		return 1;
	}

	setvbuf(stdout, NULL, _IONBF, 0);

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "%s: %s\n", path, strerror(errno));
		return 1;
	}
	const char* data = (const char*)mmap(NULL, kFileSize, PROT_READ,
		MAP_PRIVATE, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return 1;
	}

	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = fault_handler;
	sigaction(SIGSEGV, &action, NULL);
	sigaction(SIGBUS, &action, NULL);

	int failures = 0;

	// The neighbours first: each of these faults reads around itself, and
	// the bad page is in what it reads. Then everything else near it.
	const size_t first = badPage - 32;
	const size_t last = badPage + 32;
	for (int pass = 0; pass < 2; pass++) {
		for (size_t index = first; index <= last; index++) {
			const bool neighbour = index + 1 == badPage || index == badPage + 1;
			if (index == badPage || neighbour != (pass == 0))
				continue;

			const bigtime_t start = system_time();
			const int result = read_page(data, index);
			const bigtime_t duration = system_time() - start;
			if (neighbour || result != 1) {
				printf("page %zu%s: %s (%" B_PRIdBIGTIME " ms)\n", index,
					neighbour ? " (next to the unreadable one)" : "",
					result == 1 ? "read"
						: result == 0 ? "FAILED: wrong contents"
						: "FAILED: the access faulted", duration / 1000);
			}
			if (result != 1)
				failures++;
		}
	}
	printf("pages %zu to %zu but page %zu: %d could not be read\n", first,
		last, badPage, failures);

	const int result = read_page(data, badPage);
	if (result == -1)
		printf("page %zu: the access faulted, as it has to\n", badPage);
	else {
		printf("page %zu: FAILED: it was read (%s): no read error was "
			"injected, nothing was tested\n", badPage,
			result == 1 ? "with its contents" : "wrong contents");
		failures++;
	}

	if (failures != 0) {
		printf("FAILED\n");
		return 1;
	}

	printf("passed\n");
	return 0;
}
