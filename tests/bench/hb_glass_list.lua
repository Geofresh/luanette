-- title: hb glass list
-- by: geo
-- about: hd bench: a typical app screen on the whole glass (header, 9 rows of text and rules), all drawn every frame
-- api: 2
-- canvas: full
local c = caps()
local W, H = c.w, c.h
function _update() end
function _draw()
  hd.rect(0, 0, W, H, 0x0b0f1a)
  hd.rect(0, 0, W, 64, 0x151d2e)
  hd.text("DRAG, OR TAP A ROW", W / 2, 30, 0x8d97ab, 14, "mono", "center")
  for i = 1, 9 do
    local y = 64 + (i - 1) * 48
    hd.text("Row " .. i, 32, y + 14, 0xf2f4f8, 18, "bold")
    hd.text(tostring(i * 7), W - 32, y + 16, 0x4fb0ff, 16, "mono", "right")
    hd.line(24, y + 47, W - 24, y + 47, 0x222c42)
  end
end
