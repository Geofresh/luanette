-- title: particles demo
-- by: luanette
-- about: a campfire that smokes and sparks; tap or press A for a burst (the particles library)
-- api: 1

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

function _update()
  particles.update()
  particles.burst(64, 96, 1, {colors = {5, 6, 13}, angle = 0.25, spread = 0.2, speed = 20, life = 1.5, size = 1})
  if rnd(1) < 0.3 then
    particles.burst(64, 100, 1, {colors = {8, 9, 10}, angle = 0.25, spread = 0.3, speed = 50, life = 0.5})
  end
  local x, y = touch()
  if btnp(4) then
    particles.burst(x or 64, y or 50, 30, {colors = {7, 10, 12, 14}, speed = 70, gravity = 120, life = 0.9})
  end
end

function _draw()
  cls(1)
  rectfill(52, 102, 76, 106, 4)
  particles.draw()
  print(particles.count() .. " particles", 2, 2, 6)
end
