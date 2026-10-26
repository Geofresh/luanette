-- title: timer demo
-- by: luanette
-- about: a countdown, a blinking light, and a light every A press turns on for a second (the timer library)
-- api: 1

-- lib: timer 1, api 1: run code later, or every so often: timer.after(2, open_door), timer.every(0.5, blink)
-- Call timer.update() once a frame, in _update. It goes by time(), so it holds at any frame rate.
local timer = {}
local list = {}

-- Calls fn() once, secs seconds from now. Returns a handle for timer.cancel.
function timer.after(secs, fn)
  local h = {at = time() + secs, fn = fn}
  list[#list + 1] = h
  return h
end

-- Calls fn() every secs seconds, `times` times (forever if left out).
function timer.every(secs, fn, times)
  local h = {at = time() + secs, every = secs, fn = fn, left = times}
  list[#list + 1] = h
  return h
end

function timer.cancel(h)
  h.cancelled = true
end

-- Cancels them all (starting a level over, say).
function timer.clear()
  for _, h in ipairs(list) do h.cancelled = true end
end

function timer.update()
  local now = time()
  local i = 1
  while i <= #list do
    local h = list[i]
    if not h.cancelled and now >= h.at then
      h.fn()
      if h.every and (h.left == nil or h.left > 1) then
        h.left = h.left and h.left - 1
        h.at = h.at + h.every
        if h.at < now then h.at = now + h.every end  -- after a long pause, no burst of catching up
      else
        h.cancelled = true
      end
    end
    if h.cancelled then table.remove(list, i) else i = i + 1 end
  end
end
-- end lib: timer

local count, blink, lit = 3, false, false

function _init()
  fps(30, 4)  -- lazy: a still screen redraws a few times a second
  timer.every(1, function() count = count - 1 end, 3)   -- 3, 2, 1, then GO
  timer.every(0.5, function() blink = not blink end)    -- for ever
end

function _update()
  timer.update()
  if btnp(4) then
    lit = true
    timer.after(1, function() lit = false end)
  end
end

function _draw()
  cls(1)
  print(count > 0 and count or "go!", 58, 30, 7)
  circfill(40, 80, 10, blink and 8 or 2)
  circfill(88, 80, 10, lit and 11 or 3)
  print("every 0.5 s", 18, 96, 6)
  print("press a", 74, 96, 6)
end
