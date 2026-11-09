/*
 * schedperf: how the scheduler and the CPU clocks serve CPU-bound work.
 *
 * Work is a chain of dependent integer additions: one per clock cycle on
 * every core this runs on (Zen, Cortex-A76, Cortex-A55), so additions per
 * microsecond read as the clock in MHz the thread really got - core speed,
 * frequency ramp and time lost to other threads all show in it.
 *
 *   schedperf cpus                     each CPU's speed, pinned
 *   schedperf ramp [cpu] [runs]        speed per 1 ms after 1.5 s idle
 *   schedperf rampdown cpu             clock after a busy second (x86)
 *   schedperf place N [s]              N busy threads: where they ran, speed
 *   schedperf burst P_ms W_us F [bg]   periodic job of W us at top speed
 *   schedperf wake [n] [bg]            wake-up latency of a blocked thread
 *   schedperf mode lowlat|powersave    set the scheduler mode
 *
 * Lines starting with "R " are results for scripts: R <test> key=value...
 */

#include <OS.h>
#include <scheduler.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int _kern_get_cpu(void);
extern status_t _kern_set_cpu_enabled(int32 cpu, bool enabled);
extern status_t _kern_generic_syscall(const char* subsystem, uint32 function,
	void* buffer, size_t bufferSize);
extern status_t _kern_set_thread_affinity(thread_id id, const void* mask,
	size_t size);

#define MAX_CPUS 256

static double energy_joules(void);
#define ADDS_PER_ITER 16


static inline uint64_t
spin(uint64_t iterations)
{
	uint64_t x = 0;
	for (uint64_t i = 0; i < iterations; i++) {
#if defined(__x86_64__) || defined(__i386__)
		__asm__ volatile(".rept 16\n\taddq $1, %0\n\t.endr" : "+r"(x));
#elif defined(__aarch64__)
		__asm__ volatile(".rept 16\n\tadd %0, %0, #1\n\t.endr" : "+r"(x));
#else
#	error "no spin loop for this architecture"
#endif
	}
	return x;
}


// Typical code instead of a pure clock count: independent arithmetic, loads
// from a 256 KiB table and a data-dependent branch. Big and little cores
// differ here by much more than their clocks. SCHEDPERF_WORK=mixed picks it.
static uint32_t sTable[65536];
static int sMixed;


static inline uint64_t
spin_mixed(uint64_t iterations)
{
	uint64_t a = 1, b = 2, c = 3, d = 4;
	for (uint64_t i = 0; i < iterations; i++) {
		a = a * 6364136223846793005ULL + 1442695040888963407ULL;
		b += a >> 17;
		c ^= b << 3;
		d += sTable[(a >> 40) & 65535];
		if ((a >> 59) == 0)
			c += d;
		else
			d ^= c;
	}
	__asm__ volatile("" : : "r"(a), "r"(b), "r"(c), "r"(d));
	return d;
}


static inline uint64_t
work(uint64_t iterations)
{
	if (sMixed)
		return spin_mixed(iterations);
	return spin(iterations);
}


static int32
cpu_count(void)
{
	system_info info;
	get_system_info(&info);
	return info.cpu_count;
}


static status_t
pin(int cpu)
{
	uint32_t mask[MAX_CPUS / 32];
	memset(mask, 0, sizeof(mask));
	if (cpu < 0)
		memset(mask, 0xff, sizeof(mask));
	else
		mask[cpu / 32] = 1u << (cpu % 32);
	status_t status = _kern_set_thread_affinity(find_thread(NULL), mask,
		sizeof(mask));
	// the thread moves at its next reschedule
	snooze(2000);
	if (cpu >= 0 && _kern_get_cpu() != cpu) {
		for (int i = 0; i < 100 && _kern_get_cpu() != cpu; i++)
			snooze(1000);
	}
	return status;
}


// additions per microsecond during about `us' microseconds
static double
measure_mhz(bigtime_t us)
{
	bigtime_t start = system_time();
	bigtime_t end = start + us;
	uint64_t iterations = 0;
	bigtime_t now;
	do {
		work(64);
		iterations += 64;
		now = system_time();
	} while (now < end);
	return (double)(iterations * ADDS_PER_ITER) / (now - start);
}


