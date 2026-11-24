/* SPDX-License-Identifier: MIT */
/*
 * Lazy binding of PLT jump slots by the runtime loader (LD_BIND_LAZY=1).
 * Link with lazy_binding_module.c built as a shared library. Checks that the
 * lazy binding entry hands every argument register (integer, vector up to
 * AVX-512, the variadic %al, the indirect result register) to the function
 * intact, that a first call from a signal handler binds, and that many
 * threads binding the same slots at the same time all get the right
 * functions. Run it eagerly too: the results must not differ.
 *
 * With "missing" it links against a copy of the module that references a
 * function nobody defines: lazily bound it starts, and "missing call" ends
 * with status 127 when the function is first called.
 */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#if defined(__x86_64__)
#	include <cpuid.h>
#	include <immintrin.h>
#endif

#ifdef LAZY_MISSING
#	pragma GCC diagnostic ignored "-Wunused-function"
#endif


int lazy_check_integers(long, long, long, long, long, long, long, long);
int lazy_check_floats(double, double, double, double, double, double, double,
	double, double);
int lazy_check_mixed(int, double, long, float, char, double, short, double);
struct lazy_big { long values[8]; };
struct lazy_big lazy_make_big(long first);
double lazy_sum_variadic(int count, ...);
long lazy_signal_function(long x);
#if defined(__x86_64__)
__attribute__((target("avx"))) int lazy_check_avx(__m256d, __m256d, __m256d,
	__m256d, __m256d, __m256d, __m256d, __m256d);
__attribute__((target("avx512f"))) int lazy_check_avx512(__m512d, __m512d,
	__m512d, __m512d, __m512d, __m512d, __m512d, __m512d);
#endif

#define DECLARE(n) long lazy_function_##n(long x);
#define D8(a, b, c, d, e, f, g, h) DECLARE(a) DECLARE(b) DECLARE(c) \
	DECLARE(d) DECLARE(e) DECLARE(f) DECLARE(g) DECLARE(h)
D8(0, 1, 2, 3, 4, 5, 6, 7) D8(8, 9, 10, 11, 12, 13, 14, 15)
D8(16, 17, 18, 19, 20, 21, 22, 23) D8(24, 25, 26, 27, 28, 29, 30, 31)
D8(32, 33, 34, 35, 36, 37, 38, 39) D8(40, 41, 42, 43, 44, 45, 46, 47)
D8(48, 49, 50, 51, 52, 53, 54, 55) D8(56, 57, 58, 59, 60, 61, 62, 63)

enum { kFunctionCount = 64, kThreadCount = 16 };

static long
call_function(int n)
{
	switch (n) {
#define CASE(n) case n: return lazy_function_##n(5);
#define C8(a, b, c, d, e, f, g, h) CASE(a) CASE(b) CASE(c) CASE(d) CASE(e) \
	CASE(f) CASE(g) CASE(h)
		C8(0, 1, 2, 3, 4, 5, 6, 7) C8(8, 9, 10, 11, 12, 13, 14, 15)
		C8(16, 17, 18, 19, 20, 21, 22, 23) C8(24, 25, 26, 27, 28, 29, 30, 31)
		C8(32, 33, 34, 35, 36, 37, 38, 39) C8(40, 41, 42, 43, 44, 45, 46, 47)
		C8(48, 49, 50, 51, 52, 53, 54, 55) C8(56, 57, 58, 59, 60, 61, 62, 63)
	}
	return -1;
}


static int sFailures = 0;


static void
check(int ok, const char* what)
{
	printf("%s: %s\n", ok ? "ok" : "FAILED", what);
	if (!ok)
		sFailures++;
}


static void
fatal_signal(int signal)
{
	char message[64];
	int length = snprintf(message, sizeof(message),
		"FAILED: signal %d (deadline or crash)\n", signal);
	write(1, message, length);
	_exit(3);
}


static volatile long sSignalResult;


static void
user_signal(int signal)
{
	(void)signal;
	sSignalResult = lazy_signal_function(234);
}


static pthread_barrier_t sBarrier;


static void*
worker(void* cookie)
{
	long index = (long)cookie;
	long failures = 0;
	pthread_barrier_wait(&sBarrier);
	for (int i = 0; i < kFunctionCount; i++) {
		/* each thread its own order: ascending, descending, strided */
		int n = (index % 2 == 0)
			? (i * (2 * index + 1) + index) % kFunctionCount
			: kFunctionCount - 1 - (i * (2 * index + 1)) % kFunctionCount;
		if (call_function(n) != (long)n * (n + 1) / 2)
			failures++;
	}
	return (void*)failures;
}


