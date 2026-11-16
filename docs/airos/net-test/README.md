# Network stack tests (airos-works/haiku #121-#130, #113)

`nettest.c` holds guest-side regression tests for the TCP/IP stack. Most of them
play a TCP peer at 10.9.0.2 behind `tun/0`, so they can send what no
well-behaved stack would: lost segments, duplicate ACKs, window updates,
floods. Every test prints `PASS <name>` or `FAIL <name>: why`.

| Test | Checks |
|---|---|
| `recovery` | one loss is repaired with one retransmission (#125) |
| `sack` | three holes in one window are repaired at once (RFC 6675) |
| `rtoresend` | after a retransmit timeout, a duplicate ACK does not stop the resend until the next timeout (#125) |
| `lostresend` | a segment and its first resend are lost in a bulk transfer: repaired well before the retransmit timeout (#125) |
| `finloss` | the last short segment with the FIN is lost after close(): it is resent (#125; it never was) |
| `tailloss` | the last two segments of a write are repaired by a tail loss probe in well under the 200 ms retransmit timeout (#125) |
| `rttramp` | the peer's ACKs come back later and later (20 to 420 ms) with 64 KB in flight: the round trip estimate follows, no spurious timeouts (#125 item 2) |
| `seqwrap` | a loss just past 2^32 is repaired by fast retransmit, without SACK (#125 item 4; the guest connects from successive ports until its initial sequence number lies 1-3 MB below 2^32) |
| `seqnowrap` | the same loss without the wrap; failed for initial sequence numbers above 2^31 |
| `winupdate` | window updates are not duplicate ACKs |
| `persist` | the persist timer keeps probing a closed window |
| `ooo`, `synflood`, `fragflood`, `udpburst`, `udpempty`, `ipv6ext`, `rfc5961`, `predictable`, `mss12`, `mss1460`, `icmp`, `timewait`, `ioctl`, `priv` | the #126-#130 fixes |
| `sink N` / `[sndbuf B] send ADDR S` | throughput between two guests |
| `[rounds R] sink N` / `[linger S] sendn ADDR MB COUNT` | COUNT uploads of MB each, timed until the receiver closes (or, with `linger`, until a lingering `close()` returns); the sink takes R connections (default 3) |

## Running

`build.sh` cross-compiles it against a configured build directory, and
`mkscript.sh` wraps it and its arguments into a script for the test-script disk.
The QEMU runner is the kperf one (`docs/airos/kernel-perf/qemu-run.sh`): it
boots an image with that disk and the scratch disks its `iotest` launch job
expects.

```
./build.sh
./mkscript.sh full.sh bin/nettest recovery sack rtoresend lostresend finloss tailloss \
	winupdate persist rttramp seqwrap seqnowrap
IMAGE=haiku-nightly.image SCRIPT=full.sh ../kernel-perf/qemu-run.sh nettest
```

## A slow, lossy, distant link without root

`relay.py` connects two guests through QEMU `dgram` netdevs and plays the
link between them: a bottleneck of a given rate with a tail-drop queue, a
one-way delay and random loss in one or both directions (netem without
root; unprivileged user namespaces are not available on the build server).
`pair-lossy.sh` boots a sink and a sender behind it:

```
./pair-lossy.sh <name> <image> "send 10.10.0.1 10 send 10.10.0.1 10" 100 10 0.01 ba 256 1
#                                                      Mbit/s ms loss dir queue-KiB seed
```

It prints the receive rates, the sender's `netstat -s` TCP counters
(retransmissions, retransmit timeouts, fast recoveries) and the relay's
counters. `TRACE=/dev/shm/file` makes the relay log every TCP frame;
`flight.py` and `gaps.py` read such a trace. `SINK="rounds 8 sink 30"`
replaces the sink's arguments.

The relay also takes, in its environment:
- `BURST=after:n:k:rx`: reproducible loss (see `relay.py`).
- `SACKWIN=1`: every ACK carrying SACK blocks advertises a smaller window
  than the one before, as a Linux receiver's does while its out-of-order
  queue fills. A Haiku receiver keeps its window, so without this a sender
  that ignored such ACKs as window updates passed every test here and still
  timed out in half of its uploads to a Linux server (#113).

An upload like the X399's to a server 24 ms away:

```
SACKWIN=1 SINK="rounds 8 sink 30" ./pair-lossy.sh sy <image> "sendn 10.10.0.1 10 8" 200 12 0 ba 128 1
```

Traps:
- Write traces to `/dev/shm`. A trace on a busy disk blocked the relay for
  seconds, and the guests' retransmit timeouts during that time looked like a
  TCP bug.
- Run A/B pairs one after the other. Two pairs at once on a loaded host
  showed timeout storms on whichever pair the scheduler starved.
- The sender's script powers the guest off when it is done; `close()`
  returns with up to 4 MB still queued, so it waits 12 s before `netstat`
  to let the last stream drain.
- A guest can stop for 100-250 ms under load. A receiving guest then sends
  no ACKs and drops part of what arrives in its virtio ring, and the sender's
  retransmit timeout is right: look for a silence in the ACKs of the trace
  before blaming the sender.
- The relay's UDP sockets are capped at `net.core.rmem_max` (about 200 KB), so
  very large bursts also lose frames before the relay sees them.
