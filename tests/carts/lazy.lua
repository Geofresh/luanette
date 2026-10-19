-- title: Lazy
-- by: geo
-- about: fps(active, idle): its rate and clock printed each frame, for the parity test (both sides must agree)
-- api: 2

function _init()
  fps(30, 2)
end

local n = 0
function _update()
  n = n + 1
  if n == 200 then fps(60, 5) end
end

function _draw()
  cls(1)
  print("fps " .. fps(), 4, 4, 7)
  print("ms " .. flr(time() * 1000), 4, 12, 7)
  print("now " .. now() % 1000, 4, 20, 7)
  print("frame " .. n, 4, 28, 7)
end
