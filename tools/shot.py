#!/usr/bin/env python3
"""A screenshot of the board over USB: what the cart on screen shows, as a PNG.

    python tools/shot.py [OUT.png] [--port /dev/ttyACM0]

The board sends the picture in parts, each with its CRC-32; the log shares the USB port, so a part
that comes back damaged is asked for again (`shot <i>`).
"""
import argparse
import base64
import binascii
import glob
import sys
import time
import zlib

import serial  # pyserial
from PIL import Image


def read_answer(s, timeout=20):
    """The "@shot" lines and the closing "@ok"/"@error" line of one answer."""
    lines, buf, deadline = [], b"", time.time() + timeout
    while time.time() < deadline:
        buf += s.read(65536)
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if line.startswith(b"@shot "):
                lines.append(line)
            elif line.startswith(b"@ok") or line.startswith(b"@error"):
                return lines, line.decode(errors="replace")
        time.sleep(0.002)
    return lines, None


def take(lines, parts):
    for line in lines:
        try:
            _, i, crc, data = line.split(b" ", 3)
            raw = base64.b64decode(data, validate=True)
        except (ValueError, binascii.Error):
            continue  # damaged on the way: asked for again below
        if zlib.crc32(raw) == int(crc, 16):
            parts[int(i)] = raw


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("out", nargs="?", default="shot.png")
    ap.add_argument("--port")
    a = ap.parse_args()
    port = a.port or (sorted(glob.glob("/dev/ttyACM*")) or [None])[0]
    if not port:
        sys.exit("no board on USB (/dev/ttyACM*)")
    parts = {}
    with serial.Serial(port, 115200, timeout=0) as s:
        s.reset_input_buffer()
        s.write(b"shot\n")
        lines, end = read_answer(s)
        if not end or end.startswith("@error"):
            sys.exit(end or "no screenshot came back (is it running the Luanette firmware?)")
        w, h, n = (int(v) for v in end.split()[1:4])
        take(lines, parts)
        for i in range(n):  # the damaged ones again
            for _ in range(5):
                if i in parts:
                    break
                s.write(f"shot {i}\n".encode())
                more, _ = read_answer(s, 5)
                take(more, parts)
            if i not in parts:
                sys.exit(f"part {i} kept arriving damaged")
    raw = b"".join(parts[i] for i in range(n))
    img = Image.frombytes("RGB", (w, h), bytes(
        c for k in range(0, len(raw), 2)
        for v in [raw[k] << 8 | raw[k + 1]]
        for c in ((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31)))
    img.save(a.out)
    print(a.out)


if __name__ == "__main__":
    main()
