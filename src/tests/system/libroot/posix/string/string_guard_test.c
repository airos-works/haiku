/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

/*!	The string functions with architecture implementations (#234) against
	byte-wise reference versions: all lengths 0..300 at many alignments, with
	the buffers ending right before an inaccessible page or starting right
	after one, so that reading past the end (or before the start) of a string
	crashes the test.
	In the tree it tests the system's functions (TEST_LIBROOT). The same file
	also tests the assembly sources on a build host: define nothing and link
	them renamed to t_<name> (-Dstrlen=t_strlen ...), or, for arm64 under
	qemu-aarch64, define FREESTANDING too (raw Linux system calls, no libc).
	Prints the number of checks, exits 1 on failure.
*/

#define _GNU_SOURCE
#define _DEFAULT_SOURCE
#include <stddef.h>
#include <stdint.h>

#ifdef FREESTANDING
typedef long ssize_t;
static long
sys(long n, long a, long b, long c, long d, long e, long f)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	register long x3 __asm__("x3") = d;
	register long x4 __asm__("x4") = e;
	register long x5 __asm__("x5") = f;
	__asm__ __volatile__("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2),
		"r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}
static void
out(const char* s)
{
	size_t n = 0;
	while (s[n])
		n++;
	sys(64, 1, (long)s, n, 0, 0, 0);
		// write
}
static void
die(int code)
{
	sys(93, code, 0, 0, 0, 0, 0);
		// exit
	for (;;)
		;
}
static void*
map_pages(size_t size)
{
	return (void*)sys(222, 0, size, 3, 0x22, -1, 0);
		// mmap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS
}
static void
protect_none(void* p, size_t size)
{
	sys(226, (long)p, size, 0, 0, 0, 0);
		// mprotect, PROT_NONE
}
void*
memset(void* d, int c, size_t n)
{
	unsigned char* p = d;
	while (n--)
		*p++ = c;
	return d;
}
void*
memcpy(void* d, const void* s, size_t n)
{
	unsigned char* p = d;
	const unsigned char* q = s;
	while (n--)
		*p++ = *q++;
	return d;
}
#else
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
static void out(const char* s) { fputs(s, stdout); fflush(stdout); }
static void die(int code) { exit(code); }
static void*
map_pages(size_t size)
{
	return mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
		-1, 0);
}
static void protect_none(void* p, size_t size) { mprotect(p, size, PROT_NONE); }
#endif

static void
out_number(long value)
{
	char buffer[24];
	int i = 23;
	int negative = value < 0;
	unsigned long v = negative ? -(unsigned long)value : (unsigned long)value;
	buffer[i] = 0;
	do {
		buffer[--i] = '0' + v % 10;
		v /= 10;
	} while (v != 0);
	if (negative)
		buffer[--i] = '-';
	out(buffer + i);
}

// SIZE_MAX, hidden from the compiler, which warns about such bounds
static volatile size_t sHuge = (size_t)-1;

static long sChecks;
static int sFailures;

static void
fail(const char* function, long a, long b, long c, long d)
{
	out("FAIL ");
	out(function);
	out(" ");
	out_number(a);
	out(" ");
	out_number(b);
	out(" ");
	out_number(c);
	out(" ");
	out_number(d);
	out("\n");
	if (++sFailures > 20)
		die(1);
}

#define CHECK(condition, function, a, b, c, d) \
	do { sChecks++; if (!(condition)) fail(function, a, b, c, d); } while (0)

// functions under test
#ifdef TEST_LIBROOT
	// the system's own: the test as it is in the tree
