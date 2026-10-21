# Carts: the API

A cart is one Lua 5.4 file. It draws a 128x128 picture, shown 3x as 384x384, and can add a 3D
scene under it and a full-color layer at the full 384x384.

This file is the contract. `check.py` tests carts against it, and the runtime in `engine/`
implements the same calls, drawing exactly the pixels `check.py`'s preview shows
(`tests/parity.py` checks that). For how to build a cart that runs well (frame rates, drawing
only what changed, scrolling, measuring), read `DESIGN.md` first.

## Shape of a cart

```lua
-- title: Tilt Snake
-- by: you
-- about: eat the dots, tilt or press to steer

function _init()   -- once, at start
end

function _update() -- 30 times a second (define _update60 instead for 60; or fps(), below)
end

function _draw()   -- after every update
end
```

The three header comments are required: `title`, `by` and `about`. The title shows in the
launcher. Two more are optional:

- `-- api: 2` says which version of this API the cart needs (see Versions). Give it to any cart
  that uses the full-color layer.
- `-- canvas: full` gives the full-color layer the whole glass (the entire 410x502 screen,
  corners and all) instead of the 384x384 window (see The full-color layer).

Everything else is up to the cart. Globals persist between frames.

## Versions and what a cart runs on

The API has a version. Version 1 is everything before the full-color layer; version 2 adds the
layer (`hd`), its images, the `ui` helpers, `caps()` and `touch(true)`; version 3 adds the cart's own
sounds and music (`SOUNDS`, `MUSIC`, `sfx(8-63)`, `music()`). A runtime older than a
cart's `-- api: N` refuses it with a message saying so ("it needs API 2; this runtime has 1
(update the firmware)") instead of failing at the first call it doesn't know. `check.py` refuses
a version the board doesn't have, and asks for `-- api: 2` when a cart uses the layer.

`caps()` says what the cart runs on, for carts that adapt. On this board:

| field | the board (Waveshare AMOLED-2.06) |
|---|---|
| `w`, `h` | the canvas in the panel's pixels: 384, 384 (the window); with `-- canvas: full`, the glass: 410, 502 |
| `picture_x`, `picture_y` | where the 128 x 128 picture sits on the canvas: 0, 0; on a full canvas 12, 60 |
| `corner` | the glass's corners' radius on a full canvas: 100 (0 for the window). `hd.inset` does the sums |
| `scale` | the panel's pixels for each canvas unit: 1 (the canvas is always 384 x 384 units; another board scales it) |
| `round` | a round window whose corners don't show: false |
| `touch`, `tilt`, `buzz`, `sound` | true, true, false (no vibration motor is fitted), true (the speaker plays `sfx`) |
| `hd`, `stage` | the full-color layer and the 3D scene: true, true |
| `host` | `data()` and `act()` reach the console: true |
| `version` | the API version the runtime has: 3 |
| `board` | `"waveshare-amoled-206"` (`"browser"` in the playground) |

## Limits

- Source: at most 32 KB, not counting comments. Comments are stripped before a cart is sent or
  run (the header lines stay), so comment freely.
- Time: 30,000 Lua instructions per frame (`_update` plus `_draw`); the API's own work, drawing
  and input, doesn't count. `check.py` stops a cart that goes over. The board doesn't count
  instructions, since that would slow every one of them. Instead it stops a frame that's still
  running Lua after 100 ms, which catches endless loops. A cart within the budget holds its frame
  rate: 30,000 instructions is at most about 15 ms of a 16.7 ms frame, and typical carts use a few
  thousand.
