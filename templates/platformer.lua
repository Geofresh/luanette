-- title: Platformer
-- by: you
-- about: jump between platforms and pick up the coins
-- api: 4
-- canvas: full

-- sprites (the playground's sprite editor writes this table)
SPRITES = {
  [1] = "00888800 08888880 08f1f180 08ffff80 00888800 08888880 08800880 08800880", -- player
  [2] = "00aaaa00 0aa99aa0 aa9aa9aa aa9aa9aa aa9aa9aa aa9aa9aa 0aa99aa0 00aaaa00", -- coin
}
-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "03 0000 18050 1f051 24045", -- jump
  [9] = "03 0000 2f160 34165", -- coin
  [10] = "06 0000 1f050 18053", -- fall
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
local floors = {         -- platforms: x, y (their top), w
  {x = 0, y = 120, w = 128}, {x = 12, y = 96, w = 36}, {x = 72, y = 80, w = 40},
  {x = 24, y = 58, w = 32}, {x = 80, y = 40, w = 36},
}
local me = {x = 8, y = 100, vx = 0, vy = 0, ground = false}
local coins, score, level = {}, 0, 1

local function new_coins()
  coins = {}
  for i = 2, #floors do
    local f = floors[i]
    coins[#coins + 1] = {x = f.x + flr(rnd(f.w - 8)), y = f.y - 12}
  end
end

function _init()
  bands("PLATFORMER", "drag to run  ·  tap to jump")
  new_coins()
end

function _update()
  me.vx = (btn(0) and -SPEED or 0) + (btn(1) and SPEED or 0)
  if btnp(4) and me.ground then
    me.vy = JUMP
    sfx(8)
  end
  me.vy = me.vy + GRAVITY
  local was = me.y
  me.x = mid(0, me.x + me.vx, 120)
  me.y = me.y + me.vy
  me.ground = false
  for _, f in ipairs(floors) do  -- land on a platform's top on the way down
    if me.vy >= 0 and was + 8 <= f.y and me.y + 8 >= f.y and me.x + 6 > f.x and me.x + 2 < f.x + f.w then
      me.y, me.vy, me.ground = f.y - 8, 0, true
    end
  end
  for i = #coins, 1, -1 do
    local k = coins[i]
    if abs(me.x - k.x) < 7 and abs(me.y - k.y) < 7 then
      table.remove(coins, i)
      score = score + 1
      sfx(9)
    end
  end
  if #coins == 0 then
    level = level + 1
    log("level", level, "score", score)
    new_coins()
  end
  if me.y > 128 then  -- fell off: back to the start
    me.x, me.y, me.vy = 8, 100, 0
    sfx(10)
  end
end

function _draw()
  cls(12)
  for _, f in ipairs(floors) do rectfill(f.x, f.y, f.x + f.w - 1, f.y + 3, 3) end
  for _, k in ipairs(coins) do spr(2, k.x, k.y) end
  spr(1, me.x, me.y, me.vx < 0)
  print("score " .. score, 2, 2, 7)
end
