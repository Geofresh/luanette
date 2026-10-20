-- title: Lock & Load
-- by: geo
-- about: a light-gun rail shooter in 3 stages: tap to shoot, tap the magazine to reload
-- api: 2
-- canvas: full

-- A light-gun rail shooter. The camera rides a rail through a lit 3D scene and stops for each
-- shootout; enemies step out and a lock-on ring closes on each one: shoot before it does.
-- Built the docs/DESIGN.md way:
-- - The scene is the 3D stage (both cores, each band drawn just ahead of the panel's scan), so the
--   street costs no Lua drawing at all. Filling pixels is the stage's real cost (measured: the span
--   loop is most of both cores), so the ground is the sky's gradient carried below the horizon
--   (free: the band's clear) with 7 thin stripes sliding past as you walk and a far strip for the
--   horizon, camera-facing so none ever crosses the near plane and drops out.
-- - The HUD (score, lives, the magazine) is on the full-color layer in the glass's bands above and
--   below the scene, repainted only when what it shows changes (ui.changed). Cards and tags over
--   the scene are drawn once and cleared once; the layer blends them over each frame's scene.
-- - What moves every frame (lock-on rings, sparks, score pops, the hit flash) is the 128x128
--   picture over the scene: cheap to redraw, color 0 shows the scene through.
-- - Every motion is by time; nothing is allocated per frame; menus run at 30 fps, play at 60.

local sin, cos, rad, deg, atan, sqrt, floor = math.sin, math.cos, math.rad, math.deg, math.atan, math.sqrt, math.floor
local min, max = math.min, math.max
local unpack = table.unpack
local c = caps()
local W, H = c.w, c.h
local PX, PY = c.picture_x or 0, c.picture_y or 0
local S0, S1 = PY, PY + 384                     -- the scene's rows on the glass
local BG, INK, SOFT, AMBER, RED, GOLD = 0x0a0c12, 0xf2f4f8, 0x8d97ab, 0xffb547, 0xff3b5c, 0xffd34a
local F = 64 / math.tan(rad(30))               -- the camera's focal length in picture pixels (fov 60)
local EYE, SPEED = 150, 430                    -- eye height; walking speed (units a second)
local AUTO = nil                               -- tests/lockload_auto.lua sets it: the cart plays itself

-- ---- shapes (made once) ----

local SH = {
  bA = {"box",320,760,480},bB = {"box",300,520,380},bW = {"box",1500,900,300},
  car = {"box",190,90,420},cab = {"box",170,70,220},crate = {"box",110},
  cont = {"box",250,260,620},leg = {"box",40,900,40},beam = {"box",40,40,1000},
  ware = {"box",1600,600,200},ac = {"box",160,110,200},hut = {"box",260,260,260},
  tank = {"cylinder",90,160,10},roof = {"cone",100,70,10},neon = {"box",260,36,10},
  tower = {"box",360,1600,360},
  legs = {"box",52,96,30},torso = {"box",70,80,40},head = {"sphere",19,6},gun = {"box",12,12,54},
  drone = {"sphere",30,6},rotor = {"torus",46,6,10},pick = {"box",46},orb = {"sphere",18,6},
  boom = {"sphere",90,8},boom2 = {"sphere",45,6},barrel = {"cylinder",38,100,10},
  hull = {"box",380,130,170},cock = {"sphere",70,10},tail = {"box",50,50,320},
  blade = {"box",700,6,40},pod = {"cylinder",26,140,8},
}
local S = {}
local function half(k)                          -- a shape's half height: a prop's center above the ground
  local s = SH[k]
  if s[1] == "box" then return (s[3] or s[2]) / 2 end
  return s[1] == "sphere" and s[2] or s[3] / 2
end

-- ---- the stages ----
-- props: {shape, color, x, z, [y], [turn]}. path: {x, z, yaw, wave}: the camera's rail; it stops
-- at a node with a wave. A spawn: {t, kind, x, z, y, dx, dz, dy}: at t seconds into the wave,
-- at (x, y, z), arriving from (dx, dy, dz) away (left out: 0). Kinds: g grunt, r runner, h heavy (4 hits),
-- c civilian (don't shoot; {t, "c", x, z, y, 1 or -1}: walks right or left), d drone, b barrel, p pickup ({t, "p", x, z, y, kind}), B boss.

local C1, C2, C3, CRATE = 0x6b4f4a, 0x58606e, 0x7a6450, 0x9a7040
local STAGES = {
  {name="DOWNTOWN",sky={0xff9a5a,0x3a3644},light={-100,40,235,0x5a4658},shadow=0x24202c,
  ground={0x2a2832,0x5a4a58},aim=2.5,
  props={
   {"bA",C1,-620,300},{"bB",C2,-620,820},{"bA",C3,-620,1320},{"bB",C2,-620,1840},
   {"bB",C3,620,350},{"bA",C1,620,860},{"bB",C2,620,1400},{"bB",C1,620,1980},
   {"bW",C2,-150,2900},{"bA",C3,1150,2860,nil,90},{"bA",C1,1700,2860,nil,90},
   {"bB",C3,1250,1900,nil,90},{"bB",C2,1800,1900,nil,90},{"bW",C2,2500,2350,nil,90},
   {"car",0x2a4a8a,-190,1150},{"cab",0x1a2a40,-190,1110,125},
   {"crate",CRATE,170,1000},{"crate",CRATE,290,1020},{"crate",CRATE,230,1010,165},
   {"crate",CRATE,1450,2230},{"crate",CRATE,1450,2470}},
  path={{0,-250,0},{0,650,0,1},{0,1400,0,2},{0,2000,0},{300,2350,90},{1000,2350,90,3}},
  waves={
   {{0.4,"g",60,1110,0,170},{1.2,"g",-60,1450,0,-130},
   {2.6,"g",420,1800,0,250},{3.6,"r",150,1900,0,-250}},
   {{0.3,"c",-350,1900,0,1},{0.6,"g",200,2050,0,250},
   {1.4,"g",-200,2150,0,-250},{2.4,"p",-120,1800,90,"mag"},
   {3.0,"h",60,2300,0,0,300},{4.2,"r",250,2200,0,250}},
   {{0.4,"g",1540,2160,0,0,70},{0.9,"g",1540,2540,0,0,-70},
   {2.0,"h",2000,2350,0,300},{2.8,"c",1700,2650,0,1},
   {3.5,"r",1800,2200,0,0,-300},{4.5,"p",1300,2350,100,"heart"},
   {5.5,"g",1900,2450,0,400}}}},
  {name="THE DOCKS",sky={0x8fb8c8,0x86867f},light={-125,55,240,0x40505a},shadow=0x50565a,
  ground={0x6a6a64,0x2a5a7a},aim=2.1,
  props={
   {"cont",0xa8402e,-560,800},{"cont",0x3a7a4a,-560,800,390},{"cont",0x2e5a9a,-480,1500},
   {"cont",0x2e5a9a,560,500},{"cont",0xc07a30,-320,2750,nil,90},{"cont",0xa8402e,320,2780,nil,90},
   {"crate",CRATE,-150,1150},{"crate",CRATE,150,2350},{"crate",CRATE,-220,2420},
   {"leg",0xd8b030,520,1700},{"leg",0xd8b030,520,2600},{"beam",0xd8b030,520,2150,900},
   {"ware",0x6a6e72,-1000,2100,nil,90}},
  path={{0,-250,0},{0,520,-25,1},{0,1350,30,2},{0,2000,0,3}},
  waves={
   {{0.4,"g",-430,1300,260,0,0,-215},{1.0,"g",-380,900,0,-150},
   {1.0,"b",-300,990,0},{2.2,"g",-20,1230,0,-130},
   {3.2,"r",-260,1350,0,-220},{4.0,"g",-480,1650,260,0,0,-215}},
   {{0.3,"d",420,1900,260,0,300,250},{1.0,"d",500,1950,320,200,0,250},
   {1.6,"g",380,1800,0,250},{2.0,"c",130,2080,0,1},
   {2.5,"p",250,1600,100,"spread"},{3.2,"d",300,2100,300,-200,0,250},
   {4.0,"h",350,2200,0,0,350}},
   {{0.4,"g",-150,2700,260,0,0,-215},{0.8,"g",150,2780,260,0,0,-215},
   {1.0,"b",-80,2500,0},{1.0,"b",130,2560,0},{1.6,"h",200,2550,0,300},
   {2.4,"r",-60,2500,0,-160},{3.2,"d",0,2500,260,0,300,250},
   {4.0,"p",100,2300,110,"gold"},{4.5,"h",-150,2560,0,-400},
   {5.5,"g",-60,2700,260,0,0,-215}}}},
  {name="SKYLINE",sky={0x070722,0x34303e},light={-100,40,210,0x3a3050},shadow=0x15141e,
  ground={0x22202c,0x1a1a28},aim=1.8,
  props={
   {"ac",0x6a6e78,-300,1000},{"ac",0x6a6e78,320,1300},{"ac",0x6a6e78,-250,1900},
   {"hut",0x4a4650,-450,1250},{"tank",0x6a4a3a,420,900,80},{"roof",0x5a3a2e,420,900,195},
   {"neon",0xff3a9a,-450,1116,300},{"tower",0x1c1e2c,-900,2900},{"tower",0x1c1e2c,750,3100},
   {"tower",0x1c1e2c,-100,3400,700},{"neon",0x3af0ff,-900,2716,1100},{"neon",0xff3a9a,750,2916,1300}},
  path={{0,-250,0},{0,650,0,1},{0,1450,-20,2},{0,2050,0,3}},
  waves={
   {{0.4,"d",-110,1200,300,0,300,300},{0.9,"d",160,1300,260,0,300,300},
   {1.5,"g",-130,1150,0,-170},{2.0,"c",400,1250,0,-1},
   {2.8,"g",-260,1400,0,-160},{3.6,"r",250,1500,0,250},
   {4.4,"d",0,1500,330,0,300,300}},
   {{0.4,"h",-80,2000,0,-170},{1.0,"g",-420,2100,0,-200},
   {1.8,"r",-100,2300,0,300},{2.5,"p",-150,1800,100,"mag"},
   {3.0,"d",-300,2000,320,0,300,300},{3.8,"h",-200,2400,0,0,350},
   {4.6,"c",-300,1900,0,1},{5.2,"g",-50,1950,0,250}},
   {{0.5,"B",0,2950,380,0,600,250}}}},
}
local NEON = {[0xff3a9a] = true, [0x3af0ff] = true}  -- props that glow (unlit)

-- enemies' looks: legs, torso, head
local LOOK = {g = {0x2a2e3a,0xb83a3a,0xd8a07a},r = {0x2a2e3a,0xe08a2a,0xd8a07a},
              h = {0x2a3028,0x4a5a40,0x2a3028},c = {0x3a5a9a,0xf0f0f0,0xd8a07a}}
local PTS = {g = 100,r = 150,h = 300,d = 200}
local PICK = {mag = {0x5ac8ff,"MAG+"},spread = {0xc85aff,"SPREAD"},heart = {0xff4a6a,"LIFE"},gold = {0xffd34a,"GOLD"}}

-- ---- state ----

local mode, mt = "title", 0                      -- the game's mode and the time in it
local stage, sdef, stop = 1, STAGES[1], 1        -- the stage, its table, the camera's node
local leg, leg_len, leg_t, leg_d = {}, 0, 0, 1   -- the rail from one stop to the next
local CX, CY, CZ, CYAW = 0, EYE, -250, 0
local walked = 0                                  -- the ground's stripes follow it
local queue, qi, wt, calm = nil, 1, 0, 0          -- the wave's spawns, the next, its time, time since it ended
local score, best, lives, rounds, cap, streak = 0, 0, 5, 12, 12, 0
local mag_n = 0                                  -- the rounds the magazine on screen shows
local rel = -1                                    -- the reload's progress (-1: not reloading)
local power, power_t = nil, 0
local shots, hits, hurt = 0, 0, 0                 -- this stage's, for its bonus
local shake, recoil, hurt_t, dry_t = 0, 0, 0, 0
local card, card_n, card_until = nil, 0, nil
local lay = 0                                     -- bumped to draw the bands whole
local new_best = false
local pops, sparks = {}, {}
local last, was_down, fdt = 0, false, 0

-- objects, made in _init
local STRIP, FAR                          -- the ground: 7 thin stripes in one mesh, and the far ground
local MEN, DRONES, ORBS, BARRELS, BOOMS = {}, {}, {}, {}, {}
local PU, BOSS = nil, nil

-- Flat quads at y 0 facing up, from z0 to z1 (2100 either side); a vertex far below (in no face)
-- makes "up" the outside, as the shape's center is then under them.
local function flat(z0, depth, step, n)
  local v, f = {0, -1000, 0}, {}
  for i = 0, n - 1 do
    local a = z0 + i * step
    local k = #v // 3
    for _, q in ipairs({-2100, 0, a, 2100, 0, a, -2100, 0, a + depth, 2100, 0, a + depth}) do v[#v + 1] = q end
    for _, j in ipairs({1, 3, 2, 2, 3, 4}) do f[#f + 1] = k + j end
  end
  return shape(v, f, true)
end

local function mk(k, col, shine, unlit)
  local o = obj(S[k], col, shine or 0, unlit)
  hide(o)
  return o
end
local function gone(e)
  e.st = 0
  for _, o in ipairs(e.o) do hide(o) end
end
local function f7(n) return string.format("%07d", n) end
local function smooth(t) return t * t * (3 - 2 * t) end
local function face(x, z) return deg(atan(CX - x, CZ - z)) end  -- the yaw that looks at the camera

-- ---- the HUD (the layer) ----

local function draw_score(s)
  hd.rect(0, 22, 175, 38, BG)
  hd.text(f7(s), hd.inset(24, 36) + 8, 22, INK, 30, "vt")
end

local function draw_mid(st, mult)
  hd.rect(175, 22, 60, 38, BG)
  hd.text("STAGE " .. st, 205, 26, SOFT, 12, "mono", "center")
  if mult > 1 then hd.text("x" .. mult, 205, 38, AMBER, 18, "bold", "center") end
end

local function draw_lives(n)
  hd.rect(235, 22, W - 235, 38, BG)
  local x0 = W - hd.inset(24, 36) - 20
  for i = 1, n do
    local x, y = x0 - (i - 1) * 25, 28
    hd.circle(x - 5, y + 5, 5.5, RED)
    hd.circle(x + 5, y + 5, 5.5, RED)
    for r = 0, 5 do
      local w = 10.5 - r * 1.9
      hd.rect(x - w, y + 6 + r * 2, w * 2, 2, RED)
    end
  end
end

-- The magazine, lying in the bottom band: rounds in a window along it, the feed lips on the left
-- (rounds leave from there), the count on the baseplate. `off` slides it out and back (a reload).
local MX, MY, MW, MH = 80, S1 + 6, 250, 44
local function slot(i, n, cp, gold, y)  -- round i of cp: still there (the last n), or gone
  local sw = (MW - 58) / cp
  local x = MX + 7 + (i - 1) * sw
  if i > cp - n then
    hd.rect(x, y + 19, sw - 2, 15, 0xc8a040)
    hd.rect(x, y + 11, sw - 2, 10, gold and GOLD or 0xd07a40, 255, (sw - 2) / 2)
  else
    hd.rect(x + (sw - 2) / 2 - 1, y + 30, 2, 3, 0x262a33)
  end
end

local function draw_mag(n, cp, off, warn, gold)
  hd.rect(60, S1, W - 120, H - S1, BG)
  local y = MY + off
  local win = MW - 58
  hd.rect(MX - 6, y + 7, 10, MH - 14, 0x1c2028, 255, 2)
  hd.rect(MX, y, MW - 40, MH, warn and 0x4a1c24 or 0x262b35, 255, 6)
  hd.rect(MX + 2, y + 2, MW - 44, 3, 0x3a414e)
  hd.rect(MX + MW - 46, y - 3, 46, MH + 6, 0x343b48, 255, 7)
  hd.rect(MX + 6, y + 8, MW - 58, MH - 16, 0x0b0d12, 255, 4)
  for i = 1, cp do slot(i, n, cp, gold, y) end
  hd.text(gold and "INF" or tostring(n), MX + MW - 23, y + 9, warn and RED or INK, 24, "bold", "center")
end

-- After a shot only the round that went and the count change: just those two are repainted.
local function slot_box(i, cp)
  local sw = (MW - 58) / cp
  return MX + 6 + (i - 1) * sw, MY + 10, sw, MH - 20
end
local function draw_slot(i, n, cp)
  hd.rect(MX, MY, MW, MH, 0x0b0d12)
  slot(i, n, cp, false, MY)
end
local function draw_count(n, warn)
  hd.rect(MX, MY, MW, MH, 0x343b48)
  hd.text(tostring(n), MX + MW - 23, MY + 9, warn and RED or INK, 24, "bold", "center")
end

local function panel(x, y, w, h)
  hd.rect(x, y, w, h, 0x05070c, 200, 18)
  hd.frame(x, y, w, h, AMBER, 2, 18, 200)
end

local CARD = {45, S0 + 100, W - 90, 160}
local function draw_card(k)
  local x, y, w = CARD[1], CARD[2], CARD[3]
  local m = x + w / 2
  if k.big then  -- "ACTION!", "GO!": a word over the scene
    hd.rect(m - 110, y + 48, 220, 64, 0x05070c, 170, 14)
    hd.text(k.big, m, y + 58, k.col or AMBER, 32, "bold", "center")
    return
  end
  panel(x, y, w, CARD[4])
  hd.text(k[1], m, y + 12, k.col or AMBER, 32, "bold", "center")
  for i = 2, #k do hd.text(k[i], m, y + 4 + i * 28, i == #k and SOFT or INK, i == #k and 14 or 18, i == #k and "mono" or "bold", "center") end
end

local function draw_pill(p, t)
  hd.rect(18, S0 + 8, 150, 26, 0x05070c, 190, 13)
  hd.text(p .. " " .. t, 93, S0 + 12, PICK[p == "SPREAD" and "spread" or "gold"][1], 16, "bold", "center")
end

local function draw_bossbar(hp)
  hd.rect(W - 212, S0 + 8, 194, 26, 0x05070c, 190, 13)
  hd.text("BOSS", W - 200, S0 + 13, RED, 12, "mono")
  hd.rect(W - 160, S0 + 15, 132, 12, 0x2a1018, 255, 6)
  if hp > 0 then hd.rect(W - 160, S0 + 15, 132 * hp / 40, 12, RED, 255, 6) end
end

local function draw_hint()
  hd.rect(W / 2 - 110, S1 - 40, 220, 30, 0x05070c, 190, 15)
  hd.text("RELOAD! TAP THE MAG", W / 2, S1 - 35, RED, 16, "bold", "center")
end

local function clear_then(fn, ...)
  hd.clear()
  if fn then fn(...) end
end

local function draw_hud()
  local mult = min(4, 1 + streak // 6)
  local whole = ui.changed("layout", lay)
  if whole then
    hd.rect(0, 0, W, S0, BG)
    hd.rect(0, S1, W, H - S1, BG)
  end
  if ui.changed("score", score) or whole then draw_score(score) end
  if ui.changed("mid", stage, mult) or whole then draw_mid(stage, mult) end
  if ui.changed("lives", lives) or whole then draw_lives(lives) end
  local off = 0
  if rel >= 0 then off = floor((rel < 0.5 and rel or 1 - rel) * 2 * 56) end
  local warn = dry_t > 0 or (rounds == 0 and power ~= "GOLD")
  if ui.changed("mag", cap, off, warn, power == "GOLD") or whole then
    hd.repaint(60, S1, W - 120, H - S1, draw_mag, rounds, cap, off, warn, power == "GOLD")
    mag_n = rounds
  elseif rounds ~= mag_n then
    for i = cap - max(rounds, mag_n) + 1, cap - min(rounds, mag_n) do
      local x, y, w, h = slot_box(i, cap)
      hd.repaint(x, y, w, h, draw_slot, i, rounds, cap)
    end
    hd.repaint(MX + MW - 42, MY + 6, 38, 30, draw_count, rounds, warn)
    mag_n = rounds
  end
  -- over the scene
  if ui.changed("card", card_n) then hd.repaint(CARD[1], CARD[2], CARD[3], CARD[4], clear_then, card and draw_card, card) end
  local pt = power and math.ceil(power_t)
  if ui.changed("pill", power, pt) then hd.repaint(18, S0 + 8, 150, 26, clear_then, power and draw_pill, power, pt) end
  local bh = BOSS and BOSS.st > 0 and BOSS.st < 3 and BOSS.hp
  if ui.changed("boss", bh) then hd.repaint(W - 212, S0 + 8, 194, 26, clear_then, bh and draw_bossbar, bh) end
  local hint = (mode == "fight" or mode == "move") and rounds == 0 and power ~= "GOLD" and rel < 0 and floor(time() * 3) % 2 == 0
  if ui.changed("hint", hint) then hd.repaint(W / 2 - 110, S1 - 40, 220, 30, clear_then, hint and draw_hint) end
end

local function show_card(k, secs)
  card, card_n, card_until = k, card_n + 1, secs and time() + secs
end

-- ---- the world ----

local function reset_actors()
  for _, pool in ipairs({MEN, DRONES, ORBS, BARRELS, BOOMS, {PU, BOSS}}) do
    for _, e in ipairs(pool) do gone(e) end
  end
  queue, power, power_t = nil, nil, 0
end

local function set_stage(n)
  stage, sdef = n, STAGES[n]
  for i, s in ipairs(STAGES) do
    for _, p in ipairs(s.props) do hide(p.o, i ~= n) end
  end
  sky(sdef.sky[1], sdef.sky[2])
  light(unpack(sdef.light))
  ground(0, sdef.shadow)
  paint(STRIP, sdef.ground[1])
  paint(FAR, sdef.ground[2])
  reset_actors()
  stop = 1
  local p = sdef.path[1]
  CX, CZ, CYAW, walked = p[1], p[2], p[3], 0
  shots, hits, hurt = 0, 0, 0
end

local function start_leg()
  local p, i = sdef.path, stop
  leg, leg_len = {p[i]}, 0
  repeat
    i = i + 1
    local a, b = p[i - 1], p[i]
    leg_len = leg_len + sqrt((b[1] - a[1]) ^ 2 + (b[2] - a[2]) ^ 2)
    leg[#leg + 1] = b
  until b[4] or i == #p
  stop = i
  leg_t, leg_d = 0, max(1.2, leg_len / SPEED)
  mode = "move"
  fps(60)
end

local function cam_on_leg(u)
  local d = u * leg_len
  for k = 2, #leg do
    local a, b = leg[k - 1], leg[k]
    local l = sqrt((b[1] - a[1]) ^ 2 + (b[2] - a[2]) ^ 2)
    if d <= l or k == #leg then
      local f = l > 0 and min(1, d / l) or 1
      CX, CZ, CYAW = a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f, a[3] + (b[3] - a[3]) * f
      return
    end
    d = d - l
  end
end

local function free(pool)
  for _, e in ipairs(pool) do if e.st == 0 then return e end end
end

local function look(e)
  local l = LOOK[e.k]
  paint(e.o[1], l[1])
  paint(e.o[2], l[2])
  paint(e.o[3], l[3])
end

local function flash(e, n)
  for i = 1, n do paint(e.o[i], 0xffffff) end
  e.fl = 0.07
end

local function boom(x, y, z, big)
  local b = BOOMS[big and 1 or 2]
  b.st, b.t, b.x, b.y, b.z = 1, 0, x, y, z
  place(b.o[1], x, y, z)
  hide(b.o[1], false)
  local sx, sy, d = project(x, y, z)
  if sx then sparks[#sparks + 1] = {x = sx, y = sy, t = 0, r = (big and 150 or 70) * F / d} end
  sfx(4)
end

local function spawn(s)
  local k = s[2]
  local e
  if k == "d" then e = free(DRONES)
  elseif k == "b" then e = free(BARRELS)
  elseif k == "p" then e = PU.st == 0 and PU or nil
  elseif k == "B" then e = BOSS
  else e = free(MEN) end
  if not e then return false end
  e.k, e.x, e.z, e.y = k, s[3], s[4], s[5] or 0
  e.dx, e.dz, e.dy = s[6] or 0, s[7] or 0, s[8] or 0
  e.st, e.a, e.t, e.fl, e.fall, e.ox, e.aim, e.mz = 1, 0, 0, 0, 0, 0, nil, 0
  e.cool = 0.4 + rnd(0.8)
  e.aimd = sdef.aim + (k == "h" and 0.4 or k == "r" and -0.3 or k == "d" and -0.2 or 0)
  e.hp = k == "h" and 4 or k == "B" and 40 or 1
  local yaw = sdef.path[stop][3]
  e.rx, e.rz = cos(rad(yaw)), -sin(rad(yaw))    -- the stop's right: runners strafe along it, civilians walk it
  if k == "c" then e.vx, e.vz, e.dx, e.dz, e.dy = e.rx * 150 * s[6], e.rz * 150 * s[6], 0, 0, 0 end
  if k == "p" then
    e.kind, e.dx = s[6], 0
    paint(e.o[1], PICK[e.kind][1], 0, true)
  elseif k == "B" then
    e.fire, e.drop = 2.5, 0
  elseif e.o[4] then
    look(e)
    hide(e.o[4], k == "c")
    shadow(e.o[1], e.y == 0)
    shadow(e.o[2], e.y == 0)
  end
  for i, o in ipairs(e.o) do
    if not (k == "c" and i == 4) then hide(o, false) end
  end
  return true
end

local function hostile()
  for _, e in ipairs(MEN) do if e.st == 1 or e.st == 2 then if e.k ~= "c" then return true end end end
  for _, e in ipairs(DRONES) do if e.st == 1 or e.st == 2 then return true end end
  for _, e in ipairs(ORBS) do if e.st > 0 then return true end end
  return BOSS.st > 0
end

local function finish(m, title, again)  -- game over, or the mission complete
  mode, mt = m, 0
  if score > best then best, new_best = score, true save({best = best}) end
  show_card({title, "SCORE " .. f7(score), new_best and "NEW BEST!" or "BEST " .. f7(best), again})
  fps(30)
end

local function game_over()
  reset_actors()
  finish("over", "GAME OVER", "tap to try again")
end

local function hurt_player(why)
  if mode ~= "fight" and mode ~= "move" then return end
  lives, hurt, streak = lives - 1, hurt + 1, 0
  hurt_t, shake = 0.35, 0.3
  buzz(140)
  sfx(2)
  if why then pops[#pops + 1] = {x = 64, y = 60, t = 0, s = why, col = 8} end
  if lives <= 0 then game_over() end
end

local function add(pts, x, y, z, txt)
  pts = pts * min(4, 1 + streak // 6) * (power == "GOLD" and 2 or 1)
  score = score + pts
  local sx, sy = project(x, y, z)
  if sx then pops[#pops + 1] = {x = sx, y = sy, t = 0, s = txt or ("+" .. pts), col = txt and 10 or 7} end
end

local function man_pos(e)  -- its feet, with its arrival and a runner's strafe
  local u = 1 - smooth(e.a)
  return e.x + e.dx * u + e.rx * e.ox, e.y + e.dy * u, e.z + e.dz * u + e.rz * e.ox
end

local function kill(e, head)
  e.st, e.fall = 3, 0
  flash(e, e.o[4] and 3 or 2)
  local x, y, z = man_pos(e)
  if e.k == "d" then boom(x, y, z) gone(e) e.st = 3 end
  add(PTS[e.k] * (head and 2 or 1), x, y + 150, z, head and "HEADSHOT" or nil)
  sfx(2)
end

local function hit_man(e, part)
  if e.k == "c" then
    e.st, e.fall = 3, 0
    hurt_player("DON'T SHOOT CIVILIANS!")
    return false
  end
  e.hp = e.hp - (part == "head" and 2 or 1)
  if e.hp <= 0 then kill(e, part == "head") else flash(e, 3) sfx(2) end
  return true
end

local function explode_barrel(e)
  gone(e)
  boom(e.x, 60, e.z, true)
  shake = 0.25
  for _, pool in ipairs({MEN, DRONES}) do
    for _, m in ipairs(pool) do
      if (m.st == 1 or m.st == 2) and m.k ~= "c" then
        local x, _, z = man_pos(m)
        if (x - e.x) ^ 2 + (z - e.z) ^ 2 < 300 * 300 then kill(m) end
      end
    end
  end
end

local function collect(kind)
  local p = PICK[kind]
  if kind == "mag" then cap = min(24, cap + 6) rounds = cap
  elseif kind == "heart" then lives = min(6, lives + 1)
  else power, power_t = p[2], kind == "gold" and 10 or 12 end
  pops[#pops + 1] = {x = 64, y = 40, t = 0, s = p[2] .. (kind == "mag" and " " .. cap .. " ROUNDS" or ""), col = 12}
  sfx(1)
  buzz(30)
end

local function boss_pos(b)
  local u = 1 - smooth(min(1, b.a))
  return b.x + sin(b.t * 0.55) * 300 * (1 - u) + b.dx * u, b.y + sin(b.t * 1.4) * 35 + b.dy * u - b.fall * 150, b.z + b.dz * u
end

-- What a tap at picture pixel (hx, hy) hits: the nearest thing under it (with SPREAD, everything
-- in reach). A figure hit at its head and body counts as the head.
local function hit_test(hx, hy)
  local tol, got, near, nd = power == "SPREAD" and 9 or 2.5, {}, nil, 1e9
  local function try(e, part, x, y, z, r)
    local sx, sy, d = project(x, y, z)
    if sx and (sx - hx) ^ 2 + (sy - hy) ^ 2 <= (r * F / d + tol) ^ 2 and got[e] ~= "head" then
      got[e] = part
      if d < nd then near, nd = e, d end
    end
  end
  for _, e in ipairs(MEN) do
    if e.st == 1 or e.st == 2 then
      local x, y, z = man_pos(e)
      try(e, "body", x, y + 136, z, 46)
      try(e, "body", x, y + 48, z, 38)
      try(e, "head", x, y + 197, z, 19)
    end
  end
  for _, e in ipairs(DRONES) do if e.st == 1 or e.st == 2 then local x, y, z = man_pos(e) try(e, "drone", x, y, z, 38) end end
  for _, e in ipairs(ORBS) do if e.st > 0 then try(e, "orb", e.px, e.py, e.pz, 28) end end
  for _, e in ipairs(BARRELS) do if e.st > 0 then try(e, "barrel", e.x, 50, e.z, 46) end end
  if PU.st > 0 then try(PU, "pick", PU.x, PU.y, PU.z, 42) end
  if BOSS.st == 2 then
    local x, y, z = boss_pos(BOSS)
    try(BOSS, "boss", x, y, z, 190)
    try(BOSS, "head", x, y, z - 100, 70)
  end
  local any = false
  for e, part in pairs(got) do
    if (e == near or power == "SPREAD") and e.st > 0 and e.st < 3 then
      local ok = true
      if e == BOSS then
        e.hp = max(0, e.hp - (part == "head" and 2 or 1))
        flash(e, 2)
        add(part == "head" and 40 or 20, boss_pos(e))
        if e.hp <= 0 then e.st, e.fall, shake = 3, 0, 0.6 end
      elseif part == "drone" then kill(e)
      elseif part == "orb" then gone(e) boom(e.px, e.py, e.pz) add(50, e.px, e.py, e.pz)
      elseif part == "barrel" then explode_barrel(e)
      elseif part == "pick" then gone(e) collect(e.kind)
      else ok = hit_man(e, part) end
      any = any or ok
    end
  end
  return any
end

local function shoot(tx, ty)
  if rel >= 0 then return end
  if rounds <= 0 and power ~= "GOLD" then
    dry_t = 0.4
    sfx(6)
    return
  end
  if power ~= "GOLD" then rounds = rounds - 1 end
  recoil = 1
  local hx, hy = (tx - PX) / 3, (ty - PY) / 3
  sparks[#sparks + 1] = {x = hx, y = hy, t = 0}
  if mode == "fight" then shots = shots + 1 end
  if hit_test(hx, hy) then
    hits, streak = hits + 1, streak + 1
  else
    streak = 0
  end
end

local function reload()
  if rel < 0 and rounds < cap and power ~= "GOLD" then rel = 0 sfx(5) end
end

local function stage_clear()
  mode, mt = "clear", 0
  local acc = shots > 0 and hits / shots or 1
  local bonus = floor(min(1, acc) * 30) * 100 + (hurt == 0 and 2000 or 0)
  score = score + bonus
  show_card({"STAGE CLEAR", "ACCURACY " .. floor(min(1, acc) * 100) .. "%", "BONUS +" .. bonus, hurt == 0 and "PERFECT: NO HITS TAKEN" or sdef.name .. " DONE"})
  sfx(7)
  fps(30)
end

local function to_intro(n)
  set_stage(n)
  mode, mt = "intro", 0
  lay = lay + 1
  show_card({"STAGE " .. n, sdef.name, n == 1 and "tap to shoot · tap the mag to reload" or "get ready"}, 2.2)
  fps(30)  -- (the card over the moving scene: a steady 30, not a 60 it can't hold; play is 60)
end

local function new_game()
  score, lives, cap, rounds, streak, rel, new_best = 0, 5, 12, 12, 0, -1, false
  to_intro(1)
end

local function to_title()
  set_stage(1)
  mode, mt = "title", 0
  lay = lay + 1
  show_card({"LOCK & LOAD", "3 STAGES · 1 MAGAZINE", string.format("BEST %07d", best), "tap to start"})
  fps(30)
end

-- ---- the actors, each frame ----

local function upd_man(e, dt)
  e.t = e.t + dt
  if e.st == 1 then
    e.a = min(1, e.a + dt / (e.k == "d" and 1.0 or 0.45))
    if e.a >= 1 then e.st = 2 end
  elseif e.st == 2 then
    if e.k == "c" then
      e.x, e.z = e.x + e.vx * dt, e.z + e.vz * dt
      if e.t > 5 then gone(e) end
    else
      if e.k == "r" then e.ox = sin(e.t * 2.2) * 110 elseif e.k == "d" then e.ox = sin(e.t * 1.3) * 140 end
      if e.aim then
        e.aim = e.aim + dt
        if e.aim >= e.aimd then
          e.aim, e.cool, e.mz = nil, 0.9 + rnd(1.2), 0.12
          hurt_player()
        end
      else
        e.cool = e.cool - dt
        -- only aims where you can see it (and its ring): never a shot from off the screen
        local x, y, z = man_pos(e)
        local sx, sy = project(x, y + (e.k == "d" and 0 or 136), z)
        if e.cool <= 0 and sx and sx > 6 and sx < 122 and sy > 6 and sy < 122 then e.aim = 0 end
      end
    end
  elseif e.st == 3 then
    e.fall = e.fall + dt
    if e.fall > 0.55 then
      gone(e)
    end
  end
  if e.mz > 0 then e.mz = e.mz - dt end
  if e.fl > 0 then
    e.fl = e.fl - dt
    if e.fl <= 0 and e.o[4] then look(e) end
  end
end

-- A part of a figure standing at x, y, z facing (fx, fz), fallen back by a degrees (s, k its sine
-- and cosine): h up it, side to its left, fwd towards the camera. (No closure: nothing allocated a frame.)
local function part(o, x, y, z, fx, fz, s, k, a, fy, h, side, fwd)
  place(o, x - fx * h * s + fz * side + fx * fwd, y + h * k, z - fz * h * s - fx * side + fz * fwd, -a, fy, 0)
end

local function put_man(e)
  local x, y, z = man_pos(e)
  local fy = face(x, z)
  local fx, fz = sin(rad(fy)), cos(rad(fy))     -- toward the camera
  local a = min(90, e.fall * 260)                -- falls back, away from it
  local s, k = sin(rad(a)), cos(rad(a))
  local o = e.o
  part(o[1], x, y, z, fx, fz, s, k, a, fy, 48, 0, 0)
  part(o[2], x, y, z, fx, fz, s, k, a, fy, 136, 0, 0)
  part(o[3], x, y, z, fx, fz, s, k, a, fy, 197, 0, 0)
  if e.k ~= "c" then part(o[4], x, y, z, fx, fz, s, k, a, fy, 145, 30, 36) end
end

local function put_drone(e)
  local x, y, z = man_pos(e)
  y = y + sin(e.t * 2.1) * 30 - e.fall * 200
  place(e.o[1], x, y, z, e.t * 90, e.t * 140, 0)
  place(e.o[2], x, y + 26, z, 90, 0, e.t * 400)
end

local function upd_boss(b, dt)
  b.t = b.t + dt
  if b.st == 1 then
    b.a = b.a + dt / 2.2
    if b.a >= 1 then b.st = 2 end
  elseif b.st == 2 then
    b.fire = b.fire - dt
    if b.fire <= 0 then
      b.fire = max(1.2, 2.6 - (1 - b.hp / 40) * 1.4)
      local x, y, z = boss_pos(b)
      for side = (b.hp < 20 and -1 or 1), 1, 2 do
        local o = free(ORBS)
        if o then
          o.st, o.t, o.sx, o.sy, o.sz = 1, 0, x + side * 220, y - 20, z - 60
          o.px, o.py, o.pz = o.sx, o.sy, o.sz
          hide(o.o[1], false)
        end
      end
    end
    if (b.drop == 0 and b.hp <= 28) or (b.drop == 1 and b.hp <= 14) then
      b.drop = b.drop + 1
      spawn({0, "d", -250, 2600, 300, 0, 0, 300})
      spawn({0, "d", 250, 2650, 320, 0, 0, 300})
      spawn({0, "p", 0, 2450, 130, b.drop == 1 and "spread" or "heart"})
    end
  elseif b.st == 3 then
    b.fall = b.fall + dt
    if floor(b.fall * 6) ~= floor((b.fall - dt) * 6) then
      local x, y, z = boss_pos(b)
      boom(x + rnd(400) - 200, y + rnd(100), z - 80, rnd(1) < 0.4)
      shake = 0.3
      buzz(40)
    end
    if b.fall > 2.4 then
      gone(b)
      local x, y, z = boss_pos(b)
      score = score + 5000
      pops[#pops + 1] = {x = 64, y = 50, t = 0, s = "BOSS DOWN +5000", col = 10}
      for _, o in ipairs(ORBS) do gone(o) end
    end
  end
  if b.fl > 0 then
    b.fl = b.fl - dt
    if b.fl <= 0 then paint(b.o[1], 0x2a2e36, 60) paint(b.o[2], 0x5ab8e0, 120) end
  end
end

local function put_boss(b)
  local x, y, z = boss_pos(b)
  local tilt = b.fall * 20
  place(b.o[1], x, y, z, tilt, 0, b.fall * 15)
  place(b.o[2], x, y, z - 100, tilt, 0, 0)
  place(b.o[3], x, y + 10, z + 230, tilt, 0, 0)
  place(b.o[4], x, y + 80, z, 0, b.t * 900, 0)
  place(b.o[5], x - 215, y - 30, z - 10, 90, 0, 0)
  place(b.o[6], x + 215, y - 30, z - 10, 90, 0, 0)
end

local function upd_orb(o, dt)
  o.t = o.t + dt / 2.3
  local fx, fz = sin(rad(CYAW)), cos(rad(CYAW))
  local u = o.t * o.t
  o.px = o.sx + (CX + fx * 60 - o.sx) * u
  o.py = o.sy + (CY - 30 - o.sy) * u
  o.pz = o.sz + (CZ + fz * 60 - o.sz) * u
  place(o.o[1], o.px, o.py, o.pz)
  if o.t >= 1 then
    gone(o)
    hurt_player()
  end
end

local function put_ground()
  local fx, fz = sin(rad(CYAW)), cos(rad(CYAW))
  local ph = walked % 240                        -- the stripes slide back as you walk, a pair at a time
  place(STRIP, CX - fx * ph, 0, CZ - fz * ph, 0, CYAW, 0)
  place(FAR, CX, -2, CZ, 0, CYAW, 0)
end

-- ---- the cart ----

function _init()
  for k, v in pairs(SH) do S[k] = shape(unpack(v)) end
  for _, s in ipairs(STAGES) do
    for _, p in ipairs(s.props) do
      p.o = mk(p[1], p[2], 0, NEON[p[2]])
      place(p.o, p[3], p[5] or half(p[1]), p[4], 0, p[6] or 0, 0)
      if p[1] == "crate" or p[1] == "car" or p[1] == "cont" or p[1] == "ac" then shadow(p.o, true) end
    end
  end
  for i = 1, 6 do MEN[i] = {o = {mk("legs", 0x2a2e3a), mk("torso", 0xb83a3a), mk("head", 0xd8a07a), mk("gun", 0x16181c)}, st = 0} end
  for i = 1, 3 do DRONES[i] = {o = {mk("drone", 0x40464f, 90), mk("rotor", 0x9aa0a8)}, st = 0} end
  for i = 1, 4 do ORBS[i] = {o = {mk("orb", 0xff6a3a, 0, true)}, st = 0} end
  for i = 1, 3 do BARRELS[i] = {o = {mk("barrel", 0xc0302a, 60)}, st = 0} end
  BOOMS[1] = {o = {mk("boom", 0xffa030, 0, true)}, st = 0}
  BOOMS[2] = {o = {mk("boom2", 0xffa030, 0, true)}, st = 0}
  PU = {o = {mk("pick", 0x5ac8ff, 0, true)}, st = 0}
  BOSS = {o = {mk("hull", 0x2a2e36, 60), mk("cock", 0x5ab8e0, 120), mk("tail", 0x2a2e36, 60),
               mk("blade", 0x1a1c20), mk("pod", 0x3a3e46), mk("pod", 0x3a3e46)}, st = 0, fl = 0}
  -- the ground last: drawn after what stands on it, its hidden pixels fail the depth test early
  STRIP = obj(flat(100, 26, 240, 7), 0x444444)
  FAR = obj(flat(1540, 2000, 0, 1), 0x444444)
  local s = load()
  best = s and s.best or 0
  last = time()
  to_title()
end

function _update()
  local now = time()
  local dt = min(0.05, now - last)
  last, fdt = now, dt
  mt = mt + dt
  local tx, ty = touch(true)
  local tap = tx and not was_down
  was_down = tx ~= nil
  if card_until and now > card_until then card, card_n, card_until = nil, card_n + 1, nil end
  if AUTO then AUTO(dt) end

  if mode == "title" then
    CZ, CYAW = -250 + sin(mt * 0.2) * 150, sin(mt * 0.13) * 25
    if tap then new_game() end
  elseif mode == "over" or mode == "win" then
    if tap and mt > 1.2 then to_title() end
  elseif mode == "clear" then
    if mt > 3.5 then
      if stage < #STAGES then to_intro(stage + 1)
      else finish("win", "MISSION COMPLETE", "tap to play again") end
    end
  else -- intro, move, fight: the game
    if tap then
      if ty >= S1 - 6 then reload() elseif ty > S0 then shoot(tx, ty) end
    end
    if btnp(5) then reload() end
    if rel >= 0 then
      local was = rel
      rel = rel + dt / 0.5
      if was < 0.5 and rel >= 0.5 then rounds = cap end
      if rel >= 1 then rel = -1 end
    end
    if power then
      power_t = power_t - dt
      if power_t <= 0 then power = nil end
    end
    if mode == "intro" then
      if mt > 0.6 then start_leg() end
    elseif mode == "move" then
      local ox, oz = CX, CZ
      leg_t = leg_t + dt
      cam_on_leg(smooth(min(1, leg_t / leg_d)))
      walked = walked + sqrt((CX - ox) ^ 2 + (CZ - oz) ^ 2)
      if leg_t >= leg_d then
        mode, queue, qi, wt, calm = "fight", sdef.waves[sdef.path[stop][4]], 1, 0, 0
        show_card({big = "ACTION!"}, 0.9)
      end
    elseif mode == "fight" then
      wt = wt + dt
      while queue[qi] and wt >= queue[qi][1] do
        if not spawn(queue[qi]) then break end
        qi = qi + 1
      end
      if not queue[qi] and not hostile() then
        calm = calm + dt
        if calm > 0.9 then
          for _, e in ipairs(MEN) do gone(e) end
          for _, e in ipairs(BARRELS) do gone(e) end
          gone(PU)
          if stop == #sdef.path then stage_clear() else start_leg() show_card({big = "GO!", col = 0x7cff9a}, 0.7) end
        end
      end
    end
  end

  -- the actors
  for _, e in ipairs(MEN) do if e.st > 0 then upd_man(e, dt) if e.st > 0 then put_man(e) end end end
  for _, e in ipairs(DRONES) do if e.st > 0 then upd_man(e, dt) if e.st > 0 then put_drone(e) end end end
  for _, o in ipairs(ORBS) do if o.st > 0 then upd_orb(o, dt) end end
  if BOSS.st > 0 then upd_boss(BOSS, dt) if BOSS.st > 0 then put_boss(BOSS) end end
  if PU.st > 0 then
    PU.t = PU.t + dt
    place(PU.o[1], PU.x, PU.y + sin(PU.t * 3) * 12, PU.z, 30, PU.t * 120, 0)
    if PU.t > 8 then gone(PU) end
  end
  for _, e in ipairs(BARRELS) do if e.st == 1 then place(e.o[1], e.x, 50, e.z) e.st = 2 end end
  for i, b in ipairs(BOOMS) do
    if b.st > 0 then
      b.t = b.t + dt
      paint(b.o[1], floor(b.t * 30) % 2 == 0 and 0xffa030 or 0xffe070, 0, true)
      if b.t > 0.22 then gone(b) end
    end
  end

  -- the camera: walking bob, recoil, the shake of a hit
  shake, recoil = max(0, shake - dt), max(0, recoil - dt * 8)
  hurt_t, dry_t = max(0, hurt_t - dt), max(0, dry_t - dt)
  CY = EYE + (mode == "move" and sin(walked * 0.03) * 3 or 0)
  local j = shake > 0 and shake * 8 or 0
  view(CX, CY, CZ, CYAW + (rnd(2) - 1) * j, recoil * 1.2 + (rnd(2) - 1) * j * 0.6, 60)
  put_ground()
end

local function ring(x, y, z, f, big)
  local sx, sy = project(x, y, z)
  if not sx then return end
  local r = 4 + (1 - f) * (big or 16)
  local col = f < 0.5 and 11 or f < 0.8 and 10 or 8
  circ(sx, sy, r, col)
  circ(sx, sy, r + 1, col)
  line(sx - r - 4, sy, sx - r - 1, sy, col)
  line(sx + r + 1, sy, sx + r + 4, sy, col)
  line(sx, sy - r - 4, sx, sy - r - 1, col)
  line(sx, sy + r + 1, sx, sy + r + 4, col)
end

function _draw()
  draw_hud()
  cls(0)
  clip(1, 0, 126, 128)  -- (the picture's edge columns run out to the glass's sides)
  for _, e in ipairs(MEN) do
    if e.st == 2 and e.aim then
      local x, y, z = man_pos(e)
      ring(x, y + 136, z, e.aim / e.aimd)
    end
    if e.st == 2 and e.mz > 0 then
      local x, y, z = man_pos(e)
      local sx, sy = project(x, y + 145, z)
      if sx then circfill(sx, sy, 3, 10) circfill(sx, sy, 1, 7) end
    end
  end
  for _, e in ipairs(DRONES) do
    if e.st == 2 and e.aim then
      local x, y, z = man_pos(e)
      ring(x, y + sin(e.t * 2.1) * 30, z, e.aim / e.aimd, 12)
    end
  end
  for _, o in ipairs(ORBS) do if o.st > 0 then ring(o.px, o.py, o.pz, o.t, 10) end end
  if PU.st > 0 then
    local sx, sy = project(PU.x, PU.y + 50, PU.z)
    local s = PICK[PU.kind][2]
    if sx and floor(PU.t * 4) % 2 == 0 then print(s, sx - #s * 2, sy - 6, 7) end
  end
  for i = #sparks, 1, -1 do
    local s = sparks[i]
    s.t = s.t + fdt
    if s.r then
      if s.t > 0.25 then table.remove(sparks, i)
      else circfill(s.x, s.y, min(22, s.r) * (0.5 + s.t * 2), s.t < 0.12 and 10 or 9) end
    elseif s.t > 0.12 then table.remove(sparks, i)
    else
      local r = 2 + s.t * 40
      circ(s.x, s.y, r, 10)
      pset(s.x, s.y, 7)
    end
  end
  for i = #pops, 1, -1 do
    local p = pops[i]
    p.t = p.t + fdt
    if p.t > 0.9 then table.remove(pops, i)
    else print(p.s, p.x - #p.s * 2, p.y - p.t * 16, p.col or 7) end
  end
  if hurt_t > 0 and floor(hurt_t * 20) % 2 == 0 then
    rectfill(1, 0, 126, 3, 8)
    rectfill(1, 124, 126, 127, 8)
    rectfill(1, 0, 4, 127, 8)
    rectfill(123, 0, 126, 127, 8)
  end
  clip()
end
