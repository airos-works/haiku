/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*
 * Host harness for realtekwifi's RTL8710B table file reader
 * (dev/rtwn/rtl8710b/r10b_tables.c, built as is):
 *
 *	r10b_tables_test FILE PACKAGE
 *
 * PACKAGE is the eFuse package byte (0xee, 0xfe, ...). Prints the tables
 * the driver would use, one "KIND 0xREG 0xVALUE" line per entry, and
 * exits 0; or prints "FAIL: reason" and exits 1. Driven by
 * test_r10b_tables.py.
 */

#include <stdio.h>
#include <stdlib.h>

#include "r10b_tables.h"

int
main(int argc, char **argv)
{
	static const char *const kinds[R10B_TBL_KIND_COUNT] = {
		"MAC", "BB", "AGC", "RF"
	};
	struct r10b_tbl_set set;
	const char *why;
	unsigned char *data;
	size_t len, cap = 1 << 20;
	unsigned long package;
	uint32_t i, reg, val;
	FILE *f;
	int k;

	if (argc != 3) {
		fprintf(stderr, "usage: %s FILE PACKAGE\n", argv[0]);
		return 2;
	}
	package = strtoul(argv[2], NULL, 0);

	f = fopen(argv[1], "rb");
	if (f == NULL) {
		perror(argv[1]);
		return 2;
	}
	data = malloc(cap);
	len = fread(data, 1, cap, f);
	fclose(f);

	/* Exactly the bytes, no slack: a reader past the end would show. */
	data = realloc(data, len > 0 ? len : 1);

	if (r10b_tbl_parse(data, len, (unsigned int)package, &set, &why) != 0) {
		printf("FAIL: %s\n", why);
		free(data);
		return 1;
	}

	printf("SOURCE %s\n", set.source);
	for (k = 0; k < R10B_TBL_KIND_COUNT; k++) {
		for (i = 0; i < set.tables[k].count; i++) {
			r10b_tbl_entry(&set.tables[k], i, &reg, &val);
			printf("%s 0x%03x 0x%08x\n", kinds[k], reg, val);
		}
	}
	free(data);
	return 0;
}
