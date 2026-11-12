#!/usr/bin/env python3
"""Generates IntelGpioTables.h: where each Intel GPIO controller keeps the
pads that ACPI GpioInt resources name, for i2c_hid's IntelGpioController.

The data (communities, pad groups, register offsets) comes from Linux's
drivers/pinctrl/intel/pinctrl-<soc>.c; only the facts are taken, as tables.

usage: generate_intel_gpio_tables.py <linux drivers/pinctrl/intel> > IntelGpioTables.h
"""

import re
import sys

# The controllers this driver knows, by the Linux file that describes them.
SOC_FILES = [
	"pinctrl-broxton.c",		# Apollo Lake (and Broxton)
	"pinctrl-geminilake.c",		# Gemini Lake
	"pinctrl-sunrisepoint.c",	# Skylake, Kaby Lake (PCH-LP and -H)
	"pinctrl-cannonlake.c",		# Cannon Lake, Coffee Lake, Whiskey Lake, Comet Lake
	"pinctrl-icelake.c",		# Ice Lake
	"pinctrl-jasperlake.c",		# Jasper Lake
	"pinctrl-elkhartlake.c",	# Elkhart Lake
	"pinctrl-tigerlake.c",		# Tiger Lake, Alder Lake-P (INTC1055)
	"pinctrl-alderlake.c",		# Alder Lake-S and -N, Raptor Lake-S
	"pinctrl-meteorlake.c",		# Meteor Lake
]

GPIO_BASE_MATCH = 0
GPIO_BASE_NOMAP = -1
GPIO_BASE_ZERO = -2
MAX_GPP_SIZE = 32


def strip_comments(text):
	text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
	return re.sub(r"//[^\n]*", "", text)


def number(token, consts):
	token = token.strip()
	if token in consts:
		return consts[token]
	if token == "INTEL_GPIO_BASE_NOMAP":
		return GPIO_BASE_NOMAP
	if token == "INTEL_GPIO_BASE_ZERO":
		return GPIO_BASE_ZERO
	if token == "INTEL_GPIO_BASE_MATCH":
		return GPIO_BASE_MATCH
	return int(token, 0)


def split_args(text):
	args, depth, current = [], 0, ""
	for c in text:
		if c == "," and depth == 0:
			args.append(current.strip())
			current = ""
			continue
		if c in "([{":
			depth += 1
		elif c in ")]}":
			depth -= 1
		current += c
	if current.strip():
		args.append(current.strip())
	return args


def macro_calls(body, names):
	"""Yields (name, args) for every call of one of the macros in body."""
	pattern = re.compile(r"\b(" + "|".join(map(re.escape, names)) + r")\s*\(")
	pos = 0
	while True:
		m = pattern.search(body, pos)
		if not m:
			return
		depth, i = 1, m.end()
		while depth:
			if body[i] == "(":
				depth += 1
			elif body[i] == ")":
				depth -= 1
			i += 1
		yield m.group(1), split_args(body[m.end():i - 1])
		pos = i


