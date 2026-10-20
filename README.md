# Luanette

[![ci](https://github.com/Geofresh/luanette/actions/workflows/ci.yml/badge.svg)](https://github.com/Geofresh/luanette/actions/workflows/ci.yml)

A Lua game console for ESP32-S3 boards. A game, or cart, is one Lua file. It draws into a
128x128 picture of 16 colours, and can add a lit 3D scene under it and a full-colour layer over
it, at up to 60 fps. You write a cart on your computer, check it there, and send it to the board over USB.

The name is Lua plus *lunette*, French for a little moon or a small window.

**[Try it in your browser](https://geofresh.github.io/luanette/).** The five example carts run
there on the same engine, compiled to WebAssembly, next to an editor.

<img src="docs/images/board.jpg" width="320" alt="Luanette's launcher on the Waveshare ESP32-S3-Touch-AMOLED-2.06">

![The launcher, Lock & Load, Bubbles, Night Swarm, Dice and Mochi](docs/images/screens.png)

It's in the spirit of PICO-8 but not compatible with it; the API is its own (`docs/api.md`).
One board is supported so far, the Waveshare ESP32-S3-Touch-AMOLED-2.06 (410x502 AMOLED,
touch, IMU, speaker). Only `board/` knows about it.

## Why it's fast

- **Only changed pixels are sent.** The panel keeps its own copy of the image, so each frame
  sends just the rows and columns that changed. A still screen sends nothing, and a ticking
  clock costs a few thousand pixels instead of the whole 410x502.
- **Frames are timed to the panel.** The panel redraws itself top to bottom 59.7 times a second
  and signals each pass. Every strip of a frame is sent right behind that pass, or just ahead of
  it when there's time. A frame never shows half drawn.
- **Both cores work.** Lua runs on one core while the other composes and sends the last frame.
  A 3D scene is never stored as a full image: each strip is drawn just before it goes out, split
  across both cores.
- **Hot loops use the S3's vector instructions.** Each one is checked bit for bit against plain
  C on the board itself.

Measured on the AMOLED-2.06:

| What | Result |
|---|---|
| 3D, two lit rings of 3,200 triangles | 59 fps at 384x384 |
| The same scene in Jet, the 3D library the stage builds on | 26 fps at 192x192 |
| The launcher, idle | 1.3 ms of Lua a frame, 2 frames a second |
| Sending a cart over USB | under a second |

`docs/ARCHITECTURE.md` explains how each of these works.

## What's in the repo

- `engine/`: the Lua runtime, the renderer, the 3D stage and the sound synth
- `board/`: drivers for the AMOLED-2.06
- `firmware/`: the firmware, with a launcher and the examples built in
- `tools/`: check a cart, preview it, send it, take a screenshot
- `web/`: the browser playground
- `examples/`: five complete carts

## What you need

- A Waveshare ESP32-S3-Touch-AMOLED-2.06 and a USB-C cable.
- `esptool` to flash the prebuilt firmware, or [PlatformIO](https://platformio.org/) to build
  it yourself. PlatformIO fetches ESP-IDF on its own.
- For the tools: Python 3.10 or newer with its development headers (`python3-dev` or
  `python3-devel`, or a Python from uv or pyenv), a C++17 compiler and CMake.

I've only used it on Linux. macOS should work. On Windows, `cartrun`, which previews 3D and
full-colour carts, doesn't build yet, and `push.py` needs `--port COM3` or similar.

## Quick start

Flash the four files from the [latest release](https://github.com/Geofresh/luanette/releases/latest):

```bash
pip install esptool
esptool --chip esp32s3 write-flash 0x0 bootloader.bin 0x8000 partitions.bin 0x19000 ota_data_initial.bin 0x20000 luanette.bin
```

This leaves the rest of the flash alone, so saves and sent carts survive an update. To build it
yourself instead:

```bash
cd firmware
pio run -t upload
```

The board boots into the launcher with the five examples. Then set up the tools:

```bash
python -m venv .venv && . .venv/bin/activate
pip install -r tools/requirements.txt
python tools/build_lupa32.py        # Lua with the board's 32-bit numbers, for check.py
cmake -S host -B host/build && cmake --build host/build
```

Change something in an example, a colour or a speed, and send it. Your copy replaces the
built-in one:

```bash
python tools/push.py examples/bubbles.lua
```

`push.py` runs the cart through `check.py` first, so a broken cart never reaches the board. It
also sets the board's clock and timezone from your computer. Sent carts stay in flash.

To take a screenshot:

```bash
python tools/shot.py bubbles.png
```

## Using it

- Tap is A, drag is the d-pad, and the BOOT key is B.
- Swipe down from the top edge, or press PWR, to leave a cart.
- The launcher shows the battery next to the cart count. Tap the clock (or the Settings row at the
  end of the list) for volume, brightness and the battery's details.
- PWR in the launcher turns the screen off. Any key or a touch turns it back on.
- The screen dims after a minute and turns off after ten. While it's off, the chip sleeps.
- Below 3.4 V the board shuts itself down. Plug it in to start it again.

There's no Wi-Fi to set up: carts arrive over the USB cable, and the board needs nothing else.

## Writing a cart

```lua
-- title: Hello
-- by: you
-- about: a dot you move with your finger

x, y = 64, 64

function _update()
  if btn(0) then x = x - 1 end
  if btn(1) then x = x + 1 end
  if btn(2) then y = y - 1 end
  if btn(3) then y = y + 1 end
end

function _draw()
  cls(1)
  circfill(x, y, 4, 8)
  print("hello", 2, 2, 7)
end
```

```bash
python tools/check.py hello.lua --preview hello.png   # runs it here and enforces the limits
python tools/push.py hello.lua
```

- [`docs/api.md`](docs/api.md) is the full API: drawing, input, the colour layer, images, 3D,
  sound and saves.
- [`docs/DESIGN.md`](docs/DESIGN.md) covers making a cart run well. Read it before writing
  anything big.
- [`examples/`](examples/) has five complete carts, from a bubble shooter to a 3D rail shooter.

A cart gets 32 KB of source (comments don't count), 1 MB of memory and 2 KB of saved data.
`check.py` allows 30,000 Lua instructions a frame; the board stops a frame that runs past 100 ms.

## How it works

[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) covers the engine: how a frame gets from Lua to
the screen, the runtime's sandbox, storage, the USB protocol, how to check that a change didn't
break or slow anything, and what porting to another board takes.
[`docs/3d-stage.md`](docs/3d-stage.md) has the measurements behind the 3D stage.

## Status

It works, but so far it's had one user (me) and one board. Expect rough edges.

- One board. The engine doesn't depend on it, but the screen layout is set for this panel.
  `docs/ARCHITECTURE.md` lists what a port needs.
- Carts arrive over USB only.
- There's no sprite, map or music editor, and `sfx()` plays eight built-in sounds.

## Credits

Written in collaboration with [Claude Code](https://claude.com/claude-code).

## License

MIT (`LICENSE`). Lua and Jet are MIT, the fonts are SIL OFL 1.1, and the panel's start-up table
follows Waveshare's Apache 2.0 board package. Details are in `NOTICE` and `LICENSES/`.
