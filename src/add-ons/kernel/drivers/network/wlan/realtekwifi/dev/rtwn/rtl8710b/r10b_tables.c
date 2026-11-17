/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

/*
 * Reader for the RTL8710B register table file (format in r10b_tables.h).
 * No libc and no kernel calls, so that the host test
 * (src/tests/add-ons/kernel/drivers/network/realtekwifi/) builds this very
 * file.
 */

#ifdef _KERNEL
#include <sys/param.h>
#include <sys/systm.h>
#endif

#include "r10b_tables.h"

static uint16_t
r10b_le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
r10b_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint32_t
r10b_tbl_crc32(uint32_t crc, const void *data, size_t len)
{
	const uint8_t *p = data;
	int bit;

	crc = ~crc;
	while (len-- > 0) {
		crc ^= *p++;
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1)));
	}
	return ~crc;
}

uint32_t
r10b_rf_delay_us(uint32_t reg)
{
	switch (reg) {
	case 0xfe:
		return 50000;
	case 0xfd:
		return 5000;
	case 0xfc:
		return 1000;
	case 0xfb:
		return 50;
	case 0xfa:
		return 5;
	case 0xf9:
		return 1;
	default:
		return 0;
	}
}

void
r10b_tbl_entry(const struct r10b_tbl_ref *ref, uint32_t index, uint32_t *reg,
    uint32_t *val)
{
	const uint8_t *p = ref->entries + (size_t)index * R10B_TBL_ENTRY_SIZE;

	*reg = r10b_le32(p);
	*val = r10b_le32(p + 4);
}

static int
r10b_tbl_entry_ok(unsigned int kind, uint32_t reg, uint32_t val)
{
	switch (kind) {
	case R10B_TBL_KIND_MAC:
		return reg < 0x800 && val <= 0xff;
	case R10B_TBL_KIND_BB:
	case R10B_TBL_KIND_AGC:
		return reg >= 0x800 && reg < 0x1000 && (reg & 3) == 0;
	case R10B_TBL_KIND_RF:
		if (reg >= R10B_RF_DELAY_MIN && reg <= R10B_RF_DELAY_MAX)
			return 1;
		return reg <= 0xff && val <= 0xfffff;
	default:
		return 0;
	}
}

#define R10B_FAIL(msg) do {	\
	*why = (msg);		\
	return -1;		\
} while (0)

int
r10b_tbl_parse(const void *data, size_t len, unsigned int variant,
    struct r10b_tbl_set *set, const char **why)
{
	const uint8_t *file = data;
	const uint8_t *dir;
	const char *magic = R10B_TBL_MAGIC;
	uint32_t count, crc, i, j, dir_end;
	/* Directory index of the exact and the "any" table of each kind. */
	int exact[R10B_TBL_KIND_COUNT], any[R10B_TBL_KIND_COUNT];
	uint8_t zero[4] = { 0, 0, 0, 0 };

	for (i = 0; i < R10B_TBL_KIND_COUNT; i++) {
		set->tables[i].entries = NULL;
		set->tables[i].count = 0;
		exact[i] = any[i] = -1;
	}
	set->source[0] = '\0';

	if (data == NULL || len < R10B_TBL_HDR_SIZE)
		R10B_FAIL("file is shorter than its header");
	for (i = 0; i < R10B_TBL_MAGIC_LEN; i++) {
		if (file[R10B_TBL_OFF_MAGIC + i] != (uint8_t)magic[i])
			R10B_FAIL("not a register table file (bad magic)");
	}
	if (r10b_le16(file + R10B_TBL_OFF_VERSION) != R10B_TBL_VERSION)
		R10B_FAIL("unsupported table file version");
	if (r10b_le16(file + R10B_TBL_OFF_HDRSIZE) != R10B_TBL_HDR_SIZE)
		R10B_FAIL("unsupported table file header size");
	if (r10b_le32(file + R10B_TBL_OFF_CHIP) != R10B_TBL_CHIP_RTL8710B)
		R10B_FAIL("table file is for another chip");
	if (r10b_le32(file + R10B_TBL_OFF_FILESIZE) != len)
		R10B_FAIL("table file is truncated or has trailing data");

