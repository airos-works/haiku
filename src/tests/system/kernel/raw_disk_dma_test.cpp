/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Data through a disk driver's DMA paths (airos-works/haiku#181): writes a
	pattern to a raw device with requests of many sizes, from buffers of
	every alignment (page, sector, cache line, 4 bytes, odd) and with
	scattered vectors (writev), reads it back with a different mix, and
	compares every byte. Optionally measures sequential throughput.

		raw_disk_dma_test <raw device> [MiB to test (default 32)]
			[--bench MiB] [--seed N] [--offset MiB]

	THE DEVICE'S CONTENTS IN THAT RANGE ARE OVERWRITTEN. Exits 0 when every
	byte read back is what was written, 1 on a difference or an error, 2 when
	it took too long (a hung transfer).
*/


#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <unistd.h>

#include <OS.h>


static uint64_t sSeed = 181;
static int sFailures = 0;


static void
timeout(int)
{
	static const char kMessage[] = "raw_disk_dma_test: FAIL: timed out "
		"(a transfer hangs)\n";
	write(STDOUT_FILENO, kMessage, sizeof(kMessage) - 1);
	_exit(2);
}


static inline uint8_t
pattern_byte(uint64_t position)
{
	// a different value for every byte, and for every seed
	uint64_t x = (position >> 3) * 0x9e3779b97f4a7c15ULL ^ sSeed;
	x ^= x >> 29;
	x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 32;
	return (uint8_t)(x >> ((position & 7) * 8));
}


static void
fill(uint8_t* buffer, uint64_t position, size_t size)
{
	for (size_t i = 0; i < size; i++)
		buffer[i] = pattern_byte(position + i);
}


static bool
check(const uint8_t* buffer, uint64_t position, size_t size, const char* what)
{
	for (size_t i = 0; i < size; i++) {
		if (buffer[i] != pattern_byte(position + i)) {
			size_t bad = 0;
			for (size_t j = i; j < size; j++)
				bad += buffer[j] != pattern_byte(position + j);
			printf("  MISMATCH %s: at byte %llu (request at %llu, %zu bytes): "
				"read %#x, expected %#x; %zu bytes differ\n", what,
				(unsigned long long)(position + i),
				(unsigned long long)position, size, buffer[i],
				pattern_byte(position + i), bad);
			sFailures++;
			return false;
		}
	}
	return true;
}


// Buffers at these distances from a page boundary
static const size_t kMisalignments[] = { 0, 512, 64, 4, 1, 2048 + 4, 4096 - 512 };
static const size_t kMisalignmentCount
	= sizeof(kMisalignments) / sizeof(kMisalignments[0]);

// Request sizes: single blocks, pages, odd numbers of blocks, the old
// 128 KiB arm64 limit and around it, around 512 KiB, several MiB
static const size_t kWriteSizes[] = { 512, 4096, 4608, 65536, 131072 + 512,
	1048576, 3 * 512, 524288, 524288 + 4096, 3 * 1048576 + 1024, 8192,
	131072 };
static const size_t kReadSizes[] = { 1048576, 512, 65536 + 512, 4096,
	2 * 1048576, 131072, 7 * 512, 524288 - 512, 16384, 393216 };


struct Buffer {
	Buffer(size_t size)
	{
		fBase = (uint8_t*)malloc(size + 2 * B_PAGE_SIZE);
		fAligned = (uint8_t*)(((uintptr_t)fBase + B_PAGE_SIZE - 1)
			& ~(uintptr_t)(B_PAGE_SIZE - 1));
	}
	~Buffer() { free(fBase); }
	uint8_t* At(size_t misalignment) { return fAligned + misalignment; }

	uint8_t* fBase;
	uint8_t* fAligned;
};


static bool
do_io(int fd, bool write, uint64_t position, void* buffer, size_t size)
{
	ssize_t done = write ? pwrite(fd, buffer, size, position)
		: pread(fd, buffer, size, position);
	if (done != (ssize_t)size) {
		printf("  %s of %zu bytes at %llu: %s\n", write ? "write" : "read",
			size, (unsigned long long)position,
			done < 0 ? strerror(errno) : "short transfer");
		sFailures++;
		return false;
	}
	return true;
}


/*!	A request in several vectors: pieces of 512, 3584, 4096 bytes and odd
	sizes, each from its own place in \a buffer, some of them misaligned.
	Returns how many bytes it transferred (up to 64 pieces' worth of
	\a size), 0 on an error.
*/
static size_t
do_vectors(int fd, bool write, uint64_t position, Buffer& buffer,
	size_t size, int round)
{
	static const size_t kPieces[] = { 512, 3584, 4096, 1000, 12288, 24,
		65536, 2560 };
	iovec vecs[64];
	int count = 0;
	size_t total = 0;
	uint8_t* next = buffer.At(0);
	while (total < size && count < 64) {
		size_t piece = kPieces[(round + count) % 8];
		if (piece > size - total)
			piece = size - total;
		// a gap and, every other piece, a misalignment between pieces
		next += (count & 1) != 0 ? 4096 + 4 : 4096;
		next = (uint8_t*)(((uintptr_t)next + 3) & ~(uintptr_t)3);
		vecs[count].iov_base = next;
		vecs[count].iov_len = piece;
		if (write)
			fill(next, position + total, piece);
		else
			memset(next, 0x5a, piece);
		next += piece;
		total += piece;
		count++;
	}
	ssize_t done = write ? writev_pos(fd, position, vecs, count)
		: readv_pos(fd, position, vecs, count);
	if (done != (ssize_t)total) {
		printf("  %s of %d vectors (%zu bytes) at %llu: %s\n",
			write ? "writev" : "readv", count, total,
			(unsigned long long)position,
			done < 0 ? strerror(errno) : "short transfer");
		sFailures++;
		return 0;
	}
	if (!write) {
		size_t offset = 0;
		for (int i = 0; i < count; i++) {
			if (!check((uint8_t*)vecs[i].iov_base, position + offset,
					vecs[i].iov_len, "readv"))
				return 0;
			offset += vecs[i].iov_len;
		}
	}
	return total;
}


