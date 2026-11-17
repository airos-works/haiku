#!/usr/bin/env python3
# Copyright 2026, Haiku, Inc. All rights reserved.
# Distributed under the terms of the MIT license.
"""Cross-check the RTL8723B initialisation tables of realtekwifi.

realtekwifi's RTL8723B support comes from FreeBSD's rtwn (BSD-2-Clause,
dev/rtwn/rtl8723b/r23b_priv.h). This script compares its MAC, BB, AGC and
RF tables with the ones in a Linux kernel tree's rtl8xxxu driver
(drivers/net/wireless/realtek/rtl8xxxu/8723b.c), which carries the same
Realtek register data independently. It only reads the Linux file to
compare values; nothing from it ends up in Haiku's tree.

	check_r23b_tables.py [--haiku HAIKU_TOP] LINUX_TOP

It exits non-zero when a table differs (each difference is listed).
"""

import argparse
import os
import re
import sys

HEX = r"0x[0-9a-fA-F]+"


def read(path):
	with open(path, encoding="utf-8", errors="replace") as f:
		return f.read()


def strip_comments(text):
	text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
	return re.sub(r"//[^\n]*", "", text)


def c_array(text, name):
	"""The body of `name[] = { ... };` (outer braces removed)."""
	m = re.search(r"\b" + re.escape(name) + r"\s*\[\s*\]\s*=\s*\{", text)
	if m is None:
		sys.exit("cannot find array %s" % name)
	depth, i = 1, m.end()
	while depth:
		depth += {"{": 1, "}": -1}.get(text[i], 0)
		i += 1
	return text[m.end():i - 1]


def numbers(body):
	return [int(v, 16) for v in re.findall(HEX, body)]


def pairs(body):
	return [(int(a, 16), int(b, 16)) for a, b in
		re.findall(r"\{\s*(" + HEX + r")\s*,\s*(" + HEX + r")\s*\}", body)]


def linux_pairs(text, name):
	# rtl8xxxu tables end with a 0xffff/0xff terminator entry.
	return [(r, v) for r, v in pairs(c_array(text, name))
		if r not in (0xff, 0xffff)]


def compare(label, ours, theirs):
	if ours == theirs:
		print("%-4s %4d entries, identical" % (label, len(ours)))
		return True
	print("%-4s DIFFERS: realtekwifi %d entries, rtl8xxxu %d"
		% (label, len(ours), len(theirs)))
	ours_set, theirs_set = set(ours), set(theirs)
	for entry in ours:
		if entry not in theirs_set:
			print("     only in realtekwifi: %s" % (tuple(map(hex, entry)),))
	for entry in theirs:
		if entry not in ours_set:
			print("     only in rtl8xxxu:    %s" % (tuple(map(hex, entry)),))
	if ours_set == theirs_set:
		print("     (same entries, different order)")
	return False


def main():
	parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
	here = os.path.dirname(os.path.abspath(__file__))
	parser.add_argument("--haiku",
		default=os.path.normpath(os.path.join(here, *[".."] * 7)))
	parser.add_argument("linux")
	args = parser.parse_args()

	ours = strip_comments(read(os.path.join(args.haiku,
		"src/add-ons/kernel/drivers/network/wlan/realtekwifi/dev/rtwn"
		"/rtl8723b/r23b_priv.h")))
	theirs = strip_comments(read(os.path.join(args.linux,
		"drivers/net/wireless/realtek/rtl8xxxu/8723b.c")))

	ok = True
	ok &= compare("MAC", pairs(c_array(ours, "rtl8723b_mac")),
		linux_pairs(theirs, "rtl8723b_mac_init_table"))

	bb = list(zip(numbers(c_array(ours, "rtl8723b_bb_regs")),
		numbers(c_array(ours, "rtl8723b_bb_vals"))))
	ok &= compare("BB", bb, linux_pairs(theirs, "rtl8723b_phy_1t_init_table"))

	# rtwn writes each AGC value to R92C_OFDM0_AGCRSSITABLE (0xc78); the
	# three writes rtl8xxxu keeps at the end of its AGC table are explicit
	# in r23b_init_bb_common() (R92C_OFDM0_AGCCORE1(0), R92C_HSSI_PARAM2(0)).
	agc = [(0xc78, v) for v in numbers(c_array(ours, "rtl8723b_agc_vals"))]
	init = read(os.path.join(args.haiku,
		"src/add-ons/kernel/drivers/network/wlan/realtekwifi/dev/rtwn"
		"/rtl8723b/r23b_init.c"))
	for reg, name in ((0xc50, r"R92C_OFDM0_AGCCORE1\(0\)"),
			(0x824, r"R92C_HSSI_PARAM2\(0\)")):
		for value in re.findall(r"rtwn_write_4\(sc,\s*" + name + r",\s*("
				+ HEX + r")\)", init):
			agc.append((reg, int(value, 16)))
	ok &= compare("AGC", agc, linux_pairs(theirs, "rtl8xxx_agc_8723bu_table"))

	rf = list(zip(numbers(c_array(ours, "rtl8723b_rf_regs")),
		numbers(c_array(ours, "rtl8723b_rf_vals"))))
	ok &= compare("RF", rf,
		linux_pairs(theirs, "rtl8723bu_radioa_1t_init_table"))

	return 0 if ok else 1


if __name__ == "__main__":
	sys.exit(main())
