/*
 * kperf: kernel scalability microbenchmarks for airos-works/haiku #96-#100
 * (mutex convoys, page allocation, file and anonymous faults, page daemon).
 * Every result is one line "KP <test> <args> <value> <unit>".
 *
 *   kperf faultthr <threads> [MiB]    threads of one team touch own areas
 *   kperf faultteam <teams> [MiB]     separate teams touch own areas
 *   kperf stat <threads> [path]       parallel stat() of one path, 1 s
 *   kperf areas <threads>             parallel create/delete_area, 1 s
 *   kperf malloc <threads>            parallel malloc/free mix
 *   kperf anon <MiB> [threads]        touch fresh anonymous memory
 *   kperf filemap <path> [rounds]     map a cached file and read each page
 *   kperf exec <count>                fork + exec /bin/true
 *   kperf daemon <MiB> <seconds>      keep touching memory; page daemon time
 *   kperf mkfile <path> <MiB>         write a file of that size
 */
#include <OS.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>


static void
report(const char* test, const char* args, double value, const char* unit)
{
	printf("KP %s %s %.3f %s\n", test, args, value, unit);
	fflush(stdout);
}


static void
die(const char* what)
{
	printf("KP_FAIL %s: %s\n", what, strerror(errno));
	fflush(stdout);
	exit(1);
}


static int
compare_bigtime(const void* a, const void* b)
{
	bigtime_t x = *(const bigtime_t*)a, y = *(const bigtime_t*)b;
	return x < y ? -1 : x > y;
}


static bigtime_t
median(bigtime_t* values, int count)
{
	qsort(values, count, sizeof(bigtime_t), compare_bigtime);
	return values[count / 2];
}


// #pragma mark - start barrier


static pthread_barrier_t sBarrier;
static volatile int sStop;


static void
touch_area(size_t size)
{
	void* address;
	area_id area = create_area("kperf touch", &address, B_ANY_ADDRESS, size,
		B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	if (area < 0) {
		errno = area;
		die("create_area");
	}
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE)
		((volatile char*)address)[offset] = 1;
	delete_area(area);
}


// #pragma mark - faultthr


static size_t sTouchSize;


static void*
fault_thread(void* arg)
{
	pthread_barrier_wait(&sBarrier);
	touch_area(sTouchSize);
	pthread_barrier_wait(&sBarrier);
	return NULL;
}


static void
test_faultthr(int threads, int mib)
{
	sTouchSize = (size_t)mib << 20;
	bigtime_t times[7];
	for (int round = 0; round < 7; round++) {
		pthread_barrier_init(&sBarrier, NULL, threads + 1);
		pthread_t thread[threads];
		for (int i = 0; i < threads; i++)
			pthread_create(&thread[i], NULL, fault_thread, NULL);
		pthread_barrier_wait(&sBarrier);
		bigtime_t start = system_time();
		pthread_barrier_wait(&sBarrier);
		times[round] = system_time() - start;
		for (int i = 0; i < threads; i++)
			pthread_join(thread[i], NULL);
		pthread_barrier_destroy(&sBarrier);
	}
	char args[64];
	snprintf(args, sizeof(args), "t=%d mib=%d", threads, mib);
	report("faultthr", args, median(times, 7) / 1000.0, "ms");
}


// #pragma mark - faultteam


static void
test_faultteam(int teams, int mib)
{
	bigtime_t times[7];
	for (int round = 0; round < 7; round++) {
		int go[2], done[2];
		if (pipe(go) != 0 || pipe(done) != 0)
			die("pipe");
		pid_t child[teams];
		for (int i = 0; i < teams; i++) {
			child[i] = fork();
			if (child[i] == 0) {
				char c;
				close(go[1]);
				read(go[0], &c, 1);
				touch_area((size_t)mib << 20);
				write(done[1], "d", 1);
				_exit(0);
			}
		}
		close(go[0]);
		snooze(20000);
		bigtime_t start = system_time();
		close(go[1]);
		for (int i = 0; i < teams; i++) {
			char c;
			read(done[0], &c, 1);
		}
		times[round] = system_time() - start;
		for (int i = 0; i < teams; i++)
			waitpid(child[i], NULL, 0);
		close(done[0]);
		close(done[1]);
	}
	char args[64];
	snprintf(args, sizeof(args), "n=%d mib=%d", teams, mib);
	report("faultteam", args, median(times, 7) / 1000.0, "ms");
}


