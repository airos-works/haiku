#!/usr/bin/env python3
# Copyright 2026, Haiku, Inc. All rights reserved.
# Distributed under the terms of the MIT license.
"""Make realtekwifi's RTL8710B register table file from Linux's rtl8xxxu.

	rtl8710b-tables.py LINUX_8710B_C -o rtwn-rtl8710b-tables.bin
	    [--source-desc TEXT]
	rtl8710b-tables.py LINUX_8710B_C --dump
	rtl8710b-tables.py LINUX_8710B_C --check rtwn-rtl8710b-tables.bin

LINUX_8710B_C is drivers/net/wireless/realtek/rtl8xxxu/8710b.c of a Linux
tree (v6.12 to v6.18 have the same tables; sha256 577ac51f... at v6.18).

realtekwifi (BSD/MIT) has no RTL8710B register data of its own: it reads
the MAC, BB, AGC and RF initialisation tables at run time from
firmware/realtekwifi/rtwn-rtl8710b-tables.bin. This tool reads the six
tables from the Linux file and writes them in the format that
src/add-ons/kernel/drivers/network/wlan/realtekwifi/dev/rtwn/rtl8710b/
r10b_tables.h describes. The tool itself holds no register data; what it
writes is derived from the Linux file and is distributed under the
GPL-2.0-only, with that file as its source (see docs/airos/RTL8710B.md).

The output is deterministic: the same input gives the same bytes.
--dump prints the tables as text; --check compares the tables (and the
rest of the format) of an existing table file with this Linux file's and
exits non-zero if they differ; the recorded source hash and description
may differ.
"""

import argparse
import hashlib
import re
import struct
import sys
import zlib

MAGIC = b"RTWNTABL"
VERSION = 1
HDR_SIZE = 128
DIR_SIZE = 16
ENTRY_SIZE = 8
CHIP_RTL8710B = 0x8710B
SOURCE_LEN = 64

KIND_MAC, KIND_BB, KIND_AGC, KIND_RF = 1, 2, 3, 4
VARIANT_ANY, VARIANT_QFN48M_U, VARIANT_QFN48M_S = 0x00, 0xEE, 0xFE

KIND_NAMES = {KIND_MAC: "MAC", KIND_BB: "BB", KIND_AGC: "AGC", KIND_RF: "RF"}
VARIANT_NAMES = {VARIANT_ANY: "any", VARIANT_QFN48M_U: "QFN48M-U",
	VARIANT_QFN48M_S: "QFN48M-S"}

# (Linux array, kind, variant, end marker), in file order.
TABLES = [
	("rtl8710b_mac_init_table", KIND_MAC, VARIANT_ANY, (0xFFFF, 0xFF)),
	("rtl8710bu_qfn48m_u_phy_init_table", KIND_BB, VARIANT_QFN48M_U,
		(0xFFFF, 0xFFFFFFFF)),
	("rtl8710bu_qfn48m_s_phy_init_table", KIND_BB, VARIANT_QFN48M_S,
		(0xFFFF, 0xFFFFFFFF)),
	("rtl8710b_agc_table", KIND_AGC, VARIANT_ANY, (0xFFFF, 0xFFFFFFFF)),
	("rtl8710bu_qfn48m_u_radioa_init_table", KIND_RF, VARIANT_QFN48M_U,
		(0xFF, 0xFFFFFFFF)),
	("rtl8710bu_qfn48m_s_radioa_init_table", KIND_RF, VARIANT_QFN48M_S,
		(0xFF, 0xFFFFFFFF)),
]


def strip_comments(text):
	text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
	return re.sub(r"//[^\n]*", " ", text)


def read_array(text, name, end):
	"""The (register, value) pairs of `name[] = { {r, v}, ... };`,
	without the end marker, which must be the last element."""
	m = re.search(r"\b" + re.escape(name) + r"\s*\[\s*\]\s*=\s*\{", text)
	if m is None:
		sys.exit("%s: array not found" % name)
	depth, i = 1, m.end()
	while depth and i < len(text):
		if text[i] == "{":
			depth += 1
		elif text[i] == "}":
			depth -= 1
		i += 1
	if depth:
		sys.exit("%s: unterminated array" % name)
	body = text[m.end():i - 1]
	pairs = [(int(r, 16), int(v, 16)) for r, v in re.findall(
		r"\{\s*(0[xX][0-9a-fA-F]+)\s*,\s*(0[xX][0-9a-fA-F]+)\s*\}", body)]
	# Every element must have matched: no expressions or macros.
	if len(re.findall(r"\{", body)) != len(pairs):
		sys.exit("%s: element that is not {hex, hex}" % name)
	if not pairs or pairs[-1] != end:
		sys.exit("%s: does not end with the end marker" % name)
	pairs = pairs[:-1]
	if end in pairs:
		sys.exit("%s: end marker before the last element" % name)
	return pairs