	crc = r10b_tbl_crc32(0, file, R10B_TBL_OFF_CRC);
	crc = r10b_tbl_crc32(crc, zero, sizeof(zero));
	crc = r10b_tbl_crc32(crc, file + R10B_TBL_OFF_CRC + 4,
	    len - R10B_TBL_OFF_CRC - 4);
	if (crc != r10b_le32(file + R10B_TBL_OFF_CRC))
		R10B_FAIL("table file checksum mismatch");

	count = r10b_le32(file + R10B_TBL_OFF_COUNT);
	if (count == 0 || count > R10B_TBL_MAX_TABLES)
		R10B_FAIL("bad number of tables");
	dir = file + R10B_TBL_HDR_SIZE;
	dir_end = R10B_TBL_HDR_SIZE + count * R10B_TBL_DIR_SIZE;
	if (dir_end > len)
		R10B_FAIL("table directory runs past the end of the file");

	for (i = 0; i < count; i++) {
		const uint8_t *d = dir + i * R10B_TBL_DIR_SIZE;
		unsigned int kind = r10b_le16(d);
		unsigned int var = r10b_le16(d + 2);
		uint32_t n = r10b_le32(d + 8);
		uint32_t off = r10b_le32(d + 12);
		int *slot;

		if (kind < 1 || kind > R10B_TBL_KIND_COUNT)
			R10B_FAIL("unknown table kind");
		if (var != R10B_TBL_VARIANT_ANY
		    && var != R10B_TBL_VARIANT_QFN48M_U
		    && var != R10B_TBL_VARIANT_QFN48M_S)
			R10B_FAIL("unknown table variant");
		if (r10b_le32(d + 4) != 0)
			R10B_FAIL("reserved directory field is not zero");
		if (n == 0 || n > R10B_TBL_MAX_ENTRIES)
			R10B_FAIL("bad number of table entries");
		if ((off & 3) != 0 || off < dir_end || off > len
		    || (len - off) / R10B_TBL_ENTRY_SIZE < n)
			R10B_FAIL("table runs outside the file");

		slot = var == R10B_TBL_VARIANT_ANY ? &any[kind - 1]
		    : var == variant ? &exact[kind - 1] : NULL;
		/* Duplicates are refused whichever variant is asked for. */
		for (j = 0; j < i; j++) {
			const uint8_t *o = dir + j * R10B_TBL_DIR_SIZE;
			if (r10b_le16(o) == kind && r10b_le16(o + 2) == var)
				R10B_FAIL("duplicate table");
		}

		for (j = 0; j < n; j++) {
			const uint8_t *e = file + off + j * R10B_TBL_ENTRY_SIZE;
			if (!r10b_tbl_entry_ok(kind, r10b_le32(e),
			    r10b_le32(e + 4)))
				R10B_FAIL("register or value out of range");
		}

		if (slot != NULL)
			*slot = (int)i;
	}

	for (i = 0; i < R10B_TBL_KIND_COUNT; i++) {
		int index = exact[i] >= 0 ? exact[i] : any[i];
		const uint8_t *d;

		if (index < 0) {
			static const char *const missing[R10B_TBL_KIND_COUNT] = {
				"no MAC table for this chip package",
				"no BB table for this chip package",
				"no AGC table for this chip package",
				"no RF table for this chip package"
			};
			R10B_FAIL(missing[i]);
		}
		d = dir + index * R10B_TBL_DIR_SIZE;
		set->tables[i].count = r10b_le32(d + 8);
		set->tables[i].entries = file + r10b_le32(d + 12);
	}

	for (i = 0; i < R10B_TBL_SOURCE_LEN; i++) {
		uint8_t c = file[R10B_TBL_OFF_SOURCE + i];
		if (c == '\0')
			break;
		set->source[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '?';
	}
	set->source[i] = '\0';

	*why = NULL;
	return 0;
}
