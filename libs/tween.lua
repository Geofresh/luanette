-- lib: tween 1, api 1: move numbers smoothly over time: tween.to(obj, 0.5, {x = 100}, ease.out_back)
-- tween.to changes fields of any table over a number of seconds, along an easing curve; call
-- tween.update() once a frame, in _update. It goes by time(), so it holds at any frame rate.
local ease = {}
function ease.linear(t) return t end
function ease.in_quad(t) return t * t end
function ease.out_quad(t) return t * (2 - t) end
function ease.in_out_quad(t)
  if t < 0.5 then return 2 * t * t end
  return -1 + (4 - 2 * t) * t
end
function ease.out_cubic(t)
  t = t - 1
  return t * t * t + 1
end
-- Goes a little past the end, then settles back.
function ease.out_back(t)
  t = t - 1
  return t * t * (2.70158 * t + 1.70158) + 1
end
-- Bounces at the end, like a dropped ball.
function ease.out_bounce(t)
  if t < 1 / 2.75 then return 7.5625 * t * t end
  if t < 2 / 2.75 then t = t - 1.5 / 2.75 return 7.5625 * t * t + 0.75 end
  if t < 2.5 / 2.75 then t = t - 2.25 / 2.75 return 7.5625 * t * t + 0.9375 end
  t = t - 2.625 / 2.75
  return 7.5625 * t * t + 0.984375
end
-- Springs past the end and wobbles back.
function ease.out_elastic(t)
  if t <= 0 or t >= 1 then return t end
  return 2 ^ (-10 * t) * math.sin((t * 10 - 0.75) * (2 * math.pi / 3)) + 1
end

local tween = {}
local running = {}

-- Moves obj's fields to the values in `to` over `secs` seconds, along `easing` (ease.out_quad if
-- left out), then calls done(obj) if given. A field of obj that's already moving stops moving there
-- and goes from where it is now. Returns a handle for tween.cancel.
function tween.to(obj, secs, to, easing, done)
  local t = {obj = obj, t0 = time(), secs = secs, from = {}, to = {}, easing = easing or ease.out_quad, done = done}
  for k, v in pairs(to) do
    for _, o in ipairs(running) do
      if o.obj == obj then o.to[k] = nil end
    end
    t.from[k], t.to[k] = obj[k], v
  end
  running[#running + 1] = t
  return t
end

-- Moves everything along; call once a frame.
function tween.update()
  local now = time()
  local i = 1
  while i <= #running do
    local o = running[i]
    local p = o.secs > 0 and (now - o.t0) / o.secs or 1
    if p >= 1 then
      for k, b in pairs(o.to) do o.obj[k] = b end
      table.remove(running, i)
      if o.done then o.done(o.obj) end
    else
      local e = o.easing(p)
      for k, b in pairs(o.to) do
        local a = o.from[k]
        o.obj[k] = a + (b - a) * e
      end
      i = i + 1
    end
  end
end

-- Stops a tween where it is (its done isn't called).
function tween.cancel(h)
  for i, o in ipairs(running) do
    if o == h then table.remove(running, i) return end
  end
end

-- Whether any of obj's fields are moving (or anything at all, with no obj).
function tween.busy(obj)
  if obj == nil then return #running > 0 end
  for _, o in ipairs(running) do
    if o.obj == obj and next(o.to) then return true end
  end
  return false
end
-- end lib: tween
