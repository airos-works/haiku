#!/usr/bin/env python3
"""Linux side of docs/x399-workstation/tests/tcpbench.cpp.

tcpbench-peer.py send <host> <port> <MiB>   send to a tcpbench sink
tcpbench-peer.py sink <port>                receive from tcpbench send
tcpbench-peer.py udpsend <host> <port> <s>  UDP datagrams for tcpbench udpsink
"""
import socket
import sys
import time


def report(what, nbytes, took):
	print(f"{what} {nbytes / 1048576:.1f} MiB in {took:.2f} s: "
		f"{nbytes / 1048576 / took:.1f} MiB/s ({nbytes * 8 / took / 1e6:.0f} Mbit/s)")


def main():
	if len(sys.argv) >= 5 and sys.argv[1] == "send":
		host, port, mib = sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
		sock = socket.create_connection((host, port))
		block = b"\x5a" * (256 * 1024)
		total = mib * 1048576
		sent = 0
		start = time.monotonic()
		while sent < total:
			sock.sendall(block)
			sent += len(block)
		sock.shutdown(socket.SHUT_WR)
		sock.recv(1)
		report("sent", sent, time.monotonic() - start)
	elif len(sys.argv) >= 5 and sys.argv[1] == "udpsend":
		# udpsend <host> <port> <seconds>: 1472-byte datagrams as fast as
		# the host sends them (one full Ethernet frame each)
		host, port, seconds = sys.argv[2], int(sys.argv[3]), float(sys.argv[4])
		sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
		sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 << 20)
		block = b"\x5a" * 1472
		count = 0
		start = time.monotonic()
		while time.monotonic() - start < seconds:
			for _ in range(256):
				try:
					sock.sendto(block, (host, port))
					count += 1
				except BlockingIOError:
					pass
		report("sent", count * 1472, time.monotonic() - start)
	elif len(sys.argv) >= 3 and sys.argv[1] == "sink":
		listener = socket.socket()
		listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
		listener.bind(("0.0.0.0", int(sys.argv[2])))
		listener.listen(1)
		conn, _ = listener.accept()
		total = 0
		start = time.monotonic()
		while True:
			data = conn.recv(1 << 20)
			if not data:
				break
			total += len(data)
		report("received", total, time.monotonic() - start)
	else:
		print(__doc__)
		return 1
	return 0


if __name__ == "__main__":
	sys.exit(main())
