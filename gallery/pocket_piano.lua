-- title: Pocket Piano
-- by: geo
-- about: an octave to play with a finger or the keys; B plays a tune
-- api: 3
-- canvas: full

-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "08 0000 18660 18650 18645", -- c4
  [9] = "08 0000 19660 19650 19645", -- c#4
  [10] = "08 0000 1a660 1a650 1a645", -- d4
  [11] = "08 0000 1b660 1b650 1b645", -- d#4
  [12] = "08 0000 1c660 1c650 1c645", -- e4
  [13] = "08 0000 1d660 1d650 1d645", -- f4
  [14] = "08 0000 1e660 1e650 1e645", -- f#4
  [15] = "08 0000 1f660 1f650 1f645", -- g4
  [16] = "08 0000 20660 20650 20645", -- g#4
  [17] = "08 0000 21660 21650 21645", -- a4
  [18] = "08 0000 22660 22650 22645", -- a#4
  [19] = "08 0000 23660 23650 23645", -- b4
  [20] = "08 0000 24660 24650 24645", -- c5
  [21] = "0e 0000 28150 28150 29150 2b150 2b150 29150 28150 26150 24150 24150 26150 28150 28150 26150 26150 00000 28150 28150 29150 2b150 2b150 29150 28150 26150 24150 24150 26150 28150 26150 24150 24150 00000", -- ode melody
  [22] = "0e 0000 0c360 0c345 13340 13335 07360 07345 0e340 0e335 0c360 0c345 13340 13335 07360 07345 0e340 0e335 0c360 0c345 13340 13335 07360 07345 0e340 0e335 07360 07345 0e340 0e335 0c360 0c345 13340 13335", -- ode bass
}
-- the song (the playground's music editor writes this table)
MUSIC = {
  [0] = "00 15 16 -- --",
}

local WHITE = {1, 3, 5, 6, 8, 10, 12, 13}   -- the white keys among the 13 (C to C)
local BLACK = {2, 4, 7, 9, 11}
local X0, KW, KTOP, KBOT = 4, 15, 56, 120      -- the keyboard's left edge (clear of the picture's edge
local BW, BBOT = 9, 92                         -- columns, which carry on out to the glass); key sizes
local NAMES = {"c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b", "c"}
local down, sel, glow = nil, 1, {}
local tune = false
local sparks = {}

-- Where key k is: white keys side by side, a black one between its two whites.
local function key_x(k)
  for i, w in ipairs(WHITE) do if w == k then return X0 + (i - 1) * KW, KW, false end end
  for i, w in ipairs(WHITE) do if w == k - 1 then return X0 + i * KW - flr(BW / 2), BW, true end end
end

local function key_at(x, y)
  if y < KTOP or y > KBOT then return nil end
  if y <= BBOT then
    for _, k in ipairs(BLACK) do
      local kx, kw = key_x(k)
      if x >= kx and x < kx + kw then return k end
    end
  end
  return WHITE[mid(1, flr((x - X0) / KW) + 1, 8)]
end

local function play(k)
  sfx(7 + k)
  glow[k] = 1
  local kx, kw = key_x(k)
  for i = 1, 4 do
    sparks[#sparks + 1] = {x = kx + kw / 2, y = KTOP - 2, vx = rnd(2) - 1, vy = -1 - rnd(1.5), life = 1, c = 8 + k % 8}
  end
end

local c = caps()
local W, H = c.w, c.h
local BAND, BOT = c.picture_y, c.picture_y + 384

function _init()
  fps(60, 10)
  hd.scanlines(false)
  hd.gradient(0, 0, W, BAND, 0x2a1a08, 0x000000)
  hd.text("POCKET PIANO", W / 2, BAND / 2 - 16, 0xffd27a, 24, "bold", "center")
  hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x2a1a08)
  hd.text("slide along the keys  ·  B: a tune", W / 2, BOT + 22, 0xb8a080, 14, "mono", "center")
end

function _update60()
  local tx, ty = touch()
  local k = tx and key_at(tx, ty)
  if k and k ~= down then play(k) sel = k end  -- a finger sliding along plays each key it reaches
  down = k
  if not tx then
    if btnp(0) then sel = sel > 1 and sel - 1 or 13 end
    if btnp(1) then sel = sel < 13 and sel + 1 or 1 end
    if btnp(4) then play(sel) end
  end
  if btnp(5) then
    tune = not tune
    music(tune and 0 or -1)
  end
  for k, v in pairs(glow) do glow[k] = v > 0.05 and v * 0.9 or nil end
  for i = #sparks, 1, -1 do
    local s = sparks[i]
    s.x, s.y, s.vy, s.life = s.x + s.vx, s.y + s.vy, s.vy + 0.06, s.life - 0.02
    if s.life <= 0 then table.remove(sparks, i) end
  end
end

function _draw()
  cls(1)
  print(tune and "playing a tune: b stops it" or "b plays a tune", 2, 4, tune and 11 or 6)
  print(NAMES[sel], 116, 3, 10)
  for _, s in ipairs(sparks) do pset(s.x, s.y, s.c) end
  for i, k in ipairs(WHITE) do
    local x = X0 + (i - 1) * KW
    rectfill(x + 1, KTOP, x + KW - 1, KBOT, glow[k] and 10 or 7)
    if k == sel then rectfill(x + 4, KBOT - 6, x + KW - 4, KBOT - 4, 8) end
  end
  for _, k in ipairs(BLACK) do
    local x = key_x(k)
    rectfill(x, KTOP, x + BW - 1, BBOT, glow[k] and 9 or 0)
    if k == sel then rectfill(x + 2, BBOT - 6, x + BW - 3, BBOT - 4, 8) end
  end
end
