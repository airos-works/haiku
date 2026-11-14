#!/usr/bin/env python3
"""relay.py: a lossy, slow, distant link between two QEMU guests (netem without root).

Each guest uses a dgram netdev: local 127.0.0.1:<qa|qb>, remote 127.0.0.1:<la|lb>.
Frames from A go to B and back through a bottleneck of RATE bit/s with a
tail-drop queue of QUEUE bytes, then DELAY ms; LOSS (0..1) drops frames at
random, in the direction(s) LOSSDIR names (ab, ba or both).

usage: relay.py la qa lb qb [rate_mbit delay_ms loss lossdir queue_kb seed]
Prints counters every 5 s and at SIGTERM.
"""
import heapq
import os
import struct
import random
import select
import signal
import socket
import sys
import time

la, qa, lb, qb = (int(x) for x in sys.argv[1:5])
rate = float(sys.argv[5]) * 1e6 if len(sys.argv) > 5 else 100e6
delay = float(sys.argv[6]) / 1000 if len(sys.argv) > 6 else 0.010
loss = float(sys.argv[7]) if len(sys.argv) > 7 else 0.0
lossdir = sys.argv[8] if len(sys.argv) > 8 else "ba"
queue = int(sys.argv[9]) * 1024 if len(sys.argv) > 9 else 256 * 1024
random.seed(int(sys.argv[10]) if len(sys.argv) > 10 else 1)

sa = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sa.bind(("127.0.0.1", la))
sb = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
sb.bind(("127.0.0.1", lb))
for s in (sa, sb):
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8 << 20)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 8 << 20)
    s.setblocking(False)

# direction "ab": received on sa (from guest A), sent from sb to guest B
dirs = {
    sa: ("ab", sb, ("127.0.0.1", qb)),
    sb: ("ba", sa, ("127.0.0.1", qa)),
}
free_at = {"ab": 0.0, "ba": 0.0}
stats = {d: {"in": 0, "out": 0, "lost": 0, "qdrop": 0, "bytes": 0}
         for d in ("ab", "ba")}
heap = []
seq = 0
trace = open(os.environ["TRACE"], "w") if os.environ.get("TRACE") else None


def log_frame(t, d, data, what):
    # Ethernet + IPv4 + TCP: time, direction, event, seq, ack, payload, window
    if trace is None or len(data) < 54 or data[12:14] != b"\x08\x00" \
            or data[23] != 6:
        return
    ihl = (data[14] & 0x0f) * 4
    tl = struct.unpack("!H", data[16:18])[0]
    t0 = 14 + ihl
    sp, dp = struct.unpack("!HH", data[t0:t0 + 4])
    sq, ak = struct.unpack("!II", data[t0 + 4:t0 + 12])
    flags = data[t0 + 13]
    doff = (data[t0 + 12] >> 4) * 4
    win = struct.unpack("!H", data[t0 + 14:t0 + 16])[0]
    trace.write("%.6f %s %s %u %u %d %d %d %d %d\n" % (t, d, what, sq, ak,
        tl - ihl - doff, win, sp, dp, flags))
start = time.monotonic()
last_report = start


def report(tag="NT relay"):
    t = time.monotonic() - start
    parts = []
    for d in ("ab", "ba"):
        s = stats[d]
        parts.append("%s in %d out %d lost %d qdrop %d %.1f Mbit/s" % (
            d, s["in"], s["out"], s["lost"], s["qdrop"],
            s["bytes"] * 8 / max(t, 1e-3) / 1e6))
    print("%s t=%.0f %s" % (tag, t, " | ".join(parts)), flush=True)


def stop(*_):
    report("relay final")
    if trace:
        trace.close()
    sys.exit(0)


signal.signal(signal.SIGTERM, stop)
signal.signal(signal.SIGINT, stop)

while True:
    now = time.monotonic()
    timeout = 0.5
    if heap:
        timeout = max(0.0, min(timeout, heap[0][0] - now))
    readable, _, _ = select.select([sa, sb], [], [], timeout)
    now = time.monotonic()
    for s in readable:
        d, out, dest = dirs[s]
        while True:
            try:
                data = s.recv(65536)
            except BlockingIOError:
                break
            st = stats[d]
            st["in"] += 1
            log_frame(now - start, d, data, "in")
            if loss > 0 and (lossdir == "both" or lossdir == d) \
                    and random.random() < loss:
                st["lost"] += 1
                log_frame(now - start, d, data, "lost")
                continue
            # bottleneck: serialization at rate, tail drop beyond the queue
            begin = max(now, free_at[d])
            if (begin - now) * rate / 8 > queue:
                st["qdrop"] += 1
                continue
            free_at[d] = begin + len(data) * 8 / rate
            seq += 1
            heapq.heappush(heap, (free_at[d] + delay, seq, out, dest, data, d))
    while heap and heap[0][0] <= now:
        _, _, out, dest, data, d = heapq.heappop(heap)
        try:
            out.sendto(data, dest)
            stats[d]["out"] += 1
            stats[d]["bytes"] += len(data)
        except OSError:
            pass
    if now - last_report >= 5:
        last_report = now
        report()
