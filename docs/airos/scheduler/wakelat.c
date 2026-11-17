/*
 * wakelat: wake-up latency of threads while lowest-priority threads keep
 * every CPU busy (airos-works/haiku #135).
 *
 *   wakelat pingpong <spinners> <seconds> [prio]
 *	two threads at prio (default 10) bounce a semaphore; round trip times
 *   wakelat timer <spinners> <seconds> <interval us> [prio]
 *	a thread at prio sleeps interval; lateness of each wake-up
 *   wakelat fair <threads> <seconds> [prio]
 *	threads at prio (default 10) spin; work done in total and per thread
 *   wakelat share <threads> <seconds> <busy us> <sleep us> <prio> <spinners>
 *	threads at prio run busy and sleep in turn next to spinners at
 *	priority 10: the work each group got done
 *   wakelat mode lowlat|powersave
 *
 * Spinners run at priority 1 (B_LOWEST_ACTIVE_PRIORITY). Results are lines
 * "KP <test> <args> ..." for the QEMU harness.
 */

#include <OS.h>
#include <scheduler.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define MAX_SAMPLES	(4 * 1024 * 1024)
#define MAX_THREADS	256


static volatile int sStop;
static volatile int sSpinStop;
static thread_id sSpinners[MAX_THREADS];
static int sSpinnerCount;
static double* sSamples;
static int sSampleCount;


static int
compare_double(const void* a, const void* b)
{
	double x = *(const double*)a, y = *(const double*)b;
	return x < y ? -1 : x > y ? 1 : 0;
}


static double
percentile(double p)
{
	if (sSampleCount == 0)
		return 0;
	int index = (int)(p * (sSampleCount - 1) + 0.5);
	return sSamples[index];
}


static status_t
spin(void* data)
{
	volatile unsigned long* counter = (volatile unsigned long*)data;
	unsigned long x = 0;
	while (!sSpinStop) {
		for (int i = 0; i < 4096; i++)
			x++;
		if (counter != NULL)
			*counter = x;
	}
	return 0;
}


static void
start_spinners(int count)
{
	sSpinStop = 0;
	sSpinnerCount = count;
	for (int i = 0; i < count; i++) {
		sSpinners[i] = spawn_thread(spin, "spinner", B_LOWEST_ACTIVE_PRIORITY,
			NULL);
		resume_thread(sSpinners[i]);
	}
	snooze(300000);
}


static void
stop_spinners(void)
{
	sSpinStop = 1;
	for (int i = 0; i < sSpinnerCount; i++) {
		status_t ret;
		wait_for_thread(sSpinners[i], &ret);
	}
}


static void
report(const char* test, const char* args, double seconds)
{
	qsort(sSamples, sSampleCount, sizeof(double), compare_double);
	int over1ms = 0;
	double sum = 0;
	for (int i = 0; i < sSampleCount; i++) {
		sum += sSamples[i];
		if (sSamples[i] > 1000)
			over1ms++;
	}
	printf("KP %s %s n=%d rate=%.0f/s mean=%.1f p50=%.1f p90=%.1f p99=%.1f "
		"p999=%.1f max=%.1f over1ms=%d us\n", test, args, sSampleCount,
		sSampleCount / seconds, sSampleCount ? sum / sSampleCount : 0,
		percentile(0.5), percentile(0.9), percentile(0.99), percentile(0.999),
		sSampleCount ? sSamples[sSampleCount - 1] : 0, over1ms);
	fflush(stdout);
}


//	#pragma mark - ping-pong


static sem_id sPing, sPong;


static status_t
pong(void* unused)
{
	while (acquire_sem(sPing) == B_OK)
		release_sem(sPong);
	return 0;
}


static status_t
ping(void* unused)
{
	while (!sStop && sSampleCount < MAX_SAMPLES) {
		bigtime_t start = system_time();
		release_sem(sPing);
		acquire_sem(sPong);
		sSamples[sSampleCount++] = (double)(system_time() - start);
	}
	return 0;
}


static void
pingpong(int spinners, int seconds, int priority)
{
	sPing = create_sem(0, "ping");
	sPong = create_sem(0, "pong");
	start_spinners(spinners);
	sStop = 0;
	sSampleCount = 0;
	thread_id b = spawn_thread(pong, "pong", priority, NULL);
	thread_id a = spawn_thread(ping, "ping", priority, NULL);
	resume_thread(b);
	resume_thread(a);
	snooze((bigtime_t)seconds * 1000000);
	sStop = 1;
	status_t ret;
	wait_for_thread(a, &ret);
	delete_sem(sPing);
	wait_for_thread(b, &ret);
	delete_sem(sPong);
	stop_spinners();
	char args[64];
	snprintf(args, sizeof(args), "spin=%d prio=%d", spinners, priority);
	report("pingpong", args, seconds);
}


//	#pragma mark - timer


static bigtime_t sInterval;


static status_t
sleeper(void* unused)
{
	while (!sStop && sSampleCount < MAX_SAMPLES) {
		bigtime_t target = system_time() + sInterval;
		snooze_until(target, B_SYSTEM_TIMEBASE);
		sSamples[sSampleCount++] = (double)(system_time() - target);
	}
	return 0;
}


