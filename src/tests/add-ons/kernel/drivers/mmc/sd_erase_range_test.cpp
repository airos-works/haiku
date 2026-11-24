/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


/*!	The arguments mmc_disk sends for a trim (airos-works/haiku#172): the end
	of the SD erase sequence is the last block of the range. It used to be
	the block after it, so every trimmed range took the first sector of what
	follows it along. Runs on the build host, see run_host.sh.
*/


#include <stdio.h>

#include "sd_erase_range.h"


static int sFailures;


static void
check(const char* what, uint64_t offset, uint64_t length, bool blockAddressed,
	bool expectedResult, uint32_t expectedFirst, uint32_t expectedLast)
{
	uint32_t first = 0;
	uint32_t last = 0;
	bool result = sd_erase_range(offset, length, 512, blockAddressed, &first,
		&last);
	bool ok = result == expectedResult
		&& (!result || (first == expectedFirst && last == expectedLast));
	if (result) {
		printf("%s %s: bytes %llu + %llu -> CMD32 %u, CMD33 %u", ok ? "ok  "
			: "FAIL", what, (unsigned long long)offset,
			(unsigned long long)length, first, last);
	} else {
		printf("%s %s: bytes %llu + %llu -> refused", ok ? "ok  " : "FAIL",
			what, (unsigned long long)offset, (unsigned long long)length);
	}
	if (!ok && expectedResult)
		printf(" (expected %u, %u)", expectedFirst, expectedLast);
	else if (!ok)
		printf(" (expected a refusal)");
	printf("\n");
	if (!ok)
		sFailures++;
}


int
main()
{
	// sectors 100..107 of a card addressed in blocks: the last one is 107
	check("SDHC, 8 sectors", 100 * 512, 8 * 512, true, true, 100, 107);
	check("SDHC, 1 sector", 100 * 512, 512, true, true, 100, 100);
	check("SDHC, first sector", 0, 512, true, true, 0, 0);
	check("SDHC, 2 TiB card's last sector", 0xffffffffULL * 512, 512, true,
		true, 0xffffffffU, 0xffffffffU);
	check("SDHC, past 32 bits", 0xffffffffULL * 512, 1024, true, false, 0, 0);

	// a card addressed in bytes takes the address its last block starts at
	check("SDSC, 8 sectors", 100 * 512, 8 * 512, false, true, 51200, 54784);
	check("SDSC, 1 sector", 100 * 512, 512, false, true, 51200, 51200);
	check("SDSC, up to 4 GiB", 0xfffffe00ULL - 512, 1024, false, true,
		0xfffffc00U, 0xfffffe00U);
	check("SDSC, past 4 GiB", 0xfffffe00ULL, 1024, false, false, 0, 0);

	check("nothing", 4096, 0, true, false, 0, 0);
	check("not whole sectors", 4096, 1000, true, false, 0, 0);
	check("not at a sector", 100, 512, true, false, 0, 0);
	check("wrapping", 0xfffffffffffffe00ULL, 1024, true, false, 0, 0);

	printf(sFailures == 0 ? "PASS\n" : "FAILED\n");
	return sFailures == 0 ? 0 : 1;
}