def parse(path):
	text = strip_comments(open(path).read())

	# register offsets: <PREFIX>_PAD_OWN etc.
	offsets = {}
	for prefix, reg, value in re.findall(
			r"#define\s+(\w+?)_(PAD_OWN|PADCFGLOCK|HOSTSW_OWN|GPI_IS|GPI_IE)\s+(0x[0-9a-fA-F]+|\d+)",
			text):
		offsets.setdefault(prefix, {})[reg] = int(value, 0)

	# pad group macros: NAME(r, s, e[, g])
	gpp_macros = {}
	for name, params in re.findall(r"#define\s+(\w+_GPP)\(([^)]*)\)", text):
		gpp_macros[name] = len(split_args(params))
	gpp_macros.setdefault("INTEL_GPP", 4)

	# community macros: NAME(b, s, e[, g]) -> GPPS(b, s, e, g, PFX) or SIZE(b, s, e, gs, gn, PFX)
	community_macros = {}
	for name, params, body in re.findall(
			r"#define\s+(\w+_COMMUNITY)\(([^)]*)\)\s*\\\s*\n\s*(INTEL_COMMUNITY_(?:GPPS|SIZE)\([^)]*\))",
			text):
		kind = "GPPS" if "GPPS" in body else "SIZE"
		inner = split_args(body[body.index("(") + 1:body.rindex(")")])
		community_macros[name] = (split_args(params), kind, inner)

	# pad group arrays
	padgroups = {}
	for name, body in re.findall(
			r"static\s+const\s+struct\s+intel_padgroup\s+(\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
		groups = []
		for macro, args in macro_calls(body, list(gpp_macros)):
			r, s, e = (number(a, {}) for a in args[:3])
			g = number(args[3], {}) if len(args) > 3 else GPIO_BASE_MATCH
			groups.append((r, s, e - s + 1, g))
		padgroups[name] = groups

	# community arrays
	communities = {}
	for name, body in re.findall(
			r"static\s+const\s+struct\s+intel_community\s+(\w+)\[\]\s*=\s*\{(.*?)\};", text, re.S):
		result = []
		for macro, args in macro_calls(body, list(community_macros)):
			params, kind, inner = community_macros[macro]
			binding = dict(zip(params, args))
			resolved = [binding.get(a, a) for a in inner]
			prefix = resolved[-1]
			b, s, e = (number(a, {}) for a in resolved[:3])
			regs = offsets[prefix]
			community = {
				"bar": b, "pin_base": s, "npins": e - s + 1,
				"padown": regs.get("PAD_OWN", 0), "padcfglock": regs.get("PADCFGLOCK", 0),
				"hostown": regs.get("HOSTSW_OWN", 0), "is": regs["GPI_IS"], "ie": regs["GPI_IE"],
			}
			if kind == "GPPS":
				community["groups"] = by_gpps(padgroups[resolved[3]])
			else:
				community["groups"] = by_size(s, e - s + 1, number(resolved[3], {}),
					number(resolved[4], {}))
			result.append(community)
		communities[name] = result

	# SoC data: name -> (uid, communities)
	socs = {}
	for name, body in re.findall(
			r"static\s+const\s+struct\s+intel_pinctrl_soc_data\s+(\w+)\s*=\s*\{(.*?)\};", text, re.S):
		uid = re.search(r"\.uid\s*=\s*\"([^\"]*)\"", body)
		comm = re.search(r"\.communities\s*=\s*(\w+)", body)
		if comm is None:
			continue
		socs[name] = (uid.group(1) if uid else None, communities[comm.group(1)])

	# arrays of SoC data, one per UID
	soc_arrays = {}
	for name, body in re.findall(
			r"static\s+const\s+struct\s+intel_pinctrl_soc_data\s*\*\s*(\w+)\[\]\s*=\s*\{(.*?)\};",
			text, re.S):
		soc_arrays[name] = re.findall(r"&(\w+)", body)

	# ACPI IDs
	result = []
	match = re.search(r"struct\s+acpi_device_id\s+\w+\[\]\s*=\s*\{(.*?)\};", text, re.S)
	for hid, ref in re.findall(r"\{\s*\"(\w+)\"\s*,\s*\(kernel_ulong_t\)\s*&?(\w+)\s*\}",
			match.group(1)):
		if ref in socs:
			result.append((hid, socs[ref][0], socs[ref][1]))
		else:
			for soc in soc_arrays[ref]:
				result.append((hid, socs[soc][0], socs[soc][1]))
	return result


def by_gpps(groups):
	result, padown_num = [], 0
	for reg_num, base, size, gpio_base in groups:
		assert size <= MAX_GPP_SIZE
		if gpio_base == GPIO_BASE_MATCH:
			gpio_base = base
		elif gpio_base == GPIO_BASE_ZERO:
			gpio_base = 0
		result.append((reg_num, base, size, gpio_base, padown_num))
		padown_num += (size * 4 + MAX_GPP_SIZE - 1) // MAX_GPP_SIZE
	return result


def by_size(pin_base, npins, gpp_size, gpp_num_padown_regs):
	assert gpp_size <= MAX_GPP_SIZE
	result, padown_num = [], 0
	count = (npins + gpp_size - 1) // gpp_size
	for i in range(count):
		size = min(gpp_size, npins)
		npins -= size
		base = pin_base + i * gpp_size
		result.append((i, base, size, base, padown_num))
		padown_num += gpp_num_padown_regs
	return result


def main():
	directory = sys.argv[1]
	controllers = []
	for f in SOC_FILES:
		controllers += [(f,) + c for c in parse(directory + "/" + f)]

	group_lines, community_lines, controller_lines = [], [], []
	layouts = {}
	for f, hid, uid, communities in controllers:
		key = repr(communities)
		if key not in layouts:
			first_community = len(community_lines)
			for c in communities:
				first_group = len(group_lines)
				for reg_num, base, size, gpio_base, padown_num in c["groups"]:
					group_lines.append("\t{ %d, %d, %d, %d, %d },"
						% (reg_num, base, size, gpio_base, padown_num))
				community_lines.append("\t{ %d, %#x, %#x, %#x, %#x, %#x, %d, %d, %d, %d },"
					% (c["bar"], c["padown"], c["padcfglock"], c["hostown"], c["is"],
						c["ie"], c["pin_base"], c["npins"], first_group,
						len(group_lines) - first_group))
			layouts[key] = (first_community, len(community_lines) - first_community)
		first, count = layouts[key]
		controller_lines.append("\t{ \"%s\", %s, %d, %d },\t// %s"
			% (hid, "\"%s\"" % uid if uid else "NULL", first, count, f[len("pinctrl-"):-2]))

	out = """/*
 * Generated by generate_intel_gpio_tables.py from Linux 6.18
 * drivers/pinctrl/intel: the communities and pad groups of Intel GPIO
 * controllers, as tables. Do not edit.
 */
#ifndef I2C_HID_INTEL_GPIO_TABLES_H
#define I2C_HID_INTEL_GPIO_TABLES_H


#include <SupportDefs.h>


// GpioInt pin numbers of a group: gpioBase onwards; kIntelNoGpio: none.
static const int16 kIntelNoGpio = -1;


struct IntelPadGroup {
	uint8	regNum;		// its GPI_IS/GPI_IE/HOSTSW_OWN register (PADCFGLOCK: two)
	uint16	base;		// its first pin
	uint8	size;		// pins, at most 32
	int16	gpioBase;	// GpioInt number of its first pin, or kIntelNoGpio
	uint8	padOwnNum;	// its first PAD_OWN register
};


struct IntelCommunity {
	uint8	bar;		// memory resource of the controller's _CRS
	uint16	padOwn;		// register offsets, 0: none
	uint16	padCfgLock;
	uint16	hostSwOwn;
	uint16	gpiIs;
	uint16	gpiIe;
	uint16	pinBase;
	uint16	pinCount;
	uint16	firstGroup;	// in kIntelPadGroups
	uint8	groupCount;
};


struct IntelGpioSoc {
	const char*	hid;
	const char*	uid;	// NULL: any; else one ACPI device per community set
	uint8		firstCommunity;	// in kIntelCommunities
	uint8		communityCount;
};


static const IntelPadGroup kIntelPadGroups[] = {
""" + "\n".join(group_lines) + """
};


static const IntelCommunity kIntelCommunities[] = {
""" + "\n".join(community_lines) + """
};


static const IntelGpioSoc kIntelGpioSocs[] = {
""" + "\n".join(controller_lines) + """
	{ NULL, NULL, 0, 0 }
};


#endif	// I2C_HID_INTEL_GPIO_TABLES_H"""
	print(out)


if __name__ == "__main__":
	main()
