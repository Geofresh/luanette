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