static double
now()
{
	return system_time() / 1000000.0;
}


static void
bench(int fd, uint64_t start, uint64_t size)
{
	static const size_t kSizes[] = { 1048576, 65536, 4096 };
	Buffer buffer(1048576);
	fill(buffer.At(0), 0, 1048576);
	for (size_t s = 0; s < 3; s++) {
		size_t request = kSizes[s];
		uint64_t bytes = request == 4096 ? size / 8 : size;
		double begin = now();
		for (uint64_t done = 0; done < bytes; done += request)
			do_io(fd, true, start + done, buffer.At(0), request);
		fsync(fd);
		double written = now() - begin;
		begin = now();
		for (uint64_t done = 0; done < bytes; done += request)
			do_io(fd, false, start + done, buffer.At(0), request);
		double read = now() - begin;
		printf("bench: %7zu-byte requests, %llu MiB: write %.1f MB/s, "
			"read %.1f MB/s\n", request,
			(unsigned long long)(bytes >> 20), bytes / written / 1e6,
			bytes / read / 1e6);
	}
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <raw device> [MiB] [--bench MiB] "
			"[--seed N] [--offset MiB]\n", argv[0]);
		return 1;
	}
	const char* device = argv[1];
	uint64_t size = 32ULL << 20;
	uint64_t benchSize = 0;
	uint64_t start = 0;
	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc)
			benchSize = strtoull(argv[++i], NULL, 0) << 20;
		else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc)
			sSeed = strtoull(argv[++i], NULL, 0);
		else if (strcmp(argv[i], "--offset") == 0 && i + 1 < argc)
			start = strtoull(argv[++i], NULL, 0) << 20;
		else
			size = strtoull(argv[i], NULL, 0) << 20;
	}

	// line by line also into a pipe or a log, to see where it got
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGALRM, timeout);
	alarm(1200);

	int fd = open(device, O_RDWR);
	if (fd < 0) {
		printf("raw_disk_dma_test: FAIL: %s: %s\n", device, strerror(errno));
		return 1;
	}

	Buffer buffer(4 * 1048576);
	Buffer vectors(64 * (65536 + 8192));

	// 1. Write the whole range with a mix of request sizes, alignments and
	//    vectored requests, and a few that do not start or end on a block.
	double begin = now();
	uint64_t position = start;
	int round = 0;
	while (position < start + size && sFailures == 0) {
		size_t request = kWriteSizes[round % (sizeof(kWriteSizes)
			/ sizeof(kWriteSizes[0]))];
		if (request > start + size - position)
			request = start + size - position;
		if (round % 5 == 4) {
			request = do_vectors(fd, true, position, vectors, request, round);
			if (request == 0)
				break;
		} else {
			uint8_t* data = buffer.At(kMisalignments[round % kMisalignmentCount]);
			fill(data, position, request);
			do_io(fd, true, position, data, request);
		}
		position += request;
		round++;
	}
	// partial blocks: rewrite a few pieces that start and end inside one
	for (uint64_t i = 1; i <= 8 && sFailures == 0; i++) {
		uint64_t at = start + i * (size / 10) + 100 * i;
		size_t request = 1000 * i + 37;
		if (at + request > start + size)
			continue;
		uint8_t* data = buffer.At(kMisalignments[i % kMisalignmentCount]);
		fill(data, at, request);
		do_io(fd, true, at, data, request);
	}
	if (fsync(fd) != 0) {
		printf("  fsync: %s\n", strerror(errno));
		sFailures++;
	}
	printf("wrote %llu MiB in %d requests (%.2f s)\n",
		(unsigned long long)(size >> 20), round, now() - begin);

	// 2. Read it back with another mix, and compare every byte.
	begin = now();
	position = start;
	round = 0;
	while (position < start + size && sFailures < 4) {
		size_t request = kReadSizes[round % (sizeof(kReadSizes)
			/ sizeof(kReadSizes[0]))];
		if (request > start + size - position)
			request = start + size - position;
		if (round % 4 == 3) {
			request = do_vectors(fd, false, position, vectors, request, round);
			if (request == 0)
				break;
		} else {
			uint8_t* data = buffer.At(kMisalignments[(round + 3)
				% kMisalignmentCount]);
			memset(data, 0xa5, request);
			if (do_io(fd, false, position, data, request))
				check(data, position, request, "read");
		}
		position += request;
		round++;
	}
	// and pieces that start and end inside a block
	for (uint64_t i = 1; i <= 16 && sFailures < 4; i++) {
		uint64_t at = start + i * (size / 17) + 77 * i;
		size_t request = 700 * i + 11;
		if (at + request > start + size)
			continue;
		uint8_t* data = buffer.At(kMisalignments[i % kMisalignmentCount]);
		if (do_io(fd, false, at, data, request))
			check(data, at, request, "partial read");
	}
	printf("read back %llu MiB in %d requests (%.2f s)\n",
		(unsigned long long)(size >> 20), round, now() - begin);

	if (benchSize != 0 && sFailures == 0)
		bench(fd, start, benchSize);

	close(fd);
	if (sFailures != 0) {
		printf("raw_disk_dma_test: FAIL: %d errors\n", sFailures);
		return 1;
	}
	printf("raw_disk_dma_test: PASS: %llu MiB written and read back, every "
		"byte as written\n", (unsigned long long)(size >> 20));
	return 0;
}
