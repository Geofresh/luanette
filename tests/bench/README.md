# Benchmarks

What drawing costs on the board. Each `hb_*.lua` cart does one thing every frame (the same thing,
so its Lua time is the drawing's cost and its publish time the cost of finding that nothing
changed). `tools/check.py`'s cost estimate (`HD_NS`, `HD_ROW_US`) comes from these.

```bash
python tests/bench/run.py --save before      # all of them, about 4 minutes
python tests/bench/run.py --compare before   # after a change: the difference per bench
python tests/bench/run.py hb_text hb_card    # just these
python tests/bench/stage_vs_jet.py           # the 3D stage against Jet's own pipeline
```

Each bench is sent over USB, runs for two of the firmware's 5 s stats windows (the second is the
one reported: the first includes the start, when everything is drawn), and is removed again.
Results land in `results/`.

Measured 2026-10-19 with firmware 0.1.0 (Lua ms a frame / publish ms):

| bench | what it draws | Lua | publish |
|---|---|---|---|
| hb_noop | nothing | 0.14 | 0.03 |
| hb_clear | the layer cleared | 6.30 | 5.65 |
| hb_grad | a full-window gradient | 7.15 | 5.65 |
| hb_blend2 | a translucent rect over an opaque one | 14.51 | 5.64 |
| hb_blend | a translucent rect over itself | 48.48 | 8.53 |
| hb_text | 5 lines of text | 3.61 | 1.86 |
| hb_circ | 5 circles, r 22 | 2.07 | 1.06 |
| hb_ring | 2 rings | 3.04 | 2.76 |
| hb_line | 2 lines | 1.92 | 2.38 |
| hb_card | a card | 3.08 | 2.23 |
| hb_img | a 128 px photo at its own size | 1.57 | 2.93 |
| hb_imgscale | the photo over the whole window | 6.97 | 5.68 |
| hb_imgalpha | 20 soft-edged hearts | 12.14 | 4.31 |

The 3D benches all run at 59.6-59.7 fps: hb_3d0 (no layer), hb_3de (an empty layer), hb_3do
(an opaque card over the scene), hb_3d (a translucent one).

Stage against Jet, same scene and resolution (192x192), 3,200 triangles: Jet 25.9 fps
(48.8 ms of work a frame), the stage 59.2 fps (17.3 ms). At 384x384 the stage still holds
59.2 fps (20.5 ms).

The images in `images/bench/` are generated for these benches (a synthetic photo and a heart);
any 128 px photo and 48 px RGBA icon would do.
