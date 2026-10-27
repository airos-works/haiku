#!/usr/bin/env python3
"""Keep a permanent log of the Cubie A7S serial console.

Runs `cat` on the NanoKVM's UART over ssh and appends everything to
<log>.raw (bytes as received) and <log> (one line per serial line, each
prefixed with the host's UTC time). Reconnects when ssh drops. Being the
only reader of the UART, it must be the one source of serial output:
other tools read its log instead of the device. Writing to the UART
(serial-send.sh) does not disturb it.

    serial-logger.py <log> [tty]
"""

import datetime
import os
import subprocess
import sys
import time

LOG = sys.argv[1]
TTY = sys.argv[2] if len(sys.argv) > 2 else "/dev/ttyS1"
SSH = ["ssh", "-F", os.environ.get("NANOKVM_SSH_CONFIG", "/mnt/HaikuWork/nanokvm/.ssh/config"),
       "-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=3", "nanokvm"]
REMOTE = (f"stty -F {TTY} 115200 raw -echo -crtscts clocal && "
          f"for p in $(pidof cat); do grep -qs {TTY} /proc/$p/cmdline && kill $p; done; "
          f"exec cat {TTY}")


def stamp():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%H:%M:%S.%f")[:-3]


def main():
    raw = open(LOG + ".raw", "ab", buffering=0)
    text = open(LOG, "a", buffering=1, errors="replace")
    pending = b""
    while True:
        proc = subprocess.Popen(SSH + [REMOTE], stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)
        text.write(f"{stamp()} [logger] connected\n")
        while True:
            chunk = os.read(proc.stdout.fileno(), 4096)
            if not chunk:
                break
            raw.write(chunk)
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                line = line.rstrip(b"\r").replace(b"\x00", b"")
                text.write(f"{stamp()} {line.decode('utf-8', 'replace')}\n")
        proc.wait()
        text.write(f"{stamp()} [logger] ssh ended ({proc.returncode}), reconnecting\n")
        time.sleep(3)


if __name__ == "__main__":
    main()
