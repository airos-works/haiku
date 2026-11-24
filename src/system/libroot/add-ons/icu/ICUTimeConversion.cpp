/*
 * Copyright 2010-2011, Oliver Tappe, zooey@hirschkaefer.de.
 * Distributed under the terms of the MIT License.
 */


#include "ICUTimeConversion.h"

#include <math.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>

#include <locks.h>

#include <unicode/gregocal.h>


U_NAMESPACE_USE


namespace BPrivate {
namespace Libroot {


// Guards the time zone and the calendar of every ICUTimeConversion: TZSet()
// replaced the zone while other threads made calendars of it.
static mutex sLock = MUTEX_INITIALIZER("ICU time conversion");
static pthread_once_t sForkHookOnce = PTHREAD_ONCE_INIT;


static void
reinit_lock_after_fork()
{
	mutex_init(&sLock, "ICU time conversion");
}


static void
register_fork_hook()
{
	pthread_atfork(NULL, NULL, &reinit_lock_after_fork);
}


struct Locker {
	Locker()	{ mutex_lock(&sLock); }
	~Locker()	{ mutex_unlock(&sLock); }
};


ICUTimeConversion::ICUTimeConversion(const ICUTimeData& timeData)
	:
	fTimeData(timeData),
	fDataBridge(NULL),
	fTimeZone(NULL),
	fCalendar(NULL),
	fTZSetDone(false),
	fLastTZWasSet(false),
	fLastTZSetStatus(B_NO_INIT)
{
	fTimeZoneID[0] = '\0';
	fLastTimeZoneID[0] = '\0';
	fLastTZ[0] = '\0';

	pthread_once(&sForkHookOnce, &register_fork_hook);
}


ICUTimeConversion::~ICUTimeConversion()
{
	delete fCalendar;
	delete fTimeZone;
}


void
ICUTimeConversion::Initialize(TimeConversionDataBridge* dataBridge)
{
	fDataBridge = dataBridge;
}


status_t
ICUTimeConversion::TZSet(const char* timeZoneID, const char* tz)
{
	Locker locker;

	// Called before every conversion: nothing to do when it gets what it got
	// the last time. (_TZSet() would also notice, except for a TZ variable
	// ICU does not know, which made a new zone every time.)
	if (fTZSetDone && (tz != NULL) == fLastTZWasSet
		&& strcmp(fLastTimeZoneID, timeZoneID) == 0
		&& (tz == NULL || strcmp(fLastTZ, tz) == 0)) {
		return fLastTZSetStatus;
	}

	status_t status = _TZSet(timeZoneID, tz);

	fTZSetDone = strlcpy(fLastTimeZoneID, timeZoneID,
			sizeof(fLastTimeZoneID)) < sizeof(fLastTimeZoneID)
		&& (tz == NULL || strlcpy(fLastTZ, tz, sizeof(fLastTZ))
			< sizeof(fLastTZ));
	fLastTZWasSet = tz != NULL;
	fLastTZSetStatus = status;
	return status;
}


status_t
ICUTimeConversion::_TZSet(const char* timeZoneID, const char* tz)
{
	bool offsetHasBeenSet = false;
	bool timeZoneIdMatches = false;

	// The given TZ environment variable's content overrides the default
	// system timezone.
	if (tz != NULL) {
		// If the value given in the TZ env-var starts with a colon, that
		// value is implementation specific, we expect a full timezone ID.
		if (*tz == ':') {
			// nothing to do if the given name matches the current timezone
			if (strcasecmp(fTimeZoneID, tz + 1) == 0) {
				timeZoneIdMatches = true;
				goto done;
			}

			strlcpy(fTimeZoneID, tz + 1, sizeof(fTimeZoneID));
		} else {
			// note timezone name
			strlcpy(fTimeZoneID, tz, sizeof(fTimeZoneID));

			// nothing to do if the given name matches the current timezone
			if (strcasecmp(fTimeZoneID, fDataBridge->addrOfTZName[0]) == 0) {
				timeZoneIdMatches = true;
				goto done;
			}

			// parse TZ variable (only <std> and <offset> supported)
			const char* tzNameEnd = tz;
			while(isalpha(*tzNameEnd))
				++tzNameEnd;
			if (*tzNameEnd == '-' || *tzNameEnd == '+') {
				int hours = 0;
				int minutes = 0;
				int seconds = 0;
				sscanf(tzNameEnd + 1, "%2d:%2d:%2d", &hours, &minutes,
					&seconds);
				hours = min_c(24, max_c(0, hours));
				minutes = min_c(59, max_c(0, minutes));
				seconds = min_c(59, max_c(0, seconds));

				*fDataBridge->addrOfTimezone = (*tzNameEnd == '-' ? -1 : 1)
					* (hours * 3600 + minutes * 60 + seconds);
				offsetHasBeenSet = true;
			}
		}
	} else {
		// nothing to do if the given name matches the current timezone
		if (strcasecmp(fTimeZoneID, timeZoneID) == 0) {
			timeZoneIdMatches = true;
			goto done;
		}

		strlcpy(fTimeZoneID, timeZoneID, sizeof(fTimeZoneID));
	}

done:
	// fTimeZone can still be NULL if we don't initialize it
	// in the first TZSet, causing problems for future
	// Localtime invocations.
	if (fTimeZone != NULL && timeZoneIdMatches)
		return B_OK;

	delete fCalendar;
	fCalendar = NULL;
	delete fTimeZone;
	fTimeZone = TimeZone::createTimeZone(fTimeZoneID);
	if (fTimeZone == NULL)
		return B_NO_MEMORY;

	UnicodeString temp;
	if (fTimeZone->getID(temp) == UCAL_UNKNOWN_ZONE_ID)
		goto error;

	if (offsetHasBeenSet) {
		fTimeZone->setRawOffset(*fDataBridge->addrOfTimezone * -1 * 1000);
	} else {
		int32_t rawOffset;
		int32_t dstOffset;
		UDate nowMillis = 1000 * (UDate)time(NULL);
		UErrorCode icuStatus = U_ZERO_ERROR;
		fTimeZone->getOffset(nowMillis, FALSE, rawOffset, dstOffset, icuStatus);
		if (!U_SUCCESS(icuStatus))
			goto error;
		*fDataBridge->addrOfTimezone = -1 * (rawOffset + dstOffset) / 1000;
			// we want seconds, not the ms that ICU gives us
	}

	*fDataBridge->addrOfDaylight = fTimeZone->useDaylightTime();

	for (int i = 0; i < 2; ++i) {
		if (tz != NULL && *tz != ':' && i == 0) {
			strlcpy(fDataBridge->addrOfTZName[0], fTimeZoneID,
				fDataBridge->kTZNameLength);
		} else {
			UnicodeString icuString;
			fTimeZone->getDisplayName(i == 1, TimeZone::SHORT,
				fTimeData.ICULocaleForStrings(), icuString);
			CheckedArrayByteSink byteSink(fDataBridge->addrOfTZName[i],
				fDataBridge->kTZNameLength);
			icuString.toUTF8(byteSink);

			if (byteSink.Overflowed())
				goto error;

			// make sure to canonicalize "GMT+00:00" to just "GMT"
			if (strcmp(fDataBridge->addrOfTZName[i], "GMT+00:00") == 0)
				fDataBridge->addrOfTZName[i][3] = '\0';
		}
	}

	return B_OK;

error:
	*fDataBridge->addrOfTimezone = 0;
	*fDataBridge->addrOfDaylight = false;
	strcpy(fDataBridge->addrOfTZName[0], "GMT");
	strcpy(fDataBridge->addrOfTZName[1], "GMT");

	return B_ERROR;
}


status_t
ICUTimeConversion::Localtime(const time_t* inTime, struct tm* tmOut)
{
	Locker locker;

	GregorianCalendar* calendar;
	status_t status = _LocalCalendar(calendar);
	if (status != B_OK)
		return status;

	tmOut->tm_zone = fTimeZoneID;
	return _FillTmValues(*calendar, inTime, tmOut);
}


status_t
ICUTimeConversion::Gmtime(const time_t* inTime, struct tm* tmOut)
{
	// (libroot does the arithmetic itself: this is only asked for times
	// before the Gregorian calendar, or far in the future)
	UErrorCode icuStatus = U_ZERO_ERROR;
	GregorianCalendar calendar(*TimeZone::getGMT(), fTimeData.ICULocale(),
		icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;

	status_t status = _FillTmValues(calendar, inTime, tmOut);
	if (status == B_OK) {
		// tm_zone must be "GMT" for gmtime, not the current timezone
		// (even if that happens to be equivalent to GMT).
		tmOut->tm_zone = (char*)"GMT";
	}
	return status;
}


status_t
ICUTimeConversion::Mktime(struct tm* inOutTm, time_t& timeOut)
{
	Locker locker;

	GregorianCalendar* calendar;
	status_t status = _LocalCalendar(calendar);
	if (status != B_OK)
		return status;

	return _Mktime(*calendar, inOutTm, timeOut);
}


status_t
ICUTimeConversion::Timegm(struct tm* inOutTm, time_t& timeOut)
{
	UErrorCode icuStatus = U_ZERO_ERROR;
	GregorianCalendar calendar(*TimeZone::getGMT(), fTimeData.ICULocale(),
		icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;

	return _Mktime(calendar, inOutTm, timeOut);
}


/*!	Returns the calendar of the current time zone, made when it is first
	needed. The lock must be held.
*/
status_t
ICUTimeConversion::_LocalCalendar(GregorianCalendar*& _calendar)
{
	if (fTimeZone == NULL)
		return B_NO_INIT;

	if (fCalendar == NULL) {
		// (the locale only sets the week's first day, which none of the
		// fields asked for depends on)
		UErrorCode icuStatus = U_ZERO_ERROR;
		fCalendar = new GregorianCalendar(*fTimeZone,
			fTimeData.ICULocale(), icuStatus);
		if (fCalendar == NULL)
			return B_NO_MEMORY;
		if (!U_SUCCESS(icuStatus)) {
			delete fCalendar;
			fCalendar = NULL;
			return B_ERROR;
		}
		fCalendar->setLenient(TRUE);
	}

	_calendar = fCalendar;
	return B_OK;
}


status_t
ICUTimeConversion::_FillTmValues(Calendar& calendar, const time_t* inTime,
	struct tm* tmOut)
{
	UErrorCode icuStatus = U_ZERO_ERROR;
	calendar.setTime(1000 * (UDate)*inTime, icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;

	tmOut->tm_sec = calendar.get(UCAL_SECOND, icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	tmOut->tm_min = calendar.get(UCAL_MINUTE, icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	tmOut->tm_hour = calendar.get(UCAL_HOUR_OF_DAY, icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	tmOut->tm_mday = calendar.get(UCAL_DAY_OF_MONTH, icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	tmOut->tm_mon = calendar.get(UCAL_MONTH, icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	tmOut->tm_year = calendar.get(UCAL_YEAR, icuStatus) - 1900;
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	tmOut->tm_wday = calendar.get(UCAL_DAY_OF_WEEK, icuStatus) - 1;
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	tmOut->tm_yday = calendar.get(UCAL_DAY_OF_YEAR, icuStatus) - 1;
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	tmOut->tm_isdst = calendar.inDaylightTime(icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	tmOut->tm_gmtoff = (calendar.get(UCAL_ZONE_OFFSET, icuStatus)
		+ calendar.get(UCAL_DST_OFFSET, icuStatus)) / 1000;
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;

	tmOut->tm_zone = fDataBridge->addrOfTZName[tmOut->tm_isdst ? 1 : 0];
	return B_OK;
}


status_t
ICUTimeConversion::_Mktime(Calendar& calendar, struct tm* inOutTm,
	time_t& timeOut)
{
	UErrorCode icuStatus = U_ZERO_ERROR;
	calendar.clear();
		// The milliseconds stay 0. A new calendar had those of the current
		// time, which made the result of a time before 1970 one second too
		// late (999 times in 1000), as the division below cuts towards 0.
	calendar.setLenient(TRUE);
	calendar.set(inOutTm->tm_year + 1900, inOutTm->tm_mon, inOutTm->tm_mday,
		inOutTm->tm_hour, inOutTm->tm_min, inOutTm->tm_sec);

	UDate timeInMillis = calendar.getTime(icuStatus);
	if (!U_SUCCESS(icuStatus))
		return B_ERROR;
	timeOut = (time_t)((int64_t)timeInMillis / 1000);

	return _FillTmValues(calendar, &timeOut, inOutTm);
}


}	// namespace Libroot
}	// namespace BPrivate
