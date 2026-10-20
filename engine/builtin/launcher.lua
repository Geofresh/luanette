-- title: Carts
-- by: geo
-- about: the console's launcher: every cart on the device; tap one to play, drag to scroll
-- api: 2
-- canvas: full
-- Luanette runs this whenever no other cart is open, and again when one closes (PWR, or a swipe
-- down from the top edge). The list comes from the console as data("carts") ({name, title, about}
-- for each, sent ones newest first, then the built-in ones); act("open", name) opens one.
-- A tap on the clock, or the last row, opens the settings: volume and brightness (data("settings"), changed with
-- act("volume", step) and act("brightness", step)) and the battery (data("battery")).
-- The whole glass on the full-color layer, drawn with the ui helpers: the clock again when its
-- minute changes, a row when it's pressed or its value changes, the list moved under the finger.

local c = caps()
local W, H = c.w, c.h
local HEAD, ROW = 108, 78                    -- the header's height; a row
local CLOCK = 70                             -- the clock's rows; the line under it is below
local PAD, BADGE = 26, 52                    -- the rows' left margin; the cartridge badge's size
local BG, PRESS, RULE = 0x0b0f1a, 0x1b2438, 0x1c2536
local WHITE, GRAY, DIM = 0xf2f4f8, 0x8d97ab, 0x56607a
local RED, GREEN, BLUE, YELLOW = 0xff4d6d, 0x3ddc84, 0x29b6f6, 0xffd23f
-- Badge colors, picked by the cart's name (the same cart keeps its color)
local HUES = {0xff4d6d, 0xff9f43, 0xffd23f, 0x3ddc84, 0x29b6f6, 0x7c6cff, 0xe056fd, 0x2ee6c9}
local VOLUMES = {"Off", "Low", "Medium", "High"}
local BRIGHTNESS = {"25%", "50%", "75%", "100%"}

local mode = "carts"                         -- or "settings"
local rows = {}                              -- what's listed: {kind, name?, hue, tag, fit_title, fit_about}
local carts_data, settings, battery          -- the console's channels as last seen
local pressed, lit                           -- the row under a finger that hasn't moved ("head": the clock); the row drawn lit
local tx, ty                                 -- the last touch (glass pixels)

local function hash(s)
  local h = 7
  for i = 1, #s do h = (h * 31 + s:byte(i)) % 65536 end
  return h
end

