# The command-line tool

`luanette` (npm) runs the console's own engine, the same WebAssembly as the
[playground](https://geofresh.github.io/luanette/), in Node 18 or newer. There's nothing to build:

```bash
npx luanette check game.lua
```

In the repository, after `web/build.sh`, `node cli/luanette.mjs` is the same tool.

## Commands

### `check CART [--frames N] [--input FILE]`

Checks a cart the way `tools/check.py` does, and prints JSON:

- **The header:** size (32 KB without comments), the three header lines, `-- api:` and `-- canvas:`.
- **Running it:** that it loads, then runs its top level, `_init` and N frames (150), with
  `check.py`'s scripted input (buttons, taps, a drag and a slow tilt on a 120-frame cycle) or yours
  (`--input`), inside the 30,000-instruction frame budget.

```json
{
 "ok": true, "cart": "game.lua", "bytes": 2416, "source_bytes": 2903,
 "title": "Game", "api": 4, "canvas": "full",
 "frames": 150, "fps": 30, "max_ops": 1200, "avg_ops": 640,
 "warnings": ["..."], "log": ["..."]
}
```

When it fails, `error` says why in `check.py`'s words, with the cart's line: `doesn't load:
cart:12: ...`, `error in _init: ...`, or `error in frame 37: cart:40: ...` (then `frame` too).

**Differences from `check.py`:**
- `max_ops` counts the cart's own Lua instructions in hundreds, as the runtime does.
- Its warnings are these of `check.py`'s:
  - the window instead of the whole screen
  - a still screen at a high frame rate
  - a header missing `-- api:` for what the cart uses
  - a last frame of one color
- It doesn't estimate the full-color layer's cost on the board, or check `buzz()`.
- It doesn't pack a cart's images (`images/NAME/`): `hd.image()` draws nothing here. Use
  `check.py` and `push.py` for carts with images.

It exits 0 when `ok`, 1 when not.

### `run CART [--frames N] [--input FILE] [--shots 1,30,90] [--out DIR] [--gif]`

Runs the cart for N frames (90) with your input, and saves what it showed:

- `DIR/frame-N.png` for each shot: the whole screen, 410 x 502.
- `DIR/shots.png`: the shots side by side at half size.
- With `--gif`: `DIR/run.gif`, every other frame at half size.

It prints the paths, the `log` lines with the frame each came in, `max_ops`, and each frame's cost.

### `sound NOTES [--speed N] [--loop A-B]`

Turns readable notes into the text `SOUNDS` keeps:

```bash
npx luanette sound "C4 pulse 5, E4 slide, -, G4 saw 7 fadeout" --speed 6
# 06 0000 18150 1c151 00100 1f475
```

- **A note:** a pitch (C2 to D#7, with `#` or `b`), then any of:
  - an instrument: `square`, `pulse`, `thin`, `triangle`, `saw`, `sine`, `organ`, `noise`
  - a volume, 0-7
  - an effect: `slide`, `vibrato`, `drop`, `fadein`, `fadeout`, `arp`, `arpslow`
- **Carrying on:** an instrument or volume left out is the note before's; to start, `square` and 5.
- **`-`** is a rest.
- **`--speed`** is ticks of 1/120 s per note (8).
- **`--loop 2-4`** loops notes 2 and 3 for good.

### `sound CART N --wav FILE`, `song CART [--from N] --wav FILE`

Render a cart's sound N, or its song from pattern N (its first), to a 44.1 kHz WAV:
- A sound plays once through. A looping one goes round its loop once more.
- A song plays to its end, or round its loop once more, up to three minutes.

### `link CART [--name NAME] [--play]`

A playground share link with the cart in it (its code, sounds and song). Opening it adds the cart
to that browser's carts. With `--play` it opens in play mode: the screen alone, full screen, with
a d-pad and A and B on a phone (`&play` at the end of any playground link does the same).

### `new NAME [--template blank|platformer|shooter|puzzle] [--out FILE]`

A new cart from a template: full-screen, with sprites, sounds and `log()` already in it.

### `mcp`

The tool as an MCP server over stdio, for coding agents. For Claude Code:

```bash
claude mcp add luanette -- npx -y luanette mcp
```

Other clients take the same command:

```json
{ "mcpServers": { "luanette": { "command": "npx", "args": ["-y", "luanette", "mcp"] } } }
```

Its tools:

| tool | does |
|---|---|
| `check_cart` | `check`: give the cart as `source` (text) or `path`; `frames`, `input` |
| `run_cart` | `run`, returning the shots as images (one strip, or each at full size with `full`), the log by frame and each frame's cost |
| `render_sound` | a sound (`sound`: 8-63) or the song described note by note in `make_sound`'s words, its length and loudness; `wav`: a file to write |
| `make_sound` | `sound`: `notes`, `speed`, `loop` |
| `share_link` | `link`; `play` for play mode |
| `api_help` | the API from `docs/api.md`: one call by `name`, the calls matching `search`, or the list |
| `new_cart` | `new`, returning the source |

## The input script

A JSON list of steps, each for one `"frame": N` or a range `"frames": "A-B"` (frames count from 1):

```json
[
  {"frames": "1-40", "hold": ["right"]},
  {"frame": 45, "tap": [64, 64]},
  {"frames": "60-90", "drag": [[100, 100], [40, 100]]},
  {"frames": "1-200", "tilt": [0.4, 0]},
  {"frames": "100-110", "touch": [20, 30]}
]
```

| step | is |
|---|---|
| `hold` | buttons down on those frames: `left`, `right`, `up`, `down`, `a`, `b` |
| `tap` | a finger down at x, y for two frames from `frame`, then A on the frame it lifts, as on the board |
| `drag` | a finger moving from one point to the other across the frames; past 6 pixels it works the d-pad from its direction, as the board does |
| `touch` | a finger held at x, y (no A) |
| `tilt` | `ax, ay`, each -1 to 1 |

Positions are the picture's pixels, 0 to 127. Steps that overlap add up.

## For agents

[`AGENTS.md`](../AGENTS.md) at the top of the repository is the page to read first: the cart
format, the loop (check, run, look, fix, link), the limits and the pitfalls.