static int
compare_double(const void* a, const void* b)
{
	double x = *(const double*)a, y = *(const double*)b;
	return x < y ? -1 : x > y;
}


static double
percentile(double* sorted, int count, double p)
{
	if (count == 0)
		return 0;
	int index = (int)(p * (count - 1) + 0.5);
	return sorted[index];
}


//	#pragma mark - cpus


static double sCPUMHz[MAX_CPUS];
static double sMaxMHz;


static void
measure_cpus(bool print)
{
	int32 count = cpu_count();
	sMaxMHz = 0;
	for (int32 cpu = 0; cpu < count; cpu++) {
		pin(cpu);
		measure_mhz(100000);
		sCPUMHz[cpu] = measure_mhz(200000);
		if (sCPUMHz[cpu] > sMaxMHz)
			sMaxMHz = sCPUMHz[cpu];
		if (print)
			printf("cpu %2d: %6.0f MHz\n", (int)cpu, sCPUMHz[cpu]);
	}
	pin(-1);
	if (print) {
		printf("R cpus");
		for (int32 cpu = 0; cpu < count; cpu++)
			printf(" c%d=%.0f", (int)cpu, sCPUMHz[cpu]);
		printf("\n");
	}
}


//	#pragma mark - ramp


#define RAMP_BUCKETS 600


static void
ramp(int cpu, int runs)
{
	static double mhz[RAMP_BUCKETS];
	static int where[RAMP_BUCKETS];
	if (cpu >= 0)
		pin(cpu);

	for (int run = 0; run < runs; run++) {
		snooze(1500000);
		bigtime_t start = system_time();
		for (int b = 0; b < RAMP_BUCKETS; b++) {
			bigtime_t bucketStart = system_time();
			bigtime_t end = start + (bigtime_t)(b + 1) * 1000;
			uint64_t iterations = 0;
			bigtime_t now;
			do {
				work(32);
				iterations += 32;
				now = system_time();
			} while (now < end);
			mhz[b] = (double)(iterations * ADDS_PER_ITER)
				/ (now - bucketStart);
			where[b] = _kern_get_cpu();
		}

		// the speed it settled at: median of the last 100 ms
		double tail[100];
		memcpy(tail, mhz + RAMP_BUCKETS - 100, sizeof(tail));
		qsort(tail, 100, sizeof(double), compare_double);
		double settled = tail[50];
		int t50 = -1, t90 = -1;
		for (int b = 0; b < RAMP_BUCKETS; b++) {
			if (t50 < 0 && mhz[b] >= 0.5 * settled)
				t50 = b + 1;
			// 90 % and staying there for 5 ms
			if (t90 < 0 && mhz[b] >= 0.9 * settled) {
				bool stays = true;
				for (int k = b; k < b + 5 && k < RAMP_BUCKETS; k++)
					stays = stays && mhz[k] >= 0.9 * settled;
				if (stays)
					t90 = b + 1;
			}
		}
		int migrations = 0;
		for (int b = 1; b < RAMP_BUCKETS; b++)
			migrations += where[b] != where[b - 1];
		// work lost against running at the settled speed all along
		double done = 0;
		for (int b = 0; b < RAMP_BUCKETS; b++)
			done += mhz[b];
		double lost = 100.0 * (1 - done / (settled * RAMP_BUCKETS));

		printf("run %d: 1ms %.0f  2ms %.0f  5ms %.0f  10ms %.0f  20ms %.0f  "
			"50ms %.0f  100ms %.0f  200ms %.0f  600ms %.0f MHz; cpu %d->%d\n",
			run, mhz[0], mhz[1], mhz[4], mhz[9], mhz[19], mhz[49], mhz[99],
			mhz[199], mhz[599], where[0], where[RAMP_BUCKETS - 1]);
		printf("R ramp cpu=%d run=%d first=%.0f settled=%.0f t50=%d t90=%d "
			"lost600=%.1f migrations=%d startcpu=%d endcpu=%d\n", cpu, run,
			mhz[0], settled, t50, t90, lost, migrations, where[0],
			where[RAMP_BUCKETS - 1]);
		fflush(stdout);
	}
	if (cpu >= 0)
		pin(-1);
}


