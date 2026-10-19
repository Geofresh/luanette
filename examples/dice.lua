-- title: Dice
-- by: geo
-- about: tap a die type to add it, shake the board or tap the felt to throw; X clears

-- Rigid bodies on felt seen from above (z towards you). A corner in the floor or a wall gets an
-- impulse there (bounce, friction): dice tumble and settle on a face. Gravity follows the tilt.
-- Drawn in 3D: each die a lit object with its shadow on the felt (the background), seen from above
-- with perspective; numbers and the tray's bars on the picture over it.

local PHI = (1 + math.sqrt(5)) / 2
local W, TOP, BOT = 64, 12, 118 -- the tray
local CX, CY = 64, (TOP + BOT) / 2
local HY = (BOT - TOP) / 2
-- Gravity: a die here is ~36 px for ~16 mm (~2 px a mm), so real gravity would be ~19,600 px/s^2;
-- 900 was a twentieth of it (dice drifted down as if under water). 4200 lands and bounces briskly
-- and still reads at 60 fps; throws, caps and the rest test scale with it.
local G, DT = 4200, 1 / 120 -- px/s^2; two steps a frame
local REST_V2 = (G * DT * 1.6) ^ 2 -- a resting die still jitters by a step's gravity; under this, it's still
local FELT_E, FELT_MU, ROLL_DRAG = 0.6, 0.3, 0.9995 -- the felt's bounce and grip; a rolling die's spin kept a step
local FC = {} -- a step's corners in the felt (reused)
local felt_contacts -- (below)
local TYPES = {4, 6, 8, 10, 12, 20}
local COLS = {
  [4] = {4, 9, 10}, [6] = {5, 6, 7}, [8] = {1, 12, 6}, [10] = {3, 11, 10}, [12] = {2, 13, 14}, [20] = {2, 8, 14}}
local RGB = {[4] = 0xF08A24, [6] = 0xF2EEE4, [8] = 0x3A7BEA, [10] = 0x2FB36A, [12] = 0x9A5BE0, [20] = 0xE0384A}
local LABEL = {[4] = 7, [6] = 1, [8] = 7, [10] = 7, [12] = 7, [20] = 7} -- the numbers' colour on each
local MESH, POOL = {}, {} -- a 3D shape for each kind, and objects of dice taken away (reused)
local OY = CY - 64 -- the tray's centre below the screen's
local drop_all -- (below)
local CAM_H, CAM_Z, CAM_PITCH, CAM_FOV = 245, -110, -66, 29 -- fitted: the far wall under the top bar, the near one over the bottom bar
local SHAPES = {}
local dice, total, msg, flash, flashc = {}, nil, "SHAKE OR TAP TO THROW", 0, 7
local last_tx, last_ty, ptx, pty = nil, nil, 0, 0

local function norm(x, y, z)
  local l = math.sqrt(x * x + y * y + z * z)
  return x / l, y / l, z / l
end

-- A die's polyhedron: corners scaled to radius 1, faces wound outwards with normal, centre and number.
local function poly(verts, faces)
  local r = 0
  for _, v in ipairs(verts) do r = math.max(r, math.sqrt(v[1] ^ 2 + v[2] ^ 2 + v[3] ^ 2)) end
  for _, v in ipairs(verts) do v[1], v[2], v[3] = v[1] / r, v[2] / r, v[3] / r end
  local fs = {}
  for i, f in ipairs(faces) do
    local cx, cy, cz = 0, 0, 0
    for _, k in ipairs(f) do cx, cy, cz = cx + verts[k][1], cy + verts[k][2], cz + verts[k][3] end
    cx, cy, cz = cx / #f, cy / #f, cz / #f
    local a, b, c = verts[f[1]], verts[f[2]], verts[f[3]]
    local ux, uy, uz = b[1] - a[1], b[2] - a[2], b[3] - a[3]
    local vx, vy, vz = c[1] - a[1], c[2] - a[2], c[3] - a[3]
    local nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    if nx * cx + ny * cy + nz * cz < 0 then -- wound inwards: turn it round
      local g = {}
      for k = #f, 1, -1 do g[#g + 1] = f[k] end
      f = g
      nx, ny, nz = -nx, -ny, -nz
    end
    nx, ny, nz = norm(nx, ny, nz)
    fs[i] = {v = f, nx = nx, ny = ny, nz = nz, cx = cx, cy = cy, cz = cz, n = i}
  end
  return {v = verts, f = fs}