static void
timer(int spinners, int seconds, bigtime_t interval, int priority)
{
	sInterval = interval;
	start_spinners(spinners);
	sStop = 0;
	sSampleCount = 0;
	thread_id t = spawn_thread(sleeper, "sleeper", priority, NULL);
	resume_thread(t);
	snooze((bigtime_t)seconds * 1000000);
	sStop = 1;
	status_t ret;
	wait_for_thread(t, &ret);
	stop_spinners();
	char args[64];
	snprintf(args, sizeof(args), "spin=%d interval=%d prio=%d", spinners,
		(int)interval, priority);
	report("timer", args, seconds);
}


//	#pragma mark - fairness


static volatile unsigned long sCounters[MAX_THREADS * 8];


static void
fair(int threads, int seconds, int priority)
{
	sSpinStop = 0;
	thread_id ids[MAX_THREADS];
	for (int i = 0; i < threads; i++) {
		sCounters[i * 8] = 0;
		ids[i] = spawn_thread(spin, "worker", priority,
			(void*)&sCounters[i * 8]);
	}
	for (int i = 0; i < threads; i++)
		resume_thread(ids[i]);
	snooze((bigtime_t)seconds * 1000000);
	sSpinStop = 1;
	double total = 0, min = 1e30, max = 0;
	for (int i = 0; i < threads; i++) {
		status_t ret;
		wait_for_thread(ids[i], &ret);
		double v = sCounters[i * 8] / 4096.0 / 1000.0;
		total += v;
		if (v < min)
			min = v;
		if (v > max)
			max = v;
	}
	printf("KP fair threads=%d prio=%d total=%.0f min=%.0f max=%.0f "
		"min/mean=%.2f kloops\n", threads, priority, total, min, max,
		min * threads / total);
	fflush(stdout);
}


//	#pragma mark - share


static bigtime_t sBusy, sSleep;
static volatile unsigned long sDutyWork[MAX_THREADS * 8];


static status_t
duty(void* data)
{
	volatile unsigned long* counter = (volatile unsigned long*)data;
	unsigned long x = 0;
	while (!sSpinStop) {
		bigtime_t end = system_time() + sBusy;
		while (system_time() < end) {
			for (int i = 0; i < 256; i++)
				x++;
		}
		*counter = x;
		snooze(sSleep);
	}
	return 0;
}


/*!	\a dutyThreads threads at \a priority run \a busy us and sleep \a sleep
	us, \a spinThreads at priority 10 run all the time: the work each group
	gets done.
*/
static void
share(int dutyThreads, int spinThreads, int seconds, bigtime_t busy,
	bigtime_t sleep, int priority)
{
	sBusy = busy;
	sSleep = sleep;
	sSpinStop = 0;
	thread_id ids[MAX_THREADS];
	int count = 0;
	for (int i = 0; i < dutyThreads; i++, count++) {
		sDutyWork[count * 8] = 0;
		ids[count] = spawn_thread(duty, "duty", priority,
			(void*)&sDutyWork[count * 8]);
	}
	for (int i = 0; i < spinThreads; i++, count++) {
		sDutyWork[count * 8] = 0;
		ids[count] = spawn_thread(spin, "worker", B_NORMAL_PRIORITY,
			(void*)&sDutyWork[count * 8]);
	}
	for (int i = 0; i < count; i++)
		resume_thread(ids[i]);
	snooze((bigtime_t)seconds * 1000000);
	sSpinStop = 1;
	double dutyWork = 0, spinWork = 0;
	for (int i = 0; i < count; i++) {
		status_t ret;
		wait_for_thread(ids[i], &ret);
		double v = sDutyWork[i * 8] / 1000000.0;
		if (i < dutyThreads)
			dutyWork += v;
		else
			spinWork += v;
	}
	printf("KP share duty=%d busy=%d sleep=%d prio=%d spin=%d duty_work=%.0f "
		"spin_work=%.0f Mloops\n", dutyThreads, (int)busy, (int)sleep,
		priority, spinThreads, dutyWork, spinWork);
	fflush(stdout);
}


int
main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: wakelat pingpong|timer|fair|share <threads> "
			"<seconds> ... | mode lowlat|powersave\n");
		return 1;
	}
	if (strcmp(argv[1], "mode") == 0) {
		int mode = strcmp(argv[2], "powersave") == 0
			? SCHEDULER_MODE_POWER_SAVING : SCHEDULER_MODE_LOW_LATENCY;
		status_t status = set_scheduler_mode(mode);
		printf("KP mode %s %s\n", argv[2], strerror(status));
		return 0;
	}
	sSamples = (double*)malloc(sizeof(double) * MAX_SAMPLES);
	int count = atoi(argv[2]);
	int seconds = atoi(argv[3]);
	if (count > MAX_THREADS)
		count = MAX_THREADS;
	if (strcmp(argv[1], "pingpong") == 0)
		pingpong(count, seconds, argc > 4 ? atoi(argv[4]) : 10);
	else if (strcmp(argv[1], "timer") == 0) {
		timer(count, seconds, argc > 4 ? atoi(argv[4]) : 200,
			argc > 5 ? atoi(argv[5]) : 10);
	} else if (strcmp(argv[1], "fair") == 0)
		fair(count, seconds, argc > 4 ? atoi(argv[4]) : 10);
	else if (strcmp(argv[1], "share") == 0) {
		// share <duty threads> <seconds> <busy> <sleep> <prio> <spinners>
		share(count, atoi(argv[7]), seconds, atoi(argv[4]), atoi(argv[5]),
			atoi(argv[6]));
	} else
		return 1;
	return 0;
}
