-- title: Overdrive
-- by: geo
-- about: a chiptune in A minor at 150 BPM, made in the playground's sound editor; tap to fire
-- api: 3
-- canvas: full

-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "0c 0000 0c573 3f725 3f725 3f725 26765 3f725 0c573 3f725 0c573 3f725 3f725 0c573 26765 3f725 26735 3f725 0c573 3f725 3f725 3f725 26765 3f725 0c573 3f725 0c573 3f725 3f725 0c573 26765 3f725 26735 3f725", -- drums
  [9] = "0c 0000 0c573 3f725 3f725 3f725 26765 3f725 0c573 3f725 0c573 3f725 3f725 0c573 26765 3f725 26735 3f725 0c573 3f725 3f725 3f725 26765 3f725 0c573 3f725 0c573 3f725 26765 0c573 26765 26765 26765 26765", -- drums fill
  [10] = "0c 0000 15455 15455 21455 15455 15455 15455 21455 15455 15455 15455 21455 15455 15455 21455 15455 21455 11455 11455 1d455 11455 11455 11455 1d455 11455 11455 11455 1d455 11455 11455 1d455 11455 1d455", -- bass am f
  [11] = "0c 0000 18455 18455 24455 18455 18455 18455 24455 18455 18455 18455 24455 18455 18455 24455 18455 24455 13455 13455 1f455 13455 13455 13455 1f455 13455 13455 13455 1f455 13455 13455 1f455 13455 1f455", -- bass c g
  [12] = "0c 0000 28150 28150 28152 28152 26150 26150 24150 24150 26150 26150 26152 26152 28151 28150 28152 28152 29150 29150 28150 28150 24150 24150 24152 24152 21151 21150 21150 21150 21152 21152 21152 21152", -- lead a
  [13] = "0c 0000 2b150 2b150 2b152 2b152 28150 28150 24150 24150 28150 28150 28152 28152 2b151 2b150 2b152 2b152 26150 26150 23150 23150 1f150 1f150 1f152 1f152 23151 23150 23152 23152 26150 26150 26152 26152", -- lead b
  [14] = "0c 0000 21236 24236 28236 2d236 21236 24236 28236 2d236 21236 24236 28236 2d236 21236 24236 28236 2d236 1d236 21236 24236 29236 1d236 21236 24236 29236 1d236 21236 24236 29236 1d236 21236 24236 29236", -- shimmer am f
  [15] = "0c 0000 24236 28236 2b236 30236 24236 28236 2b236 30236 24236 28236 2b236 30236 24236 28236 2b236 30236 1f236 23236 26236 2b236 1f236 23236 26236 2b236 1f236 23236 26236 2b236 1f236 23236 26236 2b236", -- shimmer c g
  [16] = "0c 0000 2d150 2d150 2b150 2b150 28150 28150 28152 28152 2b150 2b150 2b152 2b152 2d151 2d150 2d152 2d152 30150 30150 2d150 2d150 29150 29150 29152 29152 2d151 2d150 2d152 2d152 30150 30150 30152 30152", -- lead a high
  [17] = "0c 0000 2b150 2b150 2d150 2d150 2b150 2b150 28150 28150 30151 30150 30150 30150 30152 30152 30152 30152 2f150 2f150 2f152 2f152 2b150 2b150 2b152 2b152 26150 26150 26152 26152 2b151 2b150 2b152 2b152", -- lead b high
  [18] = "02 0000 3c463 37443 30423", -- laser
}
-- the song (the playground's music editor writes this table)
MUSIC = {
  [0] = "00 08 0a -- --",
  [1] = "00 09 0b -- --",
  [2] = "01 08 0a 0c 0e",
  [3] = "00 08 0b 0d 0f",
  [4] = "00 08 0a 10 0e",
  [5] = "02 09 0b 11 0f",
}

local BEAT = 0.4         -- seconds, 150 BPM
local PATTERN = 3.2      -- seconds: 32 16ths
local c = caps()
local W, H = c.w, c.h
local TOP, BOT = c.picture_y, c.picture_y + 384  -- the bands above and below the picture
local PINK, SOFT, GOLD = 0xff4fa3, 0x9b8fb0, 0xffd04f
local start, flash = 0, 0
local bars = {}
local stars = {}

function _init()
  fps(30)
  for i = 1, 24 do stars[i] = {x = rnd(128), y = rnd(128), s = 0.5 + rnd(2)} end
  for i = 1, 16 do bars[i] = 0 end
  start = time()
  music(0)
  hd.scanlines(false)
  hd.gradient(0, 0, W, TOP, 0x2a0838, 0x000000)
  hd.text("OVERDRIVE", W / 2, TOP / 2 - 22, PINK, 32, "bold", "center")
  hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x2a0838)
  hd.text("tap to fire", W / 2, BOT + 34, SOFT, 14, "mono", "center")
end

-- Which part of the song is on (the song itself keeps the time; this follows the clock).
local function section(t)
  if t < 2 * PATTERN then return "INTRO" end
  return (t - 2 * PATTERN) % (4 * PATTERN) < 2 * PATTERN and "FULL THROTTLE" or "REDLINE"
end

function _update()
  if btnp(4) then
    sfx(18)
    flash = 1
  end
  flash = math.max(0, flash - 0.15)
  local t = time() - start
  local step = math.floor(t / (BEAT / 4))  -- the 16th the song is on
  for i = 1, 16 do
    local hit = (step + i * 3) % 16 < 3 and 1 or 0
    bars[i] = math.max(bars[i] * 0.8, hit * (0.4 + rnd(0.6)))
  end
  for _, s in ipairs(stars) do
    s.x = s.x - s.s * (1 + (t % BEAT < 0.1 and 3 or 0))
    if s.x < 0 then s.x, s.y = 128, rnd(128) end
  end
end

function _draw()
  local t = time() - start
  local beat = t % BEAT < 0.08
  cls(flash > 0.5 and 2 or 0)
  for _, s in ipairs(stars) do pset(s.x, s.y, s.s > 1.5 and 7 or 5) end
  -- the road: lines rushing toward you, faster on the beat
  for k = 0, 7 do
    local y = 70 + ((k * 8 + t * 40) % 58)
    line(0, y, 127, y, k % 2 == 0 and 2 or 13)
  end
  line(64, 70, 0, 127, 14)
  line(64, 70, 127, 127, 14)
  -- the sun, pulsing with the kick
  local r = beat and 22 or 20
  circfill(64, 60, r, 8)
  circfill(64, 60, r - 4, 9)
  for k = 0, 3 do rectfill(46, 52 + k * 5, 82, 53 + k * 5, 0) end
  -- the meters
  for i = 1, 16 do
    local h = math.floor(bars[i] * 30)
    rectfill(i * 8 - 6, 127 - h, i * 8 - 2, 127, i % 4 == 0 and 10 or 12)
  end
  local now = section(t)
  if ui.changed("section", now) then
    hd.repaint(0, BOT, W, 34, function()
      hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x2a0838)
      hd.text(now, W / 2, BOT + 8, now == "REDLINE" and GOLD or PINK, 18, "bold", "center")
    end)
  end
  if flash > 0 then
    for k = 1, 6 do line(64, 60, 64 + rnd(128) - 64, rnd(70), 7) end
  end
end
