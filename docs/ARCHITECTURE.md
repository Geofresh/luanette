# How Luanette works

This is for people changing the engine or moving it to another board. To write carts, read
`api.md` (the API) and `DESIGN.md` (how to make one run well) instead.

A cart is one Lua 5.4 file with `_init`, `_update` and `_draw`. It draws into a 128x128
picture of 16 colors, and optionally into a full-color layer and a lit 3D scene. The firmware
runs it at up to 60 frames a second on an ESP32-S3. Each frame, only the pixels that changed are
sent to the panel, timed so that a frame never shows half drawn.

One board is supported today: the Waveshare ESP32-S3-Touch-AMOLED-2.06 (410x502 AMOLED, CO5300
controller over QSPI, FT3168 touch, QMI8658 IMU, ES8311 speaker codec, 8 MB PSRAM).

## Where things are

```
engine/                   the engine; no board code
  cart.hpp, cart.cpp      the runtime: the Lua sandbox, its heap and frame deadline, the whole API
  hd.hpp, hd.cpp          the full-color layer (hd.*): RGB565 rows in PSRAM, change tracking
  hd_pie.S                the layer's two hottest loops in the S3's vector (PIE) instructions
  hd_fonts.*              the layer's fonts (JetBrains Mono, a bold face, VT323)
  stage.hpp, stage.cpp    the 3D stage: meshes, lighting, depth-buffered rasteriser
  stage_pie.S             the stage's span loop in PIE instructions
  jet/                    Jet (CubeCoders, MIT), vendored and patched; the stage builds on it
  lua/                    Lua 5.4.7, vendored, built with LUA_32BITS
  sfx.hpp, sfx.cpp        the eight built-in sounds, synthesized
  ui.lua                  helpers every cart gets (ui.changed, ui.scroller); compiled in
  builtin/                the launcher, itself a cart
  host/                   cartrun (the runtime on a PC), sfx_wav and stagerun
  esp32/                  the parts that need ESP-IDF:
    cart_player.*         runs the open cart and sends its frames (the Device interface lives here)
    scan_sync.*           the panel's refresh timing (TE)
    cart_store.*          carts and images in flash, and the messages that deliver them
    sound.*               plays sfx() on a speaker
board/                    the Waveshare AMOLED-2.06: pins, panel, touch, keys, speaker, PMIC, RTC
support/                  small shared pieces: JSON, CRC32 and base64, the audio/power interfaces
firmware/                 the console firmware (ESP-IDF project, built with PlatformIO)
  main/main.cpp           brings the board up and runs the main loop
  main/console.*          hosts the player on this board: Device, screen power, launcher, USB commands
  main/usb_link.*         the USB serial port, one command line in, one answer out
  main/builtin_carts.cpp  the carts the firmware carries (the examples)
host/                     CMake project for the desktop builds (cartrun, sfx_wav, stagerun, host_tests)
web/                      the browser playground: the engine as WebAssembly (web/build.sh)
tools/                    check.py, push.py, shot.py, pack_images.py, cartsrc.py, build_lupa32.py,
                          hd_fonts.py
tests/                    parity.py and its carts, the host tests
examples/                 five complete carts, also built into the firmware (examples/README.md)
docs/                     api.md, DESIGN.md, 3d-stage.md, this file
```

`engine/` outside `esp32/` builds on a desktop compiler, and with Emscripten for the browser.
That is what `cartrun`, the host tests and the playground use, and what any other port would
start from. `engine/esp32/` needs ESP-IDF but not this board: everything board-specific reaches it
through the `Device` interface and a few constants (see "Porting" below).

## A frame, start to finish

The panel refreshes itself at about 59.7 Hz, top row to bottom, and raises its TE line during
each vertical blank. Everything is scheduled around those passes.

