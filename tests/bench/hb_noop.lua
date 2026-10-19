-- title: hb noop
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
hd.rect(0, 0, 1, 1, 0x000000)
end
