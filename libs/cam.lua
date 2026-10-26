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
