#!/usr/bin/env python3
"""The 3D stage against Jet's own pipeline, head to head on the board: the same lit, depth-buffered
test scene (two Gouraud rings), the same resolution (192x192 doubled: Jet's mode), at a range of
triangle counts; the stage at full resolution (384x384) too, for reference.

    python tests/bench/stage_vs_jet.py [--segs 16,20,...] [--save NAME] [--port /dev/ttyACM0]

Each run opens "@stagejet<segs>" (Jet's scene prepare, queue and span loop), "@stagehalf<segs>"
(the stage's own) and "@stage<segs>" (the stage at 384x384), scenes the firmware builds in, and
reads the second of the board's 5 s stats windows (the first has the start in it). A scene has
2 * segs^2 triangles; about half face the camera ("queued", what gets rasterised).

Reported, per run:
  fps            frames shown a second (the panel refreshes at 59.7; a frame waits for its pass)
  render ms      prepare (core 1) + drawing (core 0) a frame: the work, without waiting for the panel
  Jet's metric   rasterised triangles / render time: "summed counts divided by summed render
                 microseconds, excluding pacing and scanout waits" (Jet's VALIDATION.md)
  delivered      rasterised triangles x fps: triangles actually put on the glass a second
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

STAGE = re.compile(r"stage (\S+): (\d+) frames in ([\d.]+) s \(([\d.]+)/s, (\d+) late.*?prepare \(core 1\) ([\d.]+) ms avg.*?"
                   r"drawing \(core 0\) ([\d.]+) ms avg, [\d.]+ max, (\d+) triangles queued")


def run(link: push.Link, name: str) -> dict:
    link.log, link.keep_log = [], True
    link.ask(f"cart open {name}")
    deadline = time.time() + 30
    try:
        while time.time() < deadline:
            link.pump()
            hits = [m for m in (STAGE.search(line) for line in link.log) if m and m.group(1) == name]
            if len(hits) >= 2:
                m = hits[1]
                fps, prep, draw, queued = float(m.group(4)), float(m.group(6)), float(m.group(7)), int(m.group(8))
                render = prep + draw
                return {"fps": fps, "late": int(m.group(5)), "prepare_ms": prep, "draw_ms": draw, "render_ms": render,
                        "queued": queued, "jet_metric": queued / (render / 1000), "delivered": queued * fps}
            time.sleep(0.05)
        return {"error": "no stats"}
    finally:
        link.keep_log = False


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--segs", default="16,20,24,28,32,40,48")
    ap.add_argument("--save")
    ap.add_argument("--port")
    a = ap.parse_args()
    segs = [int(s) for s in a.segs.split(",")]
    link = push.Link(a.port)
    results = {}
    print(f"{'scene':>6} {'tris':>5}  {'pipeline':<22}{'fps':>6}{'render ms':>11}{'queued':>8}{'Jet metric':>12}{'delivered':>11}")
    for s in segs:
        for mode, label in [("@stagejet", "Jet (192x192)"), ("@stagehalf", "stage (192x192)"), ("@stage", "stage (384x384)")]:
            name = f"{mode}{s}"
            r = run(link, name)
            r["triangles"] = 2 * s * s
            results[name] = r
            if "error" in r:
                print(f"{s:>6} {2 * s * s:>5}  {label:<22}{r['error']}")
                continue
            print(f"{s:>6} {2 * s * s:>5}  {label:<22}{r['fps']:>6.1f}{r['render_ms']:>11.1f}{r['queued']:>8}"
                  f"{r['jet_metric'] / 1000:>10.0f}k/s{r['delivered'] / 1000:>9.0f}k/s")
    link.ask("cart close")
    if a.save:
        out = HERE / "results" / f"{a.save}.json"
        out.parent.mkdir(exist_ok=True)
        out.write_text(json.dumps(results, indent=1))
        print("saved", out)


if __name__ == "__main__":
    main()
