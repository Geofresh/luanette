#!/usr/bin/env python3
"""The device runtime against check.py, pixel for pixel.

    python tests/parity.py CART.lua [CARTRUN]

Runs the cart in check.py (Lua through lupa) dumping each frame's input and picture, replays the
input through the board's runtime built for the host (engine/host/cartrun.cpp, built in host/build), and
compares every frame; the full-color layer's front buffer is checked against its back after each
frame too ("hd_bad" in cartrun's output: frame * 1000 + row). Carts that use rnd() differ by
design (each side has its own generator).
"""
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import check  # noqa: E402

cart = Path(sys.argv[1])
cartrun = sys.argv[2] if len(sys.argv) > 2 else str(HERE.parent / "host" / "build" / "cartrun")
with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    (d / "py").mkdir()
    (d / "c").mkdir()
    res = check.check(cart, 90, None, "pico", d / "input.json", d / "py")
    if not res["ok"]:
        sys.exit(f"check.py: {res.get('error')}")
    shipped = d / cart.name  # the board runs the cart as it's sent: comments out (check.shipped)
    shipped.write_text(check.shipped(cart.read_text()))
    out = subprocess.run([cartrun, str(shipped), str(d / "input.json"), str(d / "c")], capture_output=True, text=True,
                         env={**os.environ, "HG_HD_VERIFY": "1"})  # and the layer's front buffer checked each frame
    run = json.loads(out.stdout.strip().splitlines()[-1]) if out.stdout.strip() else {"error": out.stderr}
    if not run.get("ok"):
        sys.exit(f"cartrun: {run.get('error')}")
    bad = []
    for f in range(1, res["frames"] + 1):
        a, b = (d / "py" / f"frame_{f}.bin").read_bytes(), (d / "c" / f"frame_{f}.bin").read_bytes()
        diff = [i for i in range(len(a)) if a[i] != b[i]]
        if diff:
            bad.append((f, len(diff), [(i % 128, i // 128, a[i], b[i]) for i in diff[:6]]))
    logs_differ = res.get("log", []) != run.get("log", [])  # log() lines, in order, the same on both
    print(json.dumps({"cart": cart.name, "frames": res["frames"], "frames_differing": len(bad),
                      "first": bad[:3], "logs_differ": logs_differ, "py_max_ops": res["max_ops"], "c": run}))
    sys.exit(1 if bad or logs_differ or run.get("hd_bad") else 0)
