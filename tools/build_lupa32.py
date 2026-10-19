#!/usr/bin/env python3
"""Builds lupa for check.py with the board's numbers: 32-bit integers and floats (LUA_32BITS).

    python tools/build_lupa32.py [VERSION]   # into the Python that runs check.py (a venv)

The board's Lua is built with LUA_32BITS (engine/lua), so a cart's integers wrap
past 2^31 and its fractions keep about 7 digits. check.py has to run carts the same way, or a
cart that overflows (now() * 1000) or rounds onto another pixel passes here and misbehaves there.
lupa's wheels are 64-bit, and its bundled Lua 5.4 pins LUA_32BITS to 0 in luaconf.h, so this
fetches the sdist, gives luaconf.h the #ifndef the firmware's copy has, and installs it into this
venv with -DLUA_32BITS=1. Run it again whenever lupa gets reinstalled; check.py refuses to run
on a 64-bit Lua and says so.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import sysconfig
import tarfile
import tempfile
import urllib.request
from pathlib import Path

version = sys.argv[1] if len(sys.argv) > 1 else "2.8"
if not (Path(sysconfig.get_paths()["include"]) / "Python.h").exists():
    sys.exit("This Python has no development headers (Python.h), so lupa can't be compiled. Install them "
             "(Fedora: python3-devel; Debian/Ubuntu: python3-dev) or use a Python that has them (uv, pyenv).")
meta = json.load(urllib.request.urlopen(f"https://pypi.org/pypi/lupa/{version}/json"))
sdist = next(u for u in meta["urls"] if u["packagetype"] == "sdist")
work = Path(tempfile.mkdtemp(prefix="lupa32-"))
archive = work / sdist["filename"]
urllib.request.urlretrieve(sdist["url"], archive)
with tarfile.open(archive) as t:
    t.extractall(work / "src", filter="data")
src = next((work / "src").iterdir())

conf = src / "third-party" / "lua54" / "luaconf.h"
text = conf.read_text()
pinned = "#define LUA_32BITS\t0\n"
if text.count(pinned) != 1:
    sys.exit(f"{conf}: LUA_32BITS isn't defined the way this script expects; look at it by hand")
conf.write_text(text.replace(pinned, "#ifndef LUA_32BITS  /* carts check.py: 32-bit numbers, as the board */\n"
                                     + pinned + "#endif\n"))

env = dict(os.environ, CFLAGS=(os.environ.get("CFLAGS", "") + " -DLUA_32BITS=1").strip())
subprocess.run([sys.executable, "-m", "pip", "install", "--force-reinstall", "--no-cache-dir", str(src)], env=env, check=True)
out = subprocess.run([sys.executable, "-c", "from lupa import lua54; print(lua54.LuaRuntime().eval('math.maxinteger'))"],
                     capture_output=True, text=True, check=True).stdout.strip()
if out != "2147483647":
    sys.exit(f"lupa installed, but its Lua's largest integer is {out}, not 2^31 - 1: LUA_32BITS didn't take")
shutil.rmtree(work)
print(f"lupa {version} with 32-bit Lua numbers installed in {sys.prefix}")
