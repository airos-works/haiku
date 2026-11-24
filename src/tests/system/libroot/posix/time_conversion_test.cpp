/*
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */


/*!	gmtime_r(), timegm(), localtime_r() and mktime() (haiku#176).

	Each call asked the kernel for the time zone and made a new ICU calendar:
	6-12 µs per call. gmtime_r() and timegm() are now arithmetic, the local
	conversions keep their calendar, and the zone is asked for only when it
	has been changed.

	Checked here:
	- gmtime_r() and timegm() against an independent computation, over a
	  large sample from 1583 to the far future;
	- localtime_r() and mktime() at known DST changes;
	- mktime() of a time before 1970 (it was one second late, depending on
	  the millisecond it was called in);
	- a change of the system's time zone is followed at once (as root);
	- conversions in several threads while the zone changes;
	- the time per call (gmtime_r() must be under 2 µs; it took 6-9).
*/


#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <OS.h>


extern "C" status_t _kern_get_timezone(int32* _timezoneOffset, char* name,
	size_t nameLength);
extern "C" status_t _kern_set_timezone(int32 timezoneOffset,
	const char* name, size_t nameLength);


static int sFailures;


static void
check(bool ok, const char* format, ...)
{
	va_list args;
	va_start(args, format);
	printf("%s: ", ok ? "ok" : "FAILED");
	vprintf(format, args);
	printf("\n");
	va_end(args);
	if (!ok)
		sFailures++;
}


static unsigned long long sSeed = 0x9e3779b97f4a7c15ULL;


static unsigned long long
random_number()
{
	sSeed ^= sSeed << 13;
	sSeed ^= sSeed >> 7;
	sSeed ^= sSeed << 17;
	return sSeed;
}


//	#pragma mark - reference


static long long
floor_divide(long long a, long long b)
{
	return a / b - (a % b < 0 ? 1 : 0);
}


/*!	Days since 1970-01-01 of a proleptic Gregorian date (month 1-12). */
static long long
days_from_civil(long long year, long long month, long long day)
{
	year -= month <= 2;
	long long era = floor_divide(year, 400);
	long long yearOfEra = year - era * 400;
	long long dayOfYear = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5
		+ day - 1;
	long long dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100
		+ dayOfYear;
	return era * 146097 + dayOfEra - 719468;
}


static bool
is_leap(long long year)
{
	return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}


static void
reference_gmtime(long long t, struct tm& tm)
{
	long long days = floor_divide(t, 86400);
	long long seconds = t - days * 86400;

	long long z = days + 719468;
	long long era = floor_divide(z, 146097);
	long long dayOfEra = z - era * 146097;
	long long yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524
		- dayOfEra / 146096) / 365;
	long long year = yearOfEra + era * 400;
	long long dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4
		- yearOfEra / 100);
	long long mp = (5 * dayOfYear + 2) / 153;
	long long day = dayOfYear - (153 * mp + 2) / 5 + 1;
	long long month = mp < 10 ? mp + 3 : mp - 9;
	if (month <= 2)
		year++;

	memset(&tm, 0, sizeof(tm));
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	tm.tm_hour = seconds / 3600;
	tm.tm_min = seconds / 60 % 60;
	tm.tm_sec = seconds % 60;
	tm.tm_wday = (int)(floor_divide(days + 4, 7) * -7 + days + 4);
	tm.tm_yday = days - days_from_civil(year, 1, 1);
	(void)is_leap;
}


static bool
same_fields(const struct tm& a, const struct tm& b)
{
	return a.tm_year == b.tm_year && a.tm_mon == b.tm_mon
		&& a.tm_mday == b.tm_mday && a.tm_hour == b.tm_hour
		&& a.tm_min == b.tm_min && a.tm_sec == b.tm_sec
		&& a.tm_wday == b.tm_wday && a.tm_yday == b.tm_yday;
}


