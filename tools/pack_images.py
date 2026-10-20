#!/usr/bin/env python3
"""Packs a cart's images for the full-color layer (docs/api.md, "hd").

    python tools/pack_images.py path/to/CART.lua [OUT.hdb]

A cart's images live in images/CART/ beside the cart (PNG, JPEG or SVG; subfolders allowed). Each becomes
the board's own format, so the board never decodes PNG or JPEG and a preview draws the bytes the
board gets: RGB565 (with an alpha channel when the image has transparency), run-length
compressed; photos are ordered-dithered to hide RGB565's banding. All of a cart's images go in
one bundle. Its name for an image is the path under the folder without the extension:
images/pet/hat/crown.png is "hat/crown". push.py sends the bundle with the cart; check.py and
cartrun use it for previews. Prints one JSON line (or, imported, pack()).

Formats (engine/hd.cpp reads them):
  image  "HDI1", u16 w, u16 h, u8 flags (1: alpha), 3 reserved; then the pixels row by row as
         tokens: a byte t, t & 0x80 -> one pixel repeated (t & 0x7F) + 1 times, else t + 1
         pixels as they are. A pixel is u16 RGB565 (little-endian), then u8 alpha if flags & 1.
  bundle "HDB1", u32 count; per image u8 name length, the name, u32 offset (from the bundle's
         start), u32 length; then the images.
"""
from __future__ import annotations

import io
import json
import re
import struct
import subprocess
import sys
import zlib
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
MAX_SIDE = 384           # the square canvas
MAX_W, MAX_H = 410, 502  # the whole glass (a "-- canvas: full" cart's)
MAX_BUNDLE = 512 * 1024  # a cart's images, packed
MAX_NAME = 47
NAME_RE = re.compile(r"^[A-Za-z0-9_./-]+$")
EXTS = {".png", ".jpg", ".jpeg", ".svg"}
BAYER4 = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]


class PackError(ValueError):
    pass


def load(path: Path) -> Image.Image:
    """The image as RGBA (SVG through ImageMagick, at its own size)."""
    if path.suffix.lower() == ".svg":
        png = subprocess.run(["magick", "-background", "none", str(path), "png:-"], capture_output=True, check=False)
        if png.returncode != 0 or not png.stdout:
            raise PackError(f"{path.name}: ImageMagick couldn't draw it ({png.stderr.decode(errors='replace').strip()[:120]})")
        return Image.open(io.BytesIO(png.stdout)).convert("RGBA")
    return Image.open(path).convert("RGBA")


def to565(img: Image.Image, dither: bool) -> tuple[list[int], list[int]]:
    """Each pixel's RGB565 and alpha. With dither, a 4x4 ordered dither spreads each channel's
    rounding (hides banding in photos); without, the nearest color (keeps flat art flat)."""
    w, h = img.size
    px = img.load()
    cols, alphas = [], []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            if a == 0:
                cols.append(0)  # its color never shows: 0 keeps the runs long
                alphas.append(0)
                continue
            if dither:
                t = (BAYER4[y & 3][x & 3] + 0.5) / 16
                r5, g6, b5 = (min(31, int(r * 31 / 255 + t)), min(63, int(g * 63 / 255 + t)), min(31, int(b * 31 / 255 + t)))
            else:
                r5, g6, b5 = (r * 31 + 127) // 255, (g * 63 + 127) // 255, (b * 31 + 127) // 255
            cols.append(r5 << 11 | g6 << 5 | b5)
            alphas.append(a)
    return cols, alphas


def fit(img: Image.Image, side: int = MAX_SIDE) -> Image.Image:
    """Shrunk (never grown) to fit side x side, its proportions kept."""
    if img.width <= side and img.height <= side:
        return img
    s = side / max(img.width, img.height)
    return img.resize((max(1, round(img.width * s)), max(1, round(img.height * s))), Image.LANCZOS)


