#!/usr/bin/env python3
"""Pack raw HEVC access units and compare one-GOP UVD NV12/P010 records with FFmpeg."""
import argparse
import json
from pathlib import Path
import struct
import subprocess


def probe(source, packets=False):
    return json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_streams',
        *(['-show_packets', '-show_data'] if packets else []), '-of', 'json', str(source)]))


def pack(source, output):
    info = probe(source, True)
    assert info['streams'][0]['codec_name'] == 'hevc'
    with output.open('wb') as dest:
        for packet in info['packets']:
            data = bytes.fromhex(''.join(line.split(':', 1)[1].split('  ', 1)[0]
                for line in packet['data'].splitlines() if ':' in line))
            assert len(data) == int(packet['size']) and 0 < len(data) <= 4 * 1024 * 1024
            assert data.startswith((b'\x00\x00\x01', b'\x00\x00\x00\x01')), 'raw Annex B input required'
            dest.write(struct.pack('<I', len(data)))
            dest.write(data)
    print(f"packed {len(info['packets'])} HEVC access units: {output}")


def verify(source, output):
    info = probe(source)['streams'][0]
    width, height = info['width'], info['height']
    assert not (width % 2 or height % 2)
    p010 = info['profile'] == 'Main 10'
    sample_bytes = 2 if p010 else 1
    reference = subprocess.check_output([
        'ffmpeg', '-v', 'error', '-xerror', '-i', str(source), '-an',
        '-pix_fmt', 'p010le' if p010 else 'nv12', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-'])
    frame_bytes = width * height * 3 // 2 * sample_bytes
    assert len(reference) % frame_bytes == 0
    data, offset, frames, epochs = output.read_bytes(), 0, {}, set()
    while offset < len(data):
        assert len(data) - offset >= 64
        (magic, index, poc, epoch, coded_width, coded_height, pitch, size,
         left, top, w, h, output_height, fmt, depth, flags) = struct.unpack_from('<16I', data, offset)
        poc = struct.unpack('<i', struct.pack('<I', poc))[0]
        offset += 64
        assert magic == 0x55445632 and (w, h) == (width, height)
        assert fmt == (0x50303130 if p010 else 0x4e563132)
        assert depth in (8, 10) and (p010 or depth == 8) and flags & 1
        assert pitch >= coded_width * sample_bytes and output_height >= coded_height
        assert left + w <= coded_width and top + h <= coded_height and not (left % 2 or top % 2)
        assert size == pitch * output_height * 3 // 2 and offset + size <= len(data)
        pixels = data[offset:offset + size]
        offset += size
        visible = bytearray()
        for base, y, rows in ((0, top, h), (pitch * output_height, top // 2, h // 2)):
            for row in range(y, y + rows):
                start = base + row * pitch + left * sample_bytes
                visible.extend(pixels[start:start + w * sample_bytes])
        assert len(visible) == frame_bytes and poc not in frames
        if p010:
            assert all(value & 0x3f == 0 for value in pixels[::2]), 'P010 low bits are not zero'
        frames[poc] = bytes(visible)
        epochs.add(epoch)
    assert len(epochs) == 1, 'this comparison requires one coded sequence'
    assert sorted(frames) == list(range(len(reference) // frame_bytes))
    actual = b''.join(frames[i] for i in sorted(frames))
    if actual != reference:
        mismatches = sum(a != b for a, b in zip(actual, reference))
        raise AssertionError(f'{mismatches} byte differences from FFmpeg')
    print(f'PASS: {len(frames)} pictures, {len(actual)} visible bytes exactly match FFmpeg '
          f"{'P010 (full precision)' if p010 else 'NV12'}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument('--pack', type=Path)
    action.add_argument('--verify', type=Path)
    args = parser.parse_args()
    if args.pack:
        pack(args.source, args.pack)
    else:
        verify(args.source, args.verify)
