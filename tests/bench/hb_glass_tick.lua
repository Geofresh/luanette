-- title: hb glass tick
-- by: geo
-- about: hd bench: one small part of a full-glass screen changing every frame (a counter), the rest kept
-- api: 2
-- canvas: full
local c = caps()
local W, H = c.w, c.h
local n = 0
function _init()
  hd.rect(0, 0, W, H, 0x0b0f1a)
  hd.text("A COUNTER", W / 2, 60, 0x8d97ab, 16, "mono", "center")
end
function _update() n = n + 1 end
function _draw()
  hd.rect(W / 2 - 80, 200, 160, 80, 0x0b0f1a)
  hd.text(tostring(n), W / 2, 210, 0xf2f4f8, 72, "vt", "center")
end