// #pragma mark - stat, areas, malloc: operations per second


static const char* sStatPath;
static int64 sOps[64];


static void*
stat_thread(void* arg)
{
	int index = (int)(addr_t)arg;
	struct stat st;
	int64 ops = 0;
	pthread_barrier_wait(&sBarrier);
	while (!sStop) {
		for (int i = 0; i < 64; i++)
			stat(sStatPath, &st);
		ops += 64;
	}
	sOps[index] = ops;
	return NULL;
}


static void*
areas_thread(void* arg)
{
	int index = (int)(addr_t)arg;
	int64 ops = 0;
	pthread_barrier_wait(&sBarrier);
	while (!sStop) {
		void* address;
		area_id area = create_area("kperf a", &address, B_ANY_ADDRESS,
			16 * B_PAGE_SIZE, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
		((volatile char*)address)[0] = 1;
		delete_area(area);
		ops++;
	}
	sOps[index] = ops;
	return NULL;
}


static void*
malloc_thread(void* arg)
{
	int index = (int)(addr_t)arg;
	uint32 seed = index * 7919 + 1;
	void* slots[256] = {};
	int64 ops = 0;
	pthread_barrier_wait(&sBarrier);
	while (!sStop) {
		for (int i = 0; i < 256; i++) {
			seed = seed * 1103515245 + 12345;
			int slot = (seed >> 8) & 255;
			free(slots[slot]);
			size_t size = (seed >> 16) % 100 == 0
				? 64 * 1024 + (seed % 512) * 1024 : 16 + (seed >> 20) % 2048;
			slots[slot] = malloc(size);
			memset(slots[slot], 1, size < 256 ? size : 256);
		}
		ops += 256;
	}
	for (int i = 0; i < 256; i++)
		free(slots[i]);
	sOps[index] = ops;
	return NULL;
}


static void
run_rate(const char* test, int threads, void* (*function)(void*))
{
	double best = 0;
	for (int round = 0; round < 3; round++) {
		sStop = 0;
		pthread_barrier_init(&sBarrier, NULL, threads + 1);
		pthread_t thread[threads];
		for (int i = 0; i < threads; i++)
			pthread_create(&thread[i], NULL, function, (void*)(addr_t)i);
		pthread_barrier_wait(&sBarrier);
		bigtime_t start = system_time();
		snooze(1000000);
		sStop = 1;
		int64 total = 0;
		for (int i = 0; i < threads; i++) {
			pthread_join(thread[i], NULL);
			total += sOps[i];
		}
		double rate = total / ((system_time() - start) / 1000000.0);
		if (rate > best)
			best = rate;
		pthread_barrier_destroy(&sBarrier);
	}
	char args[64];
	snprintf(args, sizeof(args), "t=%d", threads);
	report(test, args, best / 1000.0, "kops/s");
}


// #pragma mark - anon


static void*
anon_thread(void* arg)
{
	size_t size = (size_t)arg;
	pthread_barrier_wait(&sBarrier);
	char* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		die("mmap");
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE)
		((volatile char*)p)[offset] = 1;
	pthread_barrier_wait(&sBarrier);
	// second pass: TLB reach (memory already populated)
	pthread_barrier_wait(&sBarrier);
	for (int pass = 0; pass < 4; pass++) {
		for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE)
			((volatile char*)p)[offset]++;
	}
	pthread_barrier_wait(&sBarrier);
	munmap(p, size);
	pthread_barrier_wait(&sBarrier);
	return NULL;
}


static void
test_anon(int mib, int threads)
{
	bigtime_t touch[5], walk[5], unmap[5];
	for (int round = 0; round < 5; round++) {
		pthread_barrier_init(&sBarrier, NULL, threads + 1);
		pthread_t thread[threads];
		for (int i = 0; i < threads; i++) {
			pthread_create(&thread[i], NULL, anon_thread,
				(void*)(((size_t)mib << 20) / threads));
		}
		pthread_barrier_wait(&sBarrier);
		bigtime_t start = system_time();
		pthread_barrier_wait(&sBarrier);
		touch[round] = system_time() - start;
		pthread_barrier_wait(&sBarrier);
		start = system_time();
		pthread_barrier_wait(&sBarrier);
		walk[round] = system_time() - start;
		start = system_time();
		pthread_barrier_wait(&sBarrier);
		unmap[round] = system_time() - start;
		for (int i = 0; i < threads; i++)
			pthread_join(thread[i], NULL);
		pthread_barrier_destroy(&sBarrier);
	}
	char args[64];
	snprintf(args, sizeof(args), "mib=%d t=%d", mib, threads);
	report("anon_touch", args, median(touch, 5) / 1000.0, "ms");
	report("anon_walk4", args, median(walk, 5) / 1000.0, "ms");
	report("anon_unmap", args, median(unmap, 5) / 1000.0, "ms");
}


