-- title: Primitives
-- by: geo
-- about: every drawing call, for the parity test between check.py and the device runtime

SPRITES = {
  "0077770007000070700770077070070770000007707007070700007000777700",
  "8800008808800880008888000008800000088000008888000880088088000088",
}
local t = 0
function _init() t = 0 end
function _update() t = t + 1 end
function _draw()
  cls(1)
  for i = 0, 15 do rectfill(i * 8, 0, i * 8 + 7, 5, i) end
  line(0, 8, 127, 30, 7)
  line(127, 8, 0, 40, 8)
  line(10, 50, 10, 50, 9)
  line(-40, 60, 200, 70, 10)
  rect(4, 44, 40, 70, 11)
  rectfill(50, 44, 30, 60, 12)
  for r = 0, 6 do circ(20 + r * 15, 85, r, 7) circfill(20 + r * 15, 100, r, 14) end
  circ(64, 64, 30, 6)
  print("HELLO, carts! 0123456789", 2, 108, 7)
  print("+-*/=%()[]<>#_$&'\"?:.,", 2, 114, 15)
  print("two\nlines", 100, 40)
  local x = print(t, 100, 54, 10)
  print(x, 100, 60)
  print(3.0, 112, 60)
  spr(1, 2, 120) spr(2, 12, 120, true) spr(1, 22, 120, false, true)
  pal(7, 8) spr(1, 32, 120) pal()
  camera(-3, 2) rect(40, 118, 60, 127, 13) camera()
  clip(70, 118, 10, 10) circfill(75, 123, 9, 9) clip()
  pset(126, 126, 7) pset(200, 200, 8)
  local c = pget(126, 126)
  print(c, 84, 120, c)
  local tx, ty = touch()
  if tx then circfill(tx, ty, 3, 8) end
  if btn(0) then rectfill(90, 120, 93, 123, 11) end
  if btnp(4) then rectfill(95, 120, 98, 123, 10) end
  print(flr(-2.5) .. " " .. ceil(2.1) .. " " .. abs(-3) .. " " .. sgn(-1) .. " " .. min(3, 2) .. max(1, 4) .. mid(5, 1, 3), 2, 76, 6)
end
