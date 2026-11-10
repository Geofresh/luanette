# Example carts

Nine complete carts, each one Lua file. The firmware carries all nine, so they're in the launcher
from the first boot, and the [playground](https://geofresh.github.io/luanette/) runs them in a
browser. To change one on the board, edit it and send your version. It replaces the built-in copy:

```bash
python tools/push.py examples/bubbles.lua
```

| cart | what it is | what it shows |
|---|---|---|
| `lockload.lua` | a light-gun rail shooter in three stages: tap to shoot, tap the magazine to reload | the 3D stage with the 16-color picture and the layer over it, 60 fps |
| `bubbles.lua` | a bubble shooter: drag to aim, release to fire, BOOT swaps the bubble | the full-color layer on the whole glass, redrawn only where it changes |
| `mochi.lua` | a pet that lives in real time: feed it, play with it, send it exploring | `now()` and `save()` (it ages while the cart is closed), sprites, tilt |
| `night_swarm.lua` | survive the swarm: move, and your bolts fire on their own | many moving sprites at 30 fps on the full glass |
| `dice.lua` | shake the board or tap to throw any mix of dice | the 3D stage and tilt: dice tumble with physics and land on a felt table |
| `overdrive.lua` | a chiptune: drums, a galloping bass, a lead and C64-style arpeggios, looping; tap to fire a laser over it | the cart's own sounds and song (`SOUNDS`, `MUSIC`, `music()`), made in the playground's Sounds and Music tabs: open it there to see how it's built |
| `overworld.lua` | walk a world of 1,024 screens with towns, woods, ponds and roads; hold A to run, press A at a door to knock, B shows the whole map | a giant world in ~8 KB: the world is a grid of 64 x 64 blocks, each block 8 x 8 map cells (as Mother 1 stored its world), and `MAP` is only ever the 3 x 3 blocks around you, rebuilt as you walk, so a frame is one `map()` call. What blocks the kid and what's a door are sprite flags (`FLAGS`, `fget`); the villagers talk through the `dialog` library, their faces drawn by `sspr` |
| `hilltop.lua` | a platformer in three levels: run, jump, stomp slimes, dodge spikes, ride springs, collect coins and reach the flag; three lives and a best score | a base for your own platformer: the levels are `LEVEL1` to `LEVEL3` for the Map tab, and what's solid, a coin, a hazard, a spring or the goal is sprite flags, so a new tile only needs its flags. It's built on the `actor`, `cam`, `anim`, `particles` and `state` libraries, runs at 60 fps (`_update60`) with coyote time and a jump buffer, and keeps how it feels in a few numbers at the top of the game code |
| `drift.lua` | a time trial in mode 7: three laps of a track by a lake, steering by drag, keys or tilt, against a ghost of your best lap; sand and grass slow you, trees stop you | how a racer is built on `tline()`: the `mode7` library draws the track as a floor in perspective and stands the trees and the ghost on it, `rowpal()` gives the sky and the lake a shade a row, and `rowshift()` makes the heat shimmer over the horizon. The track is `MAP` (edit it in the Map tab), what's road, sand and grass is sprite flags, and the ghost is kept in the save, 4 characters a sample |

All nine pass `check.py`. Lock & Load is the largest: 32,550 bytes without its comments, just
under the 32 KB limit.

None of them uses `data()` or `act()`, so they need nothing from a host once they're on the
board.
