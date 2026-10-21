# Example carts

Six complete carts, each one Lua file. The firmware carries all six, so they're in the launcher
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

All six pass `check.py`. Lock & Load is the largest: 32,550 bytes without its comments, just
under the 32 KB limit.

None of them uses `data()` or `act()`, so they need nothing from a host once they're on the
board.
