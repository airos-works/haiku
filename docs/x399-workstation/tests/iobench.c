/* iobench: disk and file I/O measurements for airos-works/haiku #102 and
 * #103, and a check that concurrent I/O returns the right data.
 *
 *   iobench rand PATH THREADS SECONDS [BLOCK]
 *       random reads of BLOCK bytes (default 4096), aligned, from THREADS
 *       threads: I/O per second, MB/s, and CPU time and context switches per
 *       I/O (whole team; the waits are what the driver costs)
 *   iobench seq PATH BLOCK SECONDS [write]
 *       sequential reads (or writes) of BLOCK bytes from offset 0, wrapping
 *       at the end of PATH (a device or a file)
 *   iobench verify PATH THREADS BYTES
 *       writes a pattern that encodes each block's offset over BYTES of PATH
 *       (DESTROYS ITS CONTENT), then reads random blocks of random sizes from
 *       THREADS threads and checks every byte; also checks the whole range
 *       sequentially afterwards
 *   iobench file PATH BLOCK
 *       reads the whole file sequentially once in BLOCK pieces: MB/s (for
 *       read-ahead, with a cold cache)
 *   iobench fwrite PATH BYTES BLOCK [over]
 *       writes the verify pattern to the file in BLOCK pieces (creating it
 *       anew, or with "over" writing over what is there): MB/s for the
 *       writes, and the time fsync() takes after them
 *   iobench fcheck PATH BLOCK [THREADS]
 *       reads the file written by fwrite in BLOCK pieces and checks every
 *       byte; with THREADS, each reads its own slice of the file
 *       sequentially, all at the same time
 *   iobench size PATH
 *       prints the size of a device or file in bytes
 *
 * Results go to standard output, one line per measurement, starting "IOB".
 */
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <OS.h>
#include <Drivers.h>


static const char* sPath;
static int sThreads;
static double sSeconds;
static size_t sBlock;
static off_t sSize;
static volatile int sStop;
static int64 sOperations[256];
static int64 sErrors;


static off_t
path_size(int fd)
{
	struct stat st;
	if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode))
		return st.st_size;
	off_t size = 0;
	if (ioctl(fd, B_GET_DEVICE_SIZE, &size, sizeof(size)) == 0 && size > 0)
		return size;
	device_geometry geometry;
	if (ioctl(fd, B_GET_GEOMETRY, &geometry, sizeof(geometry)) == 0) {
		return (off_t)geometry.bytes_per_sector * geometry.sectors_per_track
			* geometry.cylinder_count * geometry.head_count;
	}
	return lseek(fd, 0, SEEK_END);
}


static uint64_t
rand64(uint64_t* state)
{
	// xorshift64*
	uint64_t x = *state;
	x ^= x >> 12;
	x ^= x << 25;
	x ^= x >> 27;
	*state = x;
	return x * 2685821657736338717ULL;
}


static bigtime_t
team_cpu(void)
{
	team_usage_info usage;
	get_team_usage_info(B_CURRENT_TEAM, B_TEAM_USAGE_SELF, &usage);
	return usage.user_time + usage.kernel_time;
}


static void*
rand_thread(void* data)
{
	int index = (int)(intptr_t)data;
	int fd = open(sPath, O_RDONLY);
	if (fd < 0) {
		atomic_add64(&sErrors, 1);
		return NULL;
	}
	char* buffer = malloc(sBlock + 4096);
	char* aligned = (char*)(((uintptr_t)buffer + 4095) & ~(uintptr_t)4095);
	uint64_t state = 0x9e3779b97f4a7c15ULL * (index + 1) + system_time();
	const off_t blocks = sSize / (off_t)sBlock;
	while (!sStop) {
		off_t offset = (off_t)(rand64(&state) % blocks) * sBlock;
		if (pread(fd, aligned, sBlock, offset) != (ssize_t)sBlock) {
			atomic_add64(&sErrors, 1);
			break;
		}
		sOperations[index]++;
	}
	free(buffer);
	close(fd);
	return NULL;
}


