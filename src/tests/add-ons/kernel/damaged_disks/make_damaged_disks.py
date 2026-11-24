#!/usr/bin/env python3
"""make_damaged_disks.py OUTDIR [BFS_SHELL]

Disk images with one damaged field each, for airos-works/haiku#165: every one
of them used to stop the kernel (a panic in the block cache, a division by
zero, a wild copy, a stack that ran out) when the disk was scanned, mounted
or listed. Each must now give an error and a syslog line.

The FAT32 image comes from mkfs.vfat and the BFS image from bfs_shell (the
second argument; without it the BFS images are left out); the ISO9660 and GPT
images are written from nothing. damaged_disks_test.sh runs those that have a
shell (bfs_shell, fat_shell) on the build host and says how to try the others
in a virtual machine.
"""
import os, struct, subprocess, sys, zlib

out = sys.argv[1]
bfs_shell = sys.argv[2] if len(sys.argv) > 2 else None
os.makedirs(out, exist_ok=True)
made = []


def write(name, data, what):
    with open(os.path.join(out, name), 'wb') as f:
        f.write(data)
    made.append((name, what))


# --- BFS -------------------------------------------------------------------

BFS_SB = 512                # the super block's offset in the volume
SB_LOG_RUN, SB_LOG_START, SB_LOG_END, SB_ROOT = 88, 96, 104, 116
INODE_DIRECT0 = 72          # bfs_inode::data.direct[0]


def run(group, start, length):
    return struct.pack('<iHH', group, start, length)


