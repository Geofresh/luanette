# The 3D stage

How the 3D stage (`engine/stage.hpp`, `stage.cpp`, `stage_pie.S`) draws, and how it got to 60 fps,
with the measurements behind each decision. The overview is in `ARCHITECTURE.md`.

The stage draws a lit, depth-buffered 3D scene on the cart window in full RGB565 colour, at
384x384 or at 192x192 shown as 2x2. There's no framebuffer: the scene is drawn band by band into
the band buffers just before each band goes out. Meshes, materials and the lighting model come
from Jet (`jet/`, a software rasteriser, vendored and patched; see NOTICE). The pipeline and the
pixel loops are the stage's own.

Most of these measurements were taken on the firmware Luanette grew out of, which shared the
chip with Wi-Fi and a UI. The standalone firmware has far more free internal RAM and runs the
same scenes a few percent faster (`tests/bench/`).

- **Preparing** (core 1, after the cart's Lua, into a back copy while the front one is drawn): an object
  whose bounding sphere is outside the view is skipped whole; one wholly inside skips the per-vertex
  clip and clamp. Faces turned away are found in the object's own space (the camera against each
  face's plane, kept with the mesh), before any vertex work, so vertices only back faces use are
  never transformed. The rest are transformed, projected to 1/16 pixel and lit once (Jet's
  Lambert and view-facing specular, plus the object's material). Divides use the FPU's `fdiv`
  steps, because the toolchain's `/` calls a slow ROM routine. Each front face is set up for the
  span loop (edge slopes, depth and brightness planes, a 64-byte record) and binned by the 16-row
  pieces it crosses. There's no sort: the depth buffer orders the pixels.
- **Drawing**: a span loop in IRAM. Each pixel costs a 16-bit depth test, a lookup in the
  material's brightness-to-colour table (already in the panel's byte order) and two adds. A face
  with one brightness (flat-shaded, unlit, or turned from the sun) is filled in one colour. Edges are walked from their
  upper vertex with their own slope in 32 bits (the 64-bit edge's low 4 bits are the same on every
  row, so the walk and its rounding are exact), so triangles sharing an edge meet without gaps or
  double-drawn pixels (pixel centres, top-left inclusive). Runs of 24 pixels or more go 8 at a
  time on the S3's vector unit (`stage_pie.S`: depth unzipped to 16-bit lanes, tested with a biased
  signed min and compare, colour blended by the mask; a lit face's colour broadcast when its 8
  pixels share a table entry, else looked up one by one). The sky and depth clears go 16 bytes a
  store. `Stage::selftest()` (`check selftest` over USB) draws test scenes with the
  vector loop and without, at every 16-byte phase of the band buffer, and compares them bit for bit.
- **Textures** (`texture`, `skin`): palette texels (4 to 64 each way), mapped affinely (the PS1's
  way), lit through a table of their 16 colours at 64 brightness levels; their own span loop
  (`draw_tex`, a flat face's without the brightness), about 1.6x a plain face's pixel.
- **Half size** (`res`): each frame carries its scale, so a cart can switch between frames while
  the last one is still being drawn. At half size the 410-wide scene is 206 columns (the last
  doubled one clipped). With `res("auto")` the player halves it when a frame's work at full size
  passes 14.5 ms (smoothed), and goes back under 11.
- **Two cores**: each band is drawn in two column ranges at once, the sender's on core 0 and a
  helper's on core 1, sharing the band buffer and the depth rows. The split is half and half,
  unless core 1 (which also prepares) would overrun its 13 ms; then it moves toward core 1's end.
- **Racing the beam**, as 2D carts do: a frame starts a lead before the pass it shows on, and
  each band goes out as soon as it's drawn, ahead of that pass's scan. The lead is the most the
  last 16 frames needed (each band drawn by the time the scan reaches its top, plus its transfer)
  plus 4 ms. A frame that starts too late goes whole behind the scan: a pass later, never a seam.
  The sender runs at priority 10; at 3, other tasks preempted it mid-band and seams showed. Measured: 0 seams in 60 s at the default scene, about 297 of 298
  frames raced.
- **The glass's width**: a 3D cart's scene is 410 wide (the window at x 12 in it, `fov` still across
  the window), and every cart's picture runs its edge columns out to the glass's edges (2D bands go
  out full width too). A picture or scene stopping at the window met the black margin in a hard
  vertical edge, which this AMOLED's subpixel layout draws as a coloured zig-zag (seen on the left
  edge; full-width rows never have one).
