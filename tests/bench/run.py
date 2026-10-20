#!/usr/bin/env python3
"""The full-color layer's benches on the board, over USB: each bench cart is sent, run for two of
the board's 5 s stats windows, read from the log, and removed again. Prints a table, and saves it
(results/NAME.json) for a later run to compare with.

    python tests/bench/run.py [BENCH ...] [--save NAME] [--compare NAME] [--port /dev/ttyACM0]

BENCH: hb_text, ... (all of hb_*.lua when none are named).

Each bench draws the same thing every frame, so its "Lua" is the drawing's cost and its "publish"
the cost of finding that nothing changed (each drawn row read back and hashed). 3D benches print
their prepare (core 1) and drawing (core 0) times instead.
"""
import argparse
import json
import re
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "tools"))
import push  # noqa: E402

RESULTS = HERE / "results"
STATS = re.compile(r"cart (\S+): (\d+) frames in ([\d.]+) s \(([\d.]+)/s, (\d+) late.*?Lua ([\d.]+) ms avg, ([\d.]+) max; "
                   r"sent (\d+) frames, ([\d.]+) rows/frame, ([\d.]+) ms avg(?: \([^)]*\))?(?:, [\d.]+ max, ([\d.]+) kpx/frame)?")
PUBLISH = re.compile(r"cart (\S+): layer publish ([\d.]+) ms avg, ([\d.]+) max")
STAGE = re.compile(r"stage (\S+): (\d+) frames in ([\d.]+) s \(([\d.]+)/s, (\d+) late.*?Lua ([\d.]+) ms avg, ([\d.]+) max(?: \([^)]*\))?; "
                   r"prepare \(core 1\) ([\d.]+) ms avg.*?drawing \(core 0\) ([\d.]+) ms avg.*?sent ([\d.]+) ms avg")


def wait_log(link: push.Link, pattern: re.Pattern, name: str, count: int, timeout: float) -> list[re.Match]:
    """The log's lines matching pattern for this cart, once there are count of them (or timeout)."""
    deadline = time.time() + timeout
    while True:
        link.pump()
        hits = [m for m in (pattern.search(line) for line in link.log) if m and m.group(1) == name]
        if len(hits) >= count or time.time() > deadline:
            return hits
        time.sleep(0.05)


def run(link: push.Link, path: Path) -> dict:
    name = path.stem
    images = HERE / "images" / "bench" if name.startswith("hb_img") else HERE / "images" / "none"
    msgs, _ = push.messages(path, images=images)
    link.log, link.keep_log = [], True
    for m in msgs:
        link.message(m)
    out = {"error": "didn't start"}
    if wait_log(link, re.compile(r"cart (\S+): started"), name, 1, 20):
        # the first window has the start in it (the first frame draws everything): the second is the one
        wait_log(link, re.compile(r"(?:cart|stage) (\S+): \d+ frames in"), name, 2, 15)
        stage, stats = wait_log(link, STAGE, name, 0, 0), wait_log(link, STATS, name, 0, 0)
        pubs = wait_log(link, PUBLISH, name, 0, 0)
        out = {"error": "no stats"}
        if len(stage) >= 2:  # a 3D cart: its scene prepared on core 1 and drawn band by band on core 0
            s = stage[1]
            out = {"fps": float(s.group(4)), "late": int(s.group(5)), "lua_ms": float(s.group(6)),
                   "lua_max": float(s.group(7)), "prepare_ms": float(s.group(8)), "draw_ms": float(s.group(9)),
                   "sent_ms": float(s.group(10)), "rows": 0.0}
        elif len(stats) >= 2:
            s = stats[1]
            out = {"fps": float(s.group(4)), "late": int(s.group(5)), "lua_ms": float(s.group(6)),
                   "lua_max": float(s.group(7)), "rows": float(s.group(9)), "sent_ms": float(s.group(10)),
                   "kpx": float(s.group(11) or 0)}
            if len(pubs) >= 2:
                out["publish_ms"] = float(pubs[1].group(2))
    link.keep_log = False
    link.message({"type": "cart", "delete": name})
    return out


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("benches", nargs="*")
    ap.add_argument("--save")
    ap.add_argument("--compare")
    ap.add_argument("--port")
    a = ap.parse_args()
    paths = [HERE / f"{b.removesuffix('.lua')}.lua" for b in a.benches] or sorted(HERE.glob("hb_*.lua"))
    before = json.loads((RESULTS / f"{a.compare}.json").read_text()) if a.compare else {}
    link = push.Link(a.port)
    results = {}
    print(f"{'bench':<16}{'fps':>6}{'Lua ms':>9}{'publish':>9}{'rows':>7}{'sent ms':>9}{'kpx':>7}   vs before (Lua, publish)")
    for p in paths:
        r = run(link, p)
        results[p.stem] = r
        if "error" in r:
            print(f"{p.stem:<16}  {r['error']}")
            continue
        b = before.get(p.stem, {})
        delta = ""
        if "lua_ms" in b:
            pct = lambda new, old: f"{100 * (new - old) / old:+.0f}%" if old else "-"
            delta = f"   {pct(r['lua_ms'], b['lua_ms'])}, {pct(r.get('publish_ms', 0), b.get('publish_ms', 0))}"
        extra = f"   3D: prepare {r['prepare_ms']:.2f}, draw {r['draw_ms']:.2f}" if "draw_ms" in r else ""
        print(f"{p.stem:<16}{r['fps']:>6.1f}{r['lua_ms']:>9.2f}{r.get('publish_ms', 0):>9.2f}{r['rows']:>7.1f}"
              f"{r['sent_ms']:>9.2f}{r.get('kpx', 0):>7.1f}{delta}{extra}")
    link.ask("cart close")
    if a.save:
        RESULTS.mkdir(exist_ok=True)
        (RESULTS / f"{a.save}.json").write_text(json.dumps(results, indent=1))
        print(f"saved: {RESULTS / (a.save + '.json')}")


if __name__ == "__main__":
    main()
