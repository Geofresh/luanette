-- title: hb text
-- by: geo
-- about: hd bench
-- api: 2
local t=0
function _update() t=t+1 end
function _draw()
hd.text("Red Wings 3  -  1 Leafs", 32, 28, 0xffffff, 24, "bold") hd.text("P2 08:12  ·  power play", 32, 62, 0x9fb3d1, 16, "mono") hd.text("FINAL", 352, 64, 0xce1126, 18, "bold", "right") hd.text("VT323 46", 220, 300, 0x00e436, 46, "vt") hd.text("mono 12: the quick brown fox jumps over", 16, 360, 0xc2c3c7, 12)
end
