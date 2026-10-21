-- title: Blank
-- by: you
-- about: a sprite to move: a place to start
-- api: 4
-- canvas: full

-- sprites (the playground's sprite editor writes this table)
SPRITES = {
  [1] = "00777700 07777770 77177177 77777777 77777777 77177177 07711770 00777700", -- face
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

local x, y = 60, 60   -- the sprite's top left, in picture pixels (0 to 127)

function _init()
  fps(60, 10)          -- 60 while touched, 10 when nothing moves: a still screen costs nothing
  bands("BLANK", "drag or arrow keys to move")
  log("started")       -- shows in the playground's Console
end

function _update()
  if btn(0) then x = x - 1 end
  if btn(1) then x = x + 1 end
  if btn(2) then y = y - 1 end
  if btn(3) then y = y + 1 end
  x, y = mid(0, x, 120), mid(0, y, 120)
end

function _draw()
  cls(1)
  spr(1, x, y)
end
