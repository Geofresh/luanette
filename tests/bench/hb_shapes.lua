-- title: hb shapes
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
for i = 0, 4 do hd.circle(48 + i * 72, 168, 22, 0xce1126) end hd.ring(192, 260, 60, 0x99d9d9, 6) hd.ring(192, 260, 40, 0x99d9d9, 2, 128) hd.line(40, 330, 344, 230, 0xffec27, 3) hd.line(40, 230, 344, 330, 0xff77a8, 1) hd.rect(16, 16, 352, 96, 0x16243f, 255, 14) hd.frame(16, 16, 352, 96, 0x3d5a8a, 2, 14)
end
