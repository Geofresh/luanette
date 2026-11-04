-- title: Hilltop
-- by: geo
-- about: a platformer in three levels: run, jump, stomp the slimes, grab the coins and reach the flag on the hill
-- api: 6
-- canvas: full

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

-- lib: anim 1, api 1: sprite animations: local walk = anim.new({16, 17}, 8); spr(walk:frame(), x, y)
-- An animation is a list of sprite numbers shown at so many a second. It goes by time(), so it
-- holds at any frame rate.
local anim = {}
anim.__index = anim

-- frames: sprite numbers; fps: how many a second (8 if left out); loop: false to stop on the last.
function anim.new(frames, fps, loop)
  return setmetatable({frames = frames, fps = fps or 8, loop = loop ~= false, t0 = time()}, anim)
end

-- From the first frame again.
function anim:restart()
  self.t0 = time()
  return self
end

-- The sprite to draw now.
function anim:frame()
  local n = flr((time() - self.t0) * self.fps)
  if self.loop then n = n % #self.frames else n = math.min(n, #self.frames - 1) end
  return self.frames[n + 1]
end

-- Whether one that doesn't loop has reached its end.
function anim:done()
  return not self.loop and (time() - self.t0) * self.fps >= #self.frames
end

-- Use this to change animations: `me.anim = anim.switch(me.anim, run)`. It restarts the new one
-- only when it's a change, so calling it every frame is fine.
function anim.switch(now, to)
  if now ~= to then to:restart() end
  return to
end
-- end lib: anim

-- lib: cam 1, api 1: a camera that follows the player with a dead zone, stays inside the map, and shakes
-- In _draw: cam.follow(me.x, me.y), then cam.apply() before drawing the world, and camera() before
-- the score. The view is 128 x 128 pixels; cam.x, cam.y is its top left in the world.
local cam = {x = 0, y = 0, zone_w = 24, zone_h = 16, smooth = 0.25}

-- Keeps the view inside the box from (x0, y0) to (x1, y1) in pixels; with no box set, inside the
-- map (msize()) when the cart has one.
function cam.bounds(x0, y0, x1, y1)
  cam.box = x0 and {x0, y0, x1, y1} or nil
end

local function clamp()
  local b = cam.box
  if not b and MAP then
    local w, h = msize()
    b = {0, 0, w * 8, h * 8}
  end
  if not b then return end
  cam.x = b[3] - b[1] <= 128 and b[1] or mid(b[1], cam.x, b[3] - 128)
  cam.y = b[4] - b[2] <= 128 and b[2] or mid(b[2], cam.y, b[4] - 128)
end

-- Moves the view so (x, y) stays in a zone_w x zone_h box in the middle, easing there by `smooth`
-- of the way each frame (1: at once).
function cam.follow(x, y)
  local cx, cy = x - 64, y - 64
  local hw, hh = cam.zone_w / 2, cam.zone_h / 2
  local tx, ty = cam.x, cam.y
  if cx > tx + hw then tx = cx - hw elseif cx < tx - hw then tx = cx + hw end
  if cy > ty + hh then ty = cy - hh elseif cy < ty - hh then ty = cy + hh end
  cam.x = cam.x + (tx - cam.x) * cam.smooth
  cam.y = cam.y + (ty - cam.y) * cam.smooth
  clamp()
end