// #pragma mark - filemap


static void
test_filemap(const char* path, int rounds)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		die(path);
	struct stat st;
	fstat(fd, &st);
	size_t size = st.st_size;
	// warm the file cache
	char* buffer = malloc(1 << 20);
	while (read(fd, buffer, 1 << 20) > 0)
		;
	free(buffer);
	bigtime_t times[rounds];
	volatile unsigned sum = 0;
	for (int round = 0; round < rounds; round++) {
		bigtime_t start = system_time();
		char* p = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
		if (p == MAP_FAILED)
			die("mmap file");
		for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE)
			sum += p[offset];
		munmap(p, size);
		times[round] = system_time() - start;
	}
	close(fd);
	char args[256];
	snprintf(args, sizeof(args), "%s mib=%zu", path, size >> 20);
	report("filemap_first", args, times[0] / 1000.0, "ms");
	report("filemap", args, median(times, rounds) / 1000.0, "ms");
}


// #pragma mark - exec


static void
test_exec(int count)
{
	bigtime_t start = system_time();
	for (int i = 0; i < count; i++) {
		pid_t child = fork();
		if (child == 0) {
			execl("/bin/true", "true", (char*)NULL);
			_exit(127);
		}
		waitpid(child, NULL, 0);
	}
	char args[64];
	snprintf(args, sizeof(args), "n=%d", count);
	report("exec", args, (system_time() - start) / (double)count, "us");
}


// #pragma mark - daemon


static thread_id
find_kernel_thread(const char* name)
{
	int32 cookie = 0;
	thread_info info;
	while (get_next_thread_info(1, &cookie, &info) == B_OK) {
		if (strcmp(info.name, name) == 0)
			return info.thread;
	}
	return -1;
}


static bigtime_t
thread_time(thread_id thread)
{
	thread_info info;
	if (get_thread_info(thread, &info) != B_OK)
		return 0;
	return info.kernel_time + info.user_time;
}


static void
test_daemon(int mib, int seconds)
{
	thread_id daemon = find_kernel_thread("page daemon");
	size_t size = (size_t)mib << 20;
	char* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		die("mmap");
	for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE)
		p[offset] = 1;
	bigtime_t daemonStart = thread_time(daemon);
	bigtime_t start = system_time(), end = start + seconds * 1000000LL;
	int64 touches = 0;
	while (system_time() < end) {
		for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE)
			((volatile char*)p)[offset]++;
		touches += size / B_PAGE_SIZE;
	}
	bigtime_t elapsed = system_time() - start;
	char args[64];
	snprintf(args, sizeof(args), "mib=%d s=%d", mib, seconds);
	report("daemon_cpu", args, (thread_time(daemon) - daemonStart) / 1000.0,
		"ms");
	report("daemon_touch_rate", args, touches / (elapsed / 1000000.0) / 1e6,
		"Mpages/s");
	munmap(p, size);
}


static void
test_mkfile(const char* path, int mib)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		die(path);
	char* buffer = malloc(1 << 20);
	for (int i = 0; i < mib; i++) {
		for (int j = 0; j < (1 << 20); j += 4096)
			buffer[j] = (char)(i + j);
		write(fd, buffer, 1 << 20);
	}
	fsync(fd);
	close(fd);
	free(buffer);
}


// #pragma mark - correctness


