#!/usr/bin/env python3
"""Pack Media Kit test packets; verify YUV/PTS and RGB (NumPy for ten-bit/RGB)."""
import argparse
import json
from pathlib import Path
import struct
import subprocess


def probe(path, packets=False):
    return json.loads(subprocess.check_output([
        'ffprobe', '-v', 'error', '-select_streams', 'v:0', '-show_streams',
        *(['-show_packets'] if packets else []), '-show_data', '-of', 'json', str(path)]))


def unhex(data):
    return bytes.fromhex(''.join(line.split(':', 1)[1].split('  ', 1)[0]
                                for line in data.splitlines() if ':' in line))


def pack(source, output):
    info = probe(source, True)
    stream = info['streams'][0]
    extra = unhex(stream['extradata'])
    packets = info['packets']
    with open(output, 'wb') as f:
        f.write(struct.pack('<5I', 0x314b5055, stream['width'], stream['height'], len(extra), len(packets)))
        f.write(extra)
        for p in packets:
            data = unhex(p['data'])
            assert len(data) == int(p['size']) and 0 < len(data) <= 4 * 1024 * 1024
            pts = round(float(p['pts_time']) * 1000000)
            f.write(struct.pack('<IqI', len(data), pts, 'K' in p['flags']))
            f.write(data)
    print(f'packed {len(packets)} packets, {len(extra)} codec configuration bytes: {output}')


def verify(source, output):
    info = probe(source, True)
    stream = info['streams'][0]
    width, height = stream['width'], stream['height']
    frame_size = width * height * 3 // 2
    reference = subprocess.check_output(['ffmpeg', '-v', 'error', '-i', str(source),
        '-an', '-pix_fmt', 'nv12', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-'])
    p010 = subprocess.check_output(['ffmpeg', '-v', 'error', '-xerror', '-i', str(source),
        '-an', '-pix_fmt', 'p010le', '-fps_mode', 'passthrough', '-f', 'rawvideo', '-'])
    ten_bit = stream.get('pix_fmt') in ('yuv420p10le', 'p010le')
    if ten_bit:
        import numpy as np
        # Explicit eight-bit YUV from this addon uses nearest-code rounding.
        samples = np.frombuffer(p010, dtype='<u2').astype(np.uint32) >> 6
        reference = np.minimum(255, (samples + 2) // 4).astype(np.uint8).tobytes()
    else:
        # The addon's eight-bit-to-P010 contract keeps each code value and
        # pads its low eight bits with zero. libswscale instead replicates
        # the byte across all sixteen bits (including P010's low six bits),
        # so construct this reference from independently decoded NV12.
        promoted = bytearray(len(reference) * 2)
        promoted[1::2] = reference
        p010 = promoted
    times = sorted(round(float(p['pts_time']) * 1000000) for p in info['packets'])
    assert len(reference) == len(times) * frame_size
    assert len(p010) == len(reference) * 2
    first_key = next(round(float(p['pts_time']) * 1000000) for p in info['packets'][1:] if 'K' in p['flags'])
    data = Path(output).read_bytes()
    at = 0
    stages = {}
    compared = 0
    rgb_max_error = 0
    while at < len(data):
        stage, w, h, space, size, pts = struct.unpack_from('<5Iq', data, at)
        at += 28
        assert w == width and h == height and at + size <= len(data)
        index = len(stages.setdefault(stage, []))
        expected_times = [t for t in times if t >= first_key] if stage == 3 else times
        assert index < len(expected_times) and pts == expected_times[index], (stage, index, pts, expected_times[index])
        ref_index = times.index(pts)
        ref = reference[ref_index * frame_size:(ref_index + 1) * frame_size]
        rgb = space == 0x0008
        if space == 0x50303130:
            ref = p010[ref_index * frame_size * 2:(ref_index + 1) * frame_size * 2]
        elif rgb:
            import numpy as np
            values = np.frombuffer(p010[ref_index * frame_size * 2:(ref_index + 1) * frame_size * 2],
                                   dtype='<u2').astype(np.uint32) >> (6 if ten_bit else 8)
            scale, maximum = (4, 1023) if ten_bit else (1, 255)
            full = stream.get('color_range') == 'pc'
            matrix = stream.get('color_space')
            bt709 = matrix == 'bt709' or (matrix in (None, 'unknown', 'unspecified') and height > 576)
            assert matrix in (None, 'unknown', 'unspecified', 'bt709', 'smpte170m', 'bt470bg')
            kr, kb = (.2126, .0722) if bt709 else (.299, .114)
            y = values[:width * height].reshape(height, width).astype(float)
            uv = values[width * height:].reshape(height // 2, width // 2, 2).astype(float)
            uv = np.repeat(np.repeat(uv, 2, axis=0), 2, axis=1)
            y = (y - (0 if full else 16 * scale)) / (maximum if full else 219 * scale)
            u = (uv[:, :, 0] - 128 * scale) / (maximum if full else 224 * scale)
            v = (uv[:, :, 1] - 128 * scale) / (maximum if full else 224 * scale)
            b = y + (2 - 2 * kb) * u
            g = y - 2 * kb * (1 - kb) / (1 - kr - kb) * u - 2 * kr * (1 - kr) / (1 - kr - kb) * v
            r = y + (2 - 2 * kr) * v
            ref = np.floor(np.clip(np.stack((b, g, r, np.ones_like(y)), axis=-1) * 255, 0, 255) + .5).astype(np.uint8).tobytes()
        elif space == 0x49343230:
            luma = width * height
            ref = ref[:luma] + ref[luma::2] + ref[luma+1::2]
        elif space == 0x4000:  # B_YCbCr422: Y0 Cb Y1 Cr
            packed = bytearray(width * height * 2)
            luma = width * height
            for y in range(height):
                row = ref[y * width:(y + 1) * width]
                uv = ref[luma + y // 2 * width:luma + (y // 2 + 1) * width]
                packed[y * width * 2:(y + 1) * width * 2:2] = row
                packed[y * width * 2 + 1:(y + 1) * width * 2:2] = uv
            ref = packed
        else:
            assert space == 0x4e563132, hex(space)
        actual = data[at:at + size]
        assert len(ref) == size
        if rgb:
            error = int(np.abs(np.frombuffer(actual, np.uint8).astype(int)
                               - np.frombuffer(ref, np.uint8).astype(int)).max())
            rgb_max_error = max(rgb_max_error, error)
            assert error <= 1, (stage, index, pts, "RGB error", error)
        else:
            assert actual == ref, (stage, index, pts, "pixel mismatch")
        at += size
        compared += size
        stages[stage].append(pts)
    for stage, actual in stages.items():
        expected_times = [t for t in times if t >= first_key] if stage == 3 else times
        assert actual == expected_times
        print(f'stage {stage}: {len(actual)} pictures, verified pixels and exact presentation timestamps')
    assert stages
    print(f'PASS: {sum(map(len, stages.values()))} pictures, {compared} bytes; exact YUV and timestamps, RGB max error {rgb_max_error}')


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