1. **Lua runs** (core 1, the firmware's task). One panel pass before the frame is due, the
   player calls the cart's `_update` and `_draw`. They draw into the 128x128
   picture (palette indices, internal RAM) and, if the cart uses them, the full-color layer
   and the 3D scene. A frame that runs past 100 ms is stopped: a hardware timer interrupts the
   Lua VM, so a runaway loop can't hang the board.
2. **Handoff.** The picture is copied to the sender's buffer. The layer publishes: rows drawn
   this frame become the front rows, and each changed row records which columns changed.
3. **The sender** (core 0) compares the picture with what the panel shows, row by row. It
   groups the changed rows into bands of 10 picture rows (30 panel rows), finds the columns that
   changed, and composes only those: each picture pixel becomes 3x3 panel pixels (the third row
   slightly darker, a scanline), with the layer drawn over or under. Then it hands the band to
   the panel's DMA and starts composing the next band into the other buffer.
4. **Timing each band** (`scan_sync`). Before a band goes out, `te_gate` waits until the scan
   has passed that band's rows, so the band shows in full on the next pass. When there is time,
   the player instead races the beam: a band goes just ahead of the scan and shows on the pass
   already under way, a frame sooner. If a band can't make it ahead, the rest of the frame falls
   back to behind the scan, so no tear ever shows.

A cart that draws nothing new costs almost nothing: no rows differ, so nothing is sent. The
launcher idles at about 1.3 ms of Lua a frame, two frames a second.

### 3D frames

A cart that calls any 3D function gets a stage, and its frames work differently. The scene is
never drawn into a full buffer; each 32-row band is drawn just before it goes out:

- Core 1, after the cart's Lua, **prepares** the next frame: transforms, culls back faces per
  mesh, lights vertices, projects, and files each triangle under the 16-row pieces it touches.
- Core 0's sender **draws** each band: clears its depth rows, rasterises the triangles in it
  (16-row pieces against one depth buffer), lays the cart's picture and layer over it, and
  sends it. A helper task on core 1 draws the band's right part meanwhile. The split moves each
  frame so neither core overruns.
- `res("auto")` drops the scene to half resolution (192x192, doubled) when a frame's work
  passes 14.5 ms, and goes back when it would fit at full size (under 11 ms).

Measured on the board, two lit rings of 3,200 triangles: 59 fps at 384x384, about 21 ms of
work a frame spread over both cores. Jet's own pipeline does 26 fps on the same scene at
192x192.
`3d-stage.md` has the measurements behind each design choice.

## The runtime (`cart.cpp`)

- **Sandbox.** Only Lua's base, string, table, math and coroutine libraries are loaded (no `io`,
  `os` or `debug`), and `load`, `dofile`, `loadfile`, `require` and `collectgarbage` are removed.
  The API is the only way out. Sources are at most 32 KB, not counting comments: a longer
  source is measured and run with its comments stripped (`cartsrc.cpp`, the same rules as
  `tools/cartsrc.py`).
- **Memory.** The cart's heap comes from the host in blocks (PSRAM on the board), capped at
  1 MB. Saves (`save`/`load`) are at most 2 KB, in NVS.
- **Versions.** A cart can declare `-- api: N`. A runtime older than N refuses it with a message
  saying so. The current version is 2.
- **The host.** The runtime never touches hardware. Everything goes through `carts::Host`:
  time, memory, images, data channels, saves, the frame deadline, buzz and sound. The player
  provides one on the board, `cartrun` one on the desktop, and `web/luanette_web.cpp` one in the
  browser. This is what keeps the runtime portable.
- **The deadline.** On the board, a timer interrupts Lua when a frame runs over. A host with no
  way to interrupt (the browser has one thread and no signals) returns true from
  `deadline_by_polling`, and the runtime checks the clock itself every 20,000 instructions.

### Adding an API call

1. Write the function in `cart.cpp` and add it to the `kApi` table in `Cart::start`.
2. Add the same call to `check.py`, the desktop checker. It must draw exactly what the board
   draws: `tests/parity.py` runs carts through both and compares the pictures.
3. Document it in `api.md`. If an old runtime can't run carts that use it, raise
   `kApiVersion` and say in `api.md` what the new version adds.
4. If it needs hardware, add a virtual to `carts::Host` with a harmless default, so `cartrun`
   and other hosts keep building.

## The player (`esp32/cart_player.*`)

The player owns the screen while a cart is open. The firmware hosting it implements
`carts::player::Device`:

