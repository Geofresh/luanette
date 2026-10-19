-- title: hb card
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
hd.rect(16, 16, 352, 96, 0x16243f, 255, 14) hd.frame(16, 16, 352, 96, 0x3d5a8a, 2, 14)
end
