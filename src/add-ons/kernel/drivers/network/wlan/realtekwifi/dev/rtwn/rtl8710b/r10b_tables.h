/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT license.
 */

#ifndef R10B_TABLES_H
#define R10B_TABLES_H

/*
 * RTL8710B register table file ("rtwn table file", format version 1).
 *
 * The RTL8710B's MAC, baseband (BB), AGC and RF initialisation values are
 * not part of this driver: Haiku only has them from Linux's rtl8xxxu
 * (GPL-2.0-only), and realtekwifi stays under BSD/MIT licences. They come
 * at run time from a data file next to the firmware,
 * firmware/realtekwifi/rtwn-rtl8710b-tables.bin, which
 * tools/airos/firmware/rtl8710b-tables.py makes from Linux's 8710b.c and
 * which is distributed under the GPL-2.0 like its source. This header and
 * r10b_tables.c only describe and read the format; they hold no register
 * data.
 *
 * Everything is little-endian. Offsets are from the start of the file.
 *
 *	header (128 bytes)
 *	0x00	char[8]	magic "RTWNTABL"
 *	0x08	u16	format version, 1
 *	0x0a	u16	header size, 128
 *	0x0c	u32	chip id, 0x8710b
 *	0x10	u32	file size in bytes (the whole file)
 *	0x14	u32	number of tables (1..R10B_TBL_MAX_TABLES)
 *	0x18	u32	CRC-32 of the whole file, computed with these four
 *			bytes as zero (IEEE 802.3 CRC-32: reflected polynomial
 *			0xedb88320, initial value and final XOR 0xffffffff;
 *			the same as zlib's crc32())
 *	0x1c	u32	flags, 0
 *	0x20	u8[32]	SHA-256 of the source file the tables came from
 *			(informational: lets a log line name the source)
 *	0x40	char[64] source description, ASCII, NUL padded (for example
 *			"GPL-2.0-only; Linux v6.18 rtl8xxxu/8710b.c")
 *
 *	table directory (16 bytes per table, right after the header)
 *	+0	u16	kind: 1 MAC, 2 BB, 3 AGC, 4 RF (path A)
 *	+2	u16	variant: 0 any package, 0xee QFN48M-U, 0xfe QFN48M-S
 *			(the package type byte of the physical eFuse, offset
 *			0xf8, as the vendor driver reads it)
 *	+4	u32	reserved, 0
 *	+8	u32	number of entries (1..R10B_TBL_MAX_ENTRIES)
 *	+12	u32	offset of the first entry (4-byte aligned, after the
 *			directory)
 *
 *	entries (8 bytes each)
 *	+0	u32	register
 *	+4	u32	value
 *
 * The entries are written in file order, without the end markers of the
 * Linux arrays:
 * - MAC: 8-bit writes, register < 0x800, value <= 0xff;
 * - BB and AGC: 32-bit writes, 0x800 <= register < 0x1000, register
 *   4-byte aligned;
 * - RF: writes to RF path A, register <= 0xff, value <= 0xfffff.
 *   Registers 0xf9..0xfe are not written: they are the vendor's delay
 *   codes (0xfe 50 ms, 0xfd 5 ms, 0xfc 1 ms, 0xfb 50 us, 0xfa 5 us,
 *   0xf9 1 us), and the value is ignored.
 *
 * A (kind, variant) pair appears at most once. For a chip with package
 * variant V the driver takes, for each kind, the table for V if there is
 * one and the one for "any" otherwise; all four kinds must resolve.
 * Version 1 readers refuse other versions, header sizes and chip ids.
 */

#ifdef _KERNEL
#include <sys/types.h>
#else
#include <stddef.h>
#include <stdint.h>
#endif

#define R10B_TBL_MAGIC		"RTWNTABL"
#define R10B_TBL_MAGIC_LEN	8
#define R10B_TBL_VERSION	1
#define R10B_TBL_HDR_SIZE	128
#define R10B_TBL_DIR_SIZE	16
#define R10B_TBL_ENTRY_SIZE	8
#define R10B_TBL_CHIP_RTL8710B	0x8710b
#define R10B_TBL_MAX_TABLES	16
#define R10B_TBL_MAX_ENTRIES	1024
#define R10B_TBL_SOURCE_LEN	64

/* Header field offsets. */
#define R10B_TBL_OFF_MAGIC	0x00
#define R10B_TBL_OFF_VERSION	0x08
#define R10B_TBL_OFF_HDRSIZE	0x0a
#define R10B_TBL_OFF_CHIP	0x0c
#define R10B_TBL_OFF_FILESIZE	0x10
#define R10B_TBL_OFF_COUNT	0x14
#define R10B_TBL_OFF_CRC	0x18
#define R10B_TBL_OFF_FLAGS	0x1c
#define R10B_TBL_OFF_SHA256	0x20
#define R10B_TBL_OFF_SOURCE	0x40

/* Table kinds. */
#define R10B_TBL_KIND_MAC	1
#define R10B_TBL_KIND_BB	2
#define R10B_TBL_KIND_AGC	3
#define R10B_TBL_KIND_RF	4
#define R10B_TBL_KIND_COUNT	4

/* Package variants (the eFuse package type byte). */
#define R10B_TBL_VARIANT_ANY		0x00
#define R10B_TBL_VARIANT_QFN48M_U	0xee
#define R10B_TBL_VARIANT_QFN48M_S	0xfe

/* RF delay codes. */
#define R10B_RF_DELAY_MIN	0xf9
#define R10B_RF_DELAY_MAX	0xfe

struct r10b_tbl_ref {
	const uint8_t	*entries;	/* R10B_TBL_ENTRY_SIZE each, LE */
	uint32_t	count;
};

struct r10b_tbl_set {
	/* Indexed by kind - 1. */
	struct r10b_tbl_ref	tables[R10B_TBL_KIND_COUNT];
	/* NUL-terminated copy of the header's source description. */
	char			source[R10B_TBL_SOURCE_LEN + 1];
};

/*
 * Checks the whole file and finds the tables for package `variant`.
 * Returns 0, or -1 with *why pointing to a static description of the
 * first problem found. On success the references in `set` point into
 * `data`, which must stay valid while they are used.
 */
int	r10b_tbl_parse(const void *data, size_t len, unsigned int variant,
	    struct r10b_tbl_set *set, const char **why);

/* Entry `index` of a table (index < ref->count). */
void	r10b_tbl_entry(const struct r10b_tbl_ref *ref, uint32_t index,
	    uint32_t *reg, uint32_t *val);

/* zlib-compatible CRC-32 over a buffer, continuing from `crc`. */
uint32_t r10b_tbl_crc32(uint32_t crc, const void *data, size_t len);

/* Microseconds an RF delay code stands for, or 0 for a register. */
uint32_t r10b_rf_delay_us(uint32_t reg);

#endif	/* R10B_TABLES_H */
