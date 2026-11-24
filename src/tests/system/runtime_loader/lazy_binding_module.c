/* SPDX-License-Identifier: MIT */
/*
 * The library side of lazy_binding_test.c. Every function is called through
 * the program's PLT, and calls within the library go through the library's
 * own PLT, so with LD_BIND_LAZY=1 each first call enters the runtime
 * loader's lazy binding entry. The functions check that every argument
 * register arrived intact.
 */
#include <stdarg.h>
#include <stdint.h>

#if defined(__x86_64__)
#	include <immintrin.h>
#endif


int
lazy_check_integers(long a, long b, long c, long d, long e, long f, long g,
	long h)
{
	return a == 1 && b == -2 && c == 3 && d == -4 && e == 5 && f == -6
		&& g == 7 && h == -8;
}


int
lazy_check_floats(double a, double b, double c, double d, double e,
	double f, double g, double h, double i)
{
	return a == 0.5 && b == 1.5 && c == 2.5 && d == 3.5 && e == 4.5
		&& f == 5.5 && g == 6.5 && h == 7.5 && i == 8.5;
}


int
lazy_check_mixed(int a, double b, long c, float d, char e, double f,
	short g, double h)
{
	return a == 11 && b == 12.25 && c == 13 && d == 14.5f && e == 15
		&& f == 16.75 && g == 17 && h == 18.125;
}


struct lazy_big {
	long values[8];
};


/* returned through the indirect result register (x8 on arm64, %rdi on
   x86_64) */
struct lazy_big
lazy_make_big(long first)
{
	struct lazy_big result;
	for (int i = 0; i < 8; i++)
		result.values[i] = first + i;
	return result;
}


/* variadic: %al holds the number of vector registers used on x86_64 */
double
lazy_sum_variadic(int count, ...)
{
	va_list args;
	va_start(args, count);
	double sum = 0;
	for (int i = 0; i < count; i++)
		sum += va_arg(args, double);
	va_end(args);
	return sum;
}


#if defined(__x86_64__)

__attribute__((target("avx"))) int
lazy_check_avx(__m256d a, __m256d b, __m256d c, __m256d d, __m256d e,
	__m256d f, __m256d g, __m256d h)
{
	__m256d values[8] = { a, b, c, d, e, f, g, h };
	for (int i = 0; i < 8; i++) {
		double lanes[4];
		_mm256_storeu_pd(lanes, values[i]);
		for (int lane = 0; lane < 4; lane++) {
			if (lanes[lane] != i * 4 + lane + 0.25)
				return 0;
		}
	}
	return 1;
}


__attribute__((target("avx512f"))) int
lazy_check_avx512(__m512d a, __m512d b, __m512d c, __m512d d, __m512d e,
	__m512d f, __m512d g, __m512d h)
{
	__m512d values[8] = { a, b, c, d, e, f, g, h };
	for (int i = 0; i < 8; i++) {
		double lanes[8];
		_mm512_storeu_pd(lanes, values[i]);
		for (int lane = 0; lane < 8; lane++) {
			if (lanes[lane] != i * 8 + lane + 0.75)
				return 0;
		}
	}
	return 1;
}

#endif


/* many functions, for threads that bind them at the same time; each calls
   the previous one through the library's PLT */
#define LAZY_FUNCTION(n, previous) \
	long lazy_function_##n(long x); \
	long lazy_function_##n(long x) \
	{ \
		return previous + x * 0 + n; \
	}

long lazy_function_0(long x);
long
lazy_function_0(long x)
{
	return x * 0;
}

#define F1(n, p)	LAZY_FUNCTION(n, lazy_function_##p(x))
F1(1, 0) F1(2, 1) F1(3, 2) F1(4, 3) F1(5, 4) F1(6, 5) F1(7, 6) F1(8, 7)
F1(9, 8) F1(10, 9) F1(11, 10) F1(12, 11) F1(13, 12) F1(14, 13) F1(15, 14)
F1(16, 15) F1(17, 16) F1(18, 17) F1(19, 18) F1(20, 19) F1(21, 20)
F1(22, 21) F1(23, 22) F1(24, 23) F1(25, 24) F1(26, 25) F1(27, 26)
F1(28, 27) F1(29, 28) F1(30, 29) F1(31, 30) F1(32, 31) F1(33, 32)
F1(34, 33) F1(35, 34) F1(36, 35) F1(37, 36) F1(38, 37) F1(39, 38)
F1(40, 39) F1(41, 40) F1(42, 41) F1(43, 42) F1(44, 43) F1(45, 44)
F1(46, 45) F1(47, 46) F1(48, 47) F1(49, 48) F1(50, 49) F1(51, 50)
F1(52, 51) F1(53, 52) F1(54, 53) F1(55, 54) F1(56, 55) F1(57, 56)
F1(58, 57) F1(59, 58) F1(60, 59) F1(61, 60) F1(62, 61) F1(63, 62)


long
lazy_signal_function(long x)
{
	return x + 1000;
}


#ifdef LAZY_MISSING
/* defined nowhere: only a lazily bound program can start */
extern long lazy_missing_function(long x);

long
lazy_call_missing(long x)
{
	return lazy_missing_function(x);
}
#endif