-- "Lock & Load" -> "LL", "dice" -> "DI", "Night Swarm" -> "NS"
local function initials(title)
  local words = {}
  for w in title:gmatch("%w+") do words[#words + 1] = w end
  if #words >= 2 then return (words[1]:sub(1, 1) .. words[2]:sub(1, 1)):upper() end
  return (words[1] or "?"):sub(1, 2):upper()
end

-- Text cut to fit `width` at this size and font, "..." where it was cut.
local function fit(text, width, size, font)
  if hd.measure(text, size, font) <= width then return text end
  local lo, hi = 0, #text
  while lo < hi do  -- the longest prefix that fits with its ellipsis
    local mid = (lo + hi + 1) // 2
    if hd.measure(text:sub(1, mid) .. "…", size, font) <= width then lo = mid else hi = mid - 1 end
  end
  return text:sub(1, lo):gsub("%s+$", "") .. "…"
end

-- What a settings row shows on its right, and its line under the title.
local function battery_line()
  if not battery then return "", "" end
  local volts = string.format("%.2f V", (battery.mv or 0) / 1000)
  local state = battery.charging and "charging" or (battery.usb and "on USB" or "on battery")
  return (battery.percent or 0) .. "%", volts .. ", " .. state
end
local function value_of(kind)
  if kind == "volume" then return VOLUMES[(settings.volume or 2) + 1] end
  if kind == "brightness" then return BRIGHTNESS[(settings.brightness or 3) + 1] end
  if kind == "battery" then return (battery_line()) end
  return nil
end

local function draw_row(i, y)
  local it = rows[i]
  local x0 = PAD  -- every row on the same edge (one shifting in at the rounded corners looked odd while scrolling;
                  -- the list's end has room to bring the last row clear of them)
  if i == lit then hd.rect(0, y, W, ROW, PRESS) end
  -- the cartridge: a rounded badge in its color, a notch along its top
  local by = y + (ROW - BADGE) // 2
  local hue = it.hue
  if it.kind == "battery" and battery and (battery.percent or 100) <= 20 and not battery.charging then hue = RED end
  hd.rect(x0, by, BADGE, BADGE, hue, 255, 10)
  hd.rect(x0 + 14, by, BADGE - 28, 5, BG, 110, 2)
  hd.text(it.tag, x0 + BADGE / 2, by + 15, BG, 18, "bold", "center")
  local tx0 = x0 + BADGE + 18
  local about = it.fit_about
  if it.kind == "battery" then about = select(2, battery_line()) end
  hd.text(it.fit_title, tx0, y + 18, WHITE, 18, "bold")
  hd.text(about, tx0, y + 44, GRAY, 14, "mono")
  local value = value_of(it.kind)
  if value then hd.text(value, W - PAD - 6, y + 26, WHITE, 18, "bold", "right") end
  hd.line(tx0, y + ROW - 1, W - PAD, y + ROW - 1, RULE)
end

-- The rows, the first at screen row top; only rows y0 to y1 are being painted.
local function draw_rows(top, y0, y1)
  hd.rect(0, y0, W, y1 - y0, BG)
  if #rows == 0 then
    hd.text("No carts yet", W / 2, top + 70, WHITE, 18, "bold", "center")
    hd.text("send one over USB:", W / 2, top + 104, GRAY, 14, "mono", "center")
    hd.text("tools/push.py game.lua", W / 2, top + 126, BLUE, 14, "mono", "center")
    return
  end
  for i = math.max(1, (y0 - top) // ROW + 1), #rows do
    local y = top + (i - 1) * ROW
    if y >= y1 then break end
    draw_row(i, y)
  end
end
local scroller = ui.scroller(0, HEAD, W, H - HEAD, draw_rows)

local function draw_clock(h, m)
  hd.rect(0, 0, W, CLOCK, BG)
  if not h then return end
  hd.text(string.format("%d:%02d", (h - 1) % 12 + 1, m), W / 2, 12, WHITE, 46, "vt", "center")
end

-- The line under the clock: how many carts (or "SETTINGS"), and the battery.
local function subtitle()
  local n = 0
  for _, r in ipairs(rows) do if r.kind == "cart" then n = n + 1 end end
  local s = mode == "settings" and "SETTINGS" or (n == 1 and "1 CART" or n .. " CARTS")
  if battery and battery.percent then
    s = s .. "  ·  " .. (battery.charging and "CHARGING " or "") .. battery.percent .. "%"
  end
  return s
end
local function draw_subtitle(text)
  hd.rect(0, CLOCK, W, HEAD - 3 - CLOCK, BG)
  hd.text(text, W / 2, 74, DIM, 14, "mono", "center")
end

local function draw_header(sub)
  hd.rect(0, 0, W, HEAD, BG)
  draw_subtitle(sub)
  hd.line(PAD + 30, HEAD - 2, W - PAD - 30, HEAD - 2, RULE)
end

local function entry(kind, title, about, hue, tag, name)
  local room = W - 2 * PAD - BADGE - 18 - 18  -- the text's width on a row clear of the corners
  if kind ~= "cart" and kind ~= "settings" and kind ~= "back" then room = room - 96 end  -- a value on the right
  return {kind = kind, name = name, hue = hue, tag = tag,
          fit_title = fit(title, room, 18, "bold"), fit_about = fit(about or "", room, 14, "mono")}
end

local function load_rows()
  rows = {}
  if mode == "carts" then
    for _, e in ipairs(carts_data or {}) do
      local title = (e.title and e.title ~= "") and e.title or e.name
      rows[#rows + 1] = entry("cart", title, e.about, HUES[hash(e.name) % #HUES + 1], initials(title), e.name)
    end
    if settings or battery then rows[#rows + 1] = entry("settings", "Settings", "volume, brightness, battery", DIM, "•••") end
  else
    if settings then
      rows[#rows + 1] = entry("volume", "Volume", "tap to change", BLUE, "VO")
      rows[#rows + 1] = entry("brightness", "Brightness", "tap to change", YELLOW, "BR")
    end
    if battery then rows[#rows + 1] = entry("battery", "Battery", "", GREEN, "BA") end
    rows[#rows + 1] = entry("back", "Back", "to the carts", DIM, "←")
  end
  scroller:content(#rows * ROW + hd.inset(H - 40, 40) + 24)  -- room at the end to clear the rounded corners
end

-- A tap on row i.
local function choose(i)
  local it = rows[i]
  if it.kind == "cart" then
    act("open", it.name)
  elseif it.kind == "settings" then
    mode = "settings"
  elseif it.kind == "back" then
    mode = "carts"
  elseif it.kind == "volume" then
    act("volume", tostring(((settings.volume or 2) + 1) % #VOLUMES))
    sfx(5)  -- heard at the new level (or not at all, when that's off)
  elseif it.kind == "brightness" then
    act("brightness", tostring(((settings.brightness or 3) + 1) % #BRIGHTNESS))
  end
end

function _init()
  fps(60, 2)  -- 60 while touched (the list follows the finger), 2 otherwise (the clock, the battery)
  hd.scanlines(false)
end

function _update()
  local d, s, b = data("carts"), data("settings"), data("battery")
  local relist = d ~= carts_data or (s == nil) ~= (settings == nil) or (b == nil) ~= (battery == nil)
  carts_data, settings, battery = d, s, b
  if relist then load_rows() end
  local x, y = touch(true)
  scroller:drag()
  if x then
    if not tx then  -- a finger came down: the row under it lights while it stays put
      local cy = scroller:at(y)
      pressed = cy and cy // ROW + 1 or nil
      if pressed and not rows[pressed] then pressed = nil end
      if y < HEAD and (settings or battery) then pressed = "head" end  -- the clock: settings and back
    end
    if btn() & 15 ~= 0 then pressed = nil end  -- it dragged: a scroll, not a tap
    tx, ty = x, y
  else
    if btnp(4) and pressed then  -- a tap: A as the finger lifts
      local was = mode
      if pressed == "head" then mode = mode == "carts" and "settings" or "carts" else choose(pressed) end
      if mode ~= was then
        load_rows()
        scroller:to(0)
        pressed = nil
      end
    end
    pressed, tx, ty = nil, nil, nil
  end
end

function _draw()
  local t = date()
  local sub = subtitle()
  local whole = ui.changed("screen", carts_data, mode)  -- the first frame, a new list, the other page: all of it
  local clock = ui.changed("clock", t and t.hour, t and t.min)
  local subline = ui.changed("subtitle", sub)
  -- every row's value asked every frame (a setting changed, the battery moved): those rows again
  local values = {}
  for i, it in ipairs(rows) do
    local v = it.kind == "battery" and select(2, battery_line()) .. (value_of("battery") or "") or value_of(it.kind)
    if ui.changed("row" .. i .. it.kind, v) then values[#values + 1] = i end
  end
  if whole then
    lit = type(pressed) == "number" and pressed or nil
    draw_header(sub)
    draw_clock(t and t.hour, t and t.min)
    scroller:repaint()
    return
  end
  if clock then hd.repaint(0, 0, W, CLOCK, draw_clock, t and t.hour, t and t.min) end
  if subline then hd.repaint(0, CLOCK, W, HEAD - 3 - CLOCK, draw_subtitle, sub) end
  for _, i in ipairs(values) do scroller:redraw((i - 1) * ROW, ROW) end
  local row = type(pressed) == "number" and pressed or nil
  if row ~= lit then  -- a row lit or unlit: just those rows again
    local was = lit
    lit = row
    if was then scroller:redraw((was - 1) * ROW, ROW) end
    if lit then scroller:redraw((lit - 1) * ROW, ROW) end
  end
  scroller:draw()
end
