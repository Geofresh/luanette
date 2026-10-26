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