//	#pragma mark - rampdown


static void
rampdown(int cpu)
{
	int32 count = cpu_count();
	cpu_info* info = (cpu_info*)malloc(sizeof(cpu_info) * count);
	int observer = cpu == 0 ? 1 : 0;

	pin(cpu);
	measure_mhz(1000000);
	pin(observer);
	bigtime_t stop = system_time();
	printf("clock of cpu %d after a busy second (current_frequency):\n", cpu);
	printf("R rampdown cpu=%d", cpu);
	for (int i = 0; i <= 60; i++) {
		snooze_until(stop + (bigtime_t)i * 50000, B_SYSTEM_TIMEBASE);
		get_cpu_info(0, count, info);
		printf(" t%d=%.0f", i * 50, info[cpu].current_frequency / 1e6);
	}
	printf("\n");
	pin(-1);
	free(info);
}


//	#pragma mark - place


static int sCoreOf[MAX_CPUS];


static void
load_core_map(void)
{
	uint32 count = 0;
	get_cpu_topology_info(NULL, &count);
	cpu_topology_node_info* nodes = (cpu_topology_node_info*)calloc(count,
		sizeof(cpu_topology_node_info));
	get_cpu_topology_info(nodes, &count);
	int core = -1;
	for (int i = 0; i < MAX_CPUS; i++)
		sCoreOf[i] = i;
	for (uint32 i = 0; i < count; i++) {
		if (nodes[i].type == B_TOPOLOGY_CORE)
			core++;
		else if (nodes[i].type == B_TOPOLOGY_SMT && nodes[i].id < MAX_CPUS)
			sCoreOf[nodes[i].id] = core;
	}
	free(nodes);
}


typedef struct {
	volatile bool* stop;
	uint64_t adds;
	bigtime_t runTime;
	int cpuSamples[MAX_CPUS];
	int lastCPU;
	int migrations;
	int coreMigrations;
} place_thread;


static status_t
place_worker(void* data)
{
	place_thread* t = (place_thread*)data;
	bigtime_t start = system_time();
	bigtime_t nextSample = start;
	t->lastCPU = _kern_get_cpu();
	while (!*t->stop) {
		work(256);
		t->adds += 256 * ADDS_PER_ITER;
		bigtime_t now = system_time();
		if (now >= nextSample) {
			int cpu = _kern_get_cpu();
			t->cpuSamples[cpu]++;
			if (cpu != t->lastCPU) {
				t->migrations++;
				if (sCoreOf[cpu] != sCoreOf[t->lastCPU])
					t->coreMigrations++;
			}
			t->lastCPU = cpu;
			nextSample = now + 1000;
		}
	}
	t->runTime = system_time() - start;
	return 0;
}


static void
place(int threads, double seconds, int quiet)
{
	int32 count = cpu_count();
	place_thread* t = (place_thread*)calloc(threads, sizeof(place_thread));
	thread_id* ids = (thread_id*)malloc(sizeof(thread_id) * threads);
	cpu_info* before = (cpu_info*)malloc(sizeof(cpu_info) * count);
	cpu_info* after = (cpu_info*)malloc(sizeof(cpu_info) * count);
	volatile bool stop = false;

	get_cpu_info(0, count, before);
	bigtime_t start = system_time();
	for (int i = 0; i < threads; i++) {
		t[i].stop = &stop;
		ids[i] = spawn_thread(place_worker, "place worker", B_NORMAL_PRIORITY,
			&t[i]);
		resume_thread(ids[i]);
	}
	snooze((bigtime_t)(seconds * 1e6));
	stop = true;
	for (int i = 0; i < threads; i++) {
		status_t ret;
		wait_for_thread(ids[i], &ret);
	}
	bigtime_t elapsed = system_time() - start;
	get_cpu_info(0, count, after);

	double total = 0, minimum = 1e18, maximum = 0;
	int migrations = 0, coreMigrations = 0;
	for (int i = 0; i < threads; i++) {
		double mhz = (double)t[i].adds / t[i].runTime;
		total += mhz;
		if (mhz < minimum)
			minimum = mhz;
		if (mhz > maximum)
			maximum = mhz;
		migrations += t[i].migrations;
		coreMigrations += t[i].coreMigrations;
		if (!quiet) {
			int best = 0, samples = 0;
			for (int c = 0; c < count; c++) {
				samples += t[i].cpuSamples[c];
				if (t[i].cpuSamples[c] > t[i].cpuSamples[best])
					best = c;
			}
			printf("thread %2d: %6.0f MHz, mostly cpu %d (%d%%), %d moves, on",
				i, mhz, best,
				samples ? 100 * t[i].cpuSamples[best] / samples : 0,
				t[i].migrations);
			for (int c = 0; c < count; c++) {
				if (t[i].cpuSamples[c] * 50 > samples)
					printf(" %d:%d%%", c, 100 * t[i].cpuSamples[c] / samples);
			}
			printf("\n");
		}
	}
	printf("R place threads=%d total=%.0f min=%.0f max=%.0f migrations=%d "
		"coremoves=%d busy=", threads, total, minimum, maximum, migrations,
		coreMigrations);
	for (int c = 0; c < count; c++) {
		printf("%s%.0f", c ? "," : "",
			100.0 * (after[c].active_time - before[c].active_time) / elapsed);
	}
	printf("\n");
	fflush(stdout);
	free(t);
	free(ids);
	free(before);
	free(after);
}


