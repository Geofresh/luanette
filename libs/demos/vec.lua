-- title: vec demo
-- by: luanette
-- about: an arrow that turns toward your finger, and dots that circle it (the vec library)
-- api: 6

-- lib: vec 1, api 1: 2D vector math on plain numbers: length, normalize, distance, angle, rotate, lerp
-- Vectors here are two numbers, x and y, never tables, so nothing is allocated each frame.
-- Angles are in turns (1 is a full circle), as sin() and cos() take them, and y grows downwards:
-- vec.from(0.25) is 0, -1 (up the screen).
local vec = {}

-- The length of (x, y).
function vec.len(x, y)
  return math.sqrt(x * x + y * y)
end

-- (x, y) scaled to length 1; (0, 0) stays (0, 0).
function vec.norm(x, y)
  local l = math.sqrt(x * x + y * y)
  if l == 0 then return 0, 0 end
  return x / l, y / l
end

-- The distance from (ax, ay) to (bx, by).
function vec.dist(ax, ay, bx, by)
  local dx, dy = bx - ax, by - ay
  return math.sqrt(dx * dx + dy * dy)
end

-- The angle of (x, y), 0 to 1: 0 right, 0.25 up, 0.5 left, 0.75 down.
function vec.angle(x, y)
  return (-math.atan(y, x) / (2 * math.pi)) % 1
end

-- A vector at angle a, len long (1 if left out).
function vec.from(a, len)
  len = len or 1
  return cos(a) * len, sin(a) * len
end

-- (x, y) turned by a (turns; 0.25 is a quarter turn the way vec.angle counts).
function vec.rot(x, y, a)
  local c, s = cos(a), sin(a)
  return x * c - y * s, x * s + y * c
end

-- The point t of the way from (ax, ay) to (bx, by) (t 0 to 1).
function vec.lerp(ax, ay, bx, by, t)
  return ax + (bx - ax) * t, ay + (by - ay) * t
end

-- (x, y), shortened to at most max long.
function vec.limit(x, y, max)
  local l = math.sqrt(x * x + y * y)
  if l > max then return x / l * max, y / l * max end
  return x, y
end
-- end lib: vec

local tx, ty = 100, 30  -- the target: where you touch

function _update()
  local x, y = touch()
  if x then tx, ty = x, y end
end

function _draw()
  cls(1)
  -- an arrow from the middle toward the target, 30 long
  local a = vec.angle(tx - 64, ty - 64)
  local ax, ay = vec.from(a, 30)
  line(64, 64, 64 + ax, 64 + ay, 7)
  local lx, ly = vec.rot(ax, ay, 0.4)  -- the arrowhead: the arrow turned, shortened
  local rx, ry = vec.rot(ax, ay, -0.4)
  lx, ly = vec.limit(lx, ly, 6)
  rx, ry = vec.limit(rx, ry, 6)
  line(64 + ax, 64 + ay, 64 + ax + lx, 64 + ay + ly, 7)
  line(64 + ax, 64 + ay, 64 + ax + rx, 64 + ay + ry, 7)
  -- three dots going round, a third of a turn apart
  for i = 0, 2 do
    local dx, dy = vec.from(time() / 4 + i / 3, 44)
    circfill(64 + dx, 64 + dy, 2, 8 + i)
  end
  circ(tx, ty, 3, 10)
  print("angle " .. flr(a * 360) .. "  dist " .. flr(vec.dist(64, 64, tx, ty)), 2, 2, 6)
  print("touch to aim", 2, 120, 5)
end