-- Centers on (x, y) at once (a level's start).
function cam.snap(x, y)
  cam.x, cam.y = x - 64, y - 64
  clamp()
end

-- Shakes the view for secs seconds, by up to px pixels (2 if left out).
function cam.shake(secs, px)
  cam.shake_until, cam.shake_px = time() + secs, px or 2
end

-- Sets camera() to the view (with any shake).
function cam.apply()
  local ox, oy = 0, 0
  if cam.shake_until and time() < cam.shake_until then
    local p = cam.shake_px
    ox, oy = flr(rnd(2 * p + 1)) - p, flr(rnd(2 * p + 1)) - p
  end
  camera(flr(cam.x + 0.5) + ox, flr(cam.y + 0.5) + oy)
end
-- end lib: cam

-- lib: particles 1, api 1: bursts of sparks, dust and smoke: particles.burst(x, y, 12, {color = 10})
-- Call particles.update() once a frame in _update and particles.draw() in _draw. At most
-- particles.max at once (200), so a big burst can't use up a frame's budget.
local particles = {max = 200}
local list, last = {}, nil

-- n particles from (x, y). o (all optional): color, or colors (a list to pick from); speed (pixels
-- a second, 40); angle (turns, 0.25 is up) and spread (turns, 1 is every way); life (seconds, 0.6);
-- gravity (pixels a second, each second: 200 falls); size (a radius; 0 is a pixel).
function particles.burst(x, y, n, o)
  o = o or {}
  for _ = 1, n do
    if #list >= particles.max then return end
    local a = (o.angle or 0) + (rnd(1) - 0.5) * (o.spread or 1)
    local s = (o.speed or 40) * (0.5 + rnd(0.5))
    list[#list + 1] = {x = x, y = y, vx = cos(a) * s, vy = sin(a) * s, t0 = time(),
                       life = (o.life or 0.6) * (0.7 + rnd(0.6)), g = o.gravity or 0,
                       c = o.colors and rnd(o.colors) or o.color or 7, r = o.size or 0}
  end
end

function particles.update()
  local now = time()
  local dt = last and math.min(now - last, 0.1) or 0
  last = now
  local i = 1
  while i <= #list do
    local p = list[i]
    if now - p.t0 >= p.life then
      list[i] = list[#list]  -- the last one takes its place: order doesn't matter
      list[#list] = nil
    else
      p.vy = p.vy + p.g * dt
      p.x, p.y = p.x + p.vx * dt, p.y + p.vy * dt
      i = i + 1
    end
  end
end

function particles.draw()
  for _, p in ipairs(list) do
    if p.r > 0 then circfill(p.x, p.y, p.r, p.c) else pset(p.x, p.y, p.c) end
  end
end

function particles.count()
  return #list
end

function particles.clear()
  list = {}
end
-- end lib: particles

-- lib: state 1, api 1: scenes (a title, the game, game over), each with its own update and draw
-- A scene is a table with any of enter(...), update(), draw() and leave(). state.go(scene, ...)
-- switches; call state.update() in _update and state.draw() in _draw.
local state = {}
local current, since = nil, 0

-- Leaves the scene there is and enters this one (enter gets the other arguments).
function state.go(scene, ...)
  if current and current.leave then current.leave() end
  current, since = scene, time()
  if scene.enter then scene.enter(...) end
end

function state.update()
  if current and current.update then current.update() end
end

function state.draw()
  if current and current.draw then current.draw() end
end

-- Whether this is the scene now.
function state.is(scene)
  return current == scene
end

-- Seconds since this scene began (for a title that fades in, a "press A" that waits a moment).
function state.time()
  return time() - since
end
-- end lib: state

-- A whole small platformer, made to be a base for yours: change the levels in the Map tab (LEVEL1
-- to LEVEL3), the art in the Sprites tab, the sounds and the song in their tabs, and the numbers
-- under "How it feels" below. What the player stands on, picks up and gets hurt by is sprite flags,
-- so a new tile only needs its flags set.

SPRITES = {
  [1] = "bbbbbbbb b3bbb3bb 33b3b333 43334334 44444444 44544444 44444454 44444444", -- grass
  [2] = "44444444 44544444 44444454 44444444 45444444 44444544 44444444 44454444", -- dirt
  [3] = "66666665 67777765 67666665 66666665 55555555 66656666 66656666 55555555", -- stone
  [4] = "44444444 49999994 49444494 49499494 49499494 49444494 49999994 44444444", -- crate
  [5] = "00000000 00000000 00700070 00700070 06760676 06760676 66666666 55555555", -- spikes
  [6] = "00aaaa00 0aa99aa0 aa9aa9aa aa9aa9aa aa9aa9aa aa9aa9aa 0aa99aa0 00aaaa00", -- coin
  [7] = "07888000 07888880 07888888 07888880 07888000 07000000 07000000 07000000", -- flag
  [8] = "07000000 07000000 07000000 07000000 07000000 07000000 07000000 07000000", -- pole
  [9] = "00000000 00077000 07777770 77777777 77777777 06777760 00000000 00000000", -- cloud
  [10] = "00000000 00000000 00000000 00333000 03bbb300 3bbbbb30 3b3bb3b3 33333333", -- bush
  [11] = "00000000 00000000 00e00000 0eae0000 00e00000 00300000 03300300 00333000", -- flower
  [12] = "08800880 88888888 88888888 88888888 08888880 00888800 00088000 00000000", -- heart
  [13] = "00000000 00000000 00000000 88888888 28888882 00500500 00055000 05555550", -- spring
  [16] = "00888800 08888888 00ff1f00 00ffff00 0cccccc0 0fccccf0 00111100 00100100", -- player: standing
  [17] = "00888800 08888888 00ff1f00 00ffff00 0cccccf0 fccccc00 00111100 01000010", -- running
  [18] = "00888800 08888888 00ff1f00 00ffff00 fcccccc0 0ccccc0f 00111100 00011000", -- running
  [19] = "00888800 08888888 00ff1f00 f0ffff0f 0cccccc0 00cccc00 00111100 01000010", -- jumping
  [20] = "00000000 00000000 00333300 03bbbb30 3b1bb1b3 3bbbbbb3 3bbbbbb3 03333330", -- slime
  [21] = "00000000 00000000 00000000 00333300 03bbbb30 3b1bb1b3 3bbbbbb3 33333333", -- slime, squeezed
  [22] = "00000000 00000000 00000000 00000000 00000000 00333300 3bbbbbb3 33333333", -- slime, stomped
}
-- sprite flags: fget(n, f) (the playground's sprite editor writes this table)
FLAGS = {
  [1] = 1, -- flags 0
  [2] = 1, -- flags 0
  [3] = 1, -- flags 0
  [4] = 1, -- flags 0
  [5] = 4, -- flags 2
  [6] = 2, -- flags 1
  [7] = 8, -- flags 3
  [8] = 8, -- flags 3
  [13] = 16, -- flags 4
}
-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "0e 0000 0c355 00000 18355 00000 0c355 0c355 18355 00000 07355 00000 13355 00000 07355 07355 13355 00000 09355 00000 15355 00000 09355 09355 15355 00000 05355 00000 11355 00000 05355 05355 11355 00000", -- bass
  [9] = "0e 0000 28140 00000 2b140 00000 28140 00000 24140 26140 2f140 00000 2b140 00000 26140 00000 23140 26140 28140 00000 2d140 00000 30140 00000 2d140 2b140 29140 00000 28140 00000 26140 00000 24145 00000", -- tune
  [10] = "03 0000 18050 1f051 24045", -- jump
  [11] = "03 0000 2f160 34165", -- coin
  [12] = "02 0000 20730 18733", -- stomp
  [13] = "04 0000 18450 13450 0c453", -- hurt
  [14] = "02 0000 18350 1f351 24351 2b351 30355", -- spring
  [15] = "06 0000 24150 28150 2b150 30155", -- level clear
  [16] = "08 0000 24350 1f350 1b350 18355", -- game over
}
MUSIC = {
  [0] = "03 08 09 -- --",
}

-- The levels: a character a cell (the Map tab paints them). "." is empty; the others are sprites:
-- 1 grass, 2 dirt, 3 stone, 4 crate, 5 spikes, 6 coin, 7 and 8 the flag, a bush, b flower,
-- d spring. g is where the player starts and k a slime: the game takes those two off the map.
LEVEL1 = {
  "........................................................................",
  "........................................................................",
  "........................................................................",
  "........................................................................",
  "........................................................................",
  ".........................................6666...........................",
  "........................................................................",
  "........................................................................",
  "........................................................................",
  "..........................................................666...........",
  "........666........................6666.............................7...",
  "........444......6666...............a.......................3.......8...",
  "..................................111111....666............33.......8...",
  "..g..a...b....a........b...k......222222..d.......b..k....333.......8.a.",
  "111111111111111111..111111111111112222221111...1111111111111111111111111",
  "222222222222222222..222222222222222222222222...2222222222222222222222222",
}
LEVEL2 = {
  "........................................................................................",
  "........................................................................................",
  "........................................................................................",
  "........................................................................................",
  "..................................................................666...................",
  "........................................................................................",
  "........................................................................................",
  "........................................................................................",
  ".............................................666.....................4444...............",
  ".............................................444........................................",
  "...............6666....66666................................66......................7...",
  "...............4444...........666...................................................8...",
  "..............................444.........44................44......................8...",
  "..g...a........5555......k...........k.b.........555..k.k.........d.........k..55...8...",
  "111111111111111111111111111111...11111111....11111111111111....1111111111111111111111111",
  "222222222222222222222222222222...22222222....22222222222222....2222222222222222222222222",
}
LEVEL3 = {
  "................................................................................................",
  "................................................................................................",
  "................................................................................................",
  "................................................................................................",
  "..................................................66666.....................................7...",
  "............................................................................................8...",
  "............................................................................................8...",
  "....................................6666.........................................b..k.a.....8...",
  "....................................4444......................................111111111111111111",
  "...........................................................k.a................222222222222222222",
  "..........6666.................k.......................111111111..............222222222222222222",
  "..........................11111111...............d.....222222222..............222222222222222222",
  "..................k.......22222222........11111111.....222222222..............222222222222222222",
  "..g..b........11111111....22222222..5555..22222222.....222222222...k.....d....222222222222222222",
  "1111111111....22222222....222222221111111122222222.....2222222221111111111....222222222222222222",
  "2222222222....22222222....222222222222222222222222.....2222222222222222222....222222222222222222",
}
local LEVELS = {LEVEL1, LEVEL2, LEVEL3}
local NAMES = {"the meadow", "crates and spikes", "the hilltop"}

-- How it feels: speeds in pixels a frame and frames, at 60 frames a second (_update60). A jump
-- goes about 30 pixels up (almost 4 cells) and 54 across at a run.
local RUN, ACCEL, AIR_ACCEL = 1.1, 0.12, 0.08
local GRAVITY, FALL_MAX = 0.1, 3
local JUMP, SPRING_JUMP, STOMP_BOUNCE = -2.45, -3.7, -2
local COYOTE, BUFFER = 6, 6      -- a jump still works this soon after leaving a ledge, or before landing
local SLIME_SPEED = 0.25
local LIVES = 3

-- The flags (FLAGS above; set them in the Sprites tab): actor.SOLID is flag 0.
local COIN, HURTS, GOAL, SPRING = 1, 2, 3, 4
local START_CELL, SLIME_CELL = 16, 20

-- The screen: the 128 x 128 picture, and bands above and below it in the full-color layer.
local c = caps()
local W, H = c.w, c.h
local TOP, BOT = c.picture_y, c.picture_y + 384

local function bands(sub)
  hd.scanlines(false)
  hd.gradient(0, 0, W, TOP, 0x1b3a5c, 0x000000)
  hd.text("HILLTOP", W / 2, TOP / 2 - 22, 0xffffff, 24, "bold", "center")
  hd.text(sub, W / 2, TOP / 2 + 8, 0x9fd0ff, 14, "mono", "center")
  hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x1b3a5c)
  hd.text("drag to run  ·  drag up or tap to jump", W / 2, BOT + 22, 0x9aa4b8, 12, "mono", "center")
end

local me, slimes = {}, {}
local level, lives, score, coins, best = 1, LIVES, 0, 0, 0
local run_anim = anim.new({17, 18}, 10)
local slime_anim = anim.new({20, 21}, 3)

-- Does the map cell under pixel (x, y) have flag f?
local function flag_at(x, y, f)
  local t = mget(flr(x / 8), flr(y / 8))
  return t ~= nil and fget(t, f)
end

-- The cells the player's box touches with flag f: the first one's cell coordinates, or nil.
local function touching(f)
  for _, p in ipairs({{me.x, me.y}, {me.x + me.w - 1, me.y}, {me.x, me.y + me.h - 1}, {me.x + me.w - 1, me.y + me.h - 1}}) do
    if flag_at(p[1], p[2], f) then return flr(p[1] / 8), flr(p[2] / 8) end
  end
end

local function load_level(n)
  MAP = {table.unpack(LEVELS[n])}  -- a copy: picking up coins (mset) changes the map
  slimes = {}
  local w, h = msize()
  for cy = 0, h - 1 do
    for cx = 0, w - 1 do
      local t = mget(cx, cy)
      if t == START_CELL then
        me = {x = cx * 8 + 1, y = cy * 8, w = 6, h = 8, vx = 0, vy = 0, coyote = 0, buffer = 0, face = 1, hurt = 0}
        mset(cx, cy)
      elseif t == SLIME_CELL then
        slimes[#slimes + 1] = {x = cx * 8 + 1, y = cy * 8 + 2, w = 6, h = 6, vy = 0, dir = -1}
        mset(cx, cy)
      end
    end
  end
  me.sx, me.sy = me.x, me.y
  cam.snap(me.x, 64)
  bands(("level %d: %s"):format(n, NAMES[n]))
  log("level", n, "slimes", #slimes)
end

-- Scenes (the state library): the title, playing, a level cleared, the end.
local title, play, cleared, over = {}, {}, {}, {}

local function jump_pressed() return btnp(4) or btnp(5) or btnp(2) end

-- Hurt by a slime or spikes: a life gone, a moment of blinking, and back to the start if it fell.
local function hurt(fell)
  if me.hurt > 0 and not fell then return end
  lives = lives - 1
  log(fell and "fell" or "hurt", "lives", lives, "x", flr(me.x))
  sfx(13)
  cam.shake(0.3, 2)
  particles.burst(me.x + 3, me.y + 4, 12, {colors = {8, 14, 7}, speed = 50})
  if lives == 0 then
    state.go(over, false)
    return
  end
  me.hurt = 90
  if fell then
    me.x, me.y, me.vx, me.vy = me.sx, me.sy, 0, 0
    cam.snap(me.x, 64)
  else
    me.vy, me.vx = -2, -me.face * 1
  end
end

local function update_player()
  local want = (btn(0) and -1 or 0) + (btn(1) and 1 or 0)
  if want ~= 0 then me.face = want end
  local accel = me.ground and ACCEL or AIR_ACCEL
  me.vx = me.vx + mid(-accel, want * RUN - me.vx, accel)
  -- jumping: pressed a little before landing or a little after leaving a ledge still counts
  me.buffer = jump_pressed() and BUFFER or me.buffer - 1
  me.coyote = me.ground and COYOTE or me.coyote - 1
  if me.vy >= 0 and touching(SPRING) then
    me.vy, me.buffer, me.coyote = SPRING_JUMP, 0, 0  -- run or fall onto a spring: it throws you, jump or not
    sfx(14)
  elseif me.buffer > 0 and me.coyote > 0 then
    me.vy, me.buffer, me.coyote = JUMP, 0, 0
    sfx(10)
    particles.burst(me.x + 3, me.y + 8, 5, {color = 6, speed = 20, angle = 0.25, spread = 0.5, life = 0.3})
  end
  me.vy = min(me.vy + GRAVITY, FALL_MAX)
  local hit_x, hit_y = actor.move(me, me.vx, me.vy)
  if hit_x then me.vx = 0 end
  if hit_y then me.vy = 0 end
  local w = msize()
  me.x = mid(0, me.x, w * 8 - me.w)
  -- coins
  local cx, cy = touching(COIN)
  if cx then
    mset(cx, cy)
    score, coins = score + 10, coins + 1
    sfx(11)
    particles.burst(cx * 8 + 4, cy * 8 + 4, 8, {colors = {10, 9, 7}, speed = 30, life = 0.4})
  end
  if me.hurt > 0 then me.hurt = me.hurt - 1 end
  -- spikes hurt only their pointed half: the bottom 4 pixels of the box in a spike cell's top half
  local sx, sy = touching(HURTS)
  if sx and me.y + me.h > sy * 8 + 2 then hurt(false) end
  if me.y > 136 then hurt(true) end
  if touching(GOAL) then state.go(cleared) end
end

local function update_slimes()
  for i = #slimes, 1, -1 do
    local s = slimes[i]
    if s.dead then
      if time() - s.dead > 0.4 then table.remove(slimes, i) end
    else
      s.vy = min(s.vy + GRAVITY, FALL_MAX)
      local hit_x, hit_y = actor.move(s, s.dir * SLIME_SPEED, s.vy)
      if hit_y then s.vy = 0 end
      -- turn at walls, and at ledges so they stay on their ground
      local ahead = s.dir > 0 and s.x + s.w or s.x - 1
      if hit_x or (s.ground and not actor.solid_at(ahead, s.y + s.h)) then s.dir = -s.dir end
      if s.y > 136 then table.remove(slimes, i)
      elseif actor.overlap(me, s) then
        if me.vy > 0 and me.y + me.h - s.y < 5 then  -- landing on top: stomped
          s.dead = time()
          me.vy = STOMP_BOUNCE
          score = score + 50
          sfx(12)
          particles.burst(s.x + 3, s.y + 4, 10, {colors = {11, 3}, speed = 40})
        else
          hurt(false)
        end
      end
    end
  end
end

-- Hills and clouds behind the level, moving slower than it: the world looks deep.
local function backdrop()
  cls(12)
  local far = cam.x * 0.2
  clip(1, 0, 126, 128)  -- the picture's edge columns carry on out to the glass's edges: keep sprites off them
  for i = 0, 6 do
    local x = (i * 40 - far) % 240 - 40
    spr(9, x, 14 + (i * 23) % 30)
    spr(9, x + 8, 14 + (i * 23) % 30)
  end
  clip()
  local near = cam.x * 0.5
  for i = 0, 5 do
    circfill((i * 48 - near) % 288 - 48, 128, 30 + (i * 13) % 14, 3)
  end
end

local function hud()
  for i = 1, lives do spr(12, 2 + (i - 1) * 9, 2) end
  spr(6, 96, 2)
  print(coins, 106, 4, 7)
  print(score, 126 - #tostring(score) * 4, 12, 7)
end

function title.enter()
  bands(best > 0 and ("best: " .. best) or "a platformer in three levels")
  music(-1)
end
function title.update()
  if jump_pressed() then
    level, lives, score, coins = 1, LIVES, 0, 0
    state.go(play)
  end
end
function title.draw()
  cls(12)
  circfill(30, 150, 60, 3)
  circfill(110, 160, 60, 11)
  print("HILLTOP", 50, 30, 7)
  sspr(16, 52, 48, 24, 24)  -- the player, three times its size
  if time() % 1 < 0.6 then print("tap to start", 40, 92, 7) end
end

function play.enter()
  load_level(level)
  music(0)
end
function play.update()
  update_player()
  update_slimes()
  particles.update()
end
function play.draw()
  cam.follow(me.x + 3, 64)
  backdrop()
  cam.apply()
  map()
  clip(1, 0, 126, 128)
  for _, s in ipairs(slimes) do
    spr(s.dead and 22 or slime_anim:frame(), s.x - 1, s.y - 2, s.dir > 0)
  end
  if me.hurt % 8 < 5 then  -- blinking while it can't be hurt
    local n = not me.ground and 19 or (me.vx > 0.2 or me.vx < -0.2) and run_anim:frame() or 16
    spr(n, me.x - 1, me.y, me.face < 0)
  end
  particles.draw()
  clip()
  camera()
  hud()
end

function cleared.enter()
  log("cleared", level, "score", score)
  music(-1)
  sfx(15)
  score = score + lives * 100
end
function cleared.update()
  particles.update()
  if state.time() > 2 then
    if level == #LEVELS then state.go(over, true) else level = level + 1 state.go(play) end
  end
end
function cleared.draw()
  play.draw()
  rectfill(24, 52, 103, 75, 1)
  print("level clear!", 40, 57, 10)
  print("+" .. lives * 100 .. " for lives", 34, 66, 7)
end

function over.enter(won)
  over.won = won
  music(-1)
  bands(won and "you made it to the top" or "game over")
  if not won then sfx(16) end
  if score > best then
    best = score
    save({best = best})
  end
end
function over.update()
  particles.update()
  if over.won and rnd(1) < 0.2 then particles.burst(rnd(128), rnd(64), 6, {colors = {8, 9, 10, 11, 12, 14}, speed = 40}) end
  if state.time() > 1 and jump_pressed() then state.go(title) end
end
function over.draw()
  cls(1)
  print(over.won and "you made it!" or "game over", over.won and 40 or 46, 40, over.won and 10 or 8)
  print("score " .. score, 44, 56, 7)
  print("best " .. best, 48, 66, 6)
  particles.draw()
  if time() % 1 < 0.6 then print("tap for the title", 30, 92, 7) end
end

function _init()
  local saved = load()
  best = saved and saved.best or 0
  state.go(title)
end

function _update60()
  state.update()
end

function _draw()
  state.draw()
end
