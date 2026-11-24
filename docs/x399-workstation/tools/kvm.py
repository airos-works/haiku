#!/usr/bin/env python3
"""Small NanoKVM 2.4.3 client for the X399 workstation lab.

Run with ../.venv/bin/python. Credentials are read interactively or from
NANOKVM_PASSWORD; the session cookie is saved privately under /mnt/HaikuWork/state.
"""

import argparse
import getpass
import json
import os
from pathlib import Path
import struct
import subprocess
import time
import urllib.parse
import urllib.request

BASE = os.environ.get("NANOKVM_URL", "http://192.168.1.9").rstrip("/")
SESSION = Path(os.environ.get("NANOKVM_SESSION", "/mnt/HaikuWork/x399/state/nanokvm-session.json"))


def cookie():
    return json.loads(SESSION.read_text())["cookie"]


def api(path, body=None):
    payload = None if body is None else json.dumps(body).encode()
    request = urllib.request.Request(
        BASE + path, data=payload,
        headers={"Cookie": cookie(), "Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=20) as response:
        raw = response.read().decode("utf-8", "replace")
        try:
            result = json.loads(raw)
        except ValueError:
            return {"raw": raw}
    if result.get("code") != 0:
        raise RuntimeError(f"{path}: {result}")
    return result.get("data")


def login(username):
    password = os.environ.get("NANOKVM_PASSWORD") or getpass.getpass("NanoKVM web password: ")
    encrypted = subprocess.run(
        ["openssl", "enc", "-aes-256-cbc", "-md", "md5", "-salt", "-a", "-A",
         "-pass", "pass:nanokvm-sipeed-2024"],
        input=password.encode(), capture_output=True, check=True,
    ).stdout.decode()
    body = json.dumps({"username": username, "password": urllib.parse.quote(encrypted, safe="")}).encode()
    request = urllib.request.Request(BASE + "/api/auth/login", data=body,
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=20) as response:
        raw = response.read().decode("utf-8", "replace")
        try:
            result = json.loads(raw)
        except ValueError:
            return {"raw": raw}
    if result.get("code") != 0:
        raise RuntimeError("NanoKVM login failed")
    token = result["data"]["token"]
    SESSION.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(SESSION, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as output:
        json.dump({"cookie": "nano-kvm-token=" + token}, output)
    return {"authenticated": True}


def screenshot(path):
    request = urllib.request.Request(BASE + "/api/stream/mjpeg", headers={"Cookie": cookie()})
    deadline = time.monotonic() + 15
    with urllib.request.urlopen(request, timeout=10) as response:
        data = b""
        while time.monotonic() < deadline:
            chunk = response.read1(65536)
            if not chunk:
                raise RuntimeError("Video stream ended without a frame")
            data += chunk
            start = data.find(b"\xff\xd8")
            end = data.find(b"\xff\xd9", max(0, start))
            if start >= 0 and end >= 0:
                output = Path(path)
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(data[start:end + 2])
                return {"screenshot": str(output), "bytes": output.stat().st_size}
            if len(data) > 10_000_000:
                raise RuntimeError("No JPEG frame within 10 MB")
    raise TimeoutError("No video frame within 15 seconds")


def send_reports(reports):
    import websocket
    ws = websocket.create_connection(BASE.replace("http", "ws", 1) + "/api/ws",
                                     cookie=cookie(), origin=BASE, timeout=10)
    try:
        for report in reports:
            ws.send_binary(report)
            time.sleep(0.15)
    finally:
        ws.close()


def key(names):
    modifiers = {"CTRL": 1, "SHIFT": 2, "ALT": 4, "SUPER": 8}
    codes = {"ENTER": 40, "ESC": 41, "BACKSPACE": 42, "TAB": 43, "SPACE": 44,
             "DELETE": 76, "HOME": 74, "END": 77, "PAGEUP": 75, "PAGEDOWN": 78, "INSERT": 73, "PRINTSCREEN": 70, "SYSRQ": 70, "SCROLLLOCK": 71, "PAUSE": 72, "RIGHT": 79, "LEFT": 80, "DOWN": 81, "UP": 82}
    codes.update({f"F{i}": 57 + i for i in range(1, 13)})
    codes.update({chr(65 + i): 4 + i for i in range(26)})
    modifier = 0
    keys = []
    for name in names:
        name = name.upper()
        if name in modifiers:
            modifier |= modifiers[name]
        else:
            keys.append(codes[name])
    if len(keys) > 6:
        raise ValueError("At most six non-modifier keys")
    send_reports([bytes([1, modifier, 0] + keys + [0] * (6 - len(keys))), bytes([1] + [0] * 8)])
    return {"keys": names}


def type_text(text):
    reports = []
    table = {' ': 44, '\n': 40, '\t': 43}
    for i, c in enumerate('1234567890'):
        table[c] = 30 + i
    for c, code in zip('-=[]\\;\'`,./', [45, 46, 47, 48, 49, 51, 52, 53, 54, 55, 56]):
        table[c] = code
    shift_map = dict(zip('~!@#$%^&*()_+{}|:"<>?', '`1234567890-=[]\\;\',./'))
    for ch in text:
        mod = 0
        if 'a' <= ch <= 'z':
            code = 4 + ord(ch) - 97
        elif 'A' <= ch <= 'Z':
            code = 4 + ord(ch) - 65; mod = 2
        elif ch in shift_map:
            code = table[shift_map[ch]]; mod = 2
        else:
            code = table[ch]
        reports.append(bytes([1, mod, 0, code, 0, 0, 0, 0, 0]))
        reports.append(bytes([1] + [0] * 8))
    import websocket
    ws = websocket.create_connection(BASE.replace("http", "ws", 1) + "/api/ws",
                                     cookie=cookie(), origin=BASE, timeout=10)
    try:
        for report in reports:
            ws.send_binary(report)
            time.sleep(0.02)
    finally:
        ws.close()
    return {"typed": len(text)}


def click(x, y, width, height, button=1):
    if not (0 <= x < width and 0 <= y < height):
        raise ValueError("Click is outside screen bounds")
    x = round(x / (width - 1) * 32767)
    y = round(y / (height - 1) * 32767)
    reports = [bytes([2]) + struct.pack("<BHHB", b, x, y, 0) for b in (0, button, 0)]
    send_reports(reports)
    return {"clicked": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("login"); p.add_argument("--username", default="admin")
    commands.add_parser("status")
    p = commands.add_parser("screenshot"); p.add_argument("path")
    p = commands.add_parser("key"); p.add_argument("keys", nargs="+")
    p = commands.add_parser("type"); p.add_argument("text")
    p = commands.add_parser("click")
    p.add_argument("x", type=int); p.add_argument("y", type=int)
    p.add_argument("--button", type=int, default=1)
    p.add_argument("--width", type=int, default=1920); p.add_argument("--height", type=int, default=1080)
    p = commands.add_parser("power"); p.add_argument("type", choices=["power", "reset"])
    p.add_argument("--duration", type=int, default=800)
    p = commands.add_parser("mount"); p.add_argument("image")
    commands.add_parser("unmount")
    p.add_argument("--cdrom", action="store_true")
    args = parser.parse_args()
    if args.command == "login": result = login(args.username)
    elif args.command == "status":
        result = {path: api(path) for path in ["/api/vm/hardware", "/api/vm/gpio", "/api/vm/hdmi",
                  "/api/vm/device/virtual", "/api/storage/image", "/api/storage/image/mounted"]}
    elif args.command == "screenshot": result = screenshot(args.path)
    elif args.command == "key": result = key(args.keys)
    elif args.command == "type": result = type_text(args.text.encode().decode("unicode_escape"))
    elif args.command == "click": result = click(args.x, args.y, args.width, args.height, args.button)
    elif args.command == "power":
        if not 1 <= args.duration <= 10000: raise ValueError("Duration must be 1..10000 ms")
        result = api("/api/vm/gpio", {"type": args.type, "duration": args.duration})
    elif args.command == "mount":
        if not args.image.startswith("/data/") or not args.image.lower().endswith((".img", ".iso")):
            raise ValueError("Specify an uploaded /data/*.img or /data/*.iso image")
        result = api("/api/storage/image/mount", {"file": args.image, "cdrom": args.cdrom})
    elif args.command == "unmount":
        result = api("/api/storage/image/mount", {"file": ""})
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
