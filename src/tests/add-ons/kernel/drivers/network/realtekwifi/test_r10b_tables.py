#!/usr/bin/env python3
# Copyright 2026, Haiku, Inc. All rights reserved.
# Distributed under the terms of the MIT license.
"""Host test of the RTL8710B register table file: generator and reader.

	test_r10b_tables.py [--haiku HAIKU_TOP] [--cc CC] LINUX_8710B_C

LINUX_8710B_C is drivers/net/wireless/realtek/rtl8xxxu/8710b.c of a Linux
tree. The test
- makes the table file with tools/airos/firmware/rtl8710b-tables.py, twice
  (the output must be byte for byte the same) and checks it with --check;
- builds realtekwifi's reader (dev/rtwn/rtl8710b/r10b_tables.c, unchanged)
  into r10b_tables_test.c with the host compiler;
- reads the file for both packages and compares every MAC, BB, AGC and RF
  entry the driver would write with the Linux file's arrays, parsed here
  separately from the generator;
- feeds the reader damaged files (truncated, extended, bad checksum, bad
  magic/version/chip, missing package variant, values out of range,
  tables outside the file, duplicates) and checks each is refused with
  the expected reason.
Nothing from the Linux file is kept: everything goes to a temporary
directory. Exits non-zero on the first failure.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_TOP = os.path.normpath(os.path.join(HERE, *[".."] * 7))
U, S = 0xEE, 0xFE

failures = 0


def check(cond, what):
	global failures
	print("%s: %s" % ("ok  " if cond else "FAIL", what))
	if not cond:
		failures += 1


def linux_pairs(text, name):
	"""{reg, val} pairs of a Linux array up to (not including) the end
	marker, by a parse independent of the generator's."""
	start = text.index(name)
	body = text[text.index("{", start) + 1:text.index("};", start)]
	pairs = []
	for r, v in re.findall(r"\{\s*(0x[0-9a-fA-F]+)\s*,\s*(0x[0-9a-fA-F]+)\s*\}",
			body):
		r, v = int(r, 16), int(v, 16)
		if r in (0xFF, 0xFFFF) and v in (0xFF, 0xFFFFFFFF):
			break
		pairs.append((r, v))
	return pairs


def expected(text, package):
	pkg = "u" if package == U else "s"
	return {
		"MAC": linux_pairs(text, "rtl8710b_mac_init_table[]"),
		"BB": linux_pairs(text, "rtl8710bu_qfn48m_%s_phy_init_table[]" % pkg),
		"AGC": linux_pairs(text, "rtl8710b_agc_table[]"),
		"RF": linux_pairs(text, "rtl8710bu_qfn48m_%s_radioa_init_table[]" % pkg),
	}


def run_reader(harness, data, package, tmp):
	path = os.path.join(tmp, "case.bin")
	with open(path, "wb") as f:
		f.write(data)
	p = subprocess.run([harness, path, "0x%x" % package],
		capture_output=True, text=True)
	return p.returncode, p.stdout


def parsed(stdout):
	tables = {"MAC": [], "BB": [], "AGC": [], "RF": []}
	for line in stdout.splitlines():
		kind, *rest = line.split()
		if kind in tables:
			tables[kind].append((int(rest[0], 16), int(rest[1], 16)))
	return tables


def recrc(data):
	data = bytearray(data)
	data[0x18:0x1C] = b"\0\0\0\0"
	data[0x18:0x1C] = struct.pack("<I", zlib.crc32(bytes(data)) & 0xFFFFFFFF)
	return bytes(data)


def with_size(data):
	data = bytearray(data)
	data[0x10:0x14] = struct.pack("<I", len(data))
	return recrc(data)


def directory(data):
	count = struct.unpack_from("<I", data, 0x14)[0]
	return [struct.unpack_from("<HHIII", data, 0x80 + 16 * i)
		for i in range(count)]


