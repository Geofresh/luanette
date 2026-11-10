-- title: Drift
-- by: geo
-- about: a time trial in mode 7: three laps around the lake, against the ghost of your best lap
-- api: 7
-- canvas: full

-- lib: mode7 2, api 7: the map as a floor in perspective (F-Zero, Mario Kart), a map turned and zoomed, and sprites standing on the floor
-- A camera looks along the map from (mode7.x, mode7.y), in map pixels, `height` pixels above it,
-- turned `angle` turns (0 looks up the map, 0.25 to the right). In _draw: mode7.floor() draws the
-- floor below the horizon, row by row with tline(); mode7.sprite() stands a sprite on it. Draw the
-- sky above the horizon yourself (rowpal() makes a gradient of one color).
local mode7 = {x = 0, y = 0, angle = 0, height = 24, horizon = 40, focal = 96, far = 1200}

-- Where the camera looks: forward is (fx, fy), right is (rx, ry), in map pixels (worked out again
-- only when the angle changes: project() is called for every sprite).
local fx, fy, rx, ry, seen = 0, -1, 1, 0, nil
local function axes()
  if mode7.angle == seen then return end
  seen = mode7.angle
  local s, c = -sin(seen), cos(seen)  -- (sin() is PICO-8's, upside down)
  fx, fy, rx, ry = s, -c, c, s
end

-- The floor: every row from the horizon down to y1 (127), each a line across the map at the
-- distance that row sees. Rows farther than mode7.far are left for the sky. layer: only cells with
-- those sprite flags (map()'s layer).
function mode7.floor(y1, layer)
  axes()
  local x, y, hf, hz, far = mode7.x / 8, mode7.y / 8, mode7.height * mode7.focal, mode7.horizon, mode7.far
  local lx, ly = (fx * mode7.focal - rx * 64) / 8, (fy * mode7.focal - ry * 64) / 8  -- the row's left end, per unit of s
  local sx, sy = rx / 8, ry / 8
  for row = max(hz + 1, flr(hz + hf / far) + 1), y1 or 127 do  -- (rows nearer the horizon are farther than far)
    local s = hf / (row - hz) / mode7.focal  -- map pixels a screen pixel, at the distance this row sees
    tline(0, row, 127, row, x + lx * s, y + ly * s, sx * s, sy * s, layer)
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
  if ph < 1 or ph > 160 then return nil end
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

-- sprites (the playground's sprite editor writes this table)
SPRITES = {
  [1] = "33333333 333b3333 33333333 3333333b 3b333333 33333333 33333b33 33333333", -- grass
  [2] = "55555555 55565555 55555555 55555565 55555555 56555555 55555555 55555555", -- road
  [3] = "88887777 88887777 88887777 88887777 77778888 77778888 77778888 77778888", -- curb
  [4] = "ffffffff fff9ffff ffffffff ffffff9f ffffffff f9ffffff ffffffff ffff9fff", -- sand
  [5] = "77117711 77117711 11771177 11771177 77117711 77117711 11771177 11771177", -- finish
  [6] = "3333b333 33333333 b3333333 33333333 333333b3 33b33333 33333333 3333333b", -- grass
  [16] = "00033000 00333300 03333b30 333b3333 03333330 00044000 00044000 00444400", -- tree
  [32] = "0000000c 000000c7 00000888 11108888 11188888 11188998 11188888 00005550", -- kart, left half
  [33] = "c0000000 7c000000 88800000 88880111 88888111 89988111 88888111 05550000", -- kart, right half
  [34] = "000000cc 00000c77 00008888 11108888 11188888 11188998 11188888 00005550", -- kart, turning left, left half
  [35] = "00000000 c0000000 88000000 88880111 88888111 89988111 88888111 05550000", -- kart, turning left, right half
  [36] = "00000000 0000000c 00000088 11108888 11188888 11188998 11188888 00005550", -- kart, turning right, left half
  [37] = "cc000000 77c00000 88880000 88880111 88888111 89988111 88888111 05550000", -- kart, turning right, right half
}
-- flags (the playground's sprite editor writes this table): 0 road, 1 sand, 2 grass, 3 the line
FLAGS = {
  [1] = 4, -- flags 2
  [2] = 1, -- flags 0
  [3] = 1, -- flags 0
  [4] = 2, -- flags 1
  [5] = 9, -- flags 0 3
  [6] = 4, -- flags 2
}
-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "0a 0000 21155", -- countdown
  [9] = "06 0000 2d160 2d160 2d155", -- go
  [10] = "05 0000 24050 28050 2b055", -- lap
  [11] = "05 0000 24050 28050 2b060 30060 00000 2b050 30065", -- best lap
  [12] = "03 0000 0c740 0c725", -- bump
  [13] = "0e 0000 09450 00400 09440 09450 0c450 00400 0c440 0c450 05450 00400 05440 05450 07450 00400 07440 07450", -- bass
  [14] = "0e 0000 1c130 00100 21130 00100 24130 23130 21130 00100 1c130 00100 1f130 21130 24130 00100 28135 00100", -- tune
}
MUSIC = {
  [0] = "03 0d 0e -- --",
}
-- the track (the playground's map editor writes this table): 64 x 64 cells, 512 x 512 pixels
MAP = {
  "6111161111611116111161111611116111161111611116111161111611116111",
  "1611116111161111611116111161111611116111161111611116111161111611",
  "1161111611116111161111611116111161111611116111161111611116111161",
  "1116111161111611116111161111611116111161111611116111161111611116",
  "1111611116111161111611116111161111611116111161111611116111161111",
  "6111161111611116111161111611116111161111611116111161111611116111",
  "1611116111161111611116333222223333116111161111611116111161111611",
  "1161111611116444161332222222222222331611116111161111611116111161",
  "1116111161114444433222222222222222222361111611116111161111611116",
  "1111611116144444322222222222222222222223111441111611116111161111",
  "6111161111644443222222222222222222222222314444111161111611116111",
  "1611116111444432222222222222222222222222234444611116111161111611",
  "1161111614444322222222233316133322222222223444461111611116111161",
  "1116111164443222222223161111611113322222222344416111161111611116",
  "1111611116443222222311116111161111632222222244111611116111161111",
  "6111161111632222223161111611116111161322222236111161111611116111",
  "1611116111132222221116111161111611116122222231611116111161111611",
  "1161111611122222231111611116111161111632222231161111611116111161",
  "1116111161322222216111161111611116111132222221116111161111611116",
  "1111611116322222311611116111161111611112222223111611116111161111",
  "6111161111322222311161111611116111161113222223111161111611116111",
  "1611116111222222311116111161111611116113222223611116111161111611",
  "1161111611222222161111611116111161111613222223161111611116111161",
  "1116111163222222116111161111611116111163222222116111161111611116",
  "1111611113222223111611116111161111611116222222333611116411161111",
  "6111161113222223111161111611116111161111322222222233114441116111",
  "1611116113222223611116111161111611116111322222222222344441111611",
  "1161111613222223161111611116111161111644432222222222224446111161",
  "1116111162222223116111161111611116111144443222222222223111611116",
  "1111611112222221111611116111161111611114444322222222222311161111",
  "6111161112222226111161111611116111161114441113322222222311116111",
  "1611116132222221611116111161111611116111461111611322222261111611",
  "1161111632222231161111611116111161111611116111161322222236111161",
  "1116111132222231116111161111611116111161111611116132222231611116",
  "1111611132222231111611116111161111611116111161111632222231161111",
  "6111161132222236111161111611116111161111611116111132222221116111",
  "1611116132222231611116111161111611116111161111611112222221111611",
  "1161111632222231161111611116111161111611116111161112222223111161",
  "1116111132222231116111161111611116111161111611116113222223611116",
  "1111611132222231111611116111161111611116111161111613222223161111",
  "6111161122222236111161111611116111161111611116111163222223116111",
  "1611116122222231611116111161111611116111161111611113222223111611",
  "1161111622222231161111611116111161111611116111161113222223111161",
  "1116111122222211116111161111611116111161111611116112222223611116",
  "1111644422222261111611116114444444444116111161111632222221161111",
  "6111144422222236111161111611444444444111611116111132222231116111",
  "1611144422222231611116111161444444444111161111611322222231111611",
  "1161144422222231161111611116332222333611116111161322222216111161",
  "1116144432222231116111161133222222222331111611113222222311611116",
  "1111644432222231111611116322222222222223311161132222222111161111",
  "6111144432222236111161113222222222222222223316322222223611116111",
  "1611144435555553611116322222222222222222222222222222231161111611",
  "1161144445555552233322222222222222222222222222222222211116111161",
  "1116114443222222222222222222223313322222222222222222361111611116",
  "1111614443222222222222222222331111613222222222222223116111161111",
  "6111164444322222222222222223116111161133222222222341111611116111",
  "1611116444432222222222222231111611116111333222233444111161111611",
  "1161111644444322222222233116111161111611444433444444411116111161",
  "1116111164444411333331161111611116111161444444444444161111611116",
  "1111611116444461111611116111161111611116444444444411116111161111",
  "6111161111614416111161111611116111161111611444411161111611116111",
  "1611116111161111611116111161111611116111161111611116111161111611",
  "1161111611116111161111611116111161111611116111161111611116111161",
  "1116111161111611116111161111611116111161111611116111161111611116"
}

-- How it drives: speeds in pixels a frame at 60 frames a second, turning in turns a frame.
local TOP_SPEED = {[0] = 2.6, [1] = 1.4, [2] = 1.0}  -- on road, sand, grass (the flags)
local ACCEL, BRAKE = 0.035, 0.09
local TURN = 0.0062
local GRIP, GRIP_DRIFT = 0.16, 0.045  -- how fast the slide follows where it points: going straight, turning hard at speed
local LAPS = 3
local START_X, START_Y, START_ANGLE = 96, 428, -0.058
local CHECKPOINTS = {{96, 416}, {118, 129}, {291, 91}, {383, 223}, {420, 382}, {244, 402}}  -- in the order they're passed; the first is the line
local TREES = {{194, 306}, {316, 47}, {22, 418}, {494, 242}, {338, 47}, {380, 300}, {161, 31}, {229, 465}, {438, 63}, {479, 225}, {259, 201}, {184, 297}, {427, 492}, {429, 479}, {426, 491}, {157, 385}, {435, 37}, {311, 38}, {439, 487}, {259, 495}, {304, 31}, {36, 433}, {237, 266}, {325, 302}, {284, 314}, {223, 362}, {485, 266}, {279, 21}, {215, 294}, {26, 312}, {319, 45}, {355, 370}, {27, 45}, {191, 166}, {193, 302}, {160, 197}, {387, 29}, {402, 131}, {351, 65}, {171, 176}, {427, 97}, {178, 328}, {403, 182}, {397, 146}, {182, 216}, {217, 213}}

-- The screen: the 128 x 128 picture, and bands above and below it in the full-color layer.
local c = caps()
local W, H = c.w, c.h
local TOP, BOT = c.picture_y, c.picture_y + 384
local HORIZON = 40

local function bands(sub)
  hd.scanlines(false)
  hd.gradient(0, 0, W, TOP, 0x3a1c4a, 0x000000)
  hd.text("DRIFT", W / 2, TOP / 2 - 22, 0xffffff, 24, "bold", "center")
  hd.text(sub, W / 2, TOP / 2 + 8, 0xffc9a0, 14, "mono", "center")
  hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x3a1c4a)
  hd.text("drag or tilt to steer  ·  drag down to brake", W / 2, BOT + 22, 0x9aa4b8, 12, "mono", "center")
end

-- Times: frames, shown as m:ss.cc
local function clock(f)
  local s = f / 60
  local cs = flr(s * 100) % 100
  return flr(s / 60) .. ":" .. (flr(s) % 60 < 10 and "0" or "") .. flr(s) % 60 .. "." .. (cs < 10 and "0" or "") .. cs
end

-- The best lap's ghost: where the kart was every 4 frames, kept in the save as 4 characters a
-- sample (x and y 9 bits each, the angle 6).
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
local CH = {}
for i = 1, 64 do CH[i - 1] = B64:sub(i, i) end
local function pack1(x, y, a)  -- one sample, as it's recorded (a lap's worth at once would be a slow frame)
  local v = (mid(0, flr(x), 511) << 15) | (mid(0, flr(y), 511) << 6) | (flr(a % 1 * 64) & 63)
  return CH[v >> 18] .. CH[v >> 12 & 63] .. CH[v >> 6 & 63] .. CH[v & 63]
end
local function unpack_ghost(text)
  local out = {}
  for i = 1, #text - 3, 4 do
    local v = 0
    for k = 0, 3 do v = v << 6 | (B64:find(text:sub(i + k, i + k), 1, true) - 1) end
    out[#out + 1] = {v >> 15, v >> 6 & 511, (v & 63) / 64}
  end
  return out
end

local kart, cam_angle, best, ghost, lap_ghost, lap_packed = {}, 0, nil, nil, {}, {}
local lap, lap_frames, race_frames, next_cp, lap_times, new_best = 1, 0, 0, 2, {}, false

local function surface(x, y)
  local t = mget(flr(x / 8), flr(y / 8))
  if t == nil then return 2 end
  if fget(t, 0) then return 0 elseif fget(t, 1) then return 1 end
  return 2
end

local function reset_kart()
  kart = {x = START_X, y = START_Y, a = START_ANGLE, vx = 0, vy = 0, steer = 0, slip = 0}
  cam_angle = START_ANGLE
  lap, lap_frames, race_frames, next_cp, lap_times, new_best, lap_ghost, lap_packed = 1, 0, 0, 2, {}, false, {}, {}
end

local function drive(go)
  local steer = (btn(1) and 1 or 0) - (btn(0) and 1 or 0)
  local ax = tilt()
  if abs(ax) > 0.08 then steer = mid(-1, ax * 2.5, 1) end
  kart.steer = kart.steer + (steer - kart.steer) * 0.3
  local s, cs = -sin(kart.a), cos(kart.a)
  local fx, fy = s, -cs
  local sp = kart.vx * fx + kart.vy * fy                 -- speed along where it points
  local top = TOP_SPEED[surface(kart.x, kart.y)]
  if go and not (btn(3) or btn(5)) then sp = sp + ACCEL else sp = sp - BRAKE end
  sp = mid(0, sp, max(top, sp * 0.97))                    -- off the road it slows to that road's top speed
  kart.a = kart.a + kart.steer * TURN * min(1, sp / 1.2)
  s, cs = -sin(kart.a), cos(kart.a)
  local grip = GRIP + (GRIP_DRIFT - GRIP) * abs(kart.steer) * min(1, sp / TOP_SPEED[0])
  kart.vx = kart.vx + (s * sp - kart.vx) * grip
  kart.vy = kart.vy + (-cs * sp - kart.vy) * grip
  kart.slip = abs(kart.vx * cs + kart.vy * s)            -- sideways speed: the drift
  kart.x = mid(4, kart.x + kart.vx, 508)
  kart.y = mid(4, kart.y + kart.vy, 508)
  for _, t in ipairs(TREES) do                           -- trees stop you
    local dx, dy = kart.x - t[1], kart.y - t[2]
    if dx * dx + dy * dy < 30 then
      kart.x, kart.y = kart.x - kart.vx * 2, kart.y - kart.vy * 2
      kart.vx, kart.vy = -kart.vx * 0.3, -kart.vy * 0.3
      sfx(12)
    end
  end
  -- dust off the road, smoke from a drift (on the screen, at the back wheels)
  local surf = surface(kart.x, kart.y)
  if sp > 0.6 and (surf > 0 or kart.slip > 0.55) and rnd(1) < 0.6 then
    particles.burst(56 + rnd(16), 112, 1, {colors = surf > 0 and {4, 15, 9} or {6, 7, 13}, speed = 12, life = 0.4, gravity = -10})
  end
end

local function laps_and_checkpoints()
  lap_frames, race_frames = lap_frames + 1, race_frames + 1
  if lap_frames % 4 == 0 then
    lap_ghost[#lap_ghost + 1] = {kart.x, kart.y, kart.a}
    lap_packed[#lap_packed + 1] = pack1(kart.x, kart.y, kart.a)
  end
  local cp = CHECKPOINTS[next_cp]
  local dx, dy = kart.x - cp[1], kart.y - cp[2]
  if dx * dx + dy * dy < 48 * 48 then
    if next_cp == 1 then  -- round to the line: a lap
      lap_times[#lap_times + 1] = lap_frames
      log("lap", lap, clock(lap_frames))
      if not best or lap_frames < best then
        best, new_best = lap_frames, true
        ghost = lap_ghost
        local packed = table.concat(lap_packed)
        save({best = best, ghost = #packed < 1800 and packed or nil})
        sfx(11)
        bands("best lap " .. clock(best))
      else
        sfx(10)
      end
      lap, lap_frames, lap_ghost, lap_packed = lap + 1, 0, {}, {}
      next_cp = 2
      return lap > LAPS
    end
    next_cp = next_cp % #CHECKPOINTS + 1
  end
end

local title, countdown, race, finish = {}, {}, {}, {}  -- the scenes (state library), here so world() knows race
local mountains = {}
local function sky()
  local o = flr(cam_angle * 512)
  for x = 0, 127, 2 do  -- two pixels a column: half the calls, and mountains this far off don't need more
    local m = mountains[(x - o) % 512 + 1]
    rectfill(x, HORIZON - m, x + 1, HORIZON, m > 9 and 7 or 13)
    if m > 9 then rectfill(x, HORIZON - m + 2, x + 1, HORIZON, 13) end
  end
  circfill(flr(100 - cam_angle * 512) % 512 - 64, 14, 6, 10)
end

-- The world: sky, floor, trees and the ghost farthest first, then the kart.
local function world(t)
  cls(12)
  local s, cs = -sin(cam_angle), cos(cam_angle)
  mode7.angle = cam_angle
  mode7.x, mode7.y = kart.x - s * 28, kart.y + cs * 28
  mode7.height, mode7.horizon = 18, HORIZON
  sky()
  mode7.floor()
  -- heat shimmer just over the floor's far edge
  for y = HORIZON - 2, HORIZON + 6 do rowshift(y, flr(sin(t / 50 + y / 6) * 1.5)) end
  local list = {}
  local kx, ky = kart.x, kart.y
  for _, tr in ipairs(TREES) do
    local dx, dy = tr[1] - kx, tr[2] - ky
    if dx * dx + dy * dy < 360 * 360 then  -- (the far ones aren't worth a sprite)
      local _, _, _, d = mode7.project(tr[1], tr[2])
      if d then list[#list + 1] = {d = d, x = tr[1], y = tr[2]} end
    end
  end
  if ghost and state.is(race) then
    local g = ghost[min(#ghost, flr(lap_frames / 4) + 1)]
    local _, _, _, d = mode7.project(g[1], g[2])
    if d then list[#list + 1] = {d = d, x = g[1], y = g[2], ghost = true} end
  end
  table.sort(list, function(a, b) return a.d > b.d end)
  for _, o in ipairs(list) do
    if o.ghost then  -- the best lap's kart, in ghostly colors
      pal(8, 13) pal(9, 7) pal(12, 7) pal(5, 1) pal(1, 1)
      mode7.sprite(32, o.x, o.y, 7, 2, 1)
      pal()
    else
      mode7.sprite(16, o.x, o.y, 22)
    end
  end
  local lean = kart.steer < -0.35 and 34 or kart.steer > 0.35 and 36 or 32
  sspr(lean, 48, 100 + (kart.slip > 0.55 and t % 2 or 0), 32, 16, false, false, 2, 1)
  particles.draw()
end

function title.enter()
  bands(best and ("best lap " .. clock(best)) or "three laps against the clock")
  music(-1)
  reset_kart()
end
function title.update()
  if btnp(4) or btnp(5) then state.go(countdown) end
end
function title.draw()
  cls(1)
  mode7.turned(256, 256, time() / 40, 0.22 + sin(time() / 16) * 0.03)
  rectfill(28, 20, 99, 34, 1)
  print("DRIFT", 54, 25, 10)
  if time() % 1 < 0.6 then print("tap to start", 40, 100, 7) end
  if best then print("best lap " .. clock(best), 34, 110, 15) end
end

function countdown.enter() sfx(8) end
function countdown.update()
  local t = state.time()
  if flr(t) ~= flr(t - 1 / 60) and t < 3 then sfx(8) end
  if t >= 3 then sfx(9) music(0) state.go(race) end
end
function countdown.draw()
  world(flr(state.time() * 60))
  local n = 3 - flr(state.time())
  print(n, 62, 50, 7)
end

function race.enter() bands(best and ("best lap " .. clock(best)) or "lap 1 of " .. LAPS) end
local frame = 0
function race.update()
  frame = frame + 1
  drive(true)
  particles.update()
  if laps_and_checkpoints() then state.go(finish) end
end
function race.draw()
  world(frame)
  print("lap " .. min(lap, LAPS) .. "/" .. LAPS, 2, 2, 7)
  print(clock(lap_frames), 128 - #clock(lap_frames) * 4 - 2, 2, 7)
  if best then print("best " .. clock(best), 128 - #("best " .. clock(best)) * 4 - 2, 9, 15) end
  if ghost then print("ghost", 2, 9, 12) end
end

function finish.enter()
  music(-1)
  local total = 0
  for _, f in ipairs(lap_times) do total = total + f end
  finish.total = total
  bands("finished in " .. clock(total))
end
function finish.update()
  frame = frame + 1
  drive(false)
  particles.update()
  if state.time() > 1.5 and (btnp(4) or btnp(5)) then state.go(title) end
end
function finish.draw()
  world(frame)
  rectfill(20, 46, 107, 84, 1)
  print("finished " .. clock(finish.total), 32, 50, 10)
  for i, f in ipairs(lap_times) do print("lap " .. i .. "  " .. clock(f), 38, 58 + i * 7, f == best and 11 or 7) end
end

function _init()
  -- the sky: color 12 a different shade each row, dusk at the top to the sunset at the horizon
  local function mix(a, b, t)
    local function ch(sh) return flr(((a >> sh) & 255) * (1 - t) + ((b >> sh) & 255) * t) end
    return ch(16) << 16 | ch(8) << 8 | ch(0)
  end
  for y = 0, HORIZON do rowpal(y, 12, mix(0x2a1858, 0xff9a5c, y / HORIZON)) end
  -- and the lake past the shore (off the map, which tline leaves): the sunset on the water, darker nearer
  for y = HORIZON + 1, 96 do rowpal(y, 12, mix(0xffb080, 0x1d3a7a, min(1, (y - HORIZON) / 24))) end
  for x = 1, 512 do
    local a = (x - 1) / 512
    mountains[x] = flr(7 + sin(a * 3) * 4 + sin(a * 7 + 0.3) * 3 + sin(a * 13) * 1.5)
  end
  local saved = load()
  if saved and saved.best then
    best = saved.best
    if saved.ghost then ghost = unpack_ghost(saved.ghost) end
  end
  state.go(title)
end
function _update60() state.update() end
function _draw() state.draw() end