- **From Lua** (docs/api.md, "3D"): `shape obj place orient paint hide view light sky ground shadow
  project texture skin res`. A cart's first 3D call makes its stage (`Cart::stage()`); from then on its picture is an
  overlay (colour 0 clear, with scanlines; `overlay_rows`), composited onto each band by
  the sender, and core 1 runs the cart's Lua then prepares the next frame. Planar shadows: an
  object's faces turned to the sun, dropped onto the ground plane, unlit. `cartrun` renders 3D carts
  as the board does (`frame_<n>.ppm`); check.py uses it. A full-window object costs the span loop
  every pixel (a felt box: 7 ms a frame): make big flat backgrounds the sky.
- Test scenes open by name over USB: `cart open @stage`, `@stage<segments>`,
  `@stagehalf<segments>` (192x192) and `@stagejet<segments>` (Jet's own pipeline and rasteriser,
  for comparison). `tests/bench/stage_vs_jet.py` uses them.
- Memory: nothing in internal RAM while no stage is open (Jet's code runs from flash and its trig
  tables are made in PSRAM by the first scene; it used to be 39 KB of IRAM and .bss at all times).
  An open stage takes about 13 KB: depth and colour pieces, and the helper's stack.

Measured 2026-10-14 on the board, before racing the beam (frames sent behind the scan), with two
Gouraud rings:

| triangles | 192x192 | 384x384 |
|---|---|---|
| 648 | 59.7 fps (Jet's rasteriser: 56-58, and 47 on one core) | 59.4 |
| 1568 (the default) | | 59 fps, a late band in ~1000 |
| 2048 | 59.0 | 58.4 |
| 3200 | 59.0, no late band | 32 |
| 4608 | 32 | |

The way there, at 192x192 / 648 triangles unless noted: Jet whole at 384x384 on one core 11 fps
(a triangle-list walk per band, a divide a pixel); binning; the drawing sender starved core 0's
lock holders (1 fps); half resolution, a double-buffered scene, Jet's FAST_Z, two cores: 59 at
648, 30 at 2048. Then the span loop (drawing 9 + 8 ms to 3.5 + 3.0), then the stage's own
pipeline instead of Jet's queue and sort (2048 triangles 30 to 59). Past ~3000 triangles core 1's
preparing is the limit (~5 us a triangle, mostly its records streaming through PSRAM).

2026-10-16 (racing, `tests/bench/stage_vs_jet.py`):

| triangles (facing) | 192x192 | 384x384 | before (10-15) |
|---|---|---|---|
| 2048 (955) | 59.2 fps, 13.2 ms work | 59.4, 15.6 | 59.2 / 59.2 |
| 3200 (1490) | 59.2, 17.4 | 59.0, 20.6 | 59.0 / 53.9 |
| 4608 (2150) | 45-50, 22.6 | 32.6, 26.8 | 32.1 / 29.7 |

Jet's metric (drawn triangles over render time) peaks at 95k/s, lit and depth-tested. What it
took: the vector span loop and 32-bit edges (drawing about 10% less; 11-13% where faces are big),
the 64-byte record (from 92), per-vertex brightness, object culling and the object-space face
test (vertices 3.6 to 2.6 ms at 4608). Measured on the way, for next time:
- A 64-byte record written to PSRAM costs ~1.3 us (the line fetched before it's written, and
  written back): about 1.3 ms of a 4608-triangle frame's setup. Internal RAM couldn't hold them
  while a running cart left only about 14 KB free. The standalone firmware leaves far more, so
  this is worth trying again.
- Setting faces up as the pieces draw them (20-byte faces to PSRAM instead of records) took 3.5 ms
  off preparing and put more than that on drawing, which races the scan: 7-14 fps worse. Reverted.
- The data cache's hardware prefetch (`Cache_Enable_DCache_Autoload` over the PSRAM range) panics
  the chip (cache error) at once. Don't.
- The S3 runs float divides through the ROM's `__divsf3` and spills floats to the stack freely:
  the vertex loop got its 1.4x from constants kept in registers and the depth test first.

