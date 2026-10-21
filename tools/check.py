#!/usr/bin/env python3
"""Runs a cart on this computer, against docs/api.md, before it goes anywhere near the board.

    python tools/check.py CART.lua [--frames N] [--preview OUT.png] [--theme pico|phosphor]

Needs lupa built with 32-bit numbers, as the board's Lua is (tools/build_lupa32.py), Pillow, and
for 3D carts' previews the desktop runtime (host/: cmake -S host -B host/build && cmake --build
host/build).

Loads the cart in a sandbox like the board's (no io/os/require/load/debug), runs _init and then
N frames of _update/_draw (150 = 5 s) with scripted input (buttons, tilt, a touch), and enforces
the limits: 32 KB of source, 30,000 Lua instructions a frame, 1 MB of memory, the header comments.
Prints one JSON line: {"ok": true|false, "error": ..., "max_ops": ..., "calls": {...},
"preview": path}. The preview is three frames (start, middle, end) at the board's 3x scale.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from lupa import lua54
from PIL import Image

import pack_images
from cartsrc import shipped  # the cart as it ships: comments out

W = H = 128
SCALE = 3
MAX_SOURCE = 32 * 1024
BUDGET = 30_000
MAX_SAVE = 2048
API_VERSION = 4  # engine/cart.hpp kApiVersion: 1 before the full-color layer, 2 with it, 3 with sound and music, 4 with log() and spaced sprites
# The board's runtime built for this computer (engine/host/cartrun.cpp): a 3D cart's preview
# is drawn by it (the stage's own code), and it must agree that the cart runs.
ROOT = Path(__file__).resolve().parents[1]
CARTRUN = Path(os.environ.get("CARTRUN", ROOT / "host/build/cartrun"))
# The full-color layer's glyph tables (engine/hd_fonts.cpp): hd.measure and
# hd.wrap here give the board's numbers.
HD_FONTS = Path(os.environ.get("HD_FONTS", ROOT / "engine/hd_fonts.cpp"))
UI_LUA = HD_FONTS.parent / "ui.lua"  # the helpers the runtime loads before every cart (the same file)
HD_W = HD_H = 384                 # the square canvas (the window)
GLASS_W, GLASS_H, WINDOW_X, WINDOW_Y, CORNER = 410, 502, 12, 60, 100  # the glass: a "-- canvas: full" cart's
# Live images (the host sends them while a cart runs: a cover, a generated picture): previews use
# the samples in samples/live/, by the same names ("file/hello").
LIVE_DIR = Path(__file__).resolve().parent / "samples" / "live"
_LIVE = None


def live_samples() -> tuple[bytes, dict[str, dict]]:
    global _LIVE
    if _LIVE is None:
        bundle, info = pack_images.pack("", LIVE_DIR)
        _LIVE = (bundle, {i["name"]: i for i in info})
    return _LIVE
# What the layer's work costs on the board (measured 2026-10-14, the hb_*.lua benches): ns a pixel
# filled opaque / blended / measured at an edge (circles, lines, glyphs), and us a changed row at
# the handoff (publish()). The estimate warns a cart that would miss the board's frames.
HD_NS = {"fill": 62, "blend": 140, "edge": 230, "image": 80, "image_alpha": 200, "scroll": 150}
HD_ROW_US = 16  # each row drawn on in a frame: the runtime reads it once after the frame (sent only if it changed)
HD_WARN_MS = 25
PICO = [0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
        0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA]
ACTS = {"open", "volume", "brightness"}  # act()'s commands: Luanette's firmware's (console.cpp, accepts_command)
SAMPLE = {
    # What the console gives data(): the launcher's list, the battery and the settings.
    "carts": [{"name": "lockload", "title": "Lock & Load", "about": "a light-gun rail shooter in 3 stages"},
              {"name": "bubbles", "title": "Bubbles", "about": "classic bubble shooter: drag to aim, release to fire"},
              {"name": "mochi", "title": "Mochi", "about": "a little pet that lives in real time"},
              {"name": "night_swarm", "title": "Night Swarm", "about": "survive the swarm"},
              {"name": "dice", "title": "Dice", "about": "shake the board or tap to throw"}],
    "battery": {"mv": 3912, "percent": 82, "charging": False, "usb": False},
    "settings": {"volume": 2, "brightness": 3},
}

# The 3x5 font (4x6 cells). Missing glyphs draw as a box; lowercase shows as capitals.
FONT = {
    "0": "111101101101111", "1": "010110010010111", "2": "111001111100111", "3": "111001111001111",
    "4": "101101111001001", "5": "111100111001111", "6": "111100111101111", "7": "111001001010010",
    "8": "111101111101111", "9": "111101111001111", "A": "010101111101101", "B": "110101110101110",
    "C": "011100100100011", "D": "110101101101110", "E": "111100110100111", "F": "111100110100100",
    "G": "011100101101011", "H": "101101111101101", "I": "111010010010111", "J": "001001001101010",
    "K": "101101110101101", "L": "100100100100111", "M": "101111111101101", "N": "110101101101101",
    "O": "010101101101010", "P": "110101110100100", "Q": "010101101110011", "R": "110101110101101",
    "S": "011100010001110", "T": "111010010010010", "U": "101101101101011", "V": "101101101010010",
    "W": "101101111111101", "X": "101101010101101", "Y": "101101010010010", "Z": "111001010100111",
    " ": "000000000000000", ".": "000000000000010", ",": "000000000010100", ":": "000010000010000",
    "!": "010010010000010", "?": "111001010000010", "-": "000000111000000", "+": "000010111010000",
    "/": "001001010100100", "%": "101001010100101", "(": "010100100100010", ")": "010001001001010",
    "=": "000111000111000", "'": "010010000000000", '"': "101101000000000", ">": "100010001010100",
    "<": "001010100010001", "*": "101010101000000", "#": "101111101111101", "_": "000000000000111",
    ";": "000010000010100", "[": "110100100100110", "]": "011001001001011", "$": "011110010011110", "&": "010101010101011",
}


class Cart:
    def __init__(self, lua, fps: int):
        self.lua, self.fps = lua, fps
        # fps(): the rate (fps, as the runtime's Cart::fps()), lazy's two rates, and the clock: frame
        # n's time is base_us + (n - base_n) frames at clock_rate, as cartrun counts it
        self.active, self.idle, self.hot_until = fps, 0, 0
        self.base_us, self.base_n, self.clock_rate = 0, 0, fps
        self.fb = bytearray(W * H)
        self.reset_draw_state()
        self.frame = 0
        self.held = [False] * 6
        self.was = [False] * 6
        self.touch_at = None
        self.tilt_at = (0.0, 0.0)
        self.saved = None
        self.wall0 = int(time.time())
        self.rgb = list(PICO)
        self.calls = {"buzz": 0, "sfx": 0, "music": 0, "wake": 0, "act": [], "save": 0, "data": set()}
        self.logs: list[str] = []      # log() lines (the first 200), as the runtime gives them
        self.spaced_sprites = False    # a sprite string with spaces in it (API 4)
        self.log_frame, self.log_count = -1, 0
        self.scene = None  # a 3D cart's, once it makes anything 3D
        self.hd = None  # the full-color layer's calls, once the cart draws on it
        self.hd_w, self.hd_h = HD_W, HD_H  # the canvas: the window, or the glass ("-- canvas: full")
        self.full = False
        self.hd_clipr = (0, 0, HD_W, HD_H)  # x0, y0, x1, y1
        self.hd_scan = True  # hd.scanlines
        self.hd_us = 0.0       # this frame's estimated device cost of drawing on the layer
        self.hd_rows = set()   # the canvas rows drawn on this frame
        self.hd_inked = set()  # rows drawn on since cleared (what clear() has to wipe)
        self.cart_name = ""
        self.images: dict[str, dict] = {}  # name -> pack_images.py's info (w, h, bytes, alpha)
        self.images_used: set[str] = set()
        self.warnings: list[str] = []
        self.env = None
        self.rng = random.Random(1)
        self.data_tables = {}

    def reset_draw_state(self):
        self.color = 7
        self.pmap = list(range(16))
        self.cam = (0, 0)
        self.clipr = (0, 0, W, H)

    # -- helpers --
    def _c(self, c):
        if c is None:
            return self.color
        c = int(math.floor(c)) & 15
        self.color = c
        return c

    def _px(self, x, y, c):
        x, y = int(math.floor(x)) - self.cam[0], int(math.floor(y)) - self.cam[1]
        cx, cy, cw, ch = self.clipr
        if cx <= x < cx + cw and cy <= y < cy + ch and 0 <= x < W and 0 <= y < H:
            self.fb[y * W + x] = self.pmap[c]

    # -- drawing --
    def cls(self, c=None):
        c = 0 if c is None else int(c) & 15
        self.fb[:] = bytes([c]) * (W * H)

    def pset(self, x, y, c=None):
        self._px(x, y, self._c(c))

    def pget(self, x, y):
        x, y = int(math.floor(x)), int(math.floor(y))
        return self.fb[y * W + x] if 0 <= x < W and 0 <= y < H else 0

    def line(self, x0, y0, x1, y1, c=None):
        c = self._c(c)
        x0, y0, x1, y1 = (int(math.floor(v)) for v in (x0, y0, x1, y1))
        dx, dy = abs(x1 - x0), -abs(y1 - y0)
        sx, sy = (1 if x0 < x1 else -1), (1 if y0 < y1 else -1)
        cx, cy, cw, ch = self.clipr
        ax0, ax1, ay0, ay1 = x0 - self.cam[0], x1 - self.cam[0], y0 - self.cam[1], y1 - self.cam[1]
        if ((ax0 < cx and ax1 < cx) or (ax0 >= cx + cw and ax1 >= cx + cw) or (ay0 < cy and ay1 < cy)
                or (ay0 >= cy + ch and ay1 >= cy + ch)):
            return  # wholly off one side (the board skips these too)
        err = dx + dy
        while True:
            self._px(x0, y0, c)
            if x0 == x1 and y0 == y1:
                break
            e2 = 2 * err
            if e2 >= dy:
                err += dy
                x0 += sx
            if e2 <= dx:
                err += dx
                y0 += sy

    def rect(self, x0, y0, x1, y1, c=None):
        c = self._c(c)
        self.line(x0, y0, x1, y0, c); self.line(x1, y0, x1, y1, c)
        self.line(x1, y1, x0, y1, c); self.line(x0, y1, x0, y0, c)

    def rectfill(self, x0, y0, x1, y1, c=None):
        c = self._c(c)
        x0, x1 = sorted((int(math.floor(x0)), int(math.floor(x1))))
        y0, y1 = sorted((int(math.floor(y0)), int(math.floor(y1))))
        for y in range(max(y0, -256), min(y1, 384) + 1):
            for x in range(max(x0, -256), min(x1, 384) + 1):
                self._px(x, y, c)

    def circ(self, x, y, r, c=None, fill=False):
        c = self._c(c)
        x, y, r = int(math.floor(x)), int(math.floor(y)), int(math.floor(r))
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                d = dx * dx + dy * dy
                if (fill and d <= r * r + r) or (not fill and r * r - r <= d <= r * r + r):
                    self._px(x + dx, y + dy, c)

    def trifill(self, x0, y0, x1, y1, x2, y2, c=None):
        """The board's rule exactly: a pixel whose center is inside, top-left edges kept, in 1/16 px."""
        c = self._c(c)
        def sub(v):
            if not v > -2048:
                return -2048 * 16
            return 2047 * 16 if v > 2047 else math.floor(v * 16)
        xs = [sub(x0) - self.cam[0] * 16, sub(x1) - self.cam[0] * 16, sub(x2) - self.cam[0] * 16]
        ys = [sub(y0) - self.cam[1] * 16, sub(y1) - self.cam[1] * 16, sub(y2) - self.cam[1] * 16]
        def edge(ax, ay, bx, by, px, py):
            return (bx - ax) * (py - ay) - (by - ay) * (px - ax)
        area = edge(xs[0], ys[0], xs[1], ys[1], xs[2], ys[2])
        if area == 0:
            return
        if area < 0:
            xs[1], xs[2], ys[1], ys[2] = xs[2], xs[1], ys[2], ys[1]
        edges = []
        for a in range(3):
            b = (a + 1) % 3
            dx, dy = xs[b] - xs[a], ys[b] - ys[a]
            edges.append((xs[a], ys[a], dx, dy, 0 if (dy < 0 or (dy == 0 and dx > 0)) else -1))
        cx, cy, cw, ch = self.clipr
        lo = lambda v: -((-(v - 8)) // 16)  # ceil((v - 8) / 16)
        hi = lambda v: (v - 8) // 16
        px0, px1 = max(lo(min(xs)), cx, 0), min(hi(max(xs)), cx + cw - 1, W - 1)
        py0, py1 = max(lo(min(ys)), cy, 0), min(hi(max(ys)), cy + ch - 1, H - 1)
        col = self.pmap[c]
        for py in range(py0, py1 + 1):
            for px in range(px0, px1 + 1):
                X, Y = px * 16 + 8, py * 16 + 8
                if all(dx * (Y - ay) - dy * (X - ax) + bias >= 0 for ax, ay, dx, dy, bias in edges):
                    self.fb[py * W + px] = col

    def sin(self, t):
        return -math.sin(t * 2 * math.pi)  # turns, upside down (PICO-8's)

    def cos(self, t):
        return math.cos(t * 2 * math.pi)

    def circfill(self, x, y, r, c=None):
        self.circ(x, y, r, c, fill=True)

    def print_(self, text, x=0, y=0, c=None):
        c = self._c(c)
        if isinstance(text, float) and text.is_integer():
            text = int(text)  # whole numbers print without ".0", as on the board
        text = "" if text is None else (text if isinstance(text, str) else self.lua.globals().tostring(text))
        x, y = int(math.floor(x)), int(math.floor(y))
        x_start = x
        for ch in str(text).upper():
            g = FONT.get(ch, "111101101101111" if ch != "\n" else None)
            if ch == "\n":
                y += 6
                x = x_start
                continue
            for i, bit in enumerate(g):
                if bit == "1":
                    self._px(x + i % 3, y + i // 3, c)
            x += 4
        return x

    def spr(self, n, x, y, flip_x=False, flip_y=False):
        sprites = self.env["SPRITES"] if self.env is not None else None
        if sprites is None:
            raise ValueError("spr: no SPRITES table")
        s = sprites[int(n)]
        s = re.sub(r"[ \t\r\n]", "", s) if isinstance(s, str) else None  # spaces between rows are for reading (API 4)
        if s is None or not re.fullmatch(r"[0-9a-fA-F]{64}", s):
            raise ValueError(f"spr: SPRITES[{int(n)}] must be 64 hex digits (spaces between rows are fine)")
        self.spaced_sprites = self.spaced_sprites or len(sprites[int(n)]) != 64
        for i, h in enumerate(s):
            v = int(h, 16)
            if v:
                px, py = i % 8, i // 8
                if flip_x: px = 7 - px
                if flip_y: py = 7 - py
                self._px(x + px, y + py, v)

    # palette(i, 0xRRGGBB) / palette(i) / palette(): what each index shows (the preview uses it).
    def palette(self, i=None, rgb=None):
        if i is None:
            self.rgb = list(PICO)
            return None
        i = int(i)
        if not 0 <= i <= 15:
            raise ValueError(f"palette: color {i} isn't one of 0-15")
        if rgb is None:
            return self.rgb[i]
        rgb = int(rgb)
        if not 0 <= rgb <= 0xFFFFFF:
            raise ValueError(f"palette: {rgb:#x} isn't a 0xRRGGBB color")
        self.rgb[i] = rgb
        return None

    def pal(self, c0=None, c1=None):
        if c0 is None:
            self.pmap = list(range(16))
        else:
            self.pmap[int(c0) & 15] = int(c1) & 15

    def camera(self, x=0, y=0):
        self.cam = (int(math.floor(x or 0)), int(math.floor(y or 0)))

    def clip(self, x=None, y=None, w=None, h=None):
        self.clipr = (0, 0, W, H) if x is None else (int(x), int(y), int(w), int(h))

    # -- input --
    def btn(self, b=None):
        if b is None:  # all six as bits, as the board gives them
            return sum(1 << i for i in range(6) if self.held[i])
        return bool(0 <= int(b) < 6 and self.held[int(b)])

    def btnp(self, b):
        b = int(b)
        return bool(0 <= b < 6 and self.held[b] and not self.was[b])

    def touch(self, canvas=None):  # (x, y) or None; touch(true): canvas units (3x the picture's; a full canvas: glass pixels)
        if self.touch_at is None:
            return None
        # A Lua function returns multiple values; a Python tuple crosses into Lua as ONE value, so
        # `local x, y = touch()` would leave y nil. Hand every pair back as a Lua table and the
        # HARNESS's `touch` shim unpacks it.
        if not canvas:
            return self.lua.table_from(list(self.touch_at))
        if self.full:
            x, y = self.touch_at[0] * 3 + WINDOW_X, self.touch_at[1] * 3 + WINDOW_Y
        else:
            x, y = self.touch_at[0] * 3, self.touch_at[1] * 3
        return self.lua.table_from([x, y])

    def caps(self):  # the board's (check.py stands in for it)
        f = self.full
        return self.lua.table_from({"w": GLASS_W if f else HD_W, "h": GLASS_H if f else HD_H,
                                    "picture_x": WINDOW_X if f else 0, "picture_y": WINDOW_Y if f else 0,
                                    "corner": CORNER if f else 0, "scale": 1, "round": False, "touch": True,
                                    "tilt": True, "buzz": True, "sound": True, "hd": True, "stage": True, "host": True,
                                    "version": API_VERSION, "board": "waveshare-amoled-206"})

    def tilt(self):
        self.tilted = True  # (input the lazy rate doesn't watch: no lazy-rate hint for it)
        return self.tilt_at

    # -- feedback, time, numbers --
    def buzz(self, ms):
        if not 10 <= (ms or 0) <= 500:
            self.warnings.append(f"buzz({ms}): use 10-500 ms")
        self.calls["buzz"] += 1

    def sfx(self, n):
        n = int(n)
        if n == -1 or 0 <= n <= 7:
            self.calls["sfx"] += n >= 0
            return
        if not 8 <= n <= 63:
            raise ValueError(f"sfx({n}): sounds are 0-7 (built in) and 8-63 (your SOUNDS); -1 stops yours")
        text = self._text_at("SOUNDS", n)
        if text is None:
            raise ValueError(f"sfx({n}): SOUNDS[{n}] isn't there (a string from the sound editor)")
        why = sound_problem(text)
        if why:
            raise ValueError(f"sfx({n}): SOUNDS[{n}] {why}")
        self.calls["sfx"] += 1

    def log_line(self, line):  # log(...): the harness joins the tostring()s (engine/cart.cpp f_log)
        if self.frame != self.log_frame:
            self.log_frame, self.log_count = self.frame, 0
        if self.log_count > 20:
            return
        self.log_count += 1
        line = "(more log lines this frame were dropped)" if self.log_count == 21 else str(line)[:200]
        if len(self.logs) < 200:
            self.logs.append(line)

    def music(self, n):
        n = int(n)
        if n == -1:
            return
        if not 0 <= n <= 63:
            raise ValueError(f"music({n}): patterns are 0-63; -1 stops the song")
        why = song_problem(n, lambda i: self._text_at("MUSIC", i), lambda i: self._text_at("SOUNDS", i))
        if why:
            raise ValueError(f"music({n}): {why}")
        self.calls["music"] += 1

    def _text_at(self, table, i):
        t = self.env[table] if self.env is not None else None
        if t is None or lua54.lua_type(t) != "table":
            return None
        v = t[i]
        return v if isinstance(v, str) else None

    def time_us(self):  # whole microseconds, as the board's clock (and cartrun's) counts them
        return self.base_us + (self.frame - self.base_n) * 1_000_000 // self.clock_rate

    def time(self):
        return self.time_us() / 1e6

    def advance(self, f):  # frame f begins: the clock goes on at the rate the cart asked for
        if self.fps != self.clock_rate:
            self.base_us, self.base_n, self.clock_rate = self.time_us(), f - 1, self.fps
        self.frame = f

    def settle_rate(self):  # after a frame, as Cart::frame: a lazy cart's next rate
        if self.idle:
            t = self.time_us()
            if self.touch_at is not None or any(self.held):
                self.hot_until = t + 500_000
            self.fps = self.active if t < self.hot_until else self.idle

    def fps_(self, active=None, idle=None):  # fps(), fps(rate), fps(active, idle): as the runtime
        if active is None:
            return self.fps
        whole = lambda r: int(r) if isinstance(r, float) and r.is_integer() else r  # (as luaL_checkinteger takes 30.0)
        active, idle = whole(active), whole(idle)
        ok = lambda r: isinstance(r, int) and 1 <= r <= 60 and 60 % r == 0
        if not ok(active) or (idle not in (None, 0) and not ok(idle)):
            raise ValueError("fps: a rate that divides 60 (1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30 or 60)")
        if idle and idle > active:
            raise ValueError("fps: the idle rate is above the active one")
        self.active, self.idle = active, idle or 0
        self.fps = self.idle if self.idle and self.time_us() >= self.hot_until else active

    # Wall-clock time: the box's clock when the run started, moving with the frames (as the
    # board's does while the cart runs), so a cart's catch-up logic sees time pass.
    def now(self):
        return self.wall0 + self.time_us() // 1_000_000

    def date(self, t=None):
        lt = time.localtime(self.now() if t is None else int(t))
        return self.lua.table_from({"year": lt.tm_year, "month": lt.tm_mon, "day": lt.tm_mday, "hour": lt.tm_hour,
                                    "min": lt.tm_min, "sec": lt.tm_sec, "wday": (lt.tm_wday + 1) % 7 + 1,
                                    "yday": lt.tm_yday})

    def wake(self, secs=None):
        if secs is not None and secs < 0:
            raise ValueError("wake(secs): secs must be 0 or more (wake() with nothing cancels)")
        self.calls["wake"] += 1

    def level(self):
        self.calls["level"] = self.calls.get("level", 0) + 1

    def stat(self, n):
        return {0: 40.0, 1: 0.1, 7: float(self.fps)}.get(int(n), 0)

    def rnd(self, x=None):
        if x is not None and lua54.lua_type(x) == "table":
            n = len(x)
            return x[self.rng.randrange(n) + 1] if n else None
        return self.rng.random() * (1 if x is None else x)

    def srand(self, seed):
        self.rng.seed(seed)

    def save(self, t):
        data = json.dumps(self._to_py(t, 0), separators=(",", ":"))
        if len(data) > MAX_SAVE:
            raise ValueError(f"save: {len(data)} bytes, the limit is {MAX_SAVE}")
        self.saved = data
        self.calls["save"] += 1

    def load(self):
        return None if self.saved is None else self.lua.table_from(json.loads(self.saved), recursive=True)

    def _to_py(self, v, depth):
        if depth > 8:
            raise ValueError("save: nested too deep")
        if lua54.lua_type(v) == "table":
            keys = list(v.keys())
            if keys and all(isinstance(k, int) for k in keys) and sorted(keys) == list(range(1, len(keys) + 1)):
                return [self._to_py(v[k], depth + 1) for k in sorted(keys)]
            return {str(k): self._to_py(v[k], depth + 1) for k in keys}
        if v is None or isinstance(v, (bool, int, float, str)):
            return v
        raise ValueError(f"save: can't keep a {lua54.lua_type(v) or type(v).__name__}")

    # -- the host: data() and act() --
    def data(self, name):
        # As on the board: the same table back until the channel changes (the samples never do).
        self.calls["data"].add(str(name))
        v = SAMPLE.get(str(name))
        if v is None:
            return None
        if str(name) not in self.data_tables:
            self.data_tables[str(name)] = self.lua.table_from(v, recursive=True)
        return self.data_tables[str(name)]

    def ask(self, text, fn):
        raise ValueError("ask: not on the board yet")

    def say(self, text):
        raise ValueError("say: not on the board yet")

    def act(self, name, arg=None):
        if str(name) not in ACTS:
            raise ValueError(f"act: unknown command {name!r} (allowed: {', '.join(sorted(ACTS))})")
        self.calls["act"].append(str(name))

    # -- 3D (the stage, engine/stage.hpp): checked here, drawn by cartrun --
    def _3d(self):
        if self.scene is None:
            self.scene = {"shapes": [], "objects": 0, "materials": set(), "verts": 0, "tris": 0,
                          "cam": (0.0, 0.0, -420.0, 0.0, 0.0, 60.0)}
        return self.scene

    def _rgb(self, v, fn):
        if not isinstance(v, int) or isinstance(v, bool) or not 0 <= v <= 0xFFFFFF:
            raise ValueError(f"{fn}: colors are 0xRRGGBB")
        return v

    def _fail(self, msg):
        raise ValueError(msg)

    def _add_shape(self, nv, nt):
        sc = self._3d()
        if len(sc["shapes"]) >= 64:
            self._fail("shape: too many shapes (the limit is 64)")
        if nv == 0 or nt == 0:
            self._fail("shape: a shape with no triangles")
        if sc["verts"] + nv > 16384 or sc["tris"] + nt > 16384:
            self._fail(f"shape: the scene's shapes would hold {sc['verts'] + nv} vertices and {sc['tris'] + nt} triangles; "
                       "the limits are 16384 and 16384")
        sc["verts"] += nv
        sc["tris"] += nt
        sc["shapes"].append(nt)
        return len(sc["shapes"])

    def shape(self, kind, a=None, b=None, c=None):
        if not isinstance(kind, str):  # shape(verts, faces[, smooth])
            xyz, faces = list(kind.values()) if kind is not None else [], list(a.values()) if a is not None else []
            if len(xyz) % 3 or len(faces) % 3:
                self._fail("shape: a shape's vertices come in threes (x, y, z) and its faces in threes (a, b, c)")
            nv = len(xyz) // 3
            for v in xyz:
                if not isinstance(v, (int, float)):
                    self._fail("shape: the vertices must be numbers")
            for f in faces:
                if not isinstance(f, (int, float)) or not 1 <= int(f) <= nv:
                    self._fail(f"shape: a face uses vertex {f}; the shape has {nv}")
            if c is not None:  # texture coordinates, a pair a vertex
                uv = list(c.values())
                if len(uv) != 2 * nv or not all(isinstance(x, (int, float)) for x in uv):
                    self._fail(f"shape: a shape's texture coordinates come in twos (u, v), one pair a vertex: {2 * nv} numbers, not {len(uv)}")
            return self._add_shape(nv if b else len(faces), len(faces) // 3)
        num = lambda v, d=None: float(v) if isinstance(v, (int, float)) else (d if v is None and d is not None else self._fail(f"shape: {kind} needs numbers"))
        if kind == "box":
            num(a)
            return self._add_shape(24, 12)
        segs = lambda v, d: int(v) if v is not None else d
        if kind == "sphere":
            num(a)
            n = max(4, min(64, segs(b, 16)))
            rings = max(2, n // 2)
            return self._add_shape((rings + 1) * (n + 1), n * (2 * rings - 2))
        if kind == "torus":
            num(a), num(b)
            n = max(6, min(96, segs(c, 24)))
            tube = max(3, n // 2)
            return self._add_shape((n + 1) * (tube + 1), 2 * n * tube)
        if kind in ("cylinder", "cone"):
            num(a), num(b)
            n = max(3, min(96, segs(c, 16)))
            caps = 2 if kind == "cylinder" else 1
            return self._add_shape(2 * (n + 1) + caps * (n + 2), (2 * n if kind == "cylinder" else n) + caps * n)
        self._fail(f"shape: no shape '{kind}' (box, sphere, torus, cylinder, cone, or vertices and faces)")

    def obj(self, shape, color, shine=0, unlit=False):
        sc = self._3d()
        if not isinstance(shape, int) or not 1 <= shape <= len(sc["shapes"]):
            self._fail(f"obj: no shape {shape}")
        if sc["objects"] >= 128:
            self._fail("obj: too many objects (the limit is 128)")
        key = (self._rgb(color, "obj"), max(0, min(255, int(shine or 0))), bool(unlit))
        sc["materials"].add(key)
        if len(sc["materials"]) > 64:
            self._fail("obj: too many colors in the scene (the limit is 64 color and shine pairs)")
        sc["objects"] += 1
        return sc["objects"]

    def _obj(self, o, fn):
        sc = self._3d()
        if not isinstance(o, int) or not 1 <= o <= sc["objects"]:
            self._fail(f"{fn}: no object {o}")

    def place(self, o, x, y, z, rx=0, ry=0, rz=0):
        self._obj(o, "place")
        for v in (x, y, z, rx, ry, rz):
            if not isinstance(v, (int, float)):
                self._fail("place: needs numbers")

    def orient(self, o, *m):
        self._obj(o, "orient")
        vals = list(m[0].values()) if m and not isinstance(m[0], (int, float)) else list(m)
        if len(vals) < 9 or not all(isinstance(v, (int, float)) for v in vals[:9]):
            self._fail("orient: a rotation is nine numbers (rows)")

    def hide(self, o, hidden=True):
        self._obj(o, "hide")

    def paint(self, o, color, shine=0, unlit=False):
        self._obj(o, "paint")
        sc = self._3d()
        sc["materials"].add((self._rgb(color, "paint"), max(0, min(255, int(shine or 0))), bool(unlit)))
        if len(sc["materials"]) > 64:
            self._fail("paint: too many colors in the scene (the limit is 64 color and shine pairs)")

    def view(self, x, y, z, yaw=0, pitch=0, fov=60):
        self._3d()["cam"] = (float(x), float(y), float(z), float(yaw), float(pitch), max(10.0, min(150.0, float(fov))))

    def light(self, az, el, intensity=255, ambient=None):
        self._3d()
        if ambient is not None:
            self._rgb(ambient, "light")

    def sky(self, top, bottom=None):
        self._3d()
        self._rgb(top, "sky")
        if bottom is not None:
            self._rgb(bottom, "sky")

    def ground(self, y, color=None):
        self._3d()
        if not isinstance(y, (int, float)):
            self._fail("ground: needs a height")
        if color is not None:
            self._3d()["materials"].add((self._rgb(color, "ground"), 0, True))

    def shadow(self, o, on=True):
        self._obj(o, "shadow")

    def texture(self, w, h, texels):
        sc = self._3d()
        if w not in (4, 8, 16, 32, 64) or h not in (4, 8, 16, 32, 64):
            self._fail(f"texture: a texture is 4, 8, 16, 32 or 64 texels wide and high (not {w} x {h})")
        if not isinstance(texels, str):
            self._fail("texture: the texels are a string of color numbers (hex digits, as SPRITES)")
        digits = [ch for ch in texels if ch not in " \n\t\r"]
        bad = [ch for ch in digits if ch not in "0123456789abcdefABCDEF"]
        if bad:
            self._fail(f"texture: '{bad[0]}' isn't a color number (0-9, a-f)")
        if len(digits) != w * h:
            self._fail(f"texture: a texture of {w} x {h} needs {w * h} texels (it has {len(digits)})")
        sc.setdefault("textures", 0)
        if sc["textures"] >= 16:
            self._fail("texture: too many textures (the limit is 16)")
        sc["textures"] += 1
        return sc["textures"]

    def skin(self, o, tex=None, ru=1, rv=None):
        self._obj(o, "skin")
        sc = self._3d()
        if tex is not None and (not isinstance(tex, int) or not 1 <= tex <= sc.get("textures", 0)):
            self._fail(f"skin: no texture {tex}")
        if tex is not None:
            sc["materials"].add(("tex", tex))
            if len(sc["materials"]) > 64:
                self._fail("skin: too many colors in the scene (the limit is 64 color and shine pairs)")

    def res(self, mode=None):
        sc = self._3d()
        if mode is not None:
            if mode not in ("full", "half", "auto"):
                self._fail(f'res: "full", "half" or "auto" (not {mode!r})')
            sc["res"] = mode
        return "half" if sc.get("res") == "half" else "full"

    def project(self, x, y, z):  # as Stage::project, in the picture's coordinates (the window's center, x 204 of the glass)
        cx, cy, cz, yaw, pitch, fov = self._3d()["cam"]
        rx, ry, rz = x - cx, y - cy, z - cz
        a = math.radians(-yaw)  # Ry(-yaw), then Rx(pitch)
        c, s_ = math.cos(a), math.sin(a)
        rx, rz = c * rx + s_ * rz, -s_ * rx + c * rz
        b = math.radians(pitch)
        c, s_ = math.cos(b), math.sin(b)
        ry, rz = c * ry - s_ * rz, s_ * ry + c * rz
        if rz <= 2:
            return None
        focal = 192 / math.tan(math.radians(fov) / 2)
        return (rx * focal / rz + 192) / 3, (192 - ry * focal / rz) / 3, rz

    # -- the full-color layer (engine/hd.hpp): checked here, drawn by cartrun --
    def _hd(self, fn):
        if self.hd is None:
            self.hd = {}
        self.hd[fn] = self.hd.get(fn, 0) + 1

    @staticmethod
    def _num(v, fn, what):
        if not isinstance(v, (int, float)) or isinstance(v, bool):
            raise ValueError(f"hd.{fn}: {what} must be a number")
        return v

    @staticmethod
    def _hd_rgb(v, fn):
        if not isinstance(v, (int, float)) or isinstance(v, bool) or v != int(v):
            raise ValueError(f"hd.{fn}: colors are 0xRRGGBB integers")
        if not 0 <= v <= 0xFFFFFF:
            raise ValueError(f"hd.{fn}: {int(v):#x} isn't a 0xRRGGBB color")
        return int(v)

    def _hd_shape(self, fn, nums, colors, opts):
        """nums: the arguments that must be numbers; colors: those that are 0xRRGGBB; opts: optional numbers."""
        self._hd(fn)
        for i, v in enumerate(nums):
            self._num(v, fn, f"argument {i + 1}")
        for v in colors:
            self._hd_rgb(v, fn)
        for v in opts:
            if v is not None:
                self._num(v, fn, "an optional argument")

    # -- the device cost estimate --
    def _hd_box(self, x, y, w, h):
        """(x0, y0, x1, y1) of the box within the clip, or None."""
        cx0, cy0, cx1, cy1 = self.hd_clipr
        x0, y0, x1, y1 = max(x, cx0), max(y, cy0), min(x + w, cx1), min(y + h, cy1)
        return (x0, y0, x1, y1) if x1 > x0 and y1 > y0 else None

    def _hd_cost(self, x, y, w, h, fill=0.0, edge=0.0, a=255):
        """A shape in the box (x, y, w, h): `fill` of its area whole pixels, `edge` measured ones."""
        b = self._hd_box(x, y, w, h)
        if not b:
            return
        area = (b[2] - b[0]) * (b[3] - b[1])
        ns = HD_NS["fill"] if a is None or a >= 255 else HD_NS["blend"]
        self.hd_us += area * (fill * ns + edge * HD_NS["edge"]) / 1000
        rows = range(int(b[1]), int(math.ceil(b[3])))
        self.hd_rows.update(rows)
        self.hd_inked.update(rows)

    def hd_clear(self, rgb=None, a=None):
        self._hd_shape("clear", [], [] if rgb is None else [rgb], [a])
        cx0, cy0, cx1, cy1 = self.hd_clipr
        whole = (cx0, cx1) == (0, self.hd_w) and rgb is None and not a
        rows = [y for y in range(int(cy0), int(math.ceil(cy1))) if not whole or y in self.hd_inked]
        self.hd_us += len(rows) * (cx1 - cx0) * HD_NS["fill"] / 1000
        self.hd_rows.update(rows)
        if whole:
            self.hd_inked.difference_update(rows)
        else:
            self.hd_inked.update(rows)

    def hd_order(self, o=None):
        self._hd("order")
        if o not in ("over", "under"):
            raise ValueError(f'hd.order: "over" or "under", not {o!r}')

    def hd_scanlines(self, on=None):  # hd.scanlines(): whether they're on
        self._hd("scanlines")
        if on is None:
            return self.hd_scan
        self.hd_scan = bool(on)

    def hd_rect(self, x=None, y=None, w=None, h=None, rgb=None, a=None, radius=None):
        self._hd_shape("rect", [x, y, w, h], [rgb], [a, radius])
        self._hd_cost(x, y, w, h, fill=1, a=a)

    def hd_frame(self, x=None, y=None, w=None, h=None, rgb=None, width=None, radius=None, a=None):
        self._hd_shape("frame", [x, y, w, h], [rgb], [width, radius, a])
        bw = min(1.0, 2 * (width or 1) * (w + h) / max(1.0, w * h))  # the band's share of the box
        self._hd_cost(x, y, w, h, fill=bw, edge=bw / 2, a=a)

    def hd_circle(self, x=None, y=None, r=None, rgb=None, a=None):
        self._hd_shape("circle", [x, y, r], [rgb], [a])
        self._hd_cost(x - r - 1, y - r - 1, 2 * r + 2, 2 * r + 2, fill=math.pi / 4, edge=min(1.0, 2 / max(r, 1)), a=a)

    def hd_ring(self, x=None, y=None, r=None, rgb=None, width=None, a=None):
        self._hd_shape("ring", [x, y, r], [rgb], [width, a])
        share = min(1.0, math.pi * (width or 1) * 2 / max(4 * r, 1))  # the band's share of the box
        self._hd_cost(x - r - 1, y - r - 1, 2 * r + 2, 2 * r + 2, fill=share, edge=share, a=a)

    def hd_line(self, x0=None, y0=None, x1=None, y1=None, rgb=None, width=None, a=None):
        self._hd_shape("line", [x0, y0, x1, y1], [rgb], [width, a])
        length, wd = math.hypot(x1 - x0, y1 - y0), (width or 1) + 3
        bx, by = min(x0, x1) - wd, min(y0, y1) - wd
        bw, bh = abs(x1 - x0) + 2 * wd, abs(y1 - y0) + 2 * wd
        self._hd_cost(bx, by, bw, bh, edge=min(1.0, length * wd / (bw * bh)))

    def hd_gradient(self, x=None, y=None, w=None, h=None, top=None, bottom=None, a=None):
        self._hd_shape("gradient", [x, y, w, h], [top, bottom], [a])
        self._hd_cost(x, y, w, h, fill=1, a=a)

    def _font(self, fn, size, font):
        """As Hd::pick: the nearest size the font has (the bigger on a tie)."""
        if size is not None and (not isinstance(size, (int, float)) or isinstance(size, bool) or size != int(size)):
            raise ValueError(f"hd.{fn}: size must be an integer (px)")
        font = "mono" if font is None else font
        fonts = hd_fonts()
        if font not in fonts:
            raise ValueError(f"hd: no font '{font}' (mono, bold, vt)")
        size = 16 if size is None else int(size)
        return min(fonts[font].values(), key=lambda f: (abs(f["size"] - size), -f["size"]))

    def _text_str(self, text):
        return text if isinstance(text, str) else self.lua.globals().tostring(text)

    @staticmethod
    def _measure(f, s: str) -> int:
        return sum(f["adv"].get(ord(ch), f["size"] // 2) for ch in s)

    def hd_text(self, text=None, x=None, y=None, rgb=None, size=None, font=None, align=None, a=None):
        self._hd_shape("text", [x, y], [rgb], [a])
        if align not in (None, "left", "center", "right"):
            raise ValueError(f'hd.text: align is "left", "center" or "right", not {align!r}')
        f = self._font("text", size, font)
        w = self._measure(f, self._text_str(text))
        x0 = x - (w / 2 if align == "center" else w if align == "right" else 0)
        self._hd_cost(x0, y, w, f["ascent"] + f["descent"], edge=0.35)  # about a third of a line's box is ink
        return float(w)

    def hd_measure(self, text=None, size=None, font=None):
        self._hd("measure")
        f = self._font("measure", size, font)
        return float(self._measure(f, self._text_str(text))), float(f["ascent"] + f["descent"])

    def hd_wrap(self, text=None, width=None, size=None, font=None):  # as Hd::wrap, word for word
        self._hd("wrap")
        if not isinstance(text, str):
            raise ValueError("hd.wrap: text must be a string")
        self._num(width, "wrap", "width")
        f = self._font("wrap", size, font)
        fits = lambda t: self._measure(f, t) <= width  # noqa: E731
        lines, line, word = [], "", ""

        def flush():
            nonlocal line, word
            if not word:
                return
            with_ = word if not line else line + " " + word
            if fits(with_):
                line = with_
            else:
                if line:
                    lines.append(line)
                part = ""
                for ch in word:  # a word wider than the line is broken where it must be
                    if part and not fits(part + ch):
                        lines.append(part)
                        part = ch
                    else:
                        part += ch
                line = part
            word = ""
        for ch in text:
            if ch == " ":
                flush()
            elif ch == "\n":
                flush()
                lines.append(line)
                line = ""
            else:
                word += ch
        flush()
        if line or not lines:
            lines.append(line)
        return self.lua.table_from(lines)

    def hd_inset(self, y=None, h=None):  # as the runtime: the rounded corners' inset for rows [y, y + h)
        self._hd("inset")
        self._num(y, "inset", "y")
        h = max(1, h or 1)
        r = CORNER if self.full else 0
        d = min(y + 0.5, GLASS_H - (y + h - 1) - 0.5)
        if r <= 0 or d >= r:
            return 0
        d = max(0.0, d)
        return math.ceil(r - math.sqrt(max(0.0, r * r - (r - d) ** 2)))

    def hd_scroll(self, x=None, y=None, w=None, h=None, dy=None):
        self._hd_shape("scroll", [x, y, w, h, dy], [], [])
        x0, x1 = max(0, int(x)), min(self.hd_w, int(x) + int(w))
        y0, y1 = max(0, int(y)), min(self.hd_h, int(y) + int(h))
        if int(dy) == 0 or x0 >= x1 or y0 >= y1 or abs(int(dy)) >= y1 - y0:
            return
        moved = range(y0, y1 + int(dy)) if int(dy) < 0 else range(y0 + int(dy), y1)
        if (x0, x1) != (0, self.hd_w):  # a narrower box copies its pixels (the whole width turns a row map: free)
            self.hd_us += len(moved) * (x1 - x0) * HD_NS["scroll"] / 1000
            self.hd_rows.update(moved)
        self.hd_inked.update(range(y0, y1))

    def hd_clip(self, x=None, y=None, w=None, h=None):
        if x is None:
            self._hd("clip")
            self.hd_clipr = (0, 0, self.hd_w, self.hd_h)
        else:
            self._hd_shape("clip", [x, y, w, h], [], [])
            x0, y0 = max(0, math.floor(x)), max(0, math.floor(y))
            self.hd_clipr = (x0, y0, max(x0, min(self.hd_w, math.ceil(x + w))), max(y0, min(self.hd_h, math.ceil(y + h))))

    # The cart's images: what pack_images.py made of images/CART/ beside the cart (as the board gets them).
    def hd_image(self, name=None, x=None, y=None, w=None, h=None, a=None):
        if not isinstance(name, str):
            raise ValueError("hd.image: name must be a string")
        self._hd_shape("image", [x, y], [], [w, h, a])
        info = self.images.get(name) or live_samples()[1].get(name)
        if info is None:
            self.warnings.append(f"hd.image({name!r}): not one of the cart's images (images/{self.cart_name}/ has "
                                 f"{', '.join(sorted(self.images)) or 'none'}) nor a live sample (tools/samples/live/); "
                                 "on the board the host is asked for it, and it returns false until it comes (if it has one)")
            return False
        dw, dh = (w or info["w"]), (h or info["h"])
        plain = not info["alpha"] and (a is None or a >= 255)  # copied (scaled or not), not blended
        b = self._hd_box(round(x), round(y), round(dw), round(dh))
        if b:
            area = (b[2] - b[0]) * (b[3] - b[1])
            self.hd_us += area * (HD_NS["image"] if plain else HD_NS["image_alpha"]) / 1000
            rows = range(int(b[1]), int(math.ceil(b[3])))
            self.hd_rows.update(rows)
            self.hd_inked.update(rows)
        self.images_used.add(name)
        return True

    def hd_images(self):
        self._hd("images")
        return self.lua.table_from(list(self.images))

    def api(self):
        names = ["cls", "pset", "pget", "line", "rect", "rectfill", "circ", "circfill", "spr", "pal", "palette", "camera",
                 "clip", "btn", "btnp", "touch", "tilt", "buzz", "sfx", "music", "time", "now", "date", "rnd", "srand", "save", "load",
                 "data", "ask", "say", "act", "wake", "stat", "level", "trifill", "sin", "cos",
                 "shape", "obj", "place", "orient", "hide", "paint", "view", "light", "sky", "project", "ground", "shadow",
                 "res", "texture", "skin", "caps"]
        t = {n: getattr(self, n) for n in names}
        t["print"] = self.print_
        t["log_line"] = self.log_line  # for the harness's log(); not a cart's
        t["fps"] = self.fps_
        t["hd"] = self.lua.table_from({n[3:]: getattr(self, n) for n in dir(self) if n.startswith("hd_")})
        return t


_HD_FONTS = None


def hd_fonts() -> dict:
    """{name: {size: {"size", "ascent", "descent", "adv": {cp: advance}}}} from the generated tables."""
    global _HD_FONTS
    if _HD_FONTS is None:
        if not HD_FONTS.exists():
            raise ValueError(f"hd: the font tables aren't there ({HD_FONTS}; they come with the engine)")
        src = HD_FONTS.read_text()
        glyphs = {m.group(1): {int(cp): int(adv) for cp, adv in re.findall(r"\{(\d+), -?\d+, -?\d+, -?\d+, -?\d+, -?\d+, (\d+)\}", m.group(2))}
                  for m in re.finditer(r"static const Glyph (\w+)_glyphs\[\] = \{\n(.*?)\n\};", src, re.S)}
        _HD_FONTS = {}
        for name, size, asc, desc in re.findall(r'\{"(\w+)", (\d+), (\d+), (\d+), \d+, \w+_glyphs, \w+_bits\}', src):
            _HD_FONTS.setdefault(name, {})[int(size)] = {"size": int(size), "ascent": int(asc), "descent": int(desc),
                                                         "adv": glyphs[f"{name}_{size}"]}
    return _HD_FONTS


HARNESS = r"""
local api, src, ui = ...
local budget, used, per, armed = 0, 0, 100, false
-- Counts; and while the cart's code runs (armed), stops it past the budget. The few instructions of
-- this harness around the call count but never stop anything: an error there would escape the pcall.
local function hook()
  used = used + per
  if armed and used > budget then
    armed = false
    error("over budget: more than " .. budget .. " Lua instructions in one frame", 2)
  end
end
-- Lua's count runs on through the hook's own instructions (luaG_traceexec), so each call every 100
-- stands for fewer of the cart's: measured here on a loop of known size, 2 instructions a pass, so
-- the count is the cart's own, as the runtime counts it (engine/cart.cpp count_hook, a C hook).
do
  local n = 0
  budget, used, per, armed = 1e12, 0, 100, true  -- the hook's path in a frame (armed, under budget)
  debug.sethook(hook, "", 100)
  for i = 1, 100000 do n = n + i end
  debug.sethook()
  armed = false
  per = math.floor(200000 / (used / 100) + 0.5)
end
local env = {}
for _, k in ipairs({"math", "string", "table", "coroutine", "pairs", "ipairs", "tostring", "tonumber", "type",
                    "select", "pcall", "error", "assert", "setmetatable", "getmetatable", "next", "rawget",
                    "rawset", "rawequal", "rawlen"}) do env[k] = _G[k] end
env.unpack = table.unpack
env.log = function(...)  -- as the runtime builds the line: tostring() of each, spaces between
  local t = {}
  for i = 1, select("#", ...) do t[i] = tostring((select(i, ...))) end
  api.log_line(table.concat(t, " "))
end
local flr = math.floor
env.flr = flr
env.ceil = math.ceil
env.abs = math.abs
env.min = math.min
env.max = math.max
env.sgn = function(x) return x < 0 and -1 or 1 end
env.mid = function(a, b, c) if a > b then a, b = b, a end return math.max(a, math.min(b, c)) end
for k, v in pairs(api) do env[k] = v end
env.log_line = nil
-- touch() has to return TWO values to the cart: the host hands back a table, so unpack it
do
  local raw_touch = env.touch
  env.touch = function(canvas)
    local r = raw_touch(canvas)
    if type(r) == "table" then return r[1], r[2] end
    return r
  end
end
env._G = env
local helpers, uerr = load(ui, "=ui", "t", env)  -- run as the runtime does, before the cart
if not helpers then return nil, "the runtime's ui.lua: " .. uerr end
helpers()
local fn, err = load(src, "=cart", "t", env)
if not fn then return nil, err end
local R = {env = env}
local function run(f, b)
  budget, used, armed = b, 0, true
  debug.sethook(hook, "", 100)
  local ok, e = pcall(function() f() armed = false end)
  armed = false
  debug.sethook()
  return ok, e, used
end
function R.main() return run(fn, 2000000) end
function R.call(name)
  local f = env[name]
  if type(f) ~= "function" then return true, nil, 0 end
  return run(f, 1e12)
end
function R.frame(upd, b)
  budget, used, armed = b, 0, true
  debug.sethook(hook, "", 100)
  local ok, e = pcall(function()
    if type(env[upd]) == "function" then env[upd]() end
    if type(env._draw) == "function" then env._draw() end
    armed = false
  end)
  armed = false
  debug.sethook()
  return ok, e, used
end
return R
"""


def script_input(cart: Cart, f: int, rng: random.Random):
    cart.was = cart.held[:]
    held = [False] * 6
    phase = f % 120
    for b, (a, z) in enumerate([(40, 52), (8, 20), (56, 68), (24, 36)]):  # left, right, up, down
        held[b] = a <= phase < z
    held[4] = phase in (5, 6, 30, 31, 61, 62, 90, 91) or rng.random() < 0.03
    held[5] = phase in (75, 76, 105, 106)
    cart.held = held
    cart.tilt_at = (round(math.sin(f / 20) * 0.6, 3), round(math.cos(f / 25) * 0.6, 3))
    if 70 <= phase < 73:  # a tap
        cart.touch_at = (64, 64)
    elif 80 <= phase < 100:  # a drag up, then
        cart.touch_at = (64, 110 - (phase - 80) * 4)
    else:
        cart.touch_at = None


def render(fbs, theme: str) -> Image.Image:
    """Frames side by side at the board's scale; each a picture of indices, or (picture, its 16 colors)."""
    def colors(rgb):
        if theme == "phosphor":
            cols = []
            for c in rgb:
                r, g, b = (c >> 16) & 255, (c >> 8) & 255, c & 255
                lum = (0.3 * r + 0.59 * g + 0.11 * b) / 255
                cols.append((int(40 * lum), int(30 + 225 * lum) if lum else 0, int(110 * lum)))
            return cols
        return [((c >> 16) & 255, (c >> 8) & 255, c & 255) for c in rgb]
    gap = 16
    out = Image.new("RGB", (len(fbs) * (W * SCALE + gap) + gap, H * SCALE + 2 * gap), (24, 24, 24))
    for i, fb in enumerate(fbs):
        fb, rgb = fb if isinstance(fb, tuple) else (fb, PICO)
        cols = colors(rgb)
        img = Image.new("RGB", (W, H))
        img.putdata([cols[v] for v in fb])
        out.paste(img.resize((W * SCALE, H * SCALE), Image.NEAREST), (gap + i * (W * SCALE + gap), gap))
    return out


def cartrun_input(fps: int, inputs: list, t0: int) -> dict:
    """What cartrun replays: the frames' input, the sample channels and the clock check.py used."""
    return {"fps": fps, "frames": inputs, "t0": t0, "data": {k: v for k, v in SAMPLE.items() if v is not None}}


def run_3d(path: Path, fps: int, inputs: list, shots: list[int], bundle: bytes = b"", t0: int = 0) -> dict:
    """Runs a 3D or full-color cart through cartrun with check.py's input; returns its result and the shots it drew."""
    if not CARTRUN.exists():
        return {"ok": False, "why": f"isn't built ({CARTRUN}; cmake -S host -B host/build && cmake --build host/build)"}
    d = Path(tempfile.mkdtemp(prefix="cart3d-"))
    body = cartrun_input(fps, inputs, t0)
    if bundle:
        (d / "images.hdb").write_bytes(bundle)
        body["images"] = str(d / "images.hdb")
    if live_samples()[0]:
        (d / "live.hdb").write_bytes(live_samples()[0])
        body["live"] = str(d / "live.hdb")
    (d / "input.json").write_text(json.dumps(body))
    shipped = d / path.name  # as it's sent: comments out
    shipped.write_text(shipped_src(path))
    p = subprocess.run([str(CARTRUN), str(shipped), str(d / "input.json"), str(d), ",".join(map(str, shots))],
                       capture_output=True, text=True, timeout=120)
    try:
        out = json.loads(p.stdout.strip().splitlines()[-1])
    except (IndexError, json.JSONDecodeError):
        return {"ok": False, "why": f"gave no result ({p.stderr.strip()[:200]})"}
    out["shots"] = [str(d / f"frame_{n}.ppm") for n in shots if (d / f"frame_{n}.ppm").exists()]
    return out


# The cart's own sounds and music, checked as engine/synth.cpp parses them (same words when wrong).
def _hex(text: str, dashes: bool = False):
    out = []
    for c in text:
        if c in " \t\n":
            continue
        if c in "0123456789abcdefABCDEF":
            out.append(int(c, 16))
        elif dashes and c == "-":
            out.append(None)
        else:
            return None
    return out


def sound_problem(text: str) -> str:
    d = _hex(text)
    if d is None:
        return "should be hex digits (the sound editor writes them)"
    if len(d) < 11 or (len(d) - 6) % 5:
        return "should be 6 digits (speed, loop) and then 5 for each note"
    speed, start, end, count = d[0] * 16 + d[1], d[2] * 16 + d[3], d[4] * 16 + d[5], (len(d) - 6) // 5
    if count > 32:
        return f"has {count} notes; a sound has at most 32"
    if speed < 1:
        return "has speed 0; it's 1 to 255"
    for i in range(count):
        n = d[6 + i * 5: 11 + i * 5]
        if n[0] * 16 + n[1] > 63 or n[2] > 7 or n[3] > 7 or n[4] > 7:
            return f"note {i + 1}: pitch 0-3f, instrument 0-7, volume 0-7, effect 0-7"
    if end > start and end > count:
        return f"loops to note {end}; it has {count}"
    return ""


def pattern_sounds(text: str):
    """A pattern's sounds (None for none), or the problem as a string."""
    d = _hex(text, dashes=True)
    if d is None or len(d) != 10:
        return 'should be 2 digits of flags and then a sound per channel ("00 08 09 -- --")'
    if d[0] is None or d[1] is None:
        return "should start with 2 digits of flags"
    out = []
    for c in range(4):
        a, b = d[2 + c * 2], d[3 + c * 2]
        if a is None and b is None:
            out.append(None)
            continue
        if a is None or b is None or not 8 <= a * 16 + b <= 63:
            return f"channel {c + 1}: a sound is 08-3f (hex), or -- for none"
        out.append(a * 16 + b)
    return out


def song_problem(start: int, pattern_text, sound_text) -> str:
    has = set()
    for i in range(64):
        text = pattern_text(i)
        if text is None:
            continue
        sounds = pattern_sounds(text)
        if isinstance(sounds, str):
            return f"MUSIC[{i}] {sounds}"
        has.add(i)
        for c, s in enumerate(sounds):
            if s is None:
                continue
            st = sound_text(s)
            if st is None:
                return f"MUSIC[{i}] channel {c + 1}: SOUNDS[{s}] isn't there"
            why = sound_problem(st)
            if why:
                return f"MUSIC[{i}] channel {c + 1}: SOUNDS[{s}] {why}"
    if start not in has:
        return f"MUSIC[{start}] isn't there"
    return ""


def shipped_src(path: Path) -> str:
    return shipped(path.read_text())


def check(path: Path, frames: int, preview: Path | None, theme: str, dump_input: Path | None = None,
          dump_frames: Path | None = None) -> dict:
    # What's checked, measured and run is what ships (shipped(): comments out); "source_bytes" is the file.
    raw = path.read_text()
    src = shipped(raw)
    res: dict = {"ok": False, "cart": str(path), "bytes": len(src.encode()), "source_bytes": len(raw.encode())}
    if res["bytes"] > MAX_SOURCE:
        res["error"] = f"cart is {res['bytes']} bytes without its comments; the limit is {MAX_SOURCE}"
        return res
    head = "\n".join(src.splitlines()[:12])
    missing = [k for k in ("title", "by", "about") if not re.search(rf"^--\s*{k}\s*:\s*\S", head, re.M)]
    if missing:
        res["error"] = "missing header comment(s) at the top: " + ", ".join(f"-- {k}: ..." for k in missing)
        return res
    res["title"] = re.search(r"^--\s*title\s*:\s*(.+)$", head, re.M).group(1).strip()
    m = re.search(r"^--\s*api\s*:\s*(.*?)\s*$", head, re.M)
    api = 1
    if m:  # as the runtime reads it (Cart::start)
        if not re.fullmatch(r"\d+", m.group(1)) or int(m.group(1)) < 1:
            res["error"] = f"its header says '-- api: {m.group(1)}'; that should be a whole number (1 to {API_VERSION})"
            return res
        api = int(m.group(1))
        if api > API_VERSION:
            res["error"] = f"it needs API {api}; the board's runtime has {API_VERSION}"
            return res
    res["api"] = api
    m = re.search(r"^--\s*canvas\s*:\s*(.*?)\s*$", head, re.M)
    full = False
    if m:  # as the runtime reads it
        if m.group(1) not in ("full", "square"):
            res["error"] = f"its header says '-- canvas: {m.group(1)}'; that should be full or square"
            return res
        full = m.group(1) == "full"
    if full:
        res["canvas"] = "full"
    try:
        lua = lua54.LuaRuntime(unpack_returned_tuples=True, register_eval=False, register_builtins=False,
                               max_memory=1024 * 1024 + 512 * 1024)
    except TypeError:
        lua = lua54.LuaRuntime(unpack_returned_tuples=True, register_eval=False)
    if lua.eval("math.maxinteger") != 2**31 - 1:  # the board's Lua is LUA_32BITS: so must this one be
        res["error"] = ("check.py's Lua has 64-bit numbers, the board's 32-bit: carts would be checked wrong. "
                        "Rebuild lupa: python tools/build_lupa32.py")
        return res
    try:
        bundle, image_info = pack_images.pack(path.stem, path.parent / "images" / path.stem)
    except pack_images.PackError as e:
        res["error"] = f"its images (images/{path.stem}/ beside it): {e}"
        return res
    if image_info:
        res["images"] = {"count": len(image_info), "bytes": len(bundle)}
    fps = 60 if re.search(r"function\s+_update60\b", src) else 30
    cart = Cart(lua, fps)
    cart.cart_name = path.stem
    if full:
        cart.full, cart.hd_w, cart.hd_h = True, GLASS_W, GLASS_H
        cart.hd_clipr = (0, 0, GLASS_W, GLASS_H)
    cart.images = {i["name"]: i for i in image_info}
    harness = lua.execute(HARNESS, lua.table_from(cart.api()), src, UI_LUA.read_text())
    if isinstance(harness, tuple):  # (nil, err): it didn't compile
        res["error"] = f"doesn't load: {harness[1]}"
        return res
    cart.env = harness.env
    rng = random.Random(7)
    inputs = []  # what each frame saw, for the device runtime's parity test (--dump-input)
    max_ops = total = 0
    shots = {}
    keep = {1, frames // 2, frames}
    try:
        for name in ("main", "_init"):
            ok, err, _ = harness.main() if name == "main" else harness.call("_init")
            if not ok:
                res["error"] = f"error in {'the top of the cart' if name == 'main' else '_init'}: {err}"
                return res
        cart.hd_us, cart.hd_rows = 0.0, set()  # _init's drawing happens once, before the frames
        hd_ms = []  # each frame's estimated device cost of the layer
        idle_frames, quiet_frames, prev_fb = 0, 0, bytes(cart.fb)  # frames at 20+ fps with no input; those that changed nothing
        upd = "_update60" if harness.env["_update60"] else "_update"  # (its name sets the first rate; fps() changes it)
        if not any(harness.env[n] for n in ("_update", "_update60", "_draw")):
            res["error"] = "the cart defines none of _update, _update60, _draw"
            return res
        for f in range(1, frames + 1):
            cart.advance(f)
            script_input(cart, f, rng)
            inputs.append([sum(1 << b for b in range(6) if cart.held[b]),
                           cart.touch_at[0] if cart.touch_at else None, cart.touch_at[1] if cart.touch_at else None,
                           cart.tilt_at[0], cart.tilt_at[1]])
            ok, err, used = harness.frame(upd, BUDGET)
            if not ok:
                res["error"] = f"error in frame {f}: {err}"
                res["frame"] = f
                shots[f] = (bytes(cart.fb), tuple(cart.rgb))
                return res
            max_ops, total = max(max_ops, used), total + used
            fb = bytes(cart.fb)
            if cart.fps >= 20 and not cart.idle and cart.touch_at is None and not any(cart.held):
                quiet_frames += 1
                if fb == prev_fb and not cart.hd_rows and not cart.hd_us:
                    idle_frames += 1
            prev_fb = fb
            cart.settle_rate()
            if cart.hd is not None:
                hd_ms.append((cart.hd_us + len(cart.hd_rows) * HD_ROW_US) / 1000)
                cart.hd_us, cart.hd_rows = 0.0, set()
            if dump_frames:
                (dump_frames / f"frame_{f}.bin").write_bytes(bytes(cart.fb))
            if f in keep:
                shots[f] = (bytes(cart.fb), tuple(cart.rgb))
    except getattr(lua54, "LuaMemoryError", MemoryError):
        res["error"] = "the cart used more than 1 MB of memory"
        return res
    finally:
        if dump_input:
            body = cartrun_input(fps, inputs, cart.wall0)
            if bundle:  # beside it, for cartrun (tests/parity.py)
                (dump_input.parent / "images.hdb").write_bytes(bundle)
                body["images"] = str(dump_input.parent / "images.hdb")
            if live_samples()[0]:
                (dump_input.parent / "live.hdb").write_bytes(live_samples()[0])
                body["live"] = str(dump_input.parent / "live.hdb")
            dump_input.write_text(json.dumps(body))
        res["calls"] = {k: (sorted(v) if isinstance(v, set) else v) for k, v in cart.calls.items()}
        if cart.scene is not None:
            sc = cart.scene
            res["3d"] = {"shapes": len(sc["shapes"]), "objects": sc["objects"], "triangles": sc["tris"]}
        if cart.hd is not None:
            res["hd"] = {"calls": dict(sorted(cart.hd.items()))}
            if hd_ms:
                res["hd"]["device_ms"] = {"avg": round(sum(hd_ms) / len(hd_ms), 1), "max": round(max(hd_ms), 1)}
                slow = [i + 1 for i, v in enumerate(hd_ms) if v > HD_WARN_MS]
                if slow:
                    res["hd"]["slow_frames"] = len(slow)  # a screen's first draw now and then: one late frame each
                if len(slow) > len(hd_ms) * 0.05:  # sustained: most likely the whole layer drawn every frame
                    cart.warnings.append(
                        f"the full-color layer: about {max(hd_ms):.0f} ms of the board's 33 ms frame "
                        f"({len(slow)} of {len(hd_ms)} frames over {HD_WARN_MS}); the layer keeps what's drawn, so draw "
                        "what doesn't move once and repaint only what changes (clip to a box and redraw just it)")
        if preview and shots and cart.scene is None and cart.hd is None:
            render([shots[k] for k in sorted(shots)], theme).save(preview)
            res["preview"] = str(preview)
    if cart.scene is not None or cart.hd is not None:  # through the board's runtime: it must run, and it draws the preview
        run = run_3d(path, fps, inputs, sorted(keep), bundle, cart.wall0)
        if not run.get("ok"):
            res["error"] = "the board's runtime (cartrun) " + (f"stops it: {run.get('error')}" if "error" in run else run.get("why", "failed"))
            return res
        if cart.scene is not None:
            res["3d"]["queued"] = run.get("queued")
        if preview:
            Image.open  # noqa: B018 (PIL reads the PPMs)
            gap = 16
            ims = [Image.open(shot).convert("RGB") for shot in run["shots"]]  # the glass's width (410)
            iw = max(im.width for im in ims)
            ih = max(im.height for im in ims)  # 384 (the window) or 502 (a full canvas: the glass)
            out = Image.new("RGB", (len(ims) * (iw + gap) + gap, ih + 2 * gap), (24, 24, 24))
            for i, im in enumerate(ims):
                out.paste(im, (gap + i * (iw + gap), gap))
            out.save(preview)
            res["preview"] = str(preview)
    res.update(ok=True, frames=frames, fps=fps, max_ops=max_ops, avg_ops=round(total / frames))
    if cart.warnings:
        res["warnings"] = sorted(set(cart.warnings))[:5]
    if api < 2 and re.search(r"\bui\s*\.", re.sub(r"--[^\n]*", "", src)):
        res.setdefault("warnings", []).append(
            "it uses the runtime's ui helpers (API 2): add '-- api: 2' to its header, so a board with older firmware "
            "says it needs an update instead of failing at its first ui call")
    elif cart.hd is not None and api < 2:
        res.setdefault("warnings", []).append(
            "it uses the full-color layer (API 2): add '-- api: 2' to its header, so a board with older firmware "
            "says it needs an update instead of failing at its first hd call")
    if cart.logs:
        res["log"] = cart.logs
    if api < 4 and cart.spaced_sprites:
        res.setdefault("warnings", []).append(
            "its sprites have spaces between rows (API 4): add '-- api: 4' to its header, so a board with older "
            "firmware says it needs an update instead of failing to draw them")
    if api < 4 and cart.logs:
        res.setdefault("warnings", []).append(
            "it calls log() (API 4): add '-- api: 4' to its header, so a board with older firmware says it needs "
            "an update instead of failing at its first log call")
    if api < 3 and (cart.calls.get("music") or re.search(r"^\s*(SOUNDS|MUSIC)\s*=", src, re.M)):
        res.setdefault("warnings", []).append(
            "it has its own sounds or music (API 3): add '-- api: 3' to its header, so a board with older firmware "
            "says it needs an update instead of failing at its first sfx or music call")
    if not full:
        res.setdefault("warnings", []).append(
            "it shows in the 384 x 384 window, about 30% of the glass left dark: add '-- canvas: full' (and '-- api: 2') "
            "and use the whole glass, laid out from caps(): the picture keeps its window, and the layer has the bands "
            "above and below it for a title, the score, what to do (docs/DESIGN.md, 1)")
    if quiet_frames >= 20 and idle_frames >= quiet_frames * 0.6 and cart.scene is None and not getattr(cart, "tilted", False):
        res.setdefault("warnings", []).append(
            f"{idle_frames} of its {quiet_frames} frames without input (at {cart.fps} fps) changed nothing on the screen: give it a lazy rate, fps(60, idle) in _init (60 while touched, a few frames a "
            "second otherwise), and move things by time() (docs/DESIGN.md, 2 and 3)")
    if max(cart.fb) == min(cart.fb) and cart.scene is None and cart.hd is None:
        res.setdefault("warnings", []).append("the last frame is a single color: does _draw draw anything?")
    return res


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cart")
    ap.add_argument("--frames", type=int, default=150)
    ap.add_argument("--preview")
    ap.add_argument("--theme", choices=["pico", "phosphor"], default="pico")
    ap.add_argument("--dump-input", help="write each frame's input as JSON (for tests/parity.py)")
    ap.add_argument("--dump-frames", help="write each frame's picture to DIR/frame_N.bin")
    a = ap.parse_args()
    res = check(Path(a.cart), a.frames, Path(a.preview) if a.preview else None, a.theme,
                Path(a.dump_input) if a.dump_input else None, Path(a.dump_frames) if a.dump_frames else None)
    print(json.dumps(res))
    sys.exit(0 if res["ok"] else 1)


if __name__ == "__main__":
    main()