def encode(img: Image.Image) -> bytes:
    """One image in the board's format."""
    w, h = img.size
    if w > MAX_W or h > MAX_H:
        raise PackError(f"{w}x{h} is bigger than the glass ({MAX_W}x{MAX_H}); make it smaller first")
    alpha = img.getchannel("A").getextrema()[0] < 255
    # Photos (many colors) dithered; flat art (logos, icons, pixel art) as it is.
    dither = len(img.convert("RGB").getcolors(4096) or ()) == 0
    cols, alphas = to565(img, dither)
    pix = [(c, a) for c, a in zip(cols, alphas)] if alpha else [(c, 255) for c in cols]

    def one(p):
        return struct.pack("<HB", *p) if alpha else struct.pack("<H", p[0])

    out = bytearray(b"HDI1" + struct.pack("<HHB3x", w, h, 1 if alpha else 0))
    i, n = 0, len(pix)
    while i < n:
        j = i + 1
        while j < n and j - i < 128 and pix[j] == pix[i]:
            j += 1
        if j - i >= 2:  # a run
            out.append(0x80 | (j - i - 1))
            out += one(pix[i])
            i = j
            continue
        j = i + 1  # literals, until a run of 2 starts
        while j < n and j - i < 128 and not (j + 1 < n and pix[j] == pix[j + 1]):
            j += 1
        out.append(j - i - 1)
        for p in pix[i:j]:
            out += one(p)
        i = j
    return bytes(out)


def pack(cart: str, folder: Path | None = None) -> tuple[bytes, list[dict]]:
    """The cart's bundle (b"" when it has no images) and what's in it."""
    folder = folder or Path.cwd() / "images" / cart
    if not folder.is_dir():
        return b"", []
    files = sorted(p for p in folder.rglob("*") if p.is_file() and p.suffix.lower() in EXTS)
    names, blobs, info = [], [], []
    for p in files:
        name = p.relative_to(folder).with_suffix("").as_posix()
        if not NAME_RE.match(name) or len(name) > MAX_NAME:
            raise PackError(f"{p.name}: an image's name is letters, digits and _ . / - , at most {MAX_NAME} ({name!r})")
        if name in names:
            raise PackError(f"two images are both called {name!r} (only the extension differs)")
        img = load(p)
        try:
            blob = encode(img)
        except PackError as e:
            raise PackError(f"{name}: {e}") from None
        names.append(name)
        blobs.append(blob)
        info.append({"name": name, "w": img.width, "h": img.height, "bytes": len(blob),
                     "alpha": bool(blob[8] & 1)})
    if not names:
        return b"", []
    head = len(b"HDB1") + 4 + sum(1 + len(n.encode()) + 8 for n in names)
    index, off = bytearray(), head
    for n, b in zip(names, blobs):
        index += bytes([len(n.encode())]) + n.encode() + struct.pack("<II", off, len(b))
        off += len(b)
    bundle = b"HDB1" + struct.pack("<I", len(names)) + bytes(index) + b"".join(blobs)
    if len(bundle) > MAX_BUNDLE:
        raise PackError(f"the images pack to {len(bundle) // 1024} KB; a cart's limit is {MAX_BUNDLE // 1024} KB")
    return bundle, info


def read_bundle(bundle: bytes) -> dict[str, tuple[int, int]]:
    """name -> (offset, length): what check.py and the tests read back."""
    if not bundle:
        return {}
    if bundle[:4] != b"HDB1":
        raise PackError("not an image bundle")
    (count,) = struct.unpack_from("<I", bundle, 4)
    out, i = {}, 8
    for _ in range(count):
        n = bundle[i]
        name = bundle[i + 1:i + 1 + n].decode()
        off, ln = struct.unpack_from("<II", bundle, i + 1 + n)
        out[name] = (off, ln)
        i += 1 + n + 8
    return out


def main() -> None:
    cart = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(".")
    try:
        bundle, info = pack(cart.stem, cart.parent / "images" / cart.stem)
    except PackError as e:
        print(json.dumps({"ok": False, "error": str(e)}))
        sys.exit(1)
    if len(sys.argv) > 2 and bundle:
        Path(sys.argv[2]).write_bytes(bundle)
    print(json.dumps({"ok": True, "images": info, "bytes": len(bundle), "crc": zlib.crc32(bundle)}))


if __name__ == "__main__":
    main()