//	#pragma mark - background load


static volatile bool sBackgroundStop;


static status_t
background_worker(void* unused)
{
	while (!sBackgroundStop)
		work(1024);
	return 0;
}


static thread_id sBackground[MAX_CPUS];
static int sBackgroundCount;


static void
start_background(int threads)
{
	sBackgroundStop = false;
	sBackgroundCount = threads;
	for (int i = 0; i < threads; i++) {
		sBackground[i] = spawn_thread(background_worker, "background",
			B_NORMAL_PRIORITY, NULL);
		resume_thread(sBackground[i]);
	}
	snooze(200000);
}


static void
stop_background(void)
{
	sBackgroundStop = true;
	for (int i = 0; i < sBackgroundCount; i++) {
		status_t ret;
		wait_for_thread(sBackground[i], &ret);
	}
	sBackgroundCount = 0;
}


//	#pragma mark - burst


static void
burst(double periodMs, double workUs, int frames, int background,
	int32 priority)
{
	if (sMaxMHz == 0)
		measure_cpus(false);
	uint64_t iterations = (uint64_t)(workUs * sMaxMHz / ADDS_PER_ITER);
	double* durations = (double*)malloc(sizeof(double) * frames);
	int* where = (int*)malloc(sizeof(int) * frames);
	int32 count = cpu_count();

	if (background > 0)
		start_background(background);
	set_thread_priority(find_thread(NULL), priority);
	snooze(1000000);

	bigtime_t period = (bigtime_t)(periodMs * 1000);
	energy_joules();
	bigtime_t burstStart = system_time();
	bigtime_t next = system_time() + period;
	int late = 0;
	int slowPieces = 0, pieces = 0, jobMoves = 0;
	for (int f = 0; f < frames; f++) {
		snooze_until(next, B_SYSTEM_TIMEBASE);
		bigtime_t start = system_time();
		// in 32 pieces, noting where each ran
		int last = _kern_get_cpu();
		int firstCPU = last;
		for (int p = 0; p < 32; p++) {
			work(iterations / 32);
			int cpu = _kern_get_cpu();
			if (sCPUMHz[cpu] < 0.7 * sMaxMHz)
				slowPieces++;
			if (cpu != last)
				jobMoves++;
			last = cpu;
			pieces++;
		}
		bigtime_t end = system_time();
		durations[f] = (end - start) / 1000.0;
		where[f] = _kern_get_cpu();
		if (getenv("SCHEDPERF_TRACE") != NULL) {
			printf("frame %d: %.2f ms, cpu %d -> %d\n", f, durations[f],
				firstCPU, where[f]);
		}
		if (end > next + period)
			late++;
		next += period;
		if (next < end)
			next = end + period;
	}
	double joules = energy_joules();
	double burstSeconds = (system_time() - burstStart) / 1e6;
	set_thread_priority(find_thread(NULL), B_NORMAL_PRIORITY);
	if (background > 0)
		stop_background();

	int perCPU[MAX_CPUS];
	memset(perCPU, 0, sizeof(perCPU));
	for (int f = 0; f < frames; f++)
		perCPU[where[f]]++;
	// the first frames: how it starts from idle
	double first = durations[0], first5 = 0;
	for (int f = 0; f < 5 && f < frames; f++)
		first5 += durations[f];
	qsort(durations, frames, sizeof(double), compare_double);
	double ideal = workUs / 1000.0;
	printf("burst %.1f ms every %.1f ms (%d busy threads around it): "
		"p50 %.2f p95 %.2f max %.2f ms (ideal %.2f), first %.2f, %d late\n",
		ideal, periodMs, background, percentile(durations, frames, 0.5),
		percentile(durations, frames, 0.95), durations[frames - 1], ideal,
		first, late);
	printf("R burst period=%.1f work=%.2f bg=%d prio=%d p50=%.3f p95=%.3f "
		"max=%.3f first=%.3f first5=%.3f late=%d slow=%.0f%% moves=%d "
		"watts=%.2f cpus=",
		periodMs, ideal,
		background, (int)priority, percentile(durations, frames, 0.5),
		percentile(durations, frames, 0.95), durations[frames - 1], first,
		first5 / 5, late, 100.0 * slowPieces / pieces, jobMoves,
		joules >= 0 ? joules / burstSeconds : -1.0);
	bool comma = false;
	for (int c = 0; c < count; c++) {
		if (perCPU[c] == 0)
			continue;
		printf("%s%d:%d", comma ? "," : "", c, perCPU[c]);
		comma = true;
	}
	printf("\n");
	fflush(stdout);
	free(durations);
	free(where);
}