| part | what the player needs |
|---|---|
| glass | `width`, `height`, `corner_radius`, `round`, `clear_glass` |
| band buffers | two DMA-capable buffers of 32 rows of the glass's width; `band(k)`, `band_wait(k)` (until its last transfer is done), `band_send(k, x, y, w, h)` (asynchronous) |
| screen | `screen_on`, `wake_screen` (a cart's `wake()`) |
| input | `touch()`: the newest sample, its time and where the finger came down |
| sensors | `imu_enable` / `imu_read` for `tilt()`; `buzz` / `motor_rest`; all optional |
| host | `data(channel, &version)` for `data()`; `accepts_command(name)` and `command(name, arg)` for `act()` (each firmware has its own commands), and the "cart" open/close notice |
| its own screen | `will_open` before a cart takes the screen, `closed` after it gives it back |
| housekeeping | `timed_out`, and optional hooks for profiling and power |

`data()` must give each channel its own version. The runtime keeps each channel's Lua table
and makes it again only when that channel's version changes. One shared version would rebuild
every table on every change; on an earlier firmware with many channels, that showed up as 35 ms
frames.

The firmware's task calls `carts::player::tick()` in a loop and sleeps for the milliseconds it
returns. `open`, `close` and `tick` run on that task; nothing else in the player is
thread-safe.

The panel must accept a write to any window of the glass, with a completion callback. Windows
start and end on even columns and rows (the CO5300's rule). Narrow windows are what make small
changes cheap: a clock's seconds cost a few thousand pixels, not a frame.

## Storage and delivery (`esp32/cart_store.*`)

Sent carts survive a restart. They live in the `assets` partition (4 MB): 48 slots of
36 KB for sources, then 32 KB blocks for image bundles (about 2.3 MB). When images need room,
the bundles of carts that are gone go first, then those opened least recently. Writes put the
header last, so a slot cut off mid-write reads as empty.

Carts arrive as JSON messages:

- `{"type": "cart", "name", "lua", "run"?, "images"?}`: a cart (opened at once unless `run` is false)
- `{"type": "cart.images", "name", "size", "crc", "off", "b64"}`: its images, in parts, before it
- `{"type": "cart.image", "name", "b64"}` or `{..., "missing": true}`: a live image the cart asked for
- `{"type": "cart", "open": name}`, `{"close": true}`, `{"delete": name}`

Over USB they travel as command lines, in pieces, with an offset and checksum on each, so the
sender can resend from where the board is:

```
cart msg begin                         -> @ok 0
cart msg <offset> <crc32> <base64>     -> @ok <bytes so far>   or   @error at <bytes so far>
cart msg end <crc32>                   -> @ok
```

`tools/push.py` speaks this, with four parts in flight. A cart arrives in well under a second,
and 356 KB of images in about 2.3 s.

The other USB commands, each answered on one line starting with `@`:

```
status                  -> @status {"firmware", "cart", "screen", "battery_percent", ...}
battery log [last]      -> @battery {"start", "log": [[minute, mV, percent, flags], ...]}: every
                           10 min, last 96 h; `last` is the run before the last start (saved in NVS
                           hourly, before a low-battery power-off, and on `battery log save`)
cart list               -> the stored carts
cart open NAME | cart close
time <unix seconds>     sets the clock and the RTC (push.py does this)
tz <POSIX TZ>           local time for date() (push.py sends this computer's)
volume N | brightness N the settings, steps 0-3 (as the launcher sets them; kept in NVS)
screen on | screen off  as PWR would (off: dark, and light sleep)
pm                      the power locks and the time in each mode (see Power)
check panel [frames]    see "Building and checking"
check selftest
shot                    a screenshot, in checked parts (tools/shot.py)
```

The log shares the port, so a reader takes only lines that start with `@` as answers.

## Sound (`sfx.cpp`, `esp32/sound.*`)

`sfx_render` synthesizes the eight effects as 16 kHz PCM, from square, triangle and noise
voices with pitch slides and linear envelopes. The same code runs on the board and in
`host/sfx_wav`, which writes them as WAV files for listening. On the board a task on core 0
mixes up to four at once in 10 ms frames, opens the speaker for them, and lets it go after;
the speaker's amplifier draws power only while sound plays, and the codec sleeps two seconds
after the last sound.

## Settings (`firmware/main/console.cpp`)

Volume (off, low, medium, high: the codec's output at 0, 50, 70 or 90; low on a new board) and
brightness (25, 50, 75 or 100%; full on a new board) are kept in NVS under `luanette`, and published to carts as the `settings` channel. The
launcher changes them with `act("volume", step)` and `act("brightness", step)`; USB has the same
commands. With the volume off, `sfx()` is dropped before the speaker opens, so a muted board spends
nothing on sound.

## Power (`firmware/main/console.cpp`)

While the screen is lit the console holds two power-management locks: no light sleep, and the CPU
at full speed. The screen dims to a third of the chosen brightness after a minute without input,
and goes dark after ten (or at once on PWR). Dark means the panel's display off and the panel asleep, the touch controller's interrupt
and both keys armed as wake sources, and both locks released, so the chip drops to 80 MHz and
light-sleeps between interrupts. Battery is read every 5 s; under 3.4 V for a minute on battery,
the PMIC turns the board off before the cell browns out.

With USB connected the chip never light-sleeps (`CONFIG_USJ_NO_AUTO_LS_ON_CONNECTION`), because
the USB serial port stops working in light sleep. Measure battery life unplugged; `battery log`
reads back the readings afterwards, and `battery log last` the run before a restart, so a run that
ends in a low-battery power-off isn't lost.

`pm` over USB prints every power lock, how long each was held, and the time spent in each mode,
light sleep included (`CONFIG_PM_PROFILING`, on in `sdkconfig.defaults`). If a dark board drains
fast, this shows what keeps it awake: with the screen dark, nothing but `usb_serial_jtag` (and
only while USB is connected) should be held. A cart's deadline timer, for one, used to hold
`APB_FREQ_MAX` for as long as the cart was open, and the launcher is always open.

## Building and checking

The firmware (PlatformIO fetches ESP-IDF and the two managed components the first time):

```bash
cd firmware
pio run -t upload            # build and flash over USB
pio device monitor           # the log
```

The desktop side:

```bash
cmake -S host -B host/build && cmake --build host/build
host/build/host_tests                         # the synth and the battery gauge
host/build/cartrun CART.lua INPUT.json OUT/   # a cart's frames on the desktop
host/build/sfx_wav OUT/                       # the sounds as WAV files
host/build/stagerun 28 scene.ppm               # the 3D stage's test scene, drawn band by band
python tests/parity.py tests/carts/primitives.lua
```

`parity.py` runs a cart through `check.py` and through `cartrun` and compares every frame. Carts
that use `rnd()` differ by design; `primitives.lua`, `triangles.lua` and `lazy.lua` must match
exactly.

On the board, over USB (`pio device monitor`, or any serial terminal):

- **Pixels.** `check panel 300` makes the sender keep a copy of what it has written to the
  glass and compare it with the whole frame composed afresh, for 300 frames. The panel can't be
  read back, so this is how narrow windows are checked. The log says "the glass as composed".
- **Vector loops.** `check selftest` runs the PIE loops (`hd_pie.S`, `stage_pie.S`) against
  their C versions at every alignment. Both must say "bit for bit".
- **Speed.** Every 5 s the log prints a stats line for the open cart: frames, Lua time, publish
  and send time, scan wait, late frames. `tests/bench/run.py --save before`, then `--compare
  before` after a change, runs the benchmark carts and prints the difference for each;
  `tests/bench/stage_vs_jet.py` does the 3D stage.

## Porting to another board

The runtime, the layer, the stage and the synth need nothing from the board. The work is in the
player's `Device` and in a few constants.

### What a board needs

- An ESP32-S3 with PSRAM. The Lua heap, the layer's rows and the stage's meshes live there. The
  PIE loops are S3 only; the C versions run anywhere but are slower. Two cores are assumed: the
  sender runs on core 0 while Lua runs on core 1.
- A panel driver that can write an arbitrary window from a DMA buffer and call back when done.
  `board/panel.cpp` is one for the CO5300. Most SPI LCD controllers can do the same.
- Optionally a TE line. Without it, frames go out unsynchronised: they still arrive, but can
  tear on fast motion.
- About 52 KB of internal DMA-capable RAM for the two band buffers (32 rows of 410 pixels
  each, at 2 bytes a pixel).

### What is hard-coded for the AMOLED-2.06

In `cart_player.cpp`: the picture at 3x (`kCartScale`), placed at x 12, y 60 (`kCartX`,
`kCartY`), the glass 502 rows tall (`kGlassRows`), the 24-row strip at the top that a swipe
leaves from. In `stage.hpp` and `hd.hpp`: the 384x384 window, and 410x502 as the largest
full canvas. In `cart.hpp`: `kStageW = 410`, the stage's full width. A board with another glass
needs these changed and probably a different scale. A glass narrower than 384 pixels needs the
picture at 2x and the stage and layer windows sized to match. That is the largest piece of
porting work, and nobody has done it yet.

### Input

The player turns touch into the cart's buttons: a tap is A, a drag is the d-pad,
BOOT is B, and a swipe down from the top leaves. A board with real buttons should feed them in
the firmware (`set_button_b` exists for B; the d-pad would need a similar call) and can drop the
gestures.

### Where to start

`board/` is the only directory that knows this board. Write the same
classes for yours (`board.hpp` lists them: Panel, Touch, Buttons, Imu, Rtc, Speaker, Power,
and the pins in `kConfig`), and `firmware/main` should need little change. `console.cpp` is
the `Device` implementation, screen timeouts and the USB commands, about 630 lines.

## Known limits

- One board. The geometry above is the reason.
- No map editor, sprite editor or music. `sfx` plays eight fixed effects; carts can't define
  their own sounds yet.
- Not PICO-8 compatible. The shape and palette are familiar, but the API differs and PICO-8
  carts don't run.
- Carts arrive over USB only. There is no Wi-Fi or Bluetooth delivery yet; `on_message` is
  where one would plug in.
- No vibration: no motor is fitted, so `buzz()` does nothing (`caps().buzz` is false).
