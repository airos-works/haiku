#!/usr/bin/env python3
# Copyright 2026, air/OS. Distributed under the terms of the MIT License.
"""Decode the AMD fixture with FFmpeg and compare every active pixel and pad byte.

The libdrm UVD fixture contains IDR slice NALs without SPS/PPS. Reconstruct
those headers from its ruvd_h264 fields (Mesa ac_uvd_dec.h/radeon_uvd.c).
This is an independent software decode, not an expected image made by UVD.
Generated media stays in the caller-specified evidence directory.
"""
import argparse
import hashlib
from pathlib import Path
import re
import struct
import subprocess


class Bits:
    def __init__(self):
        self.bits = ""

    def u(self, value, count):
        assert 0 <= value < 1 << count
        self.bits += format(value, f"0{count}b")
        return self

    def ue(self, value):
        encoded = format(value + 1, "b")
        self.bits += "0" * (len(encoded) - 1) + encoded
        return self

    def se(self, value):
        return self.ue(-2 * value if value <= 0 else 2 * value - 1)

    def nal(self, header):
        bits = self.bits + "1"  # rbsp_stop_one_bit
        bits += "0" * (-len(bits) % 8)
        data = bytes(int(bits[i:i + 8], 2) for i in range(0, len(bits), 8))
        result, zeros = bytearray([header]), 0
        for value in data:
            if zeros >= 2 and value <= 3:
                result.append(3)  # emulation prevention
                zeros = 0
            result.append(value)
            zeros = zeros + 1 if value == 0 else 0
        return b"\0\0\0\1" + result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--decoded", type=Path)
    parser.add_argument("--linear-output", type=Path)
    parser.add_argument("--progressive", action="store_true",
                        help="compare the client API's linear progressive NV12 output")
    args = parser.parse_args()
    fixture = (Path(__file__).resolve().parents[5]
               / "add-ons/kernel/drivers/graphics/amdgpu/UvdFixture.h").read_text()

    def array(name):
        body = re.search(r"\b" + name + r"\[\]\s*=\s*\{(.*?)\};", fixture, re.S)[1]
        return bytes(int(value, 16) for value in re.findall(r"0x([a-fA-F0-9]+)", body))

    avc, message = array("avc_decode_msg"), array("uvd_decode_msg")
    # Require the exact descriptor before using the explicit header encoding.
    assert struct.unpack_from("<9I", avc) == (
        2, 30, 0x85, 0x88, 0x01000001, 0x00020300, 2, 0, 0)
    assert struct.unpack_from("<2I", message, 0x18) == (864, 480)
    assert struct.unpack_from("<8I", message, 0x70) == (
        1024, 0, 0, 1, 0, 0x3c000, 0x78000, 0x96000)
    # dt_field_mode=1: separate top/bottom field surfaces, as in Mesa
    # r600_uvd.c/ruvd_set_dt_surfaces. This describes output storage even
    # though the coded H.264 picture has frame_mbs_only_flag=1.
    assert avc[36:36 + 224] == bytes([16]) * 224  # flat scaling lists
    sps = Bits().u(100, 8).u(0, 8).u(30, 8).ue(0)  # High, level 3.0, SPS 0
    sps.ue(1).ue(0).ue(0).u(0, 1).u(0, 1)  # 4:2:0, 8-bit, no scaling matrix
    sps.ue(1).ue(0).ue(3).ue(2).u(0, 1)  # 5-bit frame_num, POC type 0, 7-bit POC
    sps.ue(53).ue(29).u(1, 1).u(1, 1).u(0, 1).u(0, 1)  # 54x30 MBs, progressive
    pps = Bits().ue(0).ue(0).u(0, 1).u(1, 1).ue(0)  # CAVLC, bottom POC present
    pps.ue(0).ue(0).u(0, 1).u(0, 2).se(2).se(0).se(0)
    pps.u(1, 1).u(0, 1).u(0, 1).u(0, 1).u(0, 1).se(0)
    args.directory.mkdir(parents=True, exist_ok=True)
    stream = args.directory / "uvd-reference.h264"
    reference = args.directory / "uvd-reference.nv12"
    stream.write_bytes(sps.nal(0x67) + pps.nal(0x68) + array("uvd_bitstream"))
    subprocess.run(["ffmpeg", "-y", "-v", "error", "-xerror", "-i", str(stream),
                    "-frames:v", "1", "-pix_fmt", "nv12", "-f", "rawvideo",
                    str(reference)], check=True)
    expected = reference.read_bytes()
    assert len(expected) == 864 * 480 * 3 // 2
    assert sum(expected) == 0x20345d8
    print("Software reference:", len(expected), "bytes, SHA256", hashlib.sha256(expected).hexdigest())
    if args.decoded:
        actual = args.decoded.read_bytes()
        assert len(actual) == 1024 * 480 * 3 // 2
        active_errors = padding_errors = 0
        linear = bytearray()
        planes = ((0, 0x3c000, 480), (0x78000, 0x96000, 240))
        if args.progressive:
            planes = ((0, 0, 480), (1024 * 480, 1024 * 480, 240))
        for top, bottom, rows in planes:
            for row in range(rows):
                offset = (top + row * 1024 if args.progressive else
                          (bottom if row & 1 else top) + (row // 2) * 1024)
                linear.extend(actual[offset:offset + 864])
                padding_errors += sum(value != 0 for value in actual[offset + 864:offset + 1024])
        active_errors = sum(a != b for a, b in zip(linear, expected))
        print("Compared 622080 active bytes and 115200 padding bytes:",
              active_errors, "pixel mismatches,", padding_errors, "padding mismatches")
        assert active_errors == padding_errors == 0
        print("PASS: every decoded NV12 byte matches independent software reference and zero padding")
        if args.linear_output:
            args.linear_output.write_bytes(linear)


if __name__ == "__main__":
    main()
