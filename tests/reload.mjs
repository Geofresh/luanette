// New code into a running cart (Cart::reload), and a cart running the same each time it's given the
// same input (what the playground's rewinding and replays stand on), through the engine the
// command-line tool runs (web/build.sh, then cli/build.sh).
//
//     node tests/reload.mjs
import { loadEngine } from "../cli/luanette.mjs";

const e = await loadEngine();
const reload = e.m.cwrap("lw_reload", "string", ["string"]);
const step = e.m.cwrap("lw_step", "number", ["number", "number", "number", "number", "number", "number"]);
const hash = e.m.cwrap("lw_picture_hash", "number", []);
const logs = () => { const o = []; while (e.logCount()) o.push(e.log()); return o; };
let failed = 0;
const expect = (what, got, want) => {
  if (got !== want) { failed++; console.log(`FAIL ${what}: got ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); }
  else console.log(`ok   ${what}`);
};

// What a reload keeps: the game's changes stay; what it never changed takes the new code's value;
// functions are the new code's, also reached through a local holding a table; a global the new code
// doesn't have goes when the game hadn't changed it.
const A = `
JUMP = 2
score = 0
local speed = 1
local hits = 0
player = {x = 10, y = 20}
SCENE = {name = "play", update = function() score = score + speed end}
local lib = {}
function lib.hello() return "old" end
local cur = SCENE
GONE = 5
function _update() cur.update(); player.x = player.x + 1; hits = hits + 1 end
function _draw() log(score, player.x, player.y, JUMP, speed, hits, lib.hello(), cur.name, rawget(_G, "GONE")) end
`;
const B = A.replace("JUMP = 2", "JUMP = 3").replace("local speed = 1", "local speed = 10").replace('"old"', '"new"')
  .replace('name = "play"', 'name = "play2"').replace("y = 20", "y = 99").replace("GONE = 5\n", "");
e.clock(0);
e.start("reload", A);
for (let i = 0; i < 3; i++) step(0, 0, 0, 0, 0, 0);
expect("before", logs().at(-1), "3 13 20 2 1 3 old play 5");
expect("reload", reload(B), "");
step(0, 0, 0, 0, 0, 0);
expect("after", logs().at(-1), "13 14 99 3 10 4 new play2 nil");
expect("code that doesn't compile", reload("function (").split(":")[0], "doesn't load");
expect("code that fails at its top", reload("error('boom')").split("\n")[0], "in the new code's top: cart:1: boom");
step(0, 0, 0, 0, 0, 0);
expect("still running the last good code", logs().at(-1), "23 15 99 3 10 5 new play2 nil");

// The same input, the same run: tables as keys (hashed by address), strings as keys (hashed with
// Lua's seed), rnd() from the clock the run starts at.
const C = `
things = {}
function _update()
  local t = {n = #things + 1}
  things[t] = rnd(100)
  things["k" .. t.n] = t.n
  if #things < 0 then end
end
function _draw()
  cls()
  local i = 0
  for k, v in pairs(things) do
    i = i + 1
    if type(k) == "table" then pset(i % 128, (i // 128) % 128, flr(v) % 16) else pset(i % 128, 64 + (i // 128) % 64, v % 16) end
  end
end
`;
const runOnce = (clock) => {
  e.clock(clock);
  e.start("same", C);
  const hs = [];
  for (let f = 1; f <= 120; f++) {
    e.clock(clock + f * 33333);
    step(0, 0, 0, 0, 0, 0);
    if (f % 30 === 0) hs.push(hash() >>> 0);
  }
  return hs.join(" ");
};
const first = runOnce(123456);
e.start("between", "x = {} for i = 1, 500 do x[i] = {i} end function _draw() end");  // other memory in use between
expect("the same input runs the same", runOnce(123456), first);
expect("another start clock, other random numbers", runOnce(654321) !== first, true);
process.exitCode = failed ? 1 : 0;
