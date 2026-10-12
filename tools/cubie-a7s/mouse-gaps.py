#!/usr/bin/env python3
"""Mouse report gaps by the LE connection interval in force.

mouse-gaps.py <bluetooth_le.log at level 3> [since HH:MM:SS]

The interval comes from the log itself ("LE link up ... interval", "LE
connection update ... interval"); a Classic inquiry is its own state. Gaps
over 300 ms are the hand resting, and the first half second after a change
is left out. Also lists reconnections: link up, reports enabled, first
report.
"""
import re
import sys

ansi = re.compile(r"\x1b\[[0-9;]*[mK]")
since = None
if len(sys.argv) > 2:
	h, m, s = sys.argv[2].split(":")
	since = int(h) * 3600 + int(m) * 60 + float(s)

reports = []
changes = []  # (time, interval ms)
inquiry = []  # (time, on)
reconnects = []
for line in open(sys.argv[1], errors="replace"):
	line = ansi.sub("", line)
	m = re.match(r"\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d)\.(\d{3}) (.*)", line)
	if not m:
		continue
	t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) \
		+ int(m.group(4)) / 1000
	if since is not None and t < since:
		continue
	text = m.group(5)
	if "mouse: input report" in text:
		reports.append(t)
		continue
	i = re.search(r"LE link up: .*interval ([\d.]+) ms", text)
	if i:
		changes.append((t, float(i.group(1))))
		reconnects.append([t, None, None, None])
		continue
	i = re.search(r"LE connection update on handle \S+: status 0 interval "
		r"([\d.]+) ms", text)
	if i:
		changes.append((t, float(i.group(1))))
		continue
	if "Classic inquiry started" in text:
		inquiry.append((t, True))
	elif "Classic inquiry finished" in text:
		inquiry.append((t, False))
	i = re.search(r"connected; \d+ input report\(s\) enabled (\d+) ms after "
		r"encryption \(([^)]*)\)", text)
	if i and reconnects:
		reconnects[-1][1] = (t, int(i.group(1)), i.group(2))
	i = re.search(r"first report (\d+) ms after encryption", text)
	if i and reconnects:
		reconnects[-1][2] = (t, int(i.group(1)))
	if "encryption on handle" in text and "enabled" in text and reconnects:
		reconnects[-1][3] = t


def state(t):
	interval = None
	changed = -1e9
	for when, value in changes:
		if when > t:
			break
		interval, changed = value, when
	busy = False
	for when, on in inquiry:
		if when > t:
			break
		busy = on
	if t - changed < 0.5:
		return None
	if busy:
		return "Classic inquiry"
	return f"{interval:.2f} ms" if interval is not None else "unknown"


stats = {}
for a, b in zip(reports, reports[1:]):
	gap = (b - a) * 1000
	if gap > 300:
		continue
	name = state(a)
	if name is not None and state(b) == name:
		stats.setdefault(name, []).append(gap)

print(f"{'interval':16} {'gaps':>6} {'moving':>8} {'rate':>6} {'median':>7} "
	f"{'95%':>6} {'max':>6} {'>20ms':>7} {'>30ms':>7} {'>60ms':>7}")
for name in sorted(stats):
	gaps = sorted(stats[name])
	n = len(gaps)
	moving = sum(gaps) / 1000
	over = lambda limit: 100 * sum(1 for g in gaps if g > limit) / n
	print(f"{name:16} {n:6} {moving:7.1f}s {n / moving:5.0f}/s "
		f"{gaps[n // 2]:6.1f}ms {gaps[min(n - 1, int(0.95 * n))]:5.0f}ms "
		f"{gaps[-1]:5.0f}ms {over(20):6.1f}% {over(30):6.1f}% {over(60):6.1f}%")

if reconnects:
	print("\nreconnections (seconds after the link came up):")
	for up, ready, first, encrypted in reconnects:
		clock = "%02d:%02d:%06.3f" % (up // 3600, up % 3600 // 60, up % 60)
		parts = [clock]
		if encrypted:
			parts.append(f"encrypted +{encrypted - up:.2f}")
		if ready:
			parts.append(f"reports enabled +{ready[0] - up:.2f} ({ready[2]})")
		if first:
			parts.append(f"first report +{first[0] - up:.2f}")
		print("  " + ", ".join(parts))
