-- title: Platformer
-- by: you
-- about: run and jump across a level three screens wide, and pick up every coin
-- api: 5
-- canvas: full

-- sprites (the playground's sprite editor writes this table)
SPRITES = {
  [1] = "00888800 08888880 08f1f180 08ffff80 00888800 08888880 08800880 08800880", -- player
  [2] = "00aaaa00 0aa99aa0 aa9aa9aa aa9aa9aa aa9aa9aa aa9aa9aa 0aa99aa0 00aaaa00", -- coin
  [3] = "bbbbbbbb b3bbb3bb 33b3b333 43334334 44444444 44544444 44444454 44444444", -- grass
  [4] = "44444444 44544444 44444454 44444444 45444444 44444544 44444444 44454444", -- dirt
}
-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "03 0000 18050 1f051 24045", -- jump
  [9] = "03 0000 2f160 34165", -- coin
  [10] = "06 0000 1f050 18053", -- fall
}
-- the level: a character a cell, "." empty, else a sprite (the playground's map editor writes this)
LEVEL = {
  "................................................",
  "................................................",
  "................................................",
  "................................................",
  "..........................................2.2...",
  "...........................2.............33333..",
  "..................2.2.....333...................",
  ".................33333..........2...............",
  ".........2..............................2.......",
  "........333..........2.........333.....333......",
  "..............2.....333.............2...........",
  ".............333...........2.......333..........",
  ".....2...................33333..............2...",
  "....333.........................................",
  "333333333333333333333...33333333333333...3333333",
  "444444444444444444444...44444444444444...4444444",
}

-- The whole screen: the 128x128 picture in the middle, and bands above and below it in the
-- full-color layer (hd) for the title and what to do. Drawn once; they stay.
local c = caps()
local W, H = c.w, c.h
local TOP, BOT = c.picture_y, c.picture_y + 384

local function bands(title, hint)
  hd.scanlines(false)
  hd.gradient(0, 0, W, TOP, 0x1b2440, 0x000000)
  hd.text(title, W / 2, TOP / 2 - 16, 0xffffff, 24, "bold", "center")
  hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x1b2440)
  hd.text(hint, W / 2, BOT + 22, 0x9aa4b8, 14, "mono", "center")
end

local GRAVITY, JUMP, SPEED = 0.25, -3.6, 1.4
local me, score, left, round, cam = {}, 0, 0, 0, 0

-- A wall or floor at pixel (x, y): grass or dirt in that map cell.
local function solid(x, y)
  local t = mget(flr(x / 8), flr(y / 8))
  return t == 3 or t == 4
end

local function start()
  MAP = {table.unpack(LEVEL)}  -- a fresh copy: picking up coins (mset) changes the map
  local w, h = msize()
  left = 0
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      if mget(x, y) == 2 then left = left + 1 end
    end
  end
  me = {x = 8, y = 96, vx = 0, vy = 0, ground = false}
end

function _init()
  bands("PLATFORMER", "drag to run  ·  tap to jump")
  start()
end

function _update()
  me.vx = (btn(0) and -SPEED or 0) + (btn(1) and SPEED or 0)
  if btnp(4) and me.ground then
    me.vy = JUMP
    sfx(8)
  end
  -- across, unless a wall is in the way
  local nx = mid(0, me.x + me.vx, msize() * 8 - 8)
  if not (solid(nx + 1, me.y + 1) or solid(nx + 6, me.y + 1) or solid(nx + 1, me.y + 7) or solid(nx + 6, me.y + 7)) then
    me.x = nx
  end
  -- up or down: land on a floor, bump a ceiling
  me.vy = me.vy + GRAVITY
  local ny = me.y + me.vy
  me.ground = false
  if me.vy > 0 and (solid(me.x + 1, ny + 8) or solid(me.x + 6, ny + 8)) then
    ny, me.vy, me.ground = flr((ny + 8) / 8) * 8 - 8, 0, true
  elseif me.vy < 0 and (solid(me.x + 1, ny) or solid(me.x + 6, ny)) then
    ny, me.vy = flr(ny / 8) * 8 + 8, 0
  end
  me.y = ny
  -- a coin in the cell at the player's middle
  local cx, cy = flr((me.x + 4) / 8), flr((me.y + 4) / 8)
  if mget(cx, cy) == 2 then
    mset(cx, cy)
    score, left = score + 1, left - 1
    sfx(9)
  end
  if left == 0 then
    round = round + 1
    log("round", round, "score", score)
    start()
  end
  if me.y > 128 then  -- fell in a gap: back to the start
    me.x, me.y, me.vy = 8, 96, 0
    sfx(10)
  end
  cam = mid(0, me.x - 60, msize() * 8 - 128)  -- the camera follows, inside the level
end

function _draw()
  cls(12)
  camera(cam, 0)
  map()
  spr(1, me.x, me.y, me.vx < 0)
  camera()
  print("score " .. score, 2, 2, 7)
end
