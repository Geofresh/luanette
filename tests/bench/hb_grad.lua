-- title: hb grad
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
hd.gradient(0, 0, 384, 384, 0x1b2a4a, 0x0a0f1c)
end
