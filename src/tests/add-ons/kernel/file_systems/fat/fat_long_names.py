#!/usr/bin/env python3
"""fat_long_names.py IMAGE [DIR...]

The entries of a directory of a FAT32 image (the root, or DIR below it), read
from the raw directory: short name, number of UTF-16 units of the long name,
D for a directory, and the long name. names_test.sh checks what the FAT
driver wrote with it, independently of the driver and of mtools.
"""
import struct, sys
img = open(sys.argv[1], 'rb').read()
bps, spc, rsvd, nfats = struct.unpack_from('<HBHB', img, 11)
fatsz, = struct.unpack_from('<I', img, 36)
root, = struct.unpack_from('<I', img, 44)
data = (rsvd + nfats * fatsz) * bps
def chain(c):
    out = b''
    while 2 <= c < 0x0ffffff8:
        o = data + (c - 2) * spc * bps
        out += img[o:o + spc * bps]
        c = struct.unpack_from('<I', img, rsvd * bps + c * 4)[0] & 0x0fffffff
    return out
def entries(c):
    d = chain(c); parts = {}; res = []
    for i in range(0, len(d), 32):
        e = d[i:i + 32]
        if e[0] == 0: break
        if e[0] == 0xe5: parts = {}; continue
        if e[11] == 0x0f:
            parts[e[0] & 0x3f] = e[1:11] + e[14:26] + e[28:32]; continue
        if e[11] & 0x08: parts = {}; continue
        raw = b''.join(parts[k] for k in sorted(parts))
        units = struct.unpack('<%dH' % (len(raw) // 2), raw)
        units = units[:units.index(0)] if 0 in units else tuple(u for u in units if u != 0xffff)
        long = struct.pack('<%dH' % len(units), *units).decode('utf-16-le', 'surrogatepass')
        short = e[0:8].rstrip(b' ') + (b'.' + e[8:11].rstrip(b' ') if e[8:11].strip() else b'')
        cl = struct.unpack_from('<H', e, 20)[0] << 16 | struct.unpack_from('<H', e, 26)[0]
        res.append((long, short.decode('cp850', 'replace'), len(units), e[11], cl))
        parts = {}
    return res
c = root
for name in sys.argv[2:]:
    c = [e[4] for e in entries(c) if e[0] == name or e[1] == name][0]
for long, short, units, attr, cl in entries(c):
    if short in ('.', '..'): continue
    print('%-13s %3d units %s %r' % (short, units, 'D' if attr & 0x10 else '-', long))