#	define t_strlen strlen
#	define t_strcmp strcmp
#	define t_strncmp strncmp
#	define t_memcmp memcmp
#	define t_memchr memchr
#	define t_memrchr memrchr
#	define t_strchrnul strchrnul
#	define t_strrchr strrchr
#	define t_stpcpy stpcpy
#	define t_memmove memmove
#	define t_strchr strchr
#	define t_strnlen strnlen
#	define t_strcpy strcpy
#	define HAVE_STRCHR
#else
size_t t_strlen(const char*);
int t_strcmp(const char*, const char*);
int t_strncmp(const char*, const char*, size_t);
int t_memcmp(const void*, const void*, size_t);
void* t_memchr(const void*, int, size_t);
void* t_memrchr(const void*, int, size_t);
char* t_strchrnul(const char*, int);
char* t_strrchr(const char*, int);
char* t_stpcpy(char*, const char*);
void* t_memmove(void*, const void*, size_t);
#ifdef HAVE_STRCHR
char* t_strchr(const char*, int);
size_t t_strnlen(const char*, size_t);
char* t_strcpy(char*, const char*);
#endif
#endif

// byte-wise references
static size_t
r_strlen(const char* s)
{
	size_t n = 0;
	while (s[n])
		n++;
	return n;
}
static int
sign(int v)
{
	return v < 0 ? -1 : v > 0 ? 1 : 0;
}
static int
r_strncmp(const char* a, const char* b, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		unsigned char x = a[i], y = b[i];
		if (x != y)
			return x - y;
		if (x == 0)
			return 0;
	}
	return 0;
}
static int
r_memcmp(const void* a, const void* b, size_t n)
{
	const unsigned char* x = a;
	const unsigned char* y = b;
	for (size_t i = 0; i < n; i++) {
		if (x[i] != y[i])
			return x[i] - y[i];
	}
	return 0;
}
static const void*
r_memchr(const void* s, int c, size_t n)
{
	const unsigned char* p = s;
	for (size_t i = 0; i < n; i++) {
		if (p[i] == (unsigned char)c)
			return p + i;
	}
	return NULL;
}
static const void*
r_memrchr(const void* s, int c, size_t n)
{
	const unsigned char* p = s;
	while (n-- > 0) {
		if (p[n] == (unsigned char)c)
			return p + n;
	}
	return NULL;
}
static const char*
r_strchrnul(const char* s, int c)
{
	while (*s && *(const unsigned char*)s != (unsigned char)c)
		s++;
	return s;
}
static const char*
r_strrchr(const char* s, int c)
{
	const char* last = NULL;
	for (;; s++) {
		if (*(const unsigned char*)s == (unsigned char)c)
			last = s;
		if (*s == 0)
			return last;
	}
}

static const size_t kPage = 4096;
static const size_t kMaxLength = 300;

// Two areas: [guard][4 pages data][guard]; strings are placed so that they
// end at the last byte before the upper guard or start at the first byte after
// the lower one.
static unsigned char* sArea[2];
static unsigned char* sLow[2];
static unsigned char* sHigh[2];
static unsigned int sSeed = 1;

static unsigned char
random_byte(void)
{
	sSeed = sSeed * 1103515245 + 12345;
	return sSeed >> 16;
}

static const char* sPhase = "setup";

#ifndef FREESTANDING
static void
crash_handler(int signal)
{
	// a function read past the end or before the start of its buffer
	out("FAIL: crashed in ");
	out(sPhase);
	out("\n");
	_exit(1);
}
#endif

static void
setup(void)
{
#ifndef FREESTANDING
	signal(SIGSEGV, crash_handler);
	signal(SIGBUS, crash_handler);
#endif
	for (int i = 0; i < 2; i++) {
		sArea[i] = map_pages(6 * kPage);
		if (sArea[i] == (void*)-1 || sArea[i] == NULL) {
			out("mmap failed\n");
			die(1);
		}
		protect_none(sArea[i], kPage);
		protect_none(sArea[i] + 5 * kPage, kPage);
		sLow[i] = sArea[i] + kPage;
		sHigh[i] = sArea[i] + 5 * kPage;
	}
}

// fills [start, start + length) with non-NUL bytes != avoid, terminated
static char*
make_string(unsigned char* start, size_t length, int avoid)
{
	for (size_t i = 0; i < length; i++) {
		unsigned char c;
		do {
			c = random_byte();
		} while (c == 0 || c == (unsigned char)avoid);
		start[i] = c;
	}
	start[length] = 0;
	return (char*)start;
}

