-- title: hb circ
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
for i = 0, 4 do hd.circle(48 + i * 72, 168, 22, 0xce1126) end
end
