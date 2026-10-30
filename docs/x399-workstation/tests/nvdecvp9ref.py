#!/usr/bin/env python3
"""What nvdecvp9stream should print for a VP9 stream, from libavcodec.

Usage: nvdecvp9ref.py <stream.webm|.ivf|.mp4> [out.ivf|-] [packet limit]

Prints, for every frame libavcodec's VP9 decoder shows, its size, bit depth
and the CRC-32 (zlib's) of its planes (Y, Cb, Cr, cropped to the frame;
eight-bit frames as yuv420p, ten-bit ones as yuv420p10le), as nvdecvp9stream
does on the workstation. With a second argument the stream's packets are also
written there as IVF, which nvdecvp9stream reads. Needs PyAV (pip install
av), whose libavcodec decodes VP9 as libvpx does.
"""
import struct
import sys
import zlib

import av


def crc(value, data):
    return zlib.crc32(data, value)


def plane_bytes(plane, width, height, bytes_per_sample):
    row = width * bytes_per_sample
    data = bytes(plane)
    return b"".join(data[y * plane.line_size:y * plane.line_size + row]
                    for y in range(height))


def main():
    container = av.open(sys.argv[1])
    stream = container.streams.video[0]
    ivf = (open(sys.argv[2], "wb") if len(sys.argv) > 2 and sys.argv[2] != "-"
           else None)
    limit = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    packets = []
    for packet in container.demux(stream):
        if packet.size:
            packets.append(bytes(packet))
            if limit and len(packets) >= limit:
                break
    if ivf is not None:
        ctx = stream.codec_context
        ivf.write(b"DKIF" + struct.pack("<HHIHHIIII", 0, 32, 0x30395056,
                                         ctx.width, ctx.height, 30, 1,
                                         len(packets), 0))
        for i, data in enumerate(packets):
            ivf.write(struct.pack("<IQ", len(data), i) + data)
        ivf.close()

    decoder = av.CodecContext.create("vp9", "r")
    index = 0

    def show(frame):
        nonlocal index
        ten = frame.format.name.startswith("yuv420p10")
        if frame.format.name not in ("yuv420p", "yuv420p10le"):
            print("frame %d %s" % (index, frame.format.name))
            index += 1
            return
        bytes_per_sample = 2 if ten else 1
        width, height = frame.width, frame.height
        chroma_width, chroma_height = (width + 1) // 2, (height + 1) // 2
        value = 0
        value = crc(value, plane_bytes(frame.planes[0], width, height,
                                       bytes_per_sample))
        value = crc(value, plane_bytes(frame.planes[1], chroma_width,
                                       chroma_height, bytes_per_sample))
        value = crc(value, plane_bytes(frame.planes[2], chroma_width,
                                       chroma_height, bytes_per_sample))
        print("frame %d %dx%d %d %08x" % (index, width, height,
                                           10 if ten else 8, value))
        index += 1

    for data in packets:
        for frame in decoder.decode(av.Packet(data)):
            show(frame)
    for frame in decoder.decode(None):
        show(frame)


if __name__ == "__main__":
    main()
