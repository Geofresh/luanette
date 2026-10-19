-- title: hb imgalpha
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
for i = 0, 19 do hd.image("icon/heart", (i % 5) * 72 + 12, (i // 5) * 72 + 40) end
end
