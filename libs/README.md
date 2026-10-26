# Libraries

Code a cart can include instead of writing it again: tested, commented, and small once the comments
are stripped. A cart is still one file. A library goes into it between two lines,

```lua
-- lib: tween 1, api 1: move numbers smoothly over time: tween.to(obj, 0.5, {x = 100}, ease.out_back)
...
-- end lib: tween
```

so the cart still sends to a board and shares as one link, and tools can find it later to update
it. Add them from the playground (**File, Add a library**), the command-line tool
(`npx luanette add tween timer mygame.lua`) or the MCP tool `add_library`. Adding one a cart already
has replaces it with this version; anything outside the markers is left alone. Each library's own
comments say how to call it, and each has a demo cart in [`demos/`](demos/).

| library | what it does | bytes | API |
|---|---|---|---|
| [`actor`](actor.lua) | boxes that move through the map a pixel at a time and stop at solid cells (sprite flag 0); `a.ground` when standing; box overlap | 1.4 KB | 6 |
| [`anim`](anim.lua) | sprite animations: frame lists at so many a second, looping or not, by `time()` | 0.6 KB | 1 |
| [`cam`](cam.lua) | a camera that follows a point with a dead zone and easing, stays inside the map, and shakes | 1.2 KB | 1 |
| [`dialog`](dialog.lua) | a text box that types itself out, wraps at words, and pages on A | 2.0 KB | 1 |
| [`particles`](particles.lua) | bursts of sparks, dust and smoke, with gravity, capped at 200 | 1.1 KB | 1 |
| [`state`](state.lua) | scenes (title, game, game over) with enter, update, draw and leave | 0.5 KB | 1 |
| [`timer`](timer.lua) | `timer.after(secs, fn)` and `timer.every(secs, fn)` | 0.9 KB | 1 |
| [`tween`](tween.lua) | move any table's numbers over time along easing curves (`ease.out_back`, `ease.out_bounce`, ...) | 2.0 KB | 1 |
| [`vec`](vec.lua) | 2D vector math on plain numbers: length, normalize, distance, angle in turns, rotate, lerp | 0.8 KB | 1 |

Libraries use locals (`local tween = {}`), so they cost nothing in globals and only the cart code
below them sees them. Most need only `time()`; they go by seconds, so they hold at any frame rate.
Call their `update()` once a frame where they have one.

## Writing one

A library is a `.lua` file here whose first line is `-- lib: NAME VERSION, api N: what it does` and
whose last is `-- end lib: NAME`. Raise VERSION when it changes. Add a demo in `demos/` and run
`npx luanette add NAME libs/demos/NAME.lua` to put the library in it; CI checks every demo and that
each holds its library as it is here.