static void
test_verify(int mib, int rounds)
{
	// anonymous memory larger than what is free: the page daemon ages,
	// deactivates and swaps; every page must keep its contents
	size_t size = (size_t)mib << 20;
	size_t pages = size / B_PAGE_SIZE;
	uint64* p = mmap(NULL, size, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED)
		die("mmap");
	for (size_t i = 0; i < pages; i++)
		p[i * (B_PAGE_SIZE / 8)] = i * 2654435761ULL;
	int64 errors = 0;
	bigtime_t start = system_time();
	for (int round = 0; round < rounds; round++) {
		for (size_t i = 0; i < pages; i++) {
			uint64* word = &p[i * (B_PAGE_SIZE / 8)];
			if (*word != i * 2654435761ULL + round)
				errors++;
			*word = i * 2654435761ULL + round + 1;
		}
	}
	char args[64];
	snprintf(args, sizeof(args), "mib=%d rounds=%d", mib, rounds);
	report("verify_errors", args, errors, "pages");
	report("verify_time", args, (system_time() - start) / 1000.0, "ms");
	munmap(p, size);
}


static void
test_mapwrite(const char* path, int mib)
{
	// writes through a shared file mapping must reach the file
	size_t size = (size_t)mib << 20;
	int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, size) != 0)
		die(path);
	int64 errors = 0;
	for (int round = 0; round < 3; round++) {
		uint32* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED)
			die("mmap shared");
		for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE)
			p[offset / 4] = offset * 7 + round;
		if (round == 1)
			snooze(25000000);
				// the page daemon and page writer get to see it
		munmap(p, size);
		if (round != 0)
			fsync(fd);
		for (size_t offset = 0; offset < size; offset += B_PAGE_SIZE) {
			uint32 value;
			pread(fd, &value, 4, offset);
			if (value != (uint32)(offset * 7 + round))
				errors++;
		}
	}
	close(fd);
	unlink(path);
	char args[64];
	snprintf(args, sizeof(args), "mib=%d", mib);
	report("mapwrite_errors", args, errors, "pages");
}


static size_t sPressurePages;
static uint64* sPressureAnon;
static int sPressureRounds;
static int64 sPressureAnonErrors, sPressureFileErrors;
static const char* sPressurePath;


static void*
pressure_anon(void* arg)
{
	for (int round = 0; round < sPressureRounds; round++) {
		for (size_t i = 0; i < sPressurePages; i++) {
			uint64* word = &sPressureAnon[i * (B_PAGE_SIZE / 8)];
			if (*word != i * 2654435761ULL + round)
				sPressureAnonErrors++;
			*word = i * 2654435761ULL + round + 1;
		}
	}
	return NULL;
}


static void*
pressure_file(void* arg)
{
	int fd = open(sPressurePath, O_RDONLY);
	struct stat st;
	fstat(fd, &st);
	size_t pages = st.st_size / B_PAGE_SIZE;
	for (int round = 0; round < sPressureRounds * 2; round++) {
		uint64* p = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
		if (p == MAP_FAILED)
			die("mmap pressure file");
		for (size_t i = 0; i < pages; i++) {
			if (p[i * (B_PAGE_SIZE / 8)] != i * 11400714819323198485ULL)
				sPressureFileErrors++;
		}
		munmap(p, st.st_size);
	}
	close(fd);
	return NULL;
}


