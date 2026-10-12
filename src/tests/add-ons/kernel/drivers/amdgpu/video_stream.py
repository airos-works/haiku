#!/usr/bin/env python3
# Copyright 2026, air/OS. Distributed under the terms of the MIT License.
"""Frame raw H.264 access units, then compare UVD pictures with FFmpeg software output."""
import argparse
import json
from pathlib import Path
import struct
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("stream", type=Path)
parser.add_argument("--pack", type=Path)
parser.add_argument("--frames", type=Path)
args = parser.parse_args()
if args.pack:
    packets = json.loads(subprocess.check_output([
        "ffprobe", "-v", "error", "-show_packets", "-show_entries", "packet=pos,size",
        "-of", "json", str(args.stream)]))["packets"]
    source = args.stream.read_bytes()
    with args.pack.open("wb") as output:
        for packet in packets:
            start, size = int(packet["pos"]), int(packet["size"])
            assert 0 < size <= 4 * 1024 * 1024 and 0 <= start <= len(source) - size
            output.write(struct.pack("<I", size))
            output.write(source[start:start + size])
    print("Packed", len(packets), "access units")
if args.frames:
    pictures = []
    with args.frames.open("rb") as source:
        while header := source.read(48):
            assert len(header) == 48
            magic, index, poc, sequence, width, height, pitch, size, *crop = struct.unpack("<IIi9I", header)
            assert magic == 0x55445631 and size == pitch * height * 3 // 2
            data = source.read(size)
            assert len(data) == size and width <= pitch
            linear = bytearray()
            for row in range(height * 3 // 2):
                linear.extend(data[row * pitch:row * pitch + width])
                assert not any(data[row * pitch + width:(row + 1) * pitch]), (index, "padding", row)
            pictures.append((sequence, poc, index, width, height, bytes(linear)))
    assert pictures
    pictures.sort(key=lambda p: (p[0], p[1], p[2]))
    reference = subprocess.check_output([
        "ffmpeg", "-v", "error", "-xerror", "-apply_cropping", "0", "-i", str(args.stream),
        "-fps_mode", "passthrough", "-pix_fmt", "nv12", "-f", "rawvideo", "-"])
    offset = 0
    for sequence, poc, index, width, height, actual in pictures:
        expected = reference[offset:offset + len(actual)]
        errors = sum(a != b for a, b in zip(actual, expected))
        assert len(expected) == len(actual) and errors == 0, (index, sequence, poc, width, height, errors)
        offset += len(actual)
    assert offset == len(reference), (offset, len(reference))
    print("PASS:", len(pictures), "pictures,", offset, "active bytes, exact FFmpeg match and zero row padding")
