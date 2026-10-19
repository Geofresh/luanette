-- title: hb img
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
hd.image("photo", 128, 128)
end
