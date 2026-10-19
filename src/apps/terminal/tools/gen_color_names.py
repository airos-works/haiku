#!/usr/bin/env python3
# Generates emulator/ColorNames.cpp from tools/color-names.txt (one
# "name=rrggbb" per line, the X11 color names kitty and xterm accept).
#
# usage: gen_color_names.py tools/color-names.txt > emulator/ColorNames.cpp
import sys

pairs = sorted(set(tuple(line.strip().rsplit("=", 1))
	for line in open(sys.argv[1]) if "=" in line))
out = sys.stdout
out.write("""/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * The X11 color names (as xterm and kitty accept them), generated from
 * tools/color-names.txt by tools/gen_color_names.py. Do not edit.
 */


#include "ColorNames.h"

#include <string.h>
#include <strings.h>


struct ColorName {
	const char*	name;
	uint32		color;
};


static const ColorName kColorNames[] = {
""")
for name, value in pairs:
	out.write('\t{ "%s", 0x%s },\n' % (name, value))
out.write("""};


bool
lookup_color_name(const char* name, uint32& _color)
{
	int32 low = 0;
	int32 high = sizeof(kColorNames) / sizeof(kColorNames[0]) - 1;
	while (low <= high) {
		int32 middle = (low + high) / 2;
		int compare = strcasecmp(name, kColorNames[middle].name);
		if (compare == 0) {
			_color = kColorNames[middle].color;
			return true;
		}
		if (compare < 0)
			high = middle - 1;
		else
			low = middle + 1;
	}
	return false;
}
""")
