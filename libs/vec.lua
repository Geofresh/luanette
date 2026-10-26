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
