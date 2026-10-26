-- title: actor demo
-- by: luanette
-- about: run and jump through a little level; the map's solid cells are the ones with flag 0 (the actor library)
-- api: 6

-- lib: actor 1, api 6: boxes that move through the map and stop at solid cells: actor.move(me, dx, dy)
-- An actor is any table with x, y (its top left, in pixels) and w, h (its size). Solid cells are
-- map cells whose sprite has flag actor.SOLID (0 unless you change it): set it in the Sprites tab,
-- or FLAGS in the code. Cells off the map are open, unless actor.walls is true.
local actor = {SOLID = 0, walls = false}

-- Is the pixel at (x, y) in a solid cell?
function actor.solid_at(x, y)
  local cx, cy = flr(x / 8), flr(y / 8)
  local t = mget(cx, cy)
  if t == nil then
    if not actor.walls then return false end
    local w, h = msize()
    return cx < 0 or cy < 0 or cx >= w or cy >= h
  end
  return fget(t, actor.SOLID)
end

-- Would a's box, at (x, y), overlap a solid cell? (Points every 8 pixels and at the corners: any size.)
function actor.hits(a, x, y)
  local x1, y1 = x + a.w - 1, y + a.h - 1
  local px = x
  while true do
    local py = y
    while true do
      if actor.solid_at(px, py) then return true end
      if py >= y1 then break end
      py = math.min(py + 8, y1)
    end
    if px >= x1 then break end
    px = math.min(px + 8, x1)
  end
  return false
end

-- Moves a by (dx, dy) pixels, a pixel at a time, stopping against solid cells; fractions add up
-- between calls (a speed of 0.5 moves a pixel every other frame). Returns hit_x, hit_y: whether it
-- stopped against something that way. Afterwards a.ground is true when a stands on something.
function actor.move(a, dx, dy)
  a.rx, a.ry = (a.rx or 0) + dx, (a.ry or 0) + dy
  local mx = a.rx >= 0 and flr(a.rx) or -flr(-a.rx)
  local my = a.ry >= 0 and flr(a.ry) or -flr(-a.ry)
  a.rx, a.ry = a.rx - mx, a.ry - my
  local hit_x, hit_y = false, false
  local s = mx > 0 and 1 or -1
  while mx ~= 0 do
    if actor.hits(a, a.x + s, a.y) then hit_x, a.rx = true, 0 break end
    a.x, mx = a.x + s, mx - s
  end
  s = my > 0 and 1 or -1
  while my ~= 0 do
    if actor.hits(a, a.x, a.y + s) then hit_y, a.ry = true, 0 break end
    a.y, my = a.y + s, my - s
  end
  a.ground = actor.hits(a, a.x, a.y + 1)
  return hit_x, hit_y
end

-- Do the boxes of a and b overlap? (Two actors: the player and a coin, an enemy.)
function actor.overlap(a, b)
  return a.x < b.x + b.w and b.x < a.x + a.w and a.y < b.y + b.h and b.y < a.y + a.h
end
-- end lib: actor

SPRITES = {
  [1] = "bbbbbbbb b3bbb3bb 33b3b333 43334334 44444444 44544444 44444454 44444444", -- grass
  [2] = "66666666 65555556 65666656 65655656 65655656 65666656 65555556 66666666", -- block
  [3] = "00000000 00000000 00000000 0f0000f0 0f0000f0 00000000 00000000 00000000", -- flowers (not solid)
}
FLAGS = {
  [1] = 1, -- flags 0
  [2] = 1, -- flags 0
}
MAP = {
  "................",
  "................",
  "................",
  "................",
  "................",
  "..........22....",
  "................",
  ".......222......",
  "................",
  "....22..........",
  "................",
  "..............22",
  "........3.3.....",
  "1111111111111111",
  "1111111111111111",
  "1111111111111111",
}

local me = {x = 16, y = 80, w = 6, h = 8, vy = 0}

function _update()
  local dx = (btn(1) and 1.5 or 0) - (btn(0) and 1.5 or 0)
  if me.ground and btnp(4) then me.vy = -4 end
  me.vy = math.min(me.vy + 0.25, 4)
  local _, hit_y = actor.move(me, dx, me.vy)
  if hit_y then me.vy = 0 end
end

function _draw()
  cls(12)
  map()
  rectfill(me.x, me.y, me.x + me.w - 1, me.y + me.h - 1, 8)
  print(me.ground and "on the ground" or "in the air", 2, 2, 7)
end
