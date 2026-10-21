# luanette

The command-line tool for [Luanette](https://github.com/Geofresh/luanette), a Lua game console for
ESP32-S3 boards. It runs the console's own engine (WebAssembly, the same as the
[playground](https://geofresh.github.io/luanette/)) in Node, so there's nothing to build:

```bash
npx luanette check game.lua                  # does it run? the same checks as the console's check.py
npx luanette run game.lua --input play.json  # play it with your own input; screenshots and a GIF
npx luanette sound "C4 pulse 5, E4 slide, G4 saw 7 fadeout"
npx luanette link game.lua                   # a link that opens it in the playground
npx luanette new game --template platformer
npx luanette mcp                             # a server for coding agents (MCP)
```

Every command and the input script are in
[docs/cli.md](https://github.com/Geofresh/luanette/blob/main/docs/cli.md). If you're an agent,
read [AGENTS.md](https://github.com/Geofresh/luanette/blob/main/AGENTS.md) first.

MIT licensed.
