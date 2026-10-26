-- title: anim demo
-- by: luanette
-- about: a little walker: standing still it blinks, walking it steps (the anim library)
-- api: 4

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

SPRITES = {
  [1] = "00777700 07777770 07171770 07777770 00777700 00777700 00700700 00700700", -- stand
  [2] = "00777700 07777770 07777770 07777770 00777700 00777700 00700700 00700700", -- blink
  [3] = "00777700 07777770 07171770 07777770 00777700 00777700 07000700 07000070", -- step 1
  [4] = "00777700 07777770 07171770 07777770 00777700 00777700 00707000 00070700", -- step 2
}

local stand = anim.new({1, 1, 1, 1, 1, 1, 2}, 4)
local walk = anim.new({3, 4}, 8)
local now, x, left = stand, 60, false

function _init()
  fps(30, 4)  -- lazy: a still screen redraws a few times a second
end

function _update()
  local dx = (btn(1) and 1 or 0) - (btn(0) and 1 or 0)
  x = mid(8, x + dx, 112)
  if dx ~= 0 then left = dx < 0 end
  now = anim.switch(now, dx ~= 0 and walk or stand)
end

function _draw()
  cls(1)
  rectfill(0, 72, 127, 127, 3)
  spr(now:frame(), x, 64, left)
  print("left and right to walk", 2, 2, 6)
end
