-- title: Sprites
-- by: tests
-- about: sprites written plainly and with spaces between rows draw the same; flips too
-- api: 4

SPRITES = {
  [1] = "0077770007777770771771777777777777777777771771770771177000777700",
  [2] = "00777700 07777770 77177177 77777777 77777777 77177177 07711770 00777700",
  [3] = "00888800\n08888880\n88a88a88\n88888888\n88888888\n88a88a88\n088aa880\n00888800",
  [70] = "c0000000 0c000000 00c00000 000c0000 0000c000 00000c00 000000c0 0000000c",
}

local t = 0
function _update() t = t + 1 end
function _draw()
  cls(1)
  spr(1, 10, 10)
  spr(2, 30, 10)
  spr(3, 50, 10, t % 20 < 10, false)
  spr(70, 70, 10, false, t % 30 < 15)
  spr(2, 10 + t % 100, 60)
end
