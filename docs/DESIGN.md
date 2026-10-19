# Making carts that run well

An ESP32 board gives a cart one core for its Lua, a screen that's filled over a slow link, often a
small battery, and a glass where every pixel counts. The carts that feel best use all of that
glass and do as little work as they can: nothing when nothing changes, and when something does,
only the work that change needs. Each section below is one way to do that, with what it saved on
the board.

`api.md` is the reference for the calls; this file is about putting them together. The launcher
(`engine/builtin/launcher.lua`) does everything here in under 200 lines, and the carts in
`examples/` are complete games to start from. A few of the numbers come from a list-heavy app
built on the same engine, which isn't in this repo.

## 1. Use the whole glass

The glass is 410 x 502 with rounded corners; the 128x128 picture's window is 384 x 384 of it. A
cart that stays in the window leaves about 30% of the screen dark. Give a cart
`-- canvas: full` and lay it out on the whole glass:

- Lay out from `caps()` (`w`, `h`, `picture_x`, `picture_y`), not fixed numbers.
- An app (text, lists, cards, clocks, photos) draws on the full-colour layer, edge to edge: the
  launcher's list spans the glass, rows and clock alike.
- A pixel game keeps its 128x128 picture whole in the window, with no score bar inside it.
  Put the title, the score, what to do and "game over" on the layer above and below it. In Night
  Swarm the whole picture is the night.
- Draw borders and frames around the picture on the layer, not in the picture: the picture's
  edge columns carry on out to the glass's sides, so a coloured edge there becomes two bars.
- The corners are rounded: keep headers, footers and anything near the top or bottom
  `hd.inset(y, h)` in from the sides. The top 24 rows swipe the cart away.

## 2. Run only as fast as something moves

A frame costs the same whether or not anything changed: Lua runs, the layer is checked and the
picture is compared. Don't run frames for nothing.

- Most apps (lists, pages, clocks, menus) move only under the finger:
  `fps(60, idle)` in `_init`. 60 while touched and half a second after, `idle` otherwise.
- Pick `idle` from the slowest thing the cart animates on its own: 1 for a clock showing
  minutes, 2 for seconds, 4 for something that blinks twice a second. A touch or a button wakes
  the cart at once, so a low idle rate costs no response time.
- Something animating for a while (a celebration, a transition): raise the idle rate for its
  length, then put it back.
- A game that moves every frame wants a fixed rate: `fps(30)`, or `_update60` for 60.

The launcher idles at 2 frames a second and drags at 60. On the list app, going from 30 frames
a second to 4 when idle cut its Lua from about 45 ms a second to 8.

## 3. Move things by time, not by frames

With a changing frame rate, a frame is no longer a fixed slice of time. Keep `time()` (or a `dt`
from it) for every animation and timer, and anything you count in frames becomes wrong the
moment the rate changes.

```lua
local last = time()
function _update()
  local dt = time() - last
  last = time()
  x = x + speed * dt
end
```

## 4. The screen keeps what you drew: draw only what changed

The full-colour layer (`hd`) keeps its pixels between frames. Clearing and redrawing all of it
every frame costs most of a frame on its own: redrawing the whole glass is about 18 ms of Lua,
plus 8 ms to find the rows that changed. Drawing nothing costs almost nothing.

- Write one function that draws each part of the screen whole (the header, a row, a card, the
  footer), background first.
- Each frame, ask whether what a part shows has changed, and repaint just that part with its own
  function: `if ui.changed("score", score, lives) then hd.repaint(x, y, w, h, draw_score) end`.
  `hd.repaint` keeps the drawing inside the box, so the function doesn't need to know.
- Ask about every part every frame it's on screen, even on a frame that redraws everything, so
  what's remembered stays what's shown.
- When the layout itself changes (another page, a footer appearing), draw the whole screen
  once: that's one slow frame, not one every frame.

Rebuilding the list app this way kept every pixel the same, cut a quarter of its code, and took
its worst frame from 48.6 ms to 7.4.

## 5. Scroll by moving pixels, not redrawing them

`ui.scroller` handles a box whose content is taller than it. A scroll moves what's already drawn
(across the full width this only remaps rows: no pixel is copied) and draws only the strip that
comes into view. The draw function gets told which rows are being painted: skip everything
outside them.

```lua
local list = ui.scroller(0, HEAD, W, H - HEAD, function(top, y0, y1)
  hd.rect(0, y0, W, y1 - y0, BG)
  for i = math.max(1, (y0 - top) // ROW + 1), #items do  -- only the rows in view
    local y = top + (i - 1) * ROW
    if y >= y1 then break end
    draw_row(items[i], y)
  end
end)
-- _update: list:drag()   _draw: list:draw(), or list:redraw(cy, ch) for an item that changed
```

## 6. Compute when the data changes, not every frame

Parsing, sorting, formatting and laying out are as costly as drawing. Do them when their input
changes and keep the results. `data(...)` returns the same table until the host sends a change:
compare it with the last one before doing anything. The list app's parse takes about 15 ms; it
used to run on every fetch, and now runs only when the data actually differs.

## 7. Two surfaces, two jobs

- The full-colour layer is for what stays: text, cards, photos, logos. Draw it once, repaint on
  change.
- The 128x128 picture is redrawn every frame and costs little to draw: use it for what moves
  every frame (a sprite, a blinking dot, a cursor) over the layer (`hd.order("under")`, colour 0
  shows the layer through).

## 8. Measure it

- `check.py` runs the cart as the board does and estimates each frame's cost on the board
  (`"hd": {"device_ms": ...}`), and warns when a cart redraws too much, stays in the window,
  or keeps running frames with nothing changing.
- On the board, the serial log says every 5 s: `N frames in 5.0 s (rate, late)`, `Lua avg/max`,
  `rows/frame` sent and `layer publish` time. A good cart idles at its idle rate with 0 rows a
  frame, and doesn't run late while dragging.
- What things cost (measured): filling the whole canvas ~6–9 ms, a line of text ~1 ms, a 128 px
  photo ~1.6 ms, ~16 µs after the frame for each row drawn on, a scroll nothing. A whole new screen
  on the glass (every row changed) is about 18 ms of drawing plus 8 ms to find the changed rows,
  and holds 30 fps.

## The shape of a cart that does all of this

```lua
-- title: My list
-- by: you
-- about: a list that scrolls under the finger
-- api: 2
-- canvas: full
local c = caps()
local W, H = c.w, c.h

local function draw_header() ... end           -- each part: one function that draws it whole
local list = ui.scroller(0, HEAD, W, H - HEAD, draw_rows)

function _init()
  fps(60, 2)                                    -- 60 under the finger, 2 otherwise
end

function _update()
  list:drag()
  -- read data here; parse only when it changed
end

function _draw()
  local whole = ui.changed("screen", page)      -- the layout: everything, once
  local changed = {}
  for i, it in ipairs(items) do                  -- every part asked, every frame
    if ui.changed(it.id, it.value) then changed[#changed + 1] = i end
  end
  if whole then
    draw_header()
    list:repaint()
  else
    for _, i in ipairs(changed) do list:redraw((i - 1) * ROW, ROW) end
    list:draw()                                  -- the scroll
  end
end
```