// a string of length bytes in area that ends right before the guard page
// (placement 0) or starts right after the lower one (1), or sits at the
// given alignment inside the area (2)
static char*
place_string(int area, size_t length, int placement, size_t alignment,
	int avoid)
{
	unsigned char* start;
	if (placement == 0)
		start = sHigh[area] - length - 1;
	else if (placement == 1)
		start = sLow[area];
	else
		start = sLow[area] + kPage + alignment;
	return make_string(start, length, avoid);
}

static void
test_strlen(void)
{
	for (size_t length = 0; length <= kMaxLength; length++) {
		for (int placement = 0; placement < 3; placement++) {
			for (size_t alignment = 0; alignment < (placement == 2 ? 64 : 1);
					alignment++) {
				char* s = place_string(0, length, placement, alignment, -1);
				CHECK(t_strlen(s) == length, "strlen", length, placement,
					alignment, t_strlen(s));
#ifdef HAVE_STRCHR
				for (size_t max = length > 3 ? length - 3 : 0;
						max <= length + 3; max++) {
					size_t expected = max < length ? max : length;
					CHECK(t_strnlen(s, max) == expected, "strnlen", length,
						max, placement, t_strnlen(s, max));
				}
				CHECK(t_strnlen(s, sHuge) == length, "strnlen max",
					length, placement, alignment, 0);
#endif
			}
		}
	}
	// also a string that ends at every offset of the last page
	for (size_t offset = 0; offset < 64; offset++) {
		char* s = make_string(sHigh[0] - 2 * kPage + offset, kPage
			- offset * 3 - 1, -1);
		CHECK(t_strlen(s) == r_strlen(s), "strlen long", offset, 0, 0, 0);
	}
}

static void
test_compare(void)
{
	static const int kBytes[] = { 1, 0x7f, 0x80, 0xff };
	for (size_t length = 0; length <= kMaxLength; length++) {
		// both strings end before a guard page, or are at alignments a, b
		int alignments = length <= 80 ? 32 : 8;
		for (int placementA = 0; placementA < 3; placementA++)
		for (int placementB = 0; placementB < 3; placementB++)
		for (int a = 0; a < (placementA == 2 ? alignments : 1); a++)
		for (int b = 0; b < (placementB == 2 ? alignments : 1); b++) {
			char* x = place_string(0, length, placementA, a, -1);
			char* y = (char*)(placementB == 0 ? sHigh[1] - length - 1
				: placementB == 1 ? sLow[1] : sLow[1] + kPage + b);
			memcpy(y, x, length + 1);

			CHECK(t_strcmp(x, y) == 0, "strcmp equal", length, placementA,
				placementB, a * 100 + b);
			CHECK(t_memcmp(x, y, length) == 0, "memcmp equal", length,
				placementA, placementB, a * 100 + b);
			CHECK(t_strncmp(x, y, length) == 0, "strncmp equal", length,
				placementA, placementB, a * 100 + b);
			CHECK(t_strncmp(x, y, sHuge) == 0, "strncmp equal max",
				length, placementA, placementB, a * 100 + b);

			// a difference at some positions (all for short strings)
			size_t step = length <= 64 ? 1 : 7;
			for (size_t p = 0; p < length; p += step) {
				char saved = y[p];
				int value = kBytes[p & 3];
				if ((unsigned char)value == (unsigned char)saved)
					value ^= 2;
				y[p] = value;
				int expected = sign(r_strncmp(x, y, sHuge));
				CHECK(sign(t_strcmp(x, y)) == expected, "strcmp", length, p,
					placementA * 10 + placementB, a * 100 + b);
				CHECK(sign(t_memcmp(x, y, length)) == sign(r_memcmp(x, y,
					length)), "memcmp", length, p, placementA * 10
					+ placementB, a * 100 + b);
				CHECK(sign(t_memcmp(x, y, p)) == 0, "memcmp prefix", length,
					p, placementA * 10 + placementB, a * 100 + b);
				size_t limits[] = { p, p + 1, length, sHuge };
				for (int l = 0; l < 4; l++) {
					CHECK(sign(t_strncmp(x, y, limits[l]))
						== sign(r_strncmp(x, y, limits[l])), "strncmp",
						length, p, limits[l] == (size_t)-1 ? -1
							: (long)limits[l], a * 100 + b);
				}
				// y ends at p instead
				y[p] = 0;
				CHECK(sign(t_strcmp(x, y)) == 1, "strcmp shorter", length, p,
					placementA * 10 + placementB, a * 100 + b);
				CHECK(sign(t_strcmp(y, x)) == -1, "strcmp longer", length, p,
					placementA * 10 + placementB, a * 100 + b);
				CHECK(sign(t_strncmp(y, x, length + 1)) == -1, "strncmp longer",
					length, p, placementA * 10 + placementB, a * 100 + b);
				y[p] = saved;
			}
		}
	}
}