static int
run_rand(void)
{
	int fd = open(sPath, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "iobench: %s: %s\n", sPath, strerror(errno));
		return 1;
	}
	sSize = path_size(fd);
	close(fd);
	if (sSize < (off_t)sBlock * 16) {
		fprintf(stderr, "iobench: %s is too small\n", sPath);
		return 1;
	}

	pthread_t threads[256];
	bigtime_t cpu0 = team_cpu();
	bigtime_t start = system_time();
	for (int i = 0; i < sThreads; i++)
		pthread_create(&threads[i], NULL, rand_thread, (void*)(intptr_t)i);
	snooze((bigtime_t)(sSeconds * 1000000));
	sStop = 1;
	for (int i = 0; i < sThreads; i++)
		pthread_join(threads[i], NULL);
	bigtime_t elapsed = system_time() - start;
	bigtime_t cpu = team_cpu() - cpu0;

	int64 total = 0;
	for (int i = 0; i < sThreads; i++)
		total += sOperations[i];
	double iops = total / (elapsed / 1e6);
	printf("IOB rand %s threads %d block %zu: %.0f IO/s, %.1f MB/s, %.1f us CPU/IO,"
		" errors %" B_PRId64 "\n", sPath, sThreads, sBlock, iops,
		iops * sBlock / 1e6, total ? (double)cpu / total : 0.0, sErrors);
	return sErrors ? 1 : 0;
}


static int
run_seq(int write)
{
	int fd = open(sPath, write ? O_RDWR : O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "iobench: %s: %s\n", sPath, strerror(errno));
		return 1;
	}
	sSize = path_size(fd);
	char* buffer = malloc(sBlock + 4096);
	char* aligned = (char*)(((uintptr_t)buffer + 4095) & ~(uintptr_t)4095);
	memset(aligned, 0x5a, sBlock);
	off_t offset = 0;
	int64 bytes = 0;
	bigtime_t cpu0 = team_cpu();
	bigtime_t start = system_time();
	bigtime_t end = start + (bigtime_t)(sSeconds * 1000000);
	while (system_time() < end) {
		if (offset + (off_t)sBlock > sSize)
			offset = 0;
		ssize_t done = write ? pwrite(fd, aligned, sBlock, offset)
			: pread(fd, aligned, sBlock, offset);
		if (done != (ssize_t)sBlock) {
			sErrors++;
			break;
		}
		offset += sBlock;
		bytes += sBlock;
	}
	if (write)
		fsync(fd);
	bigtime_t elapsed = system_time() - start;
	bigtime_t cpu = team_cpu() - cpu0;
	printf("IOB seq %s %s block %zu: %.1f MB/s, %.1f us CPU per MB, errors %" B_PRId64
		"\n", write ? "write" : "read", sPath, sBlock, bytes / (elapsed / 1e6) / 1e6,
		bytes ? cpu / (bytes / 1e6) : 0.0, sErrors);
	free(buffer);
	close(fd);
	return sErrors ? 1 : 0;
}


static int
run_file(void)
{
	int fd = open(sPath, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "iobench: %s: %s\n", sPath, strerror(errno));
		return 1;
	}
	char* buffer = malloc(sBlock);
	int64 bytes = 0;
	bigtime_t cpu0 = team_cpu();
	bigtime_t start = system_time();
	ssize_t done;
	while ((done = read(fd, buffer, sBlock)) > 0)
		bytes += done;
	bigtime_t elapsed = system_time() - start;
	bigtime_t cpu = team_cpu() - cpu0;
	if (done < 0)
		sErrors++;
	printf("IOB file %s block %zu: %" B_PRId64 " bytes in %.3f s, %.1f MB/s, %.1f us"
		" CPU per MB, errors %" B_PRId64 "\n", sPath, sBlock, bytes, elapsed / 1e6,
		bytes / (elapsed / 1e6) / 1e6, bytes ? cpu / (bytes / 1e6) : 0.0, sErrors);
	free(buffer);
	close(fd);
	return sErrors ? 1 : 0;
}