def drop_tables(data, keep):
	"""The file with only the directory entries for which keep() holds,
	the entries kept in place (the reader allows unreferenced bytes)."""
	entries = [e for e in directory(data) if keep(e)]
	out = bytearray(data)
	out[0x14:0x18] = struct.pack("<I", len(entries))
	for i, e in enumerate(entries):
		out[0x80 + 16 * i:0x90 + 16 * i] = struct.pack("<HHIII", *e)
	# Unused directory slots now lie between the directory and the data.
	return recrc(out)


def refused(harness, tmp, data, package, reason, what):
	rc, out = run_reader(harness, data, package, tmp)
	check(rc == 1 and reason in out, "%s refused (%s)" % (what,
		out.strip() or "rc %d" % rc))


def main():
	ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
	ap.add_argument("linux_file")
	ap.add_argument("--haiku", default=DEFAULT_TOP, help="Haiku tree")
	ap.add_argument("--cc", default=os.environ.get("CC", "cc"))
	args = ap.parse_args()

	tool = os.path.join(args.haiku, "tools/airos/firmware/rtl8710b-tables.py")
	driver = os.path.join(args.haiku, "src/add-ons/kernel/drivers/network/"
		"wlan/realtekwifi/dev/rtwn/rtl8710b")
	with open(args.linux_file, encoding="utf-8", errors="replace") as f:
		text = re.sub(r"/\*.*?\*/", "", f.read(), flags=re.S)

	with tempfile.TemporaryDirectory() as tmp:
		harness = os.path.join(tmp, "r10b_tables_test")
		subprocess.run([args.cc, "-std=c99", "-O2", "-Wall", "-Wextra",
			"-Werror", "-fsanitize=address,undefined",
			"-fno-sanitize-recover=all", "-I", driver, "-o", harness,
			os.path.join(HERE, "r10b_tables_test.c"),
			os.path.join(driver, "r10b_tables.c")], check=True)

		out1, out2 = os.path.join(tmp, "a.bin"), os.path.join(tmp, "b.bin")
		for out in (out1, out2):
			subprocess.run([sys.executable, "-I", tool, args.linux_file,
				"-o", out], check=True, capture_output=True)
		with open(out1, "rb") as f:
			good = f.read()
		with open(out2, "rb") as f:
			check(f.read() == good, "generator output is deterministic")
		p = subprocess.run([sys.executable, "-I", tool, args.linux_file,
			"--check", out1], capture_output=True, text=True)
		check(p.returncode == 0, "generator --check accepts its output")
		check(zlib.crc32(recrc(good)) == zlib.crc32(good),
			"stored CRC-32 is zlib's")

		# The tables the driver gets equal Linux's, entry for entry.
		for package, name in ((U, "QFN48M-U"), (S, "QFN48M-S")):
			rc, out = run_reader(harness, good, package, tmp)
			check(rc == 0, "reader accepts the file for %s" % name)
			got, want = parsed(out), expected(text, package)
			for kind in ("MAC", "BB", "AGC", "RF"):
				check(got[kind] == want[kind] and len(want[kind]) > 0,
					"%s %s: %d entries identical to Linux 8710b.c" % (name,
						kind, len(want[kind])))
		check(expected(text, U)["BB"] != expected(text, S)["BB"] and
			expected(text, U)["RF"] != expected(text, S)["RF"],
			"the two packages' BB and RF tables differ (variant selection "
			"is observable)")

		# Damaged files.
		refused(harness, tmp, b"", U, "shorter than its header", "empty file")
		refused(harness, tmp, good[:100], U, "shorter than its header",
			"100-byte file")
		refused(harness, tmp, good[:-1], U, "truncated", "file one byte short")
		refused(harness, tmp, good[:len(good) // 2], U, "truncated",
			"half a file")
		refused(harness, tmp, good + b"\0", U, "trailing data",
			"file with a byte appended")
		bad = bytearray(good)
		bad[-3] ^= 0x01
		refused(harness, tmp, bytes(bad), U, "checksum mismatch",
			"flipped bit in the last RF entry")
		bad = bytearray(good)
		bad[0x41] ^= 0x20
		refused(harness, tmp, bytes(bad), U, "checksum mismatch",
			"flipped bit in the source description")
		bad = bytearray(good)
		bad[0:8] = b"RTWNTABX"
		refused(harness, tmp, recrc(bad), U, "bad magic", "wrong magic")
		bad = bytearray(good)
		bad[8:10] = struct.pack("<H", 2)
		refused(harness, tmp, recrc(bad), U, "unsupported table file version",
			"format version 2")
		bad = bytearray(good)
		bad[0x0C:0x10] = struct.pack("<I", 0x8188F)
		refused(harness, tmp, recrc(bad), U, "another chip",
			"table file for another chip")

		# Package variants: a file with only the QFN48M-U tables.
		only_u = drop_tables(good, lambda e: e[1] != S)
		rc, out = run_reader(harness, only_u, U, tmp)
		check(rc == 0 and parsed(out) == expected(text, U),
			"QFN48M-U-only file still serves QFN48M-U")
		refused(harness, tmp, only_u, S, "no BB table for this chip package",
			"QFN48M-U-only file for a QFN48M-S chip")
		refused(harness, tmp, good, 0x07, "no BB table for this chip package",
			"unknown package byte 0x07")
		no_agc = drop_tables(good, lambda e: e[0] != 3)
		refused(harness, tmp, no_agc, U, "no AGC table", "file without AGC")

		# Directory and entry checks (each with a valid CRC).
		entries = directory(good)
		mac_off = entries[0][4]
		bad = bytearray(good)
		bad[mac_off + 4:mac_off + 8] = struct.pack("<I", 0x100)
		refused(harness, tmp, recrc(bad), U, "out of range",
			"MAC value above 0xff")
		bb_off = [e for e in entries if e[0] == 2][0][4]
		bad = bytearray(good)
		bad[bb_off:bb_off + 4] = struct.pack("<I", 0x802)
		refused(harness, tmp, recrc(bad), U, "out of range",
			"unaligned BB register")
		rf_off = [e for e in entries if e[0] == 4][0][4]
		bad = bytearray(good)
		bad[rf_off:rf_off + 4] = struct.pack("<I", 0x100)
		refused(harness, tmp, recrc(bad), U, "out of range",
			"RF register above 0xff")
		bad = bytearray(good)
		bad[0x80 + 12:0x80 + 16] = struct.pack("<I", len(good) - 8)
		refused(harness, tmp, recrc(bad), U, "outside the file",
			"MAC table running past the end")
		bad = bytearray(good)
		bad[0x80 + 12:0x80 + 16] = struct.pack("<I", 0x40)
		refused(harness, tmp, recrc(bad), U, "outside the file",
			"MAC table inside the header")
		bad = bytearray(good)
		bad[0x80 + 8:0x80 + 12] = struct.pack("<I", 0x10000000)
		refused(harness, tmp, recrc(bad), U, "number of table entries",
			"huge entry count")
		bad = bytearray(good)
		bad[0x14:0x18] = struct.pack("<I", 0x10000000)
		refused(harness, tmp, recrc(bad), U, "number of tables",
			"huge table count")
		bad = bytearray(good)
		bad[0x90:0x94] = struct.pack("<HH", 1, 0)	# BB/U -> MAC/any
		refused(harness, tmp, recrc(bad), U, "duplicate table",
			"two MAC tables for any package")
		bad = bytearray(good)
		bad[0x80:0x82] = struct.pack("<H", 9)
		refused(harness, tmp, recrc(bad), U, "unknown table kind",
			"table kind 9")

	print("%s" % ("all passed" if failures == 0 else "%d failed" % failures))
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
