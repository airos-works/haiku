/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */
#ifndef AIRTIME_TIME_FORMAT_H
#define AIRTIME_TIME_FORMAT_H


#include <String.h>
#include <SupportDefs.h>


/*!	airTime's ControlBar and Look are used here as airTime has them (from
	airos-works/airTime src/ui, a6d464f); this is the one helper of airTime's
	engine they need, as engine/Tracks.cpp has it. */
namespace airtime {

// "1:02:03", "2:03"; with fractions "1:02:03.25".
inline BString
format_time(bigtime_t time, bool withFraction = false, bool forceHours = false)
{
	bool negative = time < 0;
	if (negative)
		time = -time;
	int64 hundredths = time / 10000;
	int64 seconds = hundredths / 100;
	int hours = (int)(seconds / 3600);
	int minutes = (int)(seconds / 60 % 60);
	int secs = (int)(seconds % 60);

	BString text;
	if (hours > 0 || forceHours)
		text.SetToFormat("%s%d:%02d:%02d", negative ? "-" : "", hours, minutes,
			secs);
	else
		text.SetToFormat("%s%d:%02d", negative ? "-" : "", minutes, secs);
	if (withFraction) {
		BString fraction;
		fraction.SetToFormat(".%02d", (int)(hundredths % 100));
		text << fraction;
	}
	return text;
}

}	// namespace airtime


#endif	// AIRTIME_TIME_FORMAT_H
