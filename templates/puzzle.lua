-- title: Puzzle
-- by: you
-- about: lights out: switch every light off
-- api: 4
-- canvas: full

-- sprites (the playground's sprite editor writes this table)
SPRITES = {
  [1] = "000a0000 000a0000 0aaaaa00 00aaa000 00a0a000 0a000a00 00000000 00000000", -- star
}
-- sounds (the playground's sound editor writes this table)
SOUNDS = {
  [8] = "02 0000 28345", -- click
  [9] = "06 0000 24150 28150 2b150 30155", -- solved
}

-- The whole screen: the 128x128 picture in the middle, and bands above and below it in the
-- full-color layer (hd) for the title and what to do. Drawn once; they stay.
local c = caps()
local W, H = c.w, c.h
local TOP, BOT = c.picture_y, c.picture_y + 384

local function bands(title, hint)
  hd.scanlines(false)
  hd.gradient(0, 0, W, TOP, 0x1b2440, 0x000000)
  hd.text(title, W / 2, TOP / 2 - 16, 0xffffff, 24, "bold", "center")
  hd.gradient(0, BOT, W, H - BOT, 0x000000, 0x1b2440)
  hd.text(hint, W / 2, BOT + 22, 0x9aa4b8, 14, "mono", "center")
end

-- Lights out: switching a light switches the four next to it too. Get them all off.
local N, CELL, X0, Y0 = 5, 22, 9, 14
local lit, cx, cy, moves, solved = {}, 3, 3, 0, false
local was_touching = false

local function flip(x, y)
  for _, d in ipairs({{0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}}) do
    local i, j = x + d[1], y + d[2]
    if i >= 1 and i <= N and j >= 1 and j <= N then lit[j][i] = not lit[j][i] end
  end
end

local function new_puzzle()  -- from all off, a few random presses: always solvable
  for j = 1, N do lit[j] = {} for i = 1, N do lit[j][i] = false end end
  for _ = 1, 8 do flip(flr(rnd(N)) + 1, flr(rnd(N)) + 1) end
  moves, solved = 0, false
end

function _init()
  fps(60, 10)  -- 60 while touched, 10 when nothing moves
  bands("PUZZLE", "tap a light  ·  switch them all off")
  new_puzzle()
end

function _update()
  local tx, ty = touch()
  if tx then  -- a finger: the cursor goes where it is (a tap then presses, as A)
    cx = mid(1, flr((tx - X0) / CELL) + 1, N)
    cy = mid(1, flr((ty - Y0) / CELL) + 1, N)
  elseif not was_touching then
    if btnp(0) then cx = mid(1, cx - 1, N) end
    if btnp(1) then cx = mid(1, cx + 1, N) end
    if btnp(2) then cy = mid(1, cy - 1, N) end
    if btnp(3) then cy = mid(1, cy + 1, N) end
  end
  was_touching = tx ~= nil
  if btnp(4) then
    if solved then new_puzzle() return end
    flip(cx, cy)
    moves = moves + 1
    sfx(8)
    local any = false
    for j = 1, N do for i = 1, N do any = any or lit[j][i] end end
    if not any then
      solved = true
      sfx(9)
      log("solved in", moves, "moves")
    end
  end
end

function _draw()
  cls(1)
  for j = 1, N do
    for i = 1, N do
      local x, y = X0 + (i - 1) * CELL, Y0 + (j - 1) * CELL
      rectfill(x + 1, y + 1, x + CELL - 2, y + CELL - 2, lit[j][i] and 10 or 5)
      if i == cx and j == cy then rect(x, y, x + CELL - 1, y + CELL - 1, 7) end
    end
  end
  print("moves " .. moves, 2, 2, 7)
  if solved then
    spr(1, 56, 4)
    print("solved: tap for another", 18, 122, 11)
  end
end
