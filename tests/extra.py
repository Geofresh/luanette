#!/usr/bin/env python3
"""A port's own calls reach a cart the same way on the runtime and in check.py.

    python tests/extra.py [CARTRUN]

cartrun registers twice(n) through Host::extra_api when HG_TEST_EXTRA is set; check.py gets the same
call through its EXTRA. tests/carts/extra.lua logs caps().extras and twice(21) on both, and the
logs must match.
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

cart = HERE / "carts" / "extra.lua"
cartrun = sys.argv[1] if len(sys.argv) > 1 else str(HERE.parent / "host" / "build" / "cartrun")
check.EXTRA["twice"] = lambda c, n: int(n) * 2
with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    (d / "py").mkdir()
    (d / "c").mkdir()
    res = check.check(cart, 5, None, "pico", d / "input.json", d / "py")
    if not res["ok"]:
        sys.exit(f"check.py: {res.get('error')}")
    out = subprocess.run([cartrun, str(cart), str(d / "input.json"), str(d / "c")], capture_output=True, text=True,
                         env={**os.environ, "HG_TEST_EXTRA": "1"})
    run = json.loads(out.stdout.strip().splitlines()[-1])
    want = ["1 twice 42"]
    print(json.dumps({"check.py": res.get("log"), "cartrun": run.get("log")}))
    sys.exit(0 if res.get("log") == want and run.get("log") == want else 1)
