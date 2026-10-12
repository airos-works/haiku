#!/usr/bin/env python3
# Copyright 2026, air/OS. Distributed under the terms of the MIT License.
"""Create cropped-height transitions and valid repeated MMCO 5 H.264 pictures.

Requires FFmpeg with libx264. The MMCO fixture retains the encoded CAVLC picture
data, uses one reference, and resets that reference after every P picture.
FFmpeg must decode it without errors and match the original stream byte for byte
before the fixture is admitted for a hardware test.
"""
import argparse
from pathlib import Path
import re
import subprocess


def run(*arguments):
    return subprocess.run(arguments, check=True, capture_output=True)


class Bits:
    def __init__(self, value):
        self.value = value
        self.at = 0

    def unsigned(self, count):
        assert self.at + count <= len(self.value)
        value = int(self.value[self.at:self.at + count], 2) if count else 0
        self.at += count
        return value

    def ue(self):
        count = 0
        while self.unsigned(1) == 0:
            count += 1
            assert count < 32
        return (1 << count) - 1 + self.unsigned(count)

    def se(self):
        value = self.ue()
        return (value + 1) // 2 if value & 1 else -value // 2


def unescape(data):
    output = bytearray()
    zeros = 0
    for value in data:
        if zeros == 2 and value == 3:
            zeros = 0
            continue
        output.append(value)
        zeros = zeros + 1 if value == 0 else 0
    return ''.join(f'{value:08b}' for value in output)


def escape(bits):
    # Keep the RBSP stop bit and restore alignment after inserting the marking.
    bits = bits.rstrip('0')
    bits += '0' * (-len(bits) % 8)
    output = bytearray()
    zeros = 0
    for offset in range(0, len(bits), 8):
        value = int(bits[offset:offset + 8], 2)
        if zeros == 2 and value <= 3:
            output.append(3)
            zeros = 0
        output.append(value)
        zeros = zeros + 1 if value == 0 else 0
    return bytes(output)


def encode(path, size, frames, profile, parameters):
    run('ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', f'testsrc2=size={size}:rate=24',
        '-frames:v', str(frames), '-c:v', 'libx264', '-profile:v', profile,
        '-pix_fmt', 'yuv420p', '-x264-params', parameters, '-y', str(path))


def make_mmco5(directory):
    original = directory / 'original.h264'
    encode(original, '320x240', 12, 'baseline',
           'keyint=24:min-keyint=24:scenecut=0:bframes=0:ref=1:cabac=0')
    units = [nal for nal in re.split(b'\x00\x00\x00?\x01', original.read_bytes()) if nal]
    frame_bits = None
    changed = 0
    output = bytearray()
    for nal in units:
        kind = nal[0] & 31
        bits = unescape(nal[1:])
        reader = Bits(bits)
        if kind == 7:
            assert reader.unsigned(8) == 66  # Baseline; no high-profile syntax
            reader.unsigned(8)
            reader.unsigned(8)
            assert reader.ue() == 0
            frame_bits = reader.ue() + 4
            assert reader.ue() == 2 and reader.ue() == 1  # POC type 2, one reference
            assert reader.unsigned(1) == 0  # no frame_num gaps
            reader.ue()
            reader.ue()
            assert reader.unsigned(1) == 1  # progressive frames
        elif kind == 8:
            assert reader.ue() == 0 and reader.ue() == 0
            assert reader.unsigned(1) == 0 and reader.unsigned(1) == 0  # CAVLC, no extra POC
            assert reader.ue() == 0  # no slice groups
            assert reader.ue() == 0 and reader.ue() == 0  # one active reference
            assert reader.unsigned(1) == 0 and reader.unsigned(2) == 0  # no weights
            reader.se()
            reader.se()
            reader.se()
            reader.unsigned(1)
            reader.unsigned(1)
            assert reader.unsigned(1) == 0  # no redundant-pic count
        elif kind == 1:
            assert nal[0] >> 5 & 3 and reader.ue() == 0
            assert reader.ue() % 5 == 0 and reader.ue() == 0  # first P slice, PPS 0
            frame_at = reader.at
            reader.unsigned(frame_bits)
            frame_end = reader.at
            if reader.unsigned(1):
                assert reader.ue() == 0  # active-reference override still selects one
            assert reader.unsigned(1) == 0  # no reference-list reordering
            marking_at = reader.at
            assert reader.unsigned(1) == 0  # original sliding-window marking
            # The preceding MMCO 5 renumbers our only reference to frame_num 0.
            # Predict from it as frame_num 1, then reset again after this picture.
            bits = (bits[:frame_at] + format(1, f'0{frame_bits}b')
                    + bits[frame_end:marking_at] + '1' + '00110' + '1'
                    + bits[reader.at:])  # adaptive marking, ue(5), ue(0)
            nal = nal[:1] + escape(bits)
            changed += 1
        output += b'\0\0\0\1' + nal
    assert changed == 11
    modified = directory / 'mmco5.h264'
    modified.write_bytes(output)
    decoded = []
    for source in (original, modified):
        result = run('ffmpeg', '-v', 'error', '-xerror', '-i', str(source),
                     '-pix_fmt', 'yuv420p', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-')
        assert not result.stderr, result.stderr
        assert len(result.stdout) == 12 * 320 * 240 * 3 // 2
        decoded.append(result.stdout)
    assert decoded[0] == decoded[1], 'MMCO modification changed the reference pictures'
    run('ffmpeg', '-v', 'error', '-r', '24', '-i', str(modified),
        '-c', 'copy', '-y', str(directory / 'mmco5.mp4'))
    print('MMCO 5: 11 P-picture resets; all 12 decoded pictures match the original')


def make_transition(directory):
    parameters = 'keyint=12:min-keyint=12:scenecut=0:bframes=3:ref=3'
    encode(directory / 'a.mp4', '320x240', 12, 'high', parameters)
    encode(directory / 'b.mp4', '320x256', 12, 'high', parameters + ':crop-rect=0,0,0,16')
    listing = directory / 'concat.txt'
    listing.write_text("file 'a.mp4'\nfile 'b.mp4'\nfile 'a.mp4'\n")
    run('ffmpeg', '-v', 'error', '-f', 'concat', '-safe', '0', '-i', str(listing),
        '-c', 'copy', '-movflags', '+faststart', '-y', str(directory / 'transition.mp4'))
    print('Coded-height transition: 240 -> 256 -> 240, visible 320x240 throughout, 36 pictures')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    make_mmco5(args.directory)
    make_transition(args.directory)