static void
test_search(void)
{
	static const int kCharacters[] = { 'a', 0x80, 0xff, 0x100 + 'b', 0 };
	for (size_t length = 0; length <= kMaxLength; length++)
	for (int placement = 0; placement < 3; placement++)
	for (size_t alignment = 0; alignment < (placement == 2 ? 32 : 1);
			alignment++)
	for (int k = 0; k < 5; k++) {
		int c = kCharacters[k];
		char* s = place_string(0, length, placement, alignment, c);
		size_t step = length <= 64 ? 1 : 5;
		// not there
		CHECK(t_strchrnul(s, c) == r_strchrnul(s, c), "strchrnul none",
			length, placement, alignment, c);
		CHECK(t_strrchr(s, c) == r_strrchr(s, c), "strrchr none", length,
			placement, alignment, c);
		CHECK(t_memchr(s, c, length) == r_memchr(s, c, length), "memchr none",
			length, placement, alignment, c);
		CHECK(t_memrchr(s, c, length) == r_memrchr(s, c, length),
			"memrchr none", length, placement, alignment, c);
#ifdef HAVE_STRCHR
		CHECK(t_strchr(s, c) == (c == 0 ? s + length : NULL), "strchr none",
			length, placement, alignment, c);
#endif
		if (c == 0)
			continue;
		for (size_t p = 0; p < length; p += step) {
			char saved = s[p];
			s[p] = c;
			if (p + 3 < length)
				s[p + 3] = c;
			CHECK(t_strchrnul(s, c) == s + p, "strchrnul", length, p,
				placement * 100 + alignment, c);
			CHECK(t_strrchr(s, c) == r_strrchr(s, c), "strrchr", length, p,
				placement * 100 + alignment, c);
			CHECK(t_memchr(s, c, length) == s + p, "memchr", length, p,
				placement * 100 + alignment, c);
			CHECK(t_memchr(s, c, p) == NULL, "memchr short", length, p,
				placement * 100 + alignment, c);
			CHECK(t_memrchr(s, c, length) == r_memrchr(s, c, length),
				"memrchr", length, p, placement * 100 + alignment, c);
			CHECK(t_memrchr(s + p + 1, c, length - p - 1) == r_memrchr(
				s + p + 1, c, length - p - 1), "memrchr tail", length, p,
				placement * 100 + alignment, c);
#ifdef HAVE_STRCHR
			CHECK(t_strchr(s, c) == s + p, "strchr", length, p,
				placement * 100 + alignment, c);
#endif
			s[p] = saved;
			if (p + 3 < length)
				s[p + 3] = saved;
		}
	}
	// memchr with a length beyond the end of the address space stops at
	// the match (strnlen() is called like that)
	char* s = place_string(0, 100, 0, 0, 'z');
	s[50] = 'z';
	CHECK(t_memchr(s, 'z', sHuge) == s + 50, "memchr huge", 0, 0, 0, 0);
	CHECK(t_memchr(s, 0, sHuge) == s + 100, "memchr huge nul", 0, 0, 0,
		0);
}