// #pragma mark - verify


static void
fill_pattern(char* buffer, off_t offset, size_t length)
{
	// every 8 bytes: their own offset, mixed
	for (size_t i = 0; i < length; i += 8) {
		uint64_t value = (uint64_t)(offset + i) * 0x9e3779b97f4a7c15ULL;
		memcpy(buffer + i, &value, 8);
	}
}


static int64
check_pattern(const char* buffer, off_t offset, size_t length)
{
	for (size_t i = 0; i < length; i += 8) {
		uint64_t value = (uint64_t)(offset + i) * 0x9e3779b97f4a7c15ULL;
		if (memcmp(buffer + i, &value, 8) != 0)
			return offset + i;
	}
	return -1;
}


static void*
verify_thread(void* data)
{
	int index = (int)(intptr_t)data;
	int fd = open(sPath, O_RDONLY);
	if (fd < 0) {
		atomic_add64(&sErrors, 1);
		return NULL;
	}
	const size_t maxLength = 1024 * 1024;
	char* buffer = malloc(maxLength + 8192);
	uint64_t state = 0x2545f4914f6cdd1dULL * (index + 1) + system_time();
	while (!sStop) {
		// sizes from 512 bytes to 1 MiB, at 512 byte offsets, sometimes
		// misaligned in memory: the direct and the bounced paths both
		size_t length = (size_t)(1 + rand64(&state) % (maxLength / 512)) * 512;
		off_t offset = (off_t)(rand64(&state) % ((sSize - length) / 512)) * 512;
		char* target = buffer + (rand64(&state) % 2 ? 0 : 512);
		ssize_t done = pread(fd, target, length, offset);
		if (done != (ssize_t)length) {
			printf("IOB verify: read of %zu at %" B_PRIdOFF " returned %zd\n", length,
				offset, done);
			atomic_add64(&sErrors, 1);
			break;
		}
		int64 bad = check_pattern(target, offset, length);
		if (bad >= 0) {
			printf("IOB verify: wrong data at %" B_PRId64 " (read of %zu at %"
				B_PRIdOFF ")\n", bad, length, offset);
			atomic_add64(&sErrors, 1);
			break;
		}
		sOperations[index]++;
	}
	free(buffer);
	close(fd);
	return NULL;
}


static int
run_verify(off_t bytes)
{
	int fd = open(sPath, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "iobench: %s: %s\n", sPath, strerror(errno));
		return 1;
	}
	off_t size = path_size(fd);
	if (bytes > size)
		bytes = size;
	sSize = bytes;

	// write the pattern, 1 MiB at a time
	const size_t chunk = 1024 * 1024;
	char* buffer = malloc(chunk);
	bigtime_t start = system_time();
	for (off_t offset = 0; offset < bytes; offset += chunk) {
		size_t length = bytes - offset < (off_t)chunk ? bytes - offset : chunk;
		fill_pattern(buffer, offset, length);
		if (pwrite(fd, buffer, length, offset) != (ssize_t)length) {
			printf("IOB verify: write at %" B_PRIdOFF " failed: %s\n", offset,
				strerror(errno));
			return 1;
		}
	}
	fsync(fd);
	printf("IOB verify: wrote %" B_PRIdOFF " bytes in %.2f s\n", bytes,
		(system_time() - start) / 1e6);
	close(fd);

	pthread_t threads[256];
	start = system_time();
	for (int i = 0; i < sThreads; i++)
		pthread_create(&threads[i], NULL, verify_thread, (void*)(intptr_t)i);
	snooze((bigtime_t)(sSeconds * 1000000));
	sStop = 1;
	for (int i = 0; i < sThreads; i++)
		pthread_join(threads[i], NULL);
	int64 total = 0;
	for (int i = 0; i < sThreads; i++)
		total += sOperations[i];
	printf("IOB verify: %" B_PRId64 " random reads from %d threads, errors %" B_PRId64
		"\n", total, sThreads, sErrors);

	// and all of it in order
	fd = open(sPath, O_RDONLY);
	for (off_t offset = 0; offset < bytes && sErrors == 0; offset += chunk) {
		size_t length = bytes - offset < (off_t)chunk ? bytes - offset : chunk;
		if (pread(fd, buffer, length, offset) != (ssize_t)length
			|| check_pattern(buffer, offset, length) >= 0) {
			printf("IOB verify: sequential check failed at %" B_PRIdOFF "\n", offset);
			sErrors++;
		}
	}
	close(fd);
	printf("IOB verify %s: %s\n", sPath, sErrors ? "FAILED" : "ok");
	free(buffer);
	return sErrors ? 1 : 0;
}


