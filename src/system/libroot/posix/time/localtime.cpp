/*
 * Copyright 2010, Oliver Tappe, zooey@hirschkaefer.de.
 * Distributed under the terms of the MIT License.
 */


#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <syscalls.h>

#include <StorageDefs.h>

#include <errno_private.h>
#include <libroot_private.h>
#include <locks.h>
#include "LocaleBackend.h"


using BPrivate::Libroot::GetCurrentLocaleBackend;
using BPrivate::Libroot::LocaleBackend;


static char sStandardTZName[64] 		= { "GMT" };
static char sDaylightSavingTZName[64]	= { "GMT" };


char* tzname[2] = {
	sStandardTZName,
	sDaylightSavingTZName
};
long timezone = 0;
int daylight = 0;


// These three functions are used as a fallback when the locale backend could not
// be loaded, or for the POSIX locale. They are implemented in localtime_fallback.c.
extern "C" struct tm* __gmtime_r_fallback(const time_t* timep, struct tm* tmp);
extern "C" time_t __mktime_fallback(struct tm* tmp);
extern "C" time_t __timegm_fallback(struct tm* tmp);


// musl's arithmetic, in posix/musl/time
extern "C" int __secs_to_tm(long long t, struct tm* tm);
extern "C" long long __tm_to_secs(const struct tm* tm);


// The ICU backend's calendar is the Julian one before 1582-10-15 (and its
// day of the year counts the days 1582 had), and it computes in milliseconds
// held in a double, which are exact up to 2^53. Between these limits the
// arithmetic gives the same results as the backend; outside of them the
// backend is still asked.
static const long long kFirstGregorianYear = 1583;
static const long long kFirstGregorianTime = -12212553600LL;
	// 1583-01-01 00:00:00 UTC
static const long long kLastExactTime = 9007199254740LL;


// The system's time zone, as the kernel had it when its generation was
// sTimeZoneGeneration. Asking for it at every conversion was a syscall.
static mutex sTimeZoneLock = MUTEX_INITIALIZER("time zone");
static char sTimeZoneID[B_FILE_NAME_LENGTH] = { "GMT" };
static int32 sTimeZoneGeneration;
static bool sTimeZoneKnown;
static pthread_once_t sTimeZoneForkHookOnce = PTHREAD_ONCE_INIT;


static void
reinit_time_zone_lock_after_fork()
{
	mutex_init(&sTimeZoneLock, "time zone");
}


static void
register_time_zone_fork_hook()
{
	pthread_atfork(NULL, NULL, &reinit_time_zone_lock_after_fork);
}


/*!	Brings the locale backend's time zone up to date: the system's, unless
	the TZ environment variable names another. The system's is asked from
	the kernel only when it has been changed (or when \a force is \c true).
*/
static void
update_time_zone(bool force)
{
	if (GetCurrentLocaleBackend() == NULL && LocaleBackend::LoadBackend() != B_OK)
		return;

	pthread_once(&sTimeZoneForkHookOnce, &register_time_zone_fork_hook);

	mutex_lock(&sTimeZoneLock);

	int32 generation = __get_time_zone_generation();
	if (force || !sTimeZoneKnown || generation != sTimeZoneGeneration) {
		// (the generation is read first: a change during the syscall makes
		// the next call ask again)
		strlcpy(sTimeZoneID, "GMT", sizeof(sTimeZoneID));
		_kern_get_timezone(NULL, sTimeZoneID, sizeof(sTimeZoneID));
		sTimeZoneGeneration = generation;
		sTimeZoneKnown = true;
	}

	GetCurrentLocaleBackend()->TZSet(sTimeZoneID, getenv("TZ"));

	mutex_unlock(&sTimeZoneLock);
}


extern "C" void
tzset(void)
{
	update_time_zone(true);
}


extern "C" struct tm*
localtime(const time_t* inTime)
{
	static tm tm;

	return localtime_r(inTime, &tm);
}