def bfs_images():
    good = os.path.join(out, 'bfs-good.img')
    with open(good, 'wb') as f:
        f.truncate(32 << 20)
    subprocess.run([bfs_shell, '--initialize', good, 'Damaged'], check=True,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run([bfs_shell, good], input=b'mkdir /myfs/dir\nsync\nquit\n',
        check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    image = open(good, 'rb').read()
    block_size, = struct.unpack_from('<I', image, BFS_SB + 40)
    groups, = struct.unpack_from('<i', image, BFS_SB + 80)
    log = struct.unpack_from('<iHH', image, BFS_SB + SB_LOG_RUN)
    log_start, = struct.unpack_from('<q', image, BFS_SB + SB_LOG_START)
    root = struct.unpack_from('<iHH', image, BFS_SB + SB_ROOT)
    shift, = struct.unpack_from('<i', image, BFS_SB + 76)

    def patched(*patches):
        data = bytearray(image)
        for offset, value in patches:
            data[offset:offset + len(value)] = value
        return bytes(data)

    pending = (BFS_SB + SB_LOG_END,
        struct.pack('<q', (log_start + 1) % log[2]))
        # "the log has an entry to replay"

    write('bfs-log-outside.img',
        patched((BFS_SB + SB_LOG_RUN, run(0x7fff, log[1], log[2])), pending),
        'BFS: the log lies in allocation group 0x7fff and has an entry to'
        ' replay')
    write('bfs-log-empty.img',
        patched((BFS_SB + SB_LOG_RUN, run(log[0], log[1], 0)), pending),
        'BFS: the log is 0 blocks long and has an entry to replay')
    write('bfs-log-past-end.img',
        patched((BFS_SB + SB_LOG_RUN, run(log[0], 0xff00, log[2])), pending),
        'BFS: the log starts inside the volume\'s last allocation group but'
        ' past the last block, and has an entry to replay')
    write('bfs-root-outside.img',
        patched((BFS_SB + SB_ROOT, run(groups, root[1], 1))),
        'BFS: the root directory is in the allocation group after the last')
    root_block = ((root[0] << shift) + root[1]) * block_size
    write('bfs-dir-run-outside.img',
        patched((root_block + INODE_DIRECT0, run(groups, 515, 1))),
        'BFS: the root directory\'s first block run is in the allocation'
        ' group after the last (ls the volume)')


# --- FAT32 -----------------------------------------------------------------

def fat_images():
    good = os.path.join(out, 'fat-good.img')
    with open(good, 'wb') as f:
        f.truncate(100 << 20)
    subprocess.run(['mkfs.vfat', '-F', '32', good], check=True,
        stdout=subprocess.DEVNULL)
    image = bytearray(open(good, 'rb').read())
    image[0x47:0x47 + 11] = b' ' * 11
        # a blank label: with a label the driver reads the root directory with
        # read_pos() first, which fails cleanly
    bps, spc, reserved, fats = struct.unpack_from('<HBHB', image, 11)
    fat_sectors, = struct.unpack_from('<I', image, 36)
    root, = struct.unpack_from('<I', image, 44)
    backup, = struct.unpack_from('<H', image, 50)

    def boot_patched(offset, value):
        data = bytearray(image)
        for sector in (0, backup):
            at = sector * bps + offset
            data[at:at + len(value)] = value
        return data

    write('fat-root-cluster.img',
        boot_patched(44, struct.pack('<I', 0x0ffffff0)),
        'FAT32: the root directory starts at cluster 0x0ffffff0')

    # two entries whose start cluster is far past the volume's last one
    def entry(name, attributes, cluster, size):
        return struct.pack('<11sBBBHHHHHHHI', name, attributes, 0, 0, 0, 0x5a21,
            0x5a21, cluster >> 16, 0, 0x5a21, cluster & 0xffff, size)
    data = bytearray(image)
    at = (reserved + fats * fat_sectors + (root - 2) * spc) * bps
    entries = entry(b'BADDIR     ', 0x10, 0x0fffff00, 0) \
        + entry(b'BADFILE TXT', 0x20, 0x0fffff00, 4096)
    data[at:at + len(entries)] = entries
    write('fat-entry-cluster.img', data,
        'FAT32: BADDIR and BADFILE.TXT start at cluster 0x0fffff00 (ls'
        ' BADDIR, cat BADFILE.TXT)')

    # a file and a directory that start inside the volume; their cluster
    # chains go on at a cluster far past its end
    data = bytearray(image)
    entries = entry(b'CHAINDIR   ', 0x10, 3, 0) \
        + entry(b'CHAIN   TXT', 0x20, 4, 16 * spc * bps)
    data[at:at + len(entries)] = entries
    for copy in range(fats):
        fat = (reserved + copy * fat_sectors) * bps
        struct.pack_into('<II', data, fat + 3 * 4, 0x0fffff00, 0x0fffff00)
    cluster3 = at + (3 - root) * spc * bps
    dots = entry(b'.          ', 0x10, 3, 0) + entry(b'..         ', 0x10, 0, 0)
    data[cluster3:cluster3 + spc * bps] = dots \
        + b''.join(entry(b'FILL%04d   ' % i, 0x20, 0, 0)
            for i in range(spc * bps // 32 - 2))
    write('fat-chain-cluster.img', data,
        'FAT32: the cluster chains of CHAINDIR and CHAIN.TXT go on at cluster'
        ' 0x0fffff00 (ls CHAINDIR, cat CHAIN.TXT)')


# --- ISO9660 ---------------------------------------------------------------

ISO_BLOCK = 2048
ISO_BLOCKS = 64


def both16(value):
    return struct.pack('<H', value) + struct.pack('>H', value)


def both32(value):
    return struct.pack('<I', value) + struct.pack('>I', value)


def record(block, size, flags, name, system_use=b''):
    body = bytes([0]) + both32(block) + both32(size) \
        + bytes([126, 11, 24, 12, 0, 0, 0]) + bytes([flags, 0, 0]) \
        + both16(1) + bytes([len(name)]) + name
    if len(name) % 2 == 0:
        body += b'\0'
    body += system_use
    return bytes([len(body) + 1]) + body


def iso(root_record, root_dir, extra=None):
    """An image with the given root directory record in the primary volume
    descriptor, and the given bytes as the root directory's block (18)."""
    image = bytearray(ISO_BLOCKS * ISO_BLOCK)
    pvd = bytearray(ISO_BLOCK)
    pvd[0:7] = b'\x01CD001\x01'
    pvd[8:40] = b'HAIKU'.ljust(32)
    pvd[40:72] = b'DAMAGED'.ljust(32)
    pvd[80:88] = both32(ISO_BLOCKS)
    pvd[120:124] = both16(1)
    pvd[124:128] = both16(1)
    pvd[128:132] = both16(ISO_BLOCK)
    pvd[132:140] = both32(10)
    pvd[140:144] = struct.pack('<I', 19)
    pvd[148:152] = struct.pack('>I', 20)
    pvd[190:813] = b' ' * 623
    pvd[156:156 + len(root_record)] = root_record
    pvd[813:881] = (b'2026112412000000\0') * 4
    pvd[881] = 1
    image[16 * ISO_BLOCK:17 * ISO_BLOCK] = pvd
    image[17 * ISO_BLOCK:17 * ISO_BLOCK + 7] = b'\xffCD001\x01'
    image[18 * ISO_BLOCK:18 * ISO_BLOCK + len(root_dir)] = root_dir
    path_table = bytes([1, 0]) + struct.pack('<IH', 18, 1) + b'\0\0'
    image[19 * ISO_BLOCK:19 * ISO_BLOCK + 10] = path_table
    image[20 * ISO_BLOCK:20 * ISO_BLOCK + 10] = bytes([1, 0]) \
        + struct.pack('>IH', 18, 1) + b'\0\0'
    for block, data in (extra or {}).items():
        image[block * ISO_BLOCK:block * ISO_BLOCK + len(data)] = data
    return bytes(image)


def iso_images():
    DIR = 2
    root = record(18, ISO_BLOCK, DIR, b'\0')
    dots = root + record(18, ISO_BLOCK, DIR, b'\1')
    hello = record(21, 6, 0, b'HELLO.TXT;1')
    text = {21: b'hello\n'}

    write('iso-good.img', iso(root, dots + hello, text), 'ISO9660: undamaged')
    write('iso-name-short.img',
        iso(record(18, ISO_BLOCK, DIR, b'\0', b'NM\x04\x01'), dots + hello,
            text),
        'ISO9660: the root directory record carries a Rock Ridge name entry'
        ' of 4 bytes (its header takes 5)')
    write('iso-root-outside.img',
        iso(record(0x7fffff00, ISO_BLOCK, DIR, b'\0'), dots + hello, text),
        'ISO9660: the root directory starts at block 0x7fffff00 (ls the'
        ' volume)')
    write('iso-dir-long.img',
        iso(record(18, 0x7ff00000, DIR, b'\0'), dots + hello, text),
        'ISO9660: the root directory claims to be 2 GiB long on a 128 KiB'
        ' volume (ls the volume)')
    write('iso-file-outside.img',
        iso(root, dots + record(0x7fffff00, 6, 0, b'HELLO.TXT;1'), text),
        'ISO9660: HELLO.TXT starts at block 0x7fffff00 (cat it)')
    # a relocated directory ("CL") whose "." entry points at itself again
    loop = record(18, ISO_BLOCK, DIR, b'\0', b'CL\x0c\x01' + both32(18))
    write('iso-relocation-loop.img',
        iso(root, loop + record(18, ISO_BLOCK, DIR, b'\1') + hello, text),
        'ISO9660: the root directory\'s "." entry is a relocated directory'
        ' that points at itself (ls the volume)')
    write('iso-relocation-outside.img',
        iso(root, dots + record(18, ISO_BLOCK, 0, b'DEEP',
            b'CL\x0c\x01' + both32(0x7fffff00)) + hello, text),
        'ISO9660: the entry DEEP is a relocated directory at block'
        ' 0x7fffff00 (ls the volume)')
    write('iso-relocation-short.img',
        iso(root, dots + record(18, ISO_BLOCK, 0, b'DEEP', b'CL\x04\x01')
            + hello, text),
        'ISO9660: a relocation entry of 4 bytes (it takes 12)')
    # a symbolic link whose components say they are longer than the entry;
    # 255-byte components make the 8-bit position wrap around for ever
    link = b'SL\xff\x01\x00' + b'\x00\xfa' * 125
    write('iso-link-long.img',
        iso(root, dots + record(21, 0, 0, b'LINK', link[:200]) + hello, text),
        'ISO9660: the symbolic link LINK has components that reach past its'
        ' entry (ls -l the volume)')
    write('iso-attributes-short.img',
        iso(root, dots + record(21, 6, 0, b'SHORT.TXT;1', b'PX\x04\x01')
            + hello, text),
        'ISO9660: a POSIX attributes entry of 4 bytes (it takes 36)')
    # two complete names for one record: the second freed the first twice
    write('iso-name-twice.img',
        iso(root, dots + record(21, 6, 0, b'TWICE.TXT;1',
            b'NM\x08\x01\x00one' + b'NM\x45\x01\x00' + b'x' * 64) + hello,
            text),
        'ISO9660: a record with two complete Rock Ridge names (ls the'
        ' volume)')
    # a record that runs past the end of its block
    tail = bytearray(ISO_BLOCK)
    tail[:len(dots)] = dots
    filler = ISO_BLOCK - len(dots) - 40
    position = len(dots)
    while filler > 0:
        size = min(filler, 254)
        if filler - size == 1:
            size -= 1
        name = b'F' * (size - 33 - (1 - (size - 33) % 2))
        entry = record(21, 6, 0, name)
        entry = entry.ljust(size, b'\0')
        entry = bytes([size]) + entry[1:]
        tail[position:position + size] = entry
        position += size
        filler -= size
    last = bytearray(record(21, 6, 0, b'LAST.TXT;1'))
    last[0] = 255
    tail[position:position + 40] = last[:40]
    write('iso-record-past-block.img', iso(root, bytes(tail), text),
        'ISO9660: the last record of the root directory says it is 255 bytes'
        ' long, 40 bytes before the end of the block (ls the volume)')


# --- GPT -------------------------------------------------------------------

def gpt(entry_size, entry_count, entries, blocks=32768):
    """A disk with a protective MBR and two GPT headers that describe an
    entry array of the given geometry; the checksums are right."""
    image = bytearray(blocks * 512)
    image[446:462] = struct.pack('<BBBBBBBBII', 0, 0, 2, 0, 0xee, 0xff, 0xff,
        0xff, 1, blocks - 1)
    image[510:512] = b'\x55\xaa'
    crc = zlib.crc32(entries) & 0xffffffff

    def header(own, other, entries_block):
        data = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART',
            0x10000, 92, 0, 0, own, other, 34, blocks - 34,
            bytes(range(16)), entries_block, entry_count, entry_size, crc))
        struct.pack_into('<I', data, 16, zlib.crc32(bytes(data)) & 0xffffffff)
        data = data.ljust(512, b'\0')
        return data
    image[512:1024] = header(1, blocks - 1, 2)
    image[1024:1024 + len(entries)] = entries
    backup_entries = blocks - 33
    image[backup_entries * 512:backup_entries * 512 + len(entries)] = entries
    image[(blocks - 1) * 512:blocks * 512] = header(blocks - 1, 1,
        backup_entries)
    return bytes(image)


def gpt_images():
    entry = bytearray(128)
    entry[0:16] = bytes.fromhex('a2a0d0ebe5b9334487c068b6b72699c7')
        # basic data
    entry[16:32] = bytes(range(16, 32))
    struct.pack_into('<QQ', entry, 32, 2048, 4095)
    entry[56:56 + 8] = 'data'.encode('utf-16-le')
    good = bytes(entry) + bytes(128 * 127)
    write('gpt-good.img', gpt(128, 128, good), 'GPT: undamaged')
    # 0x80000080 * 2 is 0x100 in 32 bits: 256 bytes are allocated and summed
    write('gpt-entry-size.img', gpt(0x80000080, 2, good[:256]),
        'GPT: an entry size of 0x80000080 with 2 entries')
    write('gpt-entry-small.img', gpt(8, 128, good[:1024]),
        'GPT: an entry size of 8 bytes')
    write('gpt-entry-count.img', gpt(128, 0x02000002, good[:256]),
        'GPT: 0x02000002 entries of 128 bytes (256 bytes in 32 bits)')


if bfs_shell:
    bfs_images()
fat_images()
iso_images()
gpt_images()
for name, what in made:
    print('%-28s %s' % (name, what))