static const char*
format(const struct tm& tm)
{
	static char buffer[128];
	snprintf(buffer, sizeof(buffer), "%d-%02d-%02d %02d:%02d:%02d wday %d "
		"yday %d dst %d off %d %s", tm.tm_year + 1900, tm.tm_mon + 1,
		tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday, tm.tm_yday,
		tm.tm_isdst, (int)tm.tm_gmtoff, tm.tm_zone != NULL ? tm.tm_zone : "-");
	return buffer;
}


//	#pragma mark - UTC


static void
test_utc()
{
	static const long long kSpecial[] = {
		-12212553600LL, -12212553599LL, -1, 0, 1, 68169599, 68169600,
		951782399, 951782400, 4107542399LL, 4107542400LL, 253402300799LL,
		9007199254739LL, 9007199254740LL
	};
	const int kCount = 1000000;
	int wrong = 0;
	int wrongRoundTrip = 0;
	int wrongZone = 0;
	for (int i = 0; i < kCount + (int)(sizeof(kSpecial) / sizeof(kSpecial[0]));
			i++) {
		long long t;
		if (i >= kCount)
			t = kSpecial[i - kCount];
		else if (i % 2 == 0)
			t = (long long)(random_number() % 20000000000ULL) - 12212553600LL;
		else {
			t = (long long)(random_number() % (9007199254740ULL
				+ 12212553600ULL)) - 12212553600LL;
		}

		time_t time = t;
		struct tm tm;
		struct tm expected;
		gmtime_r(&time, &tm);
		reference_gmtime(t, expected);
		if (!same_fields(tm, expected)) {
			if (wrong++ < 5) {
				printf("\tgmtime_r(%lld) = %s, expected ", t, format(tm));
				printf("%s\n", format(expected));
			}
		}
		if (tm.tm_isdst != 0 || tm.tm_gmtoff != 0 || tm.tm_zone == NULL
			|| strcmp(tm.tm_zone, "GMT") != 0) {
			wrongZone++;
		}

		struct tm copy = tm;
		if ((long long)timegm(&copy) != t || !same_fields(copy, expected))
			wrongRoundTrip++;
	}
	check(wrong == 0, "gmtime_r() of %d times: %d differ from the reference",
		kCount, wrong);
	check(wrongZone == 0, "gmtime_r(): %d without dst 0, offset 0, \"GMT\"",
		wrongZone);
	check(wrongRoundTrip == 0, "timegm(gmtime_r(t)) == t: %d differ",
		wrongRoundTrip);

	// timegm() normalizes what is out of range
	int wrongNormalized = 0;
	for (int i = 0; i < 200000; i++) {
		struct tm tm;
		memset(&tm, 0, sizeof(tm));
		tm.tm_year = (int)(random_number() % 400) - 100;
		tm.tm_mon = (int)(random_number() % 60) - 24;
		tm.tm_mday = (int)(random_number() % 1000) - 400;
		tm.tm_hour = (int)(random_number() % 100) - 50;
		tm.tm_min = (int)(random_number() % 200) - 100;
		tm.tm_sec = (int)(random_number() % 200000) - 100000;

		long long year = 1900LL + tm.tm_year + floor_divide(tm.tm_mon, 12);
		long long month = tm.tm_mon - floor_divide(tm.tm_mon, 12) * 12;
		long long expected = days_from_civil(year, month + 1, 1) * 86400
			+ (tm.tm_mday - 1) * 86400LL + tm.tm_hour * 3600LL
			+ tm.tm_min * 60LL + tm.tm_sec;
		struct tm expectedTm;
		reference_gmtime(expected, expectedTm);

		long long result = timegm(&tm);
		if (result != expected || !same_fields(tm, expectedTm)) {
			if (wrongNormalized++ < 5) {
				printf("\ttimegm() = %lld, expected %lld (%s)\n", result,
					expected, format(tm));
			}
		}
	}
	check(wrongNormalized == 0, "timegm() of 200000 fields out of range: %d "
		"wrong", wrongNormalized);
}


//	#pragma mark - local time


struct LocalCase {
	const char*	zone;
	long long	time;
	int			hour;
	int			minute;
	int			isDST;
	int			offset;
	bool		unique;
		// the local time occurs once: mktime() gives the time back
};