//	#pragma mark - wake


static sem_id sWakeSem;
static volatile bigtime_t sWakeTime;
static double* sWakeLatency;
static int sWakeCount;


static status_t
wake_waiter(void* unused)
{
	for (int i = 0; i < sWakeCount; i++) {
		acquire_sem(sWakeSem);
		sWakeLatency[i] = (double)(system_time() - sWakeTime);
	}
	return 0;
}


static void
wake(int count, int background)
{
	sWakeCount = count;
	sWakeLatency = (double*)malloc(sizeof(double) * count);
	sWakeSem = create_sem(0, "wake");
	if (background > 0)
		start_background(background);

	thread_id waiter = spawn_thread(wake_waiter, "waiter", B_NORMAL_PRIORITY,
		NULL);
	resume_thread(waiter);
	for (int i = 0; i < count; i++) {
		// long enough for the waiter to be asleep on an idle CPU
		snooze(2000 + (i % 7) * 300);
		sWakeTime = system_time();
		release_sem(sWakeSem);
	}
	status_t ret;
	wait_for_thread(waiter, &ret);
	if (background > 0)
		stop_background();

	qsort(sWakeLatency, count, sizeof(double), compare_double);
	printf("wake-up latency (%d busy threads): p50 %.0f p99 %.0f max %.0f us\n",
		background, percentile(sWakeLatency, count, 0.5),
		percentile(sWakeLatency, count, 0.99), sWakeLatency[count - 1]);
	printf("R wake bg=%d p50=%.1f p90=%.1f p99=%.1f max=%.1f\n", background,
		percentile(sWakeLatency, count, 0.5),
		percentile(sWakeLatency, count, 0.9),
		percentile(sWakeLatency, count, 0.99), sWakeLatency[count - 1]);
	delete_sem(sWakeSem);
	free(sWakeLatency);
}


//	#pragma mark - energy