static void
test_copy(void)
{
	for (size_t length = 0; length <= kMaxLength; length++)
	for (int placementS = 0; placementS < 3; placementS++)
	for (int placementD = 0; placementD < 3; placementD++)
	for (size_t a = 0; a < (placementS == 2 ? 32 : 1); a++)
	for (size_t b = 0; b < (placementD == 2 ? 32 : 1); b++) {
		char* s = place_string(0, length, placementS, a, -1);
		char* d = (char*)(placementD == 0 ? sHigh[1] - length - 1
			: placementD == 1 ? sLow[1] + 1 : sLow[1] + kPage + b);
		// canaries around the destination
		memset(d - 1, 0x5a, length + 2);
		if (placementD != 0)
			d[length + 1] = 0x5a;
		d[-1] = 0x5a;
		char* end = t_stpcpy(d, s);
		CHECK(end == d + length, "stpcpy end", length, placementS * 10
			+ placementD, a, b);
		CHECK(r_memcmp(d, s, length + 1) == 0, "stpcpy data", length,
			placementS * 10 + placementD, a, b);
		CHECK((unsigned char)d[-1] == 0x5a, "stpcpy before", length,
			placementS * 10 + placementD, a, b);
		if (placementD != 0) {
			CHECK((unsigned char)d[length + 1] == 0x5a, "stpcpy after", length,
				placementS * 10 + placementD, a, b);
		}
#ifdef HAVE_STRCHR
		memset(d, 0x5a, length + 1);
		CHECK(t_strcpy(d, s) == d, "strcpy result", length, 0, a, b);
		CHECK(r_memcmp(d, s, length + 1) == 0, "strcpy data", length,
			placementS * 10 + placementD, a, b);
#endif
	}
}

static unsigned char sExpected[2 * 300 + 256];

static void
test_memmove(void)
{
	unsigned char* base = sLow[0] + 512;
	for (size_t length = 0; length <= kMaxLength; length++)
	for (int distance = -80; distance <= 80; distance++)
	for (size_t alignment = 0; alignment < 16; alignment++) {
		unsigned char* source = base + alignment;
		unsigned char* destination = source + distance;
		// region covering both, with margins
		unsigned char* low = (distance < 0 ? destination : source) - 16;
		size_t span = length + (distance < 0 ? -distance : distance) + 32;
		for (size_t i = 0; i < span; i++)
			low[i] = random_byte();
		memcpy(sExpected, low, span);
		// reference result: copy through a temporary
		unsigned char temporary[300];
		memcpy(temporary, source, length);
		memcpy(sExpected + (destination - low), temporary, length);

		void* result = t_memmove(destination, source, length);
		CHECK(result == destination, "memmove result", length, distance,
			alignment, 0);
		CHECK(r_memcmp(low, sExpected, span) == 0, "memmove", length,
			distance, alignment, 0);
	}
	// large overlapping moves, both directions, against the guard page
	for (size_t length = 4000; length <= 12000; length += 1999)
	for (int distance = -67; distance <= 67; distance += 67) {
		unsigned char* source = sHigh[0] - length
			- (distance > 0 ? distance : 0);
		unsigned char* destination = source + distance;
		for (size_t i = 0; i < length; i++)
			source[i] = i * 7 + (i >> 8);
		t_memmove(destination, source, length);
		int ok = 1;
		for (size_t i = 0; i < length; i++)
			ok &= destination[i] == (unsigned char)(i * 7 + (i >> 8));
		CHECK(ok, "memmove large", length, distance, 0, 0);
	}
}

int
main(void)
{
	setup();
	sPhase = "test_strlen";
	test_strlen();
	out("strlen/strnlen done\n");
	sPhase = "test_compare";
	test_compare();
	out("strcmp/strncmp/memcmp done\n");
	sPhase = "test_search";
	test_search();
	out("memchr/memrchr/strchr/strchrnul/strrchr done\n");
	sPhase = "test_copy";
	test_copy();
	out("stpcpy/strcpy done\n");
	sPhase = "test_memmove";
	test_memmove();
	out("memmove done\n");
	out_number(sChecks);
	out(" checks, ");
	out_number(sFailures);
	out(" failures\n");
	return sFailures != 0;
}

#ifdef FREESTANDING
void
_start(void)
{
	die(main());
}
#endif
