/*
 * Copyright 2010-2011, Oliver Tappe, zooey@hirschkaefer.de.
 * Distributed under the terms of the MIT License.
 */
#ifndef _ICU_TIME_CONVERSION_H
#define _ICU_TIME_CONVERSION_H


#include <time.h>

#include <StorageDefs.h>

#include <unicode/gregocal.h>

#include "ICUTimeData.h"
#include "LocaleBackend.h"


namespace BPrivate {
namespace Libroot {


class ICUTimeConversion {
public:
								ICUTimeConversion(const ICUTimeData& timeData);
	virtual						~ICUTimeConversion();

	virtual void				Initialize(
									TimeConversionDataBridge* dataBridge);

			status_t			TZSet(const char* timeZoneID, const char* tz);

			status_t			Localtime(const time_t* inTime,
									struct tm* tmOut);
			status_t			Gmtime(const time_t* inTime, struct tm* tmOut);

			status_t			Mktime(struct tm* inOutTm, time_t& timeOut);

			status_t			Timegm(struct tm* inOutTm, time_t& timeOut);

private:
			status_t			_TZSet(const char* timeZoneID, const char* tz);

			status_t			_LocalCalendar(
									U_NAMESPACE_QUALIFIER GregorianCalendar*&
										_calendar);

			status_t			_FillTmValues(
									U_NAMESPACE_QUALIFIER Calendar& calendar,
									const time_t* inTime, struct tm* tmOut);

			status_t			_Mktime(
									U_NAMESPACE_QUALIFIER Calendar& calendar,
									struct tm* inOutTm, time_t& timeOut);

			const ICUTimeData&	fTimeData;

			TimeConversionDataBridge*	fDataBridge;

			U_NAMESPACE_QUALIFIER TimeZone*	fTimeZone;
			char				fTimeZoneID[B_FILE_NAME_LENGTH];

			// A calendar of fTimeZone, kept: one was made for every
			// conversion, which cost more than the conversion itself.
			U_NAMESPACE_QUALIFIER GregorianCalendar*	fCalendar;

			// what the last TZSet() was given, and what came of it
			bool				fTZSetDone;
			bool				fLastTZWasSet;
			status_t			fLastTZSetStatus;
			char				fLastTimeZoneID[B_FILE_NAME_LENGTH];
			char				fLastTZ[B_FILE_NAME_LENGTH];
};


}	// namespace Libroot
}	// namespace BPrivate


#endif	// _ICU_TIME_BACKEND_H