/*!	AMD package energy (amd_pstates' "amd_energy" syscall): joules counted
	since the last call, summed over the distinct counters (a Threadripper
	has one per die; each processor reads its own). Negative when there is
	no such counter.
*/
static double
energy_joules(void)
{
	static uint64_t last[MAX_CPUS];
	static bool haveLast;
	uint32_t buffer[2 + 2 * MAX_CPUS];
	if (_kern_generic_syscall("amd_energy", 1, buffer, sizeof(buffer)) != B_OK)
		return -1;
	uint32_t unit = buffer[0], count = buffer[1];
	uint64_t* counters = (uint64_t*)(buffer + 2);
	double joules = 0;
	if (haveLast) {
		// one delta per distinct counter: processors that read the same
		// counter saw nearly the same value (it ticks every millisecond or
		// so: within a joule); separate counters differ by much more
		uint64_t seen[MAX_CPUS];
		int seenCount = 0;
		for (uint32_t c = 0; c < count; c++) {
			uint64_t delta = (counters[c] - last[c]) & 0xffffffff;
			bool known = false;
			for (int k = 0; k < seenCount; k++) {
				if ((counters[c] > seen[k] ? counters[c] - seen[k]
						: seen[k] - counters[c]) < (1u << unit)) {
					known = true;
				}
			}
			if (!known) {
				seen[seenCount++] = counters[c];
				joules += (double)delta / (1u << unit);
			}
		}
	}
	memcpy(last, counters, sizeof(uint64_t) * count);
	haveLast = true;
	return joules;
}


static void
energy_dump(void)
{
	uint32_t buffer[2 + 2 * MAX_CPUS];
	status_t status = _kern_generic_syscall("amd_energy", 1, buffer,
		sizeof(buffer));
	if (status != B_OK) {
		printf("no energy counter: %s\n", strerror(status));
		return;
	}
	uint64_t* counters = (uint64_t*)(buffer + 2);
	printf("unit 1/2^%u J:", buffer[0]);
	for (uint32_t c = 0; c < buffer[1]; c++)
		printf(" %llu", (unsigned long long)counters[c]);
	printf("\n");
}


//	#pragma mark - topology


static void
topology(void)
{
	uint32 count = 0;
	get_cpu_topology_info(NULL, &count);
	cpu_topology_node_info* nodes = (cpu_topology_node_info*)calloc(count,
		sizeof(cpu_topology_node_info));
	get_cpu_topology_info(nodes, &count);
	printf("R topo");
	for (uint32 i = 0; i < count; i++) {
		switch (nodes[i].type) {
			case B_TOPOLOGY_PACKAGE:
				printf(" P%u", (unsigned)nodes[i].id);
				break;
			case B_TOPOLOGY_CORE:
				printf(" C%u", (unsigned)nodes[i].id);
				break;
			case B_TOPOLOGY_SMT:
				printf(":%u", (unsigned)nodes[i].id);
				break;
			default:
				break;
		}
	}
	printf("\n");
	free(nodes);
}


//	#pragma mark -


static void
usage(void)
{
	fprintf(stderr,
		"usage: schedperf cpus\n"
		"       schedperf ramp [cpu|-1] [runs]\n"
		"       schedperf rampdown cpu\n"
		"       schedperf place threads [seconds]\n"
		"       schedperf burst period_ms work_us frames [bg] [priority]\n"
		"       schedperf wake [count] [bg]\n"
		"       schedperf mode [lowlat|powersave]\n");
	exit(1);
}


