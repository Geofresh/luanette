-- title: Mochi
-- by: geo
-- about: a little pet that lives in real time: feed it, play, send it exploring, dress it up

-- Prototype. Stats drift by the hour; opening the cart catches up on the time it was closed, from
-- now() (the board's RTC, set by push.py). While the date is unknown it ages only while open.
-- Nobody dies here: neglect makes Mochi hungry, sad or sick, and care brings it back.

SPRITES = {
  "0000000000000000000990000999999088888888088888800088880000000000", -- 1 bowl (feed)
  "00cccc000cc7ccc0cc77cccccccccccccccccccc0cccccc000cccc0000000000", -- 2 ball (play)
  "000a0000000a00000a0a0a0000aaa000aaa7aaa000aaa0000a0a0a00000a0000", -- 3 sparkle (clean)
  "00aaaa000aa77aa00aa7aaa00aaaaaa000aaaa00000660000006600000055000", -- 4 bulb (lights)
  "0077770007788770777887777888888778888887777887770778877000777700", -- 5 cross (heal)
  "0000000000040000000440000044440004444440444444440000000000000000", -- 6 poop
  "0000000008808800888888808888888008888800008880000008000000000000", -- 7 heart
}

local ICONS = {
  {spr = 1, name = "FEED"}, {spr = 2, name = "PLAY"}, {spr = 3, name = "CLEAN"},
  {spr = 4, name = "LIGHT"}, {spr = 5, name = "HEAL"},
}
local BAR_Y, FLOOR_Y = 108, 92
local HOUR = 3600

-- Adventures: once a day (real time) Mochi can go exploring through the door and may bring back
-- something to wear. PLAYTEST: trips take 2 minutes and aren't limited to one a day.
local PLAYTEST = true
local TRIP = PLAYTEST and 120 or 4 * HOUR
local FIND = 0.7                       -- a trip brings something back this often
local TIERS = {0.6, 0.3, 0.1}          -- common, rare, legendary
local PLACES = {"THE MOSSY CAVES", "CANDY HILLS", "THE OLD CASTLE", "STARRY BEACH", "CLOUD TOWN",
                "THE MUSHROOM WOODS", "SNOWY PEAKS", "THE NIGHT MARKET"}
local DOOR = {x0 = 3, x1 = 21, y0 = 48}          -- left wall
local WARDROBE = {x0 = 103, x1 = 124, y0 = 50}   -- right wall, under the window

local S                      -- the saved pet
local t0, clock0             -- time() at start, and the wall clock then
local mode = "home"          -- home | eat | game | stats
local anim = 0               -- seconds left of the current action's animation
local hearts = {}            -- floating hearts {x, y, t}
local sparkles = {}
local msg, msg_t = nil, 0
local last_tx, last_ty, stroke_x, stroke_d = nil, nil, nil, 0
local pet_x, pet_vx = 64, 0
local blink = 2                -- seconds to the next blink (eyes open at the start)
local game                   -- the star game's state
local boops, boop_t = 0, 0
local save_t = 0

-- ---------- time ----------

-- Wall-clock seconds: now() once the board knows the date, else the saved clock plus time open.
local function wall()
  return now() or flr(clock0 + (time() - t0))
end

-- The local hour (for bedtime), or nil while the date is unknown.
local function hour()
  local d = date()
  return d and d.hour
end

local function night()
  local h = hour()
  return h ~= nil and (h >= 22 or h < 7)
end

-- ---------- drawing helpers and the wardrobe's items ----------

-- A filled ellipse, row by row.
local function blob(cx, cy, rx, ry, c)
  for dy = -ry, ry do
    local f = 1 - (dy / ry) ^ 2
    local hw = rx * math.sqrt(f > 0 and f or 0)
    if hw >= 0.7 then rectfill(cx - hw, cy + dy, cx + hw, cy + dy, c) end  -- (no lone pixel at the poles)
  end
end

local C = {}  -- the items: name, slot (head / face / neck), rarity (1-3), draw(x, y, r, t, back)
-- the head's width at a row (for bands that hug the body)
local function width_at(r, dy)
  local f = 1 - (dy / r) ^ 2
  return r * math.sqrt(f > 0 and f or 0)
end

-- ---------- cosmetics: each draws relative to the body (x, y center, r radius) ----------
-- geometry from pet.lua: eyes at y - 0.15 r, x +- 0.4 r; mouth 0.35 r below the eyes

local function eyes(x, y, r) return y - r * 0.15, r * 0.4 end

local C = {}

C.party = {name = "PARTY HAT", slot = "head", rarity = 1, draw = function(x, y, r)
  local top = y - r
  trifill(x + 3, top - 13, x - 6, top + 2, x + 9, top + 2, 14)
  line(x - 3, top - 2, x + 7, top - 3, 10)
  line(x - 1, top - 6, x + 6, top - 7, 10)
  line(x + 1, top - 10, x + 5, top - 10, 10)
  circfill(x + 3, top - 14, 2, 10)
end}

C.beanie = {name = "BEANIE", slot = "head", rarity = 1, draw = function(x, y, r)
  local top = y - r
  clip(0, 0, 128, top + 6)
  blob(x, y, r + 1, r + 1, 12)
  clip()
  local w = width_at(r + 1, r - 4)
  rectfill(x - w, top + 4, x + w, top + 6, 1)
  for i = -w + 2, w - 2, 3 do pset(x + i, top + 5, 12) end
  circfill(x, top - 1, 2, 7)
end}

C.flower = {name = "FLOWER", slot = "head", rarity = 1, draw = function(x, y, r)
  local fx, fy = x + r * 0.5, y - r + 1
  line(fx - 2, fy + 3, fx - 5, fy + 8, 3)
  pset(fx - 4, fy + 5, 11)
  for k = 0, 4 do
    local a = k / 5 + 0.05
    circfill(fx + cos(a) * 3.5, fy + sin(a) * 3.5, 2, 14)
  end
  circfill(fx, fy, 2, 10)
  pset(fx - 1, fy - 1, 7)
end}

C.chef = {name = "CHEF HAT", slot = "head", rarity = 2, draw = function(x, y, r)
  local top = y - r
  circfill(x - 6, top - 6, 4, 6) circfill(x + 6, top - 6, 4, 6) circfill(x, top - 8, 5, 6)
  circfill(x - 6, top - 6, 3, 7) circfill(x + 6, top - 6, 3, 7) circfill(x, top - 8, 4, 7)
  rectfill(x - 8, top - 4, x + 8, top + 2, 7)
  line(x - 8, top + 2, x + 8, top + 2, 6)
  line(x - 3, top - 3, x - 3, top + 1, 6) line(x + 3, top - 3, x + 3, top + 1, 6)
end}

C.phones = {name = "HEADPHONES", slot = "head", rarity = 2, draw = function(x, y, r)
  local ey = eyes(x, y, r)
  clip(0, 0, 128, ey - 1)
  circ(x, y, r + 2, 5)
  circ(x, y, r + 3, 5)
  clip()
  rectfill(x - r - 4, ey - 4, x - r + 1, ey + 3, 8)
  rectfill(x + r - 1, ey - 4, x + r + 4, ey + 3, 8)
  rectfill(x - r - 4, ey - 4, x - r - 3, ey + 3, 2)
  rectfill(x + r + 3, ey - 4, x + r + 4, ey + 3, 2)
end}

C.frog = {name = "FROG HAT", slot = "head", rarity = 2, draw = function(x, y, r)
  local top = y - r
  clip(0, 0, 128, top + 5)
  blob(x, y, r + 1, r + 1, 11)
  clip()
  for _, s in ipairs({-1, 1}) do
    local cx = x + s * 6
    circfill(cx, top - 1, 4, 11)
    circfill(cx, top - 2, 2, 7)
    pset(cx + s, top - 2, 0) pset(cx + s, top - 1, 0)
  end
  line(x - 4, top + 3, x + 4, top + 3, 3)
end}

C.wizard = {name = "WIZARD HAT", slot = "head", rarity = 2, draw = function(x, y, r)
  local top = y - r
  trifill(x + 2, top - 20, x - 9, top + 1, x + 10, top + 1, 2)
  line(x + 2, top - 20, x + 5, top - 17, 2)
  rectfill(x - 13, top + 1, x + 14, top + 2, 2)
  rectfill(x - 7, top - 2, x + 8, top - 1, 13)
  pset(x, top - 9, 10) pset(x + 3, top - 13, 10) pset(x - 3, top - 5, 10) pset(x + 5, top - 6, 7)
end}

C.crown = {name = "CROWN", slot = "head", rarity = 3, draw = function(x, y, r)
  local top = y - r
  rectfill(x - 8, top - 3, x + 8, top + 1, 10)
  trifill(x - 8, top - 3, x - 8, top - 9, x - 3, top - 3, 10)
  trifill(x - 4, top - 3, x, top - 11, x + 4, top - 3, 10)
  trifill(x + 3, top - 3, x + 8, top - 9, x + 8, top - 3, 10)
  line(x - 8, top + 1, x + 8, top + 1, 9)
  circfill(x, top - 1, 1, 8) pset(x - 5, top - 1, 12) pset(x + 5, top - 1, 12)
  pset(x, top - 11, 7) pset(x - 8, top - 9, 7) pset(x + 8, top - 9, 7)
end}

C.halo = {name = "HALO", slot = "head", rarity = 3, draw = function(x, y, r, t)
  local hy = y - r - 6 + sin(t * 0.5) * 1
  for dx = -8, 8 do
    local w = width_at(8, dx) * 0.3
    pset(x + dx, hy - w, 10) pset(x + dx, hy + w, 10)
  end
  pset(x - 9, hy, 10) pset(x + 9, hy, 10)
  pset(x - 4, hy - 3, 7) pset(x + 6, hy - 2, 7)
end}

C.glasses = {name = "GLASSES", slot = "face", rarity = 1, draw = function(x, y, r)
  local ey, ex = eyes(x, y, r)
  circ(x - ex, ey, 3, 0) circ(x + ex, ey, 3, 0)
  line(x - ex + 3, ey, x + ex - 3, ey, 0)
  line(x - ex - 3, ey - 1, x - r + 1, ey - 2, 0) line(x + ex + 3, ey - 1, x + r - 1, ey - 2, 0)
end}

local STAR = {"..#..", "#####", ".###.", ".#.#."}
C.stickers = {name = "STAR STICKERS", slot = "face", rarity = 1, draw = function(x, y, r)
  local ey, ex = eyes(x, y, r)
  for si, s in ipairs({-1, 1}) do
    local sx, sy = x + s * (ex + 4) - 2, ey + 2
    for row, line_ in ipairs(STAR) do
      for col = 1, 5 do
        if line_:sub(col, col) == "#" then pset(sx + col - 1, sy + row - 1, si == 1 and 10 or 14) end
      end
    end
  end
end}

C.shades = {name = "SUNGLASSES", slot = "face", rarity = 2, hides_eyes = true, draw = function(x, y, r)
  local ey, ex = eyes(x, y, r)
  rectfill(x - ex - 3, ey - 2, x - ex + 2, ey + 2, 0)
  rectfill(x + ex - 2, ey - 2, x + ex + 3, ey + 2, 0)
  line(x - ex + 2, ey - 1, x + ex - 2, ey - 1, 0)
  line(x - ex - 3, ey - 1, x - r + 1, ey - 2, 0) line(x + ex + 3, ey - 1, x + r - 1, ey - 2, 0)
  pset(x - ex - 2, ey - 1, 7) pset(x + ex - 1, ey - 1, 7)
end}

C.monocle = {name = "MONOCLE", slot = "face", rarity = 2, draw = function(x, y, r)
  local ey, ex = eyes(x, y, r)
  circ(x + ex, ey, 4, 10)
  circ(x + ex, ey, 5, 9)
  pset(x + ex - 2, ey - 2, 7)
  for i = 0, 9 do if i % 2 == 0 then pset(x + ex + 4 + i * 0.3, ey + 5 + i, 10) end end
end}

local function band_y(y, r) return y + r * 0.5 end

C.scarf = {name = "SCARF", slot = "neck", rarity = 1, draw = function(x, y, r)
  local by = band_y(y, r)
  local w = width_at(r, by - y) + 1
  rectfill(x - w, by - 1, x + w, by + 2, 8)
  for i = -w + 1, w - 1, 4 do line(x + i, by - 1, x + i, by + 2, 7) end
  rectfill(x + 4, by + 2, x + 8, by + 9, 8)
  line(x + 4, by + 5, x + 8, by + 5, 7)
  pset(x + 4, by + 10, 8) pset(x + 6, by + 10, 8) pset(x + 8, by + 10, 8)
end}

C.bowtie = {name = "BOW TIE", slot = "neck", rarity = 1, draw = function(x, y, r)
  local by = band_y(y, r) - 1
  trifill(x - 6, by - 3, x - 6, by + 3, x, by, 8)
  trifill(x + 6, by - 3, x + 6, by + 3, x, by, 8)
  rectfill(x - 1, by - 1, x + 1, by + 1, 2)
end}

C.bell = {name = "BELL", slot = "neck", rarity = 2, draw = function(x, y, r)
  local by = band_y(y, r)
  local w = width_at(r, by - y) + 1
  rectfill(x - w, by - 1, x + w, by, 8)
  circfill(x, by + 3, 3, 10)
  line(x - 3, by + 5, x + 3, by + 5, 9)
  pset(x, by + 4, 4)
  pset(x - 1, by + 2, 7)
end}

C.cape = {name = "CAPE", slot = "neck", rarity = 3, back = true, draw = function(x, y, r, t, back)
  if back then
    local sway = sin(t * 0.4) * 2
    -- a cloak behind the body: from collar points beside the head down to a hem past either side
    trifill(x - r - 3, y - r * 0.6, x - r - 9 + sway, FLOOR_Y, x, FLOOR_Y, 8)
    trifill(x + r + 3, y - r * 0.6, x + r + 9 + sway, FLOOR_Y, x, FLOOR_Y, 8)
    trifill(x - r - 3, y - r * 0.6, x + r + 3, y - r * 0.6, x, FLOOR_Y, 8)
    line(x - r - 3, y - r * 0.6, x - r - 9 + sway, FLOOR_Y, 2)
    line(x + r + 3, y - r * 0.6, x + r + 9 + sway, FLOOR_Y, 2)
    line(x - r - 9 + sway, FLOOR_Y, x + r + 9 + sway, FLOOR_Y, 2)
  else
    local by = band_y(y, r) - 2
    local w = width_at(r, by - y)
    line(x - w, by - 1, x - 2, by + 1, 8) line(x + 2, by + 1, x + w, by - 1, 8)
    circfill(x, by + 1, 1, 10)
    pset(x, by + 1, 7)
  end
end}

local ORDER = {"party", "beanie", "flower", "chef", "phones", "frog", "wizard", "crown", "halo",
               "glasses", "stickers", "shades", "monocle", "scarf", "bowtie", "bell", "cape"}
local RARITY = {{"COMMON", 6}, {"RARE", 12}, {"LEGENDARY", 10}}
local SLOT_ORDER = {neck = 1, face = 2, head = 3}

-- An outfit ({head = key, face = key, neck = key}) as items, drawn neck first, head last.
local function worn(outfit)
  local items = {}
  for _, k in pairs(outfit) do if C[k] then items[#items + 1] = C[k] end end
  table.sort(items, function(a, b) return SLOT_ORDER[a.slot] < SLOT_ORDER[b.slot] end)
  return items
end

local function hides_eyes(items)
  for _, it in ipairs(items) do if it.hides_eyes then return true end end
  return false
end

-- ---------- the pet ----------

local function clamp(v) return mid(0, v, 100) end

local function new_pet()
  local w = wall()
  return {v = 1, born = w, last = w, stage = "egg", food = 80, joy = 80, energy = 90, clean = 100,
          poops = 0, sick = false, asleep = false, lights = true, care = 0, poop_in = 3 * HOUR,
          neglect = 0, hatch_at = w + 90, fed = 0, played = 0, owned = {}, wear = {}, trips = 0}
end

local function age_days() return (wall() - S.born) / 86400 end

local function grow()
  local d = age_days()
  if S.stage == "egg" and wall() >= S.hatch_at then
    S.stage, msg, msg_t = "baby", "IT HATCHED!", 4
    sfx(7)
  elseif S.stage == "baby" and d >= 1 then
    S.stage, msg, msg_t = "child", "MOCHI GREW!", 4
  elseif S.stage == "child" and d >= 3 then
    S.stage, msg, msg_t = "adult", "ALL GROWN UP", 4
  end
end

-- `dt` seconds of life, in steps of at most an hour: hunger, boredom, mess, sleep.
local function live(dt)
  if S.stage == "egg" then return end
  while dt > 0 do
    local step = math.min(dt, HOUR)
    local h = step / HOUR
    if S.trip then  -- out exploring: snacks on the way, no mess at home
      S.food = clamp(S.food - 3 * h)
    elseif S.asleep then
      S.energy = clamp(S.energy + 18 * h)
      S.food = clamp(S.food - 2 * h)
      if S.energy >= 100 and not night() then S.asleep = false end
    else
      S.food = clamp(S.food - 7 * h)
      S.joy = clamp(S.joy - (S.food < 25 and 8 or 4) * h)
      S.energy = clamp(S.energy - 4 * h)
      S.poop_in = S.poop_in - step
      if S.poop_in <= 0 and S.poops < 4 then
        S.poops, S.poop_in = S.poops + 1, (2 + rnd(2)) * HOUR
      end
      if (not S.lights and night()) or S.energy < 10 then S.asleep = true end
    end
    S.clean = clamp(S.clean - 6 * S.poops * h)  -- gentle: a workday away leaves a mess, not a crisis
    -- An hour of real need (empty belly, filthy room, gloom) is a care mistake; adults remember.
    if S.food == 0 or S.clean < 20 or S.joy == 0 then
      S.neglect = S.neglect + h
      if S.neglect >= 1 then S.care, S.neglect = S.care + 1, 0 end
    end
    if not S.sick and (S.clean < 25 or S.food == 0) and rnd(1) < 0.25 * h then S.sick = true end
    dt = dt - step
  end
end

local function persist()
  S.last = wall()
  save(S)
end

-- ---------- actions ----------

local function say(text) msg, msg_t = text, 2.5 end

local function heart(x, y) hearts[#hearts + 1] = {x = x + rnd(10) - 5, y = y, t = 1.2} end

local function act(i)
  if S.stage == "egg" then say("IT'S STILL AN EGG") return end
  local name = ICONS[i].name
  if S.asleep and name ~= "LIGHT" then say("SHH... ZZZ") return end
  if name == "FEED" then
    if S.food >= 95 then say("FULL!") return end
    S.food, S.fed = clamp(S.food + 30), S.fed + 1
    mode, anim = "eat", 1.6
    sfx(1)
  elseif name == "PLAY" then
    if S.energy < 15 then say("TOO SLEEPY") return end
    game = {t = 15, stars = {}, caught = 0, x = 64, spawn = 0}
    mode = "game"
    sfx(5)
  elseif name == "CLEAN" then
    if S.poops == 0 and S.clean > 90 then say("ALREADY TIDY") return end
    for k = 1, 12 do sparkles[#sparkles + 1] = {x = rnd(128), y = FLOOR_Y - rnd(30), t = 0.4 + rnd(0.6)} end
    S.poops, S.clean = 0, 100
    sfx(3)
  elseif name == "LIGHT" then
    S.lights = not S.lights
    if not S.lights and (night() or S.energy < 60) then S.asleep = true end
    if S.lights then S.asleep = false end
  elseif name == "HEAL" then
    if not S.sick then say("NOT SICK") return end
    S.sick, S.joy = false, clamp(S.joy - 10)
    say("ALL BETTER")
    sfx(7)
  end
  persist()
end

-- ---------- adventures ----------

local door_armed = 0  -- a first tap on the door arms it for a few seconds; the second sends Mochi

local function today()
  local d = date()
  return d and d.year * 1000 + d.yday or flr(wall() / 86400)
end

local function owned_count()
  local n = 0
  for _ in pairs(S.owned) do n = n + 1 end
  return n
end

-- Why Mochi can't go now, or nil when it can.
local function trip_blocked()
  if S.stage == "egg" then return "IT'S STILL AN EGG" end
  if S.trip then return "ALREADY OUT" end
  if S.asleep then return "SHH... ZZZ" end
  if S.sick then return "TOO SICK TO GO" end
  if S.food < 30 then return "TOO HUNGRY" end
  if S.energy < 30 then return "TOO TIRED" end
  if S.joy < 30 then return "TOO GLUM TO GO" end
  if not PLAYTEST and S.trip_day == today() then return "BACK TOMORROW" end
end

-- What a trip brings back is decided when it leaves (and saved), so reopening can't change it.
local function roll_loot()
  if rnd(1) >= FIND then return false end
  local r, tier = rnd(1), 3
  if r < TIERS[1] then tier = 1 elseif r < TIERS[1] + TIERS[2] then tier = 2 end
  local pool = {}
  for _, k in ipairs(ORDER) do if C[k].rarity == tier then pool[#pool + 1] = k end end
  return rnd(pool)
end

local function send_trip()
  S.trip = {back = wall() + TRIP, loot = roll_loot(), place = flr(rnd(#PLACES)) + 1}
  S.trip_day, S.trips = today(), (S.trips or 0) + 1
  S.energy = clamp(S.energy - 15)
  say("OFF EXPLORING!")
  sfx(3)
  persist()
end

local function trip_back()
  local tr = S.trip
  S.trip = nil
  local card = {place = tr.place}
  if tr.loot and S.owned[tr.loot] then
    card.dup, S.joy = tr.loot, clamp(S.joy + 15)
  elseif tr.loot then
    card.new = tr.loot
    S.owned[tr.loot] = true
  else
    S.joy = clamp(S.joy + 10)  -- no treasure, but it had a lovely time
  end
  S.postcard = card
  sfx(7)
  persist()
end

local function door_tap()
  local why = trip_blocked()
  if why then say(why) return end
  if door_armed > 0 then
    door_armed = 0
    send_trip()
  else
    door_armed = 3
    say("TAP AGAIN TO GO EXPLORING")
  end
end

-- The wardrobe's grid: 6 columns of 21 x 31 px cells from y 13 (tall: hats need the room), one an item.
local GRID_X, GRID_Y, CELL_W, CELL_H, COLS = 1, 13, 21, 31, 6

local function wardrobe_tap(x, y)
  if y >= BAR_Y or y < 11 then mode = "home" return end
  local col, row = flr((x - GRID_X) / CELL_W), flr((y - GRID_Y) / CELL_H)
  if col < 0 or col >= COLS or row < 0 then return end
  local k = ORDER[row * COLS + col + 1]
  if not k then return end
  if not S.owned[k] then say("NOT FOUND YET") return end
  local slot = C[k].slot
  S.wear[slot] = S.wear[slot] ~= k and k or nil
  sfx(5)
  persist()
end

-- ---------- the star game ----------

local function game_update(dt)
  local g = game
  g.t = g.t - dt
  local ax = tilt()
  if last_tx and touch() then g.x = g.x + (last_tx - g.x) * 0.3 else g.x = g.x + ax * 3 end
  g.x = mid(10, g.x, 118)
  g.spawn = g.spawn - dt
  if g.spawn <= 0 and g.t > 1 then
    g.stars[#g.stars + 1] = {x = 8 + rnd(112), y = 12, v = 30 + rnd(30)}
    g.spawn = 0.45 + rnd(0.4)
  end
  for i = #g.stars, 1, -1 do
    local s = g.stars[i]
    s.y = s.y + s.v * dt
    if s.y > FLOOR_Y - 14 and abs(s.x - g.x) < 10 then
      g.caught = g.caught + 1
      heart(g.x, FLOOR_Y - 20)
      sfx(1)
      table.remove(g.stars, i)
    elseif s.y > FLOOR_Y then
      table.remove(g.stars, i)
    end
  end
  if g.t <= 0 then
    S.joy = clamp(S.joy + 8 + g.caught * 3)
    S.energy = clamp(S.energy - 8)
    S.played = S.played + 1
    say("CAUGHT " .. g.caught .. "!")
    mode, game = "home", nil
    persist()
  end
end

-- ---------- update ----------

local prev = 0

function _init()
  t0 = time()
  local s = load()
  if s and s.v == 1 then
    S = s
    S.owned, S.wear = S.owned or {}, S.wear or {}
    clock0 = now() or S.last
    live(wall() - S.last)  -- the time it was closed (0 without now())
    grow()
    if S.trip and wall() >= S.trip.back then trip_back() end
  else
    clock0 = 0
    S = new_pet()
    say("TAP THE EGG")
  end
  prev = time()
end

function _update()
  local tnow = time()
  local dt = tnow - prev
  prev = tnow
  live(dt)
  grow()

  if S.trip and wall() >= S.trip.back then trip_back() end
  door_armed = math.max(0, door_armed - dt)

  local tx, ty = touch()
  if mode == "game" then
    if tx then last_tx, last_ty = tx, ty end
    game_update(dt)
  elseif S.postcard then
    -- The postcard waits for a tap (which also puts a new find on); BOOT just closes it.
    if btnp(4) and last_ty then
      local k = S.postcard.new
      if k then S.wear[C[k].slot] = k end
      S.postcard = nil
      persist()
    elseif btnp(5) then
      S.postcard = nil
      persist()
    end
    if tx then last_tx, last_ty = tx, ty end
  elseif mode == "wardrobe" then
    if btnp(4) and last_ty then wardrobe_tap(last_tx, last_ty) end
    if btnp(5) then mode = "home" end
    if tx then last_tx, last_ty = tx, ty end
  else
    -- A tap: the action bar, the top bar (stats), or the pet itself (a boop).
    if btnp(4) and last_ty then
      if last_ty >= BAR_Y then
        act(mid(1, flr(last_tx / (128 / #ICONS)) + 1, #ICONS))
      elseif last_ty < 11 then
        mode = mode == "stats" and "home" or "stats"
      elseif last_tx <= DOOR.x1 + 2 and last_ty >= DOOR.y0 and last_ty < FLOOR_Y then
        door_tap()
      elseif last_tx >= WARDROBE.x0 - 2 and last_ty >= WARDROBE.y0 and last_ty < FLOOR_Y then
        mode = "wardrobe"
        sfx(5)
      elseif S.trip then
        -- nobody home
      elseif abs(last_tx - pet_x) < 22 and S.stage ~= "egg" and not S.asleep then
        boops = boops + 1
        boop_t = 1
        if boops > 5 then say("HEY!") else heart(pet_x, 50) S.joy = clamp(S.joy + 1) end
        anim = math.max(anim, 0.4)
        sfx(0)
      elseif S.stage == "egg" then
        S.hatch_at = S.hatch_at - 15  -- warm it: taps hatch it sooner
        anim = 0.3
      elseif mode == "stats" then
        mode = "home"
      end
    end
    -- A stroke across the pet: petting.
    if tx and ty > 30 and ty < FLOOR_Y and abs(tx - pet_x) < 26 and S.stage ~= "egg" and not S.trip then
      if stroke_x then stroke_d = stroke_d + abs(tx - stroke_x) end
      stroke_x = tx
      if stroke_d > 40 then
        stroke_d = 0
        S.joy = clamp(S.joy + 2)
        heart(pet_x, 46)
        if S.asleep then say("PURR...") end
      end
    else
      stroke_x, stroke_d = nil, 0
    end
    if tx then last_tx, last_ty = tx, ty end
    if btnp(5) then mode = mode == "stats" and "home" or "stats" end
  end
  if not tx and not btnp(4) then last_tx, last_ty = nil, nil end

  -- Tilt: Mochi slides about (and settles back) unless asleep.
  local ax = tilt()
  if not S.asleep and mode ~= "game" then
    pet_vx = pet_vx * 0.9 + ax * 0.6 + (64 - pet_x) * 0.01
    pet_x = mid(36, pet_x + pet_vx, 90)  -- between the door and the wardrobe
  end

  anim = math.max(0, anim - dt)
  if anim == 0 and mode == "eat" then mode = "home" end
  msg_t = math.max(0, msg_t - dt)
  boop_t = math.max(0, boop_t - dt)
  if boop_t == 0 then boops = math.max(0, boops - dt * 0.5) end
  blink = blink - dt
  if blink < -0.15 then blink = 2 + rnd(3) end
  for i = #hearts, 1, -1 do
    local h = hearts[i]
    h.t, h.y = h.t - dt, h.y - 18 * dt
    if h.t <= 0 then table.remove(hearts, i) end
  end
  for i = #sparkles, 1, -1 do
    local s = sparkles[i]
    s.t = s.t - dt
    if s.t <= 0 then table.remove(sparkles, i) end
  end

  -- Call for attention when the screen is dark: light it when food will run low.
  if S.trip then
    wake(math.max(1, S.trip.back - wall()))  -- light the screen when it gets home
  elseif S.stage ~= "egg" and not S.asleep then
    wake(math.max(60, (S.food - 20) / 7 * HOUR))
  end
  save_t = save_t + dt
  if save_t > 20 then save_t = 0 persist() end
end

-- ---------- drawing ----------

local SIZE = {baby = 10, child = 14, adult = 18}

local function body_color()
  if S.sick then return 11, 3 end
  if S.stage == "adult" then
    if S.care <= 2 then return 14, 2 end   -- well loved: pink
    if S.care <= 6 then return 15, 4 end   -- fine: peach
    return 6, 5                            -- grumpy gray
  end
  return 15, 4
end

local function draw_egg(t)
  local wob = anim > 0 and sin(t * 6) * 2 or sin(t * 0.5) * 0.6
  local x, y = 64 + wob, FLOOR_Y - 16
  blob(x, y, 11, 14, 6)
  blob(x, y - 1, 10, 13, 7)
  circfill(x - 4, y - 4, 2, 13)
  circfill(x + 4, y + 3, 3, 12)
  circfill(x + 3, y - 8, 1, 14)
  local left = S.hatch_at - wall()
  if left < 30 then  -- cracks
    line(x - 6, y - 2, x - 2, y + 1, 5)
    line(x - 2, y + 1, x + 1, y - 3, 5)
    line(x + 1, y - 3, x + 5, y, 5)
  end
end

local function draw_pet(t)
  local r = SIZE[S.stage] or 10
  local bounce = 0
  if S.asleep then bounce = sin(t * 0.3) * 0.8
  elseif mode == "eat" then bounce = abs(sin(t * 3)) * -3
  elseif boop_t > 0 then bounce = -abs(sin(boop_t * 2)) * 8
  else bounce = abs(sin(t * 0.8)) * -2 end
  local squash = 1 + bounce * -0.04
  local x, y = pet_x, FLOOR_Y - r + bounce
  local c, edge = body_color()
  local items = worn(S.wear)
  for _, it in ipairs(items) do if it.back then it.draw(x, y, r, t, true) end end
  -- shadow, outline, body
  rectfill(x - r + 2, FLOOR_Y + 1, x + r - 2, FLOOR_Y + 2, 5)
  blob(x, y, r * squash + 1, r / squash + 1, edge)
  blob(x, y, r * squash, r / squash, c)
  if S.stage == "adult" and S.care <= 2 and not S.wear.head then  -- a little star antenna
    line(x, y - r, x, y - r - 5, edge)
    circfill(x, y - r - 6, 2, 10)
  end
  -- face
  local ey, ex = y - r * 0.15, r * 0.4
  if hides_eyes(items) then
    -- behind the shades
  elseif S.asleep then
    line(x - ex - 2, ey, x - ex + 1, ey, 0)
    line(x + ex - 1, ey, x + ex + 2, ey, 0)
  elseif S.sick then
    line(x - ex - 1, ey - 1, x - ex + 1, ey + 1, 0) line(x - ex - 1, ey + 1, x - ex + 1, ey - 1, 0)
    line(x + ex - 1, ey - 1, x + ex + 1, ey + 1, 0) line(x + ex - 1, ey + 1, x + ex + 1, ey - 1, 0)
  elseif blink > 0 then
    if S.joy > 60 or boop_t > 0 then  -- happy ^ ^
      line(x - ex - 2, ey + 1, x - ex, ey - 1, 0) line(x - ex, ey - 1, x - ex + 2, ey + 1, 0)
      line(x + ex - 2, ey + 1, x + ex, ey - 1, 0) line(x + ex, ey - 1, x + ex + 2, ey + 1, 0)
    else
      rectfill(x - ex - 1, ey - 1, x - ex, ey + 1, 0)
      rectfill(x + ex, ey - 1, x + ex + 1, ey + 1, 0)
    end
  else
    line(x - ex - 1, ey, x - ex + 1, ey, 0)
    line(x + ex - 1, ey, x + ex + 1, ey, 0)
  end
  rectfill(x - ex - 3, ey + 3, x - ex - 2, ey + 3, 14)  -- blush
  rectfill(x + ex + 2, ey + 3, x + ex + 3, ey + 3, 14)
  local my = ey + r * 0.35
  if mode == "eat" then circfill(x, my, 1 + flr(t * 6) % 2, 8)
  elseif S.food < 25 or S.joy < 25 or S.sick then line(x - 2, my + 1, x, my, 0) line(x, my, x + 2, my + 1, 0)
  elseif not S.asleep then line(x - 2, my, x, my + 1, 0) line(x, my + 1, x + 2, my, 0) end
  if S.asleep then
    local z = flr(t * 1.5) % 3
    print("Z", x + r, y - r - z * 3, 6)
    if z > 0 then print("z", x + r + 5, y - r - 6 - z * 2, 6) end
  end
  for _, it in ipairs(items) do it.draw(x, y, r, t, false) end
  if mode == "eat" then
    spr(1, x + r + 2, FLOOR_Y - 8)
    if flr(t * 8) % 2 == 0 then pset(x + r + rnd(6), FLOOR_Y - 9 - rnd(3), 9) end
  end
end

-- A small happy Mochi in an outfit (the postcard, the wardrobe's cells).
local function mini(x, y, r, outfit, t)
  local items = worn(outfit)
  for _, it in ipairs(items) do if it.back then it.draw(x, y, r, t, true) end end
  local c, edge = body_color()
  blob(x, y, r + 1, r + 1, edge)
  blob(x, y, r, r, c)
  local ey, ex = y - r * 0.15, r * 0.4
  if not hides_eyes(items) then
    line(x - ex - 1, ey + 1, x - ex, ey, 0) line(x - ex, ey, x - ex + 1, ey + 1, 0)
    line(x + ex - 1, ey + 1, x + ex, ey, 0) line(x + ex, ey, x + ex + 1, ey + 1, 0)
  end
  pset(x - ex - 2, ey + 2, 14) pset(x + ex + 2, ey + 2, 14)
  for _, it in ipairs(items) do it.draw(x, y, r, t, false) end
end

local function bar(x, y, label, v, c)
  print(label, x, y, 6)
  rect(x + 26, y, x + 77, y + 4, 5)
  if v > 0 then rectfill(x + 27, y + 1, x + 27 + v / 2, y + 3, v < 25 and 8 or c) end
end

local function draw_room(t)
  local dark = not S.lights
  cls(dark and 0 or 1)
  if not dark then
    rectfill(0, 11, 127, 30, 13)  -- a soft band of wall
    rectfill(96, 18, 116, 34, 12) -- a window
    rect(96, 18, 116, 34, 7)
    line(106, 18, 106, 34, 7)
    if night() then circfill(111, 23, 2, 10) else circfill(101, 23, 3, 10) end
  else
    for i = 0, 6 do pset((i * 37 + flr(t)) % 128, 14 + (i * 23) % 60, 5) end
  end
  -- the door (open while Mochi's out) and the wardrobe
  local d, w = DOOR, WARDROBE
  rectfill(d.x0 - 1, d.y0 - 1, d.x1 + 1, FLOOR_Y, 5)
  if S.trip then
    rectfill(d.x0, d.y0, d.x1, FLOOR_Y - 1, 0)
    for i = 0, 3 do pset(d.x0 + 3 + i * 4, d.y0 + 6 + (i * 7) % 20, 10) end  -- the world outside
    rectfill(d.x1 - 4, d.y0, d.x1, FLOOR_Y - 1, 4)                           -- the door, swung open
  else
    rectfill(d.x0, d.y0, d.x1, FLOOR_Y - 1, dark and 2 or 4)
    rect(d.x0 + 3, d.y0 + 4, d.x1 - 3, d.y0 + 18, dark and 1 or 2)
    rect(d.x0 + 3, d.y0 + 22, d.x1 - 3, FLOOR_Y - 5, dark and 1 or 2)
    circfill(d.x1 - 4, d.y0 + 24, 1, door_armed > 0 and 7 or 10)
  end
  rectfill(w.x0, w.y0, w.x1, FLOOR_Y - 1, dark and 1 or 2)
  rect(w.x0, w.y0, w.x1, FLOOR_Y - 1, dark and 0 or 13)
  line((w.x0 + w.x1) / 2, w.y0 + 1, (w.x0 + w.x1) / 2, FLOOR_Y - 2, dark and 0 or 13)
  pset((w.x0 + w.x1) / 2 - 2, w.y0 + 18, 10) pset((w.x0 + w.x1) / 2 + 2, w.y0 + 18, 10)
  rectfill(0, FLOOR_Y, 127, BAR_Y - 2, dark and 1 or 4)
  for i = 1, S.poops do spr(6, 26 + (i - 1) * 12, FLOOR_Y - 7) end
end

-- "BACK IN 1:42" / "BACK IN 3H 20M"
local function back_in()
  local s = math.max(0, S.trip.back - wall())
  if s < HOUR then return string.format("BACK IN %d:%02d", flr(s / 60), flr(s % 60)) end
  return string.format("BACK IN %dH %02dM", flr(s / HOUR), flr(s % HOUR / 60))
end

local function draw_postcard(t)
  local p = S.postcard
  rectfill(8, 14, 119, 104, 7)
  rect(8, 14, 119, 104, 13)
  rect(10, 16, 117, 102, 6)
  print("POSTCARD FROM", 38, 20, 13)
  local place = PLACES[p.place] or "FAR AWAY"
  print(place, 64 - #place * 2, 28, 1)
  local k = p.new or p.dup
  if k then
    local it = C[k]
    local rr = RARITY[it.rarity]
    mini(64, 68, 11, {[it.slot] = k}, t)  -- low enough for a tall hat under the place's name
    local line1 = (p.new and "FOUND: " or "ANOTHER ") .. it.name
    print(line1, 64 - #line1 * 2, 83, 0)
    print(rr[1], 64 - #rr[1] * 2, 90, rr[2] == 6 and 5 or rr[2])
    local foot = p.new and "TAP TO WEAR IT" or "IT GAVE IT AWAY: +JOY"
    print(foot, 64 - #foot * 2, 97, 13)
  else
    mini(64, 66, 11, S.wear, t)
    print("NO TREASURE TODAY,", 28, 82, 0)
    print("BUT IT HAD FUN!", 34, 89, 0)
    print("TAP TO CLOSE", 40, 96, 13)
  end
end

local function draw_wardrobe(t)
  cls(1)
  rectfill(0, 0, 127, 9, 0)
  print("WARDROBE", 2, 2, 14)
  local n = owned_count()
  print(n .. "/" .. #ORDER .. " FOUND", 126 - #(n .. "/" .. #ORDER .. " FOUND") * 4, 2, 6)
  for i, k in ipairs(ORDER) do
    local col, row = (i - 1) % COLS, flr((i - 1) / COLS)
    local x0, y0 = GRID_X + col * CELL_W, GRID_Y + row * CELL_H
    local it = C[k]
    local on = S.wear[it.slot] == k
    rectfill(x0 + 1, y0 + 1, x0 + CELL_W - 2, y0 + CELL_H - 2, S.owned[k] and 2 or 0)
    if S.owned[k] then
      clip(x0 + 1, y0 + 1, CELL_W - 2, CELL_H - 2)
      mini(x0 + CELL_W / 2, y0 + CELL_H - 9, 6, {[it.slot] = k}, t)
      clip()
    else
      print("?", x0 + CELL_W / 2 - 1, y0 + CELL_H / 2 - 3, RARITY[it.rarity][2] == 6 and 5 or RARITY[it.rarity][2])
    end
    rect(x0, y0, x0 + CELL_W - 1, y0 + CELL_H - 1, on and 10 or (S.owned[k] and 13 or 5))
  end
  rectfill(0, BAR_Y - 1, 127, 127, 0)
  line(0, BAR_Y - 1, 127, BAR_Y - 1, 5)
  print("TAP AN ITEM: WEAR / TAKE OFF", 8, BAR_Y + 4, 6)
  print("TAP HERE TO CLOSE", 30, BAR_Y + 12, 13)
end

local function draw_bar()
  rectfill(0, BAR_Y - 1, 127, 127, 0)
  line(0, BAR_Y - 1, 127, BAR_Y - 1, 5)
  local w = 128 / #ICONS
  for i, ic in ipairs(ICONS) do
    local x = flr((i - 1) * w + w / 2 - 4)
    local off = S.asleep and ic.name ~= "LIGHT"
    if off then pal(10, 5) pal(12, 5) pal(8, 5) pal(9, 5) pal(7, 6) end
    spr(ic.spr, x, BAR_Y + 3)
    pal()
    print(ic.name, x + 4 - #ic.name * 2, BAR_Y + 13, off and 5 or 6)
  end
end

local function draw_top()
  rectfill(0, 0, 127, 9, 0)
  print("MOCHI", 2, 2, 14)
  local label = S.stage == "egg" and "EGG" or (S.stage:upper() .. " DAY " .. flr(age_days()) + 1)
  print(label, 26, 2, 6)
  local d = date()
  if d then print(string.format("%d:%02d", (d.hour - 1) % 12 + 1, d.min), 106, 2, 6) end
  -- the neediest stat, at a glance
  if S.stage ~= "egg" then
    local need = math.min(S.food, S.joy, S.energy, S.clean)
    if S.sick then print("!", 98, 2, 11) elseif need < 25 then print("!", 98, 2, 8) end
  end
end

local function draw_game(t)
  local g = game
  cls(1)
  for i = 0, 12 do pset((i * 41) % 128, (i * 29 + flr(t * 4)) % 90 + 10, 13) end
  rectfill(0, FLOOR_Y, 127, BAR_Y - 2, 3)
  for _, s in ipairs(g.stars) do
    circfill(s.x, s.y, 2, 10)
    pset(s.x, s.y - 3, 10) pset(s.x, s.y + 3, 10) pset(s.x - 3, s.y, 10) pset(s.x + 3, s.y, 10)
  end
  local c, edge = body_color()
  blob(g.x, FLOOR_Y - 8, 9, 8, edge)
  blob(g.x, FLOOR_Y - 8, 8, 7, c)
  rectfill(g.x - 4, FLOOR_Y - 10, g.x - 3, FLOOR_Y - 9, 0)
  rectfill(g.x + 3, FLOOR_Y - 10, g.x + 4, FLOOR_Y - 9, 0)
  rectfill(0, 0, 127, 9, 0)
  print("CATCH THE STARS", 2, 2, 10)
  print(flr(g.caught) .. "  " .. math.max(0, flr(g.t)) .. "S", 96, 2, 7)
  rectfill(0, BAR_Y - 1, 127, 127, 0)
  print("TILT OR DRAG TO MOVE", 24, BAR_Y + 8, 6)
end

function _draw()
  local t = time()
  if mode == "game" then
    draw_game(t)
  elseif mode == "wardrobe" then
    draw_wardrobe(t)
  else
    draw_room(t)
    if S.stage == "egg" then draw_egg(t)
    elseif S.trip then
      local note = back_in()
      print("OUT EXPLORING", 38, 62, 7)
      print(note, 64 - #note * 2, 70, 6)
    else draw_pet(t) end
    for _, s in ipairs(sparkles) do pset(s.x, s.y, 10) pset(s.x + 1, s.y, 7) end
    draw_top()
    draw_bar()
    if mode == "stats" then
      rectfill(14, 24, 113, 82, 0)
      rect(14, 24, 113, 82, 13)
      bar(20, 30, "FOOD", S.food, 11)
      bar(20, 40, "JOY", S.joy, 14)
      bar(20, 50, "ENERGY", S.energy, 12)
      bar(20, 60, "CLEAN", S.clean, 10)
      print("CARE MISSES " .. S.care, 20, 71, 6)
    end
    if S.postcard then draw_postcard(t) end
  end
  for _, h in ipairs(hearts) do spr(7, h.x - 4, h.y) end
  if msg_t > 0 and msg then
    local w = #msg * 4
    rectfill(62 - w / 2, FLOOR_Y + 3, 66 + w / 2, FLOOR_Y + 11, 0)  -- on the floor: clear of the face and the stats
    print(msg, 64 - w / 2 + 1, FLOOR_Y + 5, 7)
  end
end
