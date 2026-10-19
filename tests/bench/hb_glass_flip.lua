-- title: hb glass flip
-- by: geo
-- about: hd bench: the app screen redrawn with every row different every frame (a page switch, every frame)
-- api: 2
-- canvas: full
local c = caps()
local W, H = c.w, c.h
local n = 0
function _update() n = n + 1 end
function _draw()
  hd.rect(0, 0, W, H, n % 2 == 0 and 0x0b0f1a or 0x0c101b)
  hd.rect(0, 0, W, 64, 0x151d2e)
  hd.text("DRAG, OR TAP A ROW", W / 2, 30, 0x8d97ab, 14, "mono", "center")
  for i = 1, 9 do
    local y = 64 + (i - 1) * 48
    hd.text("Row " .. (i + n % 50), 32, y + 14, 0xf2f4f8, 18, "bold")
    hd.text(tostring(i * 7), W - 32, y + 16, 0x4fb0ff, 16, "mono", "right")
    hd.line(24, y + 47, W - 24, y + 47, 0x222c42)
  end
end
