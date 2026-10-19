#!/usr/bin/env python3
"""Sends a cart to the board over USB, checked first.

    python tools/push.py path/to/CART.lua [--port /dev/ttyACM0] [--no-run]

The cart is checked again (check.py): one that fails never leaves the computer. Its images
(images/CART/ beside it, packed by pack_images.py) go ahead of it. The board keeps both in flash
and opens the cart unless --no-run. The board's clock and timezone are set from this computer's
on the way (the RTC keeps them through power-off).

The link: the console takes the same messages a host could send it any other way, as lines on the
USB serial port. A message goes as `cart msg begin`, its parts `cart msg <offset> <crc32> <base64>`
(four in flight; a part the board finds damaged or out of place is answered "@error at N", and
sending goes back to byte N), then `cart msg end <crc32>`. The log shares the port: only lines
starting with "@" are answers.
"""
from __future__ import annotations

import argparse
import base64
import collections
import glob
import json
import re
import sys
import time
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import check  # noqa: E402
import pack_images  # noqa: E402


def fail(msg: str) -> None:
    print(json.dumps({"ok": False, "error": msg}))
    sys.exit(1)


def local_tz() -> str | None:
    """This computer's timezone as a POSIX TZ rule (the last line of a TZif file, as on Linux and
    macOS), for the board's date()."""
    try:
        tail = Path("/etc/localtime").read_bytes()[-128:].split(b"\n")
        rule = [t for t in tail if t][-1].decode()
        return rule if re.fullmatch(r"[A-Za-z<>+\-0-9,./:]+", rule) else None
    except (OSError, IndexError, UnicodeDecodeError):
        return None


class Link:
    def __init__(self, port: str | None):
        import serial  # pyserial

        port = port or (sorted(glob.glob("/dev/ttyACM*") + glob.glob("/dev/cu.usbmodem*")) or [None])[0]
        if not port:
            fail("no board on USB (/dev/ttyACM*)")
        self.port = port
        self.s = serial.Serial(port, 115200, timeout=0)
        time.sleep(0.2)
        self.s.reset_input_buffer()
        self.pending = b""
        self.answers: collections.deque[str] = collections.deque()
        self.log: list[str] = []  # the log's lines, kept only while keep_log is set
        self.keep_log = False

    def send(self, text: str) -> None:
        self.s.write(text.encode() + b"\n")

    def pump(self) -> None:
        """Reads what has arrived: "@..." lines are answers, the rest is the log."""
        self.pending += self.s.read(65536)
        while b"\n" in self.pending:
            raw, self.pending = self.pending.split(b"\n", 1)
            line = raw.strip().decode(errors="replace")
            if line.startswith("@"):
                self.answers.append(line)
            elif self.keep_log:
                self.log.append(line)

    def answer(self, timeout: float = 3) -> str | None:
        """The next "@..." line from the board, or None."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            self.pump()
            if self.answers:
                return self.answers.popleft()
            time.sleep(0.001)
        return None

    def ask(self, text: str, timeout: float = 3) -> str:
        for _ in range(3):
            self.send(text)
            a = self.answer(timeout)
            if a:
                return a
        fail(f"no answer from the board on {self.port} (is it running the Luanette firmware?)")
        return ""

    def where(self) -> int:
        """How much of the message the board has (after a lost answer): asked with a part that can't fit."""
        time.sleep(0.3)
        self.pump()
        self.answers.clear()
        m = re.match(r"@error at (\d+)", self.ask("cart msg ffffffff 0 AA=="))
        if not m:
            fail("the board lost its place in the message")
        return int(m.group(1))

    def message(self, msg: dict) -> int:
        """One message; returns the times it went back."""
        raw = json.dumps(msg).encode()
        if not self.ask("cart msg begin").startswith("@ok"):
            fail("the board refused a message")
        off, inflight, ignore, back = 0, [], 0, 0
        while off < len(raw) or inflight:
            while off < len(raw) and len(inflight) < 4:  # 4 lines of ~2.8 KB: inside the board's 16 KB buffer
                part = raw[off:off + 2048]
                self.send(f"cart msg {off:x} {zlib.crc32(part):08x} {base64.b64encode(part).decode()}")
                inflight.append(off)
                off += len(part)
            a = self.answer()
            if a is None:  # an answer lost: ask where it stands
                off, inflight, ignore = self.where(), [], 0
                back += 1
                continue
            if ignore:  # answers to parts sent after one that failed
                ignore -= 1
                continue
            inflight.pop(0)
            m = re.match(r"@error at (\d+)", a)
            if m:
                off, ignore, inflight = int(m.group(1)), len(inflight), []
                back += 1
        a = self.ask(f"cart msg end {zlib.crc32(raw):08x}", 10)
        if not a.startswith("@ok"):
            fail(a)
        return back


def messages(path: Path, run: bool = True, images: Path | None = None) -> tuple[list[dict], dict]:
    """A cart as the messages that deliver it (its images first), checked; and what was sent."""
    if path.suffix != ".lua" or not path.exists():
        fail(f"no cart {path}")
    res = check.check(path, 150, None, "pico")
    if not res["ok"]:
        fail(f"{path.name} doesn't pass check.py: " + res.get("error", "?"))
    name = path.stem
    bundle, info = pack_images.pack(name, images or path.parent / "images" / name)
    body = {"type": "cart", "name": name, "title": res.get("title", name), "lua": check.shipped(path.read_text()),
            "run": run}
    msgs = []
    if bundle:  # its images first, in 12 KB parts
        step = 12 * 1024
        for off in range(0, len(bundle), step):
            msgs.append({"type": "cart.images", "name": name, "size": len(bundle), "crc": zlib.crc32(bundle), "off": off,
                         "b64": base64.b64encode(bundle[off:off + step]).decode()})
        body["images"] = {"crc": zlib.crc32(bundle), "size": len(bundle)}
    msgs.append(body)
    out = {"ok": True, "name": name, "title": res.get("title", name)}
    if bundle:
        out["images"] = {"count": len(info), "bytes": len(bundle)}
    return msgs, out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("cart", type=Path)
    ap.add_argument("--port")
    ap.add_argument("--no-run", action="store_true", help="keep it on the board without opening it")
    a = ap.parse_args()
    msgs, out = messages(a.cart, not a.no_run)
    link = Link(a.port)
    link.ask(f"time {int(time.time())}")
    tz = local_tz()
    if tz:
        link.ask(f"tz {tz}")
    out["went_back"] = sum(link.message(m) for m in msgs)
    print(json.dumps(out))


if __name__ == "__main__":
    main()
