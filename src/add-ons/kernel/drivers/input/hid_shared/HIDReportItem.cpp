/*
 * Copyright 2009-2011, Michael Lotz, mmlr@mlotz.ch.
 * Distributed under the terms of the MIT License.
 */

#ifndef USERLAND_HID
#include "Driver.h"
#else
#include "UserlandHID.h"
#endif

#include "HIDReportItem.h"
#include "HIDReport.h"

#include <string.h>

HIDReportItem::HIDReportItem(HIDReport *report, uint32 bitOffset,
	uint8 bitLength, bool hasData, bool isArray, bool isRelative,
	uint32 minimum, uint32 maximum, uint32 usage)
	:	fReport(report),
		fByteOffset(bitOffset / 8),
		fShift(bitOffset % 8),
		fMask(bitLength >= 32 ? 0xffffffff : ~(0xffffffff << bitLength)),
			// (a shift by the width of the type is undefined, and gave a
			// mask of 0 for 32 bit fields)
		fBitCount(bitLength > 32 ? 32 : bitLength),
		fByteCount((fShift + fBitCount + 7) / 8),
		fHasData(hasData),
		fArray(isArray),
		fRelative(isRelative),
		fMinimum(minimum),
		fMaximum(maximum),
		fUsage(usage),
		fData(0),
		fValid(false)
{
}


uint16
HIDReportItem::UsagePage()
{
	usage_value value;
	value.u.extended = fUsage;
	return value.u.s.usage_page;
}


uint16
HIDReportItem::UsageID()
{
	usage_value value;
	value.u.extended = fUsage;
	return value.u.s.usage_id;
}


status_t
HIDReportItem::Extract()
{
	// An item has at most 32 bits (HIDReport::AddMainItem() makes none for
	// wider fields), but it need not start at a byte: with the shift it spans
	// up to 5 bytes. So byte-align, copy those into 64 bits and then shift
	// and mask as needed.
	uint8 *report = fReport->CurrentReport();
	if (report == NULL)
		return B_NO_INIT;

	uint64 value = 0;
	memcpy(&value, report + fByteOffset, fByteCount);
	fData = (uint32)(value >> fShift) & fMask;

	if (Signed()) {
		// sign extend if needed.
		if ((fData & ~(fMask >> 1)) != 0)
			fData |= ~fMask;

		fValid = (int32)fData >= (int32)fMinimum
			&& (int32)fData <= (int32)fMaximum;
	} else
		fValid = fData >= fMinimum && fData <= fMaximum;

	return B_OK;
}


status_t
HIDReportItem::Insert()
{
	uint8 *report = fReport->CurrentReport();
	if (report == NULL)
		return B_NO_INIT;

	// up to 5 bytes, see Extract()
	uint64 value = 0;
	memcpy(&value, report + fByteOffset, fByteCount);
	value &= ~((uint64)fMask << fShift);

	if (fValid)
		value |= (uint64)(fData & fMask) << fShift;

	memcpy(report + fByteOffset, &value, fByteCount);
	return B_OK;
}


status_t
HIDReportItem::SetData(uint32 data)
{
	fData = data;

	if (Signed()) {
		fValid = (int32)fData >= (int32)fMinimum
			&& (int32)fData <= (int32)fMaximum;
	} else
		fValid = fData >= fMinimum && fData <= fMaximum;

	return fValid ? B_OK : B_BAD_VALUE;
}


uint32
HIDReportItem::ScaledData(uint8 scaleToBits, bool toBeSigned)
{
	uint32 source;
	if (Signed() != toBeSigned) {
		if (toBeSigned)
			source = (uint32)((int32)fData - (fMaximum + 1) / 2) & fMask;
		else
			source = (uint32)((int32)fData - (int32)fMinimum);
	} else
		source = fData & fMask;

	if (fBitCount == scaleToBits)
		return source;
	if (fBitCount == 0) {
		// nothing to repeat (and the loop below would not end)
		return 0;
	}

	int8 shift;
	uint32 result = 0;
	do {
		shift = scaleToBits - fBitCount;
		if (shift > 0) {
			result |= source << shift;
			scaleToBits = shift;
		} else
			result |= source >> -shift;

	} while (shift > 0);

	return result;
}


uint32
HIDReportItem::ScaledRangeData(uint32 minimum, uint32 maximum)
{
	uint64 zeroBasedData;
	if (Signed())
		zeroBasedData = (int32)fData - (int32)fMinimum;
	else
		zeroBasedData = fData - fMinimum;

	// The logical range is the device's: one that covers all 32 bits, or
	// whose maximum is just below its minimum, has a size of 0 in 32 bits.
	uint32 range = fMaximum - fMinimum + 1;
	if (range == 0)
		return minimum;

	return zeroBasedData * (maximum - minimum + 1) / range + minimum;
}


float
HIDReportItem::ScaledFloatData()
{
	if (Signed()) {
		return (double)((int32)fData - (int32)fMinimum)
			/ (fMaximum - (int32)fMinimum);
	}

	return (double)(fData - fMinimum) / (fMaximum - fMinimum);
}


void
HIDReportItem::PrintToStream(uint32 indentLevel)
{
	char indent[indentLevel + 1];
	memset(indent, '\t', indentLevel);
	indent[indentLevel] = 0;

	TRACE_ALWAYS("%sHIDReportItem %p\n", indent, this);
	TRACE_ALWAYS("%s\tbyte offset: %" B_PRIu32 "\n", indent, fByteOffset);
	TRACE_ALWAYS("%s\tshift: %u\n", indent, fShift);
	TRACE_ALWAYS("%s\tmask: 0x%08" B_PRIx32 "\n", indent, fMask);
	TRACE_ALWAYS("%s\thas data: %s\n", indent, fHasData ? "yes" : "no");
	TRACE_ALWAYS("%s\tarray: %s\n", indent, fArray ? "yes" : "no");
	TRACE_ALWAYS("%s\trelative: %s\n", indent, fRelative ? "yes" : "no");
	TRACE_ALWAYS("%s\tminimum: %" B_PRIu32 "\n", indent, fMinimum);
	TRACE_ALWAYS("%s\tmaximum: %" B_PRIu32 "\n", indent, fMaximum);
	TRACE_ALWAYS("%s\tusage : 0x%08" B_PRIx32 "\n", indent, fUsage);

}
