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
# BURST=after:n:k:rx drops, in LOSSDIR, every k-th data frame of the n that
# follow AFTER seconds past the first data frame (k=1: n in a row), and the
# first resend of each of the next RX sequences resent from then on:
# reproducible loss patterns.
burst = [float(x) for x in os.environ.get("BURST", "").split(":") if x]
burst_first = None
burst_index = 0
burst_rx = 0
burst_high = {}
burst_resent = set()
# SACKWIN=1 shrinks the window of every ACK that carries SACK blocks by one
# more unit than the one before, as a Linux receiver's does while its
# out-of-order queue fills: such ACKs are window changes, not plain
# duplicates (RFC 5681), which a sender must still count (RFC 6675).
sackwin = os.environ.get("SACKWIN", "") not in ("", "0")
sackwin_run = 0
# REORDER=p:k delays a data frame in LOSSDIR, with probability p, by k
# frame times at the bottleneck rate: it arrives after the k frames sent
# behind it, as on paths that spread a flow over several links or queues.
reorder = [float(x) for x in os.environ.get("REORDER", "").split(":") if x]
# JITTER=amp_ms:period_ms adds to the delay, in both directions, an offset
# drawn anew from 0..amp every period; frames still leave in order. Paths
# whose round trip wanders by a few ms ended slow start early (HyStart).
jitter = [float(x) for x in os.environ.get("JITTER", "").split(":") if x]
jitter_offset = 0.0
jitter_next = 0.0
last_due = {"ab": 0.0, "ba": 0.0}

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
    # SACK blocks (option 5) as left-right pairs after the flags
    sacks = []
    i = t0 + 20
    while i < t0 + doff and i < len(data):
        kind = data[i]
        if kind == 0:
            break
        if kind == 1:
            i += 1
            continue
        if i + 1 >= len(data) or data[i + 1] < 2:
            break
        if kind == 5:
            for j in range(i + 2, i + data[i + 1] - 7, 8):
                sacks.append("%u-%u" % struct.unpack("!II", data[j:j + 8]))
        i += data[i + 1]
    trace.write("%.6f %s %s %u %u %d %d %d %d %d%s\n" % (t, d, what, sq, ak,
        tl - ihl - doff, win, sp, dp, flags,
        "".join(" " + s for s in sacks)))
start = time.monotonic()
last_report = start


def report(tag="NT relay"):
    t = time.monotonic() - start
    parts = []
    for d in ("ab", "ba"):
        s = stats[d]
        parts.append("%s in %d out %d lost %d qdrop %d reord %d %.1f Mbit/s" % (
            d, s["in"], s["out"], s["lost"], s["qdrop"], s.get("reordered", 0),
            s["bytes"] * 8 / max(t, 1e-3) / 1e6))
    print("%s t=%.0f %s" % (tag, t, " | ".join(parts)), flush=True)


def stop(*_):
    report("relay final")
    if trace:
        trace.close()
    sys.exit(0)


def _sack_window(data):
    """Returns data with the window shrunk if it is a TCP ACK with SACK."""
    global sackwin_run
    if len(data) < 54 or data[12:14] != b"\x08\x00" or data[23] != 6:
        return data
    t0 = 14 + (data[14] & 0x0f) * 4
    doff = (data[t0 + 12] >> 4) * 4
    i = t0 + 20
    has_sack = False
    while i < t0 + doff and i < len(data):
        kind = data[i]
        if kind == 0:
            break
        if kind == 1:
            i += 1
            continue
        if i + 1 >= len(data) or data[i + 1] < 2:
            break
        if kind == 5:
            has_sack = True
        i += data[i + 1]
    if not has_sack:
        sackwin_run = 0
        return data
    sackwin_run += 1
    old = struct.unpack("!H", data[t0 + 14:t0 + 16])[0]
    new = max(old - min(sackwin_run, 4096), 1)
    # incremental checksum update (RFC 1624)
    c = struct.unpack("!H", data[t0 + 16:t0 + 18])[0]
    x = (~c & 0xffff) + (~old & 0xffff) + new
    x = (x & 0xffff) + (x >> 16)
    x = (x & 0xffff) + (x >> 16)
    b = bytearray(data)
    b[t0 + 14:t0 + 16] = struct.pack("!H", new)
    b[t0 + 16:t0 + 18] = struct.pack("!H", ~x & 0xffff)
    return bytes(b)


def _burst_drop(now, d, data):
    global burst_first, burst_index, burst_rx
    if len(data) < 100 or data[12:14] != b"\x08\x00" or data[23] != 6:
        return False
    t0 = 14 + (data[14] & 0x0f) * 4
    sq = struct.unpack("!I", data[t0 + 4:t0 + 8])[0]
    resend = d in burst_high and ((sq - burst_high[d]) & 0xffffffff) >= 1 << 31
    if not resend:
        burst_high[d] = sq
    if burst_first is None:
        burst_first = now
    if now - burst_first < burst[0]:
        return False
    if resend:
        # each sequence's first resend only
        if len(burst) > 3 and burst_rx < burst[3] and sq not in burst_resent:
            burst_resent.add(sq)
            burst_rx += 1
            return True
        return False
    burst_index += 1
    if burst_index > burst[1]:
        return False
    k = int(burst[2]) if len(burst) > 2 else 1
    return burst_index % k == 0


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
            if sackwin:
                data = _sack_window(data)
            st = stats[d]
            st["in"] += 1
            log_frame(now - start, d, data, "in")
            if loss > 0 and (lossdir == "both" or lossdir == d) \
                    and random.random() < loss:
                st["lost"] += 1
                log_frame(now - start, d, data, "lost")
                continue
            if burst and (lossdir == "both" or lossdir == d) \
                    and _burst_drop(now, d, data):
                st["lost"] += 1
                log_frame(now - start, d, data, "lost")
                continue
            # bottleneck: serialization at rate, tail drop beyond the queue
            begin = max(now, free_at[d])
            if (begin - now) * rate / 8 > queue:
                st["qdrop"] += 1
                log_frame(now - start, d, data, "qdrop")
                continue
            free_at[d] = begin + len(data) * 8 / rate
            seq += 1
            extra = 0.0
            if reorder and (lossdir == "both" or lossdir == d) \
                    and len(data) > 100 and random.random() < reorder[0]:
                extra = reorder[1] * 1514 * 8 / rate
                stats[d]["reordered"] = stats[d].get("reordered", 0) + 1
            due = free_at[d] + delay + extra
            if jitter:
                if now >= jitter_next:
                    jitter_offset = random.uniform(0, jitter[0] / 1000)
                    jitter_next = now + jitter[1] / 1000
                due += jitter_offset
                if extra == 0.0:
                    due = max(due, last_due[d])
                    last_due[d] = due
            heapq.heappush(heap, (due, seq, out, dest, data, d))
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