def entry_ok(kind, reg, val):
	if kind == KIND_MAC:
		return reg < 0x800 and val <= 0xFF
	if kind in (KIND_BB, KIND_AGC):
		return 0x800 <= reg < 0x1000 and reg % 4 == 0 and val <= 0xFFFFFFFF
	if 0xF9 <= reg <= 0xFE:
		return True
	return reg <= 0xFF and val <= 0xFFFFF


def read_tables(path):
	with open(path, "rb") as f:
		raw = f.read()
	text = strip_comments(raw.decode("utf-8", errors="replace"))
	tables = []
	for name, kind, variant, end in TABLES:
		pairs = read_array(text, name, end)
		for reg, val in pairs:
			if not entry_ok(kind, reg, val):
				sys.exit("%s: {0x%x, 0x%x} out of range for a %s table"
					% (name, reg, val, KIND_NAMES[kind]))
		tables.append((name, kind, variant, pairs))
	return raw, tables


def build(raw, tables, source_desc, source_sha256=None):
	desc = source_desc.encode("ascii")
	if len(desc) > SOURCE_LEN:
		sys.exit("source description longer than %d bytes" % SOURCE_LEN)
	offset = HDR_SIZE + DIR_SIZE * len(tables)
	directory, data = b"", b""
	for _name, kind, variant, pairs in tables:
		directory += struct.pack("<HHIII", kind, variant, 0, len(pairs),
			offset + len(data))
		for reg, val in pairs:
			data += struct.pack("<II", reg, val)
	size = offset + len(data)
	header = struct.pack("<8sHHIIIII", MAGIC, VERSION, HDR_SIZE,
		CHIP_RTL8710B, size, len(tables), 0, 0)
	header += source_sha256 or hashlib.sha256(raw).digest()
	header += desc.ljust(SOURCE_LEN, b"\0")
	assert len(header) == HDR_SIZE
	blob = header + directory + data
	crc = zlib.crc32(blob) & 0xFFFFFFFF
	return blob[:0x18] + struct.pack("<I", crc) + blob[0x1C:]


def main():
	ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
	ap.add_argument("linux_file", help="rtl8xxxu/8710b.c of a Linux tree")
	ap.add_argument("-o", "--output", help="table file to write")
	ap.add_argument("--source-desc", default="GPL-2.0-only; Linux rtl8xxxu/8710b.c",
		help="text stored in the header (at most 64 ASCII characters)")
	ap.add_argument("--dump", action="store_true", help="print the tables")
	ap.add_argument("--check", metavar="FILE",
		help="compare FILE's tables with the Linux file's")
	args = ap.parse_args()

	raw, tables = read_tables(args.linux_file)

	if args.dump:
		for name, kind, variant, pairs in tables:
			print("# %s: %s %s, %d entries" % (name, KIND_NAMES[kind],
				VARIANT_NAMES[variant], len(pairs)))
			for reg, val in pairs:
				print("%s %s 0x%03x 0x%08x" % (KIND_NAMES[kind],
					VARIANT_NAMES[variant], reg, val))

	if args.check:
		with open(args.check, "rb") as f:
			existing = f.read()
		# Same tables, whatever the description and the recorded source
		# hash say (a newer Linux file with the same tables matches).
		desc = existing[0x40:0x80].rstrip(b"\0").decode("ascii", "replace")
		if existing != build(raw, tables, desc, existing[0x20:0x40]):
			sys.exit("%s does not match %s" % (args.check, args.linux_file))
		same = existing[0x20:0x40] == hashlib.sha256(raw).digest()
		print("%s matches %s (%s source file)" % (args.check,
			args.linux_file, "same" if same else "a different but equivalent"))

	if args.output:
		blob = build(raw, tables, args.source_desc)
		with open(args.output, "wb") as f:
			f.write(blob)
		print("%s: %d bytes, %s, source sha256 %s" % (args.output, len(blob),
			", ".join("%s/%s %d" % (KIND_NAMES[k], VARIANT_NAMES[v], len(p))
				for _n, k, v, p in tables),
			hashlib.sha256(raw).hexdigest()), file=sys.stderr)

	if not (args.dump or args.check or args.output):
		ap.error("nothing to do: give -o, --dump or --check")


if __name__ == "__main__":
	main()
