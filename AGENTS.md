# Making Luanette carts (read this first if you're an agent)

Luanette is a small game console: Lua games, called carts, on an ESP32-S3 board with a
touchscreen, and in a browser [playground](https://geofresh.github.io/luanette/). This page is
what you need to write a cart that works the first time. The full API is in
[`docs/api.md`](docs/api.md); how to make one run well is in [`docs/DESIGN.md`](docs/DESIGN.md).

## The tools

Everything runs the console's own engine through Node 18 or newer; nothing to build:

```bash
npx luanette new mygame --template platformer   # blank, platformer, shooter or puzzle
npx luanette check mygame.lua                   # JSON: ok, error (with the line), warnings, log
npx luanette run mygame.lua --input play.json --shots 1,30,90 --out shots   # PNGs to look at
npx luanette sound "C4 pulse 5, E4 slide, G4 saw 7 fadeout"                # SOUNDS text
npx luanette link mygame.lua                    # a link that opens it in the playground
npx luanette add actor cam mygame.lua           # put libraries in it (npx luanette libs lists them)
```

Or as MCP tools (`check_cart`, `run_cart` with images back, `render_sound`, `make_sound`,
`share_link`, `api_help`, `new_cart`, `add_library`):

```bash
claude mcp add luanette -- npx -y luanette mcp
```

[`docs/cli.md`](docs/cli.md) has every option.

## The loop

1. Start from a template (`new`): it already uses the whole screen, sprites, sounds and `log()`.
2. Change it. Keep the header lines.
3. `check` after every change. Fix any `error` (it names the line and the frame) before anything else.
4. `run` it with input that plays it (below) and look at the shots: does it show what you meant?
   Read the `log`: put `log()` calls in your cart to see values. In `check` and `run`, `rnd()`
   gives the same numbers every run, so you can `log()` where things appear and aim the input at
   them.
5. When `check` says `"ok": true` and there are no warnings you can fix, `link` it, and give the
   person the link: they can play it, change it and send it to a board. `link --play` gives a link
   that opens straight into play mode (the screen alone, with buttons on a phone).

## A cart

One Lua 5.4 file:

```lua
-- title: Hello
-- by: you
-- about: a dot you move with your finger
-- api: 6
-- canvas: full

x, y = 64, 64

function _init()          -- once, at the start
  fps(60, 10)             -- 60 frames a second while touched, 10 otherwise
end

function _update()        -- every frame, before _draw: change things here
  if btn(0) then x = x - 1 end   -- 0 left, 1 right, 2 up, 3 down, 4 A, 5 B
  if btn(1) then x = x + 1 end
  local tx, ty = touch()         -- a finger on the picture, in its pixels, or nil
  if tx then x, y = tx, ty end
end

function _draw()          -- every frame: draw it all again
  cls(1)
  circfill(x, y, 4, 8)
  print("hello", 2, 2, 7)
end
```

- `-- title:`, `-- by:` and `-- about:` are required, in the first 12 lines. `-- api: 6` says which
  API the cart needs (6 is the newest: use it). `-- canvas: full` gives the cart the whole screen.
- The picture is 128 x 128 pixels, (0, 0) at the top left, 16 colors by number: 0 black, 1 dark
  blue, 2 dark purple, 3 dark green, 4 brown, 5 dark gray, 6 light gray, 7 white, 8 red,
  9 orange, 10 yellow, 11 green, 12 blue, 13 lavender, 14 pink, 15 peach.
- On the board, a tap is A (`btn(4)`) for the frame the finger lifts, a drag is the d-pad
  (`btn(0)` to `btn(3)`), and the BOOT key is B.
- Most used: `cls`, `pset`, `line`, `rect`, `rectfill`, `circ`, `circfill`, `print(text, x, y,
  color)`, `spr(n, x, y, [flip_x], [flip_y])`, `btn`, `btnp`, `touch`, `tilt`, `sfx(n)`,
  `music(n)`, `rnd`, `flr`, `mid`, `abs`, `sin`, `cos`, `time()`, `save(t)` / `load()`, `log(...)`.
  `api_help` (or `docs/api.md`) has the rest: the full-color layer `hd.*`, a lit 3D scene, images.

## The whole screen

The board's screen is 410 x 502; the 128 x 128 picture sits in a 384 x 384 window in the middle,
3x. With `-- canvas: full`, the full-color layer draws the bands above and below it: put the
title there, and the score or what to do. Draw them once, in `_init`; they stay:

```lua
local c = caps()
local W, H = c.w, c.h                              -- 410, 502
local TOP, BOT = c.picture_y, c.picture_y + 384    -- the bands: 0 to TOP, and BOT to H

function _init()
  hd.scanlines(false)
  hd.gradient(0, 0, W, TOP, 0x1b2440, 0x000000)
  hd.text("MY GAME", W / 2, TOP / 2 - 16, 0xffffff, 24, "bold", "center")
  hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x1b2440)
  hd.text("tap to jump", W / 2, BOT + 22, 0x9aa4b8, 14, "mono", "center")
end
```

Keep text in the bands short (about 30 characters at size 14): the corners are rounded.

## Sprites

8 x 8, in a `SPRITES` table, each a string of 64 hex digits (a color each, row by row), written a
row at a time with spaces between. Color 0 is see-through. `spr(1, x, y)` draws this one:

```lua
SPRITES = {
  [1] = "00777700 07777770 77177177 77777777 77777777 77177177 07711770 00777700", -- face
}
```

Sprites are 0 to 255; a map's cells can only be 0 to 63, so keep tiles there and characters and
items above. The playground's Sprites tab reads and writes this exact layout. More:

- `spr(n, x, y, w, h, [flip_x], [flip_y])` draws a block of `w` x `h` sprites, `n`, `n + 1`, ...
  row by row: a 16 x 16 character is four sprites in a row of the table, `spr(32, x, y, 2, 2)`.
- `sspr(n, x, y, dw, dh)` draws a sprite stretched to `dw` x `dh` (a big title, a portrait, a boss).
- `palt(c, true)` makes color `c` see-through as well; `palt(0, false)` draws black; `palt()` resets.
- Flags: a `FLAGS` table gives a sprite up to eight on/off flags, `FLAGS = { [3] = 1, [7] = 1 | 2 }`.
  `fget(n, f)` reads one. What they mean is yours to say; flag 0 for solid is the usual (the `actor`
  library and the platformer template use it).

## Maps

For levels bigger than the screen, or any grid of tiles: a `MAP` table of rows, top to bottom, a
string each with a character a cell. `.` is empty; `0`-`9`, `a`-`z`, `A`-`Z`, `+`, `/` are sprites 0
to 63 (`3` is sprite 3, `a` is 10). Up to 128 x 64 cells; a cell is 8 x 8 pixels.

```lua
MAP = {
  "................",
  "....2.....2.....",
  "...333...333....",
  "3333333333333333",
}

function _draw()
  cls(12)
  camera(cam_x, 0)   -- scroll: the camera moves the map and the sprites with it
  map()              -- the whole map in one call, only what's on screen costs anything
  spr(1, px, py)
  camera()           -- the score and such, fixed on screen
end

FLAGS = { [3] = 1 }          -- sprite 3 has flag 0: solid

local function solid(x, y)  -- is the pixel at x, y in a wall?
  local t = mget(flr(x / 8), flr(y / 8))
  return t ~= nil and fget(t, 0)
end
```

`map(0, 0, 0, 0, 128, 64, 2)` draws only the cells whose sprite has flag 1 (`layer` is a bitmask of
flags): draw the ground, then the player, then the treetops in front.

`mget(cx, cy)` is the sprite in a cell (nil if empty or off the map), `mset(cx, cy, n)` changes it
(`mset(cx, cy)` empties it: picking up a coin), `msize()` is the width and height in cells.
`MAP = LEVEL2` switches maps; `MAP = {table.unpack(LEVEL)}` starts a level over after `mset`. A map
is a byte a cell of the 32 KB. Never draw tiles with `spr` in a loop: `map()` is one call. The
platformer template uses all of it. For a world bigger than 128 x 64 cells, keep it as a grid of blocks
and make `MAP` only the blocks around the player, rebuilt as they walk: `examples/overworld.lua`
does a 4096 x 4096 px world that way. Maps need `-- api: 5`; flags, `palt`, `sspr`, blocks and
sprites past 63 need `-- api: 6`.

## Libraries

Don't write movement, cameras, timers or text boxes from scratch: add a library. It goes into the
cart as code between `-- lib: NAME` and `-- end lib: NAME` lines (the cart stays one file), and its
comments say how to call it. Leave the code between the markers alone, so `add` can update it.

- `actor`: an actor is a table with `x, y, w, h`; `actor.move(me, dx, dy)` moves it through the map
  and stops it at solid cells (sprites with flag 0), sets `me.ground`; `actor.overlap(a, b)`.
- `cam`: `cam.follow(me.x, me.y)` then `cam.apply()` in `_draw` (and `camera()` before the score);
  keeps inside the map; `cam.shake(0.3, 2)`.
- `anim`: `local walk = anim.new({16, 17}, 8)`, `spr(walk:frame(), x, y)`, `me.a = anim.switch(me.a, walk)`.
- `tween`: `tween.to(obj, 0.5, {x = 100}, ease.out_back, done)`, `tween.update()` each frame.
- `timer`: `timer.after(2, fn)`, `timer.every(0.5, fn)`, `timer.update()` each frame.
- `dialog`: `dialog.say({"page one", "page two"})`; `if dialog.update() then return end` at the top
  of `_update`; `dialog.draw()` last in `_draw`.
- `particles`: `particles.burst(x, y, 20, {colors = {8, 9, 10}, gravity = 120})`, `update()` and `draw()`.
- `state`: scenes as tables of `enter`, `update`, `draw`; `state.go(game)`.
- `vec`: `vec.angle(dx, dy)`, `vec.from(a, len)`, `vec.norm`, `vec.dist`, `vec.rot`.

Each has a demo in `libs/demos/`.

## Sounds and music

`sfx(0)` to `sfx(7)` are built in: 0 blip, 1 coin, 2 hit, 3 jump, 4 explosion, 5 select,
6 error, 7 success. A cart's own sounds are `SOUNDS[8]` to `SOUNDS[63]`, made from readable
notes with `luanette sound` (or `make_sound`), and played with `sfx(8)`:

```bash
npx luanette sound "B5 pulse 6, E6 fadeout" --speed 3     # prints: 03 0000 2f160 34165
```

```lua
SOUNDS = {
  [8] = "03 0000 2f160 34165", -- coin
}
```

A note is a pitch (C2 to D#7), then any of an instrument (square, pulse, thin, triangle, saw,
sine, organ, noise), a volume 0-7, and an effect (slide, vibrato, drop, fadein, fadeout, arp,
arpslow). `-` is a rest. `--speed` is ticks of 1/120 s per note (8 is a quick note; 16 a 16th at
about 112 BPM). A song is a `MUSIC` table of patterns, each `"FF SS SS SS SS"`: flags (1 loop
start, 2 loop back, 4 stop), then a sound number in hex per channel or `--`:

```lua
MUSIC = {
  [0] = "01 0a -- -- --",   -- loop start: sound 10 alone
  [1] = "02 0a 0b -- --",   -- sounds 10 and 11 together, then back to the loop start
}
```

`music(0)` starts it, `music(-1)` stops it. Carts with `SOUNDS` or `MUSIC` need `-- api: 3` or
more. `render_sound` (MCP) describes a sound note by note; `luanette song cart.lua --wav out.wav`
writes it to a file.

## Playing it: the input script

`run --input FILE` (and `run_cart`'s `input`) takes a JSON list of steps, each for `"frame": N` or
`"frames": "A-B"`:

```json
[
  {"frames": "1-40", "hold": ["right"]},
  {"frame": 45, "tap": [64, 64]},
  {"frames": "60-90", "drag": [[100, 100], [40, 100]]},
  {"frames": "1-200", "tilt": [0.4, 0]}
]
```

`hold` takes `left`, `right`, `up`, `down`, `a`, `b`. Positions are picture pixels (0-127). A
`tap` is a finger down for two frames, then A on the frame it lifts, as on the board; a `drag`
moves a finger and works the d-pad from its direction.

## Limits

- 32 KB of source, not counting comments (comment freely).
- 30,000 Lua instructions a frame (`_update` and `_draw` together). Drawing calls are cheap; loops
  over thousands of things aren't. `check` reports `max_ops`.
- 1 MB of Lua memory. 2 KB of saved data (`save(t)`).
- Numbers are 32-bit: integers to about ±2 billion, floats to about 7 digits.

## Pitfalls

It looks like PICO-8, but it isn't:

- No `+=`, `-=` and the like: write `x = x + 1`.
- No `all()`, `foreach`, `add`, `del`, `count`, `frame()` or `t()`: use `ipairs`, `table.insert`,
  `table.remove`, `#t`, and `time()`.
- No `printh`: `log(...)` writes a line `check` and `run` report (and the playground's Console).
- `print` draws text on the picture; it doesn't log.
- No `load`, `require`, `dofile`, `io` or `os`: a cart is one file, sandboxed.
- `sin` and `cos` take turns (1 is a full circle), and `sin` is upside down (screen y grows
  downwards), as PICO-8's: `sin(0.25)` is -1, `cos(0)` is 1. `math.sin` takes radians as usual.
- A cart that draws only in the window (no `-- canvas: full`) leaves 30% of the screen dark:
  `check` warns about it.
- A screen that sits still between touches (a menu, a puzzle, a game over) shouldn't redraw 30
  times a second: `fps(60, 10)` gives it 60 while touched and 10 otherwise. A game that moves on
  its own (a chaser, falling things) keeps a steady rate, `fps(30)`, and can drop to 10 on a
  still screen. Either way, move things by `time()`, not by frames. `check` warns about a still
  screen at a high rate.
- The picture's leftmost and rightmost columns carry on out to the screen's edges (so a
  background fills the width). Keep sprites off columns 0 and 127, or they streak sideways.
- The picture has scanlines (every third row of the screen a little darker): that's the
  console's look. `hd.scanlines(false)` turns them off for the full-color layer only.

## Putting it forward

Carts made with an agent are welcome in the [gallery](gallery/) and in jams. Say so in the
header (it's optional, and a comment like the others):

```lua
-- made with: Claude Code
```

The gallery marks those carts "agent-made". The person submits it from the playground with
**Share, Submit to the gallery**, after playing it.
