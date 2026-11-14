/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	Counts the starts of an update on trial in an EFI variable (#120): the
	kernel writes its count to the boot volume only once it has mounted it,
	so a start that panics in a disk driver before that would never count,
	and the loader would start the broken update again and again.
	The variable holds "<key> <starts>", the key being the trial's token.
*/


#include <SupportDefs.h>

#include <stdio.h>
#include <string.h>

#include <boot/platform.h>

#include "efi_platform.h"


static efi_guid sAirosVendorGuid = { 0x6f2a1c47, 0x3d84, 0x4b5e,
	{ 0x9a, 0x61, 0x27, 0xc4, 0x0e, 0xb8, 0x53, 0xd2 } };
static char16_t sTrialStartsName[] = u"AirOSTrialStarts";
static bool sCounted = false;
	// the boot volume can be set more than once in one loader run


extern "C" int
platform_airos_trial_starts(const char* key, bool count)
{
	if (kRuntimeServices == NULL || key == NULL || key[0] == '\0')
		return 0;

	char buffer[160];
	size_t size = sizeof(buffer) - 1;
	uint32_t attributes = 0;
	int starts = 0;
	if (kRuntimeServices->GetVariable(sTrialStartsName, &sAirosVendorGuid,
			&attributes, &size, buffer) == EFI_SUCCESS) {
		buffer[size] = '\0';
		char* space = strrchr(buffer, ' ');
		size_t keyLength = strlen(key);
		if (space != NULL && (size_t)(space - buffer) == keyLength
			&& strncmp(buffer, key, keyLength) == 0) {
			for (const char* digit = space + 1; *digit >= '0' && *digit <= '9';
					digit++) {
				starts = starts * 10 + (*digit - '0');
			}
		}
	}

	if (!count || sCounted)
		return starts;

	sCounted = true;
	starts++;
	int length = snprintf(buffer, sizeof(buffer), "%s %d", key, starts);
	if (length <= 0 || (size_t)length >= sizeof(buffer))
		return starts;
	efi_status status = kRuntimeServices->SetVariable(sTrialStartsName,
		&sAirosVendorGuid, EFI_VARIABLE_NON_VOLATILE
			| EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS,
		length, buffer);
	if (status != EFI_SUCCESS)
		dprintf("airos boot: could not count the trial start: %#lx\n",
			(unsigned long)status);
	return starts;
}