- Memory: 1 MB.
- Numbers are 32-bit: integers to about ±2 billion (past that they wrap around: `now() * 1000`
  overflows), fractions to about 7 digits (the board's FPU is single precision). check.py's Lua
  is built the same way (`tools/build_lupa32.py`), so it catches these.
- No `io`, `os`, `require`, `load`, `dofile`, `debug`, `package`. `math`, `string`, `table`,
  `coroutine`, `pairs`, `ipairs`, `tostring`, `tonumber`, `type`, `select`, `pcall`, `error`,
  `assert`, `setmetatable`, `getmetatable`, `next`, `rawget`, `rawset`, `unpack` are there.
- A cart can't block or exit. PWR or a swipe down from the top edge leaves it. While the screen
  is dark, no frames run; `time()` keeps counting, and `wake()` can light the screen again when
  something is due.

## Screen and colors

128x128 pixels, (0,0) top left. 16 colors, by index:

| 0-3 | 4-7 | 8-11 | 12-15 |
|---|---|---|---|
| 0 black | 4 brown | 8 red | 12 blue |
| 1 dark blue | 5 dark gray | 9 orange | 13 lavender |
| 2 dark purple | 6 light gray | 10 yellow | 14 pink |
| 3 dark green | 7 white | 11 green | 15 peach |

That's the PICO-8 palette. On the board each pixel's bottom third is a little darker, like a
scanline. The window is narrower than the glass, so the picture's leftmost and rightmost columns
carry on out to the glass's edges: a background color, or a bar drawn edge to edge, fills the
screen's width.

## Drawing

All coordinates are numbers (fractions are floored). The color `c` is optional where shown in
brackets; it defaults to the last color used (7 at start).

| call | does |
|---|---|
| `cls([c])` | clear the screen (default 0) |
| `pset(x, y, [c])` / `pget(x, y)` | set / read one pixel (`pget` returns the color index) |
| `line(x0, y0, x1, y1, [c])` | a line |
| `rect(x0, y0, x1, y1, [c])` / `rectfill(x0, y0, x1, y1, [c])` | a box outline / filled box (corners inclusive) |
| `circ(x, y, r, [c])` / `circfill(x, y, r, [c])` | a circle outline / filled circle |
| `trifill(x0, y0, x1, y1, x2, y2, [c])` | a filled triangle. Fractions count (to 1/16 px): triangles sharing an edge meet with no gap and no overlap, so meshes tile cleanly. For solid 3D: drop faces turned away, draw far to near |
| `print(text, x, y, [c])` | text in the 4x6 font (3x5 glyphs, 4 px a character, so 32 a line); returns the x after the text. Letters show as capitals; `\n` starts a new line at `x`; whole numbers print without `.0` |
| `spr(n, x, y, [flip_x], [flip_y])` | sprite `n` from `SPRITES` (below), color 0 transparent |
| `pal(c0, c1)` / `pal()` | draw color `c0` as `c1` from now on / reset |
| `palette(i, 0xRRGGBB)` / `palette(i)` / `palette()` | change what color index `i` (0-15) looks like, for the whole picture (this frame included) / its color now / the starting 16 again. A cart still has 16 colors at a time; this picks which 16 (an exact red, softer pastels). `pal` remaps indices, `palette` the colors behind them |
| `camera(x, y)` / `camera()` | offset everything drawn by (-x, -y) / reset |
| `clip(x, y, w, h)` / `clip()` | draw only inside a rectangle / reset |

Sprites: a global table `SPRITES` of 8x8 images, each a string of 64 hex digits (one color index
a pixel, row by row). `SPRITES[1] = "0000000000777700..."` and so on; `spr(1, x, y)` draws it.

Per-pixel `pset` over the whole screen every frame is slow on the board; draw with `rectfill`,
`circfill`, `line`, `spr` and `print`.

## 3D

A cart can draw a lit, depth-buffered 3D scene in full color under its picture, at the window's
full resolution (384x384, not the picture's 128x128). It starts with the first 3D call; from then
on the picture is an overlay: **color 0 shows the scene through**, every other color draws over it
(so `cls(0)` clears the overlay, and HUD text, labels and sprites sit on top). A cart without 3D
calls costs nothing more.

| call | does |
|---|---|
| `shape("box", w, [h, d])` | a box (h, d default to w) |
| `shape("sphere", r, [segs])` | a sphere (segs around, default 16) |
| `shape("torus", R, r, [segs])` | a ring around the z axis (facing the camera at the start) |
| `shape("cylinder", r, h, [segs])` / `shape("cone", r, h, [segs])` | along y (the cone's point up) |
| `shape(verts, faces, [smooth, uvs])` | your own: `verts` a flat list `{x, y, z, x, y, z, ...}`, `faces` a flat list of vertex numbers (from 1) in threes, each a triangle. Faces are turned outward from the shape's center, so wind them any way (meant for convex shapes: dice, gems). Flat-shaded unless `smooth`. `uvs`: texture coordinates `{u, v, u, v, ...}`, a pair a vertex, 0 to 1 across a texture (it repeats past them) |
| `obj(shape, color, [shine, unlit])` | an object: the shape placed in the scene, in `color` (`0xRRGGBB`); `shine` 0-255 adds a highlight; `unlit` shows the color as it is. Returns its number |
| `place(o, x, y, z, [rx, ry, rz])` | move object `o` there, turned by `rx, ry, rz` degrees (about x, then y, then z) |
| `orient(o, m1, ..., m9)` / `orient(o, {m1, ..., m9})` | turn it by a rotation matrix (rows), for carts that keep their own (physics) |
| `paint(o, color, [shine, unlit])` | recolor it |
| `hide(o, [hidden])` | hide it (`hide(o, false)` shows it again) |
| `view(x, y, z, [yaw, pitch, fov])` | the camera: at x, y, z, looking along +z (yaw 90 looks along +x, pitch 90 up); fov in degrees (60). Default `view(0, 0, -420)` |
| `light(azimuth, elevation, [intensity, ambient])` | the sun's direction in degrees, its strength 0-255 (255), and the shadows' color (`0x22283E`) |
| `sky(top, [bottom])` | the background, a gradient from top to bottom (`0xRRGGBB` each) |
| `ground(y, [color])` | shadows fall on the plane at height `y`, in `color` (the ground's color in shadow, `0xRRGGBB`; default black) |
| `shadow(o, [on])` | object `o` casts a shadow along the sun onto the ground (`shadow(o, false)` stops it). Costs its faces again |
| `project(x, y, z)` | where a point in the scene lands on the picture: `x, y, depth` (picture pixels, for labels), or `nil` behind the camera |
| `texture(w, h, texels)` | a texture: `w` x `h` texels (4, 8, 16, 32 or 64 each way), `texels` a string of color numbers as `SPRITES` are (hex digits row by row; spaces and line breaks ignored), in the palette as it is when made (`palette()` first for other colors). Returns its number (up to 16) |
| `skin(o, tex, [ru, rv])` | object `o` in texture `tex`, repeated `ru` x `rv` times across its shape (default 1; `rv` defaults to `ru`), lit as its `shine` and `unlit` say (its color is set aside; `skin(o)` brings it back). A box's faces each show the whole texture, a sphere's, cylinder's and cone's wrap around their side (caps across), a torus's around and through its tube |
| `res(mode)` / `res()` | the scene drawn at `"full"` size (384 x 384, the default), `"half"` (192 x 192, each pixel shown 2x2: a quarter of the drawing, chunkier), or `"auto"` (the board draws at half while frames run over and goes back to full when there's room) / the size it's drawn at now |

World units are yours (y up; the default camera sees about ±240 units across at z = 0). Shapes are
made once (in `_init`); move objects every frame. Limits: 64 shapes, 128 objects, 64 color and
shine pairs (a texture with a shine counts as one), 16 textures, 16,384 vertices and 16,384
triangles in all the shapes together. Triangles nearer the camera than 2 units are dropped. What
holds 60 fps: about 1,500 triangles facing the camera at once (a sphere of 16 segments is 224 of
them, about half facing you; a box 12, a d20 20). At about 2,000 facing, it drops to 45-50 fps
at half size and about 32 at full size. Objects out of view cost
almost nothing (each is skipped whole), and so do faces turned away. Big surfaces cost by their
pixels: a textured one about 1.6 times a plain one, and `res("half")` a quarter of either.

Textures are mapped the PS1's way: straight across each triangle on the screen, so a big triangle
close to the camera bends its texture (a floor made of a few huge triangles swims as you move).
Smaller triangles bend less: build floors and walls from a grid of them. Textures repeat, with no
smoothing between texels (crisp, and a fine texture far away shimmers).

check.py checks the calls and the limits; it draws a 3D cart's preview with the board's own
renderer (`cartrun`, built in `host/build`), and fails the cart if that runtime stops it.

## The full-color layer (`hd`)

Next to the 128x128 picture, a cart can draw in full color at the window's full resolution: a
384x384 canvas (the picture's 3x, so picture pixel `(x, y)` is canvas `(3x, 3y)` to `(3x+2, 3y+2)`),
with smooth edges, any `0xRRGGBB` color, transparency, and text in real fonts. It starts with the
first `hd.` call (a cart without one costs nothing more). Its edge columns carry on out to the
glass like the picture's, and it gets the same scanlines unless turned off.

**The whole glass.** `-- canvas: full` in the header makes the canvas the glass itself (410 x 502
on this board, `caps().w` / `h`), in glass pixels; the picture keeps its window at
`caps().picture_x`, `picture_y` (picture pixel x is canvas `picture_x + 3x`), and `touch(true)`
gives glass pixels anywhere on it. The glass's corners are rounded: `hd.inset(y, h)` says how far
in a band of rows is visible. A swipe down from its top 24 rows still leaves the cart.

**The canvas keeps what's drawn on it.** It stays until it's drawn over or cleared. Draw what doesn't move
once (in `_init`, or when it changes) and each frame repaint only what moved or changed: the
runtime's `ui` helpers (below) make that the easy way to write a screen. Clearing and repainting
the whole canvas every frame costs most of the board's frame on its own.

| call | does |
|---|---|
| `hd.clear([rgb, a])` | wipe the canvas (inside the clip) to clear, or to a color at alpha `a` (255) |
| `hd.order("over" \| "under")` | the layer over the picture (default), or under it: then the picture's **color 0 shows the layer through**, as over a 3D scene. Under a 3D scene's picture too: scene, layer, picture, in that order when "under" |
| `hd.scanlines(on)` / `hd.scanlines()` | the scanlines on the layer (default on) / whether they're on |
| `hd.rect(x, y, w, h, rgb, [a, radius])` | a filled box, its corners rounded by `radius` |
| `hd.frame(x, y, w, h, rgb, [width, radius, a])` | a box's outline, `width` px (1) |
| `hd.circle(x, y, r, rgb, [a])` / `hd.ring(x, y, r, rgb, [width, a])` | a filled circle / a ring `width` px wide inside radius `r` |
| `hd.line(x0, y0, x1, y1, rgb, [width, a])` | a line `width` px wide (1), round ends |
| `hd.gradient(x, y, w, h, top, bottom, [a])` | a box shading from `top` to `bottom`, ordered-dithered (RGB565 has 32 steps of red and blue: a slow dark gradient would band) |
| `hd.text(text, x, y, rgb, [size, font, align, a])` | text from (x, y) = the top of its line; returns its width. `font`: `"mono"` (JetBrains Mono, 12/14/16/20/24 px), `"bold"` (16/18/24/32) or `"vt"` (VT323, big terminal digits: 30/46/72, ASCII only); `size` snaps to the nearest the font has (16). `align`: `"left"`, `"center"` or `"right"` of x. Latin-1, punctuation, arrows, box drawing, ★ ♥ ✓ and friends; anything else shows as a small box |
| `hd.measure(text, [size, font])` | its width and line height |
| `hd.wrap(text, width, [size, font])` | a table of lines that fit `width` (at spaces; `\n` breaks; a word too long is broken) |
| `hd.clip(x, y, w, h)` / `hd.clip()` | draw only inside a box / anywhere (inside an `hd.repaint`: inside the box cut to the repaint's / the repaint's box) |
| `hd.repaint(x, y, w, h, fn, ...)` | calls `fn(...)` with all drawing kept inside the box, then puts the clip back: part of the screen drawn again by the code that draws it. `fn` paints the box's background too (the old pixels are there). Repaints nest (the inner box is cut to the outer); `fn` isn't called for an empty box |
| `hd.inset(y, [h])` | how far in from each side rows `y` to `y + h` show on this glass (its corners are rounded): keep a bar or a line of text that far in. 0 for the window, and for rows clear of the corners |
| `hd.scroll(x, y, w, h, dy)` | moves what's in the box by `dy` rows (down when positive); the strip it uncovers keeps stale pixels, for the cart to draw again. The whole width (`x` 0, `w` the canvas's) moves nothing (the rows are re-mapped): scrolling a list costs only its new strip. A narrower box copies its pixels. `ui.scroller` does all this |
| `hd.image(name, x, y, [w, h, a])` | draws an image (below) with its top left at x, y (to the pixel), at its own size or scaled to w x h (nearest pixel), at alpha `a` over its own; returns `true`, or `false` when there's no such image (yet) |
| `hd.images()` | the names of the cart's own images |

**The `ui` helpers.** They come with the runtime (`engine/ui.lua`, loaded before every cart; `check.py`
runs the same file). A screen written with them redraws a part only when what it shows changes,
and scrolls by moving what's already drawn. The launcher (`engine/builtin/launcher.lua`) is built
on them.

| call | does |
|---|---|
| `ui.changed(key, ...)` | `true` the first time, and whenever the values differ from those given for `key` last time (one by one, with `==`): ask each frame what a part of the screen shows, and repaint it when the answer is `true`: `if ui.changed("score", score, lives) then hd.repaint(0, 40, W, 60, draw_score) end`. Ask about a part every frame it's on screen, also on a frame the whole screen is drawn anyway, so what's remembered stays what's shown |
| `ui.scroller(x, y, w, h, draw)` | a box whose content (a list, a page) scrolls: `draw(top, y0, y1)` draws the content with its first row at screen row `top`, and only screen rows `y0` to `y1` are being painted (skip what's outside them). Returns `s`: |
| `s:content(h)` | the content's height (the scroll stays in range) |
| `s:drag()` | in `_update`: a touch that begins inside the box drags the content |
| `s:draw()` | in `_draw`: brings the box up to date: moves what's drawn by the scroll and paints the strip it uncovers (the whole box the first time, or after `s:place`) |
| `s:repaint()` / `s:redraw(cy, ch)` | the whole box drawn again / content rows `cy` to `cy + ch`, where they show (one changed item) |
| `s.pos`, `s:to(pos)` / `s:at(sy)` | the scroll (content rows above the box), and setting it / the content row at screen row `sy` (`nil` outside the box): for taps |
| `s:place(x, y, w, h)` | the box moved or resized |

A screen like this only moves under the finger, so give it `fps(60, idle)`: 60 while touched and
a few frames a second otherwise, as the launcher does. One frame of such a screen:

```lua
function _draw()
  local whole = ui.changed("screen", page)           -- another screen: all of it drawn again
  local changed = {}
  for i, it in ipairs(items) do                       -- every item asked, every frame
    if ui.changed(it.id, it.value) then changed[#changed + 1] = i end
  end
  if whole then
    draw_header()
    list:repaint()
  else
    for _, i in ipairs(changed) do list:redraw((i - 1) * ROW, ROW) end
    list:draw()                                       -- the scroll
  end
end
```

**Images.**

- **The cart's own** travel with it: put them in `images/CART/` beside the cart (PNG, JPEG or SVG,
  at most 384x384; subfolders allowed). push.py packs them (`tools/pack_images.py`: the board's own
  format, so it never decodes PNG; photos are dithered, flat art stays exact) and sends them
  ahead of the cart; the board keeps them in flash, so they work offline. An image's name is its
  path under that folder without the extension: `images/pet/hat/crown.png` is `"hat/crown"`.
  At most 512 KB of packed images a cart; the board keeps about 2.3 MB of all carts' images. When
  it needs room, images of carts it no longer has go first, then those of the cart opened least
  recently (push again to restore them).
- **Live images** come from a host connected while the cart runs: an image the host names, sent
  the first time a cart draws it. The firmware accepts them (`cart.image` messages), but nothing
  in this repo sends them yet. The first `hd.image` of a live name asks the host and returns
  `false`; it draws once the image has arrived, so call it each frame until it returns `true`. A
  name the host doesn't have keeps returning `false`. The board keeps about 1 MB of them, dropping
  the least recently used first.
- Images are unpacked once and kept (1 MB a cart). check.py previews them with the cart's own and,
  for live names, the samples in `tools/samples/live/` (`file/hello`).

Coordinates are canvas pixels and can be fractions (edges are drawn to the fraction). `a` is
0-255 (255 opaque). Drawing over a clear part of the canvas leaves it see-through where the shape
is.

What it costs on the board, to keep in mind for what's drawn each frame (the frame is 33 ms at 30
fps): filling the whole canvas once, about 9 ms (half-transparent: 20); a line of text, about 1 ms;
a circle 40 px across, 0.4 ms; a 128 px photo, 1.6 ms (one scaled to the whole canvas, 7); an image
with transparency, about 0.2 µs a pixel; and after the frame about 16 µs for each canvas row drawn
on (the runtime looks at it; one that ends up as it was isn't sent). Nothing is copied: a changed
row goes to the panel straight from where the cart drew it. `check.py` estimates every frame's cost from these
(`"hd": {"device_ms": ...}` in its output) and warns a cart that would run past 25 ms. It checks
the calls as the board does and draws the preview with the board's own runtime (`cartrun`).

## Input

| call | gives |
|---|---|
| `btn(b)` | true while button `b` is held |
| `btnp(b)` | true on the frame `b` went down |
| `touch()` / `touch(true)` | `x, y` in screen pixels while the cart window is touched, else `nil` / the same in canvas units (3x finer: for the full-color layer) |
| `tilt()` | `ax, ay`, each about -1..1: which way things would roll. `ax` > 0 when the right edge is lower, `ay` > 0 when the bottom edge is lower (screen directions: roll a ball by `ax, ay` and it goes downhill). Measured from how the board was held when the cart started (lying on a desk at a lean counts as level); `level()` takes the pose it's in now as level |

Buttons: `0` left, `1` right, `2` up, `3` down, `4` A, `5` B. There are no buttons on screen: a
drag is the d-pad (its direction from where the finger came down, diagonals too: two of 0-3
together), a tap is A (for the frame the finger lifts), and the BOOT button is B. The glass reads
one finger. `btn()` with no argument gives all six as bits.

## Feedback

| call | does |
|---|---|
| `buzz(ms)` | the vibration motor, 10-500 ms (this board has none: `caps().buzz` is false) |
| `sfx(n)` | a built-in sound: 0 blip, 1 coin, 2 hit, 3 jump, 4 explosion, 5 select, 6 error, 7 success; 8-63 your own (below); `sfx(-1)` stops yours |
| `music(n)` / `music(-1)` | your song from pattern `n` on / stop it (below) |
| `wake(secs)` / `wake()` | light the screen in `secs` seconds even if it went dark (a timer that's up) / cancel. The cart's frames resume then |

`sfx` plays short 8-bit-style effects on the board's speaker, built from square, triangle and
noise voices, up to four at once. On a board without a speaker `caps().sound` is false and `sfx`
does nothing. To hear them on a computer, the desktop build's `sfx_wav DIR` writes all eight as WAV
files; `engine/sfx.cpp` is the synth, the same code the board runs.

## Sound and music

A cart can make its own sounds and a song (API 3: put `-- api: 3` in the header). The playground's
Sounds and Music tabs are the easy way: draw notes, hear them, and the editor writes the tables
below into the cart's code. They're plain text, so they can be edited by hand too.

A **sound** is up to 32 notes played one after another at a speed. Each note has a pitch, an
instrument, a volume and an effect. `SOUNDS[n]` (8 to 63) is one sound as hex digits, spaces
anywhere:

```lua
SOUNDS = {
  [8] = "04 0000 2f160 34165",  -- speed 4; no loop; two notes
}
```

- The first 6 digits: the speed (ticks of 1/120 s per note, `01`-`ff`), then the loop's first
  note and the note it loops at (`00 00`: no loop). A sound that loops plays to its loop end, then
  goes back to the loop start until `sfx(-1)` stops it.
- Then 5 digits per note: the pitch (`00` is C2, `3f` is D#7, a semitone each), the instrument,
  the volume (`0`-`7`; 0 is a rest) and the effect.
- Instruments: 0 square, 1 pulse, 2 thin pulse, 3 triangle, 4 saw, 5 sine, 6 organ, 7 noise
  (higher notes hiss, lower ones rumble).
- Effects: 0 none, 1 slide (from the note before), 2 vibrato, 3 drop (to nothing), 4 fade in,
  5 fade out, 6 and 7 a fast and a slow arpeggio (through the four notes of its group: 1-4, 5-8, ...).

A **song** is a list of patterns in `MUSIC[0]` to `MUSIC[63]`. A pattern plays up to four sounds at
once, one per channel, for as long as the longest of them takes, then the next pattern plays:

```lua
MUSIC = {
  [0] = "01 08 09 -- --",  -- loop start; sounds 8 and 9 (hex), two channels free
  [1] = "02 08 0a -- --",  -- then back to the loop start
}
```

The first two digits are flags, as in PICO-8: 1 a loop starts here, 2 go back to the last loop
start after this pattern, 4 stop after it (add them up). Then each channel's sound, in hex
(`08`-`3f`), or `--`.

There are four channels. A song uses the ones its pattern needs; `sfx(n)` plays on a free one if
there is one, else on one the song is using (the song keeps time there unheard until the sound
ends). The built-in sounds 0-7 play on top of all that. A mistake in a sound or pattern is an error
at the `sfx` or `music` that plays it, saying what's wrong (`sfx(8): SOUNDS[8] note 3: ...`);
`check.py` finds it the same way. A cart that's closed stops its sounds and song.

## Time, numbers, saving

| call | gives |
|---|---|
| `time()` | seconds since the cart started (it keeps counting while the screen is dark) |
| `fps(rate)` / `fps(active, idle)` / `fps()` | the frame rate from the next frame (any rate that divides 60: 1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30, 60) / **lazy**: `active` while there's input (a touch on the cart, a button) and half a second after, `idle` otherwise / the rate now. An idle cart wakes at once when a touch comes down or BOOT is pressed, so a slow idle rate costs no response time: give `idle` the slowest that still shows what moves on its own (a blink twice a second needs 4; a seconds counter 1 or 2). A cart doing a few frames a second instead of 30 or 60 spends that much less of the board's battery on its frames. Move things by `time()`, not by frames, when the rate changes |
| `now()` | the wall clock: seconds since 1970 (UTC), a whole number, or `nil` while the board doesn't know the date. It knows it from its RTC (kept through reboots and power-off while the battery lasts), which push.py sets; use it for anything that lives while the cart is closed (`save(now())`, then `now() - saved` on the next run) |
| `date([t])` | local time (the host's timezone, kept for offline use) as a table: `year`, `month`, `day`, `hour`, `min`, `sec`, `wday` (1 = Sunday), `yday`; for `now()` when `t` is left out. `nil` while the date is unknown |
| `rnd([x])` | a random number in [0, x) (x defaults to 1); `rnd(t)` with a table picks an element |
| `srand(seed)` | seed `rnd` |
| `flr(x)`, `ceil(x)`, `abs(x)`, `sgn(x)`, `min(a, b)`, `max(a, b)`, `mid(a, b, c)` | the usual |
| `save(t)` / `load()` | keep a small table (numbers, strings, booleans, nested tables; at most 2 KB) across runs / get it back (`nil` the first time). Whole numbers come back exactly (timestamps included) |
| `stat(n)` | `stat(0)` memory in KB, `stat(1)` the last frame's Lua time as a fraction of a frame, `stat(7)` frames a second |

`sin(t)` / `cos(t)` take turns (1 = a full circle), as PICO-8's do, and `sin` is upside down (the
screen's y grows downwards): `sin(0.25)` is -1. They're a table lookup, faster than `math.sin` /
`math.cos` (radians), which also work.

## The host

The console that runs the cart gives it data and takes commands. In Luanette's firmware:

| call | does |
|---|---|
| `caps()` | what the cart runs on, as a table: the canvas size, where the picture sits, the board's name, what it has (touch, tilt, sound...). The fields are under "Versions and what a cart runs on" |
| `data(name)` | the latest value of a channel as a table, or `nil`: `"battery"` (mv, percent, charging, usb), `"carts"` (the carts on the board: a list of name, title, about), `"settings"` (volume and brightness, each a step from 0 to 3) |
| `act(name, [arg])` | a command to the console: `"open"` opens the cart named `arg`; `"volume"` and `"brightness"` set that setting to the step `arg` ("0" to "3"). The launcher uses all three |

`data()` is free to call every frame; a channel's table is the same one until that channel
changes. A firmware that hosts the engine elsewhere can feed its own channels and take its own
commands (`carts::player::Device` in `engine/esp32/cart_player.hpp`).