extern "C" struct tm*
localtime_r(const time_t* inTime, struct tm* tmOut)
{
	if (inTime == NULL) {
		__set_errno(EINVAL);
		return NULL;
	}

	update_time_zone(false);

	LocaleBackend* backend = GetCurrentLocaleBackend();

	if (backend != NULL) {
		status_t status = backend->Localtime(inTime, tmOut);

		if (status != B_OK)
			__set_errno(EOVERFLOW);

		return tmOut;
	}

	// without a locale backend, there are no timezones, so we fall back to
	// using a basic gmtime_r implementation.
	return __gmtime_r_fallback(inTime, tmOut);
}


extern "C" struct tm*
gmtime(const time_t* inTime)
{
	static tm tm;

	return gmtime_r(inTime, &tm);
}


extern "C" struct tm*
gmtime_r(const time_t* inTime, struct tm* tmOut)
{
	if (inTime == NULL) {
		__set_errno(EINVAL);
		return NULL;
	}

	// UTC needs neither a time zone nor a locale: arithmetic is enough, and
	// 50 times faster than an ICU calendar
	if (*inTime >= kFirstGregorianTime && *inTime <= kLastExactTime) {
		__secs_to_tm(*inTime, tmOut);
		tmOut->tm_isdst = 0;
		tmOut->tm_gmtoff = 0;
		tmOut->tm_zone = (char*)"GMT";
		return tmOut;
	}

	if (GetCurrentLocaleBackend() == NULL)
		LocaleBackend::LoadBackend();

	LocaleBackend* backend = GetCurrentLocaleBackend();

	if (backend != NULL) {
		status_t status = backend->Gmtime(inTime, tmOut);

		if (status != B_OK)
			__set_errno(EOVERFLOW);

		return tmOut;
	}

	// without a locale backend, we fall back to using a basic gmtime_r
	// implementation.
	return __gmtime_r_fallback(inTime, tmOut);
}


extern "C" time_t
mktime(struct tm* inTm)
{
	if (inTm == NULL) {
		__set_errno(EINVAL);
		return -1;
	}

	update_time_zone(false);

	LocaleBackend* backend = GetCurrentLocaleBackend();

	if (backend != NULL) {
		time_t timeOut;
		status_t status = backend->Mktime(inTm, timeOut);

		if (status != B_OK) {
			__set_errno(EOVERFLOW);
			return -1;
		}

		return timeOut;
	}

	// without a locale backend, we fall back to using a basic gmtime_r
	// implementation.
	return __mktime_fallback(inTm);
}


extern "C" time_t
timegm(struct tm* inTm)
{
	if (inTm == NULL) {
		__set_errno(EINVAL);
		return -1;
	}

	// the arithmetic, where it gives what the backend's calendar gives (see
	// gmtime_r())
	long long year = (long long)inTm->tm_year + 1900 + inTm->tm_mon / 12
		- (inTm->tm_mon % 12 < 0 ? 1 : 0);
	if (year >= kFirstGregorianYear) {
		long long seconds = __tm_to_secs(inTm);
		if (seconds >= kFirstGregorianTime && seconds <= kLastExactTime) {
			__secs_to_tm(seconds, inTm);
			inTm->tm_isdst = 0;
			inTm->tm_gmtoff = 0;
			inTm->tm_zone = (char*)"GMT";
			return (time_t)seconds;
		}
	}

	if (GetCurrentLocaleBackend() == NULL)
		LocaleBackend::LoadBackend();

	LocaleBackend* backend = GetCurrentLocaleBackend();

	if (backend != NULL) {
		time_t timeOut;
		status_t status = backend->Timegm(inTm, timeOut);

		if (status != B_OK) {
			__set_errno(EOVERFLOW);
			return -1;
		}

		return timeOut;
	}

	// without a locale backend, we fall back to using a basic timegm
	// implementation.
	return __timegm_fallback(inTm);
}