static void
test_pressure(int anonMiB, int fileMiB, const char* path, int rounds)
{
	// the file: the first word of each page is known
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		die(path);
	uint64* page = calloc(1, 1 << 20);
	size_t index = 0;
	for (int i = 0; i < fileMiB; i++) {
		for (int j = 0; j < (1 << 20) / B_PAGE_SIZE; j++, index++)
			page[j * (B_PAGE_SIZE / 8)] = index * 11400714819323198485ULL;
		write(fd, page, 1 << 20);
	}
	fsync(fd);
	close(fd);
	free(page);

	sPressurePath = path;
	sPressureRounds = rounds;
	sPressurePages = ((size_t)anonMiB << 20) / B_PAGE_SIZE;
	sPressureAnon = mmap(NULL, (size_t)anonMiB << 20, PROT_READ | PROT_WRITE,
		MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (sPressureAnon == MAP_FAILED)
		die("mmap anon");
	for (size_t i = 0; i < sPressurePages; i++)
		sPressureAnon[i * (B_PAGE_SIZE / 8)] = i * 2654435761ULL;

	thread_id daemon = find_kernel_thread("page daemon");
	bigtime_t daemonStart = thread_time(daemon);
	bigtime_t start = system_time();
	pthread_t a, b;
	pthread_create(&a, NULL, pressure_anon, NULL);
	pthread_create(&b, NULL, pressure_file, NULL);
	pthread_join(a, NULL);
	pthread_join(b, NULL);
	char args[64];
	snprintf(args, sizeof(args), "anon=%d file=%d rounds=%d", anonMiB, fileMiB,
		rounds);
	report("pressure_anon_errors", args, sPressureAnonErrors, "pages");
	report("pressure_file_errors", args, sPressureFileErrors, "pages");
	report("pressure_time", args, (system_time() - start) / 1000.0, "ms");
	report("pressure_daemon_cpu", args,
		(thread_time(daemon) - daemonStart) / 1000.0, "ms");
	munmap(sPressureAnon, (size_t)anonMiB << 20);
	unlink(path);
}


// #pragma mark - large pages (#99 stage 2)


#define LARGE_PAGES_AREA (1 << 15)
	// B_LARGE_PAGES_AREA, private


static uint64
random_walk(volatile uint64* p, size_t size, int64 steps)
{
	uint64 x = 88172645463325252ULL, sum = 0;
	size_t words = size / 8;
	for (int64 i = 0; i < steps; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		sum += p[(x % (words / 512)) * 512];
	}
	return sum;
}


static void
test_large(int mib)
{
	size_t size = (size_t)mib << 20;
	for (int large = 0; large < 2; large++) {
		const char* kind = large ? "large" : "small";
		void* address;
		bigtime_t start = system_time();
		area_id area = create_area("kperf large", &address, B_ANY_ADDRESS, size,
			B_NO_LOCK, B_READ_AREA | B_WRITE_AREA
				| (large ? LARGE_PAGES_AREA : 0));
		if (area < 0) {
			printf("KP large_unsupported %s\n", kind);
			continue;
		}
		uint64* p = (uint64*)address;
		for (size_t i = 0; i < size / 8; i += 512)
			p[i] = i * 7 + 1;
		bigtime_t touched = system_time();
		random_walk(p, size, 20000000);
		bigtime_t walked = system_time();
		char args[64];
		snprintf(args, sizeof(args), "%s mib=%d aligned=%d", kind, mib,
			((addr_t)address & 0x1fffff) == 0);
		report("large_touch", args, (touched - start) / 1000.0, "ms");
		report("large_walk20M", args, (walked - touched) / 1000.0, "ms");

		// copy-on-write after fork: the child writes, the parent keeps
		int64 errors = 0;
		pid_t child = fork();
		if (child == 0) {
			for (size_t i = 0; i < size / 8; i += 512 * 3)
				p[i] = 5;
			int64 childErrors = 0;
			for (size_t i = 0; i < size / 8; i += 512) {
				uint64 expected = (i % (512 * 3)) == 0 ? 5 : i * 7 + 1;
				if (p[i] != expected)
					childErrors++;
			}
			_exit(childErrors != 0 ? 1 : 0);
		}
		int status = 0;
		waitpid(child, &status, 0);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			errors += 1000000;
		for (size_t i = 0; i < size / 8; i += 512) {
			if (p[i] != i * 7 + 1)
				errors++;
		}
		// write again after the fork (pages now copied or shared)
		for (size_t i = 0; i < size / 8; i += 512)
			p[i] = i * 3;
		// shrink to a size that cuts through a large page, check, grow
		size_t smaller = size / 2 + 3 * B_PAGE_SIZE;
		if (resize_area(area, smaller) != B_OK)
			errors += 2000000;
		for (size_t i = 0; i < smaller / 8; i += 512) {
			if (p[i] != i * 3)
				errors++;
		}
		// protect a part read-only and back
		if (mprotect((char*)p + B_PAGE_SIZE * 7, B_PAGE_SIZE * 600, PROT_READ)
				!= 0
			|| mprotect((char*)p + B_PAGE_SIZE * 7, B_PAGE_SIZE * 600,
				PROT_READ | PROT_WRITE) != 0) {
			errors += 4000000;
		}
		for (size_t i = 0; i < smaller / 8; i += 512) {
			if (p[i] != i * 3)
				errors++;
		}
		bigtime_t deleteStart = system_time();
		delete_area(area);
		report("large_errors", args, errors, "");
		report("large_delete", args, (system_time() - deleteStart) / 1000.0,
			"ms");

		// a fresh one, touched and deleted without anything else
		area = create_area("kperf large2", &address, B_ANY_ADDRESS, size,
			B_NO_LOCK, B_READ_AREA | B_WRITE_AREA
				| (large ? LARGE_PAGES_AREA : 0));
		p = (uint64*)address;
		for (size_t i = 0; i < size / 8; i += 512)
			p[i] = 1;
		start = system_time();
		delete_area(area);
		report("large_delete_fresh", args, (system_time() - start) / 1000.0,
			"ms");
	}
}


static void
test_large_steps(int mib, int mode)
{
	// mode bits: 1 fork (child exits), 2 child writes, 4 parent writes after,
	// 8 resize, 16 mprotect
	size_t size = (size_t)mib << 20;
	void* address;
	area_id area = create_area("kperf ls", &address, B_ANY_ADDRESS, size,
		B_NO_LOCK, B_READ_AREA | B_WRITE_AREA | LARGE_PAGES_AREA);
	if (area < 0) {
		printf("KP large_unsupported\n");
		return;
	}
	uint64* p = (uint64*)address;
	int64 errors = 0;
	for (size_t i = 0; i < size / 8; i += 512)
		p[i] = i * 7 + 1;
	if (mode & 1) {
		pid_t child = fork();
		if (child == 0) {
			if (mode & 2) {
				for (size_t i = 0; i < size / 8; i += 512 * 3)
					p[i] = 5;
			}
			_exit(0);
		}
		waitpid(child, NULL, 0);
	}
	if (mode & 4) {
		for (size_t i = 0; i < size / 8; i += 512)
			p[i] = i * 7 + 1;
	}
	size_t now = size;
	if (mode & 8) {
		now = size / 2 + 3 * B_PAGE_SIZE;
		if (resize_area(area, now) != B_OK)
			errors += 1000000;
	}
	if (mode & 16) {
		mprotect((char*)p + B_PAGE_SIZE * 7, B_PAGE_SIZE * 600, PROT_READ);
		mprotect((char*)p + B_PAGE_SIZE * 7, B_PAGE_SIZE * 600,
			PROT_READ | PROT_WRITE);
	}
	for (size_t i = 0; i < now / 8; i += 512) {
		if (p[i] != i * 7 + 1)
			errors++;
	}
	delete_area(area);
	char args[64];
	snprintf(args, sizeof(args), "mode=%d", mode);
	report("large_steps_errors", args, errors, "");
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: see source\n");
		return 1;
	}
	const char* test = argv[1];
	int a = argc > 2 ? atoi(argv[2]) : 1;
	int b = argc > 3 ? atoi(argv[3]) : 0;
	if (strcmp(test, "faultthr") == 0)
		test_faultthr(a, b > 0 ? b : 16);
	else if (strcmp(test, "faultteam") == 0)
		test_faultteam(a, b > 0 ? b : 16);
	else if (strcmp(test, "stat") == 0) {
		sStatPath = argc > 3 ? argv[3] : "/boot/system/lib/libroot.so";
		run_rate("stat", a, stat_thread);
	} else if (strcmp(test, "areas") == 0)
		run_rate("areas", a, areas_thread);
	else if (strcmp(test, "malloc") == 0)
		run_rate("malloc", a, malloc_thread);
	else if (strcmp(test, "anon") == 0)
		test_anon(a, b > 0 ? b : 1);
	else if (strcmp(test, "filemap") == 0)
		test_filemap(argv[2], argc > 3 ? atoi(argv[3]) : 5);
	else if (strcmp(test, "exec") == 0)
		test_exec(a);
	else if (strcmp(test, "daemon") == 0)
		test_daemon(a, b > 0 ? b : 30);
	else if (strcmp(test, "verify") == 0)
		test_verify(a, b > 0 ? b : 4);
	else if (strcmp(test, "mapwrite") == 0)
		test_mapwrite(argv[2], atoi(argv[3]));
	else if (strcmp(test, "pressure") == 0)
		test_pressure(a, b, argv[4], atoi(argv[5]));
	else if (strcmp(test, "lsteps") == 0)
		test_large_steps(a, b);
	else if (strcmp(test, "large") == 0)
		test_large(a);
	else if (strcmp(test, "mkfile") == 0)
		test_mkfile(argv[2], atoi(argv[3]));
	else {
		fprintf(stderr, "unknown test %s\n", test);
		return 1;
	}
	return 0;
}
