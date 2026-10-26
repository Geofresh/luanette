-- title: dialog demo
-- by: luanette
-- about: press A to talk to the cat: a text box types itself out, a page at a time (the dialog library)
-- api: 1

-- lib: dialog 1, api 1: a text box that types itself out a page at a time: dialog.say("Hi!\nNice day.")
-- In _update: `if dialog.update() then return end` (the game waits while it's open; A shows the
-- whole page, then the next). In _draw, last: dialog.draw(). Change dialog.x, y, w, h and the
-- colors to place it; lines wrap at words to fit.
local dialog = {x = 4, y = 86, w = 120, h = 38, bg = 1, border = 7, color = 7, speed = 40}
local pages, page, t0, fresh

-- Lines of text no more than cols characters, broken at spaces (and at "\n").
local function wrap(text, cols)
  local lines = {}
  for para in (text .. "\n"):gmatch("(.-)\n") do
    local line = ""
    for word in para:gmatch("%S+") do
      if line == "" then
        line = word
      elseif #line + 1 + #word <= cols then
        line = line .. " " .. word
      else
        lines[#lines + 1] = line
        line = word
      end
      while #line > cols do
        lines[#lines + 1] = line:sub(1, cols)
        line = line:sub(cols + 1)
      end
    end
    lines[#lines + 1] = line
  end
  return lines
end

-- Opens the box. text: a string, or a list of strings (each starts a page). done(): called when
-- the last page is closed.
function dialog.say(text, done)
  local list = type(text) == "table" and text or {text}
  local cols, rows = flr((dialog.w - 8) / 4), math.max(1, flr((dialog.h - 6) / 6))
  pages = {}
  for _, t in ipairs(list) do
    local lines = wrap(tostring(t), cols)
    for i = 1, #lines, rows do
      pages[#pages + 1] = table.concat(lines, "\n", i, math.min(i + rows - 1, #lines))
    end
  end
  page, t0, fresh, dialog.done = 1, time(), true, done
end

function dialog.active()
  return pages ~= nil
end

local function shown()
  return math.min(#pages[page], flr((time() - t0) * dialog.speed))
end

-- Call once a frame in _update; true while the box is open.
function dialog.update()
  if not pages then return false end
  if fresh then fresh = false return true end  -- the press that opened it isn't one for it
  if btnp(4) then
    if shown() < #pages[page] then
      t0 = time() - #pages[page] / dialog.speed  -- all of this page at once
    elseif page < #pages then
      page, t0 = page + 1, time()
    else
      pages = nil
      if dialog.done then dialog.done() end
      return false
    end
  end
  return true
end

function dialog.draw()
  if not pages then return end
  local x, y, w, h = dialog.x, dialog.y, dialog.w, dialog.h
  rectfill(x, y, x + w - 1, y + h - 1, dialog.bg)
  rect(x, y, x + w - 1, y + h - 1, dialog.border)
  local n = shown()
  print(pages[page]:sub(1, n), x + 4, y + 4, dialog.color)
  if n >= #pages[page] and time() % 0.8 < 0.5 then
    print(page < #pages and ">" or "*", x + w - 8, y + h - 8, dialog.border)
  end
end
-- end lib: dialog

local talks = 0

function _update()
  if dialog.update() then return end  -- while it's open, the game waits
  if btnp(4) then
    talks = talks + 1
    dialog.say({"Meow. You found the cat.",
                "The cat has a lot to say, more than fits in one box, so it goes on to a second page.",
                "That's all. Come back any time (you've talked " .. talks .. " times)."})
  end
end

function _draw()
  cls(1)
  circfill(64, 50, 14, 9)
  pset(59, 46, 0)
  pset(69, 46, 0)
  print("press a to talk", 34, 72, 6)
  dialog.draw()
end