static void
test_local()
{
	static const LocalCase kCases[] = {
		// Berlin, 2026-03-29 01:00 UTC: 02:00 CET becomes 03:00 CEST
		{ ":Europe/Berlin", 1774745999LL, 1, 59, 0, 3600, true },
		{ ":Europe/Berlin", 1774746000LL, 3, 0, 1, 7200, true },
		// and back on 2026-10-25 01:00 UTC (02:00-03:00 occurs twice)
		{ ":Europe/Berlin", 1792889999LL, 2, 59, 1, 7200, false },
		{ ":Europe/Berlin", 1792890000LL, 2, 0, 0, 3600, true },
		// New York, 2026-03-08 07:00 UTC
		{ ":America/New_York", 1772953199LL, 1, 59, 0, -18000, true },
		{ ":America/New_York", 1772953200LL, 3, 0, 1, -14400, true },
		// Lord Howe Island: 30 minutes of DST, 2026-10-04 at 02:00 local
		{ ":Australia/Lord_Howe", 1791041399LL, 1, 59, 0, 37800, true },
		{ ":Australia/Lord_Howe", 1791041400LL, 2, 30, 1, 39600, true },
		// Kathmandu, +5:30 and +5:45
		{ ":Asia/Kathmandu", 0, 5, 30, 0, 19800, true },
		{ ":Asia/Kathmandu", 1000000000LL, 7, 31, 0, 20700, true },
	};

	for (size_t i = 0; i < sizeof(kCases) / sizeof(kCases[0]); i++) {
		const LocalCase& test = kCases[i];
		setenv("TZ", test.zone, 1);
		time_t time = test.time;
		struct tm tm;
		memset(&tm, 0, sizeof(tm));
		localtime_r(&time, &tm);
		bool ok = tm.tm_hour == test.hour && tm.tm_min == test.minute
			&& tm.tm_isdst == test.isDST && tm.tm_gmtoff == test.offset;
		check(ok, "localtime_r(%lld) in %s: %s", test.time, test.zone,
			format(tm));

		if (!test.unique)
			continue;
		struct tm copy = tm;
		copy.tm_isdst = -1;
		long long back = mktime(&copy);
		check(back == test.time && same_fields(copy, tm), "mktime() of it: "
			"%lld", back);
	}

	// The same time, before and after a change of TZ.
	time_t time = 1000000000;
	struct tm tm;
	setenv("TZ", ":Asia/Tokyo", 1);
	localtime_r(&time, &tm);
	int tokyo = tm.tm_hour;
	setenv("TZ", ":Europe/London", 1);
	localtime_r(&time, &tm);
	check(tokyo == 10 && tm.tm_hour == 2, "a change of TZ is followed: %d "
		"in Tokyo, %d in London", tokyo, tm.tm_hour);

	// Before 1970: whole seconds, whatever the millisecond is.
	setenv("TZ", ":UTC", 1);
	int wrong = 0;
	for (int i = 0; i < 200; i++) {
		memset(&tm, 0, sizeof(tm));
		tm.tm_year = 60;
		tm.tm_mday = 1;
		tm.tm_isdst = -1;
		long long result = mktime(&tm);
		if (result != -315619200LL)
			wrong++;
		snooze(1000 + i * 37);
	}
	check(wrong == 0, "mktime(1960-01-01 00:00:00 UTC) == -315619200: %d of "
		"200 differ", wrong);

	unsetenv("TZ");
}


//	#pragma mark - the system's time zone


struct ZoneOffsets {
	const char*	name;
	int			standard;
	int			daylight;
};

static const ZoneOffsets kZones[] = {
	{ "Europe/Berlin", 3600, 7200 },
	{ "America/New_York", -18000, -14400 },
	{ "Asia/Tokyo", 32400, 32400 }
};

static volatile bool sStop;
static int sThreadFailures[4];


