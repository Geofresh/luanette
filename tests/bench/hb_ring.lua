-- title: hb ring
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
hd.ring(192, 260, 60, 0x99d9d9, 6) hd.ring(192, 260, 40, 0x99d9d9, 2, 128)
end
