-- title: hb line
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
hd.line(40, 330, 344, 230, 0xffec27, 3) hd.line(40, 230, 344, 330, 0xff77a8, 1)
end