// #pragma mark - file write and check


static int
run_fwrite(off_t bytes, bool over)
{
	int fd = open(sPath, over ? O_RDWR : O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fprintf(stderr, "iobench: %s: %s\n", sPath, strerror(errno));
		return 1;
	}
	char* buffer = malloc(sBlock);
	bigtime_t cpu0 = team_cpu();
	bigtime_t start = system_time();
	for (off_t offset = 0; offset < bytes; offset += sBlock) {
		size_t length = bytes - offset < (off_t)sBlock ? bytes - offset : sBlock;
		fill_pattern(buffer, offset, length);
		if (write(fd, buffer, length) != (ssize_t)length) {
			printf("IOB fwrite: write at %" B_PRIdOFF " failed: %s\n", offset,
				strerror(errno));
			sErrors++;
			break;
		}
	}
	bigtime_t written = system_time();
	fsync(fd);
	bigtime_t synced = system_time();
	bigtime_t cpu = team_cpu() - cpu0;
	printf("IOB fwrite %s block %zu%s: %.1f MB/s writing, fsync %.3f s, %.1f MB/s"
		" in all, %.1f us CPU per MB, errors %" B_PRId64 "\n", sPath, sBlock,
		over ? " over" : "", bytes / ((written - start) / 1e6) / 1e6,
		(synced - written) / 1e6, bytes / ((synced - start) / 1e6) / 1e6,
		cpu / (bytes / 1e6), sErrors);
	free(buffer);
	close(fd);
	return sErrors ? 1 : 0;
}


struct check_slice {
	off_t start;
	off_t end;
};


static void*
check_thread(void* data)
{
	struct check_slice* slice = (struct check_slice*)data;
	int fd = open(sPath, O_RDONLY);
	if (fd < 0) {
		atomic_add64(&sErrors, 1);
		return NULL;
	}
	// (8 byte pattern words: a buffer that starts at their boundary)
	char* buffer = malloc(sBlock + 16);
	for (off_t offset = slice->start; offset < slice->end;) {
		size_t length = slice->end - offset < (off_t)sBlock
			? slice->end - offset : sBlock;
		ssize_t done = pread(fd, buffer, length, offset);
		if (done != (ssize_t)length) {
			printf("IOB fcheck: read of %zu at %" B_PRIdOFF " returned %zd\n",
				length, offset, done);
			atomic_add64(&sErrors, 1);
			break;
		}
		// the pattern words that lie whole in what was read
		off_t first = (offset + 7) & ~(off_t)7;
		off_t last = (offset + length) & ~(off_t)7;
		if (last > first) {
			char* words = malloc(last - first);
			memcpy(words, buffer + (first - offset), last - first);
			int64 bad = check_pattern(words, first, last - first);
			free(words);
			if (bad >= 0) {
				printf("IOB fcheck: wrong data at %" B_PRId64 " (read of %zu at %"
					B_PRIdOFF ")\n", bad, length, offset);
				atomic_add64(&sErrors, 1);
				break;
			}
		}
		offset += length;
	}
	free(buffer);
	close(fd);
	return NULL;
}