static void*
convert_thread(void* cookie)
{
	int index = (int)(addr_t)cookie;
	unsigned long long seed = index + 1;
	while (!sStop) {
		seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
		time_t time = (time_t)((seed >> 33) % 2000000000);
		struct tm tm;
		localtime_r(&time, &tm);
		bool known = false;
		for (size_t i = 0; i < sizeof(kZones) / sizeof(kZones[0]); i++) {
			if (tm.tm_gmtoff == kZones[i].standard
				|| tm.tm_gmtoff == kZones[i].daylight) {
				known = true;
			}
		}
		// (a zone's offsets before 1970 or so are not all listed above)
		if (!known && time > 400000000)
			sThreadFailures[index]++;
		struct tm copy = tm;
		copy.tm_isdst = -1;
		mktime(&copy);
	}
	return NULL;
}


static void
test_system_zone()
{
	if (geteuid() != 0) {
		printf("skipped: setting the system's time zone needs root\n");
		return;
	}

	int32 originalOffset = 0;
	char originalName[B_FILE_NAME_LENGTH] = "GMT";
	_kern_get_timezone(&originalOffset, originalName, sizeof(originalName));
	unsetenv("TZ");

	// summer in the north
	time_t time = 1782000000;
	bool followed = true;
	for (size_t i = 0; i < sizeof(kZones) / sizeof(kZones[0]); i++) {
		_kern_set_timezone(kZones[i].standard, kZones[i].name,
			strlen(kZones[i].name));
		struct tm tm;
		localtime_r(&time, &tm);
		if (tm.tm_gmtoff != kZones[i].daylight) {
			printf("\tsystem time zone %s: localtime_r() has offset %d\n",
				kZones[i].name, (int)tm.tm_gmtoff);
			followed = false;
		}
	}
	check(followed, "a change of the system's time zone is followed at once");

	// conversions while the zone changes
	pthread_t threads[4];
	sStop = false;
	for (int i = 0; i < 4; i++)
		pthread_create(&threads[i], NULL, convert_thread, (void*)(addr_t)i);
	bigtime_t end = system_time() + 2000000;
	int changes = 0;
	while (system_time() < end) {
		const ZoneOffsets& zone
			= kZones[changes % (sizeof(kZones) / sizeof(kZones[0]))];
		_kern_set_timezone(zone.standard, zone.name, strlen(zone.name));
		changes++;
		snooze(2000);
	}
	sStop = true;
	int failures = 0;
	for (int i = 0; i < 4; i++) {
		pthread_join(threads[i], NULL);
		failures += sThreadFailures[i];
	}
	check(failures == 0, "4 threads converting during %d changes of the "
		"system's time zone: %d results of no zone used", changes, failures);

	_kern_set_timezone(originalOffset, originalName, strlen(originalName));
}


//	#pragma mark - time per call


static double
nanoseconds_per_call(int which, int count)
{
	struct tm tm;
	volatile long sink = 0;
	bigtime_t start = system_time();
	for (int i = 0; i < count; i++) {
		time_t time = 1700000000 + i * 37;
		switch (which) {
			case 0:
				gmtime_r(&time, &tm);
				sink += tm.tm_sec;
				break;
			case 1:
				localtime_r(&time, &tm);
				sink += tm.tm_sec;
				break;
			case 2:
				localtime_r(&time, &tm);
				sink += mktime(&tm);
				break;
		}
	}
	return (system_time() - start) * 1000.0 / count;
}


static void
test_speed()
{
	setenv("TZ", ":Europe/Berlin", 1);
	const int kCount = 200000;
	double gmtime = nanoseconds_per_call(0, kCount);
	double localtime = nanoseconds_per_call(1, kCount);
	double both = nanoseconds_per_call(2, kCount);
	printf("gmtime_r: %.0f ns, localtime_r: %.0f ns, localtime_r + mktime: "
		"%.0f ns per call\n", gmtime, localtime, both);
	check(gmtime < 2000, "gmtime_r() takes less than 2 µs");
	unsetenv("TZ");
}


int
main()
{
	setvbuf(stdout, NULL, _IONBF, 0);
	alarm(120);

	test_utc();
	test_local();
	test_system_zone();
	test_speed();

	printf("%s\n", sFailures == 0 ? "PASSED" : "FAILED");
	return sFailures == 0 ? 0 : 1;
}
