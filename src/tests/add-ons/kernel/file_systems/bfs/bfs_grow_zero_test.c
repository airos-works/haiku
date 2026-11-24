/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	Growing a file without writing it: ftruncate() to a larger size, and a
	write far past the end. BFS fills the new blocks with zeros, and used to do
	so inside the transaction that changed the size (airos-works/haiku#154):
	- nothing else could create, delete or extend a file on the volume until
	  the zeros were written (196 s for 16 GiB on a boot volume),
	- a write by another thread into the new range, started after the size
	  had changed, was overwritten by the zeros.

	bfs_grow_zero_test DIR [SIZE_MB]
		Run in a directory of a BFS volume that is NOT the boot volume, with
		room for twice SIZE_MB (default 1024). Leaves its files in place.
	bfs_grow_zero_test --verify DIR [SIZE_MB]
		Checks the files of an earlier run again; meant for after the volume
		has been unmounted and mounted, when they are read from the disk.

	Exits with 0 when the grown ranges read as zeros, every block written into
	them is intact, and creating a file elsewhere on the volume did not wait
	for the zeros.
*/


#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>


#define BLOCK			4096
#define RACE_STEPS		24
#define RACE_STEP_SIZE	((off_t)32 << 20)

static char sDirectory[512];
static off_t sSize;
static int sFailures;

static volatile int sToucherDone;
static double sWorstCreate;
static int sCreates;

static int sRaceFD;
static volatile int sRaceDone;
static off_t sRaceWritten[RACE_STEPS + 1];
static int sRaceWrites;


static double
now(void)
{
	struct timespec time;
	clock_gettime(CLOCK_MONOTONIC, &time);
	return time.tv_sec + time.tv_nsec / 1e9;
}


static const char*
path(const char* name)
{
	static char buffers[4][600];
	static int next;
	char* buffer = buffers[next++ % 4];
	snprintf(buffer, 600, "%s/%s", sDirectory, name);
	return buffer;
}


static void
failed(const char* what)
{
	printf("FAIL %s\n", what);
	sFailures++;
}


static void
timed_out(int signal)
{
	static const char message[] = "FAIL the test did not finish in time\n";
	write(1, message, sizeof(message) - 1);
	_exit(1);
}


static void
fill_pattern(uint8_t* block, off_t position)
{
	int i;
	for (i = 0; i < BLOCK; i += 8) {
		uint64_t value = (uint64_t)position + i + 0x5a5a000000000000ULL;
		memcpy(block + i, &value, 8);
	}
}


/*!	Creates and removes small files, the way every other program on the
	volume would, and keeps the longest time one such round took.
*/
static void*
toucher(void* arg)
{
	char name[64];
	int i = 0;

	while (!sToucherDone) {
		double start;
		double duration;
		int fd;

		snprintf(name, sizeof(name), "touch-%d", i++ % 16);
		start = now();
		fd = open(path(name), O_CREAT | O_WRONLY, 0644);
		if (fd >= 0)
			close(fd);
		unlink(path(name));
		duration = now() - start;
		if (duration > sWorstCreate)
			sWorstCreate = duration;
		sCreates++;
		usleep(10000);
	}
	return NULL;
}


/*!	Runs \a grow (which grows the file \a fd to sSize) while a thread creates
	small files next to it.
*/
static void
measure_stall(const char* what, int fd, int (*grow)(int))
{
	pthread_t thread;
	double start;
	double duration;
	int result;

	sToucherDone = 0;
	sWorstCreate = 0;
	sCreates = 0;
	pthread_create(&thread, NULL, toucher, NULL);
	usleep(300000);
	printf("%s: creating a file in the same directory took %.0f ms at worst "
		"before (%d rounds)\n", what, sWorstCreate * 1000, sCreates);
	sWorstCreate = 0;
	sCreates = 0;

	start = now();
	result = grow(fd);
	duration = now() - start;

	sToucherDone = 1;
	pthread_join(thread, NULL);

	printf("%s to %lld MiB: %s, took %.2f s; creating a file meanwhile took "
		"%.0f ms at worst (%d rounds)\n", what, (long long)(sSize >> 20),
		result == 0 ? "ok" : strerror(errno), duration, sWorstCreate * 1000,
		sCreates);
	if (result != 0)
		failed(what);

	// The zeros take as long as they take; what must not be is that a create
	// waits for (most of) them.
	if (sWorstCreate > 1.0 && sWorstCreate > duration / 2) {
		printf("FAIL %s: creating a file waited %.1f s of the %.1f s\n", what,
			sWorstCreate, duration);
		sFailures++;
	} else
		printf("ok   %s did not hold up creating files\n", what);
}


static int
grow_with_ftruncate(int fd)
{
	return ftruncate(fd, sSize);
}


static int
grow_with_write(int fd)
{
	return pwrite(fd, "x", 1, sSize - 1) == 1 ? 0 : -1;
}


/*!	Checks that \a name is \a size bytes of zeros, but for the blocks at
	\a patterns (\a patternCount of them, or one byte 'x' at the very end when
	\a lastByte is set).
*/
static void
verify_file(const char* name, off_t size, const off_t* patterns,
	int patternCount, int lastByte)
{
	static uint8_t buffer[1 << 20];
	static uint8_t expected[BLOCK];
	struct stat st;
	off_t position = 0;
	off_t firstBad = -1;
	off_t badBytes = 0;
	int lostPatterns = 0;
	int next = 0;
	int fd = open(path(name), O_RDONLY);

	if (fd < 0 || fstat(fd, &st) != 0) {
		printf("FAIL %s: %s\n", name, strerror(errno));
		sFailures++;
		return;
	}
	if (st.st_size != size) {
		printf("FAIL %s is %lld bytes, expected %lld\n", name,
			(long long)st.st_size, (long long)size);
		sFailures++;
	}

	while (position < size) {
		ssize_t bytes = read(fd, buffer, sizeof(buffer));
		ssize_t i;
		if (bytes <= 0) {
			printf("FAIL %s: read at %lld: %s\n", name, (long long)position,
				bytes < 0 ? strerror(errno) : "end of file");
			sFailures++;
			break;
		}

		for (i = 0; i < bytes; i += BLOCK) {
			off_t at = position + i;
			size_t length = bytes - i < BLOCK ? bytes - i : BLOCK;
			size_t k;
			if (next < patternCount && patterns[next] == at) {
				fill_pattern(expected, at);
				if (memcmp(buffer + i, expected, length) != 0) {
					if (lostPatterns++ == 0) {
						printf("     %s: the block written at %lld is gone "
							"(starts with %02x %02x %02x %02x)\n", name,
							(long long)at, buffer[i], buffer[i + 1],
							buffer[i + 2], buffer[i + 3]);
					}
				}
				next++;
				continue;
			}
			for (k = 0; k < length; k++) {
				if (buffer[i + k] == 0)
					continue;
				if (lastByte && at + (off_t)k == size - 1
					&& buffer[i + k] == 'x')
					continue;
				if (firstBad < 0)
					firstBad = at + k;
				badBytes++;
			}
		}
		position += bytes;
	}
	close(fd);

	if (lostPatterns != 0) {
		printf("FAIL %s: %d of %d blocks written into the new range were "
			"overwritten\n", name, lostPatterns, patternCount);
		sFailures++;
	} else if (patternCount != 0) {
		printf("ok   %s: all %d blocks written into the new range are there\n",
			name, patternCount);
	}
	if (badBytes != 0) {
		printf("FAIL %s: %lld bytes of the grown range are not zero, the "
			"first at %lld\n", name, (long long)badBytes, (long long)firstBad);
		sFailures++;
	} else {
		printf("ok   %s: %lld MiB read back as zeros\n", name,
			(long long)(position >> 20));
	}
}


/*!	Writes a block at the end of the file's new range as soon as the file
	has grown: after the size has changed, before the zeros get there.
*/
static void*
race_writer(void* arg)
{
	uint8_t block[BLOCK];
	off_t seen = 0;

	while (!sRaceDone || sRaceWrites < RACE_STEPS) {
		struct stat st;
		if (fstat(sRaceFD, &st) != 0 || st.st_size <= seen) {
			if (sRaceDone)
				break;
			continue;
		}
		seen = st.st_size;
		fill_pattern(block, seen - BLOCK);
		if (pwrite(sRaceFD, block, BLOCK, seen - BLOCK) == BLOCK)
			sRaceWritten[sRaceWrites++] = seen - BLOCK;
		if (sRaceWrites >= RACE_STEPS)
			break;
	}
	return NULL;
}


static void
race(void)
{
	pthread_t thread;
	int step;

	sRaceFD = open(path("grow-race.bin"), O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (sRaceFD < 0) {
		failed("creating grow-race.bin");
		return;
	}
	pthread_create(&thread, NULL, race_writer, NULL);
	for (step = 1; step <= RACE_STEPS; step++) {
		if (ftruncate(sRaceFD, step * RACE_STEP_SIZE) != 0) {
			failed("ftruncate of grow-race.bin");
			break;
		}
		// let the writer catch every step
		while (sRaceWrites < step && !sRaceDone) {
			struct timespec pause = {0, 200000};
			static int waited;
			nanosleep(&pause, NULL);
			if (++waited > 100000)
				break;
		}
	}
	sRaceDone = 1;
	pthread_join(thread, NULL);
	fsync(sRaceFD);
	close(sRaceFD);

	{
		// the positions for --verify
		int fd = open(path("grow-race.positions"), O_CREAT | O_WRONLY | O_TRUNC,
			0644);
		write(fd, sRaceWritten, sizeof(off_t) * sRaceWrites);
		fsync(fd);
		close(fd);
	}
}


static void
verify_all(void)
{
	off_t positions[RACE_STEPS + 1];
	int count = 0;
	int fd = open(path("grow-race.positions"), O_RDONLY);
	if (fd >= 0) {
		ssize_t bytes = read(fd, positions, sizeof(off_t) * RACE_STEPS);
		if (bytes > 0)
			count = bytes / sizeof(off_t);
		close(fd);
	}

	verify_file("grow-truncate.bin", sSize, NULL, 0, 0);
	verify_file("grow-write.bin", sSize, NULL, 0, 1);
	if (count == 0)
		failed("no block was written into grow-race.bin");
	else
		verify_file("grow-race.bin", RACE_STEPS * RACE_STEP_SIZE, positions,
			count, 0);
}


int
main(int argc, char** argv)
{
	static uint8_t stale[1 << 20];
	int verifyOnly = 0;
	off_t written;
	int fd;

	setvbuf(stdout, NULL, _IONBF, 0);
	if (argc > 1 && strcmp(argv[1], "--verify") == 0) {
		verifyOnly = 1;
		argv++;
		argc--;
	}
	if (argc < 2) {
		fprintf(stderr, "usage: bfs_grow_zero_test [--verify] DIR [SIZE_MB]\n");
		return 2;
	}
	strlcpy(sDirectory, argv[1], sizeof(sDirectory));
	sSize = (off_t)(argc > 2 ? atoll(argv[2]) : 1024) << 20;

	signal(SIGALRM, timed_out);
	alarm(1800);

	if (verifyOnly) {
		verify_all();
		printf(sFailures == 0 ? "PASS\n" : "FAILED\n");
		return sFailures == 0 ? 0 : 1;
	}

	unlink(path("grow-truncate.bin"));
	unlink(path("grow-write.bin"));
	unlink(path("grow-race.bin"));

	// Blocks that hold something else than zeros, for the files to grow into:
	// a gap that is not zeroed must show.
	fd = open(path("grow-stale.bin"), O_CREAT | O_RDWR | O_TRUNC, 0644);
	if (fd < 0) {
		perror(path("grow-stale.bin"));
		return 2;
	}
	memset(stale, 0xa5, sizeof(stale));
	for (written = 0; written < 2 * sSize; written += sizeof(stale)) {
		if (write(fd, stale, sizeof(stale)) != (ssize_t)sizeof(stale))
			break;
	}
	fsync(fd);
	close(fd);
	unlink(path("grow-stale.bin"));
	sync();
	printf("wrote and removed %lld MiB of 0xa5\n", (long long)(written >> 20));

	fd = open(path("grow-truncate.bin"), O_CREAT | O_RDWR | O_TRUNC, 0644);
	measure_stall("ftruncate()", fd, grow_with_ftruncate);
	fsync(fd);
	close(fd);

	fd = open(path("grow-write.bin"), O_CREAT | O_RDWR | O_TRUNC, 0644);
	measure_stall("a write at the end", fd, grow_with_write);
	fsync(fd);
	close(fd);

	race();
	verify_all();

	printf(sFailures == 0 ? "PASS\n" : "FAILED\n");
	return sFailures == 0 ? 0 : 1;
}
