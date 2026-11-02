#!/usr/bin/env python3
"""A cart's own pcall can't keep a stopped frame running.

    python tests/hang.py [CARTRUN] [--cli]

tests/carts/hang.lua catches its endless loop's error with pcall, in an endless loop. check.py
(stopped by the instruction budget) and the board's runtime built for the host (stopped by the
100 ms deadline, as on the board) must each end the frame with an error, not hang; with --cli, the
command-line tool too (the budget, as check.py).
"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
cart = HERE / "carts" / "hang.lua"
args = [a for a in sys.argv[1:] if not a.startswith("--")]
cartrun = args[0] if args else str(ROOT / "host" / "build" / "cartrun")
bad = []


def ends(what, cmd, want):
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=60).stdout
    except subprocess.TimeoutExpired:
        bad.append(f"{what}: still running after 60 s")
        return
    res = json.loads(out.strip().splitlines()[-1] if what == "cartrun" else out)
    if res.get("ok") or want not in res.get("error", ""):
        bad.append(f"{what}: {res.get('error')!r}")
    else:
        print(f"{what}: {res['error'].splitlines()[0]}")


ends("check.py", [sys.executable, str(ROOT / "tools" / "check.py"), str(cart)], "over budget")
with tempfile.TemporaryDirectory() as d:
    (Path(d) / "input.json").write_text(json.dumps({"fps": 30, "frames": [[0, None, None, 0, 0]] * 3}))
    ends("cartrun", [cartrun, str(cart), str(Path(d) / "input.json"), d], "ran over 100 ms")
if "--cli" in sys.argv:
    ends("cli", ["node", str(ROOT / "cli" / "luanette.mjs"), "check", str(cart)], "over budget")
if bad:
    sys.exit("\n".join(bad))
