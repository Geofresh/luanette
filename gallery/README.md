# Gallery

Carts people have made, one Lua file each. They're in the
[playground](https://geofresh.github.io/luanette/) under Gallery, and anyone can open one, change
it, and send it to their board.

## Adding yours

In the playground, open your cart and press **Submit to gallery**. That opens a GitHub issue with a
link to the cart already in it; add anything you'd like to say and submit the issue. If the cart is
too big for a link, the issue asks you to attach the .lua file (Save .lua) instead.

What a gallery cart needs:

- It's your own work, or you have the right to share it, and it can be in this repository under the
  MIT license, like everything else here.
- It runs in the playground, and Send to board takes it (so `check.py` passes).
- Its header has `-- title:`, `-- by:` (your name or handle) and `-- about:`.
- It's fine for all ages.
- Made with a coding agent? Say so in the header, `-- made with: Claude Code` (or whichever).
  The gallery marks it "agent-made". Agent-made carts are as welcome as any.

## For maintainers

```bash
python tools/gallery_add.py 'https://geofresh.github.io/luanette/#cart=...'
```

That writes `gallery/NAME.lua` from the link and runs `check.py` on it. Open it in the playground,
then commit it and close the issue. CI checks every gallery cart like the examples, and
`tools/gallery_index.py` (run by the playground's build) refuses a name that's taken or a header
that's missing a line.
