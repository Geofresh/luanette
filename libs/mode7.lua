-- lib: mode7 1, api 7: the map as a floor in perspective (F-Zero, Mario Kart), a map turned and zoomed, and sprites standing on the floor
-- A camera looks along the map from (mode7.x, mode7.y), in map pixels, `height` pixels above it,
-- turned `angle` turns (0 looks up the map, 0.25 to the right). In _draw: mode7.floor() draws the
-- floor below the horizon, row by row with tline(); mode7.sprite() stands a sprite on it. Draw the
-- sky above the horizon yourself (rowpal() makes a gradient of one color).
local mode7 = {x = 0, y = 0, angle = 0, height = 24, horizon = 40, focal = 96, far = 1200}

-- Where the camera looks: forward is (fx, fy), right is (rx, ry), in map pixels.
local fx, fy, rx, ry = 0, -1, 1, 0
local function axes()
  local s, c = -sin(mode7.angle), cos(mode7.angle)  -- (sin() is PICO-8's, upside down)
  fx, fy, rx, ry = s, -c, c, s
end

-- The floor: every row from the horizon down to y1 (127), each a line across the map at the
-- distance that row sees. Rows farther than mode7.far are left for the sky. layer: only cells with
-- those sprite flags (map()'s layer).
function mode7.floor(y1, layer)
  axes()
  local x, y, h, f = mode7.x, mode7.y, mode7.height, mode7.focal
  for row = mode7.horizon + 1, y1 or 127 do
    local d = h * f / (row - mode7.horizon)  -- how far ahead this row is
    if d <= mode7.far then
      local s = d / f                         -- map pixels a screen pixel, at that distance
      local cx, cy = x + fx * d - rx * s * 64, y + fy * d - ry * s * 64
      tline(0, row, 127, row, cx / 8, cy / 8, rx * s / 8, ry * s / 8, layer)
    end
  end
end

-- Map point (wx, wy), z pixels above the floor, on the screen: sx, sy and how many screen pixels a
-- map pixel is there; nil when it's behind the camera or too near.
function mode7.project(wx, wy, z)
  axes()
  local dx, dy = wx - mode7.x, wy - mode7.y
  local depth = dx * fx + dy * fy
  if depth < 1 then return nil end
  local k = mode7.focal / depth
  return 64 + (dx * rx + dy * ry) * k, mode7.horizon + (mode7.height - (z or 0)) * k, k, depth
end

-- Sprite n (or the w x h block from n) standing on map point (wx, wy), `size` map pixels tall (8);
-- returns its depth, or nil when it isn't in view. Draw the farthest first.
function mode7.sprite(n, wx, wy, size, w, h, flip)
  local sx, sy, k, depth = mode7.project(wx, wy)
  if not sx then return nil end
  w, h = w or 1, h or 1
  local ph = (size or 8) * k
  local pw = ph * w / h
  if ph < 1 or ph > 400 then return nil end
  sspr(n, sx - pw / 2, sy - ph, pw, ph, flip, false, w, h)
  return depth
end

-- The map turned and zoomed, seen from above: map point (cx, cy), in pixels, at the middle of the
-- screen, turned `angle` turns, `zoom` screen pixels a map pixel. Rows y0 to y1 (0 to 127).
function mode7.turned(cx, cy, angle, zoom, y0, y1, layer)
  local s, c = -sin(angle), cos(angle)
  local k = 1 / zoom
  for row = y0 or 0, y1 or 127 do
    local dy = (row - 64) * k
    local mx, my = cx - c * 64 * k - s * dy, cy - s * 64 * k + c * dy
    tline(0, row, 127, row, mx / 8, my / 8, c * k / 8, s * k / 8, layer)
  end
end
-- end lib: mode7
