-- title: Budget
-- by: geo
-- about: plain Lua work near check.py's 30,000-instruction budget, to time it on the board

local pts = {}
for i = 1, 64 do pts[i] = {x = i, y = i * 2, vx = 1, vy = -1} end
local n = 0
function _update60()
  for k = 1, 32 do
    for i = 1, #pts do
      local p = pts[i]
      p.x = (p.x + p.vx) % 128
      p.y = (p.y + p.vy) % 128
    end
  end
  n = n + 1
end
function _draw()
  cls(0)
  for i = 1, #pts do local p = pts[i] pset(p.x, p.y, 7) end
  print(string.format("CPU %d%%", flr(stat(1) * 100 + 0.5)), 2, 2, 7)
end
