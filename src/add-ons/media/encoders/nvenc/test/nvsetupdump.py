#!/usr/bin/env python3
"""Decode NVENC H.265 picture setup buffers and method lists field by field.

    nvsetupdump.py <nvenc_drv.h> setup.bin [other.bin]
    nvsetupdump.py --methods <clc5b7.h> methods.bin

A setup buffer (the add-on's NVENC_DUMP ours-*.bin, or nvsetupscan's
setup-*.bin or raw copies of NVIDIA's driver) is printed as one "field value"
line per field of nvenc_h265_drv_pic_setup_s, followed by the slice, motion
search, mode decision and quantiser controls at the offsets its picture
control names. With two files only the fields that differ are printed, side
by side. The layout comes from the C compiler: the header is preprocessed,
parsed (pycparser) and turned into a small C program that prints every field,
bit fields included, which is built with the host's gcc and run.

A method list (nvsetupscan's methods-*.bin) is printed one method a line,
named from an NVENC class header (the method offsets are the same from
Pascal to Blackwell). Addresses are in units of 256 bytes.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile

ROOTS = [
	("nvenc_h265_drv_pic_setup_s", "d_setup"),
	("nvenc_h265_slice_control_s", "d_slice"),
	("nvenc_h264_me_control_s", "d_me"),
	("nvenc_h265_md_control_s", "d_md"),
	("nvenc_h265_quant_control_s", "d_q"),
]

MAIN = r"""
int main(int argc, char **argv) {
	static unsigned char buf[1 << 20];
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL)
		return 1;
	size_t n = fread(buf, 1, sizeof(buf), f);
	fclose(f);
	const nvenc_h265_drv_pic_setup_s *s = (const void *)buf;
	const nvenc_h265_pic_control_s *p = &s->pic_control;
	char pre[32];
	d_setup(s, "");
	for (int i = 0; i <= p->num_forced_slices_minus1; i++) {
		size_t at = p->slice_control_offset + i * sizeof(nvenc_h265_slice_control_s);
		sprintf(pre, "slice%d.", i);
		if (at + sizeof(nvenc_h265_slice_control_s) <= n)
			d_slice((const void *)(buf + at), pre);
	}
	for (int i = 0; i <= p->num_me_controls_minus1; i++) {
		size_t at = p->me_control_offset + i * sizeof(nvenc_h264_me_control_s);
		sprintf(pre, "me%d.", i);
		if (at + sizeof(nvenc_h264_me_control_s) <= n)
			d_me((const void *)(buf + at), pre);
	}
	for (int i = 0; i <= p->num_md_controls_minus1; i++) {
		size_t at = p->md_control_offset + i * sizeof(nvenc_h265_md_control_s);
		sprintf(pre, "md%d.", i);
		if (at + sizeof(nvenc_h265_md_control_s) <= n)
			d_md((const void *)(buf + at), pre);
	}
	for (int i = 0; i <= p->num_q_controls_minus1; i++) {
		size_t at = p->q_control_offset + i * sizeof(nvenc_h265_quant_control_s);
		sprintf(pre, "q%d.", i);
		if (at + sizeof(nvenc_h265_quant_control_s) <= n)
			d_q((const void *)(buf + at), pre);
	}
	return 0;
}
"""


def generate(header):
	from pycparser import c_ast, c_parser
	source = subprocess.run(["gcc", "-E", "-P", "-x", "c", header],
		check=True, capture_output=True, text=True).stdout
	typedefs = {}
	for ext in c_parser.CParser().parse(source).ext:
		if isinstance(ext, c_ast.Typedef):
			typedefs[ext.name] = ext.type
	lines = []

	def emit(node, path, expr):
		if isinstance(node, c_ast.TypeDecl):
			inner = node.type
			if isinstance(inner, c_ast.Struct):
				return emit(inner, path, expr)
			name = " ".join(inner.names)
			if name in typedefs:
				return emit(typedefs[name], path, expr)
			lines.append('\tprintf("%%s%s %%lld\\n", pre, (long long)(%s));'
				% (path, expr))
		elif isinstance(node, c_ast.ArrayDecl):
			for i in range(int(node.dim.value, 0)):
				emit(node.type, "%s[%d]" % (path, i), "%s[%d]" % (expr, i))
		elif isinstance(node, c_ast.Struct):
			for decl in node.decls:
				if decl.name is not None:
					emit(decl.type, (path + "." if path else "") + decl.name,
						expr + "." + decl.name)

	program = [source, "#include <stdio.h>"]
	for type_name, function in ROOTS:
		lines = []
		emit(typedefs[type_name], "", "(*s)")
		program.append("static void %s(const %s *s, const char *pre) {"
			% (function, type_name))
		program += lines
		program.append("}")
	program.append(MAIN)
	return "\n".join(program)


def dump_setups(header, files):
	with tempfile.TemporaryDirectory() as directory:
		c_file = os.path.join(directory, "dump.c")
		binary = os.path.join(directory, "dump")
		with open(c_file, "w") as out:
			out.write(generate(header))
		subprocess.run(["gcc", "-w", "-o", binary, c_file], check=True)
		results = []
		for name in files:
			fields = {}
			order = []
			output = subprocess.run([binary, name], check=True,
				capture_output=True, text=True).stdout
			for line in output.splitlines():
				field, value = line.rsplit(" ", 1)
				fields[field] = value
				order.append(field)
			results.append((fields, order))
	if len(results) == 1:
		fields, order = results[0]
		for field in order:
			print(field, fields[field])
		return
	(a, order_a), (b, order_b) = results
	order = order_a + [f for f in order_b if f not in a]
	width = max(len(f) for f in order)
	for field in order:
		va, vb = a.get(field, "-"), b.get(field, "-")
		if va != vb:
			print("%-*s %12s %12s" % (width, field, va, vb))


def dump_methods(header, name):
	names = {0: "SET_OBJECT"}
	for line in open(header):
		m = re.match(r"#define NV[0-9A-F]{2}B7_(\w+)\s+\(0x([0-9A-Fa-f]+)U?\)", line)
		if m and int(m.group(2), 16) < 0x1000 and not re.search(
				r"^(OS_|ENC_|ERROR)|_(ID_|NOTIFY|AWAKEN|STRUCTURE|OPERATION"
				r"|FLUSH|CODEC|SUBFRAME|RCMODE|RESTORE|RST)", m.group(1)):
			names.setdefault(int(m.group(2), 16), m.group(1))
	data = open(name, "rb").read()
	words = struct.unpack("<%dI" % (len(data) // 4), data[:len(data) // 4 * 4])
	i = 0
	while i < len(words):
		header_word = words[i]
		op = header_word >> 29
		count = (header_word >> 16) & 0x1fff
		method = (header_word & 0x1fff) * 4
		if op in (1, 3, 5) and count > 0:
			for k in range(count):
				if i + 1 + k >= len(words):
					break
				offset = method + (4 * k if op == 1 else (4 if op == 3 and k else 0))
				print("%-28s %#x" % (names.get(offset, hex(offset)), words[i + 1 + k]))
			i += 1 + count
		elif op == 4:
			print("%-28s %#x (immediate)" % (names.get(method, hex(method)), count))
			i += 1
		else:
			i += 1


def main():
	if len(sys.argv) == 4 and sys.argv[1] == "--methods":
		dump_methods(sys.argv[2], sys.argv[3])
	elif len(sys.argv) in (3, 4):
		dump_setups(sys.argv[1], sys.argv[2:])
	else:
		sys.exit(__doc__)


main()