end

-- Faces whose centres point along `dirs`: the corners nearest each, ordered round it.
local function faces_by_dirs(verts, dirs, per)
  local out = {}
  for _, d in ipairs(dirs) do
    local dx, dy, dz = norm(d[1], d[2], d[3])
    local idx = {}
    for i, v in ipairs(verts) do idx[#idx + 1] = {i, v[1] * dx + v[2] * dy + v[3] * dz} end
    table.sort(idx, function(a, b) return a[2] > b[2] end)
    local f = {}
    for k = 1, per do f[k] = idx[k][1] end
    local ax, ay, az = norm(dy, -dx, 0.0001)
    local bx, by, bz = dy * az - dz * ay, dz * ax - dx * az, dx * ay - dy * ax
    table.sort(f, function(p, q)
      local vp, vq = verts[p], verts[q]
      return math.atan(vp[1] * bx + vp[2] * by + vp[3] * bz, vp[1] * ax + vp[2] * ay + vp[3] * az)
           < math.atan(vq[1] * bx + vq[2] * by + vq[3] * bz, vq[1] * ax + vq[2] * ay + vq[3] * az)
    end)
    out[#out + 1] = f
  end
  return out
end

local function build_shapes()
  SHAPES[4] = poly({{1, 1, 1}, {1, -1, -1}, {-1, 1, -1}, {-1, -1, 1}}, {{1, 2, 3}, {1, 2, 4}, {1, 3, 4}, {2, 3, 4}})
  local function cubev()
    local t = {}
    for i = 0, 7 do t[#t + 1] = {(i & 1) * 2 - 1, (i >> 1 & 1) * 2 - 1, (i >> 2 & 1) * 2 - 1} end
    return t
  end
  SHAPES[6] = poly(cubev(), {{1, 2, 4, 3}, {5, 6, 8, 7}, {1, 2, 6, 5}, {3, 4, 8, 7}, {1, 3, 7, 5}, {2, 4, 8, 6}})
  local oct = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}}
  local of = {}
  for _, a in ipairs({1, 2}) do for _, b in ipairs({3, 4}) do for _, c in ipairs({5, 6}) do of[#of + 1] = {a, b, c} end end end
  SHAPES[8] = poly(oct, of)
  -- d10: a pentagonal trapezohedron (poles, a ring alternating up and down)
  local z0 = math.tan(math.pi / 10) ^ 2
  local tz = {{0, 0, 1}, {0, 0, -1}}
  for k = 0, 9 do tz[#tz + 1] = {math.cos(k * math.pi / 5), math.sin(k * math.pi / 5), k % 2 == 0 and z0 or -z0} end
  local tf = {}
  for k = 0, 4 do
    local u0, l0, u1, l1 = 3 + 2 * k, 4 + 2 * k, 3 + (2 * k + 2) % 10, 3 + (2 * k + 3) % 10
    tf[#tf + 1] = {1, u0, l0, u1}
    tf[#tf + 1] = {2, l0, u1, l1}
  end
  SHAPES[10] = poly(tz, tf)
  local ip = 1 / PHI
  local dod = cubev()
  for _, s1 in ipairs({-1, 1}) do
    for _, s2 in ipairs({-1, 1}) do
      dod[#dod + 1] = {0, s1 * ip, s2 * PHI}
      dod[#dod + 1] = {s1 * ip, s2 * PHI, 0}
      dod[#dod + 1] = {s1 * PHI, 0, s2 * ip}
    end
  end
  local function icov(a, b) -- an icosahedron's corners; (PHI, 1): the dodecahedron's face directions
    local t = {}
    for _, s1 in ipairs({-1, 1}) do
      for _, s2 in ipairs({-1, 1}) do
        t[#t + 1] = {0, s1 * a, s2 * b}
        t[#t + 1] = {s1 * a, s2 * b, 0}
        t[#t + 1] = {s2 * b, 0, s1 * a}
      end
    end
    return t
  end
  SHAPES[12] = poly(dod, faces_by_dirs(dod, icov(PHI, 1), 5))
  local ico = icov(1, PHI)
  local itri = {}
  for i = 1, 12 do
    for j = i + 1, 12 do
      for k = j + 1, 12 do
        local function d(a, b) return (ico[a][1] - ico[b][1]) ^ 2 + (ico[a][2] - ico[b][2]) ^ 2 + (ico[a][3] - ico[b][3]) ^ 2 end
        if d(i, j) < 4.1 and d(j, k) < 4.1 and d(i, k) < 4.1 then itri[#itri + 1] = {i, j, k} end
      end
    end
  end
  SHAPES[20] = poly(ico, itri)
  -- d6: opposite faces add to 7
  local sixes = {1, 6, 2, 5, 3, 4}
  for i, f in ipairs(SHAPES[6].f) do f.n = sixes[i] end
end

local function size_of(n) return n == 4 and 20 or n == 6 and 16 or n == 20 and 18 or 17 end

local function new_die(n)
  local d = {n = n, s = SHAPES[n], r = size_of(n), x = 0, y = 0, z = 30, vx = 0, vy = 0, vz = 0,
             wx = 0, wy = 0, wz = 0, R = {1, 0, 0, 0, 1, 0, 0, 0, 1}, still = 0, value = nil, wv = {}}
  d.x, d.y = (rnd(2) - 1) * (W - 20), (rnd(2) - 1) * (HY - 20)
  return d
end

local function throw(power)
  for _, d in ipairs(dice) do
    d.vx, d.vy = (rnd(2) - 1) * 650 * power, (rnd(2) - 1) * 650 * power
    d.vz = 340 + rnd(200) * power
    d.wx, d.wy, d.wz = (rnd(2) - 1) * 80 * power, (rnd(2) - 1) * 80 * power, (rnd(2) - 1) * 40 * power
    d.still, d.value = 0, nil
  end
  total, msg = nil, "ROLLING..."
end

-- An impulse at point p against a surface of normal n: bounce e, friction mu.
local function contact(d, px, py, pz, nx, ny, nz, depth, e, mu)
  local rx, ry, rz = px - d.x, py - d.y, pz - d.z
  local inv_i = 1 / (0.4 * d.r * d.r)
  local cvx = d.vx + d.wy * rz - d.wz * ry
  local cvy = d.vy + d.wz * rx - d.wx * rz
  local cvz = d.vz + d.wx * ry - d.wy * rx
  local vn = cvx * nx + cvy * ny + cvz * nz
  d.x, d.y, d.z = d.x + nx * depth * 0.5, d.y + ny * depth * 0.5, d.z + nz * depth * 0.5
  if vn >= 0 then return end
  -- A resting contact (closing slower than a few steps of gravity) doesn't bounce: corners resolved
  -- one at a time would otherwise kick a die lying flat into a shiver.
  if vn > -G * DT * 3 then e = 0 end
  local qx, qy, qz = ry * nz - rz * ny, rz * nx - rx * nz, rx * ny - ry * nx
  local j = -(1 + e) * vn / (1 + (qx * qx + qy * qy + qz * qz) * inv_i)
  d.vx, d.vy, d.vz = d.vx + j * nx, d.vy + j * ny, d.vz + j * nz
  d.wx, d.wy, d.wz = d.wx + inv_i * j * qx, d.wy + inv_i * j * qy, d.wz + inv_i * j * qz
  local tx, ty, tz = cvx - vn * nx, cvy - vn * ny, cvz - vn * nz
  local tl = math.sqrt(tx * tx + ty * ty + tz * tz)
  if tl > 1e-4 then
    tx, ty, tz = tx / tl, ty / tl, tz / tl
    local sx, sy, sz = ry * tz - rz * ty, rz * tx - rx * tz, rx * ty - ry * tx
    local jt = math.min(mu * j, tl / (1 + (sx * sx + sy * sy + sz * sz) * inv_i))
    d.vx, d.vy, d.vz = d.vx - jt * tx, d.vy - jt * ty, d.vz - jt * tz
    d.wx, d.wy, d.wz = d.wx - inv_i * jt * sx, d.wy - inv_i * jt * sy, d.wz - inv_i * jt * sz
  end
end

-- The corners in the felt, solved together: a few passes over them, each corner's push (up, never
-- down) and friction (at most FELT_MU of its push) accumulated and clamped, so they settle on what
-- they must jointly do. One at a time, each corner's push and bounce disturbed the others: a die
-- flat on three corners was kicked round forever, and a bounce strong enough to tumble couldn't be
-- had without dice that never stopped. A corner bounces (FELT_E) off its own speed in; a slow one,
-- resting, doesn't. (The felt's normal is +z, so the axes are the tangents.)
felt_contacts = function(d, fc, n)
  local inv_i = 1 / (0.4 * d.r * d.r)
  for k = 1, n do
    local c = fc[k]
    local rx, ry = c[1], c[2]
    local vn = d.vz + d.wx * ry - d.wy * rx
    c[4] = vn < -G * DT * 3 and -FELT_E * vn or 0 -- its target speed out
    c[5], c[6], c[7] = 0, 0, 0 -- push, friction x, friction y so far
  end
  for _ = 1, 2 do -- two passes: three were steadier by nothing measured, and cost six dice the budget
    for k = 1, n do
      local c = fc[k]
      local rx, ry, rz = c[1], c[2], c[3]
      local vn = d.vz + d.wx * ry - d.wy * rx
      local jn = math.max(0, c[5] + (c[4] - vn) / (1 + (rx * rx + ry * ry) * inv_i))
      local dj = jn - c[5]
      c[5] = jn
      d.vz = d.vz + dj
      d.wx, d.wy = d.wx + inv_i * ry * dj, d.wy - inv_i * rx * dj
      local lim = FELT_MU * jn
      local vx = d.vx + d.wy * rz - d.wz * ry
      local jx = math.max(-lim, math.min(lim, c[6] - vx / (1 + (rz * rz + ry * ry) * inv_i)))
      local djx = jx - c[6]
      c[6] = jx
      local vy = d.vy + d.wz * rx - d.wx * rz
      local jy = math.max(-lim, math.min(lim, c[7] - vy / (1 + (rz * rz + rx * rx) * inv_i)))
      local djy = jy - c[7]
      c[7] = jy
      d.vx, d.vy = d.vx + djx, d.vy + djy
      -- w += inv_i * (r x (djx, djy, 0))
      d.wx, d.wy, d.wz = d.wx - inv_i * rz * djy, d.wy + inv_i * rz * djx, d.wz + inv_i * (rx * djy - ry * djx)
    end
  end
end

local function step(d, gx, gy, gz)
  d.vx, d.vy, d.vz = d.vx + gx * DT, d.vy + gy * DT, d.vz + gz * DT
  d.x, d.y, d.z = d.x + d.vx * DT, d.y + d.vy * DT, d.z + d.vz * DT
  -- R += [w]x R dt, then Gram-Schmidt
  local R, wx, wy, wz = d.R, d.wx * DT, d.wy * DT, d.wz * DT
  for c = 1, 3 do
    local a, b, e = R[c], R[c + 3], R[c + 6]
    R[c], R[c + 3], R[c + 6] = a + wy * e - wz * b, b + wz * a - wx * e, e + wx * b - wy * a
  end
  local ax, ay, az = norm(R[1], R[4], R[7])
  local bx, by, bz = R[2], R[5], R[8]
  local k = ax * bx + ay * by + az * bz
  bx, by, bz = norm(bx - k * ax, by - k * ay, bz - k * az)
  R[1], R[4], R[7], R[2], R[5], R[8] = ax, ay, az, bx, by, bz
  R[3], R[6], R[9] = ay * bz - az * by, az * bx - ax * bz, ax * by - ay * bx
  local wv, s, r = d.wv, d.s.v, d.r
  -- Corners in the felt this step, solved together (felt_contacts).
  local fc, nfc, deep = FC, 0, 0
  for i = 1, #s do
    local v = s[i]
    local px = d.x + (R[1] * v[1] + R[2] * v[2] + R[3] * v[3]) * r
    local py = d.y + (R[4] * v[1] + R[5] * v[2] + R[6] * v[3]) * r
    local pz = d.z + (R[7] * v[1] + R[8] * v[2] + R[9] * v[3]) * r
    -- the felt grips; walls and the lid barely (a spinning die mustn't climb a wall)
    if pz < 0 then
      nfc = nfc + 1
      local c = fc[nfc]
      if not c then c = {} fc[nfc] = c end
      c[1], c[2], c[3] = px - d.x, py - d.y, pz - d.z -- from the centre
      if -pz > deep then deep = -pz end
    end
    if pz > 70 then contact(d, px, py, pz, 0, 0, -1, pz - 70, 0.3, 0.05) end
    if px < -W + 2 then contact(d, px, py, pz, 1, 0, 0, -W + 2 - px, 0.5, 0.08) end
    if px > W - 2 then contact(d, px, py, pz, -1, 0, 0, px - W + 2, 0.5, 0.08) end
    if py < -HY + 1 then contact(d, px, py, pz, 0, 1, 0, -HY + 1 - py, 0.5, 0.08) end
    if py > HY - 1 then contact(d, px, py, pz, 0, -1, 0, py - HY + 1, 0.5, 0.08) end
  end
  local touching = nfc > 0
  if touching then
    d.z = d.z + deep
    felt_contacts(d, fc, nfc)
  end
  local wd = touching and ROLL_DRAG or 0.999 -- rolling resistance, air
  -- nearly stopped on the felt, barely turning: settle (a real die doesn't keep wobbling). Only then:
  -- a rolling die's centre moves slowly while it still tumbles
  if touching and d.vx * d.vx + d.vy * d.vy + d.vz * d.vz < REST_V2 * 4
     and d.wx * d.wx + d.wy * d.wy + d.wz * d.wz < 4 then wd = 0.9 end
  d.wx, d.wy, d.wz = d.wx * wd, d.wy * wd, d.wz * wd
  if touching then d.vx, d.vy = d.vx * 0.998, d.vy * 0.998 end
  -- speed limits (a numerical slip mustn't snowball)
  local sp = d.vx * d.vx + d.vy * d.vy + d.vz * d.vz
  if sp > 1960000 then
    local f = 1400 / math.sqrt(sp)
    d.vx, d.vy, d.vz = d.vx * f, d.vy * f, d.vz * f
  end
  local spin = d.wx * d.wx + d.wy * d.wy + d.wz * d.wz
  if spin > 3600 then
    local f = 60 / math.sqrt(spin)
    d.wx, d.wy, d.wz = d.wx * f, d.wy * f, d.wz * f
  end
  return touching
end

local function collide(a, b) -- as spheres
  local dx, dy, dz = b.x - a.x, b.y - a.y, b.z - a.z
  local dist = math.sqrt(dx * dx + dy * dy + dz * dz)
  local min = (a.r + b.r) * 0.82
  if dist >= min or dist < 1e-3 then return end
  local nx, ny, nz = dx / dist, dy / dist, dz / dist
  local push = (min - dist) / 2
  a.x, a.y, a.z = a.x - nx * push, a.y - ny * push, a.z - nz * push
  b.x, b.y, b.z = b.x + nx * push, b.y + ny * push, b.z + nz * push
  local vn = (b.vx - a.vx) * nx + (b.vy - a.vy) * ny + (b.vz - a.vz) * nz
  if vn < 0 then
    local j = -1.6 * vn / 2
    a.vx, a.vy, a.vz = a.vx - j * nx, a.vy - j * ny, a.vz - j * nz
    b.vx, b.vy, b.vz = b.vx + j * nx, b.vy + j * ny, b.vz + j * nz
    a.wz, b.wz = a.wz + (rnd(2) - 1) * 4, b.wz + (rnd(2) - 1) * 4
  end
end

-- The face up is the roll; a d4's is the face down, shown at its top corner as real d4s do.
local function read(d)
  local R, best, bv = d.R, nil, nil
  for _, f in ipairs(d.s.f) do
    local nz = R[7] * f.nx + R[8] * f.ny + R[9] * f.nz
    local score = d.n == 4 and -nz or nz
    if not bv or score > bv then best, bv = f, score end
  end
  return best.n
end

function _init()
  build_shapes()
  local s = load()
  for _, n in ipairs(s and s.set or {20}) do dice[#dice + 1] = new_die(n) end
end

function _update60()
  local ax, ay = tilt()
  -- a shake: the tilt jumping faster than any tipping
  local jx, jy = ax - ptx, ay - pty
  ptx, pty = ax, ay
  local shake = math.sqrt(jx * jx + jy * jy)
  if shake > 0.22 and #dice > 0 then
    for _, d in ipairs(dice) do
      d.vx, d.vy = d.vx + jx * 1800 + (rnd(2) - 1) * 160, d.vy + jy * 1800 + (rnd(2) - 1) * 160
      d.vz = math.max(d.vz, 260 + rnd(220))
      d.wx, d.wy, d.wz = d.wx + (rnd(2) - 1) * 60, d.wy + (rnd(2) - 1) * 60, d.wz + (rnd(2) - 1) * 30
      d.still, d.value = 0, nil
    end
    total, msg = nil, "ROLLING..."
  end
  local tx, ty = touch()
  if tx then last_tx, last_ty = tx, ty end
  if btnp(4) and last_ty then
    if last_ty < TOP then
      local slot = flr(last_tx / (128 / 7)) + 1
      if slot == 7 then
        drop_all()
        total, msg = nil, "TAP A DIE TO ADD IT"
      elseif #dice < 6 then
        dice[#dice + 1] = new_die(TYPES[slot])
        throw(0.7)
      end
      local set = {}
      for _, d in ipairs(dice) do set[#set + 1] = d.n end
      save({set = set})
    elseif #dice > 0 then
      throw(1)
    end
  end
  -- the tip, capped near 30 degrees (a tray, not a cliff)
  local tl = math.sqrt(ax * ax + ay * ay)
  if tl > 0.5 then ax, ay = ax * 0.5 / tl, ay * 0.5 / tl end
  local gz = -math.sqrt(1 - ax * ax - ay * ay) * G
  local gx, gy = ax * G, ay * G
  for _ = 1, 2 do
    for i = 1, #dice do
      local d = dice[i]
      if d.still < 30 then d.touch = step(d, gx, gy, gz) end
    end
    for i = 1, #dice do for j = i + 1, #dice do collide(dice[i], dice[j]) end end
  end
  -- settled dice are read; a strong tip wakes them
  local all, sum = #dice > 0, 0
  local tipped = math.sqrt(ax * ax + ay * ay) > 0.42
  for _, d in ipairs(dice) do
    local sp = d.vx * d.vx + d.vy * d.vy + d.vz * d.vz
    local spin = d.wx * d.wx + d.wy * d.wy + d.wz * d.wz
    if d.touch and sp < REST_V2 and spin < 1.0 and not tipped then d.still = d.still + 1 else d.still = math.min(d.still, 29) end
    if tipped and d.still >= 30 then d.still = 0 end
    if d.still >= 30 then
      if not d.value then
        d.value = read(d)
        d.vx, d.vy, d.vz, d.wx, d.wy, d.wz = 0, 0, 0, 0, 0, 0
      end
      sum = sum + d.value
    else
      all = false
    end
  end
  if all and not total then
    total = sum
    local one = #dice == 1 and dice[1].n == 20
    if one and dice[1].value == 20 then flash, flashc, msg = 40, 10, "NATURAL 20!"
    elseif one and dice[1].value == 1 then flash, flashc, msg = 40, 8, "NATURAL 1..."
    else msg = nil end
  end
  if flash > 0 then flash = flash - 1 end
end

-- The dice's world (x right, y down the screen, z up off the felt) in the stage's (y up, the camera
-- above looking down: x right, -z down the screen). A proper rotation (no mirror).
local function to3(x, y, z) return x, z, -(y + OY) end

local function mesh_of(n)
  if not MESH[n] then
    local s, r, xyz, tri = SHAPES[n], size_of(n), {}, {}
    for _, v in ipairs(s.v) do
      xyz[#xyz + 1], xyz[#xyz + 2], xyz[#xyz + 3] = v[1] * r, v[3] * r, -v[2] * r
    end
    for _, f in ipairs(s.f) do
      for t = 2, #f.v - 1 do tri[#tri + 1], tri[#tri + 2], tri[#tri + 3] = f.v[1], f.v[t], f.v[t + 1] end
    end
    MESH[n] = shape(xyz, tri)
  end
  return MESH[n]
end

local function show(d)
  if not d.o then
    local pool = POOL[d.n]
    d.o = pool and table.remove(pool)
    if d.o then hide(d.o, false) else d.o = obj(mesh_of(d.n), RGB[d.n], 70) shadow(d.o) end
  end
  local R = d.R
  orient(d.o, R[1], R[3], -R[2], R[7], R[9], -R[8], -R[4], -R[6], R[5])
  place(d.o, to3(d.x, d.y, d.z))
end

-- Dice taken away: their objects hidden, kept for the next of their kind.
drop_all = function()
  for _, d in ipairs(dice) do
    if d.o then
      hide(d.o)
      POOL[d.n] = POOL[d.n] or {}
      table.insert(POOL[d.n], d.o)
    end
  end
  dice = {}
end

-- A point of a die (its own coordinates, times its radius) on the picture.
local function at(d, x, y, z)
  local R, r = d.R, d.r
  return project(to3(d.x + (R[1] * x + R[2] * y + R[3] * z) * r, d.y + (R[4] * x + R[5] * y + R[6] * z) * r,
                     d.z + (R[7] * x + R[8] * y + R[9] * z) * r))
end

-- Numbers on the faces turned up; the one facing you is the roll (bold, at rest just that one).
local function label_die(d)
  local R, s = d.R, d.s
  if d.n == 4 then -- its roll once, at the peak, outlined to read over all three faces
    local top, tz = 1, -9
    for i, v in ipairs(s.v) do
      local z = R[7] * v[1] + R[8] * v[2] + R[9] * v[3]
      if z > tz then top, tz = i, z end
    end
    local v = s.v[top]
    local px, py = at(d, v[1], v[2], v[3])
    if px then
      local label = tostring(d.value or read(d))
      local lx, ly = px - #label * 2 + 1, py - 2
      for oy = -1, 1 do for ox = -1, 1 do print(label, lx + ox, ly + oy, 4) end end
      print(label, lx, ly, 7)
    end
    return
  end
  for _, f in ipairs(s.f) do
    local nz = R[7] * f.nx + R[8] * f.ny + R[9] * f.nz
    if nz > 0.82 then
      local px, py = at(d, f.cx, f.cy, f.cz)
      if px then
        local label = tostring(f.n)
        if (f.n == 6 or f.n == 9) and d.n > 6 then label = label .. "." end
        local lx, ly = px - #label * 2 + 1, py - 2
        local c = LABEL[d.n]
        if nz > 0.93 then
          if c == 7 then print(label, lx + 1, ly + 1, COLS[d.n][1]) end
          print(label, lx, ly, c)
        else
          print(label, lx, ly, c == 7 and 6 or 5)
        end
      end
    end
  end
end

-- The tray in 3D: wooden walls where the physics has them, on the felt; the camera above its near
-- edge, looking down at 66 degrees (the dice show their sides).
local scene = false
local function set_scene()
  -- The felt is the background (a fill costs nothing; a felt the size of the window, drawn and
  -- depth-tested pixel by pixel, cost 7 ms a frame): darker towards you, as under a lamp.
  sky(0x2A8A50, 0x1A6038)
  ground(0, 0x10462A)
  light(125, 55, 255, 0x4A5A52)
  -- walls whose inner faces are where the physics' are (x: W - 2, y: HY - 1)
  local wood, wall, end_ = 0x7A4A2A, shape("box", 6, 10, 2 * HY + 10), shape("box", 2 * W + 8, 10, 6)
  for _, sx in ipairs({-1, 1}) do
    local o = obj(wall, wood, 30)
    place(o, to3(sx * (W + 1), 0, 5))
    shadow(o)
    o = obj(end_, wood, 30)
    place(o, to3(0, sx * (HY + 2), 5))
    shadow(o)
  end
  view(0, CAM_H, CAM_Z, 0, CAM_PITCH, CAM_FOV)
  scene = true
end

function _draw()
  if not scene then set_scene() end
  cls(0) -- the tray and the dice show through colour 0
  rectfill(0, 0, 127, TOP - 2, 1)
  for i, n in ipairs(TYPES) do
    local x = (i - 1) * (128 / 7)
    print("D" .. n, x + 9 - #tostring(n) * 2, 3, COLS[n][3])
  end
  print("X", 6 * (128 / 7) + 8, 3, 8)
  for _, d in ipairs(dice) do show(d) end
  clip(0, TOP, 128, BOT + 2 - TOP)
  for _, d in ipairs(dice) do label_die(d) end
  clip()
  rectfill(0, BOT + 2, 127, 127, 1)
  if total then
    local parts = {}
    for _, d in ipairs(dice) do parts[#parts + 1] = tostring(d.value) end
    local line = #dice > 1 and (table.concat(parts, "+") .. " = " .. total) or ("ROLLED " .. total)
    print(line, 64 - #line * 2, BOT + 4, flash > 0 and (flr(flash / 4) % 2 == 0 and flashc or 7) or 7)
  end
  if msg and (not total or flash > 0) then print(msg, 64 - #msg * 2, total and 50 or BOT + 4, flash > 0 and flashc or 6) end
end