int
main(int argc, char** argv)
{
	if (argc < 2)
		usage();
	setvbuf(stdout, NULL, _IOLBF, 0);
	load_core_map();
	const char* kind = getenv("SCHEDPERF_WORK");
	sMixed = kind != NULL && strcmp(kind, "mixed") == 0;
	for (int i = 0; i < 65536; i++)
		sTable[i] = i * 2654435761u;
	const char* test = argv[1];

	if (strcmp(test, "cpus") == 0) {
		measure_cpus(true);
	} else if (strcmp(test, "ramp") == 0) {
		ramp(argc > 2 ? atoi(argv[2]) : -1, argc > 3 ? atoi(argv[3]) : 3);
	} else if (strcmp(test, "rampdown") == 0) {
		rampdown(argc > 2 ? atoi(argv[2]) : 2);
	} else if (strcmp(test, "place") == 0) {
		if (argc < 3)
			usage();
		place(atoi(argv[2]), argc > 3 ? atof(argv[3]) : 2.0, 0);
	} else if (strcmp(test, "burst") == 0) {
		if (argc < 5)
			usage();
		burst(atof(argv[2]), atof(argv[3]), atoi(argv[4]),
			argc > 5 ? atoi(argv[5]) : 0,
			argc > 6 ? atoi(argv[6]) : B_NORMAL_PRIORITY);
	} else if (strcmp(test, "wake") == 0) {
		wake(argc > 2 ? atoi(argv[2]) : 2000, argc > 3 ? atoi(argv[3]) : 0);
	} else if (strcmp(test, "cpuflip") == 0) {
		// cpuflip <first> <last> <seconds>: disable and enable those CPUs
		// in turn (what s2idle does), to test the scheduler around it
		int first = atoi(argv[2]), last = atoi(argv[3]);
		bigtime_t end = system_time() + (bigtime_t)(atof(argv[4]) * 1e6);
		int flips = 0;
		while (system_time() < end) {
			for (int c = first; c <= last; c++)
				_kern_set_cpu_enabled(c, false);
			snooze(3000);
			for (int c = first; c <= last; c++)
				_kern_set_cpu_enabled(c, true);
			snooze(3000);
			flips++;
		}
		printf("R cpuflip %d\n", flips);
	} else if (strcmp(test, "idlestats") == 0) {
		// arm64: how often each core powered down while idle
		uint32_t buffer[2 + 6 * MAX_CPUS];
		status_t status = _kern_generic_syscall("arm64_idle", 1, buffer,
			sizeof(buffer));
		if (status != B_OK) {
			printf("R idlestats none %s\n", strerror(status));
			return 0;
		}
		uint64_t* values = (uint64_t*)(buffer + 2);
		printf("R idlestats enabled=%u", buffer[1]);
		for (uint32_t c = 0; c < buffer[0]; c++) {
			printf(" c%u=%llu/%llu/%llums", c,
				(unsigned long long)values[c * 3],
				(unsigned long long)values[c * 3 + 1],
				(unsigned long long)values[c * 3 + 2] / 1000);
		}
		printf("\n");
	} else if (strcmp(test, "powerdown") == 0) {
		// powerdown on|off: arm64 idle power-down, to compare
		uint32_t enable = argc > 2 && strcmp(argv[2], "off") != 0;
		status_t status = _kern_generic_syscall("arm64_idle", 2, &enable,
			sizeof(enable));
		printf("R powerdown %s %s\n", enable ? "on" : "off", strerror(status));
	} else if (strcmp(test, "pstates") == 0) {
		// pstates on|off: Zen 1 P-state scaling (amd_pstates), to compare
		uint32_t enable = argc > 2 && strcmp(argv[2], "off") != 0;
		status_t status = _kern_generic_syscall("amd_energy", 2, &enable,
			sizeof(enable));
		printf("R pstates %s %s\n", enable ? "on" : "off", strerror(status));
	} else if (strcmp(test, "energy") == 0) {
		energy_dump();
	} else if (strcmp(test, "power") == 0) {
		// power <seconds> [busy threads]: average package power
		double seconds = argc > 2 ? atof(argv[2]) : 10;
		int threads = argc > 3 ? atoi(argv[3]) : 0;
		energy_joules();
		bigtime_t start = system_time();
		if (threads > 0)
			place(threads, seconds, 1);
		else
			snooze((bigtime_t)(seconds * 1e6));
		double joules = energy_joules();
		double elapsed = (system_time() - start) / 1e6;
		printf("R power threads=%d seconds=%.1f joules=%.1f watts=%.2f\n",
			threads, elapsed, joules, joules / elapsed);
	} else if (strcmp(test, "topo") == 0) {
		topology();
	} else if (strcmp(test, "mode") == 0) {
		if (argc > 2) {
			int32 mode = strcmp(argv[2], "powersave") == 0
				? SCHEDULER_MODE_POWER_SAVING : SCHEDULER_MODE_LOW_LATENCY;
			status_t status = set_scheduler_mode(mode);
			if (status != B_OK)
				fprintf(stderr, "set_scheduler_mode: %s\n", strerror(status));
		}
		printf("R mode %s\n", get_scheduler_mode() == SCHEDULER_MODE_POWER_SAVING
			? "powersave" : "lowlat");
	} else
		usage();
	return 0;
}
