-- The runtime's helpers for the full-color layer, loaded before every cart (docs/api.md, "ui").
-- Written on the cart API itself, so check.py runs this same file and the board and the desktop
-- can't disagree about it. The layer keeps what's drawn; these make drawing only what changed the
-- easy way to write a screen:
--   hd.repaint(x, y, w, h, fn, ...)  draw part of the screen again, everything kept inside a box
--   ui.changed(key, ...)             true when these values differ from the last ones given for key
--   ui.scroller(x, y, w, h, draw)    a box whose content scrolls by moving what's drawn

ui = {}

local max, min, floor = math.max, math.min, math.floor
local raw_clip = hd.clip
local bx0, by0, bx1, by1 = nil, nil, nil, nil  -- the repaint box, while one runs
local cx0, cy0, cx1, cy1 = nil, nil, nil, nil  -- the clip in force (nil: none), to put back after one

local function apply(x0, y0, x1, y1)
  cx0, cy0, cx1, cy1 = x0, y0, x1, y1
  if x0 then raw_clip(x0, y0, max(0, x1 - x0), max(0, y1 - y0)) else raw_clip() end
end

local function number(v, what)
  if type(v) ~= "number" then error("hd.clip: " .. what .. " must be a number", 3) end
  return v
end

-- hd.clip as before, kept inside the box while a repaint runs (hd.clip() goes back to the box).
function hd.clip(x, y, w, h)
  if x == nil then
    if bx0 then apply(bx0, by0, bx1, by1) else apply() end
    return
  end
  x, y, w, h = number(x, "x"), number(y, "y"), number(w, "w"), number(h, "h")
  local x0, y0, x1, y1 = x, y, x + w, y + h
  if bx0 then x0, y0, x1, y1 = max(x0, bx0), max(y0, by0), min(x1, bx1), min(y1, by1) end
  apply(x0, y0, x1, y1)
end

-- hd.repaint(x, y, w, h, fn, ...): fn(...), its drawing kept inside the box (repaints nest: the
-- inner box is cut to the outer one). fn is the code that draws the screen (or the part there);
-- it should paint the box's background too, since the old pixels are still there. Not called
-- when the box is empty. The clip is put back as it was.
function hd.repaint(x, y, w, h, fn, ...)
  local ox0, oy0, ox1, oy1 = bx0, by0, bx1, by1
  local sx0, sy0, sx1, sy1 = cx0, cy0, cx1, cy1
  local x0, y0, x1, y1 = x, y, x + w, y + h
  if ox0 then x0, y0, x1, y1 = max(x0, ox0), max(y0, oy0), min(x1, ox1), min(y1, oy1) end
  if x1 <= x0 or y1 <= y0 then return end
  bx0, by0, bx1, by1 = x0, y0, x1, y1
  apply(x0, y0, x1, y1)
  local ok, err = pcall(fn, ...)
  bx0, by0, bx1, by1 = ox0, oy0, ox1, oy1
  apply(sx0, sy0, sx1, sy1)
  if not ok then error(err, 0) end
end

-- ui.changed(key, ...): true the first time, and whenever the values differ from the ones given
-- for this key last time (compared one by one, with ==). Ask each frame about what a part of the
-- screen shows, and repaint it when the answer is true:
--   if ui.changed("score", home, away) then hd.repaint(0, 40, W, 60, draw_score) end
-- Ask about every part every frame, even one about to be drawn anyway (a whole screen drawn
-- again), so that the remembered values stay those on the screen.
local seen = {}
function ui.changed(key, ...)
  local n, old = select("#", ...), seen[key]
  if old and old.n == n then
    local i = 1
    while i <= n and old[i] == (select(i, ...)) do i = i + 1 end
    if i > n then return false end
  end
  seen[key] = table.pack(...)
  return true
end

-- ui.scroller(x, y, w, h, draw): a box whose content (taller than it: a list, a page) scrolls.
-- draw(top, y0, y1) draws the content with its first row at screen row `top`: only rows [y0, y1)
-- of the screen are being painted (skipping what's outside them saves the time). A scroll moves
-- what's drawn and paints only the rows it uncovers: on a box the canvas's width, moving costs
-- nothing (the layer's rows are remapped, not copied).
--   s.pos                 the scroll: content rows above the box (s:to(pos) sets it, kept in range)
--   s:content(h)          the content's height (the scroll's range follows)
--   s:drag()              in _update: a touch that begins inside the box drags the content
--   s:draw()              in _draw: the box brought up to date (scrolled, or painted if new)
--   s:repaint()           the whole box painted again (part of a screen drawn again)
--   s:redraw(cy, ch)      content rows [cy, cy + ch) painted again, where they show
--   s:place(x, y, w, h)   the box moved or resized (painted again by the next s:draw)
--   s:at(sy)              the content row at screen row sy, or nil outside the box
local Scroller = {}
Scroller.__index = Scroller

function ui.scroller(x, y, w, h, draw)
  if type(draw) ~= "function" then error("ui.scroller: draw must be a function", 2) end
  local s = setmetatable({pos = 0, want = 0, height = 0, fn = draw}, Scroller)
  s:place(x, y, w, h)
  return s
end

function Scroller:place(x, y, w, h)
  self.x, self.y, self.w, self.h = floor(x), floor(y), floor(w), floor(h)
  self.shown = nil
  self:to(self.want)
end

function Scroller:to(pos)  -- the drag's exact place in `want`, the whole rows shown in `pos`
  self.want = max(0, min(pos, max(0, self.height - self.h)))
  self.pos = floor(self.want + 0.5)
end

function Scroller:content(h)
  self.height = h
  self:to(self.want)
end

function Scroller:at(sy)
  if sy >= self.y and sy < self.y + self.h then return sy - self.y + self.pos end
end

function Scroller:drag()
  local x, y = touch(true)
  local t = time()
  if not x then
    self.grab = nil
  elseif self.t and t - self.t < max(0.1, 2.5 / fps()) then  -- the same touch as last frame's
    if self.grab then
      self:to(self.want - (y - self.grab))
      self.grab = y
    end
  else  -- a touch begins (or this is the first frame drag was called in a while)
    local inside = x >= self.x and x < self.x + self.w and y >= self.y and y < self.y + self.h
    self.grab = inside and y or nil
  end
  self.t = x and t or nil
end

local function paint(s, y0, y1)  -- screen rows [y0, y1) of the box, at the scroll it shows
  y0, y1 = max(y0, s.y), min(y1, s.y + s.h)
  if y1 > y0 then hd.repaint(s.x, y0, s.w, y1 - y0, s.fn, s.y - s.shown, y0, y1) end
end

function Scroller:repaint()
  self.shown = self.pos
  paint(self, self.y, self.y + self.h)
end

function Scroller:redraw(cy, ch)
  if self.shown then paint(self, self.y - self.shown + cy, self.y - self.shown + cy + ch) end
end

function Scroller:draw()
  local dy = self.shown and self.shown - self.pos
  if not dy or dy >= self.h or -dy >= self.h then return self:repaint() end
  if dy == 0 then return end
  hd.scroll(self.x, self.y, self.w, self.h, dy)
  self.shown = self.pos
  if dy > 0 then paint(self, self.y, self.y + dy) else paint(self, self.y + self.h + dy, self.y + self.h) end
end