#if defined(__x86_64__)
static int
os_has_xsave_features(unsigned features)
{
	unsigned eax, ebx, ecx, edx;
	if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || (ecx & bit_OSXSAVE) == 0)
		return 0;
	unsigned low, high;
	__asm__ volatile("xgetbv" : "=a" (low), "=d" (high) : "c" (0));
	return (low & features) == features;
}


__attribute__((target("avx"))) static int
run_avx()
{
	__m256d v[8];
	for (int i = 0; i < 8; i++) {
		v[i] = _mm256_set_pd(i * 4 + 3.25, i * 4 + 2.25, i * 4 + 1.25,
			i * 4 + 0.25);
	}
	return lazy_check_avx(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}


__attribute__((target("avx512f"))) static int
run_avx512()
{
	__m512d v[8];
	for (int i = 0; i < 8; i++) {
		v[i] = _mm512_set_pd(i * 8 + 7.75, i * 8 + 6.75, i * 8 + 5.75,
			i * 8 + 4.75, i * 8 + 3.75, i * 8 + 2.75, i * 8 + 1.75,
			i * 8 + 0.75);
	}
	return lazy_check_avx512(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
}
#endif


#ifdef LAZY_MISSING
long lazy_call_missing(long x);
#endif


int
main(int argc, char** argv)
{
	signal(SIGALRM, fatal_signal);
	signal(SIGSEGV, fatal_signal);
	signal(SIGBUS, fatal_signal);
	signal(SIGILL, fatal_signal);
	alarm(60);

#ifdef LAZY_MISSING
	if (argc > 1 && strcmp(argv[1], "call") == 0) {
		printf("calling a function nobody defines\n");
		fflush(stdout);
		long result = lazy_call_missing(1);
		printf("FAILED: it returned %ld\n", result);
		return 1;
	}
	printf("ok: started although a jump slot cannot be resolved\n");
	return 0;
#else
	(void)argc;
	(void)argv;

	check(lazy_check_integers(1, -2, 3, -4, 5, -6, 7, -8),
		"eight integer arguments (two on the stack on x86_64)");
	check(lazy_check_floats(0.5, 1.5, 2.5, 3.5, 4.5, 5.5, 6.5, 7.5, 8.5),
		"eight double arguments in vector registers and one on the stack");
	check(lazy_check_mixed(11, 12.25, 13, 14.5f, 15, 16.75, 17, 18.125),
		"mixed integer and floating point arguments");
	struct lazy_big big = lazy_make_big(40);
	int bigOk = 1;
	for (int i = 0; i < 8; i++)
		bigOk &= big.values[i] == 40 + i;
	check(bigOk, "a structure returned through the indirect result register");
	check(lazy_sum_variadic(9, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0)
		== 45.0, "variadic call with nine double arguments");

#if defined(__x86_64__)
	if (os_has_xsave_features(0x6))
		check(run_avx(), "eight 256-bit AVX arguments");
	else
		printf("skipped: AVX (not enabled)\n");
	unsigned eax, ebx, ecx, edx;
	if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)
		&& (ebx & bit_AVX512F) != 0 && os_has_xsave_features(0xe6)) {
		check(run_avx512(), "eight 512-bit AVX-512 arguments");
	} else
		printf("skipped: AVX-512 (not available)\n");
#endif

	signal(SIGUSR1, user_signal);
	raise(SIGUSR1);
	check(sSignalResult == 1234, "first call from a signal handler");

	pthread_t threads[kThreadCount];
	pthread_barrier_init(&sBarrier, NULL, kThreadCount);
	for (long i = 0; i < kThreadCount; i++)
		pthread_create(&threads[i], NULL, worker, (void*)i);
	long failures = 0;
	for (int i = 0; i < kThreadCount; i++) {
		void* result;
		pthread_join(threads[i], &result);
		failures += (long)result;
	}
	char what[128];
	snprintf(what, sizeof(what),
		"%d threads binding %d chained functions at once (%ld wrong)",
		kThreadCount, kFunctionCount, failures);
	check(failures == 0, what);

	const char* lazy = getenv("LD_BIND_LAZY");
	printf("%s: %d failure(s) (LD_BIND_LAZY=%s)\n",
		sFailures == 0 ? "PASS" : "FAIL", sFailures, lazy ? lazy : "unset");
	return sFailures == 0 ? 0 : 1;
#endif
}
