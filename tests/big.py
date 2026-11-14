#!/usr/bin/env python3
"""Carts over 32 KB (API 8): checked by their compressed size, run by the runtime, sent compressed.

    python tests/big.py [CARTRUN]

- A 70 KB cart that compresses well passes check.py, runs in cartrun the same as in check.py, and
  push.py sends it as "lua_z", which inflates back to the code.
- One that doesn't compress to 32 KB is refused, saying so; one over 128 KB too.
"""
import base64
import json
import random
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "tools"))
import check  # noqa: E402
import push  # noqa: E402

cartrun = sys.argv[1] if len(sys.argv) > 1 else str(HERE.parent / "host" / "build" / "cartrun")
HEAD = "-- title: Big\n-- by: tests\n-- about: a cart over 32 KB\n-- api: 8\n"


def cart(body: str) -> str:
    return HEAD + body + "\nfunction _draw()\n  cls(1)\n  print(#LINES .. ' ' .. LINES[#LINES], 2, 2, 7)\nend\n"


def lines(n: int, text) -> str:
    return "LINES = {\n" + "".join(f'  "{text(i)}",\n' for i in range(n)) + "}\n"


failed = 0


def expect(what, ok, detail=""):
    global failed
    print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail and not ok else ""))
    failed += not ok


with tempfile.TemporaryDirectory() as d:
    d = Path(d)
    # 70 KB of a level's rows: repetitive, as maps and tables are, so it compresses well
    good = d / "big.lua"
    good.write_text(cart(lines(1300, lambda i: ("1234....ab" * 6)[: 40 + i % 20])))
    res = check.check(good, 30, None, "pico")
    expect("a 70 KB cart passes check.py", res["ok"] and res["bytes"] > 64 * 1024, json.dumps(res)[:300])
    expect("and says its compressed size", 0 < res.get("packed_bytes", 0) <= check.MAX_PACKED, str(res.get("packed_bytes")))
    (d / "input.json").write_text(json.dumps({"fps": 30, "frames": [[0, None, None, 0, 0]] * 30}))
    out = subprocess.run([cartrun, str(good), str(d / "input.json"), str(d)], capture_output=True, text=True)
    run = json.loads(out.stdout.strip().splitlines()[-1]) if out.stdout.strip() else {"error": out.stderr}
    expect("and runs in cartrun", run.get("ok") is True, str(run)[:300])
    msgs, _ = push.messages(good)
    body = msgs[-1]
    code = check.shipped(good.read_text()).encode()
    expect("push.py sends it compressed", "lua_z" in body and "lua" not in body and body["size"] == len(code))
    expect("which inflates to its code", zlib.decompress(base64.b64decode(body["lua_z"]), -15) == code)
    small = d / "small.lua"
    small.write_text(cart(lines(10, lambda i: "short")))
    expect("a small cart still goes as it is", "lua" in push.messages(small)[0][-1])
    # 50 KB that doesn't compress (random letters)
    rng = random.Random(3)
    bad = d / "noise.lua"
    bad.write_text(cart(lines(800, lambda i: "".join(rng.choice("abcdefghijklmnopqrstuvwxyz0123456789") for _ in range(60)))))
    res = check.check(bad, 30, None, "pico")
    expect("one that doesn't compress to 32 KB is refused", not res["ok"] and "compressed" in res.get("error", ""), str(res.get("error")))
    huge = d / "huge.lua"
    huge.write_text(cart(lines(3000, lambda i: "x" * 50)))
    res = check.check(huge, 30, None, "pico")
    expect("one over 128 KB is refused", not res["ok"] and "131072" in res.get("error", ""), str(res.get("error")))
sys.exit(1 if failed else 0)