static int
run_fcheck(int threads)
{
	int fd = open(sPath, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "iobench: %s: %s\n", sPath, strerror(errno));
		return 1;
	}
	off_t size = path_size(fd);
	close(fd);

	struct check_slice slices[256];
	pthread_t ids[256];
	off_t sliceSize = (size / threads + 4095) & ~(off_t)4095;
	bigtime_t cpu0 = team_cpu();
	bigtime_t start = system_time();
	for (int i = 0; i < threads; i++) {
		slices[i].start = i * sliceSize < size ? i * sliceSize : size;
		slices[i].end = (i + 1) * sliceSize < size ? (i + 1) * sliceSize : size;
		pthread_create(&ids[i], NULL, check_thread, &slices[i]);
	}
	for (int i = 0; i < threads; i++)
		pthread_join(ids[i], NULL);
	bigtime_t elapsed = system_time() - start;
	bigtime_t cpu = team_cpu() - cpu0;
	printf("IOB fcheck %s block %zu threads %d: %.1f MB/s, %.1f us CPU per MB,"
		" %s\n", sPath, sBlock, threads, size / (elapsed / 1e6) / 1e6,
		cpu / (size / 1e6), sErrors ? "FAILED" : "ok");
	return sErrors ? 1 : 0;
}


int
main(int argc, char** argv)
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (argc < 3)
		goto usage;
	sPath = argv[2];
	if (strcmp(argv[1], "rand") == 0 && argc >= 5) {
		sThreads = atoi(argv[3]);
		sSeconds = atof(argv[4]);
		sBlock = argc > 5 ? strtoul(argv[5], NULL, 0) : 4096;
		if (sThreads < 1 || sThreads > 256)
			goto usage;
		return run_rand();
	}
	if (strcmp(argv[1], "seq") == 0 && argc >= 5) {
		sBlock = strtoul(argv[3], NULL, 0);
		sSeconds = atof(argv[4]);
		return run_seq(argc > 5 && strcmp(argv[5], "write") == 0);
	}
	if (strcmp(argv[1], "verify") == 0 && argc >= 5) {
		sThreads = atoi(argv[3]);
		sSeconds = 10;
		if (sThreads < 1 || sThreads > 256)
			goto usage;
		return run_verify(strtoll(argv[4], NULL, 0));
	}
	if (strcmp(argv[1], "size") == 0) {
		int fd = open(sPath, O_RDONLY);
		if (fd < 0)
			return 1;
		printf("%" B_PRIdOFF "\n", path_size(fd));
		close(fd);
		return 0;
	}
	if (strcmp(argv[1], "file") == 0 && argc >= 4) {
		sBlock = strtoul(argv[3], NULL, 0);
		return run_file();
	}
	if (strcmp(argv[1], "fwrite") == 0 && argc >= 5) {
		sBlock = strtoul(argv[4], NULL, 0);
		if (sBlock == 0)
			goto usage;
		return run_fwrite(strtoll(argv[3], NULL, 0),
			argc > 5 && strcmp(argv[5], "over") == 0);
	}
	if (strcmp(argv[1], "fcheck") == 0 && argc >= 4) {
		sBlock = strtoul(argv[3], NULL, 0);
		int threads = argc > 4 ? atoi(argv[4]) : 1;
		if (sBlock == 0 || threads < 1 || threads > 256)
			goto usage;
		return run_fcheck(threads);
	}

usage:
	fprintf(stderr, "usage: iobench rand PATH THREADS SECONDS [BLOCK]\n"
		"       iobench seq PATH BLOCK SECONDS [write]\n"
		"       iobench verify PATH THREADS BYTES\n"
		"       iobench file PATH BLOCK\n"
		"       iobench fwrite PATH BYTES BLOCK [over]\n"
		"       iobench fcheck PATH BLOCK [THREADS]\n"
		"       iobench size PATH\n");
	return 2;
}
