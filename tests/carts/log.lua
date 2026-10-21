-- title: Log
-- by: tests
-- about: log() lines: values of each type, the per-frame limit, and a long line cut short
-- api: 4

local t = 0
function _update()
  t = t + 1
  if t == 1 then log("start", 1, 2.5, true, nil, {} ~= nil) end
  if t == 2 then for i = 1, 25 do log("line", i) end end
  if t == 3 then log(string.rep("x", 300)) end
end

function _draw()
  cls(1)
  print("frame " .. t, 2, 2, 7)
end
