-- title: Beat Grid
-- by: geo
-- about: a 16-step drum machine: tap a cell to switch it, B starts and stops
-- api: 3
-- canvas: full

-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "06 0000 0c573", -- kick
  [9] = "04 0000 26765 1f745", -- snare
  [10] = "02 0000 3f735", -- hat
  [11] = "06 0000 15455", -- bass
}

local ROWS = {
  {name = "kick", color = 8}, {name = "snare", color = 12},
  {name = "hat", color = 10}, {name = "bass", color = 11},
}
local STEP = 0.1              -- a 16th at 150 BPM: one frame at the idle rate (fps(60, 10))
local c = caps()
local W, H_ = c.w, c.h
local BAND, BOT = c.picture_y, c.picture_y + 384  -- the bands above and below the picture
local TOP, H = 26, 14         -- the grid's first row, and each row's height
local grid = {}               -- grid[row][step] = true when the cell plays
local cx, cy = 1, 1           -- the cursor
local playing, start, last = true, 0, -1
local was_touching = false

local function default()
  local rows = {"1000100010001000", "0000100000001000", "0101010101010101", "1001001000100100"}
  for r = 1, 4 do
    grid[r] = {}
    for s = 1, 16 do grid[r][s] = rows[r]:sub(s, s) == "1" end
  end
end

local function store()
  local t = {}
  for r = 1, 4 do
    local row = ""
    for s = 1, 16 do row = row .. (grid[r][s] and "1" or "0") end
    t[r] = row
  end
  save({rows = t})
end

local function bands()
  hd.scanlines(false)
  hd.gradient(0, 0, W, BAND, 0x10233a, 0x000000)
  hd.text("BEAT GRID", W / 2, BAND / 2 - 16, 0x7fd8ff, 24, "bold", "center")
  hd.gradient(0, BOT, W, H_ - BOT, 0x000000, 0x10233a)
  hd.text("tap a cell  ·  B starts, stops", W / 2, BOT + 22, 0x8aa0b8, 14, "mono", "center")
end

function _init()
  fps(60, 10)
  bands()
  default()
  local kept = load()
  if kept and kept.rows then
    for r = 1, 4 do
      for s = 1, 16 do grid[r][s] = kept.rows[r]:sub(s, s) == "1" end
    end
  end
  start = time()
end

function _update60()
  local tx, ty = touch()
  if tx then  -- a finger: the cursor goes where it is (a tap then switches the cell, as A)
    if ty >= TOP and ty < TOP + 4 * H then
      cx = mid(1, flr(tx / 8) + 1, 16)
      cy = mid(1, flr((ty - TOP) / H) + 1, 4)
    end
  elseif not was_touching then
    if btnp(0) then cx = cx > 1 and cx - 1 or 16 end
    if btnp(1) then cx = cx < 16 and cx + 1 or 1 end
    if btnp(2) then cy = cy > 1 and cy - 1 or 4 end
    if btnp(3) then cy = cy < 4 and cy + 1 or 1 end
  end
  was_touching = tx ~= nil
  if btnp(4) then
    grid[cy][cx] = not grid[cy][cx]
    if grid[cy][cx] then sfx(7 + cy) end
    store()
  end
  if btnp(5) then
    playing = not playing
    start, last = time(), -1
  end
  if playing then
    local step = flr((time() - start) / STEP) % 16
    if step ~= last then
      last = step
      for r = 1, 4 do
        if grid[r][step + 1] then sfx(7 + r) end
      end
    end
  end
end

function _draw()
  cls(0)
  print(playing and "150 bpm" or "stopped", 96, 3, playing and 11 or 5)
  for r = 1, 4 do
    local y = TOP + (r - 1) * H
    for s = 1, 16 do
      local x = (s - 1) * 8
      local on = grid[r][s]
      local c = on and ROWS[r].color or ((s - 1) % 4 == 0 and 5 or 1)
      if playing and s - 1 == last then c = on and 7 or 6 end
      rectfill(x + 1, y + 1, x + 6, y + H - 3, c)
    end
  end
  local x, y = (cx - 1) * 8, TOP + (cy - 1) * H
  rect(x, y, x + 7, y + H - 2, 7)
  for r = 1, 4 do
    local x = (r - 1) * 32
    rectfill(x + 2, 92, x + 5, 95, ROWS[r].color)
    print(ROWS[r].name, x + 8, 91, 6)
  end
end
